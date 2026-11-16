; i64: truncation, signed remainder, unsigned division and remainder in both
; branches, shifts, and the i1 sign extension.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

@g_trunc = internal global i32 0, align 4
@g_srem = internal global i32 0, align 4
@g_udiv = internal global i32 0, align 4
@g_urem = internal global i32 0, align 4
@g_udiv2 = internal global i32 0, align 4
@g_lshr0 = internal global i32 0, align 4
@g_lshr1 = internal global i32 0, align 4
@g_lshrv = internal global i32 0, align 4
@g_sext1 = internal global i32 0, align 4

define i32 @udon_event_start() {
entry:
  ; trunc 0x123456789ABCDEF -> 0x89ABCDEF = -1985229329
  %tr = trunc i64 81985529216486895 to i32
  store i32 %tr, ptr @g_trunc, align 4

  ; srem -100 % 7 = -2
  %sr = srem i64 -100, 7
  %srw = trunc i64 %sr to i32
  store i32 %srw, ptr @g_srem, align 4

  ; udiv 0xFFFFFFFFFFFFFFFF / 0x8000000000000001 = 1 (divisor-high-bit path)
  %ud = udiv i64 -1, -9223372036854775807
  %udw = trunc i64 %ud to i32
  store i32 %udw, ptr @g_udiv, align 4

  ; urem of the same = 0x7FFFFFFFFFFFFFFE -> low word 0xFFFFFFFE = -2
  %ur = urem i64 -1, -9223372036854775807
  %urw = trunc i64 %ur to i32
  store i32 %urw, ptr @g_urem, align 4

  ; udiv (2^63 + 10) / 3 = 3074457345618258606 (general path); low word
  ; 3074457345618258606 & 0xFFFFFFFF = 0xAAAAAAAA = -1431655766
  %ud2 = udiv i64 -9223372036854775798, 3
  %ud2w = trunc i64 %ud2 to i32
  store i32 %ud2w, ptr @g_udiv2, align 4

  ; lshr by 0 is a copy; lshr by 1 of 0xFF00000000000000 = 0x7F80000000000000
  %l0 = lshr i64 42, 0
  %l0w = trunc i64 %l0 to i32
  store i32 %l0w, ptr @g_lshr0, align 4

  %l1 = lshr i64 -72057594037927936, 1
  %l1w = trunc i64 %l1 to i32
  store i32 %l1w, ptr @g_lshr1, align 4

  ; lshr by a variable count.
  %lv = lshr i64 256, 4
  %lvw = trunc i64 %lv to i32
  store i32 %lvw, ptr @g_lshrv, align 4

  ; sext i1 true = -1
  %st = sext i1 true to i32
  store i32 %st, ptr @g_sext1, align 4

  %a = load i32, ptr @g_trunc, align 4
  %b = load i32, ptr @g_srem, align 4
  %c = load i32, ptr @g_udiv, align 4
  %d = load i32, ptr @g_urem, align 4
  %e = load i32, ptr @g_udiv2, align 4
  %f = load i32, ptr @g_lshr0, align 4
  %g = load i32, ptr @g_lshr1, align 4
  %h = load i32, ptr @g_lshrv, align 4
  %i = load i32, ptr @g_sext1, align 4
  %t0 = add i32 %a, %b
  %t1 = add i32 %t0, %c
  %t2 = add i32 %t1, %d
  %t3 = add i32 %t2, %e
  %t4 = add i32 %t3, %f
  %t5 = add i32 %t4, %g
  %t6 = add i32 %t5, %h
  %t7 = add i32 %t6, %i
  ret i32 %t7
}

; trunc: 0x89ABCDEF = -1985229329
; srem: -2
; udiv (divisor high bit): 1
; urem (divisor high bit): low word 0xFFFFFFFE = -2
; udiv2 low word: 0xAAAAAAAA = -1431655766
; lshr by 0: 42
; lshr by 1: 0x7F80000000000000 -> low word 0
; lshr by 4: 16
; sext i1: -1
; Sum with i32 wrapping = 878082259
; OUT: ret _start = 878082259
