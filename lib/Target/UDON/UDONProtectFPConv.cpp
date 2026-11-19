//===-- UDONProtectFPConv.cpp - Keep fptosi/fptoui out of the optimizers ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This target's float-to-integer conversion SATURATES: `fptosi` of 1.0e30
// yields INT_MAX, and `test/convert-fp.ll` / `test/float.ll` pin that.
// LLVM's semantics are different -- out of range is poison -- so a scalar
// optimizer is entitled to fold `fptosi double 1.0e30 to i32` to poison and
// propagate it. Measured: adding GVN to the pipeline turned that fixture's
// result from -2147483645 into 0.
//
// The fix is to make the conversion opaque for the duration of the cleanup
// passes: `UDONProtectFPConv` rewrites every `fptosi`/`fptoui` into a call to
// a private, side-effecting-by-declaration function, and `UDONRestoreFPConv`
// turns those calls back into the original instructions once the optimizers
// have run. A call the optimizer knows nothing about cannot be folded, and the
// writer then sees the same `fptosi` it always did.
//
// The private function is declared with `nobuiltin` and no other attributes,
// so it is not a candidate for anything: not inlining (it has no body), not
// constant folding (no known semantics), and not elimination (it is not
// `readnone`).
//
//===----------------------------------------------------------------------===//

#include "UDONTargetMachine.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace {

static const char *const ProtectName = "__udon_fptosi_protect";
static const char *const ProtectNameU = "__udon_fptoui_protect";

/// The declaration the protected calls target. It is `nounwind` only so it can
/// appear anywhere; every attribute that would let an optimizer reason about
/// its result is deliberately absent.
static FunctionCallee protectDecl(Module &M, Type *RetTy, Type *ArgTy,
                                  bool IsUnsigned) {
  LLVMContext &Ctx = M.getContext();
  FunctionType *FT = FunctionType::get(RetTy, {ArgTy}, false);
  FunctionCallee FC = M.getOrInsertFunction(
      IsUnsigned ? ProtectNameU : ProtectName, FT);
  auto *F = cast<Function>(FC.getCallee());
  // Internal, so once the restore pass has turned every call back into
  // `fptosi`/`fptoui` the now-dead declaration is removable: it must never
  // reach the writer as an address-taken function.
  F->setLinkage(GlobalValue::InternalLinkage);
  F->addFnAttr(Attribute::NoUnwind);
  F->addFnAttr(Attribute::NoBuiltin);
  return FC;
}

class UDONProtectFPConv : public FunctionPass {
public:
  static char ID;
  UDONProtectFPConv() : FunctionPass(ID) {}

  bool runOnFunction(Function &F) override {
    if (F.isDeclaration())
      return false;
    SmallVector<Instruction *, 16> Convs;
    for (Instruction &I : instructions(F))
      if (I.getOpcode() == Instruction::FPToSI ||
          I.getOpcode() == Instruction::FPToUI)
        Convs.push_back(&I);
    if (Convs.empty())
      return false;

    bool Changed = false;
    for (Instruction *I : Convs) {
      bool IsUnsigned = I->getOpcode() == Instruction::FPToUI;
      FunctionCallee FC = protectDecl(*F.getParent(), I->getType(),
                                      I->getOperand(0)->getType(), IsUnsigned);
      IRBuilder<> B(I);
      CallInst *CI = B.CreateCall(FC, {I->getOperand(0)}, I->getName());
      CI->setDebugLoc(I->getDebugLoc());
      I->replaceAllUsesWith(CI);
      I->eraseFromParent();
      Changed = true;
    }
    return Changed;
  }

  StringRef getPassName() const override {
    return "UDON: protect float-to-int conversion";
  }
};

char UDONProtectFPConv::ID = 0;

class UDONRestoreFPConv : public ModulePass {
public:
  static char ID;
  UDONRestoreFPConv() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    bool Changed = false;
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      SmallVector<CallInst *, 16> Calls;
      for (Instruction &I : instructions(F))
        if (auto *CI = dyn_cast<CallInst>(&I)) {
          // `getCalledFunction` is null when the callee is a bitcast, which
          // happens when two conversions of different types share the one
          // declaration: `getOrInsertFunction` then returns a cast callee.
          // Stripping the cast is what finds them all.
          auto *Callee = dyn_cast<Function>(
              CI->getCalledOperand()->stripPointerCasts());
          if (!Callee)
            continue;
          StringRef N = Callee->getName();
          if (N == ProtectName || N == ProtectNameU)
            Calls.push_back(CI);
        }
      for (CallInst *CI : Calls) {
        auto *Callee = cast<Function>(
            CI->getCalledOperand()->stripPointerCasts());
        bool IsUnsigned = Callee->getName() == ProtectNameU;
        Instruction *New = IsUnsigned
                               ? static_cast<Instruction *>(new FPToUIInst(
                                     CI->getArgOperand(0), CI->getType(),
                                     CI->getName()))
                               : static_cast<Instruction *>(new FPToSIInst(
                                     CI->getArgOperand(0), CI->getType(),
                                     CI->getName()));
        New->insertBefore(CI);
        New->setDebugLoc(CI->getDebugLoc());
        CI->replaceAllUsesWith(New);
        CI->eraseFromParent();
        Changed = true;
      }
    }
    // Every call is restored, so the declarations are dead. Erasing them is
    // what keeps them out of the writer's function-pointer table.
    for (const char *N : {ProtectName, ProtectNameU})
      if (Function *D = M.getFunction(N))
        if (D->isDeclaration() && D->use_empty()) {
          D->eraseFromParent();
          Changed = true;
        }
    return Changed;
  }

  StringRef getPassName() const override {
    return "UDON: restore float-to-int conversion";
  }
};

char UDONRestoreFPConv::ID = 0;

} // namespace

namespace llvm {
FunctionPass *createUDONProtectFPConvPass() { return new UDONProtectFPConv(); }
ModulePass *createUDONRestoreFPConvPass() { return new UDONRestoreFPConv(); }
} // namespace llvm
