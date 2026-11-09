package main

/*
#cgo LDFLAGS: -L${SRCDIR}/.. -lwordlib -lpthread -lm
#include <stdlib.h>
#include "../wordlib.h"
*/
import "C"
import ("fmt"; "unsafe")

func main() {
	C.zevic_init(0, nil)
	s := C.CString("the quick brown fox")
	defer C.free(unsafe.Pointer(s))
	fmt.Printf("words=%d clamp=%d\n", C.zv_word_count(s), C.zv_clamp(50, 0, 10))
}
