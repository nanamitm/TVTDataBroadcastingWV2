#include "pch.h"
#include "CommentLogWriter.h"
#include <string_view>
#include <vector>

namespace {
constexpr ULONGLONG kMiB = 1024 * 1024;
constexpr ULONGLONG kTicksPerDay = 24ULL * 60 * 60 * 10000000;

bool IsDigits(std::wstring_view text)
{
    return !text.empty() && text.find_first_not_of(L"0123456789") == std::wstring_view::npos;
}

ULONGLONG JoinDwords(DWORD high, DWORD low)
{
    return (static_cast<ULONGLONG>(high) << 32) | low;
}

struct LogFile
{
    std::wstring path;
    ULONGLONG bytes;
    ULONGLONG modified;
};

bool TryDeleteLog(const LogFile& file)
{
    // Request DELETE without sharing writes. Active writers are protected by
    // Windows sharing rules, and the file cannot change once this succeeds.
    HANDLE handle = CreateFileW(file.path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_DISPOSITION_INFO disposition{ TRUE };
    const bool removed = GetFileInformationByHandle(handle, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        JoinDwords(info.nFileSizeHigh, info.nFileSizeLow) == file.bytes &&
        JoinDwords(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime) == file.modified &&
        SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
    CloseHandle(handle);
    return removed;
}
}

CommentLogWriter::~CommentLogWriter()
{
    Close();
}

void CommentLogWriter::Configure(const std::wstring& folder, bool enabled, CommentLogLimits limits)
{
    enabled = enabled && !folder.empty();
    if (folder != m_folder || enabled != m_enabled) {
        Close();
    }
    if (folder != m_folder || enabled != m_enabled || limits != m_limits) {
        m_nextCleanup = 0;
    }
    m_folder  = folder;
    m_enabled = enabled;
    m_limits = limits;
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
    m_fileBytes = 0;
    m_fileStart = 0;
    m_filePath.clear();
}

bool CommentLogWriter::Open(int jkID, time_t date)
{
    // Ensure the base folder exists, then the jk{ID} subfolder.
    if (GetFileAttributesW(m_folder.c_str()) == INVALID_FILE_ATTRIBUTES &&
        !CreateDirectoryW(m_folder.c_str(), nullptr)) {
        return false;
    }
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
    // CREATE_ALWAYSだと同じ名前(同じ開始秒)の既存ログを切り詰めてしまうので追記で開く
    const std::wstring filePath = m_folder + fname;
    m_hFile = CreateFileW(filePath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_hFile == INVALID_HANDLE_VALUE) {
        CloseHandle(m_hLock);
        m_hLock = INVALID_HANDLE_VALUE;
        DeleteFileW(lockPath.c_str());
        return false;
    }

    // Header (optional, NicoJK-compatible comment). 新規作成したときだけ書く。
    LARGE_INTEGER fileSize{};
    const bool sizeKnown = GetFileSizeEx(m_hFile, &fileSize) != FALSE;
    m_fileBytes = sizeKnown ? fileSize.QuadPart : 0;
    if (sizeKnown && m_fileBytes == 0) {
        time_t t = date;
        tm lt{};
        localtime_s(&lt, &t);
        char header[128];
        int len = sprintf_s(header, "<!-- NicoJK logfile from %04d-%02d-%02dT%02d:%02d:%02d -->\r\n",
                            lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
        DWORD written = 0;
        if (WriteFile(m_hFile, header, len, &written, nullptr)) m_fileBytes += written;
    }

    m_curJk = jkID;
    m_fileStart = date;
    m_filePath = filePath;
    return true;
}

void CommentLogWriter::Write(int jkID, time_t date, const std::string& rawChatLine)
{
    if (!m_enabled || jkID <= 0 || rawChatLine.empty()) return;

    // Split at the next distinct timestamp. NicoJK filenames have one-second
    // resolution, so comments from the same second can exceed the target size.
    ULONGLONG rotateBytes = m_limits.fileMiB * kMiB;
    const ULONGLONG totalBytes = m_limits.totalMiB * kMiB;
    if (totalBytes && (!rotateBytes || totalBytes < rotateBytes)) rotateBytes = totalBytes;
    const bool rotate = m_curJk == jkID && rotateBytes && date > m_fileStart &&
        m_fileBytes + rawChatLine.size() + 2 > rotateBytes;
    if (m_curJk != jkID || rotate) {
        Close();
        if (!Open(jkID, date)) return;
    }
    if (m_hFile == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    if (!WriteFile(m_hFile, rawChatLine.c_str(), static_cast<DWORD>(rawChatLine.size()), &written, nullptr)) return;
    m_fileBytes += written;
    if (WriteFile(m_hFile, "\r\n", 2, &written, nullptr)) m_fileBytes += written;

    if (m_limits.retentionDays || m_limits.totalMiB) {
        const ULONGLONG now = GetTickCount64();
        if (now >= m_nextCleanup || (rotate && m_limits.totalMiB)) {
            m_nextCleanup = now + 60000;
            PruneLogs();
        }
    }
}

void CommentLogWriter::PruneLogs()
{
    const DWORD attributes = GetFileAttributesW(m_folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;

    std::vector<LogFile> files;
    // Directory enumeration can return a stale size for our still-open file.
    ULONGLONG total = m_fileBytes;
    WIN32_FIND_DATAW channel{};
    HANDLE channels = FindFirstFileW((m_folder + L"\\jk*").c_str(), &channel);
    if (channels == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring_view name(channel.cFileName);
        if (!(channel.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (channel.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !name.starts_with(L"jk") || !IsDigits(name.substr(2))) continue;

        const std::wstring folder = m_folder + L"\\" + channel.cFileName + L"\\";
        WIN32_FIND_DATAW entry{};
        HANDLE entries = FindFirstFileW((folder + L"*.txt").c_str(), &entry);
        if (entries == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring_view filename(entry.cFileName);
            if (entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ||
                filename.size() != 14 || !filename.ends_with(L".txt") || !IsDigits(filename.substr(0, 10))) continue;
            const std::wstring path = folder + entry.cFileName;
            if (path == m_filePath) continue;
            const ULONGLONG bytes = JoinDwords(entry.nFileSizeHigh, entry.nFileSizeLow);
            const auto modified = JoinDwords(entry.ftLastWriteTime.dwHighDateTime, entry.ftLastWriteTime.dwLowDateTime);
            total += bytes;
            files.push_back({ path, bytes, modified });
        } while (FindNextFileW(entries, &entry));
        FindClose(entries);
    } while (FindNextFileW(channels, &channel));
    FindClose(channels);

    FILETIME systemTime{};
    GetSystemTimeAsFileTime(&systemTime);
    const auto now = JoinDwords(systemTime.dwHighDateTime, systemTime.dwLowDateTime);
    const auto maxBytes = m_limits.totalMiB * kMiB;
    std::sort(files.begin(), files.end(), [](const LogFile& a, const LogFile& b) {
        if (a.modified != b.modified) return a.modified < b.modified;
        return a.path < b.path;
    });
    for (const auto& file : files) {
        const bool expired = m_limits.retentionDays && now > file.modified &&
            (now - file.modified) / kTicksPerDay >= m_limits.retentionDays;
        if (!expired && (!maxBytes || total <= maxBytes)) break;
        if (TryDeleteLog(file)) total -= file.bytes;
    }
}
