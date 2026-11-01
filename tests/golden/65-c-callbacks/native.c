int cb_read(const void *p) { return *(const int *)p; }
int cb_apply(int (*f)(int), int x) { return f(x) + 1; }
void cb_each(void (*f)(int), int n) { for (int i = 1; i <= n; i++) f(i); }
