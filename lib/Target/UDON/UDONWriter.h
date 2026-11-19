//===-- UDONWriter.h - Library for converting LLVM code to UDON --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the UDONWriter that is used by the UDON backend.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_UDON_UDONWRITER_H
#define LLVM_LIB_TARGET_UDON_UDONWRITER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace llvm {

class AllocaInst;
class BasicBlock;
class CallInst;
class Constant;
class Function;
class GetElementPtrInst;
class GlobalVariable;
class FCmpInst;
class ICmpInst;
class Instruction;
class LoadInst;
class PHINode;
class SelectInst;
class StoreInst;
class Type;
class Value;

/// Options that decide the shape of the emitted program, taken from the driver
/// and immutable once the writer runs.
struct UDONOptions {
  /// End of the globals image: the byte offset where the shadow stack begins.
  uint64_t MemoryBytes = 1u << 20;
  /// Bytes of the `malloc` heap, between the globals image and the shadow
  /// stack. Zero means no heap: a program that never calls `malloc` pays
  /// nothing.
  uint64_t HeapBytes = 0;
  /// Bytes of alloca shadow stack above the heap.
  uint64_t ShadowStackBytes = 1u << 16;
  /// Entries per `__stack_*` caller-save array.
  uint64_t StackDepth = 1024;
  /// Heap slot count; 0 means "computed from the module".
  uint64_t HeapSlots = 0;
  /// Link the embedded runtime library.
  bool LinkRuntime = true;
  /// Export every internal label with a `__` prefix, for the lit suite.
  bool ExportLabels = false;
  /// Export `__mem` itself as a program variable so a host can read and write
  /// the memory through a named slot instead of importing offsets.
  bool ExportMemory = false;
  /// Path of the used-extern manifest, empty for none.
  std::string ManifestPath;
  /// Path of the runtime IR to link, empty for none.
  std::string RuntimePath;
};

/// One UASM instruction.
///
/// Sizes are measured, not assumed: NOP/POP/COPY are 4 bytes,
/// PUSH/JUMP/JUMP_IF_FALSE/JUMP_INDIRECT/EXTERN are 8. The writer lays out its
/// own functions, so every code address it emits is this arithmetic.
struct UInst {
  enum Opcode {
    NOP,
    POP,
    COPY,
    PUSH,
    JUMP,
    JUMP_IF_FALSE,
    JUMP_INDIRECT,
    EXTERN,
  };

  Opcode Op;
  std::string Operand;

  unsigned size() const {
    switch (Op) {
    case NOP:
    case POP:
    case COPY:
      return 4;
    default:
      return 8;
    }
  }

  const char *mnemonic() const {
    switch (Op) {
    case NOP: return "NOP";
    case POP: return "POP";
    case COPY: return "COPY";
    case PUSH: return "PUSH";
    case JUMP: return "JUMP";
    case JUMP_IF_FALSE: return "JUMP_IF_FALSE";
    case JUMP_INDIRECT: return "JUMP_INDIRECT";
    case EXTERN: return "EXTERN";
    }
    return "?";
  }
};

/// A labelled run of instructions. Labels occupy no bytes, so a block's address
/// is its first instruction's address.
struct UBlock {
  std::string Label;
  std::vector<UInst> Insts;
  uint32_t Address = 0;

  unsigned size() const {
    unsigned Total = 0;
    for (const UInst &I : Insts)
      Total += I.size();
    return Total;
  }
};

/// One function's emitted form.
struct UFunction {
  const Function *F = nullptr;
  std::string BodyLabel;
  /// A deque, not a vector: selection holds a `UBlock &` across `appendBlock`,
  /// and a vector's growth would invalidate it.
  std::deque<UBlock> Blocks;
  /// Basic block -> index in `Blocks`.
  DenseMap<const BasicBlock *, unsigned> BlockIndex;
  /// Trampoline block per (predecessor, successor) edge. Keyed by the PAIR, not
  /// by the successor: a loop header has two predecessors, and sharing one
  /// trampoline between them would run the preheader's phi copies on the back
  /// edge -- zeroing the induction variable so the loop never advances.
  DenseMap<std::pair<const BasicBlock *, const BasicBlock *>, unsigned>
      EdgeIndex;
  uint32_t Address = 0;
};

