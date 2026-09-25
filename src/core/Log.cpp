#include "core/Log.h"

#include <Windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace tg {
namespace {

// 追加の出力先。UI が受け取って直近のメッセージを表示する。
LogSink g_sink;

const char* LevelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Info:  return "[info] ";
        case LogLevel::Warn:  return "[warn] ";
        case LogLevel::Error: return "[error] ";
    }
    return "[?] ";
}

// 起動からの経過秒。起動や読み込みのどこに時間がかかっているかを stderr /
// デバッガ出力から追えるようにする（UI へ渡す本文には付けない）。
double ElapsedSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void WriteLine(const char* tag, const char* body) {
    char line[2048];
    std::snprintf(line, sizeof(line), "[%8.3f] %s%s\n", ElapsedSeconds(), tag, body);
    ::OutputDebugStringA(line);
    std::fputs(line, stderr);
}

}  // namespace

void SetLogSink(LogSink sink) {
    g_sink = std::move(sink);
}

void LogMessage(LogLevel level, const char* fmt, ...) {
    char body[1920];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    WriteLine(LevelTag(level), body);
    if (g_sink) {
        g_sink(level, body);
    }
}

}  // namespace tg
