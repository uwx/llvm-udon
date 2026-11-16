; i32 arithmetic, compares, branches, select and phi.
;
; Every case writes its result into a global, and the entry point returns the
; last one, so the harness's `ret _start = <n>` line is the whole check.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

@g_sum = internal global i32 0, align 4
@g_shl = internal global i32 0, align 4
@g_lshr = internal global i32 0, align 4
@g_ashr = internal global i32 0, align 4
@g_cmp = internal global i32 0, align 4
@g_sel = internal global i32 0, align 4
@g_phi = internal global i32 0, align 4
@g_diamond = internal global i32 0, align 4

define internal void @arith() {
entry:
  %a = add i32 40, 2
  %b = sub i32 %a, 5
  %c = mul i32 %b, 3
  %d = sdiv i32 %c, 2
  %e = srem i32 %d, 7
  store i32 %e, ptr @g_sum, align 4
  ret void
}

define internal void @shifts() {
entry:
  %s = shl i32 5, 3
  store i32 %s, ptr @g_shl, align 4
  %l = lshr i32 240, 2
  store i32 %l, ptr @g_lshr, align 4
  %r = ashr i32 -16, 2
  store i32 %r, ptr @g_ashr, align 4
  ret void
}

define internal void @compares() {
entry:
  %lt = icmp slt i32 3, 7
  %ult = icmp ult i32 -1, 1
  %and = and i1 %lt, %ult
  %zext = zext i1 %and to i32
  store i32 %zext, ptr @g_cmp, align 4
  ret void
}

define internal void @selects() {
entry:
  %cond = icmp eq i32 4, 4
  %v = select i1 %cond, i32 11, i32 22
  store i32 %v, ptr @g_sel, align 4
  ret void
}

define internal void @phis(i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %accnext, %loop ]
  %accnext = add i32 %acc, %i
  %next = add i32 %i, 1
  %done = icmp eq i32 %next, %n
  br i1 %done, label %exit, label %loop
exit:
  store i32 %accnext, ptr @g_phi, align 4
  ret void
}

define internal void @diamond(i32 %x) {
entry:
  %big = icmp sgt i32 %x, 10
  br i1 %big, label %then, label %else
then:
  br label %join
else:
  br label %join
join:
  %r = phi i32 [ 100, %then ], [ 200, %else ]
  store i32 %r, ptr @g_diamond, align 4
  ret void
}

define i32 @udon_event_start() {
entry:
  call void @arith()
  call void @shifts()
  call void @compares()
  call void @selects()
  call void @phis(i32 5)
  call void @diamond(i32 42)
  %sum = load i32, ptr @g_sum, align 4
  %shl = load i32, ptr @g_shl, align 4
  %lshr = load i32, ptr @g_lshr, align 4
  %ashr = load i32, ptr @g_ashr, align 4
  %cmp = load i32, ptr @g_cmp, align 4
  %sel = load i32, ptr @g_sel, align 4
  %phi = load i32, ptr @g_phi, align 4
  %dia = load i32, ptr @g_diamond, align 4
  %t0 = add i32 %sum, %shl
  %t1 = add i32 %t0, %lshr
  %t2 = add i32 %t1, %ashr
  %t3 = add i32 %t2, %cmp
  %t4 = add i32 %t3, %sel
  %t5 = add i32 %t4, %phi
  %t6 = add i32 %t5, %dia
  ret i32 %t6
}

; g_sum: ((40+2-5)*3)/2 = 55; 55 % 7 = 6
; g_shl: 40, g_lshr: 60, g_ashr: -4, g_cmp: 0 (unsigned -1 < 1 is false),
; g_sel: 11, g_phi: 0+1+2+3+4 = 10, g_diamond: 100
; total: 6+40+60-4+0+11+10+100 = 223
; OUT: ret _start = 223
