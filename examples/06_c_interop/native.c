extern int zv_double(int);
extern void zv_hello(const char *);
int call_twice(int x) { return zv_double(x); }
void greet_from_c(void) { zv_hello("from C"); }
