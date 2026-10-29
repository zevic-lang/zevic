; hello.ll - smallest possible program
@.str = private constant [14 x i8] c"Hello, World!\00"
declare i32 @puts(ptr)
define i32 @main() {
  %r = call i32 @puts(ptr @.str)
  ret i32 0
}
