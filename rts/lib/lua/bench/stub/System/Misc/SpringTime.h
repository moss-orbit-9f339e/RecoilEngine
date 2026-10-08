#pragma once
#include <chrono>
struct spring_time_stub { double s; float toSecsf() const { return (float)s; } };
namespace spring_time { inline spring_time_stub getelapsedtime() { using C = std::chrono::steady_clock; static const C::time_point t0 = C::now(); return { std::chrono::duration<double>(C::now() - t0).count() }; } }
