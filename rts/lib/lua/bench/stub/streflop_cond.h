// Stand-in for Recoil's streflop_cond.h, so the harness builds without the streflop submodule.
// Benchmark/equality harness only: both old and new VM builds use the same stub.
#ifndef STUB_STREFLOP_COND_H
#define STUB_STREFLOP_COND_H
#include <cmath>
namespace math {
  using std::floor; using std::ceil; using std::pow; using std::fabs; using std::fmod;
  using std::sin; using std::cos; using std::tan; using std::asin; using std::acos;
  using std::atan; using std::atan2; using std::sinh; using std::cosh; using std::tanh;
  using std::sqrt; using std::log; using std::log10; using std::exp; using std::frexp;
  using std::ldexp; using std::modf; using std::isfinite; using std::isnan;
  inline int isinf(float x) { return std::isinf(x) ? (x > 0 ? 1 : -1) : 0; }
  static constexpr float PI = 3.14159265358979323846f;
  static constexpr float TWOPI = 2.0f * PI;
  static constexpr float DEG_TO_RAD = PI / 180.0f;
}
#define STREFLOP_ENABLED 0
#endif
