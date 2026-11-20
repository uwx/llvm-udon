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
#include "llvm/Support/raw_ostream.h"
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

// --- scalar global promotion ------------------------------------------------
//
// A scalar global whose address is never taken has no reason to live in
// `__mem`. In memory every access costs a word-index computation plus an array
// extern; as a typed heap slot a load is a reference and a store is one COPY.
// Measured on libsm64: 140 of 1081 globals qualify and cover 1388 sites.
//
// The address-taken test is conservative in the right direction: ANY mention
// of the global other than a bare `load/store ptr @g` marks it taken. That
// includes `getelementptr @g` (field access), `ptrtoint`, passing it as an
// argument, and storing it into a pointer -- all of which need the byte
// address that only `__mem` provides.

/// True when `G` is a scalar type that has a natural Udon slot type.
///
/// Pointers are deliberately excluded. A pointer slot and a pointer *value* are
/// the same `%SystemInt32`, so a promoted pointer global would make
/// `load ptr @g` and `load i32, ptr %p` indistinguishable at the point of use,
/// and any later `__mem` read through that value would use a slot name where a
/// byte offset belongs. Pointers stay in `__mem`, where the address is a real
/// address.
static bool isPromotableType(Type *Ty) {
  return Ty->isIntegerTy(1) || Ty->isIntegerTy(8) || Ty->isIntegerTy(16) ||
         Ty->isIntegerTy(32) || Ty->isIntegerTy(64) || Ty->isFloatTy() ||
         Ty->isDoubleTy();
}

void UDONWriter::classifyGlobals() {
  for (GlobalVariable &G : Mod->globals()) {
    if (G.isDeclaration())
      continue;
    if (!isPromotableType(G.getValueType()))
      continue;
    // The bridge ABI: the host reads and writes these through `__mem` at the
    // exported offset, so they must stay there.
    if (G.getName().starts_with("udon_export_"))
      continue;
    // `malloc`'s bump pointer and its end are written by the writer itself
    // through the data image; keeping them in memory keeps that path simple.
    if (G.getName() == "__udon_heap_ptr" || G.getName() == "__udon_heap_end")
      continue;

    bool Taken = false;
    for (const Use &U : G.uses()) {
      const auto *I = dyn_cast<Instruction>(U.getUser());
      if (!I) { Taken = true; break; }          // constant or metadata use
      if (auto *LI = dyn_cast<LoadInst>(I)) {
        if (LI->getPointerOperand() == &G) continue;
      } else if (auto *SI = dyn_cast<StoreInst>(I)) {
        if (SI->getPointerOperand() == &G) continue;
      }
      Taken = true;
      break;
    }
    if (!Taken)
      PromotedSlots[&G] = {};
  }
}

std::string UDONWriter::promotedSlot(const GlobalVariable &G) {
  auto It = PromotedSlots.find(&G);
  if (It == PromotedSlots.end())
    return {};
  if (!It->second.empty())
    return It->second;
  std::string Name = "__gs_" + mangle(G.getName());
  std::string Ty = udonTypeOf(G.getValueType());
  if (Ty.empty())
    reportUDONError("promoted global has no Udon type: " + G.getName());
  declareSlot(Name, Ty);
  // A promoted global keeps its initial value. `SystemInt32` is the one type
  // the assembler can write as a literal, so a 32-bit initializer goes into the
  // data block and needs no host write; everything else (i64, bool, f32, f64)
  // goes into the heap image, which the importer applies.
  auto *Init = G.hasInitializer() ? G.getInitializer() : nullptr;
  bool Literal = false;
  if (auto *CI = dyn_cast_or_null<ConstantInt>(Init)) {
    if (CI->getBitWidth() == 32) {
      DataLines.push_back("    " + Name + ": %SystemInt32, " +
                          std::to_string(static_cast<int32_t>(
                              CI->getValue().getSExtValue())));
      Literal = true;
    } else if (CI->getBitWidth() == 64) {
      HeapEntries.push_back("\"" + Name + "\": {\"type\": \"" + Ty +
                            "\", \"value\": {\"i64\": " +
                            std::to_string(static_cast<int64_t>(CI->getValue().getSExtValue())) + "}}");
    } else if (CI->getBitWidth() == 1) {
      HeapEntries.push_back("\"" + Name + "\": {\"type\": \"" + Ty +
                            "\", \"value\": {\"bool\": " +
                            (CI->isOne() ? "true" : "false") + "}}");
    }
  } else if (auto *CF = dyn_cast_or_null<ConstantFP>(Init)) {
    uint64_t Bits = CF->getValueAPF().bitcastToAPInt().getZExtValue();
    std::string Key = Ty == "SystemSingle" ? "f32bits" : "f64bits";
    HeapEntries.push_back("\"" + Name + "\": {\"type\": \"" + Ty +
                          "\", \"value\": {\"" + Key + "\": " +
                          std::to_string(Bits) + "}}");
  }
  if (!Literal)
    NullSlots.push_back({Name, Ty});
  It->second = Name;
  return Name;
}

// --- typed float arrays ----------------------------------------------------
//
// A global whose element type is float, which is never byte-touched and whose
// address never escapes into anything but a float-typed GEP, can live in its
// own `%SystemSingleArray`. The win is the bitcast pair: in `__mem` a float
// load is `__Get__` + `Int32BitsToSingle` and a store the reverse, so every
// float access costs two externs. In a typed array it costs one.
//
// The test is deliberately conservative. Any use that is not a float-typed
// GEP, a float load/store, or a plain copy of the pointer is an escape and
// keeps the global in `__mem`. Measured on the playground: 18 globals qualify
// (6,575 floats), including `gSineTable`, which `sins`/`coss` read from 13
// translation units.

/// True when `Ty` is a float, or an array/struct whose leaves are all floats.
static bool isAllFloat(Type *Ty) {
  if (Ty->isFloatTy())
    return true;
  if (auto *AT = dyn_cast<ArrayType>(Ty))
    return isAllFloat(AT->getElementType());
  if (auto *ST = dyn_cast<StructType>(Ty)) {
    for (Type *E : ST->elements())
      if (!isAllFloat(E))
        return false;
    return true;
  }
  return false;
}

