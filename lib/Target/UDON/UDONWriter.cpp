//===-- UDONWriter.cpp - LLVM IR to UDON UASM -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Emits a module as UASM text plus a heap image. This unit owns the module-wide
// decisions: which functions are entry points, the heap slots and their
// literals, the file layout, and the compile-time address arithmetic that lets
// a `JUMP_INDIRECT` target travel through a slot.
//
//===----------------------------------------------------------------------===//

#include "UDONWriter.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

char UDONWriter::ID = 0;

void llvm::reportUDONError(const Twine &Msg) {
  report_fatal_error(Msg, /* gen_crash_diag = */ false);
}

UDONOptions &llvm::udonWriterOptions() {
  static UDONOptions Opts;
  return Opts;
}

UDONWriter::UDONWriter(raw_pwrite_stream &Out, const UDONOptions &Opts)
    : ModulePass(ID), Opts(Opts), Out(Out) {}

namespace {

/// Udon's built-in events are camelCase after the underscore, so a source name
/// cannot be mapped to an export name by a prefix rule. This table is the
/// complete set; anything else is a custom event and keeps its name verbatim.
const char *const BuiltinEvents[] = {
    "start",             "update",         "lateUpdate",      "fixedUpdate",
    "postLateUpdate",    "onEnable",       "onDisable",       "interact",
    "onPickup",          "onDrop",         "onPickupUseDown", "onPickupUseUp",
    "onPlayerJoined",    "onPlayerLeft",   "onOwnershipTransferred",
    "onDeserialization", "onPreSerialization",
};

} // namespace

