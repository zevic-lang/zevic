#include <vector>
extern "C" int cpp_sum(int n) { std::vector<int> v; for (int i = 1; i <= n; i++) v.push_back(i); int s = 0; for (int x : v) s += x; return s; }
