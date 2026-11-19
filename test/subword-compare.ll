; Sub-word comparisons and constant addresses.
;
; Both were miscompiled, and both are shapes clang produces for ordinary C.
;
;   1. `icmp` on i8/i16 needs its operands extended in the comparison's sense
;      before comparing, because a value of a narrow type lives in a 32-bit slot
;      and its high bits are whatever the producing instruction left there.
;      `(char)0xD0 < 10` unsigned is FALSE (208 vs 10), but a sign-extended -48
;      is less than 10 in a signed 32-bit compare, and the unsigned XOR trick
;      cannot fix it: the sign bit of a byte is not bit 31. atoi's digit test is
;      exactly this shape, and with the bug it never terminated -- the pointer
;      walked past the end of memory.
;
;   2. A folded `getelementptr` constant. C folds `&buf[2]` and `(char *)x + 4`
;      into a ConstantExpr, which was rejected outright.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

@buf = internal global [4 x i8] zeroinitializer, align 4

define i32 @main() {
entry:
  %slot = alloca i8, align 1
  store i8 0, ptr %slot, align 1
  %zero = load i8, ptr %slot, align 1

  ; 0 - 48 = 0xD0 = 208 unsigned, -48 signed.
  %d = sub i8 %zero, 48

  %u = icmp ult i8 %d, 10
  %s = icmp slt i8 %d, 10
  %a = zext i1 %u to i32
  %b = zext i1 %s to i32

  ; Same value built by negating, so it is not a constant the writer can fold.
  %neg = sub i8 0, 48
  %u2 = icmp ult i8 %neg, 10
  %c2 = zext i1 %u2 to i32

  ; i16: 0 - 2 = 65534 unsigned, -2 signed.
  %w = sub i16 0, 2
  %uw = icmp ult i16 %w, 10
  %sw = icmp slt i16 %w, 10
  %d1 = zext i1 %uw to i32
  %d2 = zext i1 %sw to i32

  ; A constant address: the byte at buf + 2.
  %p = getelementptr inbounds [4 x i8], ptr @buf, i32 0, i32 2
  store i8 7, ptr %p, align 1
  %back = load i8, ptr %p, align 1
  %e = zext i8 %back to i32

  %t0 = mul i32 %a, 10000
  %t1 = mul i32 %b, 1000
  %t2 = mul i32 %c2, 100
  %t3 = mul i32 %d1, 10
  %t4 = add i32 %t0, %t1
  %t5 = add i32 %t4, %t2
  %t6 = add i32 %t5, %t3
  %t7 = add i32 %t6, %d2
  %t8 = add i32 %t7, %e
  ret i32 %t8
}

; a = 0 (208 < 10 unsigned is false)
; b = 1 (-48 < 10 signed is true)
; c2 = 0 (same as a, built from a negation)
; d1 = 0 (65534 < 10 unsigned is false)
; d2 = 1 (-2 < 10 signed is true)
; e  = 7 (read back through a folded constant address)
; 0*10000 + 1*1000 + 0*100 + 0*10 + 1 + 7 = 1008
; OUT: ret _start = 1008
