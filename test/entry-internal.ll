; An entry point calling another entry point's body: the internal call must
; return through the caller's return address, not halt the event.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _interact | FileCheck --check-prefix=OUT %s

define internal i32 @helper(i32 %x) {
entry:
  %r = mul i32 %x, 2
  ret i32 %r
}

define i32 @main() {
entry:
  ; `main` is an entry point and is also called from the interact body below.
  %r = call i32 @helper(i32 21)
  ret i32 %r
}

define i32 @udon_event_interact() {
entry:
  ; Call the entry point `main` internally: it must return here, through the
  ; caller's return address, rather than halting the event.
  %inner = call i32 @main()
  %r = add i32 %inner, 1
  ret i32 %r
}

; helper(21) = 42, then + 1 = 43
; OUT: ret _interact = 43
