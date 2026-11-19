//===-- UDONWriterCalls.cpp - calls and function pointers ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The calling convention: a direct call copies its arguments into the callee's
// private argument slots, copies its return label address into the callee's
// return slot, and jumps to the body. The return label is a block of the
// caller, so control resumes there.
//
//===----------------------------------------------------------------------===//

#include "UDONWriter.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {
inline std::string mangle(StringRef Name) { return mangleName(Name); }
} // namespace

namespace {

/// The only source-level way to re-enter the VM in this milestone.
///
/// The name deliberately avoids the `llvm.` prefix: LLVM reserves it for
/// intrinsics, and an unknown `llvm.*` name can be rejected by the verifier or
/// mis-handled by optimization passes. An ordinary `__udon_`-prefixed
/// declaration is matched by exact name here instead.
constexpr StringLiteral SendCustomEventName("__udon_send_custom_event");

/// The real registry signature for the re-entry extern. There is no
/// `SystemVoid.SendCustomEvent`.
constexpr StringLiteral SendCustomEventSignature(
    "VRCUdonCommonInterfacesIUdonEventReceiver.__SendCustomEvent__SystemString__"
    "SystemVoid");

} // namespace

void UDONWriter::collectFunctionPointers() {
  // An address-taken function gets an index into `__fn_table`. Index 0 is the
  // null entry, so a zeroed pointer reaches a halt that names itself.
  uint32_t Next = 1;
  for (Function &F : *Mod) {
    if (F.isDeclaration())
      continue;
    if (F.hasAddressTaken())
      FunctionPointerIndices[&F] = Next++;
  }
}

unsigned UDONWriter::selectDirectCall(unsigned Idx, const Function *Callee,
                                      ArrayRef<std::string> Args,
                                      const Value *Result,
                                      const Instruction *At) {
  // A call into a function that may be re-entered needs the caller's live slots
  // spilled first: the nested activation runs the same code and writes the same
  // slots, so without this the caller resumes with the callee's values.
  std::vector<std::pair<std::string, std::string>> Live;
  if (At && RecursiveFunctions.count(Callee))
    Live = liveSlotsAt(*At);
  for (const auto &[Slot, Ty] : Live)
    Idx = emitSpill(Idx, Slot, Ty);
  UBlock &B = Cur->Blocks[Idx];

  // Arguments into the callee's own slots, this call's return label into the
  // callee's return slot, then a jump to the body. Copying in the callee is
  // what keeps a nested or sibling call correct: the callee's state lives in
  // slots no other function writes.
  // A variadic callee declares fewer parameters than a call passes. The extra
  // arguments still need a home, because the copy below writes one slot per
  // argument; the callee simply never reads them. Declaring on demand is what
  // makes a variadic call (sprintf, printf) lower at all -- and a stub that
  // ignores its variadic tail needs nothing further.
  for (unsigned A = 0; A < Args.size(); A++) {
    std::string Slot =
        mangle(Callee->getName()) + "_arg_" + std::to_string(A);
    if (!SlotTypes.count(Slot)) {
      auto It = SlotTypes.find(Args[A]);
      std::string Ty = It == SlotTypes.end() ? std::string() : It->second;
      if (Ty.empty())
        reportUDONError("cannot type argument " + std::to_string(A) +
                        " of a call to " + Callee->getName());
      declareSlot(Slot, Ty);
      NullSlots.push_back({Slot, Ty});
    }
    emitCopy(B, Args[A], Slot);
  }

  std::string RetLabel =
      mangle(Cur->F->getName()) + "_ret_" + std::to_string(TempCounter++);
  emitCopy(B, addressSlotForLabel("__addr_" + RetLabel, RetLabel),
           returnSlot(Callee));
  B.Insts.push_back({UInst::JUMP, mangle(Callee->getName()) + "_body"});

  // The return label is a block of its own, and the caller's remaining
  // instructions go into a continuation block after it: a block cannot fall
  // through to two places, and the return label is reached by a jump.
  std::string ContLabel =
      mangle(Cur->F->getName()) + "_cont_" + std::to_string(TempCounter++);
  unsigned RetIdx = appendBlock(*Cur, RetLabel);
  UBlock &Ret = Cur->Blocks[RetIdx];
  Ret.Insts.push_back({UInst::NOP, {}});
  if (Result)
    emitCopy(Ret, returnValueSlot(Callee), slotFor(Result));
  // The result must be read before the restore, because the restore rewrites
  // the caller's slots -- and the return slot is one of them.
  // Restore in reverse order, which is what makes the stacks behave as a stack.
  for (auto It = Live.rbegin(); It != Live.rend(); ++It)
    emitUnspill(Ret, It->first, It->second);
  Ret.Insts.push_back({UInst::JUMP, ContLabel});
  return appendBlock(*Cur, ContLabel);
}

