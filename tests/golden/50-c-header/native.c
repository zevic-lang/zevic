#include "x.h"

int use_header(void) {
    struct Point p = {1, 2};
    zv_reset();
    return (int)zv_len(&p, "four", 1.0);
}
