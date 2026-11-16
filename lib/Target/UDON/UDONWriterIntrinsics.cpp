//===-- UDONWriterIntrinsics.cpp - intrinsic expansion -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Intrinsics are expanded here rather than left to IntrinsicLowering, whose
// results are target-agnostic and would introduce operations Udon lacks. The
// set is exactly what a `no_std` rustc `-O` build emits, plus the erasures.
//
//===----------------------------------------------------------------------===//

#include "UDONWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {
inline std::string mangle(StringRef Name) { return mangleName(Name); }
} // namespace

namespace {

std::string typePrefix(Type *Ty) {
  if (Ty->isIntegerTy(32)) return "SystemInt32";
  if (Ty->isIntegerTy(64)) return "SystemInt64";
  if (Ty->isFloatTy()) return "SystemSingle";
  if (Ty->isDoubleTy()) return "SystemDouble";
  if (Ty->isIntegerTy(1)) return "SystemBoolean";
  return {};
}

std::string binSig(StringRef Type, StringRef Op) {
  return (Type + ".__op_" + Op + "__" + Type + "_" + Type + "__" + Type).str();
}

std::string cmpSig(StringRef Type, StringRef Op) {
  return (Type + ".__op_" + Op + "__" + Type + "_" + Type + "__SystemBoolean")
      .str();
}

} // namespace

bool UDONWriter::selectIntrinsic(const Instruction &I, UFunction &UF,
                                 unsigned &Idx) {
  const auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II)
    return false;
  Intrinsic::ID ID = II->getIntrinsicID();
  UBlock &B = UF.Blocks[Idx];

  switch (ID) {
  // No observable effect.
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end:
  case Intrinsic::assume:
  case Intrinsic::dbg_declare:
  case Intrinsic::dbg_value:
  case Intrinsic::dbg_label:
  case Intrinsic::dbg_assign:
  case Intrinsic::experimental_noalias_scope_decl:
    return true;

  // umin/umax/smin/smax: compare, then a diamond selecting the other operand.
  case Intrinsic::umin:
  case Intrinsic::umax:
  case Intrinsic::smin:
  case Intrinsic::smax: {
    Type *Ty = I.getType();
    std::string Prefix = typePrefix(Ty);
    if (Prefix.empty())
      reportUDONError("unsupported min/max type in " + I.getFunction()->getName());
    bool IsMax = ID == Intrinsic::umax || ID == Intrinsic::smax;
    bool IsUnsigned = ID == Intrinsic::umin || ID == Intrinsic::umax;
    std::string LHS = slotFor(I.getOperand(0));
    std::string RHS = slotFor(I.getOperand(1));

    std::string A = LHS, Bv = RHS;
    if (IsUnsigned) {
      unsigned Bits = Ty->getIntegerBitWidth();
      uint64_t MaskBits = Bits == 64 ? 0x8000000000000000ull : 0x80000000ull;
      std::string Mask = constantSlot(ConstantInt::get(Ty, MaskBits));
      A = freshSlot("minmax_l", Ty);
      emitExtern(B, binSig(Prefix, "LogicalXor"), {LHS, Mask}, A);
      Bv = freshSlot("minmax_r", Ty);
      emitExtern(B, binSig(Prefix, "LogicalXor"), {RHS, Mask}, Bv);
    }

    // For max, `A < B` means take B; for min, `A > B` means take B.
    std::string Cmp = freshSlot("minmax_cmp", Type::getInt1Ty(Mod->getContext()));
    emitExtern(B, cmpSig(Prefix, IsMax ? "LessThan" : "GreaterThan"),
               {A, Bv}, Cmp);
    Idx = emitDiamond(UF, Idx, Cmp, RHS, LHS, slotFor(&I), "minmax");
    return true;
  }

  // llvm.abs: 0 - x when x < 0.
  case Intrinsic::abs: {
    Type *Ty = I.getType();
    std::string Prefix = typePrefix(Ty);
    std::string X = slotFor(I.getOperand(0));
    std::string Zero = constantSlot(ConstantInt::get(Ty, 0));
    std::string Neg = freshSlot("abs_neg", Ty);
    emitExtern(B, binSig(Prefix, "Subtraction"), {Zero, X}, Neg);
    std::string Cmp = freshSlot("abs_cmp", Type::getInt1Ty(Mod->getContext()));
    emitExtern(B, cmpSig(Prefix, "LessThan"), {X, Zero}, Cmp);
    Idx = emitDiamond(UF, Idx, Cmp, Neg, X, slotFor(&I), "abs");
    return true;
  }

  // bswap: extract each byte, shift it into its reversed position, or it in.
  case Intrinsic::bswap: {
    Type *Ty = I.getType();
    unsigned Bits = Ty->getIntegerBitWidth();
    std::string Prefix = typePrefix(Ty);
    std::string X = slotFor(I.getOperand(0));
    // The accumulator is a temporary, not a constant slot: it is the
    // destination of each `or`, and writing into a constant's slot would
    // corrupt every other use of that constant.
    std::string Acc = freshSlot("bswap_acc", Ty);
    emitCopy(B, constantSlot(ConstantInt::get(Ty, 0)), Acc);
    unsigned Bytes = Bits / 8;
    for (unsigned I2 = 0; I2 < Bytes; I2++) {
      // Byte `I2` of the input is at bit 8*I2; in the result it belongs at
      // bit 8*(Bytes-1-I2). So the extraction mask is at 8*I2 and the shift is
      // 8*(Bytes-1-I2) -- 0x11223344 becomes 0x44332211.
      uint64_t Mask = static_cast<uint64_t>(0xFFu) << (8 * I2);
      std::string Byte = freshSlot("bswap_b", Ty);
      emitExtern(B, binSig(Prefix, "LogicalAnd"),
                 {X, constantSlot(ConstantInt::get(Ty, Mask))}, Byte);
      // A byte extracted at 8*I2 moves to 8*(Bytes-1-I2): down when
      // I2 < Bytes-1-I2, up when it is larger, and not at all in the middle.
      int Delta = 8 * (static_cast<int>(Bytes) - 1 - 2 * static_cast<int>(I2));
      std::string Shifted = Byte;
      if (Delta > 0) {
        Shifted = freshSlot("bswap_s", Ty);
        emitExtern(B, binSig(Prefix, "LeftShift"),
                   {Byte, i32Constant(Delta)}, Shifted);
      } else if (Delta < 0) {
        Shifted = freshSlot("bswap_s", Ty);
        emitExtern(B, binSig(Prefix, "RightShift"),
                   {Byte, i32Constant(-Delta)}, Shifted);
      }
      std::string Next = freshSlot("bswap_a", Ty);
      emitExtern(B, binSig(Prefix, "LogicalOr"), {Acc, Shifted}, Next);
      Acc = Next;
    }
    emitCopy(B, Acc, slotFor(&I));
    return true;
  }

  // ctlz/cttz/ctpop: runtime helpers, because the sequences are long enough to
  // be worth writing once in IR.
  case Intrinsic::ctlz:
  case Intrinsic::cttz:
  case Intrinsic::ctpop: {
    Type *Ty = I.getType();
    bool Is64 = Ty->isIntegerTy(64);
    const char *Name = ID == Intrinsic::ctlz   ? "__udon_ctlz"
                       : ID == Intrinsic::cttz ? "__udon_cttz"
                                               : "__udon_ctpop";
    std::string Helper = std::string(Name) + (Is64 ? "_i64" : "_i32");
    const Function *Callee = Mod->getFunction(Helper);
    if (!Callee)
      reportUDONError("runtime helper is missing: " + Helper);
    Idx = selectDirectCall(Idx, Callee, {slotFor(I.getOperand(0))}, &I, &I);
    return true;
  }

  // fshl/fshr: a rotate, plus a select on n == 0. C# masks shift counts, so
  // `y >> (Bits - 0)` computes `y >> 0` and the plain sequence returns x | y
  // instead of x; the select is what keeps it correct for x != y.
  case Intrinsic::fshl:
  case Intrinsic::fshr: {
    Type *Ty = I.getType();
    std::string Prefix = typePrefix(Ty);
    unsigned Bits = Ty->getIntegerBitWidth();
    std::string X = slotFor(I.getOperand(0));
    std::string Y = slotFor(I.getOperand(1));
    std::string N = slotFor(I.getOperand(2));
    std::string Nm = freshSlot("rot_n", Ty);
    emitExtern(B, binSig(Prefix, "LogicalAnd"),
               {N, constantSlot(ConstantInt::get(Ty, Bits - 1))}, Nm);
    std::string Inv = freshSlot("rot_inv", Ty);
    emitExtern(B, binSig(Prefix, "Subtraction"),
               {constantSlot(ConstantInt::get(Ty, Bits)), Nm}, Inv);
    std::string Left = freshSlot("rot_l", Ty);
    emitExtern(B, binSig(Prefix, "LeftShift"), {X, Nm}, Left);
    std::string Right = freshSlot("rot_r", Ty);
    emitExtern(B, binSig(Prefix, "RightShift"), {Y, Inv}, Right);
    std::string Or = freshSlot("rot_or", Ty);
    emitExtern(B, binSig(Prefix, "LogicalOr"), {Left, Right}, Or);

    std::string IsZero = freshSlot("rot_z", Type::getInt1Ty(Mod->getContext()));
    emitExtern(B, cmpSig(Prefix, "Equality"),
               {Nm, constantSlot(ConstantInt::get(Ty, 0))}, IsZero);
    Idx = emitDiamond(UF, Idx, IsZero, ID == Intrinsic::fshl ? X : Y, Or,
                      slotFor(&I), "rot");
    return true;
  }

  case Intrinsic::memcpy:
  case Intrinsic::memmove:
  case Intrinsic::memset:
    selectMemoryIntrinsic(I, UF, Idx);
    return true;

  case Intrinsic::trap:
    reportUDONError("llvm.trap has no Udon equivalent");

  default:
    reportUDONError("unsupported intrinsic: " +
                    II->getCalledFunction()->getName());
  }
}