void UDONWriter::selectCall(const CallInst &I, UFunction &UF, unsigned &Idx) {
  const Function *Callee = I.getCalledFunction();

  if (!Callee) {
    // An indirect call: the callee value is an index into `__fn_table`. The
    // arguments and the return address go through the shared transfer slots,
    // and every address-taken function copies them into its own slots on
    // entry, which is what keeps a nested indirect call correct.
    std::string FpSlot = slotFor(I.getCalledOperand());
    for (unsigned A = 0; A < I.arg_size(); A++) {
      std::string Slot = "__icall_arg_" + std::to_string(A);
      std::string Ty = udonTypeOf(I.getArgOperand(A)->getType());
      if (!SlotTypes.count(Slot)) {
        declareSlot(Slot, Ty);
        NullSlots.push_back({Slot, Ty});
      }
      emitCopy(UF.Blocks[Idx], slotFor(I.getArgOperand(A)), Slot);
    }
    std::string RetLabel =
        mangle(I.getFunction()->getName()) + "_icall_ret_" +
        std::to_string(TempCounter++);
    emitCopy(UF.Blocks[Idx],
             addressSlotForLabel("__addr_" + RetLabel, RetLabel), "__icall_ret");

    // `__fn_table[index]` -> the code address, then jump through it.
    std::string Target =
        freshSlot("fnaddr", Type::getInt32Ty(Mod->getContext()));
    emitExtern(UF.Blocks[Idx],
               "SystemUInt32Array.__Get__SystemInt32__SystemUInt32",
               {"__fn_table", FpSlot}, Target);
    std::string Code =
        freshSlot("fntarget", Type::getInt32Ty(Mod->getContext()));
    emitCopy(UF.Blocks[Idx], Target, Code);
    UF.Blocks[Idx].Insts.push_back({UInst::JUMP_INDIRECT, Code});

    unsigned RetIdx = appendBlock(UF, RetLabel);
    UBlock &Ret = UF.Blocks[RetIdx];
    Ret.Insts.push_back({UInst::NOP, {}});
    if (!I.getType()->isVoidTy())
      emitCopy(Ret, "__icall_retval", slotFor(&I));
    std::string ContLabel =
        mangle(I.getFunction()->getName()) + "_icall_cont_" +
        std::to_string(TempCounter++);
    Ret.Insts.push_back({UInst::JUMP, ContLabel});
    Idx = appendBlock(UF, ContLabel);
    return;
  }

  if (Callee->getName() == SendCustomEventName) {
    // The argument is a pointer to a NUL-terminated constant string naming the
    // entry point.
    const Value *Arg = I.getArgOperand(0);
    const auto *GV = dyn_cast<GlobalVariable>(Arg->stripPointerCasts());
    if (!GV || !GV->hasInitializer())
      reportUDONError("__udon_send_custom_event needs a constant string: " +
                      I.getFunction()->getName());
    auto *CDA = dyn_cast<ConstantDataArray>(GV->getInitializer());
    if (!CDA || !CDA->getElementType()->isIntegerTy(8))
      reportUDONError(
          "__udon_send_custom_event needs a constant byte string: " +
          I.getFunction()->getName());
    StringRef Bytes = CDA->getRawDataValues();
    if (Bytes.empty() || Bytes.back() != '\0')
      reportUDONError(
          "__udon_send_custom_event string is not NUL-terminated: " +
          I.getFunction()->getName());

    std::string Name(Bytes.drop_back());
    std::string Key = "str:" + Name;
    std::string Slot;
    auto It = ConstantSlots.find(Key);
    if (It != ConstantSlots.end()) {
      Slot = It->second;
    } else {
      Slot = "__const_str_" + std::to_string(ConstantSlots.size());
      declareSlot(Slot, "SystemString");
      DataLines.push_back("    " + Slot + ": %SystemString, \"" + Name + "\"");
      ConstantSlots[Key] = Slot;
    }

    // The instance argument is the behaviour's own reference, which is the form
    // UdonSharp emits for `this`.
    if (!SlotTypes.count("__this")) {
      declareSlot("__this", "VRCUdonCommonInterfacesIUdonEventReceiver");
      DataLines.push_back(
          "    __this: %VRCUdonCommonInterfacesIUdonEventReceiver, this");
    }
    emitExtern(UF.Blocks[Idx], SendCustomEventSignature, {"__this", Slot}, "");
    return;
  }

  if (Callee->isDeclaration()) {
    // `sqrt` and `sqrtf` are the host's square root: Udon exposes exactly one,
    // the double one. Re-deriving a root in C would be slower and less
    // accurate, so both names lower straight to it. `sqrtf` narrows back.
    StringRef Name = Callee->getName();
    if (Name == "sqrt" || Name == "sqrtf") {
      LLVMContext &C = Mod->getContext();
      std::string X = slotFor(I.getArgOperand(0));
      bool FromFloat = I.getArgOperand(0)->getType()->isFloatTy();
      std::string D = X;
      if (FromFloat) {
        D = freshSlot("sqrt_d", Type::getDoubleTy(C));
        emitExtern(UF.Blocks[Idx],
                   "SystemConvert.__ToDouble__SystemSingle__SystemDouble", {X},
                   D);
      }
      std::string R = freshSlot("sqrt_r", Type::getDoubleTy(C));
      emitExtern(UF.Blocks[Idx],
                 "SystemMath.__Sqrt__SystemDouble__SystemDouble", {D}, R);
      if (FromFloat) {
        std::string F = freshSlot("sqrt_f", Type::getFloatTy(C));
        emitExtern(UF.Blocks[Idx],
                   "SystemConvert.__ToSingle__SystemDouble__SystemSingle", {R},
                   F);
        emitCopy(UF.Blocks[Idx], F, slotFor(&I));
      } else {
        emitCopy(UF.Blocks[Idx], R, slotFor(&I));
      }
      return;
    }
    reportUDONError("call to an undefined function: " + Callee->getName());
  }

  std::vector<std::string> Args;
  Args.reserve(I.arg_size());
  for (unsigned A = 0; A < I.arg_size(); A++)
    Args.push_back(slotFor(I.getArgOperand(A)));

  Idx = selectDirectCall(Idx, Callee, Args,
                         I.getType()->isVoidTy() ? nullptr : &I, &I);
}

