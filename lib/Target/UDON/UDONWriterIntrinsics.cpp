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
  // A value narrower than a slot is held in a `%SystemInt32` slot, so it is
  // operated on with the SystemInt32 operators. Callers that care about the
  // value's own width mask it themselves (min/max, fshl).
  if (Ty->isIntegerTy(8) || Ty->isIntegerTy(16)) return "SystemInt32";
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
  // A variadic function's arguments are not modelled: the writer passes every
  // argument the call supplies in its own slot, and a callee that wanted to
  // walk them would need a register save area Udon does not have. A function
  // that only forwards its `va_list` therefore starts and ends one for nothing.
  case Intrinsic::vastart:
  case Intrinsic::vaend:
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
      // The mask must be 32-bit for every narrow type, and the operands must be
      // zero-extended first: a value of a type narrower than a slot keeps
      // whatever its producing instruction left in the high bits, and the XOR
      // trick only orders correctly on zero-extended values. Building the mask
      // from a narrow type truncates 0x80000000 to zero and silently disables
      // the trick.
      unsigned Bits = Ty->isPointerTy() ? 32 : Ty->getIntegerBitWidth();
      bool Wide = Bits == 64;
      uint64_t MaskBits = Wide ? 0x8000000000000000ull : 0x80000000ull;
      std::string Mask = constantSlot(ConstantInt::get(
          Wide ? Ty : Type::getInt32Ty(Ty->getContext()), MaskBits));
      auto Normalize = [&](const std::string &Slot, const char *Hint) {
        if (Wide || Bits >= 32)
          return Slot;
        int32_t Keep = Bits == 8 ? 0xFF : 0xFFFF;
        return memBinop("LogicalAnd", Slot, i32Constant(Keep), Hint,
                        UF.Blocks[Idx]);
      };
      A = freshSlot("minmax_l", Ty);
      emitExtern(B, binSig(Prefix, "LogicalXor"),
                 {Normalize(LHS, "mmzn"), Mask}, A);
      Bv = freshSlot("minmax_r", Ty);
      emitExtern(B, binSig(Prefix, "LogicalXor"),
                 {Normalize(RHS, "mmzn"), Mask}, Bv);
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

  // bitreverse: a helper, then the bits above the value's own width are
  // discarded. The helper reverses a whole slot, so an i8 or i16 result has to
  // move up to the top before the value sits in the right place.
  case Intrinsic::bitreverse: {
    Type *Ty = I.getType();
    unsigned Bits = Ty->getIntegerBitWidth();
    bool Is64 = Bits == 64;
    const Function *Callee =
        Mod->getFunction(Is64 ? "__udon_bitreverse_i64" : "__udon_bitreverse_i32");
    if (!Callee)
      reportUDONError("runtime helper is missing: __udon_bitreverse");
    std::string X = slotFor(I.getOperand(0));
    if (Bits < 32) {
      // Only the value's own bits may be reversed, and only they may remain.
      X = memBinop("LogicalAnd", X,
                   i32Constant(Bits == 8 ? 0xFF : 0xFFFF), "brev_m",
                   UF.Blocks[Idx]);
    }
    Idx = selectDirectCall(Idx, Callee, {X}, &I, &I);
    if (Bits < 32) {
      // The helper reversed 32 bits; the value's own bits are now at the top,
      // so move them down. The destination is the same slot the helper wrote,
      // which is safe: every argument is read before the result is stored.
      emitExtern(UF.Blocks[Idx], binSig("SystemInt32", "RightShift"),
                 {slotFor(&I), i32Constant(32 - static_cast<int32_t>(Bits))},
                 slotFor(&I));
    }
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

  // Saturating add/sub. LLVM defines them as the true result clamped to the
  // type's range, so the lowering computes the true result in a width that can
  // hold it and then clamps. A narrow type already lives in an i32 slot, and
  // its own sum fits there (two i16 slots add to at most 65534), so only i32
  // needs a wider intermediate. i64 would need 128-bit arithmetic, which Udon
  // has no operators for at all, so it is refused rather than miscompiled.
  case Intrinsic::uadd_sat:
  case Intrinsic::usub_sat:
  case Intrinsic::sadd_sat:
  case Intrinsic::ssub_sat: {
    Type *Ty = I.getType();
    unsigned Bits = Ty->getIntegerBitWidth();
    if (Bits > 32)
      reportUDONError("saturating arithmetic on i" + Twine(Bits) +
                      " is unsupported: " + I.getFunction()->getName());
    bool Signed = ID == Intrinsic::sadd_sat || ID == Intrinsic::ssub_sat;
    bool IsAdd = ID == Intrinsic::uadd_sat || ID == Intrinsic::sadd_sat;
    LLVMContext &C = Mod->getContext();
    bool Wide = Bits == 32;
    Type *CTy = Wide ? Type::getInt64Ty(C) : Type::getInt32Ty(C);
    const std::string CP = Wide ? "SystemInt64" : "SystemInt32";
    auto BinSig = [&](StringRef Op) {
      return CP + ".__op_" + Op.str() + "__" + CP + "_" + CP + "__" + CP;
    };
    auto CmpSig2 = [&](StringRef Op) {
      return CP + ".__op_" + Op.str() + "__" + CP + "_" + CP + "__SystemBoolean";
    };

    // To the compute width, in the value's own sense: masked to its type width,
    // and sign-extended only for a signed operation. The mask matters because
    // nothing guarantees the high bits of a narrow slot are clear.
    auto ToCompute = [&](Value *V) -> std::string {
      std::string S = slotFor(V);
      if (Wide) {
        std::string L = freshSlot("sat_w", CTy);
        emitExtern(UF.Blocks[Idx],
                   "SystemConvert.__ToInt64__SystemInt32__SystemInt64", {S}, L);
        if (Signed)
          return L;
        std::string M = freshSlot("sat_m", CTy);
        emitExtern(UF.Blocks[Idx], BinSig("LogicalAnd"),
                   {L, constantSlot(ConstantInt::get(CTy, 0xFFFFFFFFull))}, M);
        return M;
      }
      int32_t Keep = Bits == 8 ? 0xFF : 0xFFFF;
      std::string M =
          memBinop("LogicalAnd", S, i32Constant(Keep), "sat_m", UF.Blocks[Idx]);
      if (!Signed)
        return M;
      int32_t Shift = 32 - static_cast<int32_t>(Bits);
      std::string L =
          memBinop("LeftShift", M, i32Constant(Shift), "sat_l", UF.Blocks[Idx]);
      return memBinop("RightShift", L, i32Constant(Shift), "sat_r",
                      UF.Blocks[Idx]);
    };

    // Two clamped compares. The upper bound first, so the lower bound sees a
    // value that is already at most the maximum.
    auto Clamp = [&](const std::string &V, int64_t Lo, uint64_t Hi)
        -> std::string {
      std::string HiC = constantSlot(ConstantInt::get(CTy, Hi));
      std::string CmpHi = freshSlot("sat_ch", Type::getInt1Ty(C));
      emitExtern(UF.Blocks[Idx], CmpSig2("GreaterThan"), {V, HiC}, CmpHi);
      std::string HiSel = freshSlot("sat_hi", CTy);
      unsigned NewIdx = emitDiamond(UF, Idx, CmpHi, HiC, V, HiSel, "sat_hi");
      Idx = NewIdx;
      std::string LoC = constantSlot(ConstantInt::getSigned(CTy, Lo));
      std::string CmpLo = freshSlot("sat_cl", Type::getInt1Ty(C));
      emitExtern(UF.Blocks[Idx], CmpSig2("LessThan"), {HiSel, LoC}, CmpLo);
      std::string Out = freshSlot("sat_out", CTy);
      Idx = emitDiamond(UF, Idx, CmpLo, LoC, HiSel, Out, "sat_lo");
      return Out;
    };

    uint64_t Hi = Signed ? ((1ull << (Bits - 1)) - 1) : ((Bits == 32)
                                                            ? 0xFFFFFFFFull
                                                            : ((1ull << Bits) - 1));
    int64_t Lo = Signed ? -(int64_t)(1ull << (Bits - 1)) : 0;

    std::string A = ToCompute(I.getOperand(0));
    std::string Bv = ToCompute(I.getOperand(1));
    std::string Sum = freshSlot("sat_sum", CTy);
    emitExtern(UF.Blocks[Idx], BinSig(IsAdd ? "Addition" : "Subtraction"),
               {A, Bv}, Sum);
    std::string Clamped = Clamp(Sum, Lo, Hi);
    if (Wide) {
      std::string Narrowed = emitTruncI64ToI32(Clamped, UF.Blocks[Idx]);
      emitCopy(UF.Blocks[Idx], Narrowed, slotFor(&I));
    } else {
      emitCopy(UF.Blocks[Idx], Clamped, slotFor(&I));
    }
    return true;
  }

  // fabs: the host has an absolute value for both floating types, and it is
  // exact (it only clears the sign bit), so there is nothing to derive.
  case Intrinsic::fabs: {
    Type *Ty = I.getType();
    if (!Ty->isFloatTy() && !Ty->isDoubleTy())
      reportUDONError("llvm.fabs on a non-scalar type is unsupported: " +
                      I.getFunction()->getName());
    std::string Prefix = typePrefix(Ty);
    emitExtern(B, "SystemMath.__Abs__" + Prefix + "__" + Prefix,
               {slotFor(I.getOperand(0))}, slotFor(&I));
    return true;
  }

  // sqrt: SystemMath.__Sqrt exists for both floating types, verified. Scalar
  // only; a vector sqrt reaches the scalarizer in the same way a vector add
  // does, so this case needs no vector handling of its own.
  case Intrinsic::sqrt: {
    Type *Ty = I.getType();
    if (!Ty->isFloatTy() && !Ty->isDoubleTy())
      reportUDONError("llvm.sqrt on a non-scalar type is unsupported: " +
                      I.getFunction()->getName());
    std::string Prefix = typePrefix(Ty);
    emitExtern(B, "SystemMath.__Sqrt__" + Prefix + "__" + Prefix,
               {slotFor(I.getOperand(0))}, slotFor(&I));
    return true;
  }

  // llvm.fmuladd: a * b + c. A fused form would need a single rounding step,
  // which the host's Single/Double operators do not offer, so it is lowered as
  // separate multiply and add. That is exactly what the intrinsic permits when
  // the target cannot fuse.
  case Intrinsic::fmuladd: {
    Type *Ty = I.getType();
    if (Ty->isVectorTy())
      reportUDONError(
          "llvm.fmuladd on a vector is not supported: " +
          I.getFunction()->getName());
    std::string Prefix = typePrefix(Ty);
    if (Prefix.empty())
      reportUDONError("unsupported llvm.fmuladd type in " +
                      I.getFunction()->getName());
    std::string Mul = freshSlot("fma_m", Ty);
    emitExtern(B, binSig(Prefix, "Multiplication"),
               {slotFor(I.getOperand(0)), slotFor(I.getOperand(1))}, Mul);
    emitExtern(B, binSig(Prefix, "Addition"),
               {Mul, slotFor(I.getOperand(2))}, slotFor(&I));
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
                    II->getCalledFunction()->getName() + " in " +
                    I.getFunction()->getName());
  }
}

