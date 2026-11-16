; The UDON runtime helpers.
;
; Hand-written LLVM IR, not C: there is no clang in the LLVM build tree this
; project targets, and adding a clang dependency to the build is forbidden. The
; writer links these with LinkOnlyNeeded and lowers them like user code, so each
; one is ordinary IR that the same selection path handles.
;
; What is here and why:
;   __udon_ctlz_i32/_i64, __udon_cttz_i32/_i64, __udon_ctpop_i32/_i64
;       Bit counting. The sequences are long enough to be worth writing once
;       rather than inlining at every use site.
;   __udon_memset_fill
;       A non-zero memset: one store, then doubling memcpy. The writer lowers
;       the memcpy to Array.Copy, so the loop needs no extern of its own.
;   __udon_strlen, __udon_memcmp
;       Ordinary byte loops, used by nothing in this milestone but part of the
;       runtime's contract.
;
; memcpy, memmove and a zero memset need no helper: they are single-extern
; lowerings in the writer.

target datalayout = "e-m:e-p:32:32-i64:64-i128:128-f32:32-f64:64-n32:64-S128"

define i32 @__udon_ctpop_i32(i32 %x) {
entry:
  ; Parallel bit count (Hacker's Delight 5-2), done with shifts and masks so it
  ; needs no intrinsic of its own.
  %a0 = lshr i32 %x, 1
  %a1 = and i32 %a0, 1431655765
  %a2 = sub i32 %x, %a1
  %b0 = and i32 %a2, 858993459
  %b1 = lshr i32 %a2, 2
  %b2 = and i32 %b1, 858993459
  %b3 = add i32 %b0, %b2
  %c0 = lshr i32 %b3, 4
  %c1 = add i32 %b3, %c0
  %c2 = and i32 %c1, 252645135
  %d0 = mul i32 %c2, 16843009
  %d1 = lshr i32 %d0, 24
  ret i32 %d1
}

define i64 @__udon_ctpop_i64(i64 %x) {
entry:
  %lo = trunc i64 %x to i32
  %hi64 = lshr i64 %x, 32
  %hi = trunc i64 %hi64 to i32
  %clo = call i32 @__udon_ctpop_i32(i32 %lo)
  %chi = call i32 @__udon_ctpop_i32(i32 %hi)
  %sum = add i32 %clo, %chi
  %wide = zext i32 %sum to i64
  ret i64 %wide
}

define i32 @__udon_ctlz_i32(i32 %x) {
entry:
  %iszero = icmp eq i32 %x, 0
  br i1 %iszero, label %zero, label %count

zero:
  ret i32 32

count:
  ; Isolate the highest set bit, then count the bits below it.
  %s1 = lshr i32 %x, 1
  %o1 = or i32 %x, %s1
  %s2 = lshr i32 %o1, 2
  %o2 = or i32 %o1, %s2
  %s3 = lshr i32 %o2, 4
  %o3 = or i32 %o2, %s3
  %s4 = lshr i32 %o3, 8
  %o4 = or i32 %o3, %s4
  %s5 = lshr i32 %o4, 16
  %o5 = or i32 %o4, %s5
  %pop = call i32 @__udon_ctpop_i32(i32 %o5)
  %leading = sub i32 32, %pop
  ret i32 %leading
}

define i64 @__udon_ctlz_i64(i64 %x) {
entry:
  %hi = lshr i64 %x, 32
  %hitop = trunc i64 %hi to i32
  %hizero = icmp eq i32 %hitop, 0
  br i1 %hizero, label %low, label %high

high:
  %h = call i32 @__udon_ctlz_i32(i32 %hitop)
  %hw = zext i32 %h to i64
  ret i64 %hw

low:
  %lotop = trunc i64 %x to i32
  %l = call i32 @__udon_ctlz_i32(i32 %lotop)
  %lw = zext i32 %l to i64
  %plus = add i64 %lw, 32
  ret i64 %plus
}

define i32 @__udon_cttz_i32(i32 %x) {
entry:
  %iszero = icmp eq i32 %x, 0
  br i1 %iszero, label %zero, label %count

zero:
  ret i32 32

count:
  ; x & -x isolates the lowest set bit; one less than that is all ones below
  ; it, whose popcount is the trailing-zero count.
  %neg = sub i32 0, %x
  %low = and i32 %x, %neg
  %below = sub i32 %low, 1
  %pop = call i32 @__udon_ctpop_i32(i32 %below)
  ret i32 %pop
}

define i64 @__udon_cttz_i64(i64 %x) {
entry:
  %lotop = trunc i64 %x to i32
  %lozero = icmp eq i32 %lotop, 0
  br i1 %lozero, label %high, label %low

high:
  %hi = lshr i64 %x, 32
  %hitop = trunc i64 %hi to i32
  %h = call i32 @__udon_cttz_i32(i32 %hitop)
  %hw = zext i32 %h to i64
  %plus = add i64 %hw, 32
  ret i64 %plus

low:
  %l = call i32 @__udon_cttz_i32(i32 %lotop)
  %lw = zext i32 %l to i64
  ret i64 %lw
}

define void @__udon_memset_fill(i32 %dst, i32 %value, i32 %length) {
entry:
  %empty = icmp eq i32 %length, 0
  br i1 %empty, label %done, label %first

first:
  ; One byte, then copy in chunks that double: 1, 2, 4, ... capped at what is
  ; left. The loop header carries the covered count, and the chunk is chosen
  ; with an explicit branch rather than a `select`, because a `select` splits a
  ; basic block and the back edge would then miss the split's join.
  %byte = trunc i32 %value to i8
  %dstp = inttoptr i32 %dst to ptr
  store i8 %byte, ptr %dstp, align 1
  br label %grow

grow:
  %covered = phi i32 [ 1, %first ], [ %covered.next, %cont ]
  %remaining = sub i32 %length, %covered
  %take = icmp ult i32 %remaining, %covered
  br i1 %take, label %small, label %large

small:
  br label %copy

large:
  br label %copy

copy:
  %chunk = phi i32 [ %remaining, %small ], [ %covered, %large ]
  %src = inttoptr i32 %dst to ptr
  %dstoff = add i32 %dst, %covered
  %dst2 = inttoptr i32 %dstoff to ptr
  call void @llvm.memcpy.p0.p0.i32(ptr %dst2, ptr %src, i32 %chunk, i1 false)
  br label %cont

cont:
  %chunk2 = phi i32 [ %chunk, %copy ]
  %covered.next = add i32 %covered, %chunk2
  %finished = icmp eq i32 %covered.next, %length
  br i1 %finished, label %done, label %grow

done:
  ret void
}

define i32 @__udon_strlen(ptr %s) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %next, %cont ]
  %p = getelementptr inbounds i8, ptr %s, i32 %i
  %c = load i8, ptr %p, align 1
  %iszero = icmp eq i8 %c, 0
  br i1 %iszero, label %exit, label %cont

cont:
  %next = add i32 %i, 1
  br label %loop

exit:
  ret i32 %i
}

define i32 @__udon_memcmp(ptr %a, ptr %b, i32 %n) {
entry:
  %empty = icmp eq i32 %n, 0
  br i1 %empty, label %equal, label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %next, %cont ]
  %pa = getelementptr inbounds i8, ptr %a, i32 %i
  %pb = getelementptr inbounds i8, ptr %b, i32 %i
  %ca = load i8, ptr %pa, align 1
  %cb = load i8, ptr %pb, align 1
  %diff = icmp ne i8 %ca, %cb
  br i1 %diff, label %differ, label %cont

cont:
  %next = add i32 %i, 1
  %done = icmp eq i32 %next, %n
  br i1 %done, label %equal, label %loop

differ:
  %wa = zext i8 %ca to i32
  %wb = zext i8 %cb to i32
  %r = sub i32 %wa, %wb
  ret i32 %r

equal:
  ret i32 0
}

declare void @llvm.memcpy.p0.p0.i32(ptr, ptr, i32, i1)
