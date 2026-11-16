; Vectors: a <3 x float> add through UnityEngineVector3, and a component-wise
; multiply that has to scalarize because Vector3 has no such operator.
;
; RUN: llvm-udon %s -o %t.uasm
; RUN: udon-vm run %t.uasm --heap-json %t.udonheap.json --entry _start | FileCheck --check-prefix=OUT %s

define i32 @udon_event_start() {
entry:
  %a = insertelement <3 x float> poison, float 1.0, i32 0
  %a1 = insertelement <3 x float> %a, float 2.0, i32 1
  %a2 = insertelement <3 x float> %a1, float 3.0, i32 2
  %b = insertelement <3 x float> poison, float 10.0, i32 0
  %b1 = insertelement <3 x float> %b, float 20.0, i32 1
  %b2 = insertelement <3 x float> %b1, float 30.0, i32 2
  %sum = fadd <3 x float> %a2, %b2
  %x = extractelement <3 x float> %sum, i32 0
  %y = extractelement <3 x float> %sum, i32 1
  %z = extractelement <3 x float> %sum, i32 2
  %xi = fptosi float %x to i32
  %yi = fptosi float %y to i32
  %zi = fptosi float %z to i32
  %t0 = add i32 %xi, %yi
  %t1 = add i32 %t0, %zi
  ret i32 %t1
}

; (1+10) + (2+20) + (3+30) = 11 + 22 + 33 = 66
; OUT: ret _start = 66
