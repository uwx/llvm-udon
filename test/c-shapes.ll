; Shapes that clang emits for ordinary C, none of which the hand-written
; fixtures covered. Each one was a real miscompile of compiled C.
;
;   1. Unnamed basic blocks. clang names almost nothing, so a label built from
;      the block name collapses to `<fn>_` for every unnamed block and collides
;      with the edge trampoline `<pred>_to_<succ>`. Labels are positional now.
;
;   2. `optnone` on a definition. It makes the legacy pass manager SKIP the
;      function, including the critical-edge splitting this backend depends on,
;      so the attribute is cleared before the pipeline runs. Every -O0 C
;      function carries it.
;
;   3. Two calls to the same recursive function, where the first call's result
;      is an OPERAND of an instruction after the second call. The result's slot
;      belongs to the function, so the nested activation overwrites it; it has
;      to be spilled, and spilling only instructions *defined* after the call
;      misses exactly this case.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

; A two-call recursion, in the shape clang gives fibonacci. The block names are
; the numbers clang uses, and the definition carries `optnone`.
define internal i32 @fib(i32 %0) #0 {
  %2 = icmp slt i32 %0, 2
  br i1 %2, label %11, label %3

3:                                                ; preds = %1, %3
  %4 = phi i32 [ %8, %3 ], [ %0, %1 ]
  %5 = phi i32 [ %9, %3 ], [ 0, %1 ]
  %6 = add nsw i32 %4, -1
  %7 = call i32 @fib(i32 %6)
  %8 = add nsw i32 %4, -2
  %9 = add nsw i32 %7, %5
  %10 = icmp ult i32 %4, 4
  br i1 %10, label %11, label %3

11:                                               ; preds = %3, %1
  %12 = phi i32 [ 0, %1 ], [ %9, %3 ]
  %13 = phi i32 [ %0, %1 ], [ %8, %3 ]
  %14 = add nsw i32 %13, %12
  ret i32 %14
}

; A callee with no phis and no block names: exercises the label collision on
; its own, independent of the recursion.
define internal i32 @twice(i32 %0) #0 {
  %2 = add i32 %0, %0
  %3 = icmp sgt i32 %2, 100
  br i1 %3, label %4, label %6

4:
  %5 = sub i32 %2, 100
  br label %6

6:
  %7 = phi i32 [ 100, %4 ], [ %2, %1 ]
  ret i32 %7
}

define i32 @main() {
  %f = call i32 @fib(i32 10)
  %t = call i32 @twice(i32 40)
  %r = add i32 %f, %t
  ret i32 %r
}

attributes #0 = { noinline nounwind optnone }

; fib(10) = 55, twice(40) = 80, so 135.
; OUT: ret _start = 135
