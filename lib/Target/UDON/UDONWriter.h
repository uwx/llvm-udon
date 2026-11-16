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
  /// Bytes of alloca shadow stack above the globals image.
  uint64_t ShadowStackBytes = 1u << 16;
  /// Entries per `__stack_*` caller-save array.
  uint64_t StackDepth = 1024;
  /// Heap slot count; 0 means "computed from the module".
  uint64_t HeapSlots = 0;
  /// Link the embedded runtime library.
  bool LinkRuntime = true;
  /// Export every internal label with a `__` prefix, for the lit suite.
  bool ExportLabels = false;
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
  /// Successor edge -> index of its trampoline block, for a predecessor with
  /// more than one successor whose phi copies must run only on that edge.
  DenseMap<const BasicBlock *, unsigned> EdgeIndex;
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

  std::vector<UFunction> Functions;

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

  /// Every extern signature emitted, for the manifest.
  StringSet<> UsedExterns;

  /// Symbol -> Udon type string, for every slot the writer declares.
  StringMap<std::string> SlotTypes;

  /// Constants already interned, keyed by `type:bits`.
  StringMap<std::string> ConstantSlots;

  /// Label -> address, filled by layout().
  StringMap<uint32_t> LabelAddresses;

  /// Global variable -> byte offset in `__mem`.
  DenseMap<const GlobalVariable *, uint32_t> GlobalOffsets;

  /// `__data_image` contents, indexed by `__mem` offset.
  std::vector<uint8_t> DataImage;

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
  /// Program-wide counter for split-block names. Program-wide, not per
  /// function: a split's label is used as a jump target as well as a label, so
  /// it must be unique by construction rather than deduplicated afterwards.
  unsigned BlockCounter = 0;
  /// The current function's store scratch slot, empty until first needed.
  std::string ScratchBytes;


  // --- slot and constant machinery ----------------------------------------

  /// Declare a slot; a conflicting type is a writer bug.
  void declareSlot(StringRef Name, StringRef UdonType);

  /// The slot name for a value, allocating one on first use.
  std::string slotFor(const Value *V);

  /// A fresh temporary slot of the given type, named after `Hint`.
  std::string freshSlot(StringRef Hint, Type *Ty);

  /// A fresh temporary of the same type as `V`.
  std::string freshSlot(const Value *V, StringRef Hint) {
    return freshSlot(Hint, V->getType());
  }

  /// The slot holding a constant, creating the literal or heap entry it needs.
  std::string constantSlot(const Constant *C);

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

  /// Load `Ty` from `__mem` at `Offset`, returning the value's slot.
  std::string emitLoad(UBlock &B, const std::string &Offset, Type *Ty);

  /// Store `Value` into `__mem` at `Offset`.
  void emitStore(UBlock &B, const std::string &Value,
                 const std::string &Offset, Type *Ty);

  /// The per-function `%SystemByteArray` scratch slot a store writes through.
  std::string scratchByteArray();

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
