//===-- UDONWriterRuntime.cpp - embedded runtime and program prologue ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The program's fixed slots and the runtime helpers the intrinsic expansions
// call. There is no clang in the LLVM build tree this project targets, so the
// helpers are emitted as UASM directly rather than compiled from C.
//
//===----------------------------------------------------------------------===//

#include "UDONWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {
inline std::string mangle(StringRef Name) { return mangleName(Name); }
} // namespace

void UDONWriter::declareProgramSlots() {
  // The memory array is a WORD array, not a byte array.
  //
  // A byte array forces every scalar store through
  // `BitConverter.GetBytes(v).CopyTo(mem, p)`, which allocates a fresh array
  // per store -- measured at 32 bytes, 152.6 MB per 5M stores. A word array
  // stores with `Int32Array.__Set__(i, v)`, which allocates nothing.
  //
  // The cost is on the load side: `BitConverter.To*` rejects an `Int32Array`,
  // so a load is `__Get__` plus shifts, and an unaligned load spans two words.
  // Measured 1.7x slower for the unaligned case, a wash for the aligned one.
  // That is the trade this model makes, deliberately.
  //
  // `__mem` is still addressed in BYTES everywhere else -- a pointer is a byte
  // offset -- so every access converts with `p >> 2` for the word index and
  // `(p & 3) * 8` for the bit shift.
  uint64_t MemorySize = Opts.MemoryBytes + Opts.HeapBytes +
                        Opts.ShadowStackBytes + 8;
  uint64_t WordCount = (MemorySize + 3) / 4;
  declareSlot("__mem", "SystemInt32Array");
  NullSlots.push_back({"__mem", "SystemInt32Array"});
  MemorySizeBytes = MemorySize;
  MemoryWordCount = WordCount;

  // The globals image, as words so init copies it with one `__CopyTo__`.
  declareSlot("__data_image", "SystemInt32Array");
  NullSlots.push_back({"__data_image", "SystemInt32Array"});

  // The shadow-stack pointer and the caller-save cursors. `__sp` is
  // `%SystemInt32` because the prologue uses the signed subtraction operator
  // and alloca addresses are i32 pointers, and argument typing is strict.
  // `__sp` starts at the shadow region's top, which is `-udon-memory-bytes +
  // -udon-shadow-stack-bytes`. Both are positive byte offsets into `__mem`: the
  // globals image ends at `-udon-memory-bytes`, and the stack grows up from
  // there to the top.
  declareSlot("__sp", "SystemInt32");
  DataLines.push_back("    __sp: %SystemInt32, " +
                      std::to_string(static_cast<int32_t>(
                          Opts.MemoryBytes + Opts.HeapBytes +
                              Opts.ShadowStackBytes)));

  declareSlot("__depth", "SystemInt32");
  DataLines.push_back("    __depth: %SystemInt32, 0");

  declareSlot("__halt_reason", "SystemInt32");
  DataLines.push_back("    __halt_reason: %SystemInt32, 0");

  declareSlot("__haltaddr", "SystemUInt32");
  DataLines.push_back("    __haltaddr: %SystemUInt32, 0xFFFFFFFF");

  declareSlot("__inited", "SystemBoolean");
  DataLines.push_back("    __inited: %SystemBoolean, null");

  // The function-pointer table. Index 0 is the null entry, so a zeroed pointer
  // reaches a halt that names itself; every address-taken function gets the
  // next index.
  declareSlot("__fn_table", "SystemUInt32Array");
  NullSlots.push_back({"__fn_table", "SystemUInt32Array"});

  // The indirect-call transfer slots. They exist only for the transfer: the
  // callee copies them into its own slots on entry.
  declareSlot("__icall_ret", "SystemUInt32");
  NullSlots.push_back({"__icall_ret", "SystemUInt32"});
  declareSlot("__icall_retval", "SystemInt32");
  NullSlots.push_back({"__icall_retval", "SystemInt32"});

  // The caller-save stacks, one per slot type, plus a cursor for each. An array
  // cannot be a `.data_start` literal, so each is allocated by init.
  for (const char *Stack :
       {"__stack_i32", "__stack_i64", "__stack_f32", "__stack_f64",
        "__stack_obj"}) {
    std::string Type = std::string(Stack) + "Type";
    StringRef UdonType;
    if (StringRef(Stack) == "__stack_i32") UdonType = "SystemInt32Array";
    else if (StringRef(Stack) == "__stack_i64") UdonType = "SystemInt64Array";
    else if (StringRef(Stack) == "__stack_f32") UdonType = "SystemSingleArray";
    else if (StringRef(Stack) == "__stack_f64") UdonType = "SystemDoubleArray";
    else UdonType = "SystemObjectArray";
    declareSlot(Stack, UdonType);
    NullSlots.push_back({Stack, UdonType.str()});
  }

  // The caller-save cursors, one per typed stack. They are deliberately not
  // named `__sp_*`, which would read as the shadow-stack pointer.
  for (const char *Cursor : {"__sptr_i32", "__sptr_i64", "__sptr_f32",
                             "__sptr_f64", "__sptr_obj"}) {
    declareSlot(Cursor, "SystemInt32");
    DataLines.push_back(std::string("    ") + Cursor + ": %SystemInt32, 0");
  }
}

