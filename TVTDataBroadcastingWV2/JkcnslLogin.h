#pragma once
#include "pch.h"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

// Drives nicovideo login for jkcnsl.
//
// jkcnsl no longer logs in by itself: login happens in JkcnslLoginWindow.exe
// (shipped next to jkcnsl.exe), a GUI app that opens a dedicated Edge browser
// and, when the user presses "jkcnslに保存", writes the session cookie into
// jkcnsl's nicovideo_cookie setting. 2-factor authentication happens inside
// that window too.
//
// Login:  launch JkcnslLoginWindow.exe, wait for it to exit, then query jkcnsl
//         ("S") and report Success if a nicovideo_cookie is stored.
// Logout: clear jkcnsl's stored cookie ("Snicovideo_cookie" with no value).
//         The nicovideo-side session (browser logout) is handled in
//         JkcnslLoginWindow itself.
class JkcnslLogin
{
public:
    enum class Event {
        Progress, // informational line; message holds the text
        Success,  // login (or logout) completed
        Failure,  // login failed / cancelled
    };
    using Callback = std::function<void(Event, std::string message)>;

    JkcnslLogin() = default;
    ~JkcnslLogin();
    JkcnslLogin(const JkcnslLogin&) = delete;
    JkcnslLogin& operator=(const JkcnslLogin&) = delete;

    void SetCallback(Callback cb) { m_callback = std::move(cb); }
    bool IsBusy() const { return m_running; }

    // Open JkcnslLoginWindow.exe (next to jkcnslPath) and report the result
    // after it is closed.
    bool Login(const std::wstring& jkcnslPath);
    // Clear the cookie stored in jkcnsl.
    bool Logout(const std::wstring& jkcnslPath);
    // Close JkcnslLoginWindow and stop waiting for it.
    void Cancel();
    // Stop waiting without reporting a user-visible failure. Used during
    // plug-in shutdown, before the callback target is destroyed.
    // JkcnslLoginWindow itself is left open: the user may still be using it.
    void Stop();

private:
    void LoginWorker(std::wstring jkcnslPath);
    void LogoutWorker(std::wstring jkcnslPath);
    void Finish(Event ev, const std::string& msg);
    void Notify(Event ev, const std::string& msg);
    void JoinPrevious();

    Callback m_callback;
    HANDLE m_hProcess   = nullptr;
    HANDLE m_hStopEvent = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_finished{ false };
};

// Heap payload marshalled to the UI thread (the callback runs on the worker
// thread, but WebView2 must be touched only from the UI thread).
struct JkcnslLoginEvent {
    JkcnslLogin::Event event;
    std::string        message;
};