/// UASM identifiers are `[A-Za-z_][A-Za-z0-9_]*`; LLVM names routinely contain
/// `.`, `$` and quotes, all of which are a ParseException.
std::string llvm::mangleName(StringRef Name) {
  std::string Result;
  Result.reserve(Name.size() + 1);
  for (char C : Name) {
    bool Ok = (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||
              (C >= '0' && C <= '9') || C == '_';
    Result.push_back(Ok ? C : '_');
  }
  if (Result.empty() || (Result[0] >= '0' && Result[0] <= '9'))
    Result.insert(Result.begin(), '_');
  return Result;
}

namespace {
inline std::string mangle(StringRef Name) { return mangleName(Name); }
} // namespace

// --- names and types -------------------------------------------------------

std::string UDONWriter::entryName(const Function &F) const {
  StringRef Name = F.getName();
  if (const MDNode *N = F.getMetadata("udon.event"))
    if (const MDString *S = dyn_cast<MDString>(N->getOperand(0)))
      return S->getString().str();

  if (Name == "main")
    return "_start";

  constexpr StringLiteral Prefix("udon_event_");
  if (!Name.starts_with(Prefix))
    return {};

  StringRef Rest = Name.drop_front(Prefix.size());
  for (const char *Builtin : BuiltinEvents)
    if (Rest == Builtin)
      return ("_" + Rest).str();
  // A custom event carries no prefix.
  return Rest.str();
}

std::string UDONWriter::blockLabel(const BasicBlock *BB) const {
  return mangle(BB->getParent()->getName()) + "_" + mangle(BB->getName());
}

std::string UDONWriter::returnSlot(const Function *F) const {
  return mangle(F->getName()) + "_ret";
}

std::string UDONWriter::returnValueSlot(const Function *F) const {
  return mangle(F->getName()) + "_retval";
}

std::string UDONWriter::udonTypeOf(Type *Ty) const {
  if (Ty->isIntegerTy(1))
    return "SystemBoolean";
  // i8 and i16 live in `%SystemInt32` slots, masked to their width. A
  // `%SystemByte` slot is unusable: SystemConvert.__ToInt32__SystemByte aborts
  // in the VM.
  if (Ty->isIntegerTy(8) || Ty->isIntegerTy(16) || Ty->isIntegerTy(32))
    return "SystemInt32";
  if (Ty->isIntegerTy(64))
    return "SystemInt64";
  if (Ty->isFloatTy())
    return "SystemSingle";
  if (Ty->isDoubleTy())
    return "SystemDouble";
  if (Ty->isPointerTy())
    return "SystemInt32";
  if (Ty->isVoidTy())
    return "SystemVoid";
  if (auto *VT = dyn_cast<FixedVectorType>(Ty)) {
    if (VT->getElementType()->isFloatTy()) {
      switch (VT->getNumElements()) {
      case 2: return "UnityEngineVector2";
      case 3: return "UnityEngineVector3";
      case 4: return "UnityEngineVector4";
      }
    }
  }
  return {};
}

// --- slots -----------------------------------------------------------------

void UDONWriter::declareSlot(StringRef Name, StringRef UdonType) {
  auto It = SlotTypes.find(Name);
  if (It != SlotTypes.end()) {
    if (It->second != UdonType)
      reportUDONError("slot " + Name + " declared as both " + It->second +
                      " and " + UdonType);
    return;
  }
  SlotTypes[Name] = UdonType.str();
}

std::string UDONWriter::addressSlot(StringRef Name, uint32_t Address) {
  // A global referenced from many places must not be declared twice: the
  // assembler rejects a repeated data variable.
  if (SlotTypes.count(Name))
    return Name.str();
  declareSlot(Name, "SystemUInt32");
  DataLines.push_back("    " + Name.str() + ": %SystemUInt32, 0x" +
                      utohexstr(Address, /* LowerCase = */ false,
                                /* Width = */ 8));
  return Name.str();
}

std::string UDONWriter::addressSlotForLabel(StringRef Name, StringRef Label) {
  // Idempotent: the init block is inlined into every entry point, so the same
  // address slot is requested once per entry and a second declaration is an
  // "already exists" error.
  if (SlotTypes.count(Name))
    return Name.str();
  declareSlot(Name, "SystemUInt32");
  PendingAddressSlots.push_back({Name.str(), Label.str()});
  return Name.str();
}

std::string UDONWriter::i32Constant(int32_t V) {
  std::string Key = "i32:" + std::to_string(V);
  auto It = ConstantSlots.find(Key);
  if (It != ConstantSlots.end())
    return It->second;
  std::string Name = "__const_i32_" + std::to_string(ConstantSlots.size());
  declareSlot(Name, "SystemInt32");
  // The scanner lexes the digits of a decimal literal as a positive Int32 and
  // only then applies the sign, so -2147483648 cannot be written that way
  // ("Value was either too large or too small for an Int32"). Hex has no such
  // problem -- 0x80000000 assembles -- so the one value decimal cannot express
  // is written in hex.
  if (V == INT32_MIN)
    DataLines.push_back("    " + Name + ": %SystemInt32, 0x80000000");
  else
    DataLines.push_back("    " + Name + ": %SystemInt32, " + std::to_string(V));
  ConstantSlots[Key] = Name;
  return Name;
}

std::string UDONWriter::boolConstant(bool V) {
  // A `%SystemBoolean` slot, not an Int32: COPY between two slot types is a
  // type mismatch the VM rejects, so a boolean write needs a boolean source.
  std::string Key = std::string("i1:") + (V ? "1" : "0");
  auto It = ConstantSlots.find(Key);
  if (It != ConstantSlots.end())
    return It->second;
  std::string Name = "__const_bool_" + std::to_string(ConstantSlots.size());
  declareSlot(Name, "SystemBoolean");
  NullSlots.push_back({Name, "SystemBoolean"});
  HeapEntries.push_back("\"" + Name + "\": {\"type\": \"SystemBoolean\", " +
                        "\"value\": {\"bool\": " + (V ? "true" : "false") +
                        "}}");
  ConstantSlots[Key] = Name;
  return Name;
}

std::string UDONWriter::freshSlot(StringRef Hint, Type *Ty) {
  std::string Name =
      mangle(Cur->F->getName()) + "_" + Hint.str() + "_" +
      std::to_string(TempCounter++);
  std::string UdonTy = udonTypeOf(Ty);
  if (UdonTy.empty())
    reportUDONError("temporary has no Udon type: " + Name);
  declareSlot(Name, UdonTy);
  NullSlots.push_back({Name, UdonTy});
  return Name;
}

std::string UDONWriter::constantSlot(const Constant *C) {
  Type *Ty = C->getType();

  if (auto *CI = dyn_cast<ConstantInt>(C)) {
    unsigned Bits = CI->getBitWidth();
    // i8 and i16 constants live in `%SystemInt32` slots masked to their width,
    // which is also how a narrow store reads them back.
    if (Bits == 8 || Bits == 16 || Bits == 32)
      return i32Constant(static_cast<int32_t>(CI->getSExtValue()));
    if (Bits == 64) {
      uint64_t V = CI->getZExtValue();
      std::string Key = "i64:" + std::to_string(V);
      auto It = ConstantSlots.find(Key);
      if (It != ConstantSlots.end())
        return It->second;
      std::string Name = "__const_i64_" + std::to_string(ConstantSlots.size());
      declareSlot(Name, "SystemInt64");
      NullSlots.push_back({Name, "SystemInt64"});
      HeapEntries.push_back("\"" + Name + "\": {\"type\": \"SystemInt64\", " +
                            "\"value\": {\"i64\": " +
                            std::to_string(static_cast<int64_t>(V)) + "}}");
      ConstantSlots[Key] = Name;
      return Name;
    }
    if (Bits == 1) {
      bool V = CI->isOne();
      std::string Key = std::string("i1:") + (V ? "1" : "0");
      auto It = ConstantSlots.find(Key);
      if (It != ConstantSlots.end())
        return It->second;
      std::string Name = "__const_i1_" + std::to_string(ConstantSlots.size());
      declareSlot(Name, "SystemBoolean");
      NullSlots.push_back({Name, "SystemBoolean"});
      HeapEntries.push_back("\"" + Name + "\": {\"type\": \"SystemBoolean\", " +
                            "\"value\": {\"bool\": " + (V ? "true" : "false") +
                            "}}");
      ConstantSlots[Key] = Name;
      return Name;
    }
    reportUDONError("integer constant of width " + std::to_string(Bits) +
                    " has no Udon type");
  }

  if (auto *CF = dyn_cast<ConstantFP>(C)) {
    // Raw bits, so NaN payloads and -0.0 survive and no decimal text is parsed.
    const APFloat &APF = CF->getValueAPF();
    if (Ty->isFloatTy()) {
      uint32_t Bits =
          static_cast<uint32_t>(APF.bitcastToAPInt().getZExtValue());
      std::string Key = "f32:" + std::to_string(Bits);
      auto It = ConstantSlots.find(Key);
      if (It != ConstantSlots.end())
        return It->second;
      std::string Name = "__const_f32_" + std::to_string(ConstantSlots.size());
      declareSlot(Name, "SystemSingle");
      NullSlots.push_back({Name, "SystemSingle"});
      HeapEntries.push_back("\"" + Name +
                            "\": {\"type\": \"SystemSingle\", \"value\": " +
                            "{\"f32bits\": " + std::to_string(Bits) + "}}");
      ConstantSlots[Key] = Name;
      return Name;
    }
    if (Ty->isDoubleTy()) {
      uint64_t Bits = APF.bitcastToAPInt().getZExtValue();
      std::string Key = "f64:" + std::to_string(Bits);
      auto It = ConstantSlots.find(Key);
      if (It != ConstantSlots.end())
        return It->second;
      std::string Name = "__const_f64_" + std::to_string(ConstantSlots.size());
      declareSlot(Name, "SystemDouble");
      NullSlots.push_back({Name, "SystemDouble"});
      HeapEntries.push_back("\"" + Name +
                            "\": {\"type\": \"SystemDouble\", \"value\": " +
                            "{\"f64bits\": " + std::to_string(Bits) + "}}");
      ConstantSlots[Key] = Name;
      return Name;
    }
  }

  if (isa<ConstantPointerNull>(C)) {
    // A null pointer is byte offset 0, which is why bytes 0-15 of `__mem` are
    // reserved: no real object has that address.
    return i32Constant(0);
  }

  if (auto *G = dyn_cast<GlobalVariable>(C)) {
    // A global's address is a `__mem` byte offset, which is known before
    // selection, so it is an ordinary `SystemInt32` literal slot.
    auto It = GlobalOffsets.find(G);
    if (It == GlobalOffsets.end())
      reportUDONError("global has no memory offset: " + G->getName());
    std::string Key = "gaddr:" + G->getName().str();
    auto SlotIt = ConstantSlots.find(Key);
    if (SlotIt != ConstantSlots.end())
      return SlotIt->second;
    std::string Name = "__gaddr_" + mangle(G->getName());
    declareSlot(Name, "SystemInt32");
    DataLines.push_back("    " + Name + ": %SystemInt32, " +
                        std::to_string(It->second));
    ConstantSlots[Key] = Name;
    return Name;
  }

  if (auto *Fn = dyn_cast<Function>(C)) {
    // A function used as a value is an index into `__fn_table`, never a raw
    // code address: an index stores cleanly in a vtable and in an i32 slot.
    auto It = FunctionPointerIndices.find(Fn);
    if (It == FunctionPointerIndices.end())
      reportUDONError("function has no table index: " + Fn->getName());
    return i32Constant(static_cast<int32_t>(It->second));
  }

  if (isa<UndefValue>(C) || isa<PoisonValue>(C)) {
    // Undefined and poison are fresh zero slots. Rust's `freeze` of a poison
    // operand lands here, and a zero is as good a value as any.
    std::string Name = "__const_undef_" + std::to_string(ConstantSlots.size());
    std::string UdonTy = udonTypeOf(Ty);
    if (UdonTy.empty())
      reportUDONError("undef of a type with no Udon type");
    declareSlot(Name, UdonTy);
    DataLines.push_back("    " + Name + ": %" + UdonTy + ", null");
    return Name;
  }

  reportUDONError("unsupported constant in " + Cur->F->getName() + ": " +
                  C->getName());
}

std::string UDONWriter::slotFor(const Value *V) {
  if (auto *C = dyn_cast<Constant>(V))
    return constantSlot(C);

  auto It = Slots.find(V);
  if (It != Slots.end())
    return It->second;

  if (V->getType()->isVoidTy())
    reportUDONError("value of void type has no slot");

  std::string Ty = udonTypeOf(V->getType());
  if (Ty.empty())
    reportUDONError("value has no Udon type in " + Cur->F->getName());

  // An unnamed value gets a temporary slot, and that slot is registered here:
  // `freshSlot` only allocates a name, so a second lookup would otherwise
  // generate a different one and the first would be declared twice.
  std::string Name = V->hasName()
                         ? mangle(Cur->F->getName()) + "_" + mangle(V->getName())
                         : freshSlot("v", V->getType());
  declareSlot(Name, Ty);
  // A named value's slot is declared here; `freshSlot` already declared the
  // temporary's, so only the named case needs the `null` line.
  if (V->hasName())
    NullSlots.push_back({Name, Ty});
  Slots[V] = Name;
  return Name;
}

// --- emission primitives ---------------------------------------------------

void UDONWriter::emitCopy(UBlock &B, StringRef Src, StringRef Dst) {
  if (Src == Dst)
    return;
  B.Insts.push_back({UInst::PUSH, Src.str()});
  B.Insts.push_back({UInst::PUSH, Dst.str()});
  B.Insts.push_back({UInst::COPY, {}});
}

void UDONWriter::emitExtern(UBlock &B, StringRef Signature,
                            ArrayRef<std::string> Sources,
                            const std::string &Destination) {
  for (const std::string &S : Sources)
    B.Insts.push_back({UInst::PUSH, S});
  if (!Destination.empty())
    B.Insts.push_back({UInst::PUSH, Destination});
  B.Insts.push_back({UInst::EXTERN, Signature.str()});
  UsedExterns.insert(Signature);
}

std::string UDONWriter::emitTruncI64ToI32(const std::string &Src, UBlock &B) {
  // ((v + 0x80000000) & 0xFFFFFFFF) - 0x80000000, then Convert.ToInt32. The
  // add/sub pair is what makes a bare Convert provably in range: the value is
  // a signed 32-bit number by then, so Convert cannot throw. A bare Convert on
  // an unsigned i64 above Int32.MaxValue does throw, which is exactly the case
  // `udiv 0xFFFFFFFF, 1` reaches.
  std::string Half = constantSlot(ConstantInt::get(
      Type::getInt64Ty(Mod->getContext()), 0x80000000ull));
  std::string Mask = constantSlot(ConstantInt::get(
      Type::getInt64Ty(Mod->getContext()), 0xFFFFFFFFull));

  std::string T1 = freshSlot("trunc_add", Type::getInt64Ty(Mod->getContext()));
  emitExtern(B, "SystemInt64.__op_Addition__SystemInt64_SystemInt64__SystemInt64",
             {Src, Half}, T1);
  std::string T2 = freshSlot("trunc_and", Type::getInt64Ty(Mod->getContext()));
  emitExtern(B,
             "SystemInt64.__op_LogicalAnd__SystemInt64_SystemInt64__SystemInt64",
             {T1, Mask}, T2);
  std::string T3 = freshSlot("trunc_sub", Type::getInt64Ty(Mod->getContext()));
  emitExtern(B,
             "SystemInt64.__op_Subtraction__SystemInt64_SystemInt64__SystemInt64",
             {T2, Half}, T3);
  std::string Out = freshSlot("trunc_i32", Type::getInt32Ty(Mod->getContext()));
  emitExtern(B, "SystemConvert.__ToInt32__SystemInt64__SystemInt32", {T3}, Out);
  return Out;
}

std::string UDONWriter::boolSlotFor(const Value *V, UBlock &B) {
  if (V->getType()->isIntegerTy(1))
    return slotFor(V);
  std::string Out = freshSlot("cond", Type::getInt1Ty(Mod->getContext()));
  emitExtern(B, "SystemConvert.__ToBoolean__SystemInt32__SystemBoolean",
             {slotFor(V)}, Out);
  return Out;
}

// --- module driver ---------------------------------------------------------

bool UDONWriter::runOnModule(Module &M) {
  Mod = &M;
  DL = &M.getDataLayout();

  // Link the runtime helpers the intrinsic expansions call. `LinkOnlyNeeded`
  // pulls in exactly the functions a declaration asks for, so a program that
  // uses no bit counting carries none of it.
  if (Opts.LinkRuntime && !Opts.RuntimePath.empty()) {
    SMDiagnostic Err;
    std::unique_ptr<Module> Runtime =
        parseIRFile(Opts.RuntimePath, Err, M.getContext());
    if (!Runtime)
      reportUDONError("cannot read the runtime module " + Opts.RuntimePath);
    // LinkOnlyNeeded pulls a definition only when the destination already
    // declares it, so the helpers this module can reach are declared first.
    // Declaring all of them is cheaper than proving which are reachable, and an
    // unused declaration is dropped by the linker.
    for (const char *Name : {"__udon_ctlz_i32", "__udon_ctlz_i64",
                             "__udon_cttz_i32", "__udon_cttz_i64",
                             "__udon_ctpop_i32", "__udon_ctpop_i64",
                             "__udon_memset_fill"}) {
      if (Function *Existing = Runtime->getFunction(Name)) {
        if (Existing->isDeclaration())
          continue;
        FunctionCallee Decl = M.getOrInsertFunction(
            Name, Existing->getFunctionType());
        (void)Decl;
      }
    }
    if (Linker::linkModules(M, std::move(Runtime),
                            Linker::Flags::LinkOnlyNeeded))
      reportUDONError("cannot link the runtime module " + Opts.RuntimePath);
  }

  // Entry points, checked for duplicates up front: two functions claiming
  // `_start` is a source-level mistake the export block cannot express.
  StringMap<const Function *> Seen;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    std::string Entry = entryName(F);
    if (Entry.empty())
      continue;
    if (!F.arg_empty())
      reportUDONError("entry point " + Entry +
                      " has parameters; events with arguments are not "
                      "supported yet: " + F.getName());
    auto [It, Inserted] = Seen.try_emplace(Entry, &F);
    if (!Inserted)
      reportUDONError("duplicate entry point " + Entry + ": " +
                      It->second->getName() + " and " + F.getName());
    EntryNames[&F] = Entry;
  }

  declareProgramSlots();
  buildCallGraph();
  layOutGlobals();
  layOutFrames();
  collectFunctionPointers();

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    selectFunction(F);
  }

  emitHaltBlock();

  layout();

  // Every label has an address now, so the deferred literal slots can be
  // written. They go into `.data_start`, which is emitted next.
  for (const auto &[Name, Label] : PendingAddressSlots) {
    auto It = LabelAddresses.find(Label);
    if (It == LabelAddresses.end())
      reportUDONError("address slot " + Name + " names an unknown label " +
                      Label);
    DataLines.push_back("    " + Name + ": %SystemUInt32, 0x" +
                        utohexstr(It->second, /* LowerCase = */ false,
                                  /* Width = */ 8));
  }

  emitDataImage();
  writeDataBlock();
  writeCodeBlock();
  writeHeapImage();

  // The heap capacity is the highest address plus one, and addresses are
  // assigned in declaration order by the assembler. `SlotTypes` is the
  // declaration list, so its size is the capacity -- but a slot declared and
  // then never emitted still consumes an address, so the count must come from
  // what was actually written into `.data_start`, not from the map's size.
  uint64_t Declared = 0;
  {
    // Every emitted `name: %Type, value` line is one heap slot.
    Declared = DataLines.size() + NullSlots.size();
  }
  SlotCount = Declared;
  return true;
}