// --- caller-save -----------------------------------------------------------

std::string UDONWriter::spillArrayType(StringRef UdonType) const {
  if (UdonType == "SystemInt64") return "SystemInt64Array";
  if (UdonType == "SystemUInt64") return "SystemUInt64Array";
  if (UdonType == "SystemSingle") return "SystemSingleArray";
  if (UdonType == "SystemDouble") return "SystemDoubleArray";
  if (UdonType == "UnityEngineVector2" || UdonType == "UnityEngineVector3" ||
      UdonType == "UnityEngineVector4" || UdonType == "UnityEngineQuaternion" ||
      UdonType == "UnityEngineColor")
    return "SystemObjectArray";
  // Every integer width and every pointer lives in an Int32 slot, so it spills
  // to the Int32 stack. A `SystemUInt32` slot is the one exception: it is its
  // own Udon type, so it needs the UInt32 array.
  if (UdonType == "SystemUInt32") return "SystemUInt32Array";
  return "SystemInt32Array";
}

std::string UDONWriter::spillStackFor(StringRef UdonType) const {
  if (UdonType == "SystemInt64" || UdonType == "SystemUInt64")
    return "__stack_i64";
  if (UdonType == "SystemSingle") return "__stack_f32";
  if (UdonType == "SystemDouble") return "__stack_f64";
  if (UdonType == "SystemBoolean") return "__stack_i32";
  if (UdonType == "UnityEngineVector2" || UdonType == "UnityEngineVector3" ||
      UdonType == "UnityEngineVector4" || UdonType == "UnityEngineQuaternion" ||
      UdonType == "UnityEngineColor")
    return "__stack_obj";
  // Every integer width and every pointer lives in an Int32 slot.
  return "__stack_i32";
}

