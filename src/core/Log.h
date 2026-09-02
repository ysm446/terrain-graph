#pragma once

#include <functional>

namespace tg {

enum class LogLevel {
    Info,
    Warn,
    Error,
};

// デバッガ出力とコンソールの両方へ書き出す。書式は printf 互換。
void LogMessage(LogLevel level, const char* fmt, ...);

// ログの追加の出力先。UI のステータスバーへ直近のメッセージを出すために使う。
// 設定できるのは 1 つだけで、空の関数を渡すと解除できる。
//
// **持ち主が壊れる前に必ず解除すること。** 解除しないと、破棄したオブジェクトを
// 指したままのシンクが後続のログで呼ばれる。
using LogSink = std::function<void(LogLevel, const char*)>;
void SetLogSink(LogSink sink);

}  // namespace tg

#define TG_LOG_INFO(...)  ::tg::LogMessage(::tg::LogLevel::Info, __VA_ARGS__)
#define TG_LOG_WARN(...)  ::tg::LogMessage(::tg::LogLevel::Warn, __VA_ARGS__)
#define TG_LOG_ERROR(...) ::tg::LogMessage(::tg::LogLevel::Error, __VA_ARGS__)
