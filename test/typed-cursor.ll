; A typed float-array partition fixture: an exported float array, a pointer
; global provenanced to it, a struct that carries the pointer, and a callee
; that binds the struct's fields into more pointer globals. This is the
; libsm64 geometry shape in miniature, so a lowering bug shows up in a few
; hundred bytes of UASM instead of a 160 MB program.
;
; The assertion reads the array back through the exported `__bridge_out` slot:
; the typed partition puts the array in a `%SystemSingleArray`, so the fixture
; fails outright if the partition is disabled or mis-lowered.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start --dump-symbol __bridge_out=%dump | FileCheck --check-prefix=OUT %s

@udon_export_out = global [8 x float] zeroinitializer
@cursor = global ptr null
@tri = global ptr null
@norm = global ptr null

%bufs = type { ptr, ptr }

; The writer must prove @cursor/@tri/@norm all denote @udon_export_out.
define internal void @bind(ptr %b) {
  %p = load ptr, ptr %b
  store ptr %p, ptr @tri
  %g = getelementptr %bufs, ptr %b, i32 0, i32 1
  %n = load ptr, ptr %g
  store ptr %n, ptr @norm
  ret void
}

define internal void @use(ptr %b) {
  call void @bind(ptr %b)
  ; walk the triangle cursor: write 3 floats, advance by 3
  %t = load ptr, ptr @tri
  store float 1.0, ptr %t
  %t1 = getelementptr float, ptr %t, i32 1
  store float 2.0, ptr %t1
  %t2 = getelementptr float, ptr %t, i32 2
  store float 3.0, ptr %t2
  %tn = getelementptr float, ptr %t, i32 3
  store ptr %tn, ptr @tri
  ; and the normal cursor
  %n = load ptr, ptr @norm
  store float 4.0, ptr %n
  ret void
}

define i32 @udon_event_start() {
  %b = alloca %bufs, align 4
  store ptr @udon_export_out, ptr %b
  %q = getelementptr [8 x float], ptr @udon_export_out, i32 0, i32 3
  %g = getelementptr %bufs, ptr %b, i32 0, i32 1
  store ptr %q, ptr %g
  call void @use(ptr %b)
  ; report: out[0]+out[1]+out[2] + out[3]*10 = 1+2+3+40 = 46
  %a0 = load float, ptr @udon_export_out
  %p1 = getelementptr [8 x float], ptr @udon_export_out, i32 0, i32 1
  %a1 = load float, ptr %p1
  %p2 = getelementptr [8 x float], ptr @udon_export_out, i32 0, i32 2
  %a2 = load float, ptr %p2
  %p3 = getelementptr [8 x float], ptr @udon_export_out, i32 0, i32 3
  %a3 = load float, ptr %p3
  %s0 = fadd float %a0, %a1
  %s1 = fadd float %s0, %a2
  %m = fmul float %a3, 1.0e1
  %s2 = fadd float %s1, %m
  %r = fptosi float %s2 to i32
  ret i32 %r
}

; 1 + 2 + 3 + 4*10 = 46 -- and only a correctly typed partition puts the
; normal cursor's 4.0 into out[3].
; OUT: ret _start = 46

; Nested-GEP shape: `gSineTable[i + 1024]` after inlining is
; `getelementptr float, ptr (getelementptr [N x float], ptr @g, 0, 1024), i32 %i`.
; The inner GEP must fold to element 1024, not become a byte address.
@udon_export_tbl = global [16 x float] zeroinitializer

define i32 @udon_event_nested() {
  ; write 7.0 at element 11 (the inner GEP is 10, the outer adds 1)
  %base = getelementptr [16 x float], ptr @udon_export_tbl, i32 0, i32 10
  %p = getelementptr float, ptr %base, i32 1
  store float 7.0, ptr %p
  ; read it back through the same shape
  %q = getelementptr [16 x float], ptr @udon_export_tbl, i32 0, i32 10
  %r = getelementptr float, ptr %q, i32 1
  %v = load float, ptr %r
  %ok = fcmp oeq float %v, 7.0
  %z = zext i1 %ok to i32
  ret i32 %z
}
