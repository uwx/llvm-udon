; Integer <-> floating point conversions, in both directions and at all widths,
; plus `fneg`.
;
; The extern a conversion names is not the same string as the slot type it
; writes into: the extern is `SystemConvert.__ToSingle__SystemInt64__SystemSingle`
; while the slot is `%SystemSingle`. Building the name from the slot type
; produced `__ToSystemSingle__`, which the SDK does not define.
;
; `fneg` is `0 - x`: the float types expose `UnaryMinus`, not `UnaryNegation`
; (that name belongs to Boolean and the Unity vectors), and libsm64's audio
; code was the first to trip over the wrong spelling.
;
; A narrow `sitofp`/`uitofp` source lives in an Int32 slot, but the conversion
; externs exist only from Int32 and Int64. libsm64 converts `i16` values
; (`get_vibrato_freq_scale`) and the writer had to sign-/zero-extend them to
; the slot width first -- otherwise the operand slot type and the extern's
; declared parameter disagreed and the VM threw a heap type mismatch.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

define i32 @udon_event_start() {
entry:
  ; 7 as f32 then back: 7.
  %f1 = sitofp i64 7 to float
  %b1 = fptosi float %f1 to i32

  ; -7 as f32 then back: -7.
  %f2 = sitofp i64 -7 to float
  %b2 = fptosi float %f2 to i32

  ; 0xFFFFFFFF as unsigned is 4294967295.0, not -1.0.
  %d3 = uitofp i32 -1 to double
  %cmp3 = fcmp ogt double %d3, 0.0

  ; 2^40 as unsigned i64 -> single: positive, and the extern must resolve.
  %f4 = uitofp i64 1099511627776 to float
  %cmp4 = fcmp ogt float %f4, 0.0

  ; i16 sources: -100 sign-extends to -100.0; 200 zero-extends to 200.0.
  %f5 = sitofp i16 -100 to float
  %b5 = fptosi float %f5 to i32
  %f6 = uitofp i16 200 to float
  %b6 = fptosi float %f6 to i32
  %f7 = sitofp i8 -5 to float
  %b7 = fptosi float %f7 to i32
  %f8 = uitofp i8 250 to float
  %b8 = fptosi float %f8 to i32

  ; fneg: -1.5 negated is 1.5.
  %n = fneg float 1.5
  %bn = fptosi float %n to i32

  %z1 = zext i1 %cmp3 to i32
  %z2 = zext i1 %cmp4 to i32
  %s1 = add i32 %b1, %b2
  %s2 = add i32 %s1, %z1
  %s3 = add i32 %s2, %z2
  %s4 = add i32 %s3, %b5   ; -100
  %s5 = add i32 %s4, %b6   ; +200
  %s6 = add i32 %s5, %b7   ; -5
  %s7 = add i32 %s6, %b8   ; +250
  %s8 = add i32 %s7, %bn   ; fptosi(-1.5) = -1

  ; 7 + -7 + 1 + 1 - 100 + 200 - 5 + 250 - 1 = 346
  ret i32 %s8
}

; OUT: ret _start = 346