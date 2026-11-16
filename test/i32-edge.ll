; i32 edge cases: the wrapping paths that only show up at the extremes.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

@g_rem = internal global i32 0, align 4
@g_lshr = internal global i32 0, align 4
@g_udiv1 = internal global i32 0, align 4
@g_urem = internal global i32 0, align 4
@g_ucmp = internal global i32 0, align 4
@g_udiv2 = internal global i32 0, align 4

define i32 @udon_event_start() {
entry:
  ; srem -7 % 3 = -1
  %rem = srem i32 -7, 3
  store i32 %rem, ptr @g_rem, align 4

  ; lshr 0xF0, 2 = 0x3C = 60
  %sh = lshr i32 240, 2
  store i32 %sh, ptr @g_lshr, align 4

  ; udiv 0xFFFFFFFF / 1 = 0xFFFFFFFF, which is -1 as a signed i32. A bare
  ; Convert.ToInt32 of the 64-bit quotient would throw here.
  %ud1 = udiv i32 -1, 1
  store i32 %ud1, ptr @g_udiv1, align 4

  ; urem 0xFFFFFFFF % 0x80000001 = 0x7FFFFFFE
  %ur = urem i32 -1, -2147483647
  store i32 %ur, ptr @g_urem, align 4

  ; unsigned -1 < 1 is false, so this is 0
  %uc = icmp ult i32 -1, 1
  %ucz = zext i1 %uc to i32
  store i32 %ucz, ptr @g_ucmp, align 4

  ; udiv 0xFFFFFFFF / 2 = 0x7FFFFFFF
  %ud2 = udiv i32 -1, 2
  store i32 %ud2, ptr @g_udiv2, align 4

  %r = load i32, ptr @g_rem, align 4
  %s = load i32, ptr @g_lshr, align 4
  %a = load i32, ptr @g_udiv1, align 4
  %b = load i32, ptr @g_urem, align 4
  %c = load i32, ptr @g_ucmp, align 4
  %d = load i32, ptr @g_udiv2, align 4
  %t0 = add i32 %r, %s
  %t1 = add i32 %t0, %a
  %t2 = add i32 %t1, %b
  %t3 = add i32 %t2, %c
  %t4 = add i32 %t3, %d
  ret i32 %t4
}

; -1 + 60 + -1 + 2147483646 + 0 + 2147483647
; = -1 + 60 = 59; 59 - 1 = 58; 58 + 2147483646 = 2147483704 -> wraps to
; -2147483592; -2147483592 + 0 = -2147483592; -2147483592 + 2147483647 = 55
; OUT: ret _start = 55
