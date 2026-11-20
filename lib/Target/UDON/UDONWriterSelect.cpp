//===-- UDONWriterSelect.cpp - instruction selection ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-instruction selection: arithmetic, compares, control flow and phi copies.
// Every operation becomes extern calls on typed heap slots, because Udon has no
// registers and no arithmetic of its own.
//
// A selection can branch mid-block -- `select`, `min`/`max`, `abs` and a
// variable-count `lshr` all need two arms -- so selection tracks the block
// currently being filled. When an instruction splits, the instructions after it
// continue in the join block, and `Idx` moves there. Without that, everything
// after the split would be emitted after a jump and never run.
//
//===----------------------------------------------------------------------===//

#include "UDONWriter.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ErrorHandling.h"
#include <cstdlib>

using namespace llvm;

namespace {
inline std::string mangle(StringRef Name) { return mangleName(Name); }
} // namespace

namespace {

/// The Udon type name a value's operators use.
///
/// A `std::string`, not a `StringRef`: a view into a literal is fine, but a
/// view into a temporary would dangle, and the signature builders concatenate
/// the result. Returning a view here is what produced signatures full of
/// garbage bytes.
std::string operandType(Type *Ty) {
  if (Ty->isIntegerTy(1)) return "SystemBoolean";
  // i8 and i16 live in `%SystemInt32` slots, so their operators are the Int32
  // ones.
  if (Ty->isIntegerTy(8) || Ty->isIntegerTy(16)) return "SystemInt32";
  if (Ty->isIntegerTy(32)) return "SystemInt32";
  if (Ty->isIntegerTy(64)) return "SystemInt64";
  if (Ty->isFloatTy()) return "SystemSingle";
  if (Ty->isDoubleTy()) return "SystemDouble";
  if (Ty->isPointerTy()) return "SystemInt32";
  return {};
}

// These take `StringRef` and the caller passes `std::string` locals, so the
// views stay valid for the duration of the call. Passing a temporary's view is
// what produced signatures full of garbage bytes.
std::string binarySignature(StringRef Type, StringRef Op) {
  return (Type + ".__op_" + Op + "__" + Type + "_" + Type + "__" + Type).str();
}

std::string compareSignature(StringRef Type, StringRef Op) {
  return (Type + ".__op_" + Op + "__" + Type + "_" + Type + "__SystemBoolean")
      .str();
}

StringRef icmpPredicateName(CmpInst::Predicate P) {
  switch (P) {
  case CmpInst::ICMP_EQ: return "Equality";
  case CmpInst::ICMP_NE: return "Inequality";
  case CmpInst::ICMP_SLT: return "LessThan";
  case CmpInst::ICMP_SLE: return "LessThanOrEqual";
  case CmpInst::ICMP_SGT: return "GreaterThan";
  case CmpInst::ICMP_SGE: return "GreaterThanOrEqual";
  default: return {};
  }
}

/// The signed predicate with the same result once both operands are XORed with
/// the sign bit.
CmpInst::Predicate signedEquivalentOfUnsigned(CmpInst::Predicate P) {
  switch (P) {
  case CmpInst::ICMP_ULT: return CmpInst::ICMP_SLT;
  case CmpInst::ICMP_ULE: return CmpInst::ICMP_SLE;
  case CmpInst::ICMP_UGT: return CmpInst::ICMP_SGT;
  case CmpInst::ICMP_UGE: return CmpInst::ICMP_SGE;
  default: return P;
  }
}

bool isUnsignedPredicate(CmpInst::Predicate P) {
  return P == CmpInst::ICMP_ULT || P == CmpInst::ICMP_ULE ||
         P == CmpInst::ICMP_UGT || P == CmpInst::ICMP_UGE;
}

} // namespace

// --- block plumbing --------------------------------------------------------

std::string UDONWriter::freshLabelStem(StringRef Hint) {
  return mangle(Cur->F->getName()) + "_" + Hint.str() + "_" +
         std::to_string(LabelCounter++);
}

unsigned UDONWriter::appendBlock(UFunction &UF, const std::string &Label) {
  // The label is used verbatim. Renaming it here would break the caller, which
  // holds the same string as a jump target -- and uniqueness is already
  // guaranteed: basic-block labels are `<fn>_<bb>`, and every split names its
  // blocks from `freshLabelStem`, which is program-unique.
  UBlock B;
  B.Label = Label;
  UF.Blocks.push_back(std::move(B));
  return static_cast<unsigned>(UF.Blocks.size() - 1);
}

unsigned UDONWriter::emitDiamond(UFunction &UF, unsigned &Idx,
                                 const std::string &Cond,
                                 const std::string &TrueSrc,
                                 const std::string &FalseSrc,
                                 const std::string &Result,
                                 const std::string &Hint) {
  // The branch goes at the end of the block currently being filled, which is
  // `Idx` -- not `back()`, because the function's basic blocks were preallocated
  // and the block being filled is rarely the last one.
  UBlock &B = UF.Blocks[Idx];
  // One stem, one counter increment: three separate `++`s in one expression
  // would be unsequenced relative to each other and could repeat a value.
  std::string Base = freshLabelStem(Hint);
  std::string TrueLabel = Base + "_t";
  std::string FalseLabel = Base + "_f";
  std::string JoinLabel = Base + "_j";

  B.Insts.push_back({UInst::PUSH, Cond});
  B.Insts.push_back({UInst::JUMP_IF_FALSE, FalseLabel});
  B.Insts.push_back({UInst::JUMP, TrueLabel});

  unsigned TrueIdx = appendBlock(UF, TrueLabel);
  emitCopy(UF.Blocks[TrueIdx], TrueSrc, Result);
  UF.Blocks[TrueIdx].Insts.push_back({UInst::JUMP, JoinLabel});

  unsigned FalseIdx = appendBlock(UF, FalseLabel);
  emitCopy(UF.Blocks[FalseIdx], FalseSrc, Result);
  UF.Blocks[FalseIdx].Insts.push_back({UInst::JUMP, JoinLabel});

  return appendBlock(UF, JoinLabel);
}

// --- function selection ----------------------------------------------------

void UDONWriter::selectFunction(const Function &F) {
  // The function lives in `Functions` from the start, and `Cur` points there.
  // Building it as a local and moving it in afterwards would leave `Cur`
  // dangling into a moved-from object for the rest of selection, and every
  // split block appended through `Cur` would land in the wrong container.
  //
  // `Functions` is a deque, so the reference survives later `push_back`s.
  Functions.emplace_back();
  Cur = &Functions.back();
  UFunction &UF = *Cur;
  UF.F = &F;
  UF.BodyLabel = mangle(F.getName()) + "_body";
  Slots.clear();
  SignExtendedSlots.clear();
  TempCounter = 0;

  declareSlot(returnSlot(&F), "SystemUInt32");
  NullSlots.push_back({returnSlot(&F), "SystemUInt32"});
  if (!F.getReturnType()->isVoidTy() &&
      !isa<StructType>(F.getReturnType()) && !isa<ArrayType>(F.getReturnType())) {
    declareSlot(returnValueSlot(&F), udonTypeOf(F.getReturnType()));
    NullSlots.push_back({returnValueSlot(&F), udonTypeOf(F.getReturnType())});
  }
  // An aggregate return value has no slot of its own: its fields are named
  // `<retPrefix>_f<N>` on demand, where the retPrefix is the
  // returnValueSlot name. Declared lazily by insertvalue/extractvalue users. 

  // Argument slots are part of the calling convention: a direct call copies its
  // arguments here before jumping to the body.
  for (unsigned A = 0; A < F.arg_size(); A++) {
    std::string Slot = mangle(F.getName()) + "_arg_" + std::to_string(A);
    // An aggregate argument scalarises: its "slot" is a prefix and the fields
    // are declared on demand by `aggFieldSlot`. Zig's by-value structs arrive
    // this way, and the body reads them through extractvalue, so no typed slot
    // for the whole aggregate is needed or even possible.
    if (isa<StructType>(F.getArg(A)->getType()) ||
        isa<ArrayType>(F.getArg(A)->getType())) {
      Slots[F.getArg(A)] = Slot;
      continue;
    }
    std::string Ty = udonTypeOf(F.getArg(A)->getType());
    if (Ty.empty())
      reportUDONError("argument has no Udon type: " + F.getName());
    declareSlot(Slot, Ty);
    NullSlots.push_back({Slot, Ty});
    // The argument's own slot *is* its transfer slot. Without this mapping the
    // body would read a different, never-written slot -- which is silently 0,
    // so a loop comparing against the argument would never terminate.
    Slots[F.getArg(A)] = Slot;
  }

  // The body label is the entry internal callers jump to. It pushes this
  // activation's shadow-stack frame and then enters the real entry block, so
  // every activation gets its own frame -- which is what makes recursion and
  // re-entrancy correct.
  unsigned BodyIdx = appendBlock(UF, UF.BodyLabel);
  unsigned AfterPush = emitFramePush(F, UF, BodyIdx);
  UBlock &Enter = UF.Blocks[AfterPush];
  // An address-taken function is entered through `__fn_table`, so it copies the
  // transfer slots into its own before running. That copy is what lets a nested
  // indirect call work: the callee's state lives in slots the inner call cannot
  // reach.
  if (FunctionPointerIndices.count(&F)) {
    for (unsigned A = 0; A < F.arg_size(); A++)
      emitCopy(Enter, "__icall_arg_" + std::to_string(A),
               mangle(F.getName()) + "_arg_" + std::to_string(A));
    emitCopy(Enter, "__icall_ret", returnSlot(&F));
  }
  Enter.Insts.push_back({UInst::JUMP, blockLabel(&F.getEntryBlock())});

  // Fill one basic block at a time, appending as we go. Preallocating every
  // basic block first would make `appendBlock` (used by every mid-block split)
  // land next to blocks that are still empty, and the two would collide.
  //
  // Each basic block is looked up by label, because a split renames nothing but
  // does append, so an index captured before a split is stale afterwards.
  DenseMap<const BasicBlock *, std::string> BlockLabels;
  for (const BasicBlock &BB : F)
    BlockLabels[&BB] = blockLabel(&BB);

  // Create the blocks first, so a forward branch can name one that has not been
  // filled yet -- but keep them contiguous by appending all of them before any
  // split can run.
  for (const BasicBlock &BB : F) {
    unsigned Idx = appendBlock(UF, BlockLabels[&BB]);
    UF.BlockIndex[&BB] = Idx;
  }

  for (const BasicBlock &BB : F) {
    unsigned Idx = UF.BlockIndex[&BB];
    selectBlock(BB, UF, Idx);
  }

  buildEntryBlocks(F, UF);
}