/// Emits a whole module as UASM text plus a heap image.
///
/// UASM needs its `.data_start` block before any code, and the addresses that
/// block names are decided by the code layout, so this is a ModulePass that
/// selects everything into buffers first and only then writes.
class UDONWriter : public ModulePass {
public:
  static char ID;

  UDONWriter(raw_pwrite_stream &Out, const UDONOptions &Opts);

  /// Number of heap slots the last emitted module needs.
  uint64_t slotCount() const { return SlotCount; }

  /// The heap-image JSON written beside the `.uasm` on the last run.
  StringRef heapImage() const { return HeapImage; }

  bool runOnModule(Module &M) override;

  // --- module-wide state ---------------------------------------------------

  Module *Mod = nullptr;
  const DataLayout *DL = nullptr;
  UDONOptions Opts;

  /// A deque, not a vector: `Cur` points into this container while the next
  /// function is being selected, and a vector's growth would invalidate it --
  /// every later write would land in freed memory.
  std::deque<UFunction> Functions;

  /// Position of each basic block within the module. Labels are built from this
  /// rather than from the block's name: clang names almost nothing, so a
  /// name-based label collapses to `<fn>_` for every unnamed block.
  DenseMap<const BasicBlock *, unsigned> BlockNumbers;

  /// `.data_start` literal slots whose value is known at selection time.
  std::vector<std::string> DataLines;

  /// `(slot, label)` pairs resolved after layout: `PUSH` accepts data symbols
  /// only, so a code address can only travel through a slot.
  std::vector<std::pair<std::string, std::string>> PendingAddressSlots;

  /// Heap-image entries, one JSON fragment each.
  std::vector<std::string> HeapEntries;

  /// Slots declared `null` in `.data_start` and written by the host or by init.
  std::vector<std::pair<std::string, std::string>> NullSlots;

  /// Export name -> label.
  std::vector<std::pair<std::string, std::string>> CodeExports;

  /// Data symbols to `.export` in `.data_start`, in declaration order.
  std::vector<std::string> DataExports;

  /// Every extern signature emitted, for the manifest.
  StringSet<> UsedExterns;

  /// Symbol -> Udon type string, for every slot the writer declares.
  StringMap<std::string> SlotTypes;

  /// Constants already interned, keyed by `type:bits`.
  StringMap<std::string> ConstantSlots;

  /// Slot -> the decimal value it holds, for `SystemInt32` constant slots.
  /// Populated by `i32Constant`, read by `constantByteOffset`: an access at a
  /// constant offset folds its `>> 2` into a literal index slot instead of
  /// emitting a shift extern.
  StringMap<std::string> SlotConstants;

  /// Slots that already hold a SIGN-EXTENDED narrow value: the result of an
  /// `i8`/`i16` load, which `emitLoad` widens with `<< 24 >> 24`. A `sitofp`
  /// of such a value needs no second widening -- the shifts are the only
  /// reason it pays two externs per byte, and this is 55% of a tick.
  StringSet<> SignExtendedSlots;

  /// Label -> address, filled by layout().
  StringMap<uint32_t> LabelAddresses;

  /// Global variable -> byte offset in `__mem`.
  DenseMap<const GlobalVariable *, uint32_t> GlobalOffsets;

  /// `__data_image` contents, indexed by `__mem` offset.
  std::vector<uint8_t> DataImage;

  /// The same image packed into little-endian words, which is what the heap
  /// image actually carries.
  std::vector<uint32_t> DataImageWords;

  /// Function -> its shadow-stack frame size.
  DenseMap<const Function *, uint32_t> FrameSizes;

  /// Alloca -> its offset within its function's frame.
  DenseMap<const AllocaInst *, uint32_t> AllocaOffsets;

  /// Functions that are address-taken, so their calls go through `__fn_table`.
  DenseMap<const Function *, uint32_t> FunctionPointerIndices;

