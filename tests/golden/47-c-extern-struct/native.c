typedef struct { int X, Y; } Point;
void Move(Point *p, int dx) { p->X += dx; }
int Sum(const Point *p) { return p->X + p->Y; }
