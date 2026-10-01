#include "pch.h"
#include "JkcnslLogin.h"
#include "JkcnslSettings.h"
#include <filesystem>

JkcnslLogin::~JkcnslLogin()
{
    Stop();
}

bool JkcnslLogin::Login(const std::wstring& jkcnslPath)
{
    if (m_running) return false;
    if (GetFileAttributesW(jkcnslPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    JoinPrevious();

    // JkcnslLoginWindow.exeは同じフォルダのjkcnsl.exeの設定を読み書きする
    std::filesystem::path dir = std::filesystem::path(jkcnslPath).parent_path();
    std::wstring helperPath = (dir / L"JkcnslLoginWindow.exe").wstring();
    if (GetFileAttributesW(helperPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        m_finished = false;
        Finish(Event::Failure, "helper-missing");
        return true;
    }

    m_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_hStopEvent) return false;

    std::wstring cmdline = L"\"" + helperPath + L"\"";
    std::wstring dirStr = dir.wstring();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(helperPath.c_str(), cmdline.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, dirStr.c_str(), &si, &pi)) {
        CloseHandle(m_hStopEvent); m_hStopEvent = nullptr;
        return false;
    }
    CloseHandle(pi.hThread);
    m_hProcess = pi.hProcess;
    m_finished = false;
    m_running  = true;

    // Japanese UI text is chosen by the caller from the event type; messages
    // forwarded from here are ASCII keywords.
    Notify(Event::Progress, "helper-open");
    m_thread = std::thread([this, jkcnslPath] { LoginWorker(jkcnslPath); });
    return true;
}

bool JkcnslLogin::Logout(const std::wstring& jkcnslPath)
{
    if (m_running) return false;
    if (GetFileAttributesW(jkcnslPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    JoinPrevious();

    m_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_hStopEvent) return false;
    m_finished = false;
    m_running  = true;

    Notify(Event::Progress, "start-logout");
    m_thread = std::thread([this, jkcnslPath] { LogoutWorker(jkcnslPath); });
    return true;
}

static BOOL CALLBACK CloseProcessWindowsEnumProc(HWND hwnd, LPARAM lParam)
{
    DWORD pid = 0;
    if (GetWindowThreadProcessId(hwnd, &pid) && pid == static_cast<DWORD>(lParam) && IsWindowVisible(hwnd)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

void JkcnslLogin::Cancel()
{
    if (!m_running) return;
    Finish(Event::Failure, "cancel");
    if (m_hProcess) {
        // 保存されていない操作は破棄してよいので、ウィンドウを閉じてもらう
        EnumWindows(CloseProcessWindowsEnumProc, static_cast<LPARAM>(GetProcessId(m_hProcess)));
    }
    Stop();
}

void JkcnslLogin::JoinPrevious()
{
    // Clean up any previous (finished) session: its thread/handles linger until
    // explicitly stopped.
    Stop();
}

void JkcnslLogin::Stop()
{
    if (!m_hProcess && !m_hStopEvent && !m_thread.joinable()) return;

    // A shutdown is not a login failure. This also prevents the worker from
    // invoking the UI callback while its owner is being torn down.
    m_finished = true;
    if (m_hStopEvent) SetEvent(m_hStopEvent);
    // The worker only waits on the stop event (and on jkcnsl queries that
    // also honour it), so this join returns promptly.
    if (m_thread.joinable()) m_thread.join();

    if (m_hProcess)   { CloseHandle(m_hProcess);   m_hProcess   = nullptr; }
    if (m_hStopEvent) { CloseHandle(m_hStopEvent); m_hStopEvent = nullptr; }
    m_running = false;
}

void JkcnslLogin::Notify(Event ev, const std::string& msg)
{
    if (m_callback) m_callback(ev, msg);
}

void JkcnslLogin::Finish(Event ev, const std::string& msg)
{
    if (m_finished.exchange(true)) return;
    Notify(ev, msg);
}

void JkcnslLogin::LoginWorker(std::wstring jkcnslPath)
{
    HANDLE waits[2] = { m_hStopEvent, m_hProcess };
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        // JkcnslLoginWindowが閉じられた。jkcnslに保存されたかを確かめる
        JkcnslSettings::LoginInfo info;
        if (!JkcnslSettings::QueryLogin(jkcnslPath, info, m_hStopEvent)) {
            Finish(Event::Failure, "query");
        } else if (info.loggedIn) {
            Finish(Event::Success, "login");
        } else {
            Finish(Event::Failure, "not-saved");
        }
    }
    m_running = false;
}

void JkcnslLogin::LogoutWorker(std::wstring jkcnslPath)
{
    // "Snicovideo_cookie" (no value) clears the stored cookie.
    if (JkcnslSettings::RunCommand(jkcnslPath, "Snicovideo_cookie", nullptr, m_hStopEvent)) {
        Finish(Event::Success, "logout");
    } else {
        Finish(Event::Failure, "logout");
    }
    m_running = false;
}