unsigned UDONWriter::emitEntryPrologue(UFunction &UF, unsigned StubIdx) {
  // Every stage ends in a branch, so each one fills a block and hands back the
  // block the next stage belongs in. Holding a `UBlock &` across `appendBlock`
  // would be a dangling reference, and appending to a block after its branch
  // would make the instructions unreachable.
  //
  // `__depth == 0` means no frames are live, so the shadow stack and the
  // caller-save cursors are reset -- the reset is what makes a poisoned stack
  // recoverable. It comes before the increment, because incrementing first
  // would make a fresh event read 1 and the reset would never run. It also
  // comes before the `__inited` guard, which is safe because the reset only
  // writes heap slots and dereferences nothing.
  //
  // The prologue is inlined into every entry point rather than shared: a shared
  // block would need its own return-address slot, an unnecessary hazard.
  std::string Zero = i32Constant(0);
  std::string Base = mangle(UF.F->getName());
  std::string ResetLabel = Base + "_reset";
  std::string AfterLabel = Base + "_reset_done";
  std::string InitLabel = Base + "_init";

  {
    UBlock &Stub = UF.Blocks[StubIdx];
    std::string Cond = freshSlot("depth0", Type::getInt1Ty(Mod->getContext()));
    emitExtern(Stub,
               "SystemInt32.__op_Equality__SystemInt32_SystemInt32__SystemBoolean",
               {"__depth", Zero}, Cond);
    Stub.Insts.push_back({UInst::PUSH, Cond});
    Stub.Insts.push_back({UInst::JUMP_IF_FALSE, AfterLabel});
    Stub.Insts.push_back({UInst::JUMP, ResetLabel});
  }

  {
    unsigned ResetIdx = appendBlock(UF, ResetLabel);
    UBlock &Reset = UF.Blocks[ResetIdx];
    emitCopy(Reset,
             i32Constant(static_cast<int32_t>(Opts.MemoryBytes +
                                              Opts.HeapBytes +
                                              Opts.ShadowStackBytes)),
             "__sp");
    for (const char *Cursor : {"__sptr_i32", "__sptr_i64", "__sptr_f32",
                               "__sptr_f64", "__sptr_obj"})
      emitCopy(Reset, Zero, Cursor);
    Reset.Insts.push_back({UInst::JUMP, AfterLabel});
  }

  // The reset-done block counts this activation, then checks the init guard.
  unsigned AfterIdx = appendBlock(UF, AfterLabel);
  {
    UBlock &After = UF.Blocks[AfterIdx];
    emitExtern(After,
               "SystemInt32.__op_Addition__SystemInt32_SystemInt32__SystemInt32",
               {"__depth", i32Constant(1)}, "__depth");
    // `PUSH __inited; JUMP_IF_FALSE <init>` then the caller's continuation.
    // `__inited` is a null `%SystemBoolean` slot, which reads `false`
    // (verified), so no host-written initial value is needed.
    After.Insts.push_back({UInst::PUSH, "__inited"});
    After.Insts.push_back({UInst::JUMP_IF_FALSE, InitLabel});
    After.Insts.push_back({UInst::JUMP, Base + "_prologue_done"});
  }

  // The init block allocates `__mem` and copies the globals image into it.
  // `__data_image` is a separate array because the importer runs at edit time
  // and init at play time, and a 1 MiB image must not be serialized per
  // behaviour.
  {
    unsigned InitIdx = appendBlock(UF, InitLabel);
    UBlock &Init = UF.Blocks[InitIdx];
    // Allocate the WORD array. `__mem` is addressed in bytes everywhere else,
    // so the element count is the byte size rounded up to whole words.
    emitExtern(Init, "SystemInt32Array.__ctor__SystemInt32__SystemInt32Array",
               {i32Constant(static_cast<int32_t>(MemoryWordCount))}, "__mem");

    // Copy the globals image in. Both sides are word arrays, so this is one
    // `__CopyTo__` at word offset 4 (byte offset 16) -- no unrolled per-word
    // store, which would cost a heap slot per word.
    emitExtern(Init,
               "SystemInt32Array.__CopyTo__SystemArray_SystemInt32__SystemVoid",
               {"__data_image", "__mem", i32Constant(4)}, "");
    // The typed float arrays are constructed the same way, and the heap
    // image fills them at import: their initializers are base64 f32 streams
    // applied as `float[]`, so init only allocates.
    for (const auto &[Slot, Count] : FloatArraySizes) {
      // An array with initial data comes from the heap image (a `float[]`
      // applied to the slot at load time); constructing here would replace
      // it with a fresh zeroed array. Only the empty ones construct.
      if (!F32sWithInitializers.count(Slot.str()))
        emitExtern(Init, "SystemSingleArray.__ctor__SystemInt32__SystemSingleArray",
                   {i32Constant(static_cast<int32_t>(Count))}, Slot.str());
    }

    // Allocate each caller-save stack at `-udon-stack-depth` entries.
  for (const auto &[Stack, Type] :
       {std::pair<const char *, const char *>{"__stack_i32", "SystemInt32Array"},
        {"__stack_i64", "SystemInt64Array"},
        {"__stack_f32", "SystemSingleArray"},
        {"__stack_f64", "SystemDoubleArray"},
        {"__stack_obj", "SystemObjectArray"}}) {
    std::string Sig =
        std::string(Type) + ".__ctor__SystemInt32__" + Type;
    emitExtern(Init, Sig,
               {i32Constant(static_cast<int32_t>(Opts.StackDepth))}, Stack);
  }
  // Allocate `__fn_table` and fill it from the per-entry literal slots. An array
  // cannot be a `.data_start` literal, so the addresses travel through
  // `__fnaddr_<i>` slots.
  emitExtern(Init, "SystemUInt32Array.__ctor__SystemInt32__SystemUInt32Array",
             {i32Constant(static_cast<int32_t>(
                  FunctionPointerIndices.size() + 1))},
             "__fn_table");
  {
    std::string NullSlot =
        addressSlotForLabel("__fnaddr_null", "__halt_null");
    emitExtern(Init,
               "SystemUInt32Array.__Set__SystemInt32_SystemUInt32__SystemVoid",
               {"__fn_table", i32Constant(0), NullSlot}, "");
  }
  for (const auto &[F, Index] : FunctionPointerIndices) {
    std::string Slot = addressSlotForLabel(
        "__fnaddr_" + mangle(F->getName()), mangle(F->getName()) + "_body");
    emitExtern(Init,
               "SystemUInt32Array.__Set__SystemInt32_SystemUInt32__SystemVoid",
               {"__fn_table", i32Constant(static_cast<int32_t>(Index)), Slot},
               "");
  }
  emitCopy(Init, boolConstant(true), "__inited");
    Init.Insts.push_back({UInst::JUMP, Base + "_prologue_done"});
  }

  // One continuation for both paths: the caller's remaining instructions go
  // here, so the stub can end in a branch either way.
  return appendBlock(UF, Base + "_prologue_done");
}

