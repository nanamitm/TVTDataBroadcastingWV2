#include "pch.h"
#include "JkcnslLogin.h"

JkcnslLogin::~JkcnslLogin()
{
    Stop();
}

bool JkcnslLogin::Login(const std::wstring& jkcnslPath, const std::string& mail, const std::string& password)
{
    if (m_running) return false;
    if (mail.empty() || password.empty()) return false;
    if (mail.find_first_of("\r\n") != std::string::npos ||
        password.find_first_of("\r\n") != std::string::npos) {
        return false;
    }
    m_mail = mail;
    m_password = password;
    return StartProcess(jkcnslPath, Mode::Login);
}

bool JkcnslLogin::ClearLogin(const std::wstring& jkcnslPath)
{
    if (m_running) return false;
    return StartProcess(jkcnslPath, Mode::Clear);
}

bool JkcnslLogin::SubmitOtp(const std::string& otp)
{
    if (!m_running || m_state != State::Wait2FA) return false;
    if (otp.empty() || otp.find_first_of("\r\n") != std::string::npos) return false;
    return WriteLine("+" + otp);
}

void JkcnslLogin::Cancel()
{
    if (!m_running) return;
    if (!m_finished.exchange(true)) {
        Notify(Event::Failure, "cancel");
    }
    // 'c' breaks a pending OTP wait; Stop() then sends 'q' and tears down the
    // process/thread/handles synchronously.
    WriteLine("c");
    Stop();
}