void UDONWriter::selectBlock(const BasicBlock &BB, UFunction &UF,
                             unsigned &Idx) {
  for (const Instruction &I : BB) {
    if (I.isTerminator()) {
      selectTerminator(I, UF, Idx);
      return;
    }
    selectInstruction(I, UF, Idx);
  }
}

/// The element type one aggregate level down: a struct's field or an array's
/// element. Zig's by-value structs wrap arrays (`{ [16 x float] }`), so both
/// shapes appear.
static Type *aggElementAt(Type *Ty, unsigned Idx) {
  if (auto *STy = dyn_cast<StructType>(Ty))
    return STy->getElementType(Idx);
  if (auto *ATy = dyn_cast<ArrayType>(Ty))
    return ATy->getElementType();
  return nullptr;
}

void UDONWriter::selectInstruction(const Instruction &I, UFunction &UF,
                                   unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];

  // An `invoke` reaches here as a CallBase too, and the generic call path would
  // report it as an undefined function. The terminator handler gives the right
  // diagnostic, so let it.
  if (isa<InvokeInst>(&I)) {
    selectTerminator(I, UF, Idx);
    return;
  }

  if (const auto *CB = dyn_cast<CallBase>(&I))
    if (const Function *Callee = CB->getCalledFunction())
      if (Callee->isIntrinsic())
        if (selectIntrinsic(I, UF, Idx))
          return;

  switch (I.getOpcode()) {
  case Instruction::FAdd:
  case Instruction::FSub:
  case Instruction::FMul:
  case Instruction::FDiv:
  case Instruction::FRem: {
    // A vector add or subtract is one UnityEngineVector* extern. A vector
    // multiply or divide has no such operator -- Vector3 is component-wise
    // only through accessors -- so those scalarize.
    if (auto *VT = dyn_cast<FixedVectorType>(I.getType())) {
      std::string VecTy = udonTypeOf(VT);
      if (!VecTy.empty() &&
          (I.getOpcode() == Instruction::FAdd ||
           I.getOpcode() == Instruction::FSub)) {
        const char *Op =
            I.getOpcode() == Instruction::FAdd ? "Addition" : "Subtraction";
        emitExtern(B, VecTy + ".__op_" + Op + "__" + VecTy + "_" + VecTy +
                          "__" + VecTy,
                   {slotFor(I.getOperand(0)), slotFor(I.getOperand(1))},
                   slotFor(&I));
        return;
      }
      selectVectorArith(I, UF, Idx);
      return;
    }
    std::string Ty = operandType(I.getType());
    if (Ty.empty())
      reportUDONError("float arithmetic on an unsupported type: " + I.getName());
    const char *Op = I.getOpcode() == Instruction::FAdd   ? "Addition"
                     : I.getOpcode() == Instruction::FSub ? "Subtraction"
                     : I.getOpcode() == Instruction::FMul ? "Multiplication"
                     : I.getOpcode() == Instruction::FDiv ? "Division"
                                                          : "Remainder";
    emitExtern(B, binarySignature(Ty, Op),
               {slotFor(I.getOperand(0)), slotFor(I.getOperand(1))},
               slotFor(&I));
    return;
  }
  case Instruction::FNeg: {
    std::string Ty = operandType(I.getType());
    // `fneg` is `0 - x`: the float types expose `UnaryMinus`, not
    // `UnaryNegation` (that name belongs to Boolean and the Unity vectors).
    std::string Zero = constantSlot(ConstantFP::get(I.getType(), 0.0));
    emitExtern(B, binarySignature(Ty, "Subtraction"), {Zero, slotFor(I.getOperand(0))},
               slotFor(&I));
    return;
  }
  case Instruction::FCmp:
    selectFCmp(cast<FCmpInst>(I), UF, Idx);
    return;
  case Instruction::InsertElement:
  case Instruction::ExtractElement:
  case Instruction::ShuffleVector:
    selectVectorOp(I, UF, Idx);
    return;
  case Instruction::FPExt:
  case Instruction::FPTrunc: {
    Type *DstTy = I.getType();
    const char *Sig = DstTy->isDoubleTy()
                          ? "SystemConvert.__ToDouble__SystemSingle__SystemDouble"
                          : "SystemConvert.__ToSingle__SystemDouble__SystemSingle";
    emitExtern(B, Sig, {slotFor(I.getOperand(0))}, slotFor(&I));
    return;
  }
  case Instruction::SIToFP: {
    Type *DstTy = I.getType();
    Type *SrcTy = I.getOperand(0)->getType();
    std::string Dst = DstTy->isDoubleTy() ? "SystemDouble" : "SystemSingle";
    std::string Op = DstTy->isDoubleTy() ? "Double" : "Single";
    // A narrow (i8/i16) source lives in an Int32 slot, and the conversion
    // externs only exist from Int32 and Int64 -- never from an Int32 slot that
    // holds a narrow value. Sign-extend narrow sources to i64 first, exactly as
    // the i32 path does for unsigned values: the signed `Convert.To*Int64` of a
    // sign-extended i32 is the value's own sign-extension.
    std::string Src = slotFor(I.getOperand(0));
    unsigned Bits = SrcTy->getIntegerBitWidth();
    if (Bits < 32) {
      // Narrow sources: sign-extend to the i32 slot width, then convert from
      // the Int32 slot directly. `__ToSingle__SystemInt32` is the one narrow
      // shape the SDK has; there is no i16/i8 conversion extern.
      //
      // The mask that used to precede the left shift is redundant, exactly as
      // in `emitLoad`: `(x << 24) >> 24` sign-extends the low byte of any x,
      // so masking first costs one extern to change nothing. The value this
      // receives is usually the output of an i8 `load`, which the load path
      // already sign-extended -- a second mask-and-shift there was pure waste.
      //
      // A value that came straight from an `i8`/`i16` load is already
      // sign-extended, so the widening pair would reproduce what it holds.
      // `SignExtendedSlots` records exactly those; anything else (a `trunc`, a
      // `phi`, a `select`) is unmarked and keeps the shifts.
      if (!SignExtendedSlots.count(Src)) {
        int32_t Shift = 32 - static_cast<int32_t>(Bits);
        std::string L =
            memBinop("LeftShift", Src, i32Constant(Shift), "sitofp_l", B);
        std::string E =
            memBinop("RightShift", L, i32Constant(Shift), "sitofp_r", B);
        Src = E;
      }
    }
    std::string From = Bits == 64 ? "SystemInt64" : "SystemInt32";
    emitExtern(B, "SystemConvert.__To" + Op + "__" + From + "__" + Dst, {Src},
               slotFor(&I));
    return;
  }
  case Instruction::UIToFP: {
    // Zero-extend to i64 first, then convert: an i32 above Int32.MaxValue is
    // negative in its slot, so a direct Convert would produce a negative float.
    // A narrow (i8/i16) source is zero-extended the same way: mask to its own
    // width first, because the high bits of the Int32 slot are unspecified.
    Type *DstTy = I.getType();
    Type *SrcTy = I.getOperand(0)->getType();
    std::string Dst = DstTy->isDoubleTy() ? "SystemDouble" : "SystemSingle";
    std::string Op = DstTy->isDoubleTy() ? "Double" : "Single";
    std::string Wide = slotFor(I.getOperand(0));
    if (SrcTy->isIntegerTy(8) || SrcTy->isIntegerTy(16)) {
      unsigned Bits = SrcTy->getIntegerBitWidth();
      // Mask to the value's own width, then widen: the destination extern is
      // `__To*__SystemInt64`, so the masked Int32 must become an Int64 slot.
      std::string Masked =
          memBinop("LogicalAnd", Wide, i32Constant(Bits == 8 ? 0xFF : 0xFFFF),
                   "uitofp_n", B);
      Wide = freshSlot("uitofp_w", Type::getInt64Ty(Mod->getContext()));
      emitExtern(B, "SystemConvert.__ToInt64__SystemInt32__SystemInt64",
                 {Masked}, Wide);
    }
    if (SrcTy->isIntegerTy(32)) {
      Wide = freshSlot("uitofp_w", Type::getInt64Ty(Mod->getContext()));
      emitExtern(B, "SystemConvert.__ToInt64__SystemInt32__SystemInt64",
                 {slotFor(I.getOperand(0))}, Wide);
      std::string Masked = freshSlot("uitofp_m", Type::getInt64Ty(Mod->getContext()));
      emitExtern(B,
                 "SystemInt64.__op_LogicalAnd__SystemInt64_SystemInt64__SystemInt64",
                 {Wide, constantSlot(ConstantInt::get(
                            Type::getInt64Ty(Mod->getContext()), 0xFFFFFFFFull))},
                 Masked);
      Wide = Masked;
    }
    emitExtern(B, "SystemConvert.__To" + Op + "__SystemInt64__" + Dst, {Wide},
               slotFor(&I));
    return;
  }
  case Instruction::FPToSI:
  case Instruction::FPToUI:
    selectFPToInt(I, UF, Idx);
    return;
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Shl:
  case Instruction::AShr: {
    // `ashr exact <typed-index-difference>, 2`: the IR believes the cursor
    // difference is a byte offset and shifts it to elements, but the typed
    // partition made both cursors ELEMENT indices, so the shift must go. A
    // plain copy keeps the value exactly.
    if (I.getOpcode() == Instruction::AShr) {
      auto *CI = dyn_cast<ConstantInt>(I.getOperand(1));
      if (CI && CI->getZExtValue() == 2 &&
          ElementIndexValues.count(I.getOperand(0))) {
        emitCopy(B, slotFor(I.getOperand(0)), slotFor(&I));
        ElementIndexValues.insert(&I);
        return;
      }
    }
    std::string Ty = operandType(I.getType());
    if (Ty.empty())
      reportUDONError("arithmetic on an unsupported type: " + I.getName());
    StringRef Op;
    switch (I.getOpcode()) {
    case Instruction::Add: Op = "Addition"; break;
    case Instruction::Sub: Op = "Subtraction"; break;
    case Instruction::Mul: Op = "Multiplication"; break;
    case Instruction::And: Op = "LogicalAnd"; break;
    case Instruction::Or: Op = "LogicalOr"; break;
    case Instruction::Xor: Op = "LogicalXor"; break;
    case Instruction::Shl: Op = "LeftShift"; break;
    case Instruction::AShr: Op = "RightShift"; break;
    default: llvm_unreachable("opcode");
    }
    emitExtern(B, binarySignature(Ty, Op),
               {slotFor(I.getOperand(0)), slotFor(I.getOperand(1))},
               slotFor(&I));
    return;
  }
  case Instruction::SDiv: {
    std::string Ty = operandType(I.getType());
    emitExtern(B, binarySignature(Ty, "Division"),
               {slotFor(I.getOperand(0)), slotFor(I.getOperand(1))},
               slotFor(&I));
    return;
  }
  case Instruction::SRem: {
    std::string Ty = operandType(I.getType());
    if (Ty == "SystemInt64") {
      // SystemInt64.__op_Remainder is registered but implemented nowhere: the
      // delegate request throws. a - (a/b)*b uses only operators that work.
      std::string Q = freshSlot("rem_q", I.getType());
      emitExtern(B, binarySignature(Ty, "Division"),
                 {slotFor(I.getOperand(0)), slotFor(I.getOperand(1))}, Q);
      std::string P = freshSlot("rem_p", I.getType());
      emitExtern(B, binarySignature(Ty, "Multiplication"),
                 {Q, slotFor(I.getOperand(1))}, P);
      emitExtern(B, binarySignature(Ty, "Subtraction"),
                 {slotFor(I.getOperand(0)), P}, slotFor(&I));
      return;
    }
    emitExtern(B, binarySignature(Ty, "Remainder"),
               {slotFor(I.getOperand(0)), slotFor(I.getOperand(1))},
               slotFor(&I));
    return;
  }
  case Instruction::UDiv:
  case Instruction::URem:
    if (I.getType()->isIntegerTy(64))
      selectUDivRemI64(I, UF, Idx);
    else
      selectUDivRem(I, UF, Idx);
    return;
  case Instruction::LShr:
    selectLShr(I, UF, Idx);
    return;
  case Instruction::ICmp:
    selectICmp(cast<ICmpInst>(I), UF, Idx);
    return;
  case Instruction::Select:
    selectSelect(cast<SelectInst>(I), UF, Idx);
    return;
  case Instruction::PHI:
    return;
  case Instruction::Freeze:
    emitCopy(B, slotFor(I.getOperand(0)), slotFor(&I));
    return;
  case Instruction::GetElementPtr:
    selectGEP(cast<GetElementPtrInst>(I), UF, Idx);
    return;
  case Instruction::ExtractValue: {
    // Aggregates live as one slot per leaf field, named
    // `<agg>_f<N>_f<M>...`, so an extract is a plain copy out of one leaf
    // slot. The index list descends the type to find the leaf.
    const auto &EV = cast<ExtractValueInst>(I);
    std::string Leaf = slotFor(EV.getAggregateOperand());
    Type *Ty = EV.getAggregateOperand()->getType();
    for (unsigned Idx : EV.indices()) {
      unsigned UIdx = static_cast<unsigned>(Idx);
      Leaf += "_f" + std::to_string(UIdx);
      Ty = aggElementAt(Ty, UIdx);
      if (!Ty)
        reportUDONError("extractvalue descends a non-aggregate: " + I.getName());
    }
    std::string LeafTy = udonTypeOf(Ty);
    if (LeafTy.empty())
      reportUDONError("aggregate leaf has no Udon type: " + I.getName());
    if (!SlotTypes.count(Leaf)) {
      declareSlot(Leaf, LeafTy);
      NullSlots.push_back({Leaf, LeafTy});
    }
    emitCopy(B, Leaf, slotFor(&I));
    return;
  }
  case Instruction::InsertValue: {
    // Copy the fields the insert does not touch, then write the inserted
    // one: a fresh prefix per insert, one COPY per field. Nested indices
    // (`fca.0.1` = field 0's subfield 1) append to the prefix.
    const auto &IV = cast<InsertValueInst>(I);
    // Collect the leaf type by walking the indices.
    Type *Ty = I.getType();
    for (unsigned Idx : IV.indices()) {
      auto *STy = cast<StructType>(Ty);
      Ty = STy->getElementType(static_cast<unsigned>(Idx));
    }
    std::string LeafTy = udonTypeOf(Ty);
    if (LeafTy.empty())
      reportUDONError("insertvalue leaf has no Udon type: " + I.getName());
    // The prefix of the DESTINATION and of the SOURCE aggregate.
    std::string DstPrefix = slotFor(&I);
    std::string SrcPrefix = slotFor(IV.getAggregateOperand());
    // The path this insert replaces: everything except the last index is a
    // prefix to COPY through.
    SmallVector<unsigned, 4> Idxs;
    for (unsigned Idx : IV.indices())
      Idxs.push_back(Idx);
    // 1. Copy every sibling at the top level that is NOT on the path.
    auto *Top = cast<StructType>(I.getType());
    unsigned TopIdx = Idxs.front();
    for (unsigned F = 0; F < Top->getNumElements(); F++) {
      if (F == TopIdx)
        continue;
      std::string STy2 = udonTypeOf(Top->getElementType(F));
      if (STy2.empty())
        continue; // nested aggregate handled below when needed
      emitCopy(B, SrcPrefix + "_f" + std::to_string(F),
               DstPrefix + "_f" + std::to_string(F));
    }
    // 2. The leaf write: build the leaf names.
    std::string DstLeaf = DstPrefix;
    std::string SrcLeaf = SrcPrefix;
    Type *LT = I.getType();
    for (unsigned Idx : Idxs) {
      auto *STy = cast<StructType>(LT);
      unsigned UIdx = Idx;
      DstLeaf += "_f" + std::to_string(UIdx);
      SrcLeaf += "_f" + std::to_string(UIdx);
      LT = STy->getElementType(UIdx);
    }
    if (!SlotTypes.count(DstLeaf)) {
      declareSlot(DstLeaf, LeafTy);
      NullSlots.push_back({DstLeaf, LeafTy});
    }
    if (!SlotTypes.count(SrcLeaf)) {
      declareSlot(SrcLeaf, LeafTy);
      NullSlots.push_back({SrcLeaf, LeafTy});
    }
    emitCopy(B, slotFor(IV.getInsertedValueOperand()), DstLeaf);
    return;
  }
  case Instruction::BitCast:
    selectBitCast(I, UF, Idx);
    return;
  case Instruction::Trunc:
    selectTrunc(I, UF, Idx);
    return;
  case Instruction::ZExt:
    selectZExt(I, UF, Idx);
    return;
  case Instruction::SExt:
    selectSExt(I, UF, Idx);
    return;
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
    emitCopy(B, slotFor(I.getOperand(0)), slotFor(&I));
    return;
  case Instruction::Call:
    selectCall(cast<CallInst>(I), UF, Idx);
    return;
  case Instruction::Load:
    selectLoad(cast<LoadInst>(I), UF, Idx);
    return;
  case Instruction::Store:
    selectStore(cast<StoreInst>(I), UF, Idx);
    return;
  case Instruction::Alloca:
    selectAlloca(cast<AllocaInst>(I), UF, Idx);
    return;
  case Instruction::Unreachable:
    return;
  default:
    reportUDONError("unsupported instruction: " + Twine(I.getOpcodeName()) +
                    " in " + I.getFunction()->getName() + ": " + I.getName());
  }
}

