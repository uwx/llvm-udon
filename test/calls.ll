; Calls: a two-level chain with an argument and a return value, and recursion.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

define internal i32 @add3(i32 %a, i32 %b, i32 %c) {
entry:
  %t = add i32 %a, %b
  %u = add i32 %t, %c
  ret i32 %u
}

define internal i32 @twice(i32 %x) {
entry:
  %r = call i32 @add3(i32 %x, i32 %x, i32 %x)
  ret i32 %r
}

define internal i32 @factorial(i32 %n) {
entry:
  %base = icmp sle i32 %n, 1
  br i1 %base, label %one, label %recurse

one:
  ret i32 1

recurse:
  %m = sub i32 %n, 1
  %sub = call i32 @factorial(i32 %m)
  %r = mul i32 %n, %sub
  ret i32 %r
}

define i32 @udon_event_start() {
entry:
  %a = call i32 @twice(i32 4)
  %f = call i32 @factorial(i32 5)
  %t = add i32 %a, %f
  ret i32 %t
}

; twice(4) = 4+4+4 = 12
; factorial(5) = 120
; 12 + 120 = 132
; OUT: ret _start = 132
