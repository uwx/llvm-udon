; Floating point: arithmetic, every fcmp predicate with a NaN operand, and the
; saturating float-to-integer conversions.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

@g_acc = internal global i32 0, align 4

define internal void @bump(i1 %cond, i32 %amount) {
entry:
  %old = load i32, ptr @g_acc, align 4
  %inc = add i32 %old, %amount
  %new = select i1 %cond, i32 %inc, i32 %old
  store i32 %new, ptr @g_acc, align 4
  ret void
}

define i32 @udon_event_start() {
entry:
  ; Arithmetic: (1.5 + 2.5) * 2.0 / 4.0 = 2.0
  %sum = fadd double 1.5, 2.5
  %prod = fmul double %sum, 2.0
  %quot = fdiv double %prod, 4.0
  %trunc = fptosi double %quot to i32

  ; fcmp predicates, with one NaN operand: the ordered ones are false, the
  ; unordered ones true, and `one` differs from `une`.
  %nan = fdiv double 0.0, 0.0
  %oeq = fcmp oeq double %nan, 1.0
  %une = fcmp une double %nan, 1.0
  %one = fcmp one double %nan, 1.0
  %ueq = fcmp ueq double %nan, 1.0
  %ord = fcmp ord double %nan, 1.0
  %uno = fcmp uno double %nan, 1.0

  ; A non-NaN pair: olt true, uge true.
  %olt = fcmp olt double 1.0, 2.0
  %uge = fcmp uge double 2.0, 1.0

  ; Saturating conversions.
  ; -2.7 as i32 = -2
  %n27 = fptosi double -2.7 to i32
  ; NaN as i32 = 0
  %nnan = fptosi double %nan to i32
  ; 1e30 as i32 = Int32.MaxValue
  %big = fptosi double 1.0e30 to i32
  ; f64::MAX as u64 = u64 all ones
  %umax = fptoui double 1.7976931348623157e308 to i64
  %umaxw = trunc i64 %umax to i32

  ; Fold the booleans in as 1/0.
  %b_une = zext i1 %une to i32
  %b_one = zext i1 %one to i32
  %b_ueq = zext i1 %ueq to i32
  %b_ord = zext i1 %ord to i32
  %b_uno = zext i1 %uno to i32
  %b_olt = zext i1 %olt to i32
  %b_uge = zext i1 %uge to i32

  %t0 = add i32 %trunc, %b_une
  %t1 = add i32 %t0, %b_one
  %t2 = add i32 %t1, %b_ueq
  %t3 = add i32 %t2, %b_ord
  %t4 = add i32 %t3, %b_uno
  %t5 = add i32 %t4, %b_olt
  %t6 = add i32 %t5, %b_uge
  %t7 = add i32 %t6, %n27
  %t8 = add i32 %t7, %nnan
  %t9 = add i32 %t8, %big
  %t10 = add i32 %t9, %umaxw
  ret i32 %t10
}

; trunc of 2.0 = 2
; une=1, one=0, ueq=1, ord=0, uno=1, olt=1, uge=1  -> 5
; -2.7 -> -2, NaN -> 0, 1e30 -> 2147483647, f64::MAX as u64 -> -1
; 2 + 5 - 2 + 0 + 2147483647 - 1 = 2147483651 -> wraps to -2147483645
; OUT: ret _start = -2147483645
