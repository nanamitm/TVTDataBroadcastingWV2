#pragma once
#include "pch.h"
#include <atomic>
#include <ctime>
#include <functional>
#include <string>
#include <thread>
#include <vector>

struct Comment {
    std::string text;
    std::string color;    // CSS color name
    std::string position; // "naka" / "ue" / "shita"
    std::string size;     // "small" / "medium" / "big"
    time_t date = 0;      // Unix timestamp of the comment
};

class CommentFetcher
{
public:
    using Callback = std::function<void(std::vector<Comment>)>;

    CommentFetcher() = default;
    ~CommentFetcher();
    CommentFetcher(const CommentFetcher&) = delete;
    CommentFetcher& operator=(const CommentFetcher&) = delete;

    void SetChannel(const std::string& jikkyoChannel);
    void SetCallback(Callback cb) { m_callback = std::move(cb); }
    void Start();
    void Stop();

    static std::string DetectChannel(WORD networkId, WORD serviceId, const std::wstring& serviceName);
    static void ParseMail(const std::string& mail, std::string& color, std::string& position, std::string& size);

private:
    Callback m_callback;
    std::string m_channel;
    std::mutex m_mutex;
    std::thread m_thread;
    HANDLE m_wakeEvent = nullptr;
    std::atomic<bool> m_running{ false };
    time_t m_lastSent = 0;

    void FetchLoop();
    std::vector<Comment> Fetch(const std::string& channel, time_t startTime, time_t endTime);
    static std::string HttpGet(const std::wstring& host, const std::wstring& path);
};
