#pragma once

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tg {

// 起動中のアプリを外（MCP サーバなど）から操作する口。名前付きパイプで 1 行 1 つの JSON を受け、
// 1 行の JSON を返す。中身は解釈しない（解釈と実行はメインスレッドの Application が行う）。
//
// - 受け付けはパイプのスレッドが行い、要求をキューへ積んでウィンドウを起こす（WM_NULL）。
// - 1 つの接続の中では要求を 1 つずつ順に処理する（返事が来るまで次の行を読まない）。
// - ローカルからだけ受ける（PIPE_REJECT_REMOTE_CLIENTS）。同時に繋げるのは 1 本。
// 仕様は docs/reference/remote-control.md。
class RemoteServer {
public:
    struct Request {
        uint64_t ticket = 0;
        std::string line;  // 受けた 1 行（UTF-8、改行は含まない）
    };

    RemoteServer() = default;
    RemoteServer(const RemoteServer&) = delete;
    RemoteServer& operator=(const RemoteServer&) = delete;
    ~RemoteServer() { Stop(); }

    // パイプ（\\.\pipe\<name>）を開いて待ち受けを始める。同じ名前を別のアプリが開いていれば偽。
    bool Start(const std::wstring& name, HWND wakeWindow);
    void Stop();
    bool Running() const { return m_thread.joinable(); }
    bool Connected() const { return m_connected.load(); }

    // メインスレッドから呼ぶ。届いた要求を取り出す。
    std::vector<Request> TakeRequests();
    // メインスレッドから呼ぶ。ticket の要求への返事（1 行の JSON、改行は付けない）。
    void Reply(uint64_t ticket, std::string line);

private:
    void ThreadMain();
    // 重ねた I/O の終わりを待つ。止める合図が来たら取り消して偽を返す。
    bool WaitIo(OVERLAPPED& overlapped, DWORD& bytes);
    bool WriteAll(const std::string& data);

    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_ioEvent = nullptr;
    HWND m_wakeWindow = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_connected{false};

    std::mutex m_mutex;
    std::condition_variable m_replied;
    bool m_stopping = false;
    uint64_t m_nextTicket = 0;
    std::deque<Request> m_requests;
    std::unordered_map<uint64_t, std::string> m_replies;
};

}  // namespace tg
