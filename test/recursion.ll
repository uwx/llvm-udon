; A self-recursive function: factorial of 5, which needs the caller-save
; discipline because the callee writes the caller's argument and return slots.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

define internal i32 @fact(i32 %n) {
entry:
  %base = icmp sle i32 %n, 1
  br i1 %base, label %one, label %recurse

one:
  ret i32 1

recurse:
  %m = sub i32 %n, 1
  %sub = call i32 @fact(i32 %m)
  %r = mul i32 %n, %sub
  ret i32 %r
}

define i32 @udon_event_start() {
entry:
  %r = call i32 @fact(i32 5)
  ret i32 %r
}

; OUT: ret _start = 120
