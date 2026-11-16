//===-- UDONWriterMemory.cpp - byte memory lowering ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `__mem` is one `%SystemByteArray`, and a pointer is a byte offset into it, so
// every LLVM offset is used verbatim and no shift is ever applied.
//
// Layout, stated once:
//
//   [0, 16)                                    reserved: bitcast scratch, and
//                                              the reason no object has
//                                              address 0 (Rust's None niche)
//   [16, -udon-memory-bytes)                   globals image
//   [-udon-memory-bytes, +shadow-stack-bytes)  alloca shadow stack, growing down
//   the trailing 8 bytes                       padding for sub-word windows
//
// The 8 pad bytes are what collapse the whole alignment problem: a `load i8` at
// the last byte still reads a full 32-bit window.
//
//===----------------------------------------------------------------------===//

#include "UDONWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

namespace {
inline std::string mangle(StringRef Name) { return mangleName(Name); }
} // namespace

namespace {

/// The reserved low region of `__mem`, so no global or object has address 0.
constexpr uint32_t MemoryReserve = 16;
/// Pad bytes at the end of `__mem`, so a sub-word window read is always in
/// bounds.
constexpr uint32_t MemoryPad = 8;

} // namespace

void UDONWriter::layOutGlobals() {
  // Globals start after the reserved region. Their initializer bytes go into a
  // separate `__data_image` array rather than into `__mem` directly: the
  // importer runs at edit time while `__udon_init` runs at play time, and a
  // 1 MiB image must not be serialized per behaviour.
  uint32_t Offset = MemoryReserve;
  for (GlobalVariable &G : Mod->globals()) {
    if (G.isDeclaration())
      continue;
    if (G.getAddressSpace() != 0)
      reportUDONError("global in a non-default address space: " + G.getName());

    uint64_t Size = DL->getTypeAllocSize(G.getValueType());
    unsigned Align = G.getAlign() ? G.getAlign()->value()
                                  : DL->getABITypeAlign(G.getValueType()).value();
    if (Align > 1)
      Offset = alignTo(Offset, Align);

    GlobalOffsets[&G] = Offset;
    Offset += static_cast<uint32_t>(Size);
  }

  if (Offset > Opts.MemoryBytes)
    reportUDONError("globals image is " + Twine(Offset) +
                    " bytes, does not fit below -udon-memory-bytes=" +
                    Twine(Opts.MemoryBytes) + "; raise the flag");

  DataImage.assign(Offset > MemoryReserve ? Offset - MemoryReserve : 0, 0);

  // Initializers are copied byte by byte. A global with no initializer stays
  // zero, which is what `__data_image` already holds.
  for (GlobalVariable &G : Mod->globals()) {
    if (G.isDeclaration() || !G.hasInitializer())
      continue;
    uint32_t Base = GlobalOffsets[&G];
    if (auto *CDA = dyn_cast<ConstantDataArray>(G.getInitializer())) {
      if (!CDA->getElementType()->isIntegerTy(8))
        continue;
      StringRef Bytes = CDA->getRawDataValues();
      for (size_t I = 0; I < Bytes.size(); I++)
        DataImage[Base - MemoryReserve + I] = static_cast<uint8_t>(Bytes[I]);
    } else if (auto *CI = dyn_cast<ConstantInt>(G.getInitializer())) {
      uint64_t V = CI->getZExtValue();
      unsigned Bytes = static_cast<unsigned>(DL->getTypeStoreSize(CI->getType()));
      for (unsigned I = 0; I < Bytes; I++)
        DataImage[Base - MemoryReserve + I] = static_cast<uint8_t>(V >> (8 * I));
    } else if (isa<ConstantAggregateZero>(G.getInitializer())) {
      // Already zero.
    } else if (!G.getInitializer()->isNullValue()) {
      reportUDONError("unsupported global initializer for " + G.getName());
    }
  }
}

