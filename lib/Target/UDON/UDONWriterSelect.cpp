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

unsigned UDONWriter::appendBlock(UFunction &UF, const std::string &Label) {
  // Labels must be unique within the whole program, not just the function: the
  // assembler rejects a repeat, and a duplicate is an AliasedSymbolException.
  // A suffix is cheaper than proving every naming site is disjoint.
  StringSet<> Existing;
  for (const UFunction &Other : Functions) {
    // `Cur` points at the function being selected, which is not yet in
    // `Functions`, so its own blocks must be scanned separately below.
    if (&Other == Cur)
      continue;
    for (const UBlock &B : Other.Blocks)
      Existing.insert(B.Label);
  }
  for (const UBlock &B : UF.Blocks)
    Existing.insert(B.Label);

  std::string Unique = Label;
  for (unsigned N = 1; Existing.count(Unique); N++)
    Unique = Label + "_" + std::to_string(N);

  UBlock B;
  B.Label = Unique;
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
  // A dedicated counter, not the temporary one: two diamonds with the same
  // hint must not collide even when the caller reuses a temporary name.
  // The block count is the uniqueness source: it cannot be stale or reset,
  // because `appendBlock` is what increments it.
  std::string Base = mangle(Cur->F->getName()) + "_" + Hint + "_" +
                     std::to_string(UF.Blocks.size()) + "_" +
                     std::to_string(BlockCounter++);
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
  UFunction UF;
  UF.F = &F;
  UF.BodyLabel = mangle(F.getName()) + "_body";
  Cur = &UF;
  Slots.clear();
  TempCounter = 0;

  declareSlot(returnSlot(&F), "SystemUInt32");
  NullSlots.push_back({returnSlot(&F), "SystemUInt32"});
  if (!F.getReturnType()->isVoidTy()) {
    declareSlot(returnValueSlot(&F), udonTypeOf(F.getReturnType()));
    NullSlots.push_back({returnValueSlot(&F), udonTypeOf(F.getReturnType())});
  }

  // Argument slots are part of the calling convention: a direct call copies its
  // arguments here before jumping to the body.
  for (unsigned A = 0; A < F.arg_size(); A++) {
    std::string Slot = mangle(F.getName()) + "_arg_" + std::to_string(A);
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

  Functions.push_back(std::move(UF));
  Cur = &Functions.back();
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
    emitExtern(B, Ty + ".__op_UnaryNegation__" + Ty + "__" + Ty,
               {slotFor(I.getOperand(0))}, slotFor(&I));
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
    std::string Src = SrcTy->isIntegerTy(64) ? "SystemInt64" : "SystemInt32";
    std::string Dst = DstTy->isDoubleTy() ? "SystemDouble" : "SystemSingle";
    emitExtern(B, "SystemConvert.__To" + Dst + "__" + Src + "__" + Dst,
               {slotFor(I.getOperand(0))}, slotFor(&I));
    return;
  }
  case Instruction::UIToFP: {
    // Zero-extend to i64 first, then convert: an i32 above Int32.MaxValue is
    // negative in its slot, so a direct Convert would produce a negative float.
    Type *DstTy = I.getType();
    Type *SrcTy = I.getOperand(0)->getType();
    std::string Dst = DstTy->isDoubleTy() ? "SystemDouble" : "SystemSingle";
    std::string Wide = slotFor(I.getOperand(0));
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
    emitExtern(B, "SystemConvert.__To" + Dst + "__SystemInt64__" + Dst, {Wide},
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
  auto It = UF.EdgeIndex.find(Succ);
  if (It != UF.EdgeIndex.end())
    return UF.Blocks[It->second].Label;

  unsigned Idx = appendBlock(UF, Label);
  emitPhiCopies(Pred, Succ, UF.Blocks[Idx]);
  UF.Blocks[Idx].Insts.push_back({UInst::JUMP, blockLabel(Succ)});
  UF.EdgeIndex[Succ] = Idx;
  return Label;
}

// --- compares and casts ----------------------------------------------------

void UDONWriter::selectICmp(const ICmpInst &I, UFunction &UF, unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  CmpInst::Predicate P = I.getPredicate();
  Type *OpTy = I.getOperand(0)->getType();
  std::string Ty = operandType(OpTy);
  if (Ty.empty())
    reportUDONError("compare on an unsupported type: " + I.getName());

  std::string LHS = slotFor(I.getOperand(0));
  std::string RHS = slotFor(I.getOperand(1));

  // Unsigned compares XOR both sides with the sign bit and use the signed
  // predicate: Udon has no unsigned operators on these slot types, and the XOR
  // costs one extern with no allocation. Verified: -1 < 1 unsigned is false.
  if (isUnsignedPredicate(P)) {
    unsigned Bits = OpTy->getIntegerBitWidth();
    uint64_t MaskBits = Bits == 64 ? 0x8000000000000000ull : 0x80000000ull;
    std::string Mask = constantSlot(ConstantInt::get(OpTy, MaskBits));
    std::string X = freshSlot("uxor_l", OpTy);
    emitExtern(B, binarySignature(Ty, "LogicalXor"), {LHS, Mask}, X);
    std::string Y = freshSlot("uxor_r", OpTy);
    emitExtern(B, binarySignature(Ty, "LogicalXor"), {RHS, Mask}, Y);
    emitExtern(B,
               compareSignature(Ty,
                                icmpPredicateName(signedEquivalentOfUnsigned(P))),
               {X, Y}, slotFor(&I));
    return;
  }

  StringRef Op = icmpPredicateName(P);
  if (Op.empty())
    reportUDONError("unsupported icmp predicate: " + I.getName());
  emitExtern(B, compareSignature(Ty, Op), {LHS, RHS}, slotFor(&I));
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
  if (SrcTy->isIntegerTy(1) && DstTy->isIntegerTy(32)) {
    emitExtern(B, "SystemConvert.__ToInt32__SystemBoolean__SystemInt32",
               {slotFor(I.getOperand(0))}, slotFor(&I));
    return;
  }
  if ((SrcTy->isIntegerTy(8) || SrcTy->isIntegerTy(16)) &&
      DstTy->isIntegerTy(32)) {
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
  reportUDONError("unsupported zext: " + I.getName());
}

void UDONWriter::selectSExt(const Instruction &I, UFunction &UF,
                            unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  Type *DstTy = I.getType();
  Type *SrcTy = I.getOperand(0)->getType();
  if (SrcTy->isIntegerTy(1)) {
    // `true` sign-extends to -1, not 1: 0 - Convert.ToBoolean(x). Rust emits
    // sext i1 for mask idioms, so the all-ones expectation is load-bearing.
    if (DstTy->isIntegerTy(32)) {
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
  if ((SrcTy->isIntegerTy(8) || SrcTy->isIntegerTy(16)) &&
      DstTy->isIntegerTy(32)) {
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

  // i32<->f32 and i64<->f64 go through the reserved scratch bytes at offset 0:
  // a store of the source and a load of the destination type. The
  // SystemBitConverter reinterpret externs are registered but were never
  // exercised, so the memory round-trip is the verified path.
  std::string Scratch = i32Constant(0);
  emitStore(B, slotFor(I.getOperand(0)), Scratch, SrcTy);
  emitCopy(B, emitLoad(B, Scratch, DstTy), slotFor(&I));
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
    uint64_t SignMask = Bits == 64 ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFFull;
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
  std::string SignMask = constantSlot(
      ConstantInt::get(Ty, Bits == 64 ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFFull));
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