void UDONWriter::layout() {
  uint32_t Address = 0;
  for (UFunction &UF : Functions) {
    UF.Address = Address;
    for (UBlock &B : UF.Blocks) {
      // Two labels at one address is an AliasedSymbolException, so a block with
      // no instructions gets a NOP. Nothing falls through into it either way.
      if (B.Insts.empty())
        B.Insts.push_back({UInst::NOP, {}});
      B.Address = Address;
      LabelAddresses[B.Label] = Address;
      Address += B.size();
    }
  }
}

void UDONWriter::writeDataBlock() {
  out() << ".data_start\n";
  for (const std::string &Line : DataLines)
    out() << Line << "\n";
  for (const auto &[Name, Type] : NullSlots)
    out() << "    " << Name << ": %" << Type << ", null\n";
  out() << ".data_end\n";
}

void UDONWriter::writeCodeBlock() {
  out() << ".code_start\n";
  // An export names a label that is also emitted; the assembler resolves the
  // export through the symbol table, so the two spellings must match exactly.
  for (const auto &[Export, Label] : CodeExports)
    out() << "    .export " << Label << "\n";
  for (const UFunction &UF : Functions) {
    for (const UBlock &B : UF.Blocks) {
      out() << "\n    " << B.Label << ":\n";
      for (const UInst &I : B.Insts) {
        out() << "        " << I.mnemonic();
        if (!I.Operand.empty()) {
          out() << ", ";
          if (I.Op == UInst::EXTERN)
            out() << "\"" << I.Operand << "\"";
          else
            out() << I.Operand;
        }
        out() << "\n";
      }
    }
  }
  out() << ".code_end\n";
}