void UDONWriter::selectTerminator(const Instruction &I, UFunction &UF,
                                  unsigned &Idx) {
  // Not cached before the switch. Selecting this block's instructions may have
  // split it (an unaligned word access appends a continuation), and the
  // terminator belongs in the continuation: writing it into the closed block
  // would leave the continuation empty, so control would fall through into
  // whatever block happened to follow in layout order.
  UBlock &B = UF.Blocks[Idx];
  const BasicBlock *BB = I.getParent();
  switch (I.getOpcode()) {
  case Instruction::Ret: {
    const auto &RI = cast<ReturnInst>(I);
    if (Value *RV = RI.getReturnValue())
      emitCopy(B, slotFor(RV), returnValueSlot(BB->getParent()));
    // An address-taken function leaves its result in the transfer slot too, so
    // an indirect caller can read it.
    if (FunctionPointerIndices.count(BB->getParent()))
      if (Value *RV = RI.getReturnValue())
        emitCopy(B, slotFor(RV), "__icall_retval");
    // Release this activation's frame before returning. Every frame has a fixed
    // size, so the add is exact and no save slot is needed.
    emitFramePop(BB->getParent(), B);
    B.Insts.push_back({UInst::JUMP_INDIRECT, returnSlot(BB->getParent())});
    return;
  }
  case Instruction::Br: {
    const auto &BI = cast<BranchInst>(I);
    if (BI.isUnconditional()) {
      emitPhiCopies(BB, BI.getSuccessor(0), B);
      B.Insts.push_back({UInst::JUMP, edgeTarget(UF, BB, BI.getSuccessor(0))});
      return;
    }
    std::string Cond = boolSlotFor(BI.getCondition(), B);
    std::string TrueTarget = edgeTarget(UF, BB, BI.getSuccessor(0));
    std::string FalseTarget = edgeTarget(UF, BB, BI.getSuccessor(1));
    B.Insts.push_back({UInst::PUSH, Cond});
    B.Insts.push_back({UInst::JUMP_IF_FALSE, FalseTarget});
    B.Insts.push_back({UInst::JUMP, TrueTarget});
    return;
  }
  case Instruction::Switch:
    reportUDONError("switch reached the writer: " + BB->getParent()->getName());
  case Instruction::Invoke:
    reportUDONError("exceptions are not supported: " +
                    BB->getParent()->getName());
  case Instruction::Unreachable:
    return;
  default:
    reportUDONError("unsupported terminator: " + Twine(I.getOpcodeName()) +
                    " in " + BB->getParent()->getName());
  }
}

// --- phi copies ------------------------------------------------------------

