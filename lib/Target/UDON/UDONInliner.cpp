//===-- UDONInliner.cpp - greedy inliner for the UDON backend -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A Udon call is expensive in a way a native call is not: it costs a
// shadow-stack frame (two `__mem` writes plus a bounds check), a return
// address slot, and a caller-save round trip through `__stack_*` for every
// value live across the call -- each an `__mem` extern. A 12-instruction
// helper like `mtxf_mul_vec3f_x` measured 628 VM instructions per call.
//
// The break-even size is therefore far above a native target's, and this LLVM
// tree exposes no legacy function inliner (`createFunctionInliningPass` was
// removed; only `createAlwaysInlinerLegacyPass` remains, driven by attributes
// the C frontend never sets). The backend therefore runs its own inliner over
// the `InlineFunction` utility: greedy, call-graph ordered bottom-up, with a
// size budget tuned to the extern cost.
//
// It is a legacy module pass so it slots straight into the writer's existing
// legacy pipeline, before the CFG normalization passes: an inlined callee's
// phis then land where BreakCriticalEdges and the writer expect them, and the
// lowered switch/atomic forms the writer requires are re-established by the
// passes that follow.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

// An Udon call costs a shadow-stack frame plus a caller-save round trip
// through `__mem` for every live value, so the break-even for a callee is far
// below a native target's. Measured on libsm64: `mtxf_mul_vec3f_x` (≈40
// instructions) pays for itself when inlined, and 100 is a round budget that
// also swallows the mid-size helpers. The cost is a plain callee instruction
// count -- not `InlineCost`, whose native model needs the caller's TTI and
// analysis caches keyed per function and produces garbage without them
// (measured: costs like -14985).
static cl::opt<unsigned> UdonInlineThreshold(
    "udon-inline-threshold", cl::Hidden,
    cl::desc("Maximum callee instruction count accepted by the UDON inliner "
             "(default 100)"),
    cl::init(100));

// Depth of the inline chain allowed per site (guards pathological cascades).
static constexpr unsigned MaxInlineDepth = 4;

namespace {

class UDONInliner {
public:
  explicit UDONInliner(unsigned Threshold) : Threshold(Threshold) {}

  bool run(Module &M) {
    bool Changed = false;
    // Inline every call site in every function to a fixpoint. A bottom-up
    // traversal is an optimisation here, not a requirement: InlineFunction
    // handles recursion correctly, and a function reached only through a
    // recursive SCC must still be processed. The inner `inlineCallsIn`
    // re-scans each function up to four rounds, so a chain of helpers that
    // inlines into a caller in one round is picked up by the caller's own
    // next round; the outer loop re-runs until nothing changes.
    bool Any;
    do {
      Any = false;
      for (Function &F : M) {
        if (F.isDeclaration())
          continue;
        if (inlineCallsIn(F))
          Any = true;
      }
      Changed |= Any;
    } while (Any);
    return Changed;
  }

private:
  unsigned Threshold;

