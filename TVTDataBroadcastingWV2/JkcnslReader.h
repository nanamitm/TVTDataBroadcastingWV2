#pragma once
#include "pch.h"
#include "CommentFetcher.h"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

// Reads real-time jikkyo comments by launching jkcnsl.exe and connecting to
// NX-Jikkyo (https://nx-jikkyo.tsukumijima.net) via the refuge stream protocol.
// No NicoNico authentication is required.
class JkcnslReader
{
public:
    using Callback = std::function<void(std::vector<Comment>)>;

    JkcnslReader() = default;
    ~JkcnslReader();
    JkcnslReader(const JkcnslReader&) = delete;
    JkcnslReader& operator=(const JkcnslReader&) = delete;

    // jkcnsl.exe path and jikkyo channel ("jk1", "jk4", ...)
    bool Start(const std::wstring& jkcnslPath, const std::string& jkChannel);
    void Stop();
    bool IsRunning() const { return m_running; }

    void SetCallback(Callback cb) { m_callback = std::move(cb); }

    // Parse a single XML chat line from jkcnsl stdout.
    // Returns true and fills out if the line is a valid <chat> element.
    static bool ParseChatLine(const std::string& line, Comment& out);

private:
    Callback m_callback;
    HANDLE m_hProcess   = nullptr;
    HANDLE m_hStdinWrite  = nullptr;
    HANDLE m_hStdoutRead  = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };

    void ReadLoop();
    static std::string GetXmlAttr(const std::string& xml, const std::string& attr);
};
