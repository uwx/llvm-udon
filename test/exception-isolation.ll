; Exception isolation: a throwing event must not poison the next one.
;
; `throws` abandons a large frame and then writes past the end of `__mem`, so
; Interpret() raises and `__depth` stays incremented with `__sp` left low. Its
; export name is `throws`, not `_throws`: a custom event carries no underscore.
; `_start` then needs more bytes than are left, so it succeeds only if the
; harness discarded the whole IUdonProgram (and therefore the heap) rather than
; just the UdonVM -- a fresh VM over the same program shares the heap.
;
; RUN: llvm-udon %s -o %t.uasm -udon-shadow-stack-bytes=16384
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry throws --entry _start | FileCheck --check-prefix=OUT %s

@g_scratch = internal global [8 x i8] zeroinitializer, align 4

; Abandon 12288 of the 16 KiB region, then write far past the end of __mem.
define i32 @udon_event_throws() {
entry:
  %keep = alloca [12288 x i8], align 4
  %kp = getelementptr inbounds [12288 x i8], ptr %keep, i32 0, i32 0
  store i8 1, ptr %kp, align 1
  ; __mem is -udon-memory-bytes + -udon-shadow-stack-bytes + 8 bytes; offset
  ; 2000000 is far past it, so the array write throws.
  %far = inttoptr i32 2000000 to ptr
  store i32 7, ptr %far, align 4
  ret i32 0
}

; Needs 8 KiB, which a poisoned stack (only 4 KiB left) cannot give.
define internal i32 @need_space(i32 %seed) {
entry:
  %keep = alloca [8192 x i8], align 4
  %kp = getelementptr inbounds [8192 x i8], ptr %keep, i32 0, i32 0
  %b = trunc i32 %seed to i8
  store i8 %b, ptr %kp, align 1
  %v = load i8, ptr %kp, align 1
  %vi = zext i8 %v to i32
  ret i32 %vi
}

define i32 @main() {
entry:
  %v = call i32 @need_space(i32 42)
  ret i32 %v
}

; The exception is data: an EXCEPTION line, then _start's result. Exit 0.
; OUT: EXCEPTION throws
; OUT: ret _start = 42