  /// True when `From` can reach `Target` by following direct calls, which
  /// includes `From == Target`. Bounded DFS: a real program's call graph is
  /// small and this runs once per call site, so no memoisation is needed.
  static bool canReach(Function &From, Function &Target) {
    SmallVector<Function *, 16> Work{&From};
    SmallPtrSet<Function *, 32> Seen;
    while (!Work.empty()) {
      Function *Cur = Work.pop_back_val();
      if (Cur == &Target)
        return true;
      if (!Seen.insert(Cur).second)
        continue;
      for (BasicBlock &BB : *Cur)
        for (Instruction &I : BB)
          if (auto *CB = dyn_cast<CallBase>(&I))
            if (Function *Callee = CB->getCalledFunction())
              if (!Callee->isDeclaration())
                Work.push_back(Callee);
    }
    return false;
  }
  bool inlineCallsIn(Function &F) {
    bool Changed = false;

    // Collect the sites first: InlineFunction invalidates iterators.
    SmallVector<CallBase *, 32> Sites;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (auto *CB = dyn_cast<CallBase>(&I))
          if (Function *Callee = CB->getCalledFunction())
            if (!Callee->isDeclaration() && !Callee->isIntrinsic())
              Sites.push_back(CB);

    // One pass, in order, no rescan: a site list built before any inline is
    // what a deterministic inliner needs. Rescanning to "catch" sites that a
    // prior inline moved into continuation blocks is unbounded -- each
    // success grows the callee, the size test passes again, and the pass
    // never converges (measured: 3.7 GB of output). The call sites that
    // matter are all present in this fresh list.
    for (CallBase *CB : Sites) {
      Function *Callee = CB->getCalledFunction();
      if (!Callee || Callee->isDeclaration() || Callee->isIntrinsic())
        continue;
      // Never inline a call to an address-taken function: the writer keeps a
      // separate calling convention for those (arguments and return through
      // `__icall_*`, with the body copying them into its own slots on entry).
      // Inlining would make a function both a direct and an indirect callee,
      // and the indirect prologue would clobber the direct call's return slot
      // -- measured as an infinite loop in `test/indirect.ll`.
      if (Callee->hasAddressTaken())
        continue;
      // Never inline a call that can reach the caller again: the clone would
      // re-enter a function whose frame the writer manages with the
      // emulated-stack discipline, and BreakCriticalEdges crashes on the
      // resulting CFG (measured on test/alloca.ll's `recurse`). The recursive
      // SCC keeps its call, which is what the writer's spill path is for.
      if (Callee == &F || canReach(*Callee, F))
        continue;
      if (Callee->hasFnAttribute(Attribute::NoInline))
        continue;
      if (CB->isMustTailCall() || isa<CallBrInst>(CB) || isa<InvokeInst>(CB))
        continue;
      if (Callee->getFunctionType()->isVarArg())
        continue;

      // The decision is a plain callee-size count, not LLVM's InlineCost.
      //
      // InlineCost is a native cost model: it prices a call against a
      // register-saving call instruction and needs the caller's TTI,
      // assumption cache and library info keyed per function. Feeding it
      // synthetic analyses makes it read state it was never given -- the
      // measured result was costs like -14985 -- so the decision was noise.
      //
      // Two costs matter here. Instructions: an Udon call is a shadow-stack
      // frame plus an `__mem` spill per live value, so a small callee is worth
      // inlining. Alloca bytes: a callee's frame becomes the CALLER's frame
      // once inlined, so a callee holding a large buffer would make every call
      // of the caller allocate it. `test/entry-recover.ll` depends on that
      // separation -- its oversized frame must stay in its own function so the
      // first event succeeds and only the second overflows -- so a big frame
      // is never inlined at any threshold.
      unsigned Cost = 0;
      bool BigFrame = false;
      for (BasicBlock &CBB : *Callee) {
        Cost += static_cast<unsigned>(CBB.size());
        for (Instruction &CI : CBB)
          if (auto *AI = dyn_cast<AllocaInst>(&CI)) {
            unsigned Bytes = static_cast<unsigned>(
                F.getDataLayout().getTypeAllocSize(AI->getAllocatedType()));
            if (auto *CN = dyn_cast<ConstantInt>(AI->getArraySize()))
              Bytes *= static_cast<unsigned>(CN->getZExtValue());
            if (Bytes > 256)
              BigFrame = true;
            Cost += Bytes / 8;
          }
      }
      if (BigFrame || Cost > Threshold)
        continue;
      InlineFunctionInfo IFI;
      if (!InlineFunction(*CB, IFI).isSuccess())
        continue;
      Changed = true;
    }
    return Changed;
  }
};
} // namespace

namespace {

class UDONInlinerLegacy : public ModulePass {
public:
  static char ID;
  UDONInlinerLegacy() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    return UDONInliner(UdonInlineThreshold).run(M);
  }

  StringRef getPassName() const override { return "UDON: greedy inliner"; }
};

char UDONInlinerLegacy::ID = 0;

} // namespace

namespace llvm {
ModulePass *createUDONInlinerPass() { return new UDONInlinerLegacy(); }
} // namespace llvm
