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
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"

namespace llvm {

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
  PM.add(createUnreachableBlockEliminationPass());
  PM.add(createLowerSwitchPass());
  PM.add(createCFGSimplificationPass());
  // CFGSimplify can fold LowerSwitch's comparison tree back into a switch
  // (e.g. when several cases are contiguous), and the writer cannot emit a
  // switch at all. Lower again after the CFG cleanup; idempotent when no
  // switch remains.
  PM.add(createLowerSwitchPass());
  PM.add(createLowerAtomicPass());
  PM.add(createExpandReductionsPass());
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
