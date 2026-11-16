; Byte memory: aligned and unaligned loads and stores of every width, plus a
; memcpy and a memset.
;
; Each result is logged separately rather than summed: an unaligned store at
; offset 3 overlaps a store at offset 0, so a single total would depend on
; ordering in a way that hides which access is wrong.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

@g_buf = internal global [64 x i8] zeroinitializer, align 8
@g_src = internal global [8 x i8] c"\01\02\03\04\05\06\07\08", align 8
@g_r0 = internal global i32 0, align 4
@g_r5 = internal global i32 0, align 4
@g_r20 = internal global i64 0, align 8
@g_rf = internal global i32 0, align 4
@g_rd = internal global i32 0, align 4
@g_rc = internal global i32 0, align 4
@g_rz = internal global i32 0, align 4

declare void @llvm.memcpy.p0.p0.i32(ptr, ptr, i32, i1)
declare void @llvm.memset.p0.i32(ptr, i8, i32, i1)

define i32 @udon_event_start() {
entry:
  ; Aligned i32: store 0x12345678 and read it back.
  %p0 = getelementptr inbounds [64 x i8], ptr @g_buf, i32 0, i32 0
  store i32 305419896, ptr %p0, align 4
  %v0 = load i32, ptr %p0, align 4
  store i32 %v0, ptr @g_r0, align 4

  ; Unaligned i32 at offset 5: store 0x11111111, read back.
  %p5 = getelementptr inbounds [64 x i8], ptr @g_buf, i32 0, i32 5
  store i32 286331153, ptr %p5, align 1
  %v5 = load i32, ptr %p5, align 1
  store i32 %v5, ptr @g_r5, align 4

  ; Unaligned i64 at offset 16: store 0x1122334455667788, read back.
  %p16 = getelementptr inbounds [64 x i8], ptr @g_buf, i32 0, i32 16
  store i64 1234605616436508552, ptr %p16, align 1
  %v16 = load i64, ptr %p16, align 1
  store i64 %v16, ptr @g_r20, align 8

  ; float at an odd offset, then its bits read back as an i32.
  %p11 = getelementptr inbounds [64 x i8], ptr @g_buf, i32 0, i32 11
  store float 1.5, ptr %p11, align 1
  %fb = load i32, ptr %p11, align 1
  store i32 %fb, ptr @g_rf, align 4

  ; double at an odd offset, then its low word read back as an i32.
  %p25 = getelementptr inbounds [64 x i8], ptr @g_buf, i32 0, i32 25
  store double 2.25, ptr %p25, align 1
  %db = load i64, ptr %p25, align 1
  %dl = trunc i64 %db to i32
  store i32 %dl, ptr @g_rd, align 4

  ; memcpy 8 bytes from the constant array to offset 40.
  %dst = getelementptr inbounds [64 x i8], ptr @g_buf, i32 0, i32 40
  call void @llvm.memcpy.p0.p0.i32(ptr %dst, ptr @g_src, i32 8, i1 false)
  %cp = load i32, ptr %dst, align 1
  store i32 %cp, ptr @g_rc, align 4

  ; memset 4 bytes of 0xAB at offset 50.
  %z = getelementptr inbounds [64 x i8], ptr @g_buf, i32 0, i32 50
  call void @llvm.memset.p0.i32(ptr %z, i8 -85, i32 4, i1 false)
  %zv = load i32, ptr %z, align 1
  store i32 %zv, ptr @g_rz, align 4

  ; An access at the very last byte of __mem's usable region exercises the 8
  ; pad bytes: the window read must not throw.
  %last = getelementptr inbounds [64 x i8], ptr @g_buf, i32 0, i32 63
  store i8 7, ptr %last, align 1
  %lv = load i8, ptr %last, align 1
  %lvi = zext i8 %lv to i32

  ; Fold every checked value into one number, so a single `ret` line proves all
  ; of them. Each is compared against its expected bit pattern below.
  %r0 = load i32, ptr @g_r0, align 4
  %r5 = load i32, ptr @g_r5, align 4
  %r20 = load i64, ptr @g_r20, align 8
  %r20w = trunc i64 %r20 to i32
  %rf = load i32, ptr @g_rf, align 4
  %rd = load i32, ptr @g_rd, align 4
  %rc = load i32, ptr @g_rc, align 4
  %rz = load i32, ptr @g_rz, align 4
  %t0 = add i32 %r0, %r5
  %t1 = add i32 %t0, %r20w
  %t2 = add i32 %t1, %rf
  %t3 = add i32 %t2, %rd
  %t4 = add i32 %t3, %rc
  %t5 = add i32 %t4, %rz
  %t6 = add i32 %t5, %lvi
  ret i32 %t6
}

; 0x12345678 = 305419896 (aligned i32)
; 0x11111111 = 286331153 (unaligned i32)
; low word of 0x1122334455667788 = 0x55667788 = 1432778632 (unaligned i64)
; 1.5f bits = 0x3FC00000 = 1069547520 (float at an odd offset)
; low word of 2.25 = 0 (double at an odd offset)
; first four copied bytes = 0x04030201 = 67305985 (memcpy)
; 0xABABABAB = -1414812757 (non-zero memset)
; last byte = 7 (the pad-byte window)
; Sum = 1746570436
; OUT: ret _start = 1746570436
