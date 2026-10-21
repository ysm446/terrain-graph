#include "app/RemoteServer.h"

#include <algorithm>
#include <utility>

namespace tg {
namespace {

constexpr DWORD kPipeBufferBytes = 64 * 1024;
// 1 行の上限。これを超える行が来たら接続を切る（壊れた相手でメモリを食い尽くさないため）。
constexpr size_t kMaxLineBytes = 256u * 1024u * 1024u;

}  // namespace

bool RemoteServer::Start(const std::wstring& name, HWND wakeWindow) {
    Stop();
    const std::wstring path = L"\\\\.\\pipe\\" + name;
    // 最初のインスタンスとして作る。同じ名前を別のアプリ（もう 1 つの terrain-graph など）が
    // 開いていれば失敗する（後から来た側が横取りしない）。
    m_pipe = ::CreateNamedPipeW(path.c_str(),
                                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                1, kPipeBufferBytes, kPipeBufferBytes, 0, nullptr);
    if (m_pipe == INVALID_HANDLE_VALUE) {
        return false;
    }
    m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_ioEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_stopEvent == nullptr || m_ioEvent == nullptr) {
        Stop();
        return false;
    }
    m_wakeWindow = wakeWindow;
    {
        std::lock_guard lock(m_mutex);
        m_stopping = false;
        m_requests.clear();
        m_replies.clear();
    }
    m_thread = std::thread([this] { ThreadMain(); });
    return true;
}

void RemoteServer::Stop() {
    if (m_thread.joinable()) {
        ::SetEvent(m_stopEvent);
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_replied.notify_all();
        m_thread.join();
    }
    if (m_pipe != INVALID_HANDLE_VALUE) {
        ::CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
    for (HANDLE* event : {&m_stopEvent, &m_ioEvent}) {
        if (*event != nullptr) {
            ::CloseHandle(*event);
            *event = nullptr;
        }
    }
    m_connected = false;
}

std::vector<RemoteServer::Request> RemoteServer::TakeRequests() {
    std::lock_guard lock(m_mutex);
    std::vector<Request> requests(std::make_move_iterator(m_requests.begin()),
                                  std::make_move_iterator(m_requests.end()));
    m_requests.clear();
    return requests;
}

void RemoteServer::Reply(uint64_t ticket, std::string line) {
    {
        std::lock_guard lock(m_mutex);
        m_replies[ticket] = std::move(line);
    }
    m_replied.notify_all();
}

bool RemoteServer::WaitIo(OVERLAPPED& overlapped, DWORD& bytes) {
    const HANDLE events[] = {overlapped.hEvent, m_stopEvent};
    const DWORD waited = ::WaitForMultipleObjects(2, events, FALSE, INFINITE);
    if (waited != WAIT_OBJECT_0) {
        // 止める合図。取り消した I/O が終わるのを待ってから戻る（overlapped はスタックにある）。
        ::CancelIoEx(m_pipe, &overlapped);
        ::GetOverlappedResult(m_pipe, &overlapped, &bytes, TRUE);
        return false;
    }
    return ::GetOverlappedResult(m_pipe, &overlapped, &bytes, FALSE) != FALSE;
}

bool RemoteServer::WriteAll(const std::string& data) {
    size_t written = 0;
    while (written < data.size()) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = m_ioEvent;
        ::ResetEvent(m_ioEvent);
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - written, kPipeBufferBytes));
        DWORD bytes = 0;
        if (!::WriteFile(m_pipe, data.data() + written, chunk, nullptr, &overlapped) &&
            ::GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        if (!WaitIo(overlapped, bytes) || bytes == 0) {
            return false;
        }
        written += bytes;
    }
    return true;
}

void RemoteServer::ThreadMain() {
    std::vector<char> buffer(kPipeBufferBytes);
    while (::WaitForSingleObject(m_stopEvent, 0) != WAIT_OBJECT_0) {
        // --- 接続を待つ -------------------------------------------------------
        OVERLAPPED overlapped{};
        overlapped.hEvent = m_ioEvent;
        ::ResetEvent(m_ioEvent);
        DWORD bytes = 0;
        bool connected = ::ConnectNamedPipe(m_pipe, &overlapped) != FALSE;
        if (!connected) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (error == ERROR_IO_PENDING) {
                connected = WaitIo(overlapped, bytes);
            }
        }
        if (!connected) {
            // 止める合図か、相手がすぐ切った。少し待ってから受け直す。
            if (::WaitForSingleObject(m_stopEvent, 50) == WAIT_OBJECT_0) break;
            ::DisconnectNamedPipe(m_pipe);
            continue;
        }
        m_connected = true;

        // --- 1 行ずつ読み、返事を書く ----------------------------------------
        std::string pending;
        bool alive = true;
        while (alive) {
            OVERLAPPED read{};
            read.hEvent = m_ioEvent;
            ::ResetEvent(m_ioEvent);
            DWORD got = 0;
            if (!::ReadFile(m_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &read) &&
                ::GetLastError() != ERROR_IO_PENDING && ::GetLastError() != ERROR_MORE_DATA) {
                break;
            }
            if (!WaitIo(read, got) && ::GetLastError() != ERROR_MORE_DATA) {
                break;
            }
            if (got == 0) {
                continue;
            }
            pending.append(buffer.data(), got);
            if (pending.size() > kMaxLineBytes) {
                break;
            }
            size_t newline = 0;
            while (alive && (newline = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                uint64_t ticket = 0;
                {
                    std::lock_guard lock(m_mutex);
                    ticket = ++m_nextTicket;
                    m_requests.push_back({ticket, std::move(line)});
                }
                // 背面や待機中のフレームループを起こす。
                if (m_wakeWindow != nullptr) ::PostMessageW(m_wakeWindow, WM_NULL, 0, 0);
                std::string reply;
                {
                    std::unique_lock lock(m_mutex);
                    m_replied.wait(lock, [&] { return m_stopping || m_replies.contains(ticket); });
                    if (m_stopping) {
                        alive = false;
                        break;
                    }
                    reply = std::move(m_replies[ticket]);
                    m_replies.erase(ticket);
                }
                reply += '\n';
                if (!WriteAll(reply)) alive = false;
            }
        }
        m_connected = false;
        ::DisconnectNamedPipe(m_pipe);
    }
    m_connected = false;
}

}  // namespace tg
