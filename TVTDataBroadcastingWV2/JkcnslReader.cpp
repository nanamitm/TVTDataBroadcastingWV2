#include "pch.h"
#include "JkcnslReader.h"
#include "CommentFetcher.h"
#include <sstream>

static void JkDbg(const char* msg)
{
    OutputDebugStringA("[TVTDataBroadcastingWV2/jkcnsl] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

// Extract value of attr="..." from XML string
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

// Parse a jkcnsl stdout line like:
//   -<chat thread="..." no="..." date="1234567890" mail="white" user_id="...">text</chat>
/*static*/ bool JkcnslReader::ParseChatLine(const std::string& line, Comment& out)
{
    // jkcnsl data lines start with '-'
    if (line.size() < 2 || line[0] != '-') return false;
    const std::string xml = line.substr(1);
    if (xml.find("<chat") == std::string::npos) return false;

    auto dateStr = GetXmlAttr(xml, "date");
    if (dateStr.empty()) return false;
    try { out.date = std::stoll(dateStr); } catch (...) { return false; }

    // Extract text content between > and </chat>
    auto contentStart = xml.find('>');
    if (contentStart == std::string::npos) return false;
    auto contentEnd = xml.rfind("</chat>");
    if (contentEnd == std::string::npos || contentEnd <= contentStart) return false;
    out.text = xml.substr(contentStart + 1, contentEnd - contentStart - 1);
    if (out.text.empty()) return false;

    CommentFetcher::ParseMail(GetXmlAttr(xml, "mail"), out.color, out.position, out.size);
    return true;
}

void JkcnslReader::ReadLoop()
{
    JkDbg("ReadLoop started");
    std::string lineBuf;
    char buf[4096];

    for (;;) {
        DWORD read = 0;
        BOOL ok = ReadFile(m_hStdoutRead, buf, sizeof(buf) - 1, &read, nullptr);
        if (!ok || read == 0) break;
        buf[read] = '\0';

        // Accumulate into lineBuf and process complete lines
        for (DWORD i = 0; i < read; i++) {
            if (buf[i] == '\n') {
                // Remove trailing \r
                if (!lineBuf.empty() && lineBuf.back() == '\r') lineBuf.pop_back();

                Comment c;
                if (ParseChatLine(lineBuf, c)) {
                    if (m_callback) {
                        std::vector<Comment> batch;
                        batch.push_back(std::move(c));
                        m_callback(std::move(batch));
                    }
                }
                lineBuf.clear();
            } else {
                lineBuf.push_back(buf[i]);
            }
        }
    }

    JkDbg("ReadLoop ended");
    m_running = false;
}

bool JkcnslReader::Start(const std::wstring& jkcnslPath, const std::string& jkChannel)
{
    if (m_running) return true;
    if (jkChannel.empty()) return false;

    // Verify jkcnsl.exe exists
    if (GetFileAttributesW(jkcnslPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        JkDbg("jkcnsl.exe not found");
        return false;
    }

    // Create pipes
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE hStdinRead  = nullptr;
    HANDLE hStdoutWrite = nullptr;

    if (!CreatePipe(&hStdinRead, &m_hStdinWrite, &sa, 0)) return false;
    SetHandleInformation(m_hStdinWrite, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&m_hStdoutRead, &hStdoutWrite, &sa, 0)) {
        CloseHandle(hStdinRead); CloseHandle(m_hStdinWrite); m_hStdinWrite = nullptr;
        return false;
    }
    SetHandleInformation(m_hStdoutRead, HANDLE_FLAG_INHERIT, 0);

    // Build command line: jkcnsl.exe -p {pid}
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
        JkDbg("CreateProcess failed");
        CloseHandle(m_hStdinWrite); m_hStdinWrite = nullptr;
        CloseHandle(m_hStdoutRead); m_hStdoutRead = nullptr;
        return false;
    }

    CloseHandle(pi.hThread);
    m_hProcess = pi.hProcess;
    m_running  = true;

    // Start reader thread
    m_thread = std::thread([this] { ReadLoop(); });

    // Send stream command: R1 {nx-jikkyo-url}
    // NX-Jikkyo: refuge stream, no authentication required
    std::string url  = "https://nx-jikkyo.tsukumijima.net/watch/" + jkChannel;
    std::string cmd  = "R1 " + url + "\r\n";
    JkDbg(("Sending: " + cmd).c_str());

    DWORD written = 0;
    WriteFile(m_hStdinWrite, cmd.c_str(), static_cast<DWORD>(cmd.size()), &written, nullptr);

    return true;
}

void JkcnslReader::Stop()
{
    if (!m_running && !m_hProcess) return;

    // Send quit command
    if (m_hStdinWrite) {
        DWORD written = 0;
        WriteFile(m_hStdinWrite, "q\r\n", 3, &written, nullptr);
        CloseHandle(m_hStdinWrite);
        m_hStdinWrite = nullptr;
    }

    if (m_hProcess) {
        WaitForSingleObject(m_hProcess, 5000);
        TerminateProcess(m_hProcess, 0);
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
    }

    if (m_hStdoutRead) {
        CloseHandle(m_hStdoutRead);
        m_hStdoutRead = nullptr;
    }

    if (m_thread.joinable()) m_thread.join();

    m_running = false;
    JkDbg("Stopped");
}

JkcnslReader::~JkcnslReader()
{
    Stop();
}
