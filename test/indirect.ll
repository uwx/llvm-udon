; A call through a function pointer loaded from a global.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

@g_fp = internal global ptr null, align 4

define internal i32 @target(i32 %x) {
entry:
  %r = mul i32 %x, 3
  ret i32 %r
}

define internal i32 @caller(i32 %x) {
entry:
  %fp = load ptr, ptr @g_fp, align 4
  %r = call i32 %fp(i32 %x)
  ret i32 %r
}

define i32 @udon_event_start() {
entry:
  store ptr @target, ptr @g_fp, align 4
  %r = call i32 @caller(i32 7)
  ret i32 %r
}

; 7 * 3 = 21
; OUT: ret _start = 21