void UDONWriter::emitPhiCopies(const BasicBlock *Pred, const BasicBlock *Succ,
                               UBlock &B) {
  std::vector<std::pair<std::string, std::string>> Copies;
  for (const PHINode &Phi : Succ->phis()) {
    Value *Incoming = Phi.getIncomingValueForBlock(Pred);
    if (isa<UndefValue>(Incoming))
      continue;
    Copies.push_back({slotFor(Incoming), slotFor(&Phi)});
  }
  if (Copies.empty())
    return;

  // Sequential copies are wrong when a phi's source is another phi's
  // destination, so the set is emitted with cycle detection: a copy runs only
  // once nothing else still needs to read its destination.
  std::vector<bool> Done(Copies.size(), false);
  bool Progress = true;
  while (Progress) {
    Progress = false;
    for (size_t I = 0; I < Copies.size(); I++) {
      if (Done[I])
        continue;
      bool Blocked = false;
      for (size_t J = 0; J < Copies.size(); J++)
        if (!Done[J] && J != I && Copies[J].second == Copies[I].first) {
          Blocked = true;
          break;
        }
      if (Blocked)
        continue;
      emitCopy(B, Copies[I].first, Copies[I].second);
      Done[I] = true;
      Progress = true;
    }
  }
  // Whatever remains is one or more cycles. Save the destination, redirect its
  // readers through the temporary, and the cycle opens up.
  for (size_t I = 0; I < Copies.size(); I++) {
    if (Done[I])
      continue;
    std::string Temp = freshSlot("phi", nullptr);
    emitCopy(B, Copies[I].second, Temp);
    std::string Saved = Copies[I].second;
    for (size_t J = 0; J < Copies.size(); J++)
      if (!Done[J] && Copies[J].first == Saved)
        Copies[J].first = Temp;
    Progress = true;
    while (Progress) {
      Progress = false;
      for (size_t J = 0; J < Copies.size(); J++) {
        if (Done[J])
          continue;
        bool Blocked = false;
        for (size_t K = 0; K < Copies.size(); K++)
          if (!Done[K] && K != J && Copies[K].second == Copies[J].first) {
            Blocked = true;
            break;
          }
        if (Blocked)
          continue;
        emitCopy(B, Copies[J].first, Copies[J].second);
        Done[J] = true;
        Progress = true;
      }
    }
  }
}

std::string UDONWriter::edgeTarget(UFunction &UF, const BasicBlock *Pred,
                                   const BasicBlock *Succ) {
  bool HasCopies = false;
  for (const PHINode &Phi : Succ->phis()) {
    if (!isa<UndefValue>(Phi.getIncomingValueForBlock(Pred))) {
      HasCopies = true;
      break;
    }
  }
  // An unconditional edge carries its copies inline, so only a conditional edge
  // needs a block of its own.
  const Instruction *T = Pred->getTerminator();
  if (!HasCopies || (T->getOpcode() == Instruction::Br &&
                     cast<BranchInst>(T)->isUnconditional()))
    return blockLabel(Succ);

  // One trampoline per (pred, succ) pair: a branch whose two edges share a
  // successor runs the same copies either way.
  std::string Label = blockLabel(Pred) + "_to_" + blockLabel(Succ);
  auto Key = std::make_pair(Pred, Succ);
  auto It = UF.EdgeIndex.find(Key);
  if (It != UF.EdgeIndex.end())
    return UF.Blocks[It->second].Label;

  unsigned Idx = appendBlock(UF, Label);
  emitPhiCopies(Pred, Succ, UF.Blocks[Idx]);
  UF.Blocks[Idx].Insts.push_back({UInst::JUMP, blockLabel(Succ)});
  UF.EdgeIndex[Key] = Idx;
  return Label;
}

// --- compares and casts ----------------------------------------------------

void UDONWriter::selectICmp(const ICmpInst &I, UFunction &UF, unsigned &Idx) {
  CmpInst::Predicate P = I.getPredicate();
  Type *OpTy = I.getOperand(0)->getType();
  std::string Ty = operandType(OpTy);
  if (Ty.empty())
    reportUDONError("compare on an unsupported type: " + I.getName());

  std::string LHS = slotFor(I.getOperand(0));
  std::string RHS = slotFor(I.getOperand(1));

  // Width normalization. A value of a type narrower than a slot must be brought
  // to the slot's width *in the comparison's sense* before comparing: a signed
  // comparison sign-extends, an unsigned or equality comparison zero-extends.
  //
  // Skipping this is a miscompile, not an inefficiency: `(char)0xD0 < 10`
  // unsigned is false, but a sign-extended -48 is less than 10 in a 32-bit
  // signed compare, and the unsigned XOR trick cannot repair that -- the sign
  // bit of a byte is not bit 31. A digit-range test written that way loops
  // forever, because the byte after the digits reads as a valid digit.
  unsigned OpBits = OpTy->isPointerTy() ? 32 : OpTy->getIntegerBitWidth();
  bool Narrow = !OpTy->isPointerTy() && OpBits < 32;

  auto Extend = [&](const std::string &Slot, bool Signed) {
    if (!Narrow)
      return Slot;
    int32_t Mask = OpBits == 8 ? 0xFF : 0xFFFF;
    std::string M = memBinop("LogicalAnd", Slot, i32Constant(Mask), "ext1",
                             UF.Blocks[Idx]);
    if (!Signed)
      return M;
    int32_t Shift = 32 - static_cast<int32_t>(OpBits);
    std::string L = memBinop("LeftShift", M, i32Constant(Shift), "ext2",
                             UF.Blocks[Idx]);
    return memBinop("RightShift", L, i32Constant(Shift), "ext3",
                    UF.Blocks[Idx]);
  };

  if (Narrow) {
    bool Signed = CmpInst::isSigned(P);
    LHS = Extend(LHS, Signed);
    RHS = Extend(RHS, Signed);
  }

  // Unsigned compares XOR both sides with the sign bit and use the signed
  // predicate: Udon has no unsigned operators on these slot types, and the XOR
  // costs one extern with no allocation. Verified: -1 < 1 unsigned is false.
  if (isUnsignedPredicate(P)) {
    bool Wide = OpBits == 64;
    uint64_t MaskBits = Wide ? 0x8000000000000000ull : 0x80000000ull;
    // The mask must have the comparison's width, which after normalization is
    // 32 bits for every narrow type. Building it from a narrow type truncates
    // the constant to zero and silently disables the trick.
    Type *MaskTy = Wide ? OpTy : Type::getInt32Ty(OpTy->getContext());
    std::string Mask = constantSlot(ConstantInt::get(MaskTy, MaskBits));
    std::string X = freshSlot("uxor_l", MaskTy);
    emitExtern(UF.Blocks[Idx], binarySignature(Ty, "LogicalXor"), {LHS, Mask}, X);
    std::string Y = freshSlot("uxor_r", MaskTy);
    emitExtern(UF.Blocks[Idx], binarySignature(Ty, "LogicalXor"), {RHS, Mask}, Y);
    emitExtern(UF.Blocks[Idx],
               compareSignature(Ty,
                                icmpPredicateName(signedEquivalentOfUnsigned(P))),
               {X, Y}, slotFor(&I));
    return;
  }

  StringRef Op = icmpPredicateName(P);
  if (Op.empty())
    reportUDONError("unsupported icmp predicate: " + I.getName());
  emitExtern(UF.Blocks[Idx], compareSignature(Ty, Op), {LHS, RHS}, slotFor(&I));

}

void UDONWriter::selectSelect(const SelectInst &I, UFunction &UF,
                              unsigned &Idx) {
  std::string Cond = boolSlotFor(I.getCondition(), UF.Blocks[Idx]);
  Idx = emitDiamond(UF, Idx, Cond, slotFor(I.getTrueValue()),
                    slotFor(I.getFalseValue()), slotFor(&I), "sel");
}

void UDONWriter::selectTrunc(const Instruction &I, UFunction &UF,
                             unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  Type *DstTy = I.getType();
  Type *SrcTy = I.getOperand(0)->getType();
  if (DstTy->isIntegerTy(1)) {
    emitExtern(B, "SystemConvert.__ToBoolean__SystemInt32__SystemBoolean",
               {slotFor(I.getOperand(0))}, slotFor(&I));
    return;
  }
  if (SrcTy->isIntegerTy(64) && DstTy->isIntegerTy(32)) {
    emitCopy(B, emitTruncI64ToI32(slotFor(I.getOperand(0)), B), slotFor(&I));
    return;
  }
  if (DstTy->isIntegerTy(8) || DstTy->isIntegerTy(16)) {
    // A narrow value lives in a `%SystemInt32` slot masked to its width: no
    // lowering may produce a `%SystemByte` slot, because
    // SystemConvert.__ToInt32__SystemByte aborts in the VM.
    unsigned Width = DstTy->getIntegerBitWidth();
    emitExtern(B,
               "SystemInt32.__op_LogicalAnd__SystemInt32_SystemInt32__SystemInt32",
               {slotFor(I.getOperand(0)),
                i32Constant(Width == 8 ? 0xFF : 0xFFFF)},
               slotFor(&I));
    return;
  }
  reportUDONError("unsupported trunc: " + I.getName());
}

