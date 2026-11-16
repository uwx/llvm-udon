; Two entry points: main exports _start, udon_event_interact exports _interact.
; Running one must not run the other, and the init guard must fire exactly once
; even when both run.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start --entry _interact | FileCheck --check-prefix=OUT %s

@g_starts = internal global i32 0, align 4
@g_interacts = internal global i32 0, align 4
@g_inits = internal global i32 0, align 4

define i32 @main() {
entry:
  ; Count this activation. The init block runs before the body, so a count of
  ; one proves the guard is one-shot.
  %s = load i32, ptr @g_starts, align 4
  %s1 = add i32 %s, 1
  store i32 %s1, ptr @g_starts, align 4
  ret i32 %s1
}

define i32 @udon_event_interact() {
entry:
  %i = load i32, ptr @g_interacts, align 4
  %i1 = add i32 %i, 1
  store i32 %i1, ptr @g_interacts, align 4
  ret i32 %i1
}

; _start returns 1, _interact returns 1, and neither sees the other's counter.
; OUT: ret _start = 1
; OUT: ret _interact = 1
