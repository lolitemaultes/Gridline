// Gridline — minimal test harness: TAP-style lines, non-zero exit on failure.
#pragma once
#include <cstdio>

namespace {
int failures = 0;
int checks = 0;
// Unbuffered: a test that crashes still shows every check it passed.
[[maybe_unused]] const bool kUnbufferedStdout = std::setvbuf(stdout, nullptr, _IONBF, 0) == 0;
void expect(bool ok, const char* name) {
  ++checks;
  std::printf("%s %d - %s\n", ok ? "ok" : "not ok", checks, name);
  if (!ok) ++failures;
}
int finish() {
  std::printf("1..%d\n", checks);
  if (failures) std::printf("FAILED: %d/%d\n", failures, checks);
  return failures ? 1 : 0;
}
}  // namespace
