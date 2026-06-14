#pragma once
#include "pch.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// Drives jkcnsl's nicovideo login over a single process, mirroring NicoJK's
// login state machine. jkcnsl reads one command per stdin line and emits
// '-' output lines terminated by '.' (ok) / '!' (error) / '?' (unknown).
//
// Login sequence (one process handles all steps sequentially):
//   Smail {mail}      -> .
//   Spassword {pw}    -> .
//   Ai                -> -progress... ; if a line mentions "one-time password"
//                        the caller must SubmitOtp(); then . (success) / ! (fail)
//
// Clear sequence:
//   Smail   (no arg)  -> .
//   Spassword         -> .
//
// On success jkcnsl persists the session cookie, so later 'L' streams (opened
// by JkcnslReader) become authenticated and can post comments.
class JkcnslLogin
{
public:
    enum class Event {
        Progress, // informational line; message holds the text
        Need2FA,  // waiting for a one-time password; caller should SubmitOtp()
        Success,  // login (or clear) completed
        Failure,  // login failed / cancelled
    };
    using Callback = std::function<void(Event, std::string message)>;

    JkcnslLogin() = default;
    ~JkcnslLogin();
    JkcnslLogin(const JkcnslLogin&) = delete;
    JkcnslLogin& operator=(const JkcnslLogin&) = delete;

    void SetCallback(Callback cb) { m_callback = std::move(cb); }
    bool IsBusy() const { return m_running; }

    // Start an interactive login. mail/password are UTF-8.
    bool Login(const std::wstring& jkcnslPath, const std::string& mail, const std::string& password);
    // Clear the stored mail/password/cookie.
    bool ClearLogin(const std::wstring& jkcnslPath);
    // Submit a one-time password while in the Need2FA state.
    bool SubmitOtp(const std::string& otp);
    // Abort an in-progress login.
    void Cancel();

private:
    enum class Mode { Login, Clear };
    enum class State { SetMail, SetPassword, LoginRun, Wait2FA, ClearMail, ClearPassword };

    bool StartProcess(const std::wstring& jkcnslPath, Mode mode);
    void Stop();
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
    State m_state = State::SetMail;
    std::string m_mail;
    std::string m_password;
};

// Heap payload marshalled to the UI thread (the callback runs on the worker
// thread, but WebView2 must be touched only from the UI thread).
struct JkcnslLoginEvent {
    JkcnslLogin::Event event;
    std::string        message;
};