  /// Functions that may be re-entered while running: in a recursive SCC,
  /// address-taken, or able to reach an extern that can run Udon code. Their
  /// calls need the caller-save discipline.
  DenseSet<const Function *> RecursiveFunctions;

  /// Build `RecursiveFunctions` from the IR.
  void buildCallGraph();

  /// The array type a slot's type spills through.
  std::string spillArrayType(StringRef UdonType) const;

  /// The typed stack a slot's type spills to.
  std::string spillStackFor(StringRef UdonType) const;

  /// The cursor slot for a spill stack.
  std::string spillCursorFor(StringRef UdonType) const;

  /// Spill `Slot` onto its typed stack, returning the continuation block.
  unsigned emitSpill(unsigned BlockIdx, const std::string &Slot,
                     StringRef UdonType);

  /// Restore `Slot` from its typed stack.
  void emitUnspill(UBlock &B, const std::string &Slot, StringRef UdonType);

  /// Every slot that is live across the call at `I`, in a stable order.
  std::vector<std::pair<std::string, std::string>>
  liveSlotsAt(const Instruction &I) const;

  // --- per-function selection state ---------------------------------------

  /// The function being selected.
  UFunction *Cur = nullptr;
  /// Value -> slot name, for the function being selected.
  DenseMap<const Value *, std::string> Slots;
  /// Per-function temporary counter, so names are stable across runs.
  unsigned TempCounter = 0;
  /// Program-wide counter for split-block labels. A split's label is used as a
  /// jump target as well as a label, so it must be unique by construction
  /// rather than deduplicated afterwards.
  unsigned LabelCounter = 0;


  // --- slot and constant machinery ----------------------------------------

  /// Declare a slot; a conflicting type is a writer bug.
  void declareSlot(StringRef Name, StringRef UdonType);

  /// The slot name for a value, allocating one on first use.
  std::string slotFor(const Value *V);

  /// The slot naming an AGGREGATE value's field. Aggregates are lowered by
  /// scalarisation: `{ i32, float }` becomes one `%SystemInt32` and one
  /// `%SystemSingle` slot, named `<agg>_<index>`. `slotFor` on an aggregate
  /// returns the prefix, which is never emitted; only fields are real slots.
  std::string aggFieldSlot(const Value *Agg, unsigned Index, Type *FieldTy);
  /// True when the type is a scalar the writer can name; an aggregate is not.
  bool isScalarUdonType(Type *Ty) const;

  /// A fresh temporary slot of the given type, named after `Hint`.
  std::string freshSlot(StringRef Hint, Type *Ty);

  /// A fresh temporary of the same type as `V`.
  std::string freshSlot(const Value *V, StringRef Hint) {
    return freshSlot(Hint, V->getType());
  }

  /// The slot holding a constant, creating the literal or heap entry it needs.
  std::string constantSlot(const Constant *C);

  /// If `C` denotes an address whose `__mem` byte offset is known at compile
  /// time, set `Out` and return true. Covers a global, null, a constant
  /// `getelementptr` chain, and the casts that preserve an address.
  bool constantAddress(const Constant *C, int64_t &Out) const;

  /// If `C` denotes a function, through any casts, set `Out` to its
  /// `__fn_table` index.
  bool constantFunctionIndex(const Constant *C, uint64_t &Out) const;

  /// Serialize `C` into `__data_image` at `Offset`, a `__mem` byte offset.
  void writeConstantBytes(const Constant *C, uint32_t Offset);

  /// A `SystemUInt32` literal slot holding `Address`.
  std::string addressSlot(StringRef Name, uint32_t Address);

  /// A `SystemUInt32` literal slot holding the address of `Label`.
  std::string addressSlotForLabel(StringRef Name, StringRef Label);

  /// A `SystemInt32` literal slot holding `V`.
  std::string i32Constant(int32_t V);

  /// A `SystemBoolean` slot holding `V`.
  std::string boolConstant(bool V);

  /// The Udon type string for an LLVM type, empty when it has none.
  std::string udonTypeOf(Type *Ty) const;

  // --- emission -----------------------------------------------------------

