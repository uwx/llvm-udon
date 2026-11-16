; Address-taken A calls address-taken B through a pointer; B must return to A,
; which returns to the entry. This is the regression for the transfer-slot
; overwrite: without the callee copying __icall_* into its own slots, A returns
; to B's caller.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

@g_fp = internal global ptr null, align 4

define internal i32 @b(i32 %x) {
entry:
  %r = add i32 %x, 100
  ret i32 %r
}

define internal i32 @a(i32 %x) {
entry:
  %fp = load ptr, ptr @g_fp, align 4
  %inner = call i32 %fp(i32 %x)
  ; If b overwrote A's return slot, this add never runs.
  %r = add i32 %inner, 1
  ret i32 %r
}

define i32 @udon_event_start() {
entry:
  store ptr @b, ptr @g_fp, align 4
  %r = call i32 @a(i32 5)
  ret i32 %r
}

; 5 + 100 + 1 = 106
; OUT: ret _start = 106
