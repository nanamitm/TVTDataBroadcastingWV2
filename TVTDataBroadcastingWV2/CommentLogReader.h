#pragma once
#include "pch.h"
#include "CommentFetcher.h"
#include <ctime>
#include <string>
#include <utility>
#include <vector>

// Reads NicoJK-compatible comment log files for synced playback.
//   {folder}\jk{ID}\{startUnixTime}.txt   raw <chat>...</chat> lines
//
// Stateful: Seek() to a broadcast time, then Read() the comments as the
// broadcast clock advances. Loads subsequent files when time crosses them.
class CommentLogReader
{
public:
    // Scan the jk{ID} folder. Returns true if at least one log file exists.
    bool Configure(const std::wstring& folder, int jkID);

    int  JkID() const { return m_jkID; }
    bool HasFiles() const { return !m_files.empty(); }

    // Reposition to broadcast time t (seek / jump).
    void Seek(time_t t);

    // Return buffered comments with date in (afterT, untilT], advancing position.
    std::vector<Comment> Read(time_t afterT, time_t untilT);

private:
    std::wstring m_folder;
    int m_jkID = -1;
    std::vector<std::pair<time_t, std::wstring>> m_files; // (startTime, fullpath), sorted
    std::vector<Comment> m_buf;     // parsed comments of loaded file(s), date-ordered
    size_t m_pos = 0;               // next index to consider in m_buf
    int    m_loadedFileIdx = -1;    // index of the last file loaded into m_buf

    int  FileIndexForTime(time_t t) const; // largest start <= t (or 0)
    void LoadFile(int idx, bool append);
};