  /// Emit one extern call: push each source, push the destination when the
  /// signature returns a value, then `EXTERN`.
  void emitExtern(UBlock &B, StringRef Signature,
                  ArrayRef<std::string> Sources,
                  const std::string &Destination);

  /// Emit `PUSH src; PUSH dst; COPY`.
  void emitCopy(UBlock &B, StringRef Src, StringRef Dst);

  /// `((v + 0x80000000) & 0xFFFFFFFF) - 0x80000000`, then `Convert.ToInt32`.
  /// Never a bare `Convert`, which throws for an unsigned value above
  /// Int32.MaxValue.
  std::string emitTruncI64ToI32(const std::string &Src, UBlock &B);

  /// `__mem` offset of a pointer value, as a slot name.
  std::string pointerOffset(const Value *V, UBlock &B);

  /// Load `Ty` from `__mem` at `Offset`, returning the value's slot. Takes the
  /// block index because a word-array read may split the block.
  ///
  /// `Align` is the IR's own `align` for this access. When it is at least the
  /// access width, the address is provably word-aligned and the whole
  /// alignment diamond collapses to one array read -- the writer honours
  /// `align` for globals (`layOutGlobals`) and allocas (`layOutFrames`), so
  /// the attribute is trustworthy. `Align == 0` means "unknown" and takes the
  /// general path.
  std::string emitLoad(unsigned &Idx, const std::string &Offset, Type *Ty,
                       unsigned Align = 0);

  /// Store `Value` into `__mem` at `Offset`. `Align` as for `emitLoad`.
  void emitStore(unsigned &Idx, const std::string &Value,
                 const std::string &Offset, Type *Ty, unsigned Align = 0);

  // --- word-array access ---------------------------------------------------
  //
  // `__mem` is a `%SystemInt32Array` but addresses are byte offsets, so each
  // access converts: word index is `p >> 2`, bit offset is `(p & 3) * 8`.

  std::string memWordIndex(const std::string &ByteOffset, UBlock &B);
  std::string memBitOffset(const std::string &ByteOffset, UBlock &B);
  std::string memGetWord(const std::string &WordIndex, UBlock &B);
  void memSetWord(const std::string &WordIndex, const std::string &Value,
                  UBlock &B);
  /// The constant `ByteOffset` denotes, or an empty string when it is not a
  /// compile-time constant. Lets an access fold `>> 2` into a literal index
  /// slot instead of paying an `EXTERN` for it.
  std::string constantByteOffset(const std::string &Offset);
  std::string memBinop(StringRef Op, const std::string &A,
                       const std::string &Bv, StringRef Hint, UBlock &B);
  /// A 32-bit read at a possibly-unaligned byte offset. Takes the block INDEX
  /// and updates it, because the split moves the continuation: a `UBlock&`
  /// cannot be rebound for the caller.
  ///
  /// `ProvablyAligned` skips the alignment diamond entirely: the word IS the
  /// value. Set from the IR's own `align` attribute, which the writer honours
  /// for both globals and allocas.
  std::string memReadUnaligned(const std::string &ByteOffset, unsigned &Idx,
                               bool ProvablyAligned = false);
  /// A 32-bit write at a possibly-unaligned byte offset. Same reason.
  void memWriteUnaligned(const std::string &ByteOffset,
                         const std::string &Value, unsigned &Idx,
                         bool ProvablyAligned = false);