void UDONWriter::selectZExt(const Instruction &I, UFunction &UF,
                            unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  Type *DstTy = I.getType();
  Type *SrcTy = I.getOperand(0)->getType();
  // Narrow integers all live in `%SystemInt32` slots, so an extension to i8 or
  // i16 is the same operation as one to i32.
  bool DstNarrow =
      DstTy->isIntegerTy(32) || DstTy->isIntegerTy(16) || DstTy->isIntegerTy(8);
  if (SrcTy->isIntegerTy(1) && DstNarrow) {
    emitExtern(B, "SystemConvert.__ToInt32__SystemBoolean__SystemInt32",
               {slotFor(I.getOperand(0))}, slotFor(&I));
    return;
  }
  if ((SrcTy->isIntegerTy(8) || SrcTy->isIntegerTy(16)) &&
      DstTy->isIntegerTy(64)) {
    // Sign-extend in the slot's i32, where the value already is, then convert.
    unsigned Width = SrcTy->getIntegerBitWidth();
    int32_t Shift = 32 - static_cast<int32_t>(Width);
    std::string Masked =
        memBinop("LogicalAnd", slotFor(I.getOperand(0)),
                 i32Constant(Width == 8 ? 0xFF : 0xFFFF), "sext64m", B);
    std::string L =
        memBinop("LeftShift", Masked, i32Constant(Shift), "sext64l", B);
    std::string SignExtended =
        memBinop("RightShift", L, i32Constant(Shift), "sext64r", B);
    emitExtern(B, "SystemConvert.__ToInt64__SystemInt32__SystemInt64",
               {SignExtended}, slotFor(&I));
    return;
  }
  if ((SrcTy->isIntegerTy(8) || SrcTy->isIntegerTy(16)) && DstNarrow) {
    // A narrow value already lives in a `%SystemInt32` slot; a load sign-extends
    // it, so the zero extension is a mask back to the narrow width.
    unsigned Width = SrcTy->getIntegerBitWidth();
    emitExtern(B,
               "SystemInt32.__op_LogicalAnd__SystemInt32_SystemInt32__SystemInt32",
               {slotFor(I.getOperand(0)),
                i32Constant(Width == 8 ? 0xFF : 0xFFFF)},
               slotFor(&I));
    return;
  }
  if ((SrcTy->isIntegerTy(8) || SrcTy->isIntegerTy(16)) &&
      DstTy->isIntegerTy(64)) {
    // Clear the high bits of the slot first, then convert: Convert.ToInt64 of a
    // non-negative i32 is exact, so no masking of the wide value is needed.
    unsigned Width = SrcTy->getIntegerBitWidth();
    std::string Masked =
        memBinop("LogicalAnd", slotFor(I.getOperand(0)),
                 i32Constant(Width == 8 ? 0xFF : 0xFFFF), "zext64", B);
    emitExtern(B, "SystemConvert.__ToInt64__SystemInt32__SystemInt64", {Masked},
               slotFor(&I));
    return;
  }
  if (SrcTy->isIntegerTy(32) && DstTy->isIntegerTy(64)) {
    std::string Wide = freshSlot("zext", DstTy);
    emitExtern(B, "SystemConvert.__ToInt64__SystemInt32__SystemInt64",
               {slotFor(I.getOperand(0))}, Wide);
    std::string Mask = constantSlot(ConstantInt::get(DstTy, 0xFFFFFFFFull));
    emitExtern(B,
               "SystemInt64.__op_LogicalAnd__SystemInt64_SystemInt64__SystemInt64",
               {Wide, Mask}, slotFor(&I));
    return;
  }
  if (SrcTy->isIntegerTy(1) && DstTy->isIntegerTy(64)) {
    emitExtern(B, "SystemConvert.__ToInt64__SystemBoolean__SystemInt64",
               {slotFor(I.getOperand(0))}, slotFor(&I));
    return;
  }
  reportUDONError("unsupported zext: i" +
                  Twine(SrcTy->getIntegerBitWidth()) + " to i" +
                  Twine(DstTy->isIntegerTy() ? DstTy->getIntegerBitWidth() : 0) +
                  (I.getName().empty() ? Twine("") : " (" + I.getName() + ")"));
}

void UDONWriter::selectSExt(const Instruction &I, UFunction &UF,
                            unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  Type *DstTy = I.getType();
  Type *SrcTy = I.getOperand(0)->getType();
  // As with zext, i8 and i16 live in `%SystemInt32` slots, so extending to
  // either is extending to i32.
  bool DstNarrow =
      DstTy->isIntegerTy(32) || DstTy->isIntegerTy(16) || DstTy->isIntegerTy(8);
  if (SrcTy->isIntegerTy(1)) {
    // `true` sign-extends to -1, not 1: 0 - Convert.ToBoolean(x). Rust emits
    // sext i1 for mask idioms, so the all-ones expectation is load-bearing.
    if (DstNarrow) {
      std::string One = freshSlot("sext1", DstTy);
      emitExtern(B, "SystemConvert.__ToInt32__SystemBoolean__SystemInt32",
                 {slotFor(I.getOperand(0))}, One);
      emitExtern(B,
                 "SystemInt32.__op_Subtraction__SystemInt32_SystemInt32__SystemInt32",
                 {i32Constant(0), One}, slotFor(&I));
      return;
    }
    if (DstTy->isIntegerTy(64)) {
      std::string Wide = freshSlot("sext1w", DstTy);
      emitExtern(B, "SystemConvert.__ToInt64__SystemBoolean__SystemInt64",
                 {slotFor(I.getOperand(0))}, Wide);
      emitExtern(B,
                 "SystemInt64.__op_Subtraction__SystemInt64_SystemInt64__SystemInt64",
                 {constantSlot(ConstantInt::get(DstTy, 0)), Wide}, slotFor(&I));
      return;
    }
  }
  if (SrcTy->isIntegerTy(32) && DstTy->isIntegerTy(64)) {
    emitExtern(B, "SystemConvert.__ToInt64__SystemInt32__SystemInt64",
               {slotFor(I.getOperand(0))}, slotFor(&I));
    return;
  }
  if ((SrcTy->isIntegerTy(8) || SrcTy->isIntegerTy(16)) && DstNarrow) {
    // The load already sign-extends into the `%SystemInt32` slot, so a sext to
    // i32 is the value itself. Masking to the width first is what makes it
    // correct for a value that reached the slot by a narrower route.
    unsigned Width = SrcTy->getIntegerBitWidth();
    std::string Masked = freshSlot("sext_m", DstTy);
    emitExtern(B,
               "SystemInt32.__op_LogicalAnd__SystemInt32_SystemInt32__SystemInt32",
               {slotFor(I.getOperand(0)),
                i32Constant(Width == 8 ? 0xFF : 0xFFFF)},
               Masked);
    std::string Shifted = freshSlot("sext_l", DstTy);
    emitExtern(B,
               "SystemInt32.__op_LeftShift__SystemInt32_SystemInt32__SystemInt32",
               {Masked, i32Constant(Width == 8 ? 24 : 16)}, Shifted);
    emitExtern(B,
               "SystemInt32.__op_RightShift__SystemInt32_SystemInt32__SystemInt32",
               {Shifted, i32Constant(Width == 8 ? 24 : 16)}, slotFor(&I));
    return;
  }
  reportUDONError("unsupported sext: " + I.getName());
}

void UDONWriter::selectBitCast(const Instruction &I, UFunction &UF,
                               unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  Type *SrcTy = I.getOperand(0)->getType();
  Type *DstTy = I.getType();
  if (SrcTy->isPointerTy() || DstTy->isPointerTy()) {
    emitCopy(B, slotFor(I.getOperand(0)), slotFor(&I));
    return;
  }
  unsigned SrcBits = SrcTy->getPrimitiveSizeInBits();
  unsigned DstBits = DstTy->getPrimitiveSizeInBits();
  if (SrcBits == 0 || SrcBits != DstBits)
    reportUDONError("unsupported bitcast: " + I.getName());

  // The reinterpret externs, not a memory round-trip: they were verified to
  // work (1.5f -> 0x3FC00000 -> 1.5f), and they touch no memory, so a bitcast
  // no longer depends on the scratch region being valid.
  const char *Sig = nullptr;
  if (SrcBits == 32 && SrcTy->isFloatTy() && DstTy->isIntegerTy(32))
    Sig = "SystemBitConverter.__SingleToInt32Bits__SystemSingle__SystemInt32";
  else if (SrcBits == 32 && SrcTy->isIntegerTy(32) && DstTy->isFloatTy())
    Sig = "SystemBitConverter.__Int32BitsToSingle__SystemInt32__SystemSingle";
  else if (SrcBits == 64 && SrcTy->isDoubleTy() && DstTy->isIntegerTy(64))
    Sig = "SystemBitConverter.__DoubleToInt64Bits__SystemDouble__SystemInt64";
  else if (SrcBits == 64 && SrcTy->isIntegerTy(64) && DstTy->isDoubleTy())
    Sig = "SystemBitConverter.__Int64BitsToDouble__SystemInt64__SystemDouble";
  if (!Sig)
    reportUDONError("unsupported bitcast: " + I.getName());
  emitExtern(B, Sig, {slotFor(I.getOperand(0))}, slotFor(&I));
}

// --- logical right shift ---------------------------------------------------

void UDONWriter::selectLShr(const Instruction &I, UFunction &UF, unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  // Udon has no unsigned right shift, so an `lshr` is `ashr` plus a mask that
  // clears the bits the sign extension filled in: shift by n, mask by
  // (signMask >> (n - 1)).
  Type *Ty = I.getType();
  std::string Prefix = operandType(Ty);
  unsigned Bits = Ty->getIntegerBitWidth();
  std::string X = slotFor(I.getOperand(0));
  const Value *Count = I.getOperand(1);

  if (auto *CI = dyn_cast<ConstantInt>(Count)) {
    uint64_t N = CI->getZExtValue();
    if (N == 0) {
      emitCopy(B, X, slotFor(&I));
      return;
    }
    std::string Shifted = freshSlot("lshr_s", Ty);
    emitExtern(B, binarySignature(Prefix, "RightShift"), {X, slotFor(Count)},
               Shifted);
    // The sign mask must match the *type* width, not the slot width: i8/i16
    // live in `%SystemInt32` slots (sign-extended), so a 32-bit mask would
    // leave the high sign copies in place and produce a garbage index. For
    // `i16` a mask of `0x7FFF >> (n-1)` equals `0xFFFF >> n`, which is exactly
    // the logical-shift result for the value's own width.
    uint64_t SignMask = Bits >= 64   ? 0x7FFFFFFFFFFFFFFFull
                        : Bits == 32 ? 0x7FFFFFFFull
                        : Bits == 16 ? 0x7FFFull
                                     : 0x7Full;
    uint64_t MaskBits = N >= Bits ? 0 : (SignMask >> (N - 1));
    emitExtern(B, binarySignature(Prefix, "LogicalAnd"),
               {Shifted, constantSlot(ConstantInt::get(Ty, MaskBits))},
               slotFor(&I));
    return;
  }

  // Variable count: select n == 0 -> x, else (x >> n) & (signMask >> (n - 1)).
  std::string Shifted = freshSlot("lshr_s", Ty);
  emitExtern(B, binarySignature(Prefix, "RightShift"), {X, slotFor(Count)},
             Shifted);
  uint64_t SignMaskBits = Bits >= 64   ? 0x7FFFFFFFFFFFFFFFull
                            : Bits == 32 ? 0x7FFFFFFFull
                            : Bits == 16 ? 0x7FFFull
                                         : 0x7Full;
  std::string SignMask = constantSlot(ConstantInt::get(Ty, SignMaskBits));
  std::string NM1 = freshSlot("lshr_nm1", Ty);
  emitExtern(B, binarySignature(Prefix, "Subtraction"),
             {slotFor(Count), constantSlot(ConstantInt::get(Ty, 1))}, NM1);
  std::string Mask = freshSlot("lshr_mask", Ty);
  emitExtern(B, binarySignature(Prefix, "RightShift"), {SignMask, NM1}, Mask);
  std::string Masked = freshSlot("lshr_masked", Ty);
  emitExtern(B, binarySignature(Prefix, "LogicalAnd"), {Shifted, Mask}, Masked);

  std::string IsZero = freshSlot("lshr_z", Type::getInt1Ty(Mod->getContext()));
  emitExtern(B, compareSignature(Prefix, "Equality"),
             {slotFor(Count), constantSlot(ConstantInt::get(Ty, 0))}, IsZero);
  Idx = emitDiamond(UF, Idx, IsZero, X, Masked, slotFor(&I), "lshr");
}

