#pragma once
#include "pch.h"
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

struct Comment {
    std::string text;
    std::string color;    // CSS色名
    std::string position; // "naka" / "ue" / "shita"
    std::string size;     // "small" / "medium" / "big"
};

class CommentFetcher
{
public:
    using Callback = std::function<void(std::vector<Comment>)>;

    CommentFetcher() = default;
    ~CommentFetcher();
    CommentFetcher(const CommentFetcher&) = delete;
    CommentFetcher& operator=(const CommentFetcher&) = delete;

    // チャンネル変更（空文字列で取得停止）
    void SetChannel(const std::string& jikkyoChannel);
    void SetCallback(Callback cb) { m_callback = std::move(cb); }
    void Start();
    void Stop();

    // NetworkID + ServiceID から実況チャンネル文字列へ変換
    // 対応していない場合は空文字列を返す
    static std::string DetectChannel(WORD networkId, WORD serviceId, const std::wstring& serviceName);

private:
    Callback m_callback;
    std::string m_channel;
    std::mutex m_mutex;
    std::thread m_thread;
    HANDLE m_wakeEvent = nullptr;
    std::atomic<bool> m_running{ false };
    // 重複送信防止：最後に送信したコメントの日時（Unix秒）
    time_t m_lastSent = 0;

    void FetchLoop();
    std::vector<Comment> Fetch(const std::string& channel, time_t from, time_t to);
    static void ParseMail(const std::string& mail, std::string& color, std::string& position, std::string& size);
    static std::string HttpGet(const std::wstring& host, const std::wstring& path);
};