  // --- scalar global promotion --------------------------------------------
  //
  // A scalar global whose address is never taken has no reason to live in
  // `__mem`: it can be a typed heap slot, which turns every load into a
  // reference and every store into a COPY. Measured on libsm64: 140 of 1081
  // globals qualify, covering 1388 load/store sites.
  //
  /// The promoted globals, mapped to the slot that holds them.
  DenseMap<const GlobalVariable *, std::string> PromotedSlots;
  /// Classifies every defined global: eligible or must stay in memory.
  void classifyGlobals();
  /// The typed slot a promoted global lives in, or "" if it is not promoted.
  std::string promotedSlot(const GlobalVariable &G);
  // --- typed float arrays -------------------------------------------------
  //
  // A global that is all-float and never byte-touched can live in its own
  // `%SystemSingleArray` instead of `__mem`. That turns a float load from
  // `__Get__` + `Int32BitsToSingle` into one `__Get__`, and a store from
  // `SingleToInt32Bits` + `__Set__` into one `__Set__`. Measured on the
  // playground: 64k of a tick's externs are exactly those bitcasts.
  //
  /// Global -> the `%SystemSingleArray` slot holding its floats.
  DenseMap<const GlobalVariable *, std::string> FloatArraySlots;
  /// Classifies globals into `FloatArraySlots`; runs before layOutGlobals.
  void classifyFloatArrays();
  /// The typed array slot for a pointer that provably denotes `G` or one of
  /// its elements, plus the element index. "" when it must stay in `__mem`.
  std::string floatArrayFor(const Value *Ptr, const std::string **IndexSlot);
  /// Declares each float-array slot and pushes its heap-image initializer.
  void emitFloatArrays();
  /// Slot name -> element count, for the init block's ctor calls.
  StringMap<uint64_t> FloatArraySizes;


  /// The condition as a `SystemBoolean` slot.
  std::string boolSlotFor(const Value *V, UBlock &B);

  // --- layout passes ------------------------------------------------------

  /// Assign `__mem` offsets to every global and build `__data_image`.
  void layOutGlobals();

  /// Assign shadow-stack frame sizes and per-alloca offsets.
  void layOutFrames();

  /// Assign `__fn_table` indices to address-taken functions.
  void collectFunctionPointers();

  /// Declare the program's fixed slots (`__mem`, `__sp`, `__depth`, ...).
  void declareProgramSlots();

  /// The entry prologue, emitted into the function's stub: reset when no frame
  /// is live, then count the activation, then the init guard. Returns the index
  /// of the block the stub's caller must continue in, because every stage of the
  /// prologue ends in a branch.
  unsigned emitEntryPrologue(UFunction &UF, unsigned StubIdx);

  /// Reserve this activation's shadow-stack frame, halting on overflow.
  /// Returns the index of the block the caller must continue in, because the
  /// check ends in a branch.
  unsigned emitFramePush(const Function &F, UFunction &UF, unsigned BodyIdx);

  /// Release this activation's frame.
  void emitFramePop(const Function *F, UBlock &B);

  /// `__mem`'s byte size, including the shadow stack and the pad bytes.
  uint64_t MemorySizeBytes = 0;

  /// `__mem`'s word count, which is the array's element count.
  uint64_t MemoryWordCount = 0;
  /// Data slots actually written to `.data_start`, duplicates removed.
  uint64_t DataSlotCount = 0;

  // --- selection ----------------------------------------------------------

  void selectFunction(const Function &F);

  /// Fill `UF.Blocks[Idx]` from `BB`, following a mid-block split: `select`,
  /// `min`/`max`, `abs` and a variable-count `lshr` all branch, so the
  /// instructions after them belong to the join block, and `Idx` is updated to
  /// point there.
  void selectBlock(const BasicBlock &BB, UFunction &UF, unsigned &Idx);

  void selectInstruction(const Instruction &I, UFunction &UF, unsigned &Idx);
  void selectTerminator(const Instruction &I, UFunction &UF, unsigned &Idx);
  bool selectIntrinsic(const Instruction &I, UFunction &UF, unsigned &Idx);

  /// Append an empty block to `UF` and return its index. Used for every split,
  /// so a block's address is always its first instruction's.
  unsigned appendBlock(UFunction &UF, const std::string &Label);

  /// A fresh, program-unique label stem: `<fn>_<hint>_<n>`. Every split names
  /// its blocks from this, so two splits can never collide -- which a bare
  /// counter concatenated three times in one expression could, because the
  /// three `++` evaluations are not ordered against each other.
  std::string freshLabelStem(StringRef Hint);

