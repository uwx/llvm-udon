//===-- UDONTargetMachine.cpp - TargetMachine for the UDON ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "UDONTargetMachine.h"
#include "UDONWriter.h"
#include "TargetInfo/UDONTargetInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Scalarizer.h"
#include "llvm/Transforms/Utils.h"

namespace llvm {
void addUDONInlinerPass(ModulePassManager &MPM);
ModulePass *createUDONInlinerPass();
FunctionPass *createUDONProtectFPConvPass();
ModulePass *createUDONRestoreFPConvPass();
} // namespace llvm
namespace llvm {

namespace {

// The writer needs a canonical CFG -- in particular, every critical edge split
// so a phi copy has somewhere to live -- and it gets that from the passes, not
// from its own analysis. `optnone` makes the legacy pass manager skip a
// function entirely, so a function compiled at -O0 would reach the writer with
// critical edges intact and be miscompiled.
//
// `optnone` is an optimizer directive; this backend is a code generator whose
// correctness depends on its normalization running, so the directive is cleared
// before the pipeline rather than honoured. Everything else about the function
// is left alone.
class UDONStripOptNone : public ModulePass {
public:
  static char ID;
  UDONStripOptNone() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    bool Changed = false;
    for (Function &F : M)
      if (F.hasOptNone()) {
        F.removeFnAttr(Attribute::OptimizeNone);
        Changed = true;
      }
    return Changed;
  }
  StringRef getPassName() const override { return "UDON: strip optnone"; }
};

// `optsize`/`minsize`/`safestack-opt`-style size hints make the inliner and
// the other normalization passes extraordinarily conservative -- tuned for a
// native backend where a call is cheap. Here a call is a shadow-stack frame
// plus `__mem` spills, so the hint's tradeoff is exactly backwards: stripping
// it lets the inliner buy out calls and lets the CFG passes do their job.
class UDONStripSizeHints : public ModulePass {
public:
  static char ID;
  UDONStripSizeHints() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    bool Changed = false;
    for (Function &F : M) {
      if (F.hasFnAttribute(Attribute::OptimizeForSize)) {
        F.removeFnAttr(Attribute::OptimizeForSize);
        Changed = true;
      }
      if (F.hasFnAttribute(Attribute::MinSize)) {
        F.removeFnAttr(Attribute::MinSize);
        Changed = true;
      }
    }
    return Changed;
  }

  StringRef getPassName() const override { return "UDON: strip size hints"; }
};

char UDONStripSizeHints::ID = 0;

char UDONStripOptNone::ID = 0;

} // namespace

// A pointer is a byte offset into Udon's memory array, so pointers are 32-bit
// and i64 is the widest integer. i8 and i16 are not native (n32:64): every
// sub-word access goes through a padded 32-bit window read, so declaring them
// native would let a pass assume an aligned byte load exists.
static const char *const UDONDataLayout =
    "e-m:e-p:32:32-i64:64-i128:128-f32:32-f64:64-n32:64-S128";

UDONTargetMachine::UDONTargetMachine(const Target &T, const Triple &TT,
                                     StringRef CPU, StringRef FS,
                                     const TargetOptions &Options,
                                     std::optional<Reloc::Model> RM,
                                     std::optional<CodeModel::Model> CM,
                                     CodeGenOptLevel OL, bool JIT)
    : TargetMachine(T, UDONDataLayout, TT, CPU, FS, Options) {}

