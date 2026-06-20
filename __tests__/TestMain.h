// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <cstdio>

// Minimal zero-dependency test harness. CHECK records a pass/fail; main() returns the failure count
// so CTest (and CI) fail loudly. Intentionally tiny - this is a tripwire, not a test framework.
inline int g_checks = 0;
inline int g_failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            ++g_failures;                                                            \
            std::printf("FAIL  %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond);      \
        }                                                                            \
    } while (0)
