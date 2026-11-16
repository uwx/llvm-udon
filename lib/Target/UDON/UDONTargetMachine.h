//===-- UDONTargetMachine.h - TargetMachine for the UDON --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the TargetMachine that is used by the UDON backend.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_UDON_UDONTARGETMACHINE_H
#define LLVM_LIB_TARGET_UDON_UDONTARGETMACHINE_H

#include "UDONWriter.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Target/TargetMachine.h"

namespace llvm {

class UDONTargetMachine : public TargetMachine {
public:
  UDONTargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                    StringRef FS, const TargetOptions &Options,
                    std::optional<Reloc::Model> RM,
                    std::optional<CodeModel::Model> CM, CodeGenOptLevel OL,
                    bool JIT);

  bool addPassesToEmitFile(PassManagerBase &PM, raw_pwrite_stream &Out,
                           raw_pwrite_stream *DwoOut, CodeGenFileType FileType,
                           bool DisableVerify = true,
                           MachineModuleInfoWrapperPass *MMIWP = nullptr) override;

  /// llc dereferences this unconditionally even for text output, so supply an
  /// instance rather than leaving it null.
  TargetLoweringObjectFile *getObjFileLowering() const override {
    return &TLOF;
  }

  /// What the last emitted module needs, for the driver: the slot count it
  /// computed and the heap image to write beside the `.uasm`.
  uint64_t slotCount() const;
  StringRef heapImage() const;

private:
  /// The writer added by addPassesToEmitFile, so a caller can ask it what the
  /// emitted module needs. Owned by the pass manager once added.
  UDONWriter *Writer = nullptr;

  /// UDON emits text, so the section/relocation hooks are never reached; the
  /// few pure virtuals only need to exist.
  class UDONObjectFile : public TargetLoweringObjectFile {
    MCSection *SelectSectionForGlobal(const GlobalObject *, SectionKind,
                                      const TargetMachine &) const override {
      return nullptr;
    }
    MCSection *getExplicitSectionGlobal(const GlobalObject *, SectionKind,
                                        const TargetMachine &) const override {
      return nullptr;
    }
  };
  mutable UDONObjectFile TLOF;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_UDON_UDONTARGETMACHINE_H
