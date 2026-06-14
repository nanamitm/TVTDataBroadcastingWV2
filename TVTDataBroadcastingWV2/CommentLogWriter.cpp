#include "pch.h"
#include "CommentLogWriter.h"

CommentLogWriter::~CommentLogWriter()
{
    Close();
}

void CommentLogWriter::Configure(const std::wstring& folder, bool enabled)
{
    if (folder != m_folder || enabled != m_enabled) {
        Close();
    }
    m_folder  = folder;
    m_enabled = enabled && !folder.empty();
}

void CommentLogWriter::Close()
{
    if (m_hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hFile);
        m_hFile = INVALID_HANDLE_VALUE;
    }
    if (m_hLock != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hLock);
        m_hLock = INVALID_HANDLE_VALUE;
        // Remove the lockfile for the channel we were recording.
        if (m_curJk >= 0) {
            wchar_t name[64];
            swprintf_s(name, L"\\jk%d\\lockfile", m_curJk);
            DeleteFileW((m_folder + name).c_str());
        }
    }
    m_curJk = -1;
}

bool CommentLogWriter::Open(int jkID, time_t date)
{
    wchar_t dir[64];
    swprintf_s(dir, L"\\jk%d", jkID);
    std::wstring path = m_folder + dir;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
        !CreateDirectoryW(path.c_str(), nullptr)) {
        return false;
    }

    // Lockfile (NicoJK convention).
    std::wstring lockPath = path + L"\\lockfile";
    m_hLock = CreateFileW(lockPath.c_str(), GENERIC_WRITE, 0, nullptr,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_hLock == INVALID_HANDLE_VALUE) return false;

    wchar_t fname[64];
    swprintf_s(fname, L"\\jk%d\\%010llu.txt", jkID, static_cast<unsigned long long>(date));
    m_hFile = CreateFileW((m_folder + fname).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                          nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_hFile == INVALID_HANDLE_VALUE) {
        CloseHandle(m_hLock);
        m_hLock = INVALID_HANDLE_VALUE;
        DeleteFileW(lockPath.c_str());
        return false;
    }

    // Header (optional, NicoJK-compatible comment).
    time_t t = date;
    tm lt{};
    localtime_s(&lt, &t);
    char header[128];
    int len = sprintf_s(header, "<!-- NicoJK logfile from %04d-%02d-%02dT%02d:%02d:%02d -->\r\n",
                        lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    DWORD written;
    WriteFile(m_hFile, header, len, &written, nullptr);

    m_curJk = jkID;
    return true;
}

void CommentLogWriter::Write(int jkID, time_t date, const std::string& rawChatLine)
{
    if (!m_enabled || jkID <= 0 || rawChatLine.empty()) return;

    if (m_curJk != jkID) {
        Close();
        if (!Open(jkID, date)) return;
    }
    if (m_hFile == INVALID_HANDLE_VALUE) return;

    DWORD written;
    WriteFile(m_hFile, rawChatLine.c_str(), static_cast<DWORD>(rawChatLine.size()), &written, nullptr);
    WriteFile(m_hFile, "\r\n", 2, &written, nullptr);
}
