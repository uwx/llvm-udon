//===-- UDONCallGraph.cpp - SCCs and re-entrancy classification ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Which functions may be re-entered while they are running. A function in a
// recursive SCC, or one that is address-taken, needs the emulated-stack
// discipline: its own slots can be clobbered by a nested activation, so the
// caller spills what is live across the call.
//
// The graph is built from the IR, never from `norecurse`: hand-written and
// freshly emitted IR carry no function attributes, so an attribute-based answer
// would be wrong exactly where it matters.
//
//===----------------------------------------------------------------------===//

#include "UDONWriter.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

/// Extern families that cannot synchronously run Udon code on this behaviour.
///
/// An allowlist, not a blocklist: the safe families are enumerable and
/// everything else is suspect. That covers -- without having to name them --
/// SendCustomEvent, SetProgramVariable (a field-change callback can fire an
/// event back), Instantiate (runs OnEnable), Pickup.Drop (fires OnDrop),
/// networking ownership changes, SetActive, set_enabled and Destroy.
bool isSafeExtern(StringRef Signature) {
  static const char *const Safe[] = {
      "SystemInt32",   "SystemUInt32",  "SystemInt64",  "SystemUInt64",
      "SystemSingle",  "SystemDouble",  "SystemBoolean", "SystemByte",
      "SystemSByte",   "SystemInt16",   "SystemUInt16", "SystemChar",
      "SystemMath",    "SystemConvert", "SystemBitConverter",
      "SystemString",  "SystemArray",   "SystemObject", "SystemBuffer",
      "SystemByteArray", "SystemInt32Array", "SystemUInt32Array",
      "SystemInt64Array", "SystemUInt64Array", "SystemSingleArray",
      "SystemDoubleArray", "SystemStringArray", "SystemObjectArray",
      "SystemBooleanArray", "SystemCharArray",
      "UnityEngineVector2", "UnityEngineVector3", "UnityEngineVector4",
      "UnityEngineQuaternion", "UnityEngineColor", "UnityEngineMathf",
      "UnityEngineDebug",
  };
  StringRef Module = Signature.split('.').first;
  for (const char *S : Safe)
    if (Module == S)
      return true;
  return false;
}

} // namespace

void UDONWriter::buildCallGraph() {
  // Every direct call is an edge. An indirect call site is an edge to every
  // address-taken function: without that, A calling B through a pointer while B
  // calls A directly would leave A's SCC trivial, A would get a static frame,
  // and the inner invocation would overwrite the outer one.
  DenseMap<const Function *, SmallPtrSet<const Function *, 8>> Succs;
  SmallVector<const Function *, 16> AddressTaken;
  for (Function &F : *Mod) {
    if (F.isDeclaration())
      continue;
    if (F.hasAddressTaken())
      AddressTaken.push_back(&F);
    for (BasicBlock &BB : F)
      for (Instruction &I : BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (!CB)
          continue;
        if (const Function *Callee = CB->getCalledFunction()) {
          if (Callee->isDeclaration())
            continue;
          Succs[&F].insert(Callee);
        } else {
          for (const Function *Target : AddressTaken)
            Succs[&F].insert(Target);
        }
      }
  }

  // Tarjan over the graph, iteratively: a self-edge is an SCC of size 1.
  DenseMap<const Function *, int> Index;
  DenseMap<const Function *, int> LowLink;
  DenseMap<const Function *, bool> OnStack;
  SmallVector<const Function *, 32> Stack;
  int NextIndex = 0;

  std::function<void(const Function *)> Visit = [&](const Function *F) {
    Index[F] = LowLink[F] = NextIndex++;
    Stack.push_back(F);
    OnStack[F] = true;

    for (const Function *Succ : Succs[F]) {
      if (!Index.count(Succ)) {
        Visit(Succ);
        LowLink[F] = std::min(LowLink[F], LowLink[Succ]);
      } else if (OnStack[Succ]) {
        LowLink[F] = std::min(LowLink[F], Index[Succ]);
      }
    }

    if (LowLink[F] != Index[F])
      return;

    // Pop one SCC.
    SmallVector<const Function *, 8> Members;
    const Function *Member;
    do {
      Member = Stack.pop_back_val();
      OnStack[Member] = false;
      Members.push_back(Member);
    } while (Member != F);

    bool Recursive = Members.size() > 1;
    if (!Recursive) {
      // A single member is recursive only if it calls itself.
      const Function *Only = Members.front();
      for (const Function *Succ : Succs[Only])
        if (Succ == Only) {
          Recursive = true;
          break;
        }
    }
    if (Recursive)
      for (const Function *M : Members)
        RecursiveFunctions.insert(M);
  };

  for (Function &F : *Mod)
    if (!F.isDeclaration() && !Index.count(&F))
      Visit(&F);

  // Address-taken functions also need the emulated-stack discipline: their
  // slots are reachable from a call site the writer cannot see.
  for (const Function *F : AddressTaken)
    RecursiveFunctions.insert(F);

  // Re-entrancy is transitive: a function that can reach an extern which may
  // synchronously run Udon code inherits the property.
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (Function &F : *Mod) {
      if (F.isDeclaration())
        continue;
      bool Reenters = RecursiveFunctions.count(&F) != 0;
      if (!Reenters) {
        for (BasicBlock &BB : F)
          for (Instruction &I : BB) {
            auto *CB = dyn_cast<CallBase>(&I);
            if (!CB)
              continue;
            if (const Function *Callee = CB->getCalledFunction()) {
              if (Callee->isDeclaration()) {
                if (!isSafeExtern(Callee->getName())) {
                  Reenters = true;
                  break;
                }
              } else if (RecursiveFunctions.count(Callee)) {
                Reenters = true;
                break;
              }
            } else {
              Reenters = true;
              break;
            }
          }
      }
      if (Reenters && !RecursiveFunctions.count(&F)) {
        RecursiveFunctions.insert(&F);
        Changed = true;
      }
    }
  }
}