/// True when every use of `G` stays float-typed: a GEP whose indexed type is
/// float, a float load/store through such a pointer, or a copy of the pointer
/// into a slot that is itself only used that way. Anything else is an escape.
static bool onlyFloatAccesses(const GlobalVariable &G) {
  SmallVector<const Value *, 16> Work{&G};
  SmallPtrSet<const Value *, 16> Seen;
  while (!Work.empty()) {
    const Value *V = Work.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    for (const Use &U : V->uses()) {
      const auto *I = dyn_cast<Instruction>(U.getUser());
      if (!I) {
        // A constant-expression GEP of the pointer is fine when it stays
        // float-typed: the declaring TU's own tables reach their reads that
        // way after linking. Anything else in a constant is an escape.
        auto *CE = dyn_cast<ConstantExpr>(U.getUser());
        if (CE && CE->getOpcode() == Instruction::GetElementPtr &&
            isAllFloat(CE->getType())) {
          Work.push_back(CE);
          continue;
        }
        return false; // any other constant use: treat as an escape
      }
      if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
        // The pointer operand must be what we are tracking (or another GEP of
        // the same tracked chain), and the element it lands on must be float.
        // A GEP that walks into an integer field is an escape.
        if (GEP->getPointerOperand() != V) {
          if (getenv("UDON_DBG_F32"))
            errs() << "  gep mismatch: " << GEP->getPointerOperand()->getName() << "\n";
          return false;
        }
        if (!isAllFloat(GEP->getResultElementType()))
          return false;
        Work.push_back(GEP);
        continue;
      }
      if (isa<PHINode>(I) || isa<SelectInst>(I)) {
        // A phi or select of the pointer escapes: the tables it may choose
        // between are individually typed, but Udon has no array select, so
        // the writer cannot express choosing between two typed arrays. Keep
        // such globals in memory.
        return false;
      }
      if (auto *LI = dyn_cast<LoadInst>(I)) {
        if (LI->getPointerOperand() == V && LI->getType()->isFloatTy())
          continue;
        if (getenv("UDON_DBG_F32"))
          errs() << "  bad load: " << *LI->getType() << " op=" << LI->getPointerOperand()->getName() << "\n";
        return false;
      }
      if (auto *SI = dyn_cast<StoreInst>(I)) {
        if (SI->getPointerOperand() == V && SI->getValueOperand()->getType()->isFloatTy())
          continue;
        if (getenv("UDON_DBG_F32"))
          errs() << "  bad store\n";
        return false;
      }
      if (isa<BitCastInst>(I) || isa<PtrToIntInst>(I) || isa<IntToPtrInst>(I)) {
        if (getenv("UDON_DBG_F32"))
          errs() << "  cast escape\n";
        return false;
      }
      if (isa<CallInst>(I) || isa<InvokeInst>(I)) {
        if (getenv("UDON_DBG_F32"))
          errs() << "  call escape\n";
        return false; // passed to a call: unknown callee behaviour
      }
      if (getenv("UDON_DBG_F32"))
        errs() << "  other escape\n";
      return false;
    }
  }
  return true;
}

void UDONWriter::classifyFloatArrays() {
  for (GlobalVariable &G : Mod->globals()) {
    if (G.isDeclaration() || G.isConstant())
      continue;
    // A scalar float is already handled by classifyGlobals; only ARRAYS (or
    // multi-field structs) need a typed array. A scalar claimed here would
    // collide with its promoted `__gs_*` slot and disappear from both.
    if (promotedSlot(G).size())
      continue;
    Type *Ty = G.getValueType();
    if (!isAllFloat(Ty))
      continue;
    if (!isa<ArrayType>(Ty))
      continue;
    // `analyzeTypedPointers` walks every pointer derived from this array and
    // poisons any with a use the writer cannot lower against it. An array it
    // did not clear stays byte-addressed in `__mem`.
    if (!SafeTypedArrays.count(&G)) {
      if (getenv("UDON_DBG_F32"))
        errs() << "f32 rejected (unsafe): " << G.getName() << "\n";
      continue;
    }
    // An exported typed array's slot IS the bridge: the host reads `float[]`
    // from `__bridge_<name>`, so the slot takes that name instead of the
    // `__f32_` prefix. A non-exported array keeps the internal name.
    std::string Name = "__f32_" + mangle(G.getName());
    if (G.getName().starts_with("udon_export_")) {
      StringRef Off = G.getName().drop_front(StringRef("udon_export_").size());
      Name = "__bridge_" + mangle(Off);
    }
    if (getenv("UDON_DBG_F32"))
      errs() << "f32 accepted: " << G.getName() << "\n";
    FloatArraySlots[&G] = Name;
  }
}

/// Declare each float-array slot and give it its initializer.
///
/// The slot is a `%SystemSingleArray` created at init time (an array cannot be
/// a `.data_start` literal). Its contents come from the heap image as a
/// base64 f32 stream, which `UdonHeapImage` applies as `float[]`.
void UDONWriter::emitFloatArrays() {
  if (getenv("UDON_DBG_F32"))
    errs() << "float arrays accepted: " << FloatArraySlots.size() << "\n";
  for (auto &[G, Slot] : FloatArraySlots) {
    if (getenv("UDON_DBG_F32"))
      errs() << "  emit " << Slot << " size=" << DL->getTypeAllocSize(G->getValueType()).getFixedValue() / 4 << "\n";
    declareSlot(Slot, "SystemSingleArray");
    NullSlots.push_back({Slot, "SystemSingleArray"});
    // Element count, for the ctor call the init block emits.
    TypeSize TSize = DL->getTypeAllocSize(G->getValueType());
    if (TSize.isScalable())
      reportUDONError("float array global with a scalable size: " + G->getName());
    uint64_t Count = TSize.getFixedValue() / 4;
    FloatArraySizes[Slot] = Count;

    // Initializer bytes, if the global has one.
    std::string Bytes;
    if (auto *CA = dyn_cast<ConstantAggregateZero>(G->getInitializer())) {
      (void)CA; // all zeros: the array is created zeroed, so no bytes needed
    } else if (auto *AZ = dyn_cast<ConstantDataSequential>(G->getInitializer())) {
      // A ConstantDataSequential of floats: emit its raw little-endian bits.
      for (unsigned K = 0; K < AZ->getNumElements(); K++) {
        auto *CF = dyn_cast<ConstantFP>(AZ->getElementAsConstant(K));
        if (!CF)
          break;
        uint32_t Bits = CF->getValueAPF().bitcastToAPInt().getZExtValue();
        Bytes.push_back((char)(Bits & 0xFF));
        Bytes.push_back((char)((Bits >> 8) & 0xFF));
        Bytes.push_back((char)((Bits >> 16) & 0xFF));
        Bytes.push_back((char)((Bits >> 24) & 0xFF));
      }
    }
    if (Bytes.empty())
      continue;
    // base64 of the f32 stream.
    static const char *Tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string Out;
    for (size_t I = 0; I < Bytes.size(); I += 3) {
      uint32_t N = (uint8_t)Bytes[I] << 16;
      if (I + 1 < Bytes.size()) N |= (uint8_t)Bytes[I + 1] << 8;
      if (I + 2 < Bytes.size()) N |= (uint8_t)Bytes[I + 2];
      Out += Tbl[(N >> 18) & 63];
      Out += Tbl[(N >> 12) & 63];
      Out += I + 1 < Bytes.size() ? Tbl[(N >> 6) & 63] : '=';
      Out += I + 2 < Bytes.size() ? Tbl[N & 63] : '=';
    }
    HeapEntries.push_back("\"" + Slot + "\": {\"type\": \"SystemSingleArray\", " +
                          "\"value\": {\"bytes\": \"" + Out +
                          "\", \"floatArray\": true}}");
  }
}

