extern void zv_on_event(unsigned long);
static unsigned long stored;
void hd_register(unsigned long id) { stored = id; }
unsigned long hd_stored(void) { return stored; }
void hd_fire(int times) { for (int i = 0; i < times; i++) zv_on_event(stored); }