void UDONWriter::emitDataImage() {
  // The globals image travels in the heap image as base64, because the
  // assembler cannot express a byte array literal and the importer must not
  // serialize a 1 MiB array per behaviour.
  // Even an all-zero image is emitted: `__udon_init` copies it with
  // `__CopyTo__`, which throws on a null array, so a program with no
  // initializer bytes still needs a real (empty) array here.
  static const char *Digits =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string Encoded;
  Encoded.reserve(((DataImage.size() + 2) / 3) * 4);
  for (size_t I = 0; I < DataImage.size(); I += 3) {
    uint32_t B0 = DataImage[I];
    uint32_t B1 = I + 1 < DataImage.size() ? DataImage[I + 1] : 0;
    uint32_t B2 = I + 2 < DataImage.size() ? DataImage[I + 2] : 0;
    uint32_t Triple = (B0 << 16) | (B1 << 8) | B2;
    Encoded.push_back(Digits[(Triple >> 18) & 0x3F]);
    Encoded.push_back(Digits[(Triple >> 12) & 0x3F]);
    Encoded.push_back(I + 1 < DataImage.size() ? Digits[(Triple >> 6) & 0x3F] : '=');
    Encoded.push_back(I + 2 < DataImage.size() ? Digits[Triple & 0x3F] : '=');
  }
  HeapEntries.push_back(
      std::string("\"__data_image\": {\"type\": \"SystemByteArray\", ") +
      "\"value\": {\"bytes\": \"" + Encoded + "\"}}");
}