void UDONWriter::layOutGlobals() {
  // Globals start after the reserved region. Their initializer bytes go into a
  // separate `__data_image` array rather than into `__mem` directly: the
  // importer runs at edit time while `__udon_init` runs at play time, and a
  // 1 MiB image must not be serialized per behaviour.
  uint32_t Offset = MemoryReserve;
  for (GlobalVariable &G : Mod->globals()) {
    if (G.isDeclaration())
      continue;
    // Promoted globals live in typed heap slots and claim no memory bytes.
    if (!promotedSlot(G).empty())
      continue;
    // Float arrays live in their own %SystemSingleArray slot.
    if (FloatArraySlots.count(&G))
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

  // Initializers are serialized into `__data_image` by walking the constant
  // tree. Doing it element by element, rather than only handling the shapes a
  // fixture happened to use, is what lets real C through: a table of structs, an
  // array of floats and a table of function pointers are all ordinary C and all
  // appeared in libsm64.
  //
  // A global with no initializer stays zero, which `__data_image` already is.
  for (GlobalVariable &G : Mod->globals()) {
    if (G.isDeclaration() || !G.hasInitializer())
      continue;
    // Promoted globals carry their initializer in their heap-image slot.
    if (!promotedSlot(G).empty())
      continue;
    if (FloatArraySlots.count(&G))
      continue;
    writeConstantBytes(G.getInitializer(), GlobalOffsets[&G]);
  }

  // `runtime/libc/libc.c` declares these two and leaves them zero, because the
  // C file cannot know the memory map. Fill them here: the heap starts where
  // the globals image ends and stops where the shadow stack begins. With
  // `-udon-heap-bytes=0` both are equal and `malloc` always fails, which is
  // the correct answer for a program with no heap.
  for (GlobalVariable &G : Mod->globals()) {
    StringRef Name = G.getName();
    uint64_t Value;
    if (Name == "__udon_heap_ptr")
      Value = Opts.MemoryBytes;
    else if (Name == "__udon_heap_end")
      Value = Opts.MemoryBytes + Opts.HeapBytes;
    else
      continue;
    if (!G.getValueType()->isIntegerTy(32))
      reportUDONError(Name + " must be i32: the heap bounds are byte offsets");
    writeConstantBytes(ConstantInt::get(Type::getInt32Ty(Mod->getContext()), Value),
                       GlobalOffsets[&G]);
  }
}

void UDONWriter::writeConstantBytes(const Constant *C, uint32_t Offset) {
  Type *Ty = C->getType();
  TypeSize TS386 = DL->getTypeAllocSize(Ty);
  if (TS386.isScalable())
    reportUDONError("386 scalable");
  if (TS386.getFixedValue() == 0)
    return;

  // Zero, undef and poison all mean "already zero".
  if (isa<ConstantAggregateZero>(C) || isa<UndefValue>(C) ||
      isa<PoisonValue>(C))
    return;

  // A packed bytes/ints/floats array: each element is written at its own
  // stride, which is the store size, not the alloc size.
  if (auto *CDS = dyn_cast<ConstantDataSequential>(C)) {
    Type *ElemTy = CDS->getElementType();
    TypeSize TS398 = DL->getTypeAllocSize(ElemTy);
    if (TS398.isScalable())
      reportUDONError("398 scalable");
    uint64_t Stride = TS398.getFixedValue();
    for (unsigned I = 0; I < CDS->getNumElements(); I++)
      writeConstantBytes(CDS->getElementAsConstant(I),
                         Offset + static_cast<uint32_t>(Stride * I));
    return;
  }

  if (auto *CS = dyn_cast<ConstantStruct>(C)) {
    const StructLayout *SL = DL->getStructLayout(CS->getType());
    for (unsigned I = 0; I < CS->getNumOperands(); I++)
      writeConstantBytes(cast<Constant>(CS->getOperand(I)),
                         Offset + static_cast<uint32_t>(SL->getElementOffset(I)));
    return;
  }

  if (auto *CA = dyn_cast<ConstantArray>(C)) {
    uint64_t Stride = DL->getTypeAllocSize(CA->getType()->getElementType());
    for (unsigned I = 0; I < CA->getNumOperands(); I++)
      writeConstantBytes(cast<Constant>(CA->getOperand(I)),
                         Offset + static_cast<uint32_t>(Stride * I));
    return;
  }

  if (auto *CV = dyn_cast<ConstantVector>(C)) {
    uint64_t Stride = DL->getTypeAllocSize(CV->getType()->getElementType());
    for (unsigned I = 0; I < CV->getNumOperands(); I++)
      writeConstantBytes(cast<Constant>(CV->getOperand(I)),
                         Offset + static_cast<uint32_t>(Stride * I));
    return;
  }

  if (auto *CI = dyn_cast<ConstantInt>(C)) {
    uint64_t V = CI->getZExtValue();
    unsigned N = static_cast<unsigned>(DL->getTypeStoreSize(CI->getType()));
    for (unsigned I = 0; I < N; I++)
      DataImage[Offset - MemoryReserve + I] =
          static_cast<uint8_t>(V >> (8 * I));
    return;
  }

  if (auto *CF = dyn_cast<ConstantFP>(C)) {
    // Raw bits, so a NaN payload or -0.0 survives byte for byte.
    uint64_t V = CF->getValueAPF().bitcastToAPInt().getZExtValue();
    unsigned N = static_cast<unsigned>(DL->getTypeStoreSize(CF->getType()));
    for (unsigned I = 0; I < N; I++)
      DataImage[Offset - MemoryReserve + I] =
          static_cast<uint8_t>(V >> (8 * I));
    return;
  }

  // A function used as a value is an index into `__fn_table`, exactly as it is
  // in `constantSlot`, so a callback table and an indirect call agree. This is
  // why `collectFunctionPointers` must run before the image is built.
  //
  // Checked before the address case because C spells a stored function pointer
  // either as the function itself or as `ptrtoint (ptr @fn to i32)`.
  {
    uint64_t Index = 0;
    if (constantFunctionIndex(C, Index)) {
      for (unsigned I = 0; I < 4; I++)
        DataImage[Offset - MemoryReserve + I] =
            static_cast<uint8_t>(Index >> (8 * I));
      return;
    }
  }

  // A pointer or `ptrtoint` initializer is a `__mem` byte offset, and every
  // global already has its offset, so a table of pointers (a vtable, a jump
  // table, a callback list) is resolved at compile time.
  if (Ty->isPointerTy() || Ty->isIntegerTy(32)) {
    int64_t Addr = 0;
    if (constantAddress(C, Addr)) {
      auto V = static_cast<uint64_t>(Addr);
      for (unsigned I = 0; I < 4; I++)
        DataImage[Offset - MemoryReserve + I] =
            static_cast<uint8_t>(V >> (8 * I));
      return;
    }
  }

  std::string Text;
  raw_string_ostream OS(Text);
  C->print(OS);
  reportUDONError("unsupported global initializer element: " + OS.str());
}

bool UDONWriter::constantFunctionIndex(const Constant *C,
                                       uint64_t &Out) const {
  if (auto *Fn = dyn_cast<Function>(C)) {
    auto It = FunctionPointerIndices.find(Fn);
    if (It == FunctionPointerIndices.end())
      return false;
    Out = It->second;
    return true;
  }
  // A cast or `ptrtoint` does not change which function the value denotes.
  if (auto *CE = dyn_cast<ConstantExpr>(C))
    switch (CE->getOpcode()) {
    case Instruction::PtrToInt:
    case Instruction::BitCast:
    case Instruction::AddrSpaceCast:
      return constantFunctionIndex(CE->getOperand(0), Out);
    default:
      break;
    }
  return false;
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
  // A promoted global's "address" is the slot that holds its value, so a load
  // or store naming it directly is a reference / a COPY rather than an array
  // access. Measured on libsm64: 1388 of 13813 memory sites take this path.
  if (auto *G = dyn_cast<GlobalVariable>(V))
    if (std::string S = promotedSlot(*G); !S.empty())
      return S;
  // A pointer is already the byte offset, so this is a slot lookup; a
  // `getelementptr` that survived folding has been lowered into arithmetic by
  // selectGEP and its result slot holds the offset.
  return slotFor(V);
}

/// Resolve a pointer to a float-array global: returns the typed slot and, via
/// `IndexSlot`, the slot naming the element index. Walks back through GEPs
/// whose index chain is a constant or a slot; anything else is out of reach.
std::string UDONWriter::floatArrayFor(const Value *Ptr,
                                      const std::string **IndexSlot) {
  *IndexSlot = nullptr;
  const Value *V = Ptr;
  int64_t ConstIndex = 0;
  // A value whose slot already holds an element index (a provenanced cursor
  // load, or a constant address of a typed array) resolves directly.
  auto SlotIt = Slots.find(V);
  if (SlotIt != Slots.end()) {
    auto TA = TypedPointerArrays.find(SlotIt->second);
    if (TA != TypedPointerArrays.end()) {
      *IndexSlot = &SlotIt->second;
      return TA->second;
    }
  }
  // A GEP whose base slot is a typed cursor: the GEP's own result slot holds
  // base + delta in element units (selectGEP's typed-cursor branch), so the
  // result slot IS the index into the same array.
  if (auto *GEP = dyn_cast<GetElementPtrInst>(V))
    if (GEP->getResultElementType() && GEP->getResultElementType()->isFloatTy()) {
      auto BaseSlotIt = Slots.find(GEP->getPointerOperand());
      if (BaseSlotIt != Slots.end()) {
        auto TA = TypedPointerArrays.find(BaseSlotIt->second);
        if (TA != TypedPointerArrays.end()) {
          auto ResSlotIt = Slots.find(GEP);
          if (ResSlotIt != Slots.end()) {
            *IndexSlot = &ResSlotIt->second;
            return TA->second;
          }
        }
      }
    }
  // A constant-expression GEP of a typed array: the element index is the
  // constant offset / 4, and the array is the base.
  if (auto *CE = dyn_cast<ConstantExpr>(V))
    if (CE->getOpcode() == Instruction::GetElementPtr) {
      auto *GEP = cast<GEPOperator>(CE);
      if (auto *BaseG = dyn_cast<GlobalVariable>(GEP->getOperand(0))) {
        auto FA = FloatArraySlots.find(BaseG);
        if (FA != FloatArraySlots.end()) {
          APInt Off(64, 0);
          if (GEP->accumulateConstantOffset(*DL, Off)) {
            int64_t Elem = Off.getSExtValue() / 4;
            if (Elem != 0) {
              std::string Key = "tgepi:" + std::to_string(Elem);
              auto SlotIt = ConstantSlots.find(Key);
              if (SlotIt == ConstantSlots.end()) {
                std::string Name =
                    "__tgepi_" + std::to_string(ConstantSlots.size());
                declareSlot(Name, "SystemInt32");
                DataLines.push_back("    " + Name + ": %SystemInt32, " +
                                    std::to_string(Elem));
                ConstantSlots[Key] = Name;
                SlotIt = ConstantSlots.find(Key);
              }
              *IndexSlot = &SlotIt->second;
            }
            return FA->second;
          }
        }
      }
    }
  const GlobalVariable *G = dyn_cast<GlobalVariable>(V);
  if (!G) {
    // Walk a GEP whose pointer operand is the base and whose index is a
    // constant or a slot. Only the outermost GEP is peeled: selectGEP has
    // already folded the deeper ones into one.
    auto *GEP = dyn_cast<GetElementPtrInst>(V);
    if (!GEP)
      return {};
    const Value *Base = GEP->getPointerOperand();
    SmallVector<const Value *, 4> Idxs;
    for (const Use &U : GEP->indices())
      Idxs.push_back(U.get());
    // The pointee type comes from the GEP's own source type, which opaque
    // pointers keep on the instruction.
    Type *Ty = GEP->getSourceElementType();
    int64_t Off = 0;
    for (size_t K = 0; K < Idxs.size(); K++) {
      if (auto *ST = dyn_cast<StructType>(Ty)) {
        auto *CI = dyn_cast<ConstantInt>(Idxs[K]);
        if (!CI)
          return {};
        unsigned Field = static_cast<unsigned>(CI->getZExtValue());
        // `getStructLayout` returns null for an opaque or zero-sized struct,
        // and the index can name a field the struct does not have; both are
        // reachable from GEPs the frontend emits. Either one means "not a
        // float-array access", not a crash.
        const StructLayout *SL = DL->getStructLayout(ST);
        if (!SL || Field >= ST->getNumElements())
          return {};
        TypeSize SO = SL->getElementOffset(Field);
        if (SO.isScalable())
          return {};
        Off += SO.getFixedValue();
        Ty = ST->getElementType(Field);
        continue;
      }
      if (auto *AT = dyn_cast<ArrayType>(Ty)) {
        TypeSize SzTS = DL->getTypeAllocSize(AT->getElementType());
        if (SzTS.isScalable())
          return {};
        uint64_t Sz = SzTS.getFixedValue();
        auto *CI = dyn_cast<ConstantInt>(Idxs[K]);
        if (CI) {
          Off += CI->getSExtValue() * static_cast<int64_t>(Sz);
          Ty = AT->getElementType();
        } else {
          // Variable index into an array of floats: the index slot IS the
          // element index, scaled only when the element is not 4 bytes.
          if (!AT->getElementType()->isFloatTy())
            return {};
          Ty = AT->getElementType();
          if (K + 1 != Idxs.size())
            return {};
          auto It = Slots.find(Idxs[K]);
          if (It == Slots.end())
            return {};
          *IndexSlot = &It->second;
          ConstIndex = Off / 4;
          Off = 0;
          break;
        }
      }
    }
    if (auto *GV = dyn_cast<GlobalVariable>(Base))
      G = GV;
    ConstIndex += Off / 4;
  }
  if (!G)
    return {};
  auto It = FloatArraySlots.find(G);
  if (It == FloatArraySlots.end())
    return {};
  // A non-zero constant index: `selectGEP`'s typed branch already wrote the
  // constant into the GEP's own slot, so that slot IS the index.
  if (ConstIndex != 0 && !*IndexSlot) {
    auto ResSlotIt = Slots.find(V);
    if (ResSlotIt == Slots.end())
      return {};
    *IndexSlot = &ResSlotIt->second;
  }
  return It->second;
}

void UDONWriter::selectGEP(const GetElementPtrInst &I, UFunction &UF,
                           unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  // A GEP whose base is a typed float-array global produces NO address: the
  // typed array access below works in ELEMENT indices, and the base global
  // itself has no byte offset at all. The element index travels in the GEP's
  // own result slot, which floatArrayFor reads back at the load/store.
  // The base may reach a float-array global through a phi or select of the
  // table pointer (the audio ramping tables do exactly this). Resolve the
  // chain: every incoming must denote the SAME typed array, else stay byte.
  const Value *BaseOp = I.getPointerOperand();
  // A nested GEP of a typed array -- `getelementptr float, ptr
  // (getelementptr([N x float], ptr @g, 0, <const>), i32 %i)`, which is how
  // `gSineTable[i + 1024]` appears after inlining. Peel all-constant element
  // GEPs to the typed global and keep their element offset. Only a CONSTANT
  // outer index is folded here: a variable index would need an extra addition
  // slot allocated mid-selection, and leaving that shape on the byte path is
  // both correct and rare.
  int64_t TypedBaseOffset = 0;
  const Value *Peel = BaseOp;
  for (;;) {
    auto *GEP = dyn_cast<GEPOperator>(Peel);
    if (!GEP || !GEP->hasAllConstantIndices())
      break;
    if (!GEP->getResultElementType() ||
        !GEP->getResultElementType()->isFloatTy())
      break;
    auto *GV = dyn_cast<GlobalVariable>(GEP->getPointerOperand());
    if (!GV || !FloatArraySlots.count(GV))
      break;
    APInt Off(64, 0);
    if (!GEP->accumulateConstantOffset(*DL, Off))
      break;
    TypedBaseOffset += Off.getSExtValue() / 4;
    Peel = GV;
  }
  const GlobalVariable *TypedBase = dyn_cast<GlobalVariable>(Peel);
  if (!TypedBase) {
    if (auto *P = dyn_cast<PHINode>(BaseOp)) {
      const GlobalVariable *Common = nullptr;
      bool Same = true;
      for (const Use &PU : P->incoming_values()) {
        auto *GV = dyn_cast<GlobalVariable>(PU.get());
        if (!GV || !FloatArraySlots.count(GV)) { Same = false; break; }
        if (!Common) Common = GV;
        else if (Common != GV) { Same = false; break; }
      }
      if (Same) TypedBase = Common;
    }
  }
  auto TypedCursor = [&]() -> const std::string * {
    auto It = Slots.find(BaseOp);
    if (It == Slots.end())
      return nullptr;
    auto TA = TypedPointerArrays.find(It->second);
    return TA == TypedPointerArrays.end() ? nullptr : &It->second;
  };
  if (const std::string *BaseSlotP = TypedCursor()) {
    unsigned Last = I.getNumIndices() - 1;
    const Value *LastIdx = I.getOperand(Last + 1);
    std::string Delta;
    if (auto *CI = dyn_cast<ConstantInt>(LastIdx)) {
      int64_t D = CI->getSExtValue();
      if (D == 0) {
        emitCopy(B, *BaseSlotP, slotFor(&I));
        return;
      }
      Delta = i32Constant(static_cast<int32_t>(D));
    } else {
      Delta = slotFor(LastIdx);
    }
    std::string Sum = freshSlot("gepi", Type::getInt32Ty(Mod->getContext()));
    emitExtern(B,
               "SystemInt32.__op_Addition__SystemInt32_SystemInt32__SystemInt32",
               {*BaseSlotP, Delta}, Sum);
    emitCopy(B, Sum, slotFor(&I));
    return;
  }
  if (TypedBase && FloatArraySlots.count(TypedBase)) {
      unsigned Last = I.getNumIndices() - 1;
      const Value *LastIdx = I.getOperand(Last + 1);
      if (auto *CI = dyn_cast<ConstantInt>(LastIdx)) {
        emitCopy(B, i32Constant(static_cast<int32_t>(CI->getSExtValue() +
                                                     TypedBaseOffset)),
                 slotFor(&I));
        return;
      }
      // A variable outer index with a peeled constant offset stays on the
      // byte path: folding it would need an addition slot allocated here.
      if (TypedBaseOffset == 0) {
        emitCopy(B, slotFor(LastIdx), slotFor(&I));
        return;
      }
    }
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
    // A constant base plus a constant offset is itself a constant byte offset:
    // emitting the sum as a literal slot lets every access through it fold its
    // word index and bit offset, which is the difference between one shift
    // extern per field access and none.
    if (std::string K = constantByteOffset(BaseSlot); !K.empty()) {
      uint32_t Sum = static_cast<uint32_t>(std::stoul(K)) +
                     static_cast<uint32_t>(ConstPart);
      emitCopy(B, i32Constant(static_cast<int32_t>(Sum)), slotFor(&I));
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

// --- word-array access ------------------------------------------------------
//
// `__mem` is a `%SystemInt32Array`, but every address in the IR is a BYTE
// offset, because a pointer is a byte offset. Two conversions bridge that:
//
//   word index = p >> 2
//   bit offset = (p & 3) * 8
//
// A full-word store is one `__Set__` and allocates nothing. A load is a
// `__Get__` plus shifts; a value that straddles two words is assembled from
// both, which the identity below verifies:
//
//   bit == 0 :  w0
//   bit != 0 :  ((w0 >>s bit) & ((1 << (32 - bit)) - 1)) | (w1 << (32 - bit))
//
// The mask is needed because Udon has no unsigned right shift: `>>` is
// arithmetic, so the sign bits have to be cleared by hand.

// A byte offset that came from a constant slot: the slot already holds the
// value, so the caller can fold `>> 2` and `& 3` into literal slots instead of
// paying an `EXTERN` for each. Returns "" when the offset is a runtime value.
std::string UDONWriter::constantByteOffset(const std::string &Offset) {
  auto It = SlotConstants.find(Offset);
  if (It == SlotConstants.end())
    return {};
  return It->second;
}

std::string UDONWriter::memWordIndex(const std::string &ByteOffset, UBlock &B) {
  // A constant offset needs no shift: `k >> 2` is a literal index slot. This is
  // the common case for globals and for struct-field GEPs, which is where the
  // bulk of libsm64's accesses come from.
  if (std::string K = constantByteOffset(ByteOffset); !K.empty())
    return i32Constant(static_cast<int32_t>(
        static_cast<uint32_t>(std::stoul(K)) >> 2));
  std::string Index = freshSlot("wi", Type::getInt32Ty(Mod->getContext()));
  emitExtern(B,
             "SystemInt32.__op_RightShift__SystemInt32_SystemInt32__SystemInt32",
             {ByteOffset, i32Constant(2)}, Index);
  return Index;
}

std::string UDONWriter::memBitOffset(const std::string &ByteOffset, UBlock &B) {
  if (std::string K = constantByteOffset(ByteOffset); !K.empty())
    return i32Constant(static_cast<int32_t>(
        (static_cast<uint32_t>(std::stoul(K)) & 3u) << 3));
  std::string Bits = freshSlot("wb", Type::getInt32Ty(Mod->getContext()));
  emitExtern(B,
             "SystemInt32.__op_LogicalAnd__SystemInt32_SystemInt32__SystemInt32",
             {ByteOffset, i32Constant(3)}, Bits);
  std::string Shifted = freshSlot("wbs", Type::getInt32Ty(Mod->getContext()));
  emitExtern(B,
             "SystemInt32.__op_LeftShift__SystemInt32_SystemInt32__SystemInt32",
             {Bits, i32Constant(3)}, Shifted);
  return Shifted;
}

std::string UDONWriter::memGetWord(const std::string &WordIndex, UBlock &B) {
  std::string Word = freshSlot("wv", Type::getInt32Ty(Mod->getContext()));
  emitExtern(B, "SystemInt32Array.__Get__SystemInt32__SystemInt32",
             {"__mem", WordIndex}, Word);
  return Word;
}

void UDONWriter::memSetWord(const std::string &WordIndex,
                            const std::string &Value, UBlock &B) {
  emitExtern(B,
             "SystemInt32Array.__Set__SystemInt32_SystemInt32__SystemVoid",
             {"__mem", WordIndex, Value}, "");
}

std::string UDONWriter::memBinop(StringRef Op, const std::string &A,
                                 const std::string &Bv, StringRef Hint,
                                 UBlock &B) {
  std::string Out = freshSlot(Hint, Type::getInt32Ty(Mod->getContext()));
  emitExtern(B,
             ("SystemInt32.__op_" + Op +
              "__SystemInt32_SystemInt32__SystemInt32").str(),
             {A, Bv}, Out);
  return Out;
}

/// A byte offset into `__mem` is a word index and a bit offset; a store of a
/// full word is exact only when the bit offset is zero. An unaligned store is a
/// read-modify-write of both words it touches.
std::string UDONWriter::memReadUnaligned(const std::string &ByteOffset,
                                         unsigned &Idx, bool ProvablyAligned) {
  UBlock &B = Cur->Blocks[Idx];
  std::string Index = memWordIndex(ByteOffset, B);
  std::string W0 = memGetWord(Index, B);
  std::string Out = freshSlot("ld", Type::getInt32Ty(Mod->getContext()));

  // A provably-aligned access (the IR carries `align >= 4`, which the writer
  // honours for both globals and allocas) IS the word: no bit offset, no
  // branch, no second word. 73% of libsm64's accesses take this path.
  if (ProvablyAligned) {
    emitCopy(B, W0, Out);
    return Out;
  }

  std::string Bit = memBitOffset(ByteOffset, B);

  // `bit == 0` takes the fast path: the word *is* the value. Without the branch
  // the general form would shift by 32, which C# masks to 0.
  std::string IsAligned =
      freshSlot("wal", Type::getInt1Ty(Mod->getContext()));
  emitExtern(B,
             "SystemInt32.__op_Equality__SystemInt32_SystemInt32__SystemBoolean",
             {Bit, i32Constant(0)}, IsAligned);

  std::string Base = freshLabelStem("ld");
  std::string TrueLabel = Base + "_al";
  std::string FalseLabel = Base + "_un";
  std::string JoinLabel = Base + "_j";

  B.Insts.push_back({UInst::PUSH, IsAligned});
  B.Insts.push_back({UInst::JUMP_IF_FALSE, FalseLabel});
  B.Insts.push_back({UInst::JUMP, TrueLabel});

  unsigned T = appendBlock(*Cur, TrueLabel);
  emitCopy(Cur->Blocks[T], W0, Out);
  Cur->Blocks[T].Insts.push_back({UInst::JUMP, JoinLabel});

  unsigned F = appendBlock(*Cur, FalseLabel);
  UBlock &FB = Cur->Blocks[F];
  std::string NextIndex =
      memBinop("Addition", Index, i32Constant(1), "wn", FB);
  std::string W1 = memGetWord(NextIndex, FB);
  std::string InvBits =
      memBinop("Subtraction", i32Constant(32), Bit, "inv", FB);
  // Low half: arithmetic shift right, then mask to the bits that survived.
  std::string Low = memBinop("RightShift", W0, Bit, "lo", FB);
  std::string Mask = memBinop("LeftShift", i32Constant(-1), InvBits, "msk", FB);
  // `~x` is `x ^ -1`: Udon exposes no bitwise NOT operator on any type, so the
  // complement has to be spelled as an XOR with all ones.
  std::string NotMask =
      freshSlot("nmk", Type::getInt32Ty(Mod->getContext()));
  emitExtern(FB,
             "SystemInt32.__op_LogicalXor__SystemInt32_SystemInt32__SystemInt32",
             {Mask, i32Constant(-1)}, NotMask);
  std::string LowMasked = memBinop("LogicalAnd", Low, NotMask, "lm", FB);
  // High half: the next word's low bits, moved into place.
  std::string High = memBinop("LeftShift", W1, InvBits, "hi", FB);
  std::string Combined = memBinop("LogicalOr", LowMasked, High, "or", FB);
  emitCopy(FB, Combined, Out);
  FB.Insts.push_back({UInst::JUMP, JoinLabel});

  // The join is a jump *target*, so it must not also be the block the caller
  // continues filling: that would put the caller's instructions after the
  // branch that reaches it. Append the join, then a separate continuation.
  unsigned JoinIdx = appendBlock(*Cur, JoinLabel);
  Cur->Blocks[JoinIdx].Insts.push_back({UInst::NOP, {}});
  Idx = appendBlock(*Cur, freshLabelStem("ldc"));
  return Out;
}

/// Store a full word at BYTE offset `ByteOffset`. Aligned is one `__Set__`;
/// unaligned is a read-modify-write of the two words it spans.
void UDONWriter::memWriteUnaligned(const std::string &ByteOffset,
                                   const std::string &Value, unsigned &Idx,
                                   bool ProvablyAligned) {
  UBlock &B = Cur->Blocks[Idx];
  std::string Index = memWordIndex(ByteOffset, B);

  // Aligned: the word is exactly the value -- one `__Set__`, no diamond.
  if (ProvablyAligned) {
    memSetWord(Index, Value, B);
    return;
  }

  std::string Bit = memBitOffset(ByteOffset, B);

  std::string IsAligned =
      freshSlot("wal", Type::getInt1Ty(Mod->getContext()));
  emitExtern(B,
             "SystemInt32.__op_Equality__SystemInt32_SystemInt32__SystemBoolean",
             {Bit, i32Constant(0)}, IsAligned);

  std::string Base = freshLabelStem("st");
  std::string TrueLabel = Base + "_al";
  std::string FalseLabel = Base + "_un";
  std::string JoinLabel = Base + "_j";

  B.Insts.push_back({UInst::PUSH, IsAligned});
  B.Insts.push_back({UInst::JUMP_IF_FALSE, FalseLabel});
  B.Insts.push_back({UInst::JUMP, TrueLabel});

  unsigned T = appendBlock(*Cur, TrueLabel);
  memSetWord(Index, Value, Cur->Blocks[T]);
  Cur->Blocks[T].Insts.push_back({UInst::JUMP, JoinLabel});

  unsigned F = appendBlock(*Cur, FalseLabel);
  UBlock &FB = Cur->Blocks[F];
  std::string NextIndex =
      memBinop("Addition", Index, i32Constant(1), "wn", FB);
  // A 32-bit value at bit offset `b` spans two words. The `b` bits BELOW the
  // insertion point in the low word, and everything from `b` up in the high
  // word, are kept; the 32 bits starting at `b` are replaced.
  //
  //   lowKeep  = (1 << b) - 1
  //   W0' = (W0 & lowKeep)  | (Value << b)
  //   W1' = (W1 & ~lowKeep) | ((Value >>> (32 - b)) & lowKeep)
  //
  // The high insert is masked with `lowKeep` because `Value >>> (32 - b)` is an
  // ARITHMETIC shift: its top bits are copies of the sign bit and would
  // corrupt the kept region if ORed in raw.
  std::string One = i32Constant(1);
  std::string Shifted = memBinop("LeftShift", One, Bit, "sh", FB);
  std::string LowKeep = memBinop("Subtraction", Shifted, One, "lk", FB);
  std::string HighKeep = freshSlot("hk", Type::getInt32Ty(Mod->getContext()));
  emitExtern(FB,
             "SystemInt32.__op_LogicalXor__SystemInt32_SystemInt32__SystemInt32",
             {LowKeep, i32Constant(-1)}, HighKeep);
  std::string InvBits =
      memBinop("Subtraction", i32Constant(32), Bit, "inv", FB);

  // Low word.
  std::string W0 = memGetWord(Index, FB);
  std::string KeptLow = memBinop("LogicalAnd", W0, LowKeep, "kl", FB);
  std::string ValLow = memBinop("LeftShift", Value, Bit, "vl", FB);
  std::string NewLow = memBinop("LogicalOr", KeptLow, ValLow, "nl", FB);
  memSetWord(Index, NewLow, FB);

  // High word.
  std::string W1 = memGetWord(NextIndex, FB);
  std::string KeptHigh = memBinop("LogicalAnd", W1, HighKeep, "kh", FB);
  std::string ValHighRaw = memBinop("RightShift", Value, InvBits, "vh", FB);
  std::string ValHigh = memBinop("LogicalAnd", ValHighRaw, LowKeep, "vhm", FB);
  std::string NewHigh = memBinop("LogicalOr", KeptHigh, ValHigh, "nh", FB);
  memSetWord(NextIndex, NewHigh, FB);
  FB.Insts.push_back({UInst::JUMP, JoinLabel});

  unsigned JoinIdx = appendBlock(*Cur, JoinLabel);
  Cur->Blocks[JoinIdx].Insts.push_back({UInst::NOP, {}});
  Idx = appendBlock(*Cur, freshLabelStem("stc"));
}

std::string UDONWriter::emitLoad(unsigned &Idx, const std::string &Offset,
                                 Type *Ty, unsigned Align) {
  // `Idx` moves when a word read splits the block to handle an unaligned
  // offset, so the current block is fetched through this helper at each use.
  // A `UBlock &` would NOT work: assigning to it copies the block instead of
  // rebinding, silently overwriting one block with another.
  auto cur = [&]() -> UBlock & { return Cur->Blocks[Idx]; };

  // Aligned when the IR's own guarantee covers the access width. `Align == 0`
  // (no attribute) stays on the general path.
  unsigned Width =
      static_cast<unsigned>(DL->getTypeStoreSize(Ty));
  bool Aligned = Align >= Width && Align >= 4;

  // A word read is exact for i32 and, through the reinterpret externs, for
  // float. i64 and double need two words assembled.
  if (Ty->isIntegerTy(32) || Ty->isPointerTy())
    return memReadUnaligned(Offset, Idx, Aligned);

  if (Ty->isIntegerTy(64) || Ty->isDoubleTy()) {
    // Low word, then the high word at byte offset + 4. The second word is
    // always 4 bytes past a 8-aligned base, so it inherits the proof.
    std::string Lo = memReadUnaligned(Offset, Idx, Aligned);
    std::string Four = memBinop("Addition", Offset, i32Constant(4), "p4", cur());
    std::string Hi = memReadUnaligned(Four, Idx, Aligned);
    Type *I64 = Type::getInt64Ty(Mod->getContext());
    std::string Lo64 = freshSlot("lo64", I64);
    emitExtern(cur(), "SystemConvert.__ToInt64__SystemInt32__SystemInt64", {Lo},
               Lo64);
    std::string Masked = freshSlot("lo64m", I64);
    emitExtern(cur(),
               "SystemInt64.__op_LogicalAnd__SystemInt64_SystemInt64__SystemInt64",
               {Lo64, constantSlot(ConstantInt::get(I64, 0xFFFFFFFFull))},
               Masked);
    std::string Hi64 = freshSlot("hi64", I64);
    emitExtern(cur(), "SystemConvert.__ToInt64__SystemInt32__SystemInt64", {Hi},
               Hi64);
    std::string Shifted = freshSlot("hi64s", I64);
    emitExtern(cur(),
               "SystemInt64.__op_LeftShift__SystemInt64_SystemInt64__SystemInt64",
               {Hi64, constantSlot(ConstantInt::get(I64, 32))}, Shifted);
    std::string Joined = freshSlot("j64", I64);
    emitExtern(cur(),
               "SystemInt64.__op_LogicalOr__SystemInt64_SystemInt64__SystemInt64",
               {Masked, Shifted}, Joined);

    if (Ty->isIntegerTy(64))
      return Joined;

    // double: reinterpret the assembled bits.
    std::string Out = freshSlot("ld", Ty);
    emitExtern(cur(),
               "SystemBitConverter.__Int64BitsToDouble__SystemInt64__SystemDouble",
               {Joined}, Out);
    return Out;
  }

  if (Ty->isFloatTy()) {
    std::string Bits = memReadUnaligned(Offset, Idx, Aligned);
    std::string Out = freshSlot("ld", Ty);
    emitExtern(cur(),
               "SystemBitConverter.__Int32BitsToSingle__SystemInt32__SystemSingle",
               {Bits}, Out);
    return Out;
  }

  if (Ty->isIntegerTy(1)) {
    // A bool occupies one byte holding 0 or 1. Read the byte, then convert:
    // the destination slot is a `SystemBoolean`, and copying an int into it
    // would be a slot type mismatch.
    std::string Bits =
        emitLoad(Idx, Offset, Type::getInt8Ty(Mod->getContext()), Align);
    std::string Out = freshSlot("ldi1", Ty);
    emitExtern(cur(), "SystemConvert.__ToBoolean__SystemInt32__SystemBoolean",
               {Bits}, Out);
    return Out;
  }

  if (Ty->isIntegerTy(8) || Ty->isIntegerTy(16)) {
    // Sign-extend by moving the field's sign bit to bit 31 and back down.
    //
    // The mask that used to precede the left shift is redundant: `(x << 24)`,
    // where Udon's `<<` is a 32-bit operation, already discards every bit
    // above the low byte, so `(x & 0xFF) << 24 >> 24` and `x << 24 >> 24`
    // produce the same value for ANY x. One extern fewer per sub-word load.
    //
    // Reading at the *byte* offset (not the containing word) is what makes a
    // field that straddles two words correct: the left shift brings the
    // Reading at the *byte* offset (not the containing word) is what makes a
    // field that straddles two words correct. Verified by `test/subword.ll`,
    // which covers a byte at offset 3 of a word.
    unsigned Width = Ty->getIntegerBitWidth();
    int32_t Shift = Width == 8 ? 24 : 16;

    // A sub-word access reads the containing word at its own byte offset.
    //
    // A word cache was tried here: track `base + delta` for pointer slots,
    // reuse a word another access in the block already read, and shift the
    // field out. It is NOT sound as written and was reverted. `memReadUnaligned`
    // at an unaligned byte offset already returns the field's bits positioned
    // at the bottom of the result, so an extra shift by `delta & 3` corrupts
    // it; and reading `base + (delta & ~3)` is only word-aligned when `base`
    // is, which is not known here. Measured failure: `report` returned
    // 2500000 instead of 2482752, i.e. the whole display list went wrong.
    // Doing this correctly needs the read to happen at a genuinely aligned
    // address and the field's in-word position to come from that address, not
    // from a delta relative to an arbitrary base.
    std::string Word = memReadUnaligned(Offset, Idx, Align >= Width && Width >= 2);

    std::string Left =
        memBinop("LeftShift", Word, i32Constant(Shift), "ldl", cur());
    return memBinop("RightShift", Left, i32Constant(Shift), "ldr", cur());
  }

  std::string TyText;
  raw_string_ostream TyOS(TyText);
  Ty->print(TyOS);
  reportUDONError("load of an unsupported type: " + TyOS.str());
}

void UDONWriter::emitStore(unsigned &Idx, const std::string &Value,
                           const std::string &Offset, Type *Ty,
                           unsigned Align) {
  // Refetched at each use: a word write may split the block, which moves the
  // continuation. Assigning to a `UBlock &` would copy a block, not rebind.
  auto cur = [&]() -> UBlock & { return Cur->Blocks[Idx]; };

  unsigned Width = static_cast<unsigned>(DL->getTypeStoreSize(Ty));
  bool Aligned = Align >= Width && Align >= 4;

  if (Ty->isIntegerTy(32) || Ty->isPointerTy()) {
    memWriteUnaligned(Offset, Value, Idx, Aligned);
    return;
  }

  if (Ty->isIntegerTy(64) || Ty->isDoubleTy()) {
    Type *I64 = Type::getInt64Ty(Mod->getContext());
    std::string Bits = Value;
    if (Ty->isDoubleTy()) {
      Bits = freshSlot("st_bits", I64);
      emitExtern(cur(),
                 "SystemBitConverter.__DoubleToInt64Bits__SystemDouble__SystemInt64",
                 {Value}, Bits);
    }
    std::string Lo64 = freshSlot("st_lo", I64);
    emitExtern(cur(),
               "SystemInt64.__op_LogicalAnd__SystemInt64_SystemInt64__SystemInt64",
               {Bits, constantSlot(ConstantInt::get(I64, 0xFFFFFFFFull))}, Lo64);
    std::string Lo = emitTruncI64ToI32(Lo64, cur());
    std::string Hi64 = freshSlot("st_hi", I64);
    emitExtern(cur(),
               "SystemInt64.__op_RightShift__SystemInt64_SystemInt64__SystemInt64",
               {Bits, constantSlot(ConstantInt::get(I64, 32))}, Hi64);
    std::string Hi = emitTruncI64ToI32(Hi64, cur());

    memWriteUnaligned(Offset, Lo, Idx, Aligned);
    std::string Four =
        memBinop("Addition", Offset, i32Constant(4), "p4", cur());
    memWriteUnaligned(Four, Hi, Idx, Aligned);
    return;
  }

  if (Ty->isFloatTy()) {
    std::string Bits =
        freshSlot("st_bits", Type::getInt32Ty(Mod->getContext()));
    emitExtern(cur(),
               "SystemBitConverter.__SingleToInt32Bits__SystemSingle__SystemInt32",
               {Value}, Bits);
    memWriteUnaligned(Offset, Bits, Idx, Aligned);
    return;
  }

  if (Ty->isIntegerTy(1)) {
    // Store the bool as the one byte a bool occupies: convert to an int and
    // take the byte path, which merges the byte into the surrounding word.
    std::string AsInt =
        freshSlot("sti1", Type::getInt32Ty(Mod->getContext()));
    emitExtern(cur(), "SystemConvert.__ToInt32__SystemBoolean__SystemInt32",
               {Value}, AsInt);
    emitStore(Idx, AsInt, Offset, Type::getInt8Ty(Mod->getContext()), 1);
    return;
  }

  if (Ty->isIntegerTy(8) || Ty->isIntegerTy(16)) {
    // Read the four bytes starting at `Offset`, replace the low one or two,
    // and write them back. Working at the *byte* offset rather than on the
    // containing word is what makes a field that straddles two words correct:
    // an i16 at byte offset 3 spans bytes 3..4, which are in different words,
    // and a single-word mask cannot reach both.
    unsigned Width = Ty->getIntegerBitWidth();
    uint32_t Take = Width == 8 ? 0xFFu : 0xFFFFu;
    uint32_t Keep = ~Take;
    std::string Old = memReadUnaligned(Offset, Idx, Align >= Width && Width >= 2);
    std::string Kept = memBinop("LogicalAnd", Old,
                                i32Constant(static_cast<int32_t>(Keep)), "k",
                                cur());
    std::string Taken =
        memBinop("LogicalAnd", Value, i32Constant(static_cast<int32_t>(Take)),
                 "t", cur());
    std::string Merged = memBinop("LogicalOr", Kept, Taken, "m", cur());
    memWriteUnaligned(Offset, Merged, Idx, Align >= Width && Width >= 2);
    return;
  }

  std::string TyText;
  raw_string_ostream TyOS(TyText);
  Ty->print(TyOS);
  reportUDONError("store of an unsupported type: " + TyOS.str());
}

void UDONWriter::selectLoad(const LoadInst &I, UFunction &UF,
                           unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  // A promoted global's slot IS the value: no memory access at all. Both slots
  // come from the same LLVM type, so this is a plain copy -- and the slot
  // allocator usually coalesces it away entirely.
  if (auto *G = dyn_cast<GlobalVariable>(I.getPointerOperand()))
    if (std::string S = promotedSlot(*G); !S.empty()) {
      emitCopy(B, S, slotFor(&I));
      return;
    }
  // A load of a provenanced pointer global, or of a struct field the
  // analysis proved, yields an ELEMENT INDEX: the array is recorded against
  // the load's own slot so a later `*p = v` or `p[i]` resolves.
  if (I.getType()->isPointerTy()) {
    if (auto TP = ProvenancedLoads.find(&I); TP != ProvenancedLoads.end()) {
      std::string Offset = pointerOffset(I.getPointerOperand(), B);
      std::string Word =
          emitLoad(Idx, Offset, Type::getInt32Ty(Mod->getContext()), 4);
      std::string S = slotFor(&I);
      emitCopy(UF.Blocks[Idx], Word, S);
      TypedPointerArrays[S] = FloatArraySlots.lookup(TP->second);
      return;
    }
    if (auto *G = dyn_cast<GlobalVariable>(I.getPointerOperand()))
      if (auto TPG = TypedPointers.find(G); TPG != TypedPointers.end()) {
        std::string Offset = pointerOffset(G, B);
        std::string Word =
            emitLoad(Idx, Offset, Type::getInt32Ty(Mod->getContext()), 4);
        std::string S = slotFor(&I);
        emitCopy(UF.Blocks[Idx], Word, S);
        TypedPointerArrays[S] = FloatArraySlots.lookup(TPG->second);
        return;
      }
  }
  // A float load through a typed float-array global is ONE `__Get__`: the
  // typed path needs no word-index shift and no bitcast. `selectGEP` put the
  // element index into the load's own slot when the base is a float-array
  // global, so this resolves the pointer, not an address.
  if (I.getType()->isFloatTy()) {
    const std::string *IdxSlot = nullptr;
    if (std::string Arr = floatArrayFor(I.getPointerOperand(), &IdxSlot);
        !Arr.empty()) {
      std::string Index = IdxSlot ? *IdxSlot : i32Constant(0);
      std::string V =
          freshSlot("f32ld", Type::getFloatTy(Mod->getContext()));
      emitExtern(B, "SystemSingleArray.__Get__SystemInt32__SystemSingle",
                 {Arr, Index}, V);
      emitCopy(B, V, slotFor(&I));
      return;
    }
  }
  std::string Offset = pointerOffset(I.getPointerOperand(), B);
  std::string Value = emitLoad(Idx, Offset, I.getType(), I.getAlign().value());
  emitCopy(UF.Blocks[Idx], Value, slotFor(&I));
  // A narrow load's result is already sign-extended. Record the slot the load
  // NAMES (`slotFor(&I)`), not `emitLoad`'s temporary: the copy above is what
  // defines it, and that slot is what a later `sitofp` reads.
  if (I.getType()->isIntegerTy(8) || I.getType()->isIntegerTy(16))
    SignExtendedSlots.insert(slotFor(&I));
}

void UDONWriter::selectStore(const StoreInst &I, UFunction &UF,
                            unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  if (auto *G = dyn_cast<GlobalVariable>(I.getPointerOperand()))
    if (std::string S = promotedSlot(*G); !S.empty()) {
      emitCopy(B, slotFor(I.getValueOperand()), S);
      return;
    }
  // A pointer store into a provenanced pointer global keeps an ELEMENT INDEX
  // in the global's word: `s_trianglePtr = ud_geo + 9216` becomes a store of
  // the index 9216.
  if (I.getValueOperand()->getType()->isPointerTy())
    if (auto *G = dyn_cast<GlobalVariable>(I.getPointerOperand()))
      if (auto TP = TypedPointers.find(G); TP != TypedPointers.end()) {
        const std::string *SrcIdx = nullptr;
        std::string Arr = floatArrayFor(I.getValueOperand(), &SrcIdx);
        std::string Want = FloatArraySlots.lookup(TP->second);
        if (!Arr.empty() && Arr == Want) {
          std::string Offset = pointerOffset(G, B);
          if (getenv("UDON_DBG_TPTR"))
            errs() << "tptr-store " << G->getName() << " off=" << Offset
                   << " val=" << (SrcIdx ? *SrcIdx : "<0>") << "\n";
          emitStore(Idx, SrcIdx ? *SrcIdx : i32Constant(0), Offset,
                    Type::getInt32Ty(Mod->getContext()), 4);
          return;
        }
      }
  // The store half of the same shape: one `__Set__`, no bitcast.
  if (I.getValueOperand()->getType()->isFloatTy()) {
    const std::string *IdxSlot = nullptr;
    if (std::string Arr = floatArrayFor(I.getPointerOperand(), &IdxSlot);
        !Arr.empty()) {
      std::string Index = IdxSlot ? *IdxSlot : i32Constant(0);
      emitExtern(B,
                 "SystemSingleArray.__Set__SystemInt32_SystemSingle__SystemVoid",
                 {Arr, Index, slotFor(I.getValueOperand())}, "");
      return;
    }
  }
  std::string Offset = pointerOffset(I.getPointerOperand(), B);
  emitStore(Idx, slotFor(I.getValueOperand()), Offset,
            I.getValueOperand()->getType(), I.getAlign().value());
}
