; overflow.ll - I32 add with overflow check, panic on overflow
@.msg = private unnamed_addr constant [17 x i8] c"integer overflow\00"
@.file = private unnamed_addr constant [8 x i8] c"ovf.zev\00"
declare void @zevic_panic(ptr, i64, ptr, i32) noreturn
declare { i32, i1 } @llvm.sadd.with.overflow.i32(i32, i32)

define i32 @add(i32 %a, i32 %b) {
entry:
  %r = call { i32, i1 } @llvm.sadd.with.overflow.i32(i32 %a, i32 %b)
  %v = extractvalue { i32, i1 } %r, 0
  %o = extractvalue { i32, i1 } %r, 1
  br i1 %o, label %ovf, label %ok
ovf:
  call void @zevic_panic(ptr @.msg, i64 16, ptr @.file, i32 1)
  unreachable
ok:
  ret i32 %v
}

define i32 @main() {
  %x = call i32 @add(i32 2147483647, i32 1)
  ret i32 0
}