void UDONWriter::emitHaltBlock() {
  // One shared halt block serves every halt path: the frame overflow, the
  // caller-save overflow, and a null function pointer. It decrements the depth
  // and jumps to the sentinel, ending only the current Interpret().
  //
  // It deliberately does NOT reset `__sp` or the `__sptr_*` cursors: a halt
  // abandons the current activation without running its epilogue, so that frame
  // leaks, and resetting `__sp` here would put the pointer above a resuming
  // nested caller's live frame.
  UFunction UF;
  UF.F = nullptr;
  UF.BodyLabel = "__halt";

  UBlock Halt;
  Halt.Label = "__halt";
  emitExtern(Halt,
             "SystemInt32.__op_Subtraction__SystemInt32_SystemInt32__SystemInt32",
             {"__depth", i32Constant(1)}, "__depth");
  Halt.Insts.push_back({UInst::JUMP_INDIRECT, "__haltaddr"});
  UF.Blocks.push_back(std::move(Halt));

  // `__halt_null` is what `__fn_table[0]` points at: it names the reason before
  // falling into the shared block, so the log never reports a stale one.
  UBlock NullHalt;
  NullHalt.Label = "__halt_null";
  emitCopy(NullHalt, i32Constant(3), "__halt_reason");
  NullHalt.Insts.push_back({UInst::JUMP, "__halt"});
  UF.Blocks.push_back(std::move(NullHalt));

  Functions.push_back(std::move(UF));
}

void UDONWriter::writeHeapImage() {
  std::string Image = "{";
  for (size_t I = 0; I < HeapEntries.size(); I++) {
    if (I)
      Image += ",";
    Image += "\n";
    Image += HeapEntries[I];
  }
  if (!HeapEntries.empty())
    Image += "\n";
  Image += "}";
  HeapImage = Image;
}
