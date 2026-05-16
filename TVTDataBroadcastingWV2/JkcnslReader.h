#pragma once
#include "pch.h"
#include "CommentFetcher.h"
#include <atomic>
#include <functional>
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
    const std::string& GetChannel() const { return m_channel; }
    void SetCallback(Callback cb) { m_callback = std::move(cb); }

    static bool ParseChatLine(const std::string& line, Comment& out);

private:
    Callback m_callback;
    HANDLE m_hProcess    = nullptr;
    HANDLE m_hStdinWrite = nullptr;
    HANDLE m_hStdoutRead = nullptr; // overlapped read end
    HANDLE m_hStopEvent  = nullptr; // manual-reset, signals ReadLoop to exit
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::string m_channel;

    void ReadLoop();
    void ProcessBuffer(const char* buf, DWORD size, std::string& lineBuf);
    static std::string GetXmlAttr(const std::string& xml, const std::string& attr);
};
