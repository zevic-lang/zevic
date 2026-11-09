#include <cstdio>
#include "wordlib.h"
int main(int argc, char **argv) {
    zevic_init(argc, argv);
    std::printf("words=%d clamp=%d\n", zv_word_count("the quick brown fox"), zv_clamp(50, 0, 10));
}
