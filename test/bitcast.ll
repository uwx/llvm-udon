; bitcast through the reserved scratch bytes of __mem: i32<->f32 and i64<->f64.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

define i32 @udon_event_start() {
entry:
  ; 1.5f is 0x3FC00000 = 1069547520
  %bits = bitcast float 1.5 to i32
  ; and back: the same value as a float, whose bits must match again.
  %back = bitcast i32 %bits to float
  %again = bitcast float %back to i32
  ; 2.25 as a double is 0x4002000000000000; low word 0.
  %dbits = bitcast double 2.25 to i64
  %dlow = trunc i64 %dbits to i32
  %dback = bitcast i64 %dbits to double
  %dbits2 = bitcast double %dback to i64
  %dlow2 = trunc i64 %dbits2 to i32
  ; A negative-zero round trip: -0.0f is 0x80000000.
  %nz = bitcast float -0.0 to i32
  %nzback = bitcast i32 %nz to float
  %nzagain = bitcast float %nzback to i32

  %t0 = add i32 %bits, %again
  %t1 = add i32 %t0, %dlow
  %t2 = add i32 %t1, %dlow2
  %t3 = add i32 %t2, %nzagain
  ret i32 %t3
}

; 1069547520 + 1069547520 = 2139095040 -> wraps to -2147483648 + ... let me
; compute: 2139095040 < 2^31 so it is positive.
; + 0 = 2139095040
; + 0 = 2139095040
; + 0x80000000 = 2139095040 + -2147483648 = -8388608
; OUT: ret _start = -8388608