void UDONWriter::layOutFrames() {
  // A function's frame is the sum of its surviving allocas, rounded up to 16 so
  // every frame stays 16-aligned: the data layout is S128 and optimized code
  // may rely on the low pointer bits.
  for (Function &F : *Mod) {
    if (F.isDeclaration())
      continue;
    uint32_t FrameSize = 0;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB) {
        auto *AI = dyn_cast<AllocaInst>(&I);
        if (!AI)
          continue;
        if (!isa<ConstantInt>(AI->getArraySize()))
          reportUDONError("dynamic alloca is not supported: " + F.getName());
        uint64_t Count =
            cast<ConstantInt>(AI->getArraySize())->getZExtValue();
        uint64_t Size = DL->getTypeAllocSize(AI->getAllocatedType()) * Count;
        unsigned Align = AI->getAlign().value();
        if (Align > 1)
          FrameSize = alignTo(FrameSize, Align);
        AllocaOffsets[AI] = FrameSize;
        FrameSize += static_cast<uint32_t>(Size);
      }
    if (FrameSize > 0)
      FrameSizes[&F] = alignTo(FrameSize, 16);
  }
}

std::string UDONWriter::pointerOffset(const Value *V, UBlock &B) {
  // A pointer is already the byte offset, so this is a slot lookup; a
  // `getelementptr` that survived folding has been lowered into arithmetic by
  // selectGEP and its result slot holds the offset.
  return slotFor(V);
}

void UDONWriter::selectGEP(const GetElementPtrInst &I, UFunction &UF,
                           unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  // GEP arithmetic on the byte offset: a constant part folds into the base, and
  // a variable index is multiplied by the size of the type it indexes.
  //
  // `gep_type_iterator` already walks the type for each index, so the element
  // size comes from `getIndexedType()` -- deriving it from the previous index's
  // type instead is what multiplied a `[64 x i8]` index by 4.
  const Value *Base = I.getPointerOperand();
  std::string BaseSlot = slotFor(Base);

  APInt ConstantOffset(64, 0);
  SmallVector<std::pair<const Value *, uint64_t>, 4> Terms;

  for (gep_type_iterator GTI = gep_type_begin(&I), E = gep_type_end(&I);
       GTI != E; ++GTI) {
    const Value *Index = GTI.getOperand();
    if (StructType *STy = GTI.getStructTypeOrNull()) {
      auto *CI = dyn_cast<ConstantInt>(Index);
      if (!CI)
        reportUDONError("non-constant struct index: " + I.getName());
      ConstantOffset += DL->getStructLayout(STy)->getElementOffset(
          static_cast<unsigned>(CI->getZExtValue()));
      continue;
    }
    // The size of one step: the indexed type's allocation size.
    uint64_t ElementSize = DL->getTypeAllocSize(GTI.getIndexedType());
    if (auto *CI = dyn_cast<ConstantInt>(Index)) {
      ConstantOffset += ElementSize * CI->getSExtValue();
      continue;
    }
    Terms.push_back({Index, ElementSize});
  }

  int64_t ConstPart = ConstantOffset.getSExtValue();
  if (Terms.empty()) {
    if (ConstPart == 0) {
      emitCopy(B, BaseSlot, slotFor(&I));
      return;
    }
    emitExtern(B,
               "SystemInt32.__op_Addition__SystemInt32_SystemInt32__SystemInt32",
               {BaseSlot, i32Constant(static_cast<int32_t>(ConstPart))},
               slotFor(&I));
    return;
  }

  std::string Accum = BaseSlot;
  if (ConstPart != 0) {
    Accum = freshSlot("gep_base", I.getType());
    emitExtern(B,
               "SystemInt32.__op_Addition__SystemInt32_SystemInt32__SystemInt32",
               {BaseSlot, i32Constant(static_cast<int32_t>(ConstPart))}, Accum);
  }
  for (auto &[Index, ElementSize] : Terms) {
    std::string Scaled = slotFor(Index);
    if (ElementSize != 1) {
      Scaled = freshSlot("gep_scale", I.getType());
      emitExtern(B,
                 "SystemInt32.__op_Multiplication__SystemInt32_SystemInt32__SystemInt32",
                 {slotFor(Index), i32Constant(static_cast<int32_t>(ElementSize))},
                 Scaled);
    }
    std::string Next = freshSlot("gep_add", I.getType());
    emitExtern(B,
               "SystemInt32.__op_Addition__SystemInt32_SystemInt32__SystemInt32",
               {Accum, Scaled}, Next);
    Accum = Next;
  }
  emitCopy(B, Accum, slotFor(&I));
}