bool UDONTargetMachine::addPassesToEmitFile(
    PassManagerBase &PM, raw_pwrite_stream &Out, raw_pwrite_stream *DwoOut,
    CodeGenFileType FileType, bool DisableVerify,
    MachineModuleInfoWrapperPass *MMIWP) {
  if (FileType != CodeGenFileType::AssemblyFile)
    return true;

  // Prepare the IR for the constructs UASM can express: no switch, no atomics,
  // no reduction intrinsics, SSA locals, and a CFG whose every edge is a
  // suitable home for a phi copy.
  PM.add(new UDONStripOptNone());
  PM.add(new UDONStripSizeHints());
  PM.add(createUnreachableBlockEliminationPass());
  // Scalar cleanup FIRST. Two reasons:
  //
  // 1. GVN must run before inlining. Inlining a function that contains an
  //    indirect call moves that call into a caller that also holds the
  //    `store ptr @target` feeding it; GVN then devirtualizes the indirect
  //    call into a direct one, which collides with the writer's separate
  //    `__icall_*` convention for address-taken callees. Measured: an infinite
  //    loop in `test/indirect.ll`. Run before inlining, GVN sees the store and
  //    the indirect call in different functions and leaves both alone.
  //
  // 2. `fptosi`/`fptoui` are hidden behind opaque calls for the duration of
  //    the cleanup: this target saturates where LLVM says out-of-range is
  //    poison, so a scalar optimizer would fold `fptosi double 1.0e30 to i32`
  //    to poison. Measured: without the guard, GVN turned `test/float.ll`'s
  //    expected -2147483645 into 0.
  PM.add(createUDONProtectFPConvPass());
  PM.add(createEarlyCSEPass());
  PM.add(createGVNPass());
  PM.add(createLICMPass());
  PM.add(createUDONRestoreFPConvPass());
  // (The protect declarations are erased by the restore pass's module
  // finalisation, so nothing address-takes them afterwards.)
  // `__mem` externs, so the break-even point for a call is a fraction of a
  // native one and small helpers must be inlined. Inlining also keeps their
  // locals in slots instead of shadow-stack words, which is what turns a
  // float load there into one array extern plus a reinterpret instead of two.
  // It runs before the CFG normalization passes because they assume a settled
  // CFG, and before BreakCriticalEdges so the inlined phis land where the
  // writer can copy them.
  PM.add(createUDONInlinerPass());
  // SROA after inlining: a callee's `Vec3f`/`Mat4` arguments were pointers, so
  // before inlining their backing arrays had to live in shadow-stack memory.
  // After inlining nothing takes their address, and SROA splits those arrays
  // into scalar slots -- which removes the store/load round trip and the
  // bitcast externs from every component.
  PM.add(createSROAPass(/*PreserveCFG=*/false));
  PM.add(createLowerSwitchPass());
  // CFGSimplify can fold LowerSwitch's comparison tree back into a switch
  // (e.g. when several cases are contiguous), and the writer cannot emit a
  // switch at all. Lower again after the CFG cleanup; idempotent when no
  // switch remains.
  PM.add(createLowerSwitchPass());
  PM.add(createLowerAtomicPass());
  PM.add(createExpandReductionsPass());
  // Udon has no vector type, no vector instruction and no SIMD extern, so a
  // vector that reaches the writer has no lowering at all. clang produces them
  // anyway -- a two-element float array initialised in one go becomes a
  // `store <2 x float>`, and arithmetic on it stays vectorised -- because the
  // frontend was told x86 is the target. The Scalarizer splits every vector
  // instruction into its elements, which is exactly the lowering Udon needs;

  // InstCombine is deliberately NOT run here. It constant-folds `fptosi` of an
  // out-of-range constant to poison, and this target's `fptosi` is a saturating
  // conversion at run time -- a documented difference from LLVM's semantics that
  // folding would erase. The Scalarizer's leftover vector constants are handled
  // by the writer instead.
  //
  // Loads and stores of vector type are only split when asked: the default is
  // to leave them alone, and a `store <2 x float>` is precisely the shape the
  // frontend emits for a two-element float array.
  ScalarizerPassOptions ScalarizeMemory;
  ScalarizeMemory.ScalarizeLoadStore = true;
  PM.add(createScalarizerPass(ScalarizeMemory));
  // A second run costs nothing and catches anything the first left behind.
  PM.add(createScalarizerPass(ScalarizeMemory));
  // `*.with.overflow` yields a `{ T, i1 }` aggregate and the writer has no
  // aggregate slot, so it is split into the value and the flag here. It must
  // run after InstCombine, not before: InstCombine recognises a multiply whose
  // high bits are tested and *re-forms* the intrinsic, so lowering first just
  // gives it something to undo.
  PM.add(createUDONLowerOverflowPass());
  PM.add(createPromoteMemoryToRegisterPass());
  // BreakCriticalEdges must be last: CFGSimplify merges the blocks it splits
  // and LowerSwitch adds new edges, so an earlier run is undone. It splits
  // only critical edges, which leaves every remaining edge with a predecessor
  // that has one successor or a successor that has one predecessor -- exactly
  // where the writer's phi copies go.
  PM.add(createBreakCriticalEdgesPass());
  Writer = new UDONWriter(Out, udonWriterOptions());
  PM.add(Writer);
  return false;
}

uint64_t UDONTargetMachine::slotCount() const {
  return Writer ? Writer->slotCount() : 0;
}

StringRef UDONTargetMachine::heapImage() const {
  return Writer ? Writer->heapImage() : StringRef();
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeUDONTarget() {
  RegisterTargetMachine<UDONTargetMachine> X(getTheUDONTarget());
}

} // namespace llvm