std::string UDONWriter::spillCursorFor(StringRef UdonType) const {
  if (UdonType == "SystemInt64" || UdonType == "SystemUInt64")
    return "__sptr_i64";
  if (UdonType == "SystemSingle") return "__sptr_f32";
  if (UdonType == "SystemDouble") return "__sptr_f64";
  if (UdonType == "UnityEngineVector2" || UdonType == "UnityEngineVector3" ||
      UdonType == "UnityEngineVector4" || UdonType == "UnityEngineQuaternion" ||
      UdonType == "UnityEngineColor")
    return "__sptr_obj";
  return "__sptr_i32";
}

unsigned UDONWriter::emitSpill(unsigned BlockIdx, const std::string &Slot,
                               StringRef UdonType) {
  UBlock &B = Cur->Blocks[BlockIdx];
  // The bounds test comes BEFORE the write, so a push that trips the check
  // leaves the cursor unchanged and the caller's pops stay balanced.
  std::string Stack = spillStackFor(UdonType);
  std::string Cursor = spillCursorFor(UdonType);
  std::string Ok = freshSlot("spill_ok", Type::getInt1Ty(Mod->getContext()));
  emitExtern(B,
             "SystemInt32.__op_LessThan__SystemInt32_SystemInt32__SystemBoolean",
             {Cursor, i32Constant(static_cast<int32_t>(Opts.StackDepth))}, Ok);

  // On overflow: name the reason and halt. Written inline rather than through a
  // shared block because the check is a compare and a branch.
  std::string Base = mangle(Cur->F->getName()) + "_spill_" +
                     std::to_string(TempCounter++);
  std::string Overflow = Base + "_of";
  std::string Done = Base + "_ok";

  B.Insts.push_back({UInst::PUSH, Ok});
  B.Insts.push_back({UInst::JUMP_IF_FALSE, Overflow});
  // The stack is a typed array, but a `SystemBoolean` has no `SystemBooleanArray`
  // in the whitelist, so it spills on the Int32 stack. That means the value
  // must actually BE an Int32 at the `__Set__` call: convert the boolean slot
  // into an Int32 slot, and convert back on the way out.
  std::string ArrayType = spillArrayType(UdonType);
  std::string Stored = Slot;
  if (UdonType == "SystemBoolean") {
    Stored = freshSlot("spill_b", Type::getInt32Ty(Mod->getContext()));
    emitExtern(B, "SystemConvert.__ToInt32__SystemBoolean__SystemInt32",
               {Slot}, Stored);
  }
  emitExtern(B, ArrayType + ".__Set__SystemInt32_" +
                    (UdonType == "SystemBoolean" ? std::string("SystemInt32")
                                                 : UdonType.str()) +
                    "__SystemVoid",
             {Stack, Cursor, Stored}, "");
  emitExtern(B,
             "SystemInt32.__op_Addition__SystemInt32_SystemInt32__SystemInt32",
             {Cursor, i32Constant(1)}, Cursor);
  B.Insts.push_back({UInst::JUMP, Done});

  unsigned OfIdx = appendBlock(*Cur, Overflow);
  UBlock &Of = Cur->Blocks[OfIdx];
  emitCopy(Of, i32Constant(1), "__halt_reason");
  Of.Insts.push_back({UInst::JUMP, "__halt"});

  // The caller continues in a block of its own: the check above ends in a
  // branch, so appending here would put the next spill after a jump.
  return appendBlock(*Cur, Done);
}

void UDONWriter::emitUnspill(UBlock &B, const std::string &Slot,
                             StringRef UdonType) {
  std::string Stack = spillStackFor(UdonType);
  std::string Cursor = spillCursorFor(UdonType);
  emitExtern(B,
             "SystemInt32.__op_Subtraction__SystemInt32_SystemInt32__SystemInt32",
             {Cursor, i32Constant(1)}, Cursor);
  std::string ArrayType = spillArrayType(UdonType);
  if (UdonType == "SystemBoolean") {
    // Recover the Int32 the spill stored, then convert it back to a Boolean
    // slot; the `__Get__` result cannot land in a Boolean-typed slot.
    std::string AsInt = freshSlot("unspill_b", Type::getInt32Ty(Mod->getContext()));
    emitExtern(B, "SystemInt32Array.__Get__SystemInt32__SystemInt32",
               {Stack, Cursor}, AsInt);
    emitExtern(B, "SystemConvert.__ToBoolean__SystemInt32__SystemBoolean",
               {AsInt}, Slot);
    return;
  }
  emitExtern(B, ArrayType + ".__Get__SystemInt32__" + UdonType.str(),
             {Stack, Cursor}, Slot);
}

std::vector<std::pair<std::string, std::string>>
UDONWriter::liveSlotsAt(const Instruction &I) const {
  // The caller's own slots that a nested activation would clobber: the
  // function's return slot, its argument slots, and every value of this
  // function that is read after the call. A value defined after the call cannot
  // be live across it.
  //
  // Spilling a slot that did not need it costs one push and one pop; missing
  // one corrupts the caller, so the set is deliberately conservative about
  // *which* instructions count, not about how many.
  std::vector<std::pair<std::string, std::string>> Live;
  const Function *F = I.getFunction();
  StringSet<> Seen;

  auto Add = [&](const std::string &Slot, StringRef Ty) {
    if (Slot.empty() || Seen.count(Slot))
      return;
    Seen.insert(Slot);
    Live.push_back({Slot, Ty.str()});
  };

  Add(returnSlot(F), "SystemUInt32");
  for (unsigned A = 0; A < F->arg_size(); A++)
    Add(mangle(F->getName()) + "_arg_" + std::to_string(A),
        udonTypeOf(F->getArg(A)->getType()));

  // Walk forward from the call. A slot is live across it when some later
  // instruction READS it, so both the later instruction's own result and each
  // of its operands are added.
  //
  // Adding only the later instruction's result is wrong, and wrong in the case
  // that matters most: with two calls to the same recursive function, the first
  // call's result is an OPERAND of an instruction after the second call
  // (`%r = add %call1, %call2`). Its slot belongs to this function, so a nested
  // activation overwrites it; without the spill the first result is lost.

  // The original walk below scans the function's blocks in DECLARATION order
  // and adds the results of instructions after the call. That misses one
  // shape: a LOOP HEADER is usually declared before the block that contains
  // the call, so a phi in the header whose operand is computed just before a
  // recursive call is never seen. The value is live across the call, the
  // callee (same function) clobbers its slot, and the phi then reads garbage.
  // Measured on the Zig display-list walker: the cursor desynced by exactly
  // one word per sub-list return, so the walk ran off the end of the list.
  //
  // The fix is narrow on purpose. Spilling every block reachable from the call
  // is also "correct" but spills loop-body temporaries that the callee never
  // reads, which tripled the program and overflowed the caller-save stacks.
  auto AddValue = [&](const Value *V) {
    auto It = Slots.find(V);
    if (It == Slots.end())
      return;
    std::string Ty = udonTypeOf(V->getType());
    if (Ty.empty())
      return;
    Add(It->second, Ty);
  };

  bool After = false;
  for (const BasicBlock &BB : *F)
    for (const Instruction &Other : BB) {
      if (&Other == &I) {
        After = true;
        continue;
      }
      if (!After)
        continue;
      if (!Other.getType()->isVoidTy())
        AddValue(&Other);
      for (const Use &U : Other.operands())
        AddValue(U.get());
    }

  return Live;
}