void UDONWriter::selectAlloca(const AllocaInst &I, UFunction &UF,
                             unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  // A surviving alloca lives in the shadow stack: its address is
  // `__sp + <offset within the frame>`. mem2reg removed every alloca it could,
  // so what is left is address-taken or a local array or struct.
  auto It = AllocaOffsets.find(&I);
  if (It == AllocaOffsets.end())
    reportUDONError("alloca has no frame offset: " + I.getName());
  std::string Addend = i32Constant(static_cast<int32_t>(It->second));
  emitExtern(B,
             "SystemInt32.__op_Addition__SystemInt32_SystemInt32__SystemInt32",
             {"__sp", Addend}, slotFor(&I));
}

std::string UDONWriter::emitLoad(UBlock &B, const std::string &Offset,
                                 Type *Ty) {
  std::string Dst = freshSlot("load", Ty);
  if (Ty->isIntegerTy(32) || Ty->isPointerTy()) {
    emitExtern(B,
               "SystemBitConverter.__ToInt32__SystemByteArray_SystemInt32__SystemInt32",
               {"__mem", Offset}, Dst);
    return Dst;
  }
  if (Ty->isIntegerTy(64)) {
    emitExtern(B,
               "SystemBitConverter.__ToInt64__SystemByteArray_SystemInt32__SystemInt64",
               {"__mem", Offset}, Dst);
    return Dst;
  }
  if (Ty->isFloatTy()) {
    emitExtern(B,
               "SystemBitConverter.__ToSingle__SystemByteArray_SystemInt32__SystemSingle",
               {"__mem", Offset}, Dst);
    return Dst;
  }
  if (Ty->isDoubleTy()) {
    emitExtern(B,
               "SystemBitConverter.__ToDouble__SystemByteArray_SystemInt32__SystemDouble",
               {"__mem", Offset}, Dst);
    return Dst;
  }
  if (Ty->isIntegerTy(8) || Ty->isIntegerTy(16)) {
    // A padded 32-bit window read, then mask. The 8 pad bytes at the end of
    // `__mem` are what make the window always in bounds.
    std::string Word = freshSlot("load_w", Type::getInt32Ty(Mod->getContext()));
    emitExtern(B,
               "SystemBitConverter.__ToInt32__SystemByteArray_SystemInt32__SystemInt32",
               {"__mem", Offset}, Word);
    std::string Mask =
        i32Constant(Ty->isIntegerTy(8) ? 0xFF : 0xFFFF);
    std::string Narrow = freshSlot("load_n", Type::getInt32Ty(Mod->getContext()));
    emitExtern(B,
               "SystemInt32.__op_LogicalAnd__SystemInt32_SystemInt32__SystemInt32",
               {Word, Mask}, Narrow);
    // Sign-extend from the narrow width: (x << (32 - w)) >> (32 - w).
    if (Ty->isIntegerTy(8)) {
      std::string Shifted = freshSlot("load_se", Narrow.empty()
                                                   ? Type::getInt32Ty(Mod->getContext())
                                                   : Type::getInt32Ty(Mod->getContext()));
      emitExtern(B,
                 "SystemInt32.__op_LeftShift__SystemInt32_SystemInt32__SystemInt32",
                 {Narrow, i32Constant(24)}, Shifted);
      std::string Result = freshSlot("load_se2", Type::getInt32Ty(Mod->getContext()));
      emitExtern(B,
                 "SystemInt32.__op_RightShift__SystemInt32_SystemInt32__SystemInt32",
                 {Shifted, i32Constant(24)}, Result);
      emitCopy(B, Result, Dst);
    } else {
      std::string Shifted = freshSlot("load_se", Type::getInt32Ty(Mod->getContext()));
      emitExtern(B,
                 "SystemInt32.__op_LeftShift__SystemInt32_SystemInt32__SystemInt32",
                 {Narrow, i32Constant(16)}, Shifted);
      std::string Result = freshSlot("load_se2", Type::getInt32Ty(Mod->getContext()));
      emitExtern(B,
                 "SystemInt32.__op_RightShift__SystemInt32_SystemInt32__SystemInt32",
                 {Shifted, i32Constant(16)}, Result);
      emitCopy(B, Result, Dst);
    }
    return Dst;
  }
  reportUDONError("load of an unsupported type");
}

