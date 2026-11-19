; A frame larger than the shadow stack must halt with reason 2, not throw a
; bounds exception.
;
; Run with a deliberately small shadow region so a fixed-size frame overflows:
;   llvm-udon %s -o %t.uasm -udon-shadow-stack-bytes=1024
;
; The writer's pipeline runs SROA after inlining, which would promote a
; single-element store/load array away, so this fixture keeps its address
; escaping to guarantee the alloca survives to the shadow stack. It is testing
; the writer's prologue bounds check, which needs a real frame to refuse.
;
; RUN: llvm-udon %s -o %t.uasm -udon-shadow-stack-bytes=1024
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

@escape = internal global ptr null, align 4

define internal i32 @big_frame() {
entry:
  ; 4096 bytes of local array: four times the 1024-byte shadow region, so the
  ; prologue's bounds check must fail.
  %arr = alloca [1024 x i32], align 4
  %p = getelementptr inbounds [1024 x i32], ptr %arr, i32 0, i32 0
  store i32 5, ptr %p, align 4
  %v = load i32, ptr %p, align 4
  ; A dead store of the address would be folded, so it is volatile: SROA
  ; cannot promote an alloca whose address escapes.
  store volatile ptr %p, ptr @escape, align 4
  ret i32 %v
}

define i32 @udon_event_start() {
entry:
  %r = call i32 @big_frame()
  ret i32 %r
}

; The frame is refused before it is committed, so `__sp` is untouched and the
; halt reports the shadow-stack reason rather than a bounds exception.
; OUT: HALT _start 2
