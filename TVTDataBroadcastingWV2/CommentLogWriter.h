#pragma once
#include "pch.h"
#include <ctime>
#include <string>

// Records received jikkyo comments to NicoJK-compatible log files:
//   {folder}\jk{ID}\{firstUnixTime}.txt   raw <chat ...>...</chat> lines (CRLF)
//   {folder}\jk{ID}\lockfile              present while recording (NicoJK convention)
//
// Use a folder separate from NicoJK's so the two plugins don't fight over the
// same files.
class CommentLogWriter
{
public:
    ~CommentLogWriter();

    // enabled=false or an empty folder disables recording.
    void Configure(const std::wstring& folder, bool enabled);

    // Append one comment (raw <chat> tag) for jkID. Rotates files on channel
    // change. date is the comment's Unix time (used for the file name).
    void Write(int jkID, time_t date, const std::string& rawChatLine);

    // Close the current file (called on channel change / disable).
    void Close();

private:
    std::wstring m_folder;
    bool   m_enabled = false;
    int    m_curJk   = -1;
    HANDLE m_hFile   = INVALID_HANDLE_VALUE;
    HANDLE m_hLock   = INVALID_HANDLE_VALUE;

    bool Open(int jkID, time_t date);
};
