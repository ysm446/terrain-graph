#pragma once

#include <cstdio>

// テストの共通部品。フレームワークは入れない。
// 数が増えて手に負えなくなったら、そのとき導入を考える。
namespace tg::tests {

inline int g_failures = 0;

inline void Check(bool condition, const char* name) {
    std::printf("  %-58s %s\n", name, condition ? "OK" : "*** NG ***");
    if (!condition) {
        ++g_failures;
    }
}

inline void Section(const char* name) {
    std::printf("\n%s\n", name);
}

}  // namespace tg::tests
