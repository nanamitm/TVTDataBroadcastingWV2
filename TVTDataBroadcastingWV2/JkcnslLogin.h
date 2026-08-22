#pragma once
#include "pch.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// Drives jkcnsl's nicovideo login over a single process. jkcnsl reads one
// command per stdin line and emits '-' output lines terminated by '.' (ok) /
// '!' (error) / '?' (unknown).
//
// jkcnsl authenticates through its bundled browser helper
// (jkcnsl_login/jkcnsl-qt-login.exe): 'Ai' opens a browser window, the user
// signs in there, and the helper hands the session cookie back over a named
// pipe. Mail/password/one-time-password are no longer involved -- two-factor
// authentication happens inside that browser window. Requires a jkcnsl build
// with the browser helper; older mail+password builds are not supported.
//
// Login sequence:
//   Ai   -> -progress...  ; '.' success / '!' failure
//           If a stored cookie is still valid jkcnsl answers '.' immediately
//           without opening any window.
//
// Logout sequence (server-side logout, then drop the stale local settings):
//   Ao                -> .
//   Smail   (no arg)  -> .
//   Spassword         -> .
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

    // Start a browser login. Opens jkcnsl's helper window unless the stored
    // cookie is still valid.
    bool Login(const std::wstring& jkcnslPath);
    // Log out on the nicovideo side, then clear the stored cookie/mail/password.
    bool Logout(const std::wstring& jkcnslPath);
    // Abort an in-progress login.
    void Cancel();
    // Stop the helper without reporting a user-visible failure. Used during
    // plug-in shutdown, before the callback target is destroyed.
    void Stop();

private:
    enum class Mode { Login, Logout };
    enum class State { LoginRun, LogoutRun, ClearMail, ClearPassword };

    bool StartProcess(const std::wstring& jkcnslPath, Mode mode);
    void WorkerLoop();
    void ProcessBuffer(const char* buf, DWORD size, std::string& lineBuf);
    void HandleLine(const std::string& line);
    bool WriteLine(const std::string& s);
    void Finish(Event ev, const std::string& msg);
    void Notify(Event ev, const std::string& msg);

    Callback m_callback;
    HANDLE m_hProcess    = nullptr;
    HANDLE m_hStdinWrite = nullptr;
    HANDLE m_hStdoutRead = nullptr;
    HANDLE m_hStopEvent  = nullptr;
    std::mutex m_stdinMutex;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_finished{ false };

    Mode  m_mode  = Mode::Login;
    State m_state = State::LoginRun;
    // Set when jkcnsl reports the browser helper is missing, so the failure can
    // name the real cause instead of the generic "login failed".
    bool  m_helperMissing = false;
};

// Heap payload marshalled to the UI thread (the callback runs on the worker
// thread, but WebView2 must be touched only from the UI thread).
struct JkcnslLoginEvent {
    JkcnslLogin::Event event;
    std::string        message;
};
