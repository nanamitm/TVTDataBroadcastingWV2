#pragma once
#include "pch.h"
#include "CommentFetcher.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// 接続直後にjkcnslが送る過去コメント(<x_past_chat_begin>..<x_past_chat_end>)の
// 受信中かどうか。NicoJKと同じくニコニコと避難所で別々に覚え、混合接続で片方の
// 過去コメントがもう片方の新しいコメントを巻き込まないようにする。
class PastChatTracker
{
public:
    // jkcnslの1行(行頭マーカー付きでよい)を見て状態を更新する。
    // 制御タグはrefuge="1"の有無で、chatタグはx_refuge/nx_jikkyoで接続先を区別する。
    void OnLine(const std::string& line);
    bool IsPast(bool refuge) const { return refuge ? m_refuge : m_nico; }
    void Reset() { m_nico = m_refuge = false; }

private:
    bool m_nico = false;
    bool m_refuge = false;
};

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

    // True from a successful Start() until jkcnsl closes the stream ('.'/'!'/'?'),
    // the pipe dies, or Stop() is called. Unlike IsConnected() this stays true
    // while the stream is still being established, so a watchdog can use it to
    // retry only genuinely dead streams (mirrors NicoJK's CJKStream::bOpened_).
    bool IsStreamOpen() const { return m_streamOpen; }
    const std::string& GetChannel() const { return m_channel; }
    void SetCallback(Callback cb) { m_callback = std::move(cb); }

    // Notified (with the new state) whenever the stream connection changes:
    // connected once jkcnsl opens the stream ('*'), disconnected on close/stop.
    using ConnectionCallback = std::function<void(bool connected)>;
    void SetConnectionCallback(ConnectionCallback cb) { m_connCallback = std::move(cb); }

    enum class Status { Connecting, Connected, Disconnected, Failed };
    using StatusCallback = std::function<void(Status)>;
    void SetStatusCallback(StatusCallback cb) { m_statusCallback = std::move(cb); }

    // jkcnslが接続先ごとに出す<x_room>(接続した)と<x_disconnect>(混合接続で片方が
    // 切断または接続失敗した)。NicoJKと同じく接続先を名指しした状態行に使う。
    struct RoomEvent
    {
        bool disconnect = false; // false=<x_room>、true=<x_disconnect>
        bool refuge = false;     // 避難所側 (refuge="1")
        bool loggedIn = false;   // <x_room is_logged_in="1">
        std::string nickname;    // <x_room nickname="..."> (実体参照は戻し済み)
        int status = 0;          // <x_disconnect status="...">
    };
    using RoomCallback = std::function<void(const RoomEvent&)>;
    void SetRoomCallback(RoomCallback cb) { m_roomCallback = std::move(cb); }
    // jkcnslの1行(行頭マーカー付きでよい)が<x_room>/<x_disconnect>なら解釈する
    static bool ParseRoomLine(const std::string& line, RoomEvent& out);

    // 実況ログに残す価値のある状態変化かを返す。lastは直前に残した状態 (まだ
    // 何も残していなければ空)。つながらないままだとウォッチドッグが20秒ごとに
    // 張り直すので、同じ状態の繰り返しと、失敗したまま再試行している間の接続
    // 開始は残さない。ログがコメントではなく状態行で埋まるのを防ぐ。
    static bool ShouldLogStatus(std::optional<Status> last, Status next);

    static bool ParseChatLine(const std::string& line, Comment& out);
    // jkcnslの行頭マーカーを除いた "<chat ...>...</chat>" 単体を解釈する。
    // ParseChatLineと違い本文が空でも成功する([CustomReplace]の置換結果用)。
    static bool ParseChatXml(const std::string& xml, Comment& out);
    // chat本文の実体参照(&lt; &gt; &amp; &quot; &apos; &#10; &#13;)を文字に戻す。
    static std::string DecodeEntityReference(const std::string& s);

private:
    Callback m_callback;
    ConnectionCallback m_connCallback;
    StatusCallback m_statusCallback;
    RoomCallback m_roomCallback;
    HANDLE m_hProcess    = nullptr;
    HANDLE m_hStdinWrite = nullptr;
    HANDLE m_hStdoutRead = nullptr; // overlapped read end
    HANDLE m_hStopEvent  = nullptr; // manual-reset, signals ReadLoop to exit
    std::mutex m_stdinMutex;        // guards writes to / closing of m_hStdinWrite
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_connected{ false };
    std::atomic<bool> m_streamOpen{ false };
    PastChatTracker m_pastChat; // x_past_chat range state (worker thread only)
    std::string m_channel;

    void ReadLoop();
    void ProcessBuffer(const char* buf, DWORD size, std::string& lineBuf);
    void SetConnected(bool connected);
    void CloseStream(bool failed = false);
    static std::string GetXmlAttr(const std::string& xml, const std::string& attr);
};
