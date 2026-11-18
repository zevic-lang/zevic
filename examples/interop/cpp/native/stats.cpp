#include <algorithm>
#include <numeric>
#include <vector>

// C++ code (templates, the STL) behind an extern "C" door.
extern "C" double cpp_median(const double *xs, size_t n) {
    std::vector<double> v(xs, xs + n);
    std::sort(v.begin(), v.end());
    if (v.empty()) return 0.0;
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}
