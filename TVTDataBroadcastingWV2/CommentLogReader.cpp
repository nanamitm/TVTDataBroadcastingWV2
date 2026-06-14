#include "pch.h"
#include "CommentLogReader.h"
#include "JkcnslReader.h"
#include <algorithm>

bool CommentLogReader::Configure(const std::wstring& folder, int jkID)
{
    m_folder = folder;
    m_jkID = jkID;
    m_files.clear();
    m_buf.clear();
    m_pos = 0;
    m_loadedFileIdx = -1;

    if (folder.empty() || jkID <= 0) return false;

    wchar_t dir[64];
    swprintf_s(dir, L"\\jk%d\\*.txt", jkID);
    std::wstring pattern = folder + dir;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        // file name like 0000000000.txt = start unix time
        long long start = _wtoll(fd.cFileName);
        if (start <= 0) continue;
        wchar_t sub[80];
        swprintf_s(sub, L"\\jk%d\\%s", jkID, fd.cFileName);
        m_files.emplace_back(static_cast<time_t>(start), folder + sub);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(m_files.begin(), m_files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return !m_files.empty();
}

int CommentLogReader::FileIndexForTime(time_t t) const
{
    if (m_files.empty()) return -1;
    int idx = 0;
    for (int i = 0; i < static_cast<int>(m_files.size()); ++i) {
        if (m_files[i].first <= t) idx = i; else break;
    }
    return idx;
}

void CommentLogReader::LoadFile(int idx, bool append)
{
    if (idx < 0 || idx >= static_cast<int>(m_files.size())) return;
    if (!append) { m_buf.clear(); m_pos = 0; }

    HANDLE h = CreateFileW(m_files[idx].second.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    m_loadedFileIdx = idx;
    if (h == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::string data;
    if (size.QuadPart > 0 && size.QuadPart < (64LL << 20)) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0, total = 0;
        while (total < data.size() &&
               ReadFile(h, &data[total], static_cast<DWORD>(data.size() - total), &read, nullptr) && read > 0) {
            total += read;
        }
        data.resize(total);
    }
    CloseHandle(h);

    // Parse each line (raw <chat> tag) by reusing ParseChatLine (needs a '-' prefix).
    size_t start = 0;
    while (start <= data.size()) {
        size_t nl = data.find('\n', start);
        std::string line = data.substr(start, (nl == std::string::npos ? data.size() : nl) - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '<' && line.compare(0, 5, "<chat") == 0) {
            Comment c;
            if (JkcnslReader::ParseChatLine("-" + line, c)) {
                m_buf.push_back(std::move(c));
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

void CommentLogReader::Seek(time_t t)
{
    m_buf.clear();
    m_pos = 0;
    int i = FileIndexForTime(t);
    if (i < 0) return;
    LoadFile(i, false);
    // Position at the first comment after t.
    m_pos = 0;
    while (m_pos < m_buf.size() && m_buf[m_pos].date <= t) ++m_pos;
}

std::vector<Comment> CommentLogReader::Read(time_t afterT, time_t untilT)
{
    std::vector<Comment> result;
    for (;;) {
        while (m_pos < m_buf.size() && m_buf[m_pos].date <= untilT) {
            if (m_buf[m_pos].date > afterT) result.push_back(m_buf[m_pos]);
            ++m_pos;
        }
        // Buffer exhausted: load the next file if the clock has reached it.
        if (m_pos >= m_buf.size() &&
            m_loadedFileIdx + 1 < static_cast<int>(m_files.size()) &&
            m_files[m_loadedFileIdx + 1].first <= untilT) {
            LoadFile(m_loadedFileIdx + 1, true);
            continue;
        }
        break;
    }
    return result;
}
