; string_enum.ll - String = { ptr, i64 }, enum = { tag, payload }
%String = type { ptr, i64 }
; enum Shape { Circle(F64), Rect(F64, F64), Empty }  -> tag + 2 x i64 payload
%Shape = type { i32, [2 x i64] }
%Shape.Rect = type { double, double }

@.s = private unnamed_addr constant [6 x i8] c"hello\00"
declare i32 @puts(ptr)

define double @area(ptr %p) {
entry:
  %tagp = getelementptr inbounds %Shape, ptr %p, i32 0, i32 0
  %tag = load i32, ptr %tagp
  %pay = getelementptr inbounds %Shape, ptr %p, i32 0, i32 1
  switch i32 %tag, label %empty [ i32 0, label %circle
                                  i32 1, label %rect ]
circle:
  %r = load double, ptr %pay
  %r2 = fmul double %r, %r
  %a = fmul double %r2, 3.14
  ret double %a
rect:
  %rc = load %Shape.Rect, ptr %pay
  %w = extractvalue %Shape.Rect %rc, 0
  %h = extractvalue %Shape.Rect %rc, 1
  %ra = fmul double %w, %h
  ret double %ra
empty:
  ret double 0.0
}

define i32 @main() {
  %s = insertvalue %String { ptr @.s, i64 5 }, i64 5, 1
  %sp = extractvalue %String %s, 0
  call i32 @puts(ptr %sp)
  %sh = alloca %Shape
  store i32 1, ptr %sh
  %pay = getelementptr inbounds %Shape, ptr %sh, i32 0, i32 1
  store %Shape.Rect { double 3.0, double 4.0 }, ptr %pay
  %a = call double @area(ptr %sh)
  %i = fptosi double %a to i32
  ret i32 %i      ; exit code 12
}
