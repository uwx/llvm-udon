//===-- UDONLowerOverflow.cpp - Lower *.with.overflow intrinsics -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `llvm.{u,s}{add,sub,mul}.with.overflow` returns a `{ T, i1 }` aggregate. The
// writer has no slot type for an aggregate and no lowering for `extractvalue`:
// every SSA value it emits is a single slot. The intrinsic is therefore split
// before code generation into the value and the flag, both ordinary values,
// and the `extractvalue` users that read them are replaced.
//
// This is also what makes clang-compiled C work without noticing: `calloc`'s
// `n * size` overflow test is turned into exactly this intrinsic at -O1.
//
//===----------------------------------------------------------------------===//

#include "UDONTargetMachine.h"
#include "llvm/IR/Constants.h"
#include <climits>
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

using namespace llvm;

namespace {

class UDONLowerOverflow : public FunctionPass {
public:
  static char ID;
  UDONLowerOverflow() : FunctionPass(ID) {}

  bool runOnFunction(Function &F) override;

private:
  bool lower(CallInst *CI, Intrinsic::ID Id);
};

char UDONLowerOverflow::ID = 0;

bool UDONLowerOverflow::runOnFunction(Function &F) {
  SmallVector<CallInst *, 8> Calls;
  for (Instruction &I : instructions(F)) {
    auto *CI = dyn_cast<CallInst>(&I);
    if (!CI)
      continue;
    Function *Callee = CI->getCalledFunction();
    if (!Callee || !Callee->isIntrinsic())
      continue;
    switch (Callee->getIntrinsicID()) {
    case Intrinsic::uadd_with_overflow:
    case Intrinsic::usub_with_overflow:
    case Intrinsic::sadd_with_overflow:
    case Intrinsic::ssub_with_overflow:
    case Intrinsic::umul_with_overflow:
    case Intrinsic::smul_with_overflow:
      Calls.push_back(CI);
      break;
    default:
      break;
    }
  }

  bool Changed = false;
  for (CallInst *CI : Calls)
    Changed |= lower(CI, cast<IntrinsicInst>(CI)->getIntrinsicID());
  return Changed;
}

bool UDONLowerOverflow::lower(CallInst *CI, Intrinsic::ID Id) {
  // Only `extractvalue` of a single field is a shape this can replace. Anything
  // else means the aggregate escaped, which the writer cannot represent either,
  // so failing here with one message beats producing half-lowered IR.
  SmallVector<ExtractValueInst *, 4> Extracts;
  for (User *U : CI->users()) {
    auto *EV = dyn_cast<ExtractValueInst>(U);
    if (!EV || EV->getNumIndices() != 1)
      report_fatal_error("llvm-udon: an overflow intrinsic result escaped "
                         "through something other than extractvalue in " +
                         CI->getFunction()->getName());
    Extracts.push_back(EV);
  }

  IRBuilder<> B(CI);
  Value *A = CI->getArgOperand(0);
  Value *C = CI->getArgOperand(1);
  Type *Ty = A->getType();
  LLVMContext &Ctx = Ty->getContext();
  unsigned Bits = Ty->getIntegerBitWidth();
  Value *Zero = ConstantInt::get(Ty, 0);
  Type *I1 = Type::getInt1Ty(Ctx);

  Value *Result = nullptr;
  Value *Overflow = nullptr;

  switch (Id) {
  case Intrinsic::uadd_with_overflow:
    // `r <u a` is the carry out of the top bit.
    Result = B.CreateAdd(A, C);
    Overflow = B.CreateICmpULT(Result, A);
    break;
  case Intrinsic::usub_with_overflow:
    // A borrow happens exactly when the subtrahend exceeds the minuend.
    Result = B.CreateSub(A, C);
    Overflow = B.CreateICmpULT(A, C);
    break;
  case Intrinsic::sadd_with_overflow:
    // Both operands agree with the result in sign exactly when no wrap occurred.
    Result = B.CreateAdd(A, C);
    Overflow = B.CreateICmpSLT(
        B.CreateAnd(B.CreateXor(A, Result), B.CreateXor(C, Result)), Zero);
    break;
  case Intrinsic::ssub_with_overflow:
    // A wrap needs the operands to disagree, and the result to follow the
    // subtrahend's sign rather than the minuend's.
    Result = B.CreateSub(A, C);
    Overflow = B.CreateICmpSLT(
        B.CreateAnd(B.CreateXor(A, C), B.CreateXor(A, Result)), Zero);
    break;
  case Intrinsic::umul_with_overflow:
  case Intrinsic::smul_with_overflow: {
    bool IsSigned = Id == Intrinsic::smul_with_overflow;
    if (Bits < 64) {
      // Widen to exactly twice the width, where the full product fits, and ask
      // whether the truncated result still carries the same value.
      Type *Wide = IntegerType::get(Ctx, Bits * 2);
      Value *AW = IsSigned ? B.CreateSExt(A, Wide) : B.CreateZExt(A, Wide);
      Value *CW = IsSigned ? B.CreateSExt(C, Wide) : B.CreateZExt(C, Wide);
      Value *Full = B.CreateMul(AW, CW);
      Result = B.CreateTrunc(Full, Ty);
      if (IsSigned) {
        // Sign-extending the truncated product reproduces the full product
        // only when nothing was lost.
        Overflow = B.CreateICmpNE(Full, B.CreateSExt(Result, Wide));
      } else {
        Value *Max = ConstantInt::get(
            Wide, APInt::getMaxValue(Bits).zext(Bits * 2));
        Overflow = B.CreateICmpUGT(Full, Max);
      }
      break;
    }

    // i64 has no wider type here, so overflow is a division round-trip. The
    // divisor is replaced by 1 whenever it is the value that would trap, so the
    // division is never poison, and the real check is selected afterwards.
    Result = B.CreateMul(A, C);
    if (IsSigned) {
      Value *NonZero = B.CreateICmpNE(A, Zero);
      Value *MinusOne = ConstantInt::get(Ty, -1);
      Value *IsMinusOne = B.CreateICmpEQ(A, MinusOne);
      Value *SafeA = B.CreateSelect(IsMinusOne, ConstantInt::get(Ty, 1), A);
      Value *Back = B.CreateSDiv(Result, SafeA);
      Value *Wrong = B.CreateICmpNE(Back, C);
      Value *Min = ConstantInt::getSigned(Ty, INT64_MIN);
      Value *Corner = B.CreateICmpEQ(C, Min);
      Overflow = B.CreateSelect(
          NonZero, B.CreateOr(Wrong, B.CreateAnd(IsMinusOne, Corner)),
          ConstantInt::getFalse(I1));
    } else {
      Value *NonZero = B.CreateICmpNE(C, Zero);
      Value *SafeC = B.CreateSelect(NonZero, C, ConstantInt::get(Ty, 1));
      Value *Back = B.CreateUDiv(Result, SafeC);
      Overflow = B.CreateSelect(NonZero, B.CreateICmpNE(Back, A),
                                 ConstantInt::getFalse(I1));
    }
    break;
  }
  default:
    llvm_unreachable("unexpected intrinsic");
  }

  bool ZeroUsed = false, FlagUsed = false;
  for (ExtractValueInst *EV : Extracts) {
    unsigned Index = EV->getIndices()[0];
    (Index == 0 ? ZeroUsed : FlagUsed) = true;
    EV->replaceAllUsesWith(Index == 0 ? Result : Overflow);
    EV->eraseFromParent();
  }
  // Keep the arithmetic only when something read it; a caller that wanted just
  // the flag should not pay for a multiply whose result nothing uses.
  if (!ZeroUsed && Result->use_empty())
    if (auto *I = dyn_cast<Instruction>(Result))
      I->eraseFromParent();
  if (!FlagUsed && Overflow->use_empty())
    if (auto *I = dyn_cast<Instruction>(Overflow))
      I->eraseFromParent();
  CI->eraseFromParent();
  return true;
}

} // namespace

FunctionPass *llvm::createUDONLowerOverflowPass() {
  return new UDONLowerOverflow();
}