bool JkcnslLogin::StartProcess(const std::wstring& jkcnslPath, Mode mode)
{
    // Clean up any previous (finished) session: its thread/handles linger until
    // explicitly stopped.
    Stop();

    if (GetFileAttributesW(jkcnslPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    m_mode = mode;
    m_state = (mode == Mode::Login) ? State::SetMail : State::ClearMail;
    m_finished = false;

    m_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_hStopEvent) return false;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE hStdinRead = nullptr;
    if (!CreatePipe(&hStdinRead, &m_hStdinWrite, &sa, 0)) {
        CloseHandle(m_hStopEvent); m_hStopEvent = nullptr;
        return false;
    }
    SetHandleInformation(m_hStdinWrite, HANDLE_FLAG_INHERIT, 0);

    WCHAR pipeName[64];
    swprintf_s(pipeName, L"\\\\.\\pipe\\tvtdbwv2login_%08x_%08x",
               GetCurrentProcessId(), GetCurrentThreadId());

    HANDLE hStdoutWrite = CreateNamedPipeW(pipeName,
        PIPE_ACCESS_OUTBOUND, 0, 1, 8192, 8192, 0, &sa);
    if (hStdoutWrite == INVALID_HANDLE_VALUE) {
        CloseHandle(hStdinRead); CloseHandle(m_hStdinWrite); m_hStdinWrite = nullptr;
        CloseHandle(m_hStopEvent); m_hStopEvent = nullptr;
        return false;
    }

    m_hStdoutRead = CreateFileW(pipeName, GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (m_hStdoutRead == INVALID_HANDLE_VALUE) {
        CloseHandle(hStdoutWrite);
        CloseHandle(hStdinRead); CloseHandle(m_hStdinWrite); m_hStdinWrite = nullptr;
        CloseHandle(m_hStopEvent); m_hStopEvent = nullptr;
        return false;
    }

    WCHAR args[64];
    swprintf_s(args, L" -p %u", GetCurrentProcessId());
    std::wstring cmdline = L"\"" + jkcnslPath + L"\"" + args;

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = hStdinRead;
    si.hStdOutput = hStdoutWrite;
    si.hStdError  = CreateFileW(L"nul", GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, nullptr);

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr,
                                  TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    if (si.hStdError && si.hStdError != INVALID_HANDLE_VALUE) CloseHandle(si.hStdError);
    CloseHandle(hStdinRead);
    CloseHandle(hStdoutWrite);

    if (!created) {
        CloseHandle(m_hStdoutRead); m_hStdoutRead = nullptr;
        CloseHandle(m_hStdinWrite); m_hStdinWrite = nullptr;
        CloseHandle(m_hStopEvent);  m_hStopEvent  = nullptr;
        return false;
    }

    CloseHandle(pi.hThread);
    m_hProcess = pi.hProcess;
    m_running  = true;

    m_thread = std::thread([this] { WorkerLoop(); });

    // Kick off the first command. Japanese UI text is chosen by the caller from
    // the event type; messages forwarded from here are ASCII keywords or jkcnsl's
    // own UTF-8 output lines (safe to embed in JSON).
    Notify(Event::Progress, mode == Mode::Login ? "start-login" : "start-clear");
    if (mode == Mode::Login) {
        WriteLine("Smail " + m_mail);
    } else {
        WriteLine("Smail"); // no argument => clear
    }
    return true;
}

void JkcnslLogin::Stop()
{
    if (!m_hProcess && !m_hStopEvent && !m_thread.joinable()) return;

    if (m_hStopEvent) SetEvent(m_hStopEvent);

    {
        std::lock_guard<std::mutex> lock(m_stdinMutex);
        if (m_hStdinWrite) {
            DWORD written = 0;
            WriteFile(m_hStdinWrite, "q\r\n", 3, &written, nullptr);
            CloseHandle(m_hStdinWrite);
            m_hStdinWrite = nullptr;
        }
    }

    if (m_hProcess) {
        if (WaitForSingleObject(m_hProcess, 10000) == WAIT_TIMEOUT) {
            TerminateProcess(m_hProcess, 1);
        }
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
    }

    if (m_thread.joinable()) {
        HANDLE hThread = m_thread.native_handle();
        if (WaitForSingleObject(hThread, 10000) == WAIT_TIMEOUT) {
            m_thread.detach();
        } else {
            m_thread.join();
        }
    }

    if (m_hStdoutRead) { CloseHandle(m_hStdoutRead); m_hStdoutRead = nullptr; }
    if (m_hStopEvent)  { CloseHandle(m_hStopEvent);  m_hStopEvent  = nullptr; }

    m_running = false;
}

bool JkcnslLogin::WriteLine(const std::string& s)
{
    std::string line = s + "\r\n";
    std::lock_guard<std::mutex> lock(m_stdinMutex);
    if (!m_hStdinWrite) return false;
    DWORD written = 0;
    return WriteFile(m_hStdinWrite, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr)
        && written == line.size();
}

void JkcnslLogin::Notify(Event ev, const std::string& msg)
{
    if (m_callback) m_callback(ev, msg);
}

void JkcnslLogin::Finish(Event ev, const std::string& msg)
{
    if (m_finished.exchange(true)) return;
    Notify(ev, msg);
    WriteLine("q");
    if (m_hStopEvent) SetEvent(m_hStopEvent);
}

void JkcnslLogin::HandleLine(const std::string& line)
{
    if (line.empty()) return;
    char c = line[0];
    std::string rest = line.substr(1);

    switch (c) {
    case '-':
        if ((m_state == State::LoginRun || m_state == State::Wait2FA) &&
            (rest.find("one-time password") != std::string::npos || rest.find("2FA") != std::string::npos)) {
            m_state = State::Wait2FA;
            Notify(Event::Need2FA, rest);
        } else {
            Notify(Event::Progress, rest);
        }
        break;
    case '.': // current step finished successfully
        switch (m_state) {
        case State::SetMail:
            m_state = State::SetPassword;
            WriteLine("Spassword " + m_password);
            break;
        case State::SetPassword:
            m_state = State::LoginRun;
            WriteLine("Ai");
            break;
        case State::LoginRun:
        case State::Wait2FA:
            Finish(Event::Success, "login");
            break;
        case State::ClearMail:
            m_state = State::ClearPassword;
            WriteLine("Spassword"); // no argument => clear
            break;
        case State::ClearPassword:
            Finish(Event::Success, "clear");
            break;
        }
        break;
    case '!':
    case '?':
        Finish(Event::Failure, m_mode == Mode::Login ? "login" : "clear");
        break;
    case '*':
    default:
        break;
    }
}

void JkcnslLogin::ProcessBuffer(const char* buf, DWORD size, std::string& lineBuf)
{
    for (DWORD i = 0; i < size; i++) {
        if (buf[i] == '\n') {
            if (!lineBuf.empty() && lineBuf.back() == '\r') lineBuf.pop_back();
            HandleLine(lineBuf);
            lineBuf.clear();
        } else {
            lineBuf.push_back(buf[i]);
        }
    }
}

void JkcnslLogin::WorkerLoop()
{
    HANDLE ioEvent = CreateEventW(nullptr, FALSE, TRUE, nullptr);
    if (!ioEvent) {
        m_running = false;
        return;
    }

    HANDLE olEvents[2] = { m_hStopEvent, ioEvent };
    OVERLAPPED ol = {};
    ol.hEvent = nullptr;
    char olBuf[8192];
    std::string lineBuf;

    for (;;) {
        DWORD ret = WaitForMultipleObjects(2, olEvents, FALSE, INFINITE);
        if (ret == WAIT_OBJECT_0) {
            break;
        }
        if (ret == WAIT_OBJECT_0 + 1) {
            if (ol.hEvent) {
                DWORD xferred = 0;
                if (GetOverlappedResult(m_hStdoutRead, &ol, &xferred, FALSE) && xferred > 0) {
                    ProcessBuffer(olBuf, xferred, lineBuf);
                }
            }
            ol.hEvent = ioEvent;
            while (ReadFile(m_hStdoutRead, olBuf, sizeof(olBuf), nullptr, &ol)) {
                DWORD xferred = 0;
                if (GetOverlappedResult(m_hStdoutRead, &ol, &xferred, FALSE) && xferred > 0) {
                    ProcessBuffer(olBuf, xferred, lineBuf);
                }
            }
            if (GetLastError() != ERROR_IO_PENDING) {
                ol.hEvent = nullptr;
                break;
            }
        }
    }

    if (ol.hEvent) {
        CancelIo(m_hStdoutRead);
        DWORD xferred = 0;
        GetOverlappedResult(m_hStdoutRead, &ol, &xferred, TRUE);
    }

    CloseHandle(ioEvent);

    // The jkcnsl pipe closed before a terminator arrived (e.g. crash/exit).
    if (!m_finished.exchange(true)) {
        Notify(Event::Failure, "disconnect");
    }
    m_running = false;
}
