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

    HANDLE hStdoutRead = nullptr, hStdoutWrite = nullptr;
    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0)) {
        CloseHandle(hStdinRead); CloseHandle(hStdinWrite);
        return false;
    }
    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);

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

    // Send the command and quit.
    std::string line = command + "\r\nq\r\n";
    DWORD written = 0;
    WriteFile(hStdinWrite, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(hStdinWrite);

    // Drain stdout until EOF (jkcnsl closes it on exit). Output is tiny, so a
    // blocking read cannot fill the pipe / deadlock.
    std::string buf;
    char chunk[1024];
    DWORD read = 0;
    while (ReadFile(hStdoutRead, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
        buf.append(chunk, read);
    }
    CloseHandle(hStdoutRead);

    if (WaitForSingleObject(pi.hProcess, kTimeoutMs) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hProcess);

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
