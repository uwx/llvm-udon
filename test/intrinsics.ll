; Intrinsic expansion: min/max/abs/bit counting/byte swap/rotate.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

declare i32 @llvm.umin.i32(i32, i32)
declare i32 @llvm.umax.i32(i32, i32)
declare i32 @llvm.smin.i32(i32, i32)
declare i32 @llvm.smax.i32(i32, i32)
declare i32 @llvm.abs.i32(i32, i1)
declare i32 @llvm.ctlz.i32(i32, i1)
declare i32 @llvm.cttz.i32(i32, i1)
declare i32 @llvm.ctpop.i32(i32)
declare i32 @llvm.bswap.i32(i32)
declare i32 @llvm.fshl.i32(i32, i32, i32)

@g_r = internal global i32 0, align 4

define i32 @udon_event_start() {
entry:
  ; umin(-1, 1): as unsigned, 1 is smaller
  %umin = call i32 @llvm.umin.i32(i32 -1, i32 1)
  ; umax(-1, 1): as unsigned, -1 (0xFFFFFFFF) is larger
  %umax = call i32 @llvm.umax.i32(i32 -1, i32 1)
  ; smin(-5, 3) = -5
  %smin = call i32 @llvm.smin.i32(i32 -5, i32 3)
  ; smax(-5, 3) = 3
  %smax = call i32 @llvm.smax.i32(i32 -5, i32 3)
  ; abs(-9) = 9
  %abs = call i32 @llvm.abs.i32(i32 -9, i1 false)
  ; ctlz(1) = 31
  %clz = call i32 @llvm.ctlz.i32(i32 1, i1 false)
  ; cttz(8) = 3
  %ctz = call i32 @llvm.cttz.i32(i32 8, i1 false)
  ; ctpop(0xF0F0) = 8
  %pop = call i32 @llvm.ctpop.i32(i32 61680)
  ; bswap(0x11223344) = 0x44332211 = 1144201745
  %bsw = call i32 @llvm.bswap.i32(i32 287454020)
  ; fshl(1, 0, 1) = 2
  %rot = call i32 @llvm.fshl.i32(i32 1, i32 0, i32 1)

  %t0 = add i32 %umin, %umax
  %t1 = add i32 %t0, %smin
  %t2 = add i32 %t1, %smax
  %t3 = add i32 %t2, %abs
  %t4 = add i32 %t3, %clz
  %t5 = add i32 %t4, %ctz
  %t6 = add i32 %t5, %pop
  %t7 = add i32 %t6, %bsw
  %t8 = add i32 %t7, %rot
  ret i32 %t8
}

; 1 + -1 + -5 + 3 + 9 + 31 + 3 + 8 + 1144201745 + 2
; = 1 - 1 = 0; 0 - 5 = -5; -5 + 3 = -2; -2 + 9 = 7; 7 + 31 = 38;
; 38 + 3 = 41; 41 + 8 = 49; 49 + 1144201745 = 1144201794; + 2 = 1144201796
; OUT: ret _start = 1144201796