void UDONWriter::selectMemoryIntrinsic(const Instruction &I, UFunction &UF,
                                       unsigned &Idx) {
  UBlock &B = UF.Blocks[Idx];
  const auto *MI = cast<MemIntrinsic>(&I);
  std::string Length = slotFor(MI->getLength());
  std::string Dst = pointerOffset(MI->getRawDest(), B);

  if (const auto *MT = dyn_cast<MemTransferInst>(&I)) {
    std::string Src = pointerOffset(MT->getRawSource(), UF.Blocks[Idx]);
    // A byte loop, not `SystemArray.__Copy__`: that extern copies array
    // ELEMENTS, and `__mem` is a word array, so a byte-granular copy would move
    // four times too much. The loop lowers to `load i8`/`store i8`, which are
    // correct at any alignment, and it handles overlap (memmove) for free
    // because it walks forwards from `src`.
    const Function *Helper = Mod->getFunction("__udon_memcpy_fill");
    if (!Helper)
      reportUDONError("runtime helper is missing: __udon_memcpy_fill");
    Idx = selectDirectCall(Idx, Helper, {Dst, Src, Length}, nullptr, &I);
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
  // turns into `__udon_memcpy_fill`, so a non-zero fill is a doubling byte copy.
  const Function *Helper = Mod->getFunction("__udon_memset_fill");
  if (!Helper)
    reportUDONError("runtime helper is missing: __udon_memset_fill");
  Idx = selectDirectCall(Idx, Helper, {Dst, slotFor(MS->getValue()), Length},
                         nullptr, &I);
}