void UDONWriter::selectMemoryIntrinsic(const Instruction &I, UFunction &UF,
                                       unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  const auto *MI = cast<MemIntrinsic>(&I);
  std::string Length = slotFor(MI->getLength());
  std::string Dst = pointerOffset(MI->getRawDest(), B);

  if (const auto *MT = dyn_cast<MemTransferInst>(&I)) {
    std::string Src = pointerOffset(MT->getRawSource(), B);
    // Array.Copy on one byte array handles overlap, which is exactly memmove
    // semantics, so memcpy and memmove share this lowering.
    emitExtern(B,
               "SystemArray.__Copy__SystemArray_SystemInt32_SystemArray_"
               "SystemInt32_SystemInt32__SystemVoid",
               {"__mem", Src, "__mem", Dst, Length}, "");
    return;
  }

  const auto *MS = cast<MemSetInst>(&I);
  if (auto *CI = dyn_cast<ConstantInt>(MS->getValue()))
    if (CI->isZero()) {
      emitExtern(
          B, "SystemArray.__Clear__SystemArray_SystemInt32_SystemInt32__SystemVoid",
          {"__mem", Dst, Length}, "");
      return;
    }

  // A non-zero fill is one `store i8` then a doubling memcpy loop, written in
  // the runtime module. The loop calls llvm.memcpy, which this same lowering
  // turns into Array.Copy.
  const Function *Helper = Mod->getFunction("__udon_memset_fill");
  if (!Helper)
    reportUDONError("runtime helper is missing: __udon_memset_fill");
  Idx = selectDirectCall(Idx, Helper, {Dst, slotFor(MS->getValue()), Length},
                         nullptr, &I);
}
