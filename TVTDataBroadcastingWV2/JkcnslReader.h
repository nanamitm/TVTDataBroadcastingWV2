#pragma once
#include "pch.h"
#include "CommentFetcher.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class JkcnslReader
{
public:
    using Callback = std::function<void(std::vector<Comment>)>;

    JkcnslReader() = default;
    ~JkcnslReader();
    JkcnslReader(const JkcnslReader&) = delete;
    JkcnslReader& operator=(const JkcnslReader&) = delete;

    bool Start(const std::wstring& jkcnslPath, const std::string& jkChannel);
    void Stop();
    bool IsRunning() const { return m_running; }

    // Post a comment on the open stream. payload is the UTF-8 "[mail]comment"
    // body; this prepends '+' and appends CRLF. Returns false if not connected
    // or the write fails.
    bool Post(const std::string& payload);
    bool IsConnected() const { return m_connected; }
    const std::string& GetChannel() const { return m_channel; }
    void SetCallback(Callback cb) { m_callback = std::move(cb); }

    // Notified (with the new state) whenever the stream connection changes:
    // connected once jkcnsl opens the stream ('*'), disconnected on close/stop.
    using ConnectionCallback = std::function<void(bool connected)>;
    void SetConnectionCallback(ConnectionCallback cb) { m_connCallback = std::move(cb); }

    static bool ParseChatLine(const std::string& line, Comment& out);

private:
    Callback m_callback;
    ConnectionCallback m_connCallback;
    HANDLE m_hProcess    = nullptr;
    HANDLE m_hStdinWrite = nullptr;
    HANDLE m_hStdoutRead = nullptr; // overlapped read end
    HANDLE m_hStopEvent  = nullptr; // manual-reset, signals ReadLoop to exit
    std::mutex m_stdinMutex;        // guards writes to / closing of m_hStdinWrite
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_connected{ false };
    std::string m_channel;

    void ReadLoop();
    void ProcessBuffer(const char* buf, DWORD size, std::string& lineBuf);
    void SetConnected(bool connected);
    static std::string GetXmlAttr(const std::string& xml, const std::string& attr);
};
