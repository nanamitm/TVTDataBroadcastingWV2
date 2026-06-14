#include "pch.h"
#include "JkcnslSettings.h"
#include <vector>

namespace {
constexpr DWORD kTimeoutMs = 8000;
}

/*static*/ bool JkcnslSettings::RunCommand(const std::wstring& jkcnslPath,
                                           const std::string& command,
                                           std::string* output)
{
    if (command.find_first_of("\r\n") != std::string::npos) return false;
    if (GetFileAttributesW(jkcnslPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };

    HANDLE hStdinRead = nullptr, hStdinWrite = nullptr;
    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0)) return false;
    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);

    // stdout via an overlapped named pipe so we can read with a timeout. jkcnsl
    // cancels in-flight commands when it sees 'q', so we must read the command's
    // terminator ('.'/'!'/'?') BEFORE sending 'q' (otherwise output is empty).
    WCHAR pipeName[64];
    swprintf_s(pipeName, L"\\\\.\\pipe\\tvtdbwv2set_%08x_%08x",
               GetCurrentProcessId(), GetCurrentThreadId());
    HANDLE hStdoutWrite = CreateNamedPipeW(pipeName, PIPE_ACCESS_OUTBOUND, 0, 1, 8192, 8192, 0, &sa);
    if (hStdoutWrite == INVALID_HANDLE_VALUE) {
        CloseHandle(hStdinRead); CloseHandle(hStdinWrite);
        return false;
    }
    HANDLE hStdoutRead = CreateFileW(pipeName, GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (hStdoutRead == INVALID_HANDLE_VALUE) {
        CloseHandle(hStdoutWrite);
        CloseHandle(hStdinRead); CloseHandle(hStdinWrite);
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
    CloseHandle(hStdoutWrite); // jkcnsl owns the write end; needed for EOF on exit

    if (!created) {
        CloseHandle(hStdinWrite);
        CloseHandle(hStdoutRead);
        return false;
    }
    CloseHandle(pi.hThread);

    // 1) Send the command only (no 'q' yet).
    {
        std::string line = command + "\r\n";
        DWORD written = 0;
        WriteFile(hStdinWrite, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
    }

    // 2) Read stdout until a terminator line ('.'/'!'/'?') appears or we time out.
    std::string buf;
    HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
    char rb[2048];
    bool sawTerminator = false;
    while (ioEvent && !sawTerminator) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        DWORD remain = static_cast<DWORD>(deadline - now);

        OVERLAPPED ol{};
        ol.hEvent = ioEvent;
        ResetEvent(ioEvent);
        DWORD rd = 0;
        BOOL r = ReadFile(hStdoutRead, rb, sizeof(rb), nullptr, &ol);
        if (!r && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ioEvent, remain) != WAIT_OBJECT_0) { CancelIo(hStdoutRead); break; }
        } else if (!r) {
            break; // pipe closed / error
        }
        if (!GetOverlappedResult(hStdoutRead, &ol, &rd, FALSE) || rd == 0) break;
        buf.append(rb, rd);

        // Look for any terminator at a line start.
        size_t s = 0;
        while (s <= buf.size()) {
            char c = (s < buf.size()) ? buf[s] : '\0';
            if (c == '.' || c == '!' || c == '?') { sawTerminator = true; break; }
            size_t nl = buf.find('\n', s);
            if (nl == std::string::npos) break;
            s = nl + 1;
        }
    }
    if (ioEvent) CloseHandle(ioEvent);

    // 3) Now quit and clean up.
    {
        DWORD written = 0;
        WriteFile(hStdinWrite, "q\r\n", 3, &written, nullptr);
    }
    CloseHandle(hStdinWrite);
    if (WaitForSingleObject(pi.hProcess, 3000) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(hStdoutRead);

    // Classify by terminator lines: '.' ok, '!' / '?' error. Collect '-' lines.
    bool ok = false, err = false;
    size_t start = 0;
    while (start <= buf.size()) {
        size_t nl = buf.find('\n', start);
        std::string l = buf.substr(start, (nl == std::string::npos ? buf.size() : nl) - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty()) {
            if (l[0] == '.')      ok = true;
            else if (l[0] == '!' || l[0] == '?') err = true;
            else if (l[0] == '-' && output) output->append(l.substr(1)).push_back('\n');
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return ok && !err;
}

/*static*/ bool JkcnslSettings::QueryLogin(const std::wstring& jkcnslPath, LoginInfo& out)
{
    out = LoginInfo{};
    std::string output;
    // "S" with no argument dumps all settings as "-key value" lines.
    if (!RunCommand(jkcnslPath, "S", &output)) return false;

    size_t start = 0;
    while (start <= output.size()) {
        size_t nl = output.find('\n', start);
        std::string line = output.substr(start, (nl == std::string::npos ? output.size() : nl) - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        auto sp = line.find(' ');
        std::string key = (sp == std::string::npos) ? line : line.substr(0, sp);
        std::string val = (sp == std::string::npos) ? ""   : line.substr(sp + 1);
        if (key == "nicovideo_cookie" && !val.empty()) out.loggedIn = true;
        else if (key == "mail")                        out.mail = val;
        else if (key == "cache_server_url")            out.cacheServerUrl = val;

        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return true;
}

/*static*/ bool JkcnslSettings::SetCacheServerUrl(const std::wstring& jkcnslPath, const std::string& url)
{
    // "Scache_server_url {url}" sets it; "Scache_server_url" (no arg) clears it.
    std::string cmd = url.empty() ? "Scache_server_url" : ("Scache_server_url " + url);
    return RunCommand(jkcnslPath, cmd);
}