// --- entry blocks ----------------------------------------------------------

void UDONWriter::buildEntryBlocks(const Function &F, UFunction &UF) {
  if (!EntryNames.count(&F))
    return;

  std::string ExitLabel = mangle(F.getName()) + "_exit";

  // The exit block ends the event's Interpret(): it zeroes the halt reason so
  // the harness can attribute a halt, then jumps to the sentinel. A halted
  // activation never reaches it, which is why zeroing cannot mask a real halt.
  unsigned ExitIdx = appendBlock(UF, ExitLabel);
  UBlock &Exit = UF.Blocks[ExitIdx];
  emitCopy(Exit, i32Constant(0), "__halt_reason");

  // An entry point also publishes its result under a name derived from the
  // export name, so the harness can read it without knowing the source
  // function's name. The copy lives here because only here is the value final.
  if (!F.getReturnType()->isVoidTy()) {
    std::string Public = "__retval_" + EntryNames[&F];
    declareSlot(Public, udonTypeOf(F.getReturnType()));
    NullSlots.push_back({Public, udonTypeOf(F.getReturnType())});
    emitCopy(Exit, returnValueSlot(&F), Public);
  }
  Exit.Insts.push_back({UInst::JUMP_INDIRECT, "__haltaddr"});

  // The stub writes the exit block's address into this function's own return
  // slot and falls into the body, so a `ret` from the top-level activation
  // lands in the exit block. Internal callers target the body, never the stub.
  //
  // `.export <name>` names a code label, so the stub's label *is* the export
  // name.
  unsigned StubIdx = appendBlock(UF, EntryNames[&F]);
  unsigned ContIdx = emitEntryPrologue(UF, StubIdx);

  // After the prologue: point this activation's return at the exit block and
  // enter the body. The exit address goes into the function's own return slot,
  // so a `ret` from the top-level activation lands in the exit block.
  UBlock &Cont = UF.Blocks[ContIdx];
  std::string ExitSlot =
      addressSlotForLabel("__exitaddr_" + mangle(F.getName()), ExitLabel);
  emitCopy(Cont, ExitSlot, returnSlot(&F));
  Cont.Insts.push_back({UInst::JUMP, UF.BodyLabel});

  CodeExports.push_back({EntryNames[&F], EntryNames[&F]});
}

// --- unsigned division -----------------------------------------------------

void UDONWriter::selectUDivRem(const Instruction &I, UFunction &UF,
                               unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  // Zero-extend both operands to i64, divide signed (both are non-negative, so
  // signed and unsigned division agree), then wrap the result back to i32.
  //
  // The wrap is mandatory, not cosmetic: 0xFFFFFFFF / 1 is 4294967295, which a
  // bare Convert.ToInt32 throws on. `emitTruncI64ToI32` is the only narrowing
  // that survives it.
  //
  // The remainder is a - (a/b)*b, because SystemInt64 has no working
  // __op_Remainder and this path is already in i64.
  Type *I32 = Type::getInt32Ty(Mod->getContext());
  Type *I64 = Type::getInt64Ty(Mod->getContext());
  std::string Mask = constantSlot(ConstantInt::get(I64, 0xFFFFFFFFull));

  auto zext = [&](const Value *V) {
    std::string Wide = freshSlot("udiv_z", I64);
    emitExtern(B, "SystemConvert.__ToInt64__SystemInt32__SystemInt64", {slotFor(V)},
               Wide);
    std::string Masked = freshSlot("udiv_m", I64);
    emitExtern(B,
               "SystemInt64.__op_LogicalAnd__SystemInt64_SystemInt64__SystemInt64",
               {Wide, Mask}, Masked);
    return Masked;
  };

  std::string A = zext(I.getOperand(0));
  std::string Bv = zext(I.getOperand(1));
  std::string Q = freshSlot("udiv_q", I64);
  emitExtern(B, "SystemInt64.__op_Division__SystemInt64_SystemInt64__SystemInt64",
             {A, Bv}, Q);

  std::string Value = Q;
  if (I.getOpcode() == Instruction::URem) {
    std::string P = freshSlot("urem_p", I64);
    emitExtern(B,
               "SystemInt64.__op_Multiplication__SystemInt64_SystemInt64__SystemInt64",
               {Q, Bv}, P);
    Value = freshSlot("urem_r", I64);
    emitExtern(B,
               "SystemInt64.__op_Subtraction__SystemInt64_SystemInt64__SystemInt64",
               {A, P}, Value);
  }

  std::string Narrow = emitTruncI64ToI32(Value, B);
  emitCopy(B, Narrow, slotFor(&I));
  (void)I32;
}

// --- floating-point compares ----------------------------------------------

void UDONWriter::selectFCmp(const FCmpInst &I, UFunction &UF, unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  CmpInst::Predicate P = I.getPredicate();
  Type *Ty = I.getOperand(0)->getType();
  std::string Prefix = operandType(Ty);
  std::string LHS = slotFor(I.getOperand(0));
  std::string RHS = slotFor(I.getOperand(1));
  std::string Out = slotFor(&I);
  std::string BoolTy = "SystemBoolean";

  auto Cmp = [&](StringRef Op) {
    emitExtern(B, compareSignature(Prefix, Op), {LHS, RHS}, Out);
  };
  auto Neg = [&](const std::string &Src) {
    emitExtern(B, "SystemBoolean.__op_UnaryNegation__SystemBoolean__SystemBoolean",
               {Src}, Out);
  };
  auto Or = [&](const std::string &A, const std::string &Bv) {
    emitExtern(B,
               "SystemBoolean.__op_LogicalOr__SystemBoolean_SystemBoolean__"
               "SystemBoolean",
               {A, Bv}, Out);
  };
  auto IsNaN = [&](const std::string &Src) {
    std::string T = freshSlot("nan", Type::getInt1Ty(Mod->getContext()));
    emitExtern(B, Prefix + ".__IsNaN__" + Prefix + "__SystemBoolean", {Src}, T);
    return T;
  };

  // C# comparisons are ordered (false on NaN), so the ordered predicates map
  // directly, and `une` maps to `!=`, which is true when either side is NaN.
  switch (P) {
  case CmpInst::FCMP_OEQ: Cmp("Equality"); return;
  case CmpInst::FCMP_OGT: Cmp("GreaterThan"); return;
  case CmpInst::FCMP_OGE: Cmp("GreaterThanOrEqual"); return;
  case CmpInst::FCMP_OLT: Cmp("LessThan"); return;
  case CmpInst::FCMP_OLE: Cmp("LessThanOrEqual"); return;
  case CmpInst::FCMP_UNE: Cmp("Inequality"); return;

  case CmpInst::FCMP_ONE: {
    // one = olt || ogt, not `!=`: `!=` is also true for a NaN operand.
    std::string Lt = freshSlot("one_lt", Type::getInt1Ty(Mod->getContext()));
    emitExtern(B, compareSignature(Prefix, "LessThan"), {LHS, RHS}, Lt);
    std::string Gt = freshSlot("one_gt", Type::getInt1Ty(Mod->getContext()));
    emitExtern(B, compareSignature(Prefix, "GreaterThan"), {LHS, RHS}, Gt);
    Or(Lt, Gt);
    return;
  }
  case CmpInst::FCMP_UEQ: {
    // ueq = !(ogt || olt)
    std::string Lt = freshSlot("ueq_lt", Type::getInt1Ty(Mod->getContext()));
    emitExtern(B, compareSignature(Prefix, "LessThan"), {LHS, RHS}, Lt);
    std::string Gt = freshSlot("ueq_gt", Type::getInt1Ty(Mod->getContext()));
    emitExtern(B, compareSignature(Prefix, "GreaterThan"), {LHS, RHS}, Gt);
    std::string Any = freshSlot("ueq_any", Type::getInt1Ty(Mod->getContext()));
    emitExtern(B,
               "SystemBoolean.__op_LogicalOr__SystemBoolean_SystemBoolean__"
               "SystemBoolean",
               {Lt, Gt}, Any);
    Neg(Any);
    return;
  }
  case CmpInst::FCMP_UGT: {
    std::string Le = freshSlot("ugt_le", Type::getInt1Ty(Mod->getContext()));
    emitExtern(B, compareSignature(Prefix, "LessThanOrEqual"), {LHS, RHS}, Le);
    Neg(Le);
    return;
  }
  case CmpInst::FCMP_UGE: {
    std::string Lt = freshSlot("uge_lt", Type::getInt1Ty(Mod->getContext()));
    emitExtern(B, compareSignature(Prefix, "LessThan"), {LHS, RHS}, Lt);
    Neg(Lt);
    return;
  }
  case CmpInst::FCMP_ULT: {
    std::string Ge = freshSlot("ult_ge", Type::getInt1Ty(Mod->getContext()));
    emitExtern(B, compareSignature(Prefix, "GreaterThanOrEqual"), {LHS, RHS},
               Ge);
    Neg(Ge);
    return;
  }
  case CmpInst::FCMP_ULE: {
    std::string Gt = freshSlot("ule_gt", Type::getInt1Ty(Mod->getContext()));
    emitExtern(B, compareSignature(Prefix, "GreaterThan"), {LHS, RHS}, Gt);
    Neg(Gt);
    return;
  }
  case CmpInst::FCMP_ORD: {
    // ord = !(a isNaN || b isNaN)
    std::string NA = IsNaN(LHS);
    std::string NB = IsNaN(RHS);
    std::string Any = freshSlot("ord_any", Type::getInt1Ty(Mod->getContext()));
    emitExtern(B,
               "SystemBoolean.__op_LogicalOr__SystemBoolean_SystemBoolean__"
               "SystemBoolean",
               {NA, NB}, Any);
    Neg(Any);
    return;
  }
  case CmpInst::FCMP_UNO: {
    std::string NA = IsNaN(LHS);
    std::string NB = IsNaN(RHS);
    Or(NA, NB);
    return;
  }
  default:
    reportUDONError("unsupported fcmp predicate: " + I.getName());
  }
}

