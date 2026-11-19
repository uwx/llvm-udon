; Recovery: a poisoned shadow stack must not break the next event.
;
; A function's frame is the sum of its allocas, so the oversized frame lives in
; its own function -- putting it in `main` would make every invocation request
; it, and the first one would halt too.
;
; Each frame's address escapes through a volatile store, because the writer's
; pipeline runs SROA after inlining: without an escape, SROA promotes the
; array away and the frame stops existing, which is the opposite of what this
; fixture is testing.
;
; RUN: llvm-udon %s -o %t.uasm -udon-shadow-stack-bytes=16384
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --repeat 3 --entry _start | FileCheck --check-prefix=OUT %s
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry recover_nested | FileCheck --check-prefix=NESTED %s

@g_count = internal global i32 0, align 4
@escape = internal global ptr null, align 4

; 12 KiB: fits a fresh 16 KiB region, and leaves only 4 KiB.
define internal i32 @big_ok(i32 %seed) {
entry:
  %keep = alloca [12288 x i8], align 4
  %kp = getelementptr inbounds [12288 x i8], ptr %keep, i32 0, i32 0
  %b = trunc i32 %seed to i8
  store i8 %b, ptr %kp, align 1
  %kv = load i8, ptr %kp, align 1
  store volatile ptr %kp, ptr @escape, align 4
  %kvi = zext i8 %kv to i32
  ret i32 %kvi
}

; 20000 bytes: never fits a 16 KiB region.
define internal i32 @too_big() {
entry:
  %big = alloca [20000 x i8], align 4
  %bp = getelementptr inbounds [20000 x i8], ptr %big, i32 0, i32 0
  store i8 1, ptr %bp, align 1
  store volatile ptr %bp, ptr @escape, align 4
  ret i32 -1
}

define i32 @main() {
entry:
  %n = load i32, ptr @g_count, align 4
  %n1 = add i32 %n, 1
  store i32 %n1, ptr @g_count, align 4
  %is2 = icmp eq i32 %n1, 2
  br i1 %is2, label %overflow, label %normal

overflow:
  %r = call i32 @too_big()
  ret i32 %r

normal:
  %v = call i32 @big_ok(i32 %n1)
  ret i32 %v
}

define i32 @udon_event_recover_nested() {
entry:
  ; Re-enter this behaviour: the first event overflows, the second must still
  ; work. The names carry no leading underscore, because a custom event is
  ; exported verbatim.
  %v = call i32 @big_ok(i32 7)
  ret i32 %v
}

define i32 @udon_event_recover_overflow() {
entry:
  %r = call i32 @too_big()
  ret i32 %r
}

define i32 @udon_event_recover_probe() {
entry:
  %v = call i32 @big_ok(i32 9)
  ret i32 %v
}

; Sequential: invocation 1 returns 1, invocation 2 halts with reason 2,
; invocation 3 returns 3.
; OUT: ret _start = 1
; OUT: HALT _start 2
; OUT: ret _start = 3
;
; Nested: recover_nested returns 7 after re-entering through the custom events.
; NESTED: ret recover_nested = 7
