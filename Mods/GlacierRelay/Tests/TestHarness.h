#pragma once

#include <cstdio>

// Minimal checks, no framework: each test file defines Run<Name>Tests() and main sums failures.
inline int g_Failures = 0;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            ++g_Failures; \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        } \
    } while (false)