void UDONWriter::emitStore(UBlock &B, const std::string &Value,
                           const std::string &Offset, Type *Ty) {
  if (Ty->isIntegerTy(32) || Ty->isPointerTy() || Ty->isIntegerTy(64) ||
      Ty->isFloatTy() || Ty->isDoubleTy()) {
    // GetBytes returns a fresh array into a scratch slot, then __CopyTo__
    // writes it into `__mem` at the offset. One small allocation per store.
    //
    // Measured alternatives, all worse:
    //   Convert.__ToByte__SystemInt32 -> aborts in the VM, even in range
    //   SystemArray.__SetValue__(object, i32) -> type check rejects the box
    //   Int32Array.__Set__(i, v) -> works and allocates nothing, but
    //     BitConverter.To* rejects an Int32Array, so every load would need
    //     manual shifts; that is a different memory model, not a local change
    // wasm2usharp makes the same choice: BitConverter.GetBytes(v).CopyTo(mem, i).
    StringRef SrcType;
    if (Ty->isIntegerTy(32) || Ty->isPointerTy()) SrcType = "SystemInt32";
    else if (Ty->isIntegerTy(64)) SrcType = "SystemInt64";
    else if (Ty->isFloatTy()) SrcType = "SystemSingle";
    else SrcType = "SystemDouble";

    std::string Bytes = scratchByteArray();
    emitExtern(B, ("SystemBitConverter.__GetBytes__" + SrcType +
                   "__SystemByteArray").str(),
               {Value}, Bytes);
    emitExtern(B,
               "SystemByteArray.__CopyTo__SystemArray_SystemInt32__SystemVoid",
               {Bytes, "__mem", Offset}, "");
    return;
  }

  if (Ty->isIntegerTy(8) || Ty->isIntegerTy(16)) {
    // Read-modify-write on the little-endian 32-bit window at the offset: the
    // target byte is at shift 0, so a byte store is
    // (w & 0xFFFFFF00) | (v & 0xFF), and a 16-bit store uses 0xFFFF.
    std::string Word = freshSlot("st_w", Type::getInt32Ty(Mod->getContext()));
    emitExtern(B,
               "SystemBitConverter.__ToInt32__SystemByteArray_SystemInt32__SystemInt32",
               {"__mem", Offset}, Word);

    uint32_t Keep = Ty->isIntegerTy(8) ? 0xFFFFFF00u : 0xFFFF0000u;
    uint32_t Take = Ty->isIntegerTy(8) ? 0xFFu : 0xFFFFu;

    std::string Kept = freshSlot("st_keep", Type::getInt32Ty(Mod->getContext()));
    emitExtern(B,
               "SystemInt32.__op_LogicalAnd__SystemInt32_SystemInt32__SystemInt32",
               {Word, i32Constant(static_cast<int32_t>(Keep))}, Kept);
    std::string Taken = freshSlot("st_take", Type::getInt32Ty(Mod->getContext()));
    emitExtern(B,
               "SystemInt32.__op_LogicalAnd__SystemInt32_SystemInt32__SystemInt32",
               {Value, i32Constant(static_cast<int32_t>(Take))}, Taken);
    std::string Merged = freshSlot("st_merge", Type::getInt32Ty(Mod->getContext()));
    emitExtern(B,
               "SystemInt32.__op_LogicalOr__SystemInt32_SystemInt32__SystemInt32",
               {Kept, Taken}, Merged);

    std::string Bytes = scratchByteArray();
    emitExtern(B, "SystemBitConverter.__GetBytes__SystemInt32__SystemByteArray",
               {Merged}, Bytes);
    emitExtern(B,
               "SystemByteArray.__CopyTo__SystemArray_SystemInt32__SystemVoid",
               {Bytes, "__mem", Offset}, "");
    return;
  }

  reportUDONError("store of an unsupported type");
}

std::string UDONWriter::scratchByteArray() {
  // One scratch slot per function, reused by every store: the array is written
  // and consumed within one instruction's expansion, so a second store can
  // safely overwrite it.
  if (ScratchBytes.empty()) {
    ScratchBytes = mangle(Cur->F->getName()) + "_store_bytes";
    declareSlot(ScratchBytes, "SystemByteArray");
    NullSlots.push_back({ScratchBytes, "SystemByteArray"});
  }
  return ScratchBytes;
}

void UDONWriter::selectLoad(const LoadInst &I, UFunction &UF,
                           unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  std::string Offset = pointerOffset(I.getPointerOperand(), B);
  emitCopy(B, emitLoad(B, Offset, I.getType()), slotFor(&I));
}

void UDONWriter::selectStore(const StoreInst &I, UFunction &UF,
                            unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  std::string Offset = pointerOffset(I.getPointerOperand(), B);
  emitStore(B, slotFor(I.getValueOperand()), Offset,
            I.getValueOperand()->getType());
}
