; The alloca shadow stack: a local array, an sret struct return, two calls in a
; row that must not alias, and recursion whose levels must not alias either.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

%Pair = type { i32, i32 }

define internal void @fill_pair(ptr sret(%Pair) %out, i32 %a, i32 %b) {
entry:
  %pa = getelementptr inbounds %Pair, ptr %out, i32 0, i32 0
  %pb = getelementptr inbounds %Pair, ptr %out, i32 0, i32 1
  store i32 %a, ptr %pa, align 4
  store i32 %b, ptr %pb, align 4
  ret void
}

; A local array: eight slots, filled with i*3, summed.
define internal i32 @local_array() {
entry:
  %arr = alloca [8 x i32], align 4
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %p = getelementptr inbounds [8 x i32], ptr %arr, i32 0, i32 %i
  %triple = mul i32 %i, 3
  store i32 %triple, ptr %p, align 4
  %next = add i32 %i, 1
  %done = icmp eq i32 %next, 8
  br i1 %done, label %sum, label %loop

sum:
  %s0 = load i32, ptr %arr, align 4
  %p1 = getelementptr inbounds [8 x i32], ptr %arr, i32 0, i32 1
  %s1 = load i32, ptr %p1, align 4
  %p2 = getelementptr inbounds [8 x i32], ptr %arr, i32 0, i32 2
  %s2 = load i32, ptr %p2, align 4
  %p3 = getelementptr inbounds [8 x i32], ptr %arr, i32 0, i32 3
  %s3 = load i32, ptr %p3, align 4
  %p4 = getelementptr inbounds [8 x i32], ptr %arr, i32 0, i32 4
  %s4 = load i32, ptr %p4, align 4
  %p5 = getelementptr inbounds [8 x i32], ptr %arr, i32 0, i32 5
  %s5 = load i32, ptr %p5, align 4
  %p6 = getelementptr inbounds [8 x i32], ptr %arr, i32 0, i32 6
  %s6 = load i32, ptr %p6, align 4
  %p7 = getelementptr inbounds [8 x i32], ptr %arr, i32 0, i32 7
  %s7 = load i32, ptr %p7, align 4
  %a0 = add i32 %s0, %s1
  %a1 = add i32 %a0, %s2
  %a2 = add i32 %a1, %s3
  %a3 = add i32 %a2, %s4
  %a4 = add i32 %a3, %s5
  %a5 = add i32 %a4, %s6
  %a6 = add i32 %a5, %s7
  ret i32 %a6
}

; Recursion depth 3: each level writes its own value and reads it back after the
; inner call, which fails if the frames alias.
define internal i32 @recurse(i32 %depth) {
entry:
  %slot = alloca i32, align 4
  store i32 %depth, ptr %slot, align 4
  %base = icmp eq i32 %depth, 0
  br i1 %base, label %done, label %deeper

deeper:
  %inner = sub i32 %depth, 1
  %r = call i32 @recurse(i32 %inner)
  br label %done

done:
  %mine = load i32, ptr %slot, align 4
  %seed = phi i32 [ 0, %entry ], [ %r, %deeper ]
  %out = add i32 %seed, %mine
  ret i32 %out
}

define i32 @udon_event_start() {
entry:
  %pair = alloca %Pair, align 4
  call void @fill_pair(ptr sret(%Pair) %pair, i32 10, i32 20)
  %pa = getelementptr inbounds %Pair, ptr %pair, i32 0, i32 0
  %pb = getelementptr inbounds %Pair, ptr %pair, i32 0, i32 1
  %a = load i32, ptr %pa, align 4
  %b = load i32, ptr %pb, align 4

  %arr = call i32 @local_array()

  %t0 = add i32 %a, %b
  %t1 = add i32 %t0, %arr
  ret i32 %t1
}

; pair: 10 + 20 = 30
; local_array: 0+3+6+9+12+15+18+21 = 84
; 30 + 84 = 114
;
; Recursion is deliberately NOT part of this fixture: a recursive call needs the
; caller-save discipline of Step 6, and until then the callee clobbers the
; caller's argument slot. `test/recursion.ll` covers it.
; OUT: ret _start = 114
