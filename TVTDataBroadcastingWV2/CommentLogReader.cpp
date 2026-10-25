#include "pch.h"
#include "CommentLogReader.h"
#include "JkcnslReader.h"
#include <algorithm>
#include <unordered_set>

bool CommentLogReader::Configure(const std::wstring& folder, int jkID)
{
    m_folder = folder;
    m_jkID = jkID;
    m_files.clear();
    m_buf.clear();
    m_pos = 0;
    m_loadedFileIdx = -1;
    m_fileOffset = 0;
    m_partialLine.clear();
    m_lastScanTick = 0;

    if (folder.empty() || jkID <= 0) return false;
    RefreshFiles(true);
    return !m_files.empty();
}

void CommentLogReader::RefreshFiles(bool force)
{
    // ファイル一覧の走査は、追記を読み切ったときでも毎秒1回まで。
    const ULONGLONG now = GetTickCount64();
    if (!force && now - m_lastScanTick < 1000) return;
    m_lastScanTick = now;
    if (m_folder.empty() || m_jkID <= 0) return;
    const std::wstring loadedPath = m_loadedFileIdx >= 0 ? m_files[m_loadedFileIdx].second : L"";
    std::unordered_set<std::wstring> known;
    for (const auto& file : m_files) known.insert(file.second);
    const auto oldSize = m_files.size();

    wchar_t dir[64];
    swprintf_s(dir, L"\\jk%d\\", m_jkID);
    const std::wstring directory = m_folder + dir;
    const std::wstring pattern = directory + L"*.txt";

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        // file name like 0000000000.txt = start unix time
        long long start = _wtoll(fd.cFileName);
        if (start <= 0) continue;
        const std::wstring path = directory + fd.cFileName;
        if (known.insert(path).second)
            m_files.emplace_back(static_cast<time_t>(start), path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (m_files.size() != oldSize)
    {
        std::sort(m_files.begin(), m_files.end());
        if (!loadedPath.empty())
        {
            for (size_t i = 0; i < m_files.size(); ++i)
                if (m_files[i].second == loadedPath) { m_loadedFileIdx = static_cast<int>(i); break; }
        }
    }
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
    // 追記の前に消費済みのコメントを捨てる。長時間の再生で読み込んだファイルの
    // 中身がm_bufに際限なく積み上がるのを防ぐ。
    else if (m_pos > 0) { m_buf.erase(m_buf.begin(), m_buf.begin() + m_pos); m_pos = 0; }

    m_loadedFileIdx = idx;
    m_fileOffset = 0;
    m_partialLine.clear();
    ReadMore();
}

bool CommentLogReader::ReadMore()
{
    if (m_loadedFileIdx < 0) return false;

    HANDLE h = CreateFileW(m_files[m_loadedFileIdx].second.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return false; }
    if (size.QuadPart < m_fileOffset)
    {
        m_fileOffset = 0;
        m_partialLine.clear();
    }
    LARGE_INTEGER offset{};
    offset.QuadPart = m_fileOffset;
    if (!SetFilePointerEx(h, offset, nullptr, FILE_BEGIN)) { CloseHandle(h); return false; }
    // 全ファイルの再読み込みを避け、追記分を64KiBずつ読み進める。
    char data[64 * 1024];
    DWORD read = 0;
    const bool ok = ReadFile(h, data, sizeof(data), &read, nullptr) != FALSE;
    CloseHandle(h);
    if (!ok || read == 0) return false;
    m_fileOffset += read;
    if (m_pos > 0) { m_buf.erase(m_buf.begin(), m_buf.begin() + m_pos); m_pos = 0; }
    m_partialLine.append(data, read);

    // Parse each line (raw <chat> tag) by reusing ParseChatLine (needs a '-' prefix).
    size_t start = 0;
    for (;;) {
        const size_t nl = m_partialLine.find('\n', start);
        if (nl == std::string::npos) break;
        std::string line = m_partialLine.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '<' && line.compare(0, 5, "<chat") == 0) {
            Comment c;
            if (JkcnslReader::ParseChatLine("-" + line, c)) {
                m_buf.push_back(std::move(c));
            }
        }
        start = nl + 1;
    }
    m_partialLine.erase(0, start);
    // 改行なしで終わる既存ログも扱う。タグの途中なら次の追記まで保持する。
    Comment trailing;
    if (JkcnslReader::ParseChatLine("-" + m_partialLine, trailing))
    {
        m_buf.push_back(std::move(trailing));
        m_partialLine.clear();
    }
    // 壊れたログに改行がなくても、保留中の行を際限なく貯めない。
    if (m_partialLine.size() > 1024 * 1024) m_partialLine.clear();
    return true;
}

void CommentLogReader::Seek(time_t t)
{
    m_buf.clear();
    m_pos = 0;
    RefreshFiles(true);
    int i = FileIndexForTime(t);
    if (i < 0) return;
    LoadFile(i, false);
    // Position at the first comment after t.
    m_pos = 0;
    for (;;) {
        while (m_pos < m_buf.size() && m_buf[m_pos].date <= t) ++m_pos;
        if (m_pos < m_buf.size() || !ReadMore()) break;
    }
}

std::vector<Comment> CommentLogReader::Read(time_t afterT, time_t untilT)
{
    std::vector<Comment> result;
    for (;;) {
        while (m_pos < m_buf.size() && m_buf[m_pos].date <= untilT) {
            if (m_buf[m_pos].date > afterT) result.push_back(m_buf[m_pos]);
            ++m_pos;
        }
        if (m_pos < m_buf.size()) break;
        if (ReadMore()) continue;
        RefreshFiles();
        // Configure時にファイルがなかった場合も、記録開始を後から検出する。
        if (m_loadedFileIdx < 0 && !m_files.empty()) {
            LoadFile(FileIndexForTime(afterT), false);
            continue;
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
