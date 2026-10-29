; rt_call.ll - call the C runtime (panic) from IR
@.msg = private unnamed_addr constant [9 x i8] c"it works\00"
@.file = private unnamed_addr constant [8 x i8] c"rt.zev\00\00"
declare void @zevic_panic(ptr, i64, ptr, i32) noreturn
define i32 @main() {
  call void @zevic_panic(ptr @.msg, i64 8, ptr @.file, i32 3)
  unreachable
}