// --- floating-point to integer ---------------------------------------------

void UDONWriter::selectFPToInt(const Instruction &I, UFunction &UF,
                               unsigned &Idx) {
  // System.Convert rounds, and the saturating intrinsics must truncate, so the
  // clamping happens in the floating-point domain before any Convert: widen to
  // double, map NaN to zero, clamp, truncate, convert.
  //
  // Every split rebinds the block reference: `emitDiamond` appends blocks and
  // moves `Idx`, so a reference taken before it points at the block that now
  // ends in a branch and the remaining instructions would never run.
  Type *DstTy = I.getType();
  Type *SrcTy = I.getOperand(0)->getType();
  bool ToUnsigned = I.getOpcode() == Instruction::FPToUI;
  bool Wide = DstTy->isIntegerTy(64);
  std::string Src = slotFor(I.getOperand(0));
  Type *F64 = Type::getDoubleTy(Mod->getContext());

  // Widen f32 to f64 first, so every bound is a double and there is one clamp
  // path.
  std::string X = Src;
  if (SrcTy->isFloatTy()) {
    X = freshSlot("f2i_w", F64);
    emitExtern(UF.Blocks[Idx],
               "SystemConvert.__ToDouble__SystemSingle__SystemDouble", {Src},
               X);
  }

  auto DoubleConst = [&](double V) { return constantSlot(ConstantFP::get(F64, V)); };

  // NaN -> 0 first: Math.Max(NaN, 0) is NaN in .NET, so a clamp alone would
  // leave the NaN to reach Convert and throw.
  std::string NotNaN = freshSlot("f2i_nn", F64);
  std::string IsNaN = freshSlot("f2i_isnan", Type::getInt1Ty(Mod->getContext()));
  emitExtern(UF.Blocks[Idx], "SystemDouble.__IsNaN__SystemDouble__SystemBoolean",
             {X}, IsNaN);
  Idx = emitDiamond(UF, Idx, IsNaN, DoubleConst(0.0), X, NotNaN, "f2i_nan");

  // Clamp in double, then truncate.
  double Lo = 0.0;
  double Hi = 0.0;
  if (!ToUnsigned && !Wide) {
    Lo = -2147483648.0;
    Hi = 2147483647.0;
  } else if (!ToUnsigned && Wide) {
    // The largest double below 2^63, not 9223372036854775807.0: that literal
    // rounds to exactly 2^63 and Convert.ToInt64 would throw on it.
    Lo = -9223372036854775808.0;
    Hi = 9223372036854774784.0;
  } else if (ToUnsigned && !Wide) {
    Lo = 0.0;
    Hi = 4294967295.0;
  } else {
    // fptoui to i64. The clamp must stay inside Int64's range, because the
    // clamped value still goes through Convert.ToInt64; the >= 2^64 case is
    // handled by its own select below, before this value is used. 2^63 is the
    // largest exactly-representable bound that Convert.ToInt64 accepts.
    Lo = 0.0;
    Hi = 9223372036854774784.0;
  }

  std::string Low = freshSlot("f2i_lo", F64);
  emitExtern(UF.Blocks[Idx],
             "SystemMath.__Max__SystemDouble_SystemDouble__SystemDouble",
             {NotNaN, DoubleConst(Lo)}, Low);
  std::string High = freshSlot("f2i_hi", F64);
  emitExtern(UF.Blocks[Idx],
             "SystemMath.__Min__SystemDouble_SystemDouble__SystemDouble",
             {Low, DoubleConst(Hi)}, High);
  std::string Truncated = freshSlot("f2i_tr", F64);
  emitExtern(UF.Blocks[Idx],
             "SystemMath.__Truncate__SystemDouble__SystemDouble", {High},
             Truncated);

  if (!Wide) {
    if (ToUnsigned) {
      // Convert.ToInt64 then wrap: a UInt32 result has no legal i32 slot path.
      std::string AsI64 = freshSlot("f2i_u64", Type::getInt64Ty(Mod->getContext()));
      emitExtern(UF.Blocks[Idx],
                 "SystemConvert.__ToInt64__SystemDouble__SystemInt64",
                 {Truncated}, AsI64);
      emitCopy(UF.Blocks[Idx], emitTruncI64ToI32(AsI64, UF.Blocks[Idx]),
               slotFor(&I));
    } else {
      emitExtern(UF.Blocks[Idx],
                 "SystemConvert.__ToInt32__SystemDouble__SystemInt32",
                 {Truncated}, slotFor(&I));
    }
    return;
  }

  if (!ToUnsigned) {
    emitExtern(UF.Blocks[Idx],
               "SystemConvert.__ToInt64__SystemDouble__SystemInt64",
               {Truncated}, slotFor(&I));
    return;
  }

  // fptoui to i64, in this order: x >= 2^64 -> -1 (all ones); x >= 2^63 ->
  // Convert.ToInt64(x - 2^63) | Int64.MinValue; otherwise Convert.ToInt64(x).
  // Converting first and splitting after would send the whole [2^63, 2^64)
  // range to Convert.ToInt64, which throws.
  Type *I64 = Type::getInt64Ty(Mod->getContext());
  // Both tests use `High`, the CLAMPED value: the clamp already bounds it at
  // the largest double below 2^63, so `>= 2^63` can never hold and the
  // `>= 2^64` test must therefore be made against the ORIGINAL value, before
  // the clamp. That is why `NotNaN` is tested here rather than `Truncated`.
  std::string Ge64 = freshSlot("f2u_ge64", Type::getInt1Ty(Mod->getContext()));
  emitExtern(UF.Blocks[Idx],
             "SystemDouble.__op_GreaterThanOrEqual__SystemDouble_SystemDouble__"
             "SystemBoolean",
             {NotNaN, DoubleConst(18446744073709551616.0)}, Ge64);
  std::string Ge63 = freshSlot("f2u_ge63", Type::getInt1Ty(Mod->getContext()));
  emitExtern(UF.Blocks[Idx],
             "SystemDouble.__op_GreaterThanOrEqual__SystemDouble_SystemDouble__"
             "SystemBoolean",
             {NotNaN, DoubleConst(9223372036854775808.0)}, Ge63);

  // The high-range value: (x - 2^63) converted, then OR with the sign bit.
  std::string Shifted = freshSlot("f2u_sh", F64);
  emitExtern(UF.Blocks[Idx],
             "SystemDouble.__op_Subtraction__SystemDouble_SystemDouble__"
             "SystemDouble",
             {High, DoubleConst(9223372036854775808.0)}, Shifted);
  std::string ShiftedI = freshSlot("f2u_shi", I64);
  emitExtern(UF.Blocks[Idx],
             "SystemConvert.__ToInt64__SystemDouble__SystemInt64", {Shifted},
             ShiftedI);
  std::string Signed = freshSlot("f2u_sign", I64);
  emitExtern(UF.Blocks[Idx],
             "SystemInt64.__op_LogicalOr__SystemInt64_SystemInt64__SystemInt64",
             {ShiftedI, constantSlot(ConstantInt::get(I64, 0x8000000000000000ull))},
             Signed);

  // The low-range value.
  std::string Plain = freshSlot("f2u_plain", I64);
  emitExtern(UF.Blocks[Idx],
             "SystemConvert.__ToInt64__SystemDouble__SystemInt64", {Truncated},
             Plain);

  std::string HighOrLow = freshSlot("f2u_hl", I64);
  Idx = emitDiamond(UF, Idx, Ge63, Signed, Plain, HighOrLow, "f2u_hi");

  std::string AllOnes =
      constantSlot(ConstantInt::get(I64, 0xFFFFFFFFFFFFFFFFull));
  Idx = emitDiamond(UF, Idx, Ge64, AllOnes, HighOrLow, slotFor(&I), "f2u_top");
}

