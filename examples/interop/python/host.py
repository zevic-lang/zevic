import ctypes
import sys

lib = ctypes.CDLL(sys.argv[1])
lib.zevic_init(0, None)
lib.zv_word_count.argtypes = [ctypes.c_char_p]
print("words=%d clamp=%d" % (lib.zv_word_count(b"the quick brown fox"), lib.zv_clamp(50, 0, 10)))
