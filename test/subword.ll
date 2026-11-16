; Sub-word access: u8 store/load pairs, signed i8 sign extension, and an i16 at
; an odd offset. rustc emits exactly these shapes for `bool`, `u8` and enum
; discriminants.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

@g_buf = internal global [16 x i8] zeroinitializer, align 8
@g_u8 = internal global i8 0, align 1
@g_s8 = internal global i8 0, align 1
@g_i16 = internal global i16 0, align 2

define i32 @udon_event_start() {
entry:
  ; u8 round trip at offset 0.
  %p0 = getelementptr inbounds [16 x i8], ptr @g_buf, i32 0, i32 0
  store i8 -56, ptr %p0, align 1
  %v0 = load i8, ptr %p0, align 1
  store i8 %v0, ptr @g_u8, align 1

  ; signed i8: 0x80 must sign-extend to -128, not 128.
  %p1 = getelementptr inbounds [16 x i8], ptr @g_buf, i32 0, i32 1
  store i8 -128, ptr %p1, align 1
  %v1 = load i8, ptr %p1, align 1
  store i8 %v1, ptr @g_s8, align 1
  %s1 = sext i8 %v1 to i32

  ; i16 at an odd offset.
  %p3 = getelementptr inbounds [16 x i8], ptr @g_buf, i32 0, i32 3
  store i16 25960, ptr %p3, align 1
  %v3 = load i16, ptr %p3, align 1
  store i16 %v3, ptr @g_i16, align 2
  %z3 = zext i16 %v3 to i32

  ; The neighbours of the odd i16 must be untouched: store a marker either side.
  %p2 = getelementptr inbounds [16 x i8], ptr @g_buf, i32 0, i32 2
  %p5 = getelementptr inbounds [16 x i8], ptr @g_buf, i32 0, i32 5
  store i8 17, ptr %p2, align 1
  store i8 34, ptr %p5, align 1
  %v2 = load i8, ptr %p2, align 1
  %v5 = load i8, ptr %p5, align 1
  %z2 = zext i8 %v2 to i32
  %z5 = zext i8 %v5 to i32

  %u8 = load i8, ptr @g_u8, align 1
  %zu8 = zext i8 %u8 to i32
  %t0 = add i32 %zu8, %s1
  %t1 = add i32 %t0, %z3
  %t2 = add i32 %t1, %z2
  %t3 = add i32 %t2, %z5
  ret i32 %t3
}

; 0xC8 = 200
; -128
; 25960 (0x6568)
; 17
; 34
; 200 - 128 = 72; 72 + 25960 = 26032; + 17 = 26049; + 34 = 26083
; OUT: ret _start = 26083
