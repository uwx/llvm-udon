//===-- UDONTargetInfo.cpp - UDON Target Implementation -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "UDONTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
using namespace llvm;

Target &llvm::getTheUDONTarget() {
  static Target TheUDONTarget;
  return TheUDONTarget;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeUDONTargetInfo() {
  // UDON has no architecture of its own; it is selected by name (-march=udon)
  // rather than by target triple, so intentionally match no triple.
  RegisterTarget<> X(getTheUDONTarget(), "udon", "UDON backend", "UDON");
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeUDONTargetMC() {}
