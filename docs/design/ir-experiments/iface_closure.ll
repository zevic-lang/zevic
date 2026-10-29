; iface_closure.ll - interface value = { obj, vtable }, closure = { fn, env }
%Iface = type { ptr, ptr }
%Closure = type { ptr, ptr }
%Counter = type { i32 }

define i32 @Counter.Value(ptr %this) {
  %p = getelementptr inbounds %Counter, ptr %this, i32 0, i32 0
  %v = load i32, ptr %p
  ret i32 %v
}
@vt.Counter.Valued = private constant [1 x ptr] [ptr @Counter.Value]

define i32 @call_iface(%Iface %i) {
  %obj = extractvalue %Iface %i, 0
  %vt = extractvalue %Iface %i, 1
  %fp = load ptr, ptr %vt
  %r = call i32 %fp(ptr %obj)
  ret i32 %r
}

; closure capturing an i32: env = { i32 }
define i32 @lam(ptr %env, i32 %x) {
  %p = getelementptr inbounds { i32 }, ptr %env, i32 0, i32 0
  %c = load i32, ptr %p
  %r = add i32 %x, %c
  ret i32 %r
}

define i32 @main() {
  %c = alloca %Counter
  store i32 40, ptr %c
  %i0 = insertvalue %Iface undef, ptr %c, 0
  %i1 = insertvalue %Iface %i0, ptr @vt.Counter.Valued, 1
  %v = call i32 @call_iface(%Iface %i1)
  %env = alloca { i32 }
  store i32 2, ptr %env
  %f0 = insertvalue %Closure undef, ptr @lam, 0
  %f1 = insertvalue %Closure %f0, ptr %env, 1
  %fp = extractvalue %Closure %f1, 0
  %ep = extractvalue %Closure %f1, 1
  %r = call i32 %fp(ptr %ep, i32 %v)
  ret i32 %r       ; 42
}