unsigned UDONWriter::emitFramePush(const Function &F, UFunction &UF,
                                   unsigned BodyIdx) {
  UBlock &B = UF.Blocks[BodyIdx];
  auto It = FrameSizes.find(&F);
  if (It == FrameSizes.end() || It->second == 0)
    return BodyIdx;
  int32_t Frame = static_cast<int32_t>(It->second);

  // `__sp_new = __sp - frameSize`, checked against the region's bottom before
  // it is committed: a failed frame must allocate nothing, so a nested caller
  // that resumes keeps its own frame intact.
  std::string New = freshSlot("sp_new", Type::getInt32Ty(Mod->getContext()));
  emitExtern(B,
             "SystemInt32.__op_Subtraction__SystemInt32_SystemInt32__SystemInt32",
             {"__sp", i32Constant(Frame)}, New);

  // The shadow region's bottom is `__mem`'s byte offset where the globals image
  // ends -- that is, `-udon-memory-bytes`. It is NOT a negative address: `__sp`
  // starts at `-udon-memory-bytes + -udon-shadow-stack-bytes`, both of which are
  // ordinary positive byte offsets into `__mem`.
  std::string Floor =
      i32Constant(static_cast<int32_t>(Opts.MemoryBytes + Opts.HeapBytes));
  std::string Ok = freshSlot("sp_ok", Type::getInt1Ty(Mod->getContext()));
  emitExtern(B,
             "SystemInt32.__op_GreaterThanOrEqual__SystemInt32_SystemInt32__"
             "SystemBoolean",
             {New, Floor}, Ok);

  // On overflow: name the reason and halt. The shared halt block decrements the
  // depth and logs, and deliberately leaves `__sp` alone so a nested caller
  // stays intact.
  std::string Base = mangle(F.getName());
  std::string HaltLabel = Base + "_sp_overflow";
  std::string DoneLabel = Base + "_sp_ok_done";

  B.Insts.push_back({UInst::PUSH, Ok});
  B.Insts.push_back({UInst::JUMP_IF_FALSE, HaltLabel});
  emitCopy(B, New, "__sp");
  B.Insts.push_back({UInst::JUMP, DoneLabel});

  unsigned HaltIdx = appendBlock(UF, HaltLabel);
  UBlock &Halt = UF.Blocks[HaltIdx];
  emitCopy(Halt, i32Constant(2), "__halt_reason");
  Halt.Insts.push_back({UInst::JUMP, "__halt"});

  // The caller continues here: the check above ends in a branch, so appending
  // to the same block would put its instructions after the jump and make them
  // unreachable.
  return appendBlock(UF, DoneLabel);
}

void UDONWriter::emitFramePop(const Function *F, UBlock &B) {
  auto It = FrameSizes.find(F);
  if (It == FrameSizes.end() || It->second == 0)
    return;
  emitExtern(B,
             "SystemInt32.__op_Addition__SystemInt32_SystemInt32__SystemInt32",
             {"__sp", i32Constant(static_cast<int32_t>(It->second))}, "__sp");
}
