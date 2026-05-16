#include "pch.h"
#include "JkcnslReader.h"
#include "CommentFetcher.h"

static void JkDbg(const char* msg)
{
    OutputDebugStringA("[TVTDataBroadcastingWV2/jkcnsl] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

// Valid jikkyo channels for jkcnsl L command: jk1-jk9, jk101, jk211
static bool IsValidJkChannel(const std::string& ch)
{
    if (ch.size() < 3 || ch[0] != 'j' || ch[1] != 'k') return false;
    int n = 0;
    try { n = std::stoi(ch.substr(2)); } catch (...) { return false; }
    return (n >= 1 && n <= 9) || n == 101 || n == 211;
}

/*static*/ std::string JkcnslReader::GetXmlAttr(const std::string& xml, const std::string& attr)
{
    std::string key = attr + "=\"";
    auto pos = xml.find(key);
    if (pos == std::string::npos) return "";
    pos += key.size();
    auto end = xml.find('"', pos);
    if (end == std::string::npos) return "";
    return xml.substr(pos, end - pos);
}

/*static*/ bool JkcnslReader::ParseChatLine(const std::string& line, Comment& out)
{
    if (line.size() < 2 || line[0] != '-') return false;
    const std::string xml = line.substr(1);
    if (xml.find("<chat") == std::string::npos) return false;

    auto dateStr = GetXmlAttr(xml, "date");
    if (dateStr.empty()) return false;
    try { out.date = std::stoll(dateStr); } catch (...) { return false; }

    auto contentStart = xml.find('>');
    if (contentStart == std::string::npos) return false;
    auto contentEnd = xml.rfind("</chat>");
    if (contentEnd == std::string::npos || contentEnd <= contentStart) return false;
    out.text = xml.substr(contentStart + 1, contentEnd - contentStart - 1);
    if (out.text.empty()) return false;

    CommentFetcher::ParseMail(GetXmlAttr(xml, "mail"), out.color, out.position, out.size);
    return true;
}

void JkcnslReader::ProcessBuffer(const char* buf, DWORD size, std::string& lineBuf)
{
    for (DWORD i = 0; i < size; i++) {
        if (buf[i] == '\n') {
            if (!lineBuf.empty() && lineBuf.back() == '\r') lineBuf.pop_back();
            Comment c;
            if (ParseChatLine(lineBuf, c) && m_callback) {
                std::vector<Comment> batch;
                batch.push_back(std::move(c));
                m_callback(std::move(batch));
            }
            lineBuf.clear();
        } else {
            lineBuf.push_back(buf[i]);
        }
    }
}

// ReadLoop uses overlapped I/O + WaitForMultipleObjects, mirroring NicoJK's JKStream.
// olEvents[0] = m_hStopEvent  (manual-reset: signals the loop to exit)
// olEvents[1] = ioEvent       (auto-reset:   signals when async read completes)
void JkcnslReader::ReadLoop()
{
    JkDbg("ReadLoop started");

    // ioEvent starts signaled so the first WaitForMultipleObjects returns immediately
    // and kicks off the first async ReadFile.
    HANDLE ioEvent = CreateEventW(nullptr, FALSE, TRUE, nullptr);
    if (!ioEvent) {
        JkDbg("ReadLoop: CreateEvent failed");
        m_running = false;
        return;
    }

    HANDLE olEvents[2] = { m_hStopEvent, ioEvent };
    OVERLAPPED ol = {};
    ol.hEvent = nullptr; // no pending read yet
    char olBuf[8192];
    std::string lineBuf;

    for (;;) {
        DWORD ret = WaitForMultipleObjects(2, olEvents, FALSE, INFINITE);

        if (ret == WAIT_OBJECT_0) {
            // Stop event — exit loop
            break;
        }

        if (ret == WAIT_OBJECT_0 + 1) {
            // I/O event: collect result of the previous async read (if any)
            if (ol.hEvent) {
                DWORD xferred = 0;
                if (GetOverlappedResult(m_hStdoutRead, &ol, &xferred, FALSE) && xferred > 0) {
                    ProcessBuffer(olBuf, xferred, lineBuf);
                }
            }

            // Drain synchronously, then issue a new async read
            ol.hEvent = ioEvent;
            while (ReadFile(m_hStdoutRead, olBuf, sizeof(olBuf), nullptr, &ol)) {
                DWORD xferred = 0;
                if (GetOverlappedResult(m_hStdoutRead, &ol, &xferred, FALSE) && xferred > 0) {
                    ProcessBuffer(olBuf, xferred, lineBuf);
                }
            }

            if (GetLastError() != ERROR_IO_PENDING) {
                // Pipe closed or unrecoverable error
                ol.hEvent = nullptr;
                break;
            }
            // Async read is now pending; wait for ioEvent to fire
        }
    }

    // Cancel any pending async I/O before exiting
    if (ol.hEvent) {
        CancelIo(m_hStdoutRead);
        DWORD xferred = 0;
        GetOverlappedResult(m_hStdoutRead, &ol, &xferred, TRUE);
    }

    CloseHandle(ioEvent);
    JkDbg("ReadLoop ended");
    m_running = false;
}

bool JkcnslReader::Start(const std::wstring& jkcnslPath, const std::string& jkChannel)
{
    if (m_running) return true;
    if (jkChannel.empty()) return false;

    if (!IsValidJkChannel(jkChannel)) {
        char buf[64];
        sprintf_s(buf, "channel '%s' not supported by L command, skipping", jkChannel.c_str());
        JkDbg(buf);
        return false;
    }

    if (GetFileAttributesW(jkcnslPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        JkDbg("jkcnsl.exe not found");
        return false;
    }

    // Manual-reset stop event (initially not signaled)
    m_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_hStopEvent) return false;

    // Stdin pipe (synchronous)
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE hStdinRead = nullptr;
    if (!CreatePipe(&hStdinRead, &m_hStdinWrite, &sa, 0)) {
        CloseHandle(m_hStopEvent); m_hStopEvent = nullptr;
        return false;
    }
    SetHandleInformation(m_hStdinWrite, HANDLE_FLAG_INHERIT, 0);

    // Named pipe for stdout with FILE_FLAG_OVERLAPPED (same technique as NicoJK)
    WCHAR pipeName[64];
    swprintf_s(pipeName, L"\\\\.\\pipe\\tvtdbwv2_%08x_%08x",
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

    // Launch jkcnsl.exe with -p {pid} so it self-exits if TVTest crashes
    WCHAR args[64];
    swprintf_s(args, L" -p %u", GetCurrentProcessId());
    std::wstring cmdline = L"\"" + jkcnslPath + L"\"" + args;

    STARTUPINFOW si{};
    si.cb        = sizeof(si);
    si.dwFlags   = STARTF_USESTDHANDLES;
    si.hStdInput = hStdinRead;
    si.hStdOutput = hStdoutWrite;
    si.hStdError  = CreateFileW(L"nul", GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, nullptr);

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr,
                                  TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    if (si.hStdError && si.hStdError != INVALID_HANDLE_VALUE) CloseHandle(si.hStdError);
    CloseHandle(hStdinRead);
    CloseHandle(hStdoutWrite); // jkcnsl now owns the write end via inheritance

    if (!created) {
        JkDbg("CreateProcess failed");
        CloseHandle(m_hStdoutRead); m_hStdoutRead = nullptr;
        CloseHandle(m_hStdinWrite); m_hStdinWrite = nullptr;
        CloseHandle(m_hStopEvent); m_hStopEvent = nullptr;
        return false;
    }

    CloseHandle(pi.hThread);
    m_hProcess = pi.hProcess;
    m_channel  = jkChannel;
    m_running  = true;

    m_thread = std::thread([this] { ReadLoop(); });

    // Send stream command
    std::string cmd = "L" + jkChannel + "\r\n";
    JkDbg(("Sending: " + cmd).c_str());
    DWORD written = 0;
    WriteFile(m_hStdinWrite, cmd.c_str(), static_cast<DWORD>(cmd.size()), &written, nullptr);

    return true;
}

void JkcnslReader::Stop()
{
    if (!m_hProcess && !m_hStopEvent) return;

    // 1. Signal ReadLoop to exit
    if (m_hStopEvent) SetEvent(m_hStopEvent);

    // 2. Tell jkcnsl to quit gracefully, then close stdin
    if (m_hStdinWrite) {
        DWORD written = 0;
        WriteFile(m_hStdinWrite, "q\r\n", 3, &written, nullptr);
        CloseHandle(m_hStdinWrite);
        m_hStdinWrite = nullptr;
    }

    // 3. Wait for jkcnsl to exit; forcibly terminate if it takes too long
    if (m_hProcess) {
        if (WaitForSingleObject(m_hProcess, 10000) == WAIT_TIMEOUT) {
            JkDbg("jkcnsl did not exit in time, terminating");
            TerminateProcess(m_hProcess, 1);
        }
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
    }

    // 4. Wait for ReadLoop thread; detach if it hangs
    if (m_thread.joinable()) {
        HANDLE hThread = m_thread.native_handle();
        if (WaitForSingleObject(hThread, 10000) == WAIT_TIMEOUT) {
            JkDbg("ReadLoop thread did not exit in time, detaching");
            m_thread.detach();
        } else {
            m_thread.join();
        }
    }

    // 5. Release remaining handles
    if (m_hStdoutRead) { CloseHandle(m_hStdoutRead); m_hStdoutRead = nullptr; }
    if (m_hStopEvent)  { CloseHandle(m_hStopEvent);  m_hStopEvent  = nullptr; }

    m_running = false;
    m_channel.clear();
    JkDbg("Stopped");
}

JkcnslReader::~JkcnslReader()
{
    Stop();
}