  /// A two-arm diamond: `Cond` chooses between two copies into `Result`, and
  /// the returned index is the join block, which the caller continues filling.
  unsigned emitDiamond(UFunction &UF, unsigned &Idx, const std::string &Cond,
                       const std::string &TrueSrc, const std::string &FalseSrc,
                       const std::string &Result, const std::string &Hint);

  void selectICmp(const ICmpInst &I, UFunction &UF, unsigned &Idx);
  void selectFCmp(const FCmpInst &I, UFunction &UF, unsigned &Idx);
  void selectFPToInt(const Instruction &I, UFunction &UF, unsigned &Idx);
  void selectVectorOp(const Instruction &I, UFunction &UF, unsigned &Idx);
  void selectVectorArith(const Instruction &I, UFunction &UF, unsigned &Idx);
  void selectLShr(const Instruction &I, UFunction &UF, unsigned &Idx);
  void selectUDivRem(const Instruction &I, UFunction &UF, unsigned &Idx);
  void selectUDivRemI64(const Instruction &I, UFunction &UF, unsigned &Idx);
  void selectSelect(const SelectInst &I, UFunction &UF, unsigned &Idx);
  void selectTrunc(const Instruction &I, UFunction &UF, unsigned &Idx);
  void selectZExt(const Instruction &I, UFunction &UF, unsigned &Idx);
  void selectSExt(const Instruction &I, UFunction &UF, unsigned &Idx);
  void selectBitCast(const Instruction &I, UFunction &UF, unsigned &Idx);
  void selectLoad(const LoadInst &I, UFunction &UF, unsigned &Idx);
  void selectStore(const StoreInst &I, UFunction &UF, unsigned &Idx);
  void selectAlloca(const AllocaInst &I, UFunction &UF, unsigned &Idx);
  void selectGEP(const GetElementPtrInst &I, UFunction &UF, unsigned &Idx);
  void selectCall(const CallInst &I, UFunction &UF, unsigned &Idx);

  /// Emit a direct call and return the index of the continuation block the
  /// caller's remaining instructions belong to.
  unsigned selectDirectCall(unsigned Idx, const Function *Callee,
                            ArrayRef<std::string> Args, const Value *Result,
                            const Instruction *At = nullptr);
  void selectMemoryIntrinsic(const Instruction &I, UFunction &UF, unsigned &Idx);

  /// Emit a function's phi copies for the edge `Pred -> Succ` into `B`.
  void emitPhiCopies(const BasicBlock *Pred, const BasicBlock *Succ, UBlock &B);

  /// The block a jump from `Pred` to `Succ` must target: the successor itself,
  /// or its edge trampoline when the edge carries phi copies.
  std::string edgeTarget(UFunction &UF, const BasicBlock *Pred,
                         const BasicBlock *Succ);

  /// Build the entry stub and exit block for `F`.
  void buildEntryBlocks(const Function &F, UFunction &UF);

  /// The label of a basic block.
  std::string blockLabel(const BasicBlock *BB) const;

  /// The slot a function's `ret` jumps through.
  std::string returnSlot(const Function *F) const;

  /// The slot a function's return value is written to.
  std::string returnValueSlot(const Function *F) const;

  /// The entry-point name a function exports under, empty if it is not one.
  std::string entryName(const Function &F) const;

private:
  void layout();
  void writeDataBlock();
  void writeCodeBlock();
  void writeHeapImage();
  void emitDataImage();
  void emitHaltBlock();
  void emitRuntimePrelude();

  raw_ostream &out() { return Out; }
  raw_pwrite_stream &Out;

  DenseMap<const Function *, std::string> EntryNames;
  uint64_t SlotCount = 0;
  std::string HeapImage;
};

/// Process-wide writer options, because `TargetRegistry`'s constructor
/// signature leaves no room to pass them to the target machine.
UDONOptions &udonWriterOptions();

/// Reject a construct the backend cannot express, as a one-line diagnostic.
[[noreturn]] void reportUDONError(const Twine &Msg);

/// Map a name into UASM's `[A-Za-z_][A-Za-z0-9_]*` identifier charset.
std::string mangleName(StringRef Name);

} // namespace llvm

#endif // LLVM_LIB_TARGET_UDON_UDONWRITER_H