void UDONWriter::selectUDivRemI64(const Instruction &I, UFunction &UF,
                                  unsigned &Idx) {
  // Unsigned i64 division without SystemUInt64: i64 values live in
  // `%SystemInt64` slots and argument typing is strict, so a reinterpret would
  // cost a GetBytes allocation every time.
  //
  // Hacker's Delight 9-3. The special case is a DIVISOR with its top bit set,
  // not a dividend: if b is negative as a signed value it is >= 2^63 unsigned,
  // so the quotient is 1 when a >=u b and 0 otherwise.
  //
  // Every split rebinds the block reference, because `emitDiamond` appends
  // blocks and moves `Idx`.
  Type *I64 = Type::getInt64Ty(Mod->getContext());
  std::string A = slotFor(I.getOperand(0));
  std::string Bv = slotFor(I.getOperand(1));
  std::string SignBit =
      constantSlot(ConstantInt::get(I64, 0x8000000000000000ull));
  std::string Zero = constantSlot(ConstantInt::get(I64, 0));
  std::string One = constantSlot(ConstantInt::get(I64, 1));
  bool WantQuotient = I.getOpcode() == Instruction::UDiv;

  auto UGe = [&](const std::string &X, const std::string &Y) {
    UBlock &CurB = UF.Blocks[Idx];
    std::string Xs = freshSlot("ucmp_x", I64);
    emitExtern(CurB,
               "SystemInt64.__op_LogicalXor__SystemInt64_SystemInt64__SystemInt64",
               {X, SignBit}, Xs);
    std::string Ys = freshSlot("ucmp_y", I64);
    emitExtern(CurB,
               "SystemInt64.__op_LogicalXor__SystemInt64_SystemInt64__SystemInt64",
               {Y, SignBit}, Ys);
    std::string R = freshSlot("ucmp_r", Type::getInt1Ty(Mod->getContext()));
    emitExtern(CurB,
               "SystemInt64.__op_GreaterThanOrEqual__SystemInt64_SystemInt64__"
               "SystemBoolean",
               {Xs, Ys}, R);
    return R;
  };

  // The general path, computed in the current block.
  std::string HalfA = freshSlot("udiv_t", I64);
  emitExtern(UF.Blocks[Idx],
             "SystemInt64.__op_RightShift__SystemInt64_SystemInt64__SystemInt64",
             {A, One}, HalfA);
  // `a >>> 1` of a signed value needs the sign bit cleared: ashr by 1 leaves it
  // set, so mask it off.
  std::string HalfMasked = freshSlot("udiv_tm", I64);
  emitExtern(UF.Blocks[Idx],
             "SystemInt64.__op_LogicalAnd__SystemInt64_SystemInt64__SystemInt64",
             {HalfA, constantSlot(ConstantInt::get(I64, 0x7FFFFFFFFFFFFFFFull))},
             HalfMasked);
  std::string HalfQ = freshSlot("udiv_hq2", I64);
  emitExtern(UF.Blocks[Idx],
             "SystemInt64.__op_Division__SystemInt64_SystemInt64__SystemInt64",
             {HalfMasked, Bv}, HalfQ);
  std::string Doubled = freshSlot("udiv_q2", I64);
  emitExtern(UF.Blocks[Idx],
             "SystemInt64.__op_LeftShift__SystemInt64_SystemInt64__SystemInt64",
             {HalfQ, One}, Doubled);
  std::string Prod = freshSlot("udiv_p", I64);
  emitExtern(
      UF.Blocks[Idx],
      "SystemInt64.__op_Multiplication__SystemInt64_SystemInt64__SystemInt64",
      {Doubled, Bv}, Prod);
  std::string Rem = freshSlot("udiv_r", I64);
  emitExtern(UF.Blocks[Idx],
             "SystemInt64.__op_Subtraction__SystemInt64_SystemInt64__SystemInt64",
             {A, Prod}, Rem);

  // The correction test is `r >=u b`, not "r is negative": that is the i32
  // shape and it is wrong here.
  std::string RemCmp = UGe(Rem, Bv);
  std::string QPlus = freshSlot("udiv_qp", I64);
  emitExtern(UF.Blocks[Idx],
             "SystemInt64.__op_Addition__SystemInt64_SystemInt64__SystemInt64",
             {Doubled, One}, QPlus);
  std::string QFinal = freshSlot("udiv_qf", I64);
  Idx = emitDiamond(UF, Idx, RemCmp, QPlus, Doubled, QFinal, "udiv_corr");

  std::string RMinus = freshSlot("urem_rm", I64);
  emitExtern(UF.Blocks[Idx],
             "SystemInt64.__op_Subtraction__SystemInt64_SystemInt64__SystemInt64",
             {Rem, Bv}, RMinus);
  std::string RFinal = freshSlot("urem_rf", I64);
  Idx = emitDiamond(UF, Idx, RemCmp, RMinus, Rem, RFinal, "urem_corr");

  // The divisor-high-bit path: q = (a >=u b) ? 1 : 0, r = a - q*b.
  std::string HighCmp = UGe(A, Bv);
  std::string HighQ = freshSlot("udiv_hq", I64);
  Idx = emitDiamond(UF, Idx, HighCmp, One, Zero, HighQ, "udiv_hi");

  std::string HighProd = freshSlot("udiv_hp", I64);
  emitExtern(
      UF.Blocks[Idx],
      "SystemInt64.__op_Multiplication__SystemInt64_SystemInt64__SystemInt64",
      {HighQ, Bv}, HighProd);
  std::string HighRem = freshSlot("udiv_hr", I64);
  emitExtern(UF.Blocks[Idx],
             "SystemInt64.__op_Subtraction__SystemInt64_SystemInt64__SystemInt64",
             {A, HighProd}, HighRem);

  std::string DivHigh = freshSlot("udiv_dh", Type::getInt1Ty(Mod->getContext()));
  emitExtern(UF.Blocks[Idx],
             "SystemInt64.__op_LessThan__SystemInt64_SystemInt64__SystemBoolean",
             {Bv, Zero}, DivHigh);
  std::string Value = WantQuotient ? QFinal : RFinal;
  std::string HighValue = WantQuotient ? HighQ : HighRem;
  Idx = emitDiamond(UF, Idx, DivHigh, HighValue, Value, slotFor(&I), "udiv_sel");
}

// --- vectors ---------------------------------------------------------------

void UDONWriter::selectVectorOp(const Instruction &I, UFunction &UF,
                                unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  auto VecTypeOf = [](Type *Ty) -> std::string {
    auto *VT = dyn_cast<FixedVectorType>(Ty);
    if (!VT || !VT->getElementType()->isFloatTy())
      return {};
    switch (VT->getNumElements()) {
    case 2: return "UnityEngineVector2";
    case 3: return "UnityEngineVector3";
    case 4: return "UnityEngineVector4";
    }
    return {};
  };

  if (I.getOpcode() == Instruction::ExtractElement) {
    const auto &EE = cast<ExtractElementInst>(I);
    std::string Vec = slotFor(EE.getVectorOperand());
    std::string VecTy = VecTypeOf(EE.getVectorOperand()->getType());
    auto *CI = dyn_cast<ConstantInt>(EE.getIndexOperand());
    if (!CI || VecTy.empty())
      reportUDONError("unsupported extractelement: " + I.getName());
    unsigned Index = static_cast<unsigned>(CI->getZExtValue());
    // The component accessors are the only component-wise reads Udon offers.
    static const char *const Names3[] = {"x", "y", "z"};
    static const char *const Names4[] = {"x", "y", "z", "w"};
    const char *Component = VecTy == "UnityEngineVector2"
                                ? Names3[Index]
                                : (VecTy == "UnityEngineVector3" ? Names3[Index]
                                                                 : Names4[Index]);
    emitExtern(B, VecTy + ".__get_" + Component + "__SystemSingle", {Vec},
               slotFor(&I));
    return;
  }

  if (I.getOpcode() == Instruction::InsertElement) {
    // Building a vector component by component: read the existing components,
    // replace one, and reconstruct through the constructor. A `poison` base
    // contributes zeros.
    const auto &IE = cast<InsertElementInst>(I);
    std::string VecTy = VecTypeOf(IE.getType());
    auto *CI = dyn_cast<ConstantInt>(IE.getOperand(2));
    if (!CI || VecTy.empty())
      reportUDONError("unsupported insertelement: " + I.getName());
    unsigned Index = static_cast<unsigned>(CI->getZExtValue());
    unsigned Count = VecTy == "UnityEngineVector2"
                         ? 2
                         : (VecTy == "UnityEngineVector3" ? 3 : 4);
    static const char *const Names4[] = {"x", "y", "z", "w"};

    std::vector<std::string> Parts;
    Parts.reserve(Count);
    for (unsigned C = 0; C < Count; C++) {
      if (C == Index) {
        Parts.push_back(slotFor(IE.getOperand(1)));
        continue;
      }
      const Value *Base = IE.getOperand(0);
      if (isa<PoisonValue>(Base) || isa<UndefValue>(Base)) {
        Parts.push_back(constantSlot(ConstantFP::get(
            Type::getFloatTy(Mod->getContext()), 0.0)));
        continue;
      }
      std::string Part = freshSlot("vec_c", Type::getFloatTy(Mod->getContext()));
      emitExtern(B, VecTy + ".__get_" + Names4[C] + "__SystemSingle",
                 {slotFor(Base)}, Part);
      Parts.push_back(Part);
    }
    std::string Sig = VecTy + ".__ctor__";
    for (unsigned C = 0; C < Count; C++)
      Sig += (C ? "_" : "") + std::string("SystemSingle");
    Sig += "__" + VecTy;
    emitExtern(B, Sig, Parts, slotFor(&I));
    return;
  }

  reportUDONError("unsupported vector operation: " + Twine(I.getOpcodeName()));
}

void UDONWriter::selectVectorArith(const Instruction &I, UFunction &UF,
                                   unsigned &Idx) {
  // A component-wise vector multiply or divide: read each component of both
  // operands, combine with the scalar operator, and reconstruct. Vector3 has no
  // component-wise multiply, so this is the only correct lowering.
  UBlock &B = UF.Blocks[Idx];
  auto *VT = cast<FixedVectorType>(I.getType());
  std::string VecTy = udonTypeOf(VT);
  unsigned Count = VT->getNumElements();
  static const char *const Names4[] = {"x", "y", "z", "w"};
  const char *Op = I.getOpcode() == Instruction::FMul ? "Multiplication"
                                                      : "Division";

  std::vector<std::string> Parts;
  for (unsigned C = 0; C < Count; C++) {
    std::string CA = freshSlot("vec_a", Type::getFloatTy(Mod->getContext()));
    emitExtern(B, VecTy + ".__get_" + Names4[C] + "__SystemSingle",
               {slotFor(I.getOperand(0))}, CA);
    std::string CB = freshSlot("vec_b", Type::getFloatTy(Mod->getContext()));
    emitExtern(B, VecTy + ".__get_" + Names4[C] + "__SystemSingle",
               {slotFor(I.getOperand(1))}, CB);
    std::string R = freshSlot("vec_r", Type::getFloatTy(Mod->getContext()));
    emitExtern(B, std::string("SystemSingle.__op_") + Op +
                      "__SystemSingle_SystemSingle__SystemSingle",
               {CA, CB}, R);
    Parts.push_back(R);
  }
  std::string Sig = VecTy + ".__ctor__";
  for (unsigned C = 0; C < Count; C++)
    Sig += (C ? "_" : "") + std::string("SystemSingle");
  Sig += "__" + VecTy;
  emitExtern(B, Sig, Parts, slotFor(&I));
}
