#include "core/Log.h"

#include <Windows.h>

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

void WriteLine(const char* tag, const char* body) {
    char line[2048];
    std::snprintf(line, sizeof(line), "%s%s\n", tag, body);
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
