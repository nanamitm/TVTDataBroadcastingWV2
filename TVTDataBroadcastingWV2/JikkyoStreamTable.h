#pragma once
#include "pch.h"
#include <map>
#include <string>

// Per-jikkyo-channel stream IDs, mirroring NicoJK's forceList chatStreamID model.
//   chatStreamID        nicovideo stream id (ch/co/lv); empty if none
//   refugeChatStreamID  refuge stream id / flag; empty if none
struct JikkyoStream {
    std::string chatStreamID;
    std::string refugeChatStreamID;
};

// Resolves jkID -> JikkyoStream from a built-in table merged with the INI
// [ChatStreams] section (NicoJK-compatible format).
class JikkyoStreamTable
{
public:
    // (Re)load: built-in table first, then [ChatStreams] overrides/additions.
    void Load(const std::wstring& iniPath);

    // Returns true and fills out if jkID has a usable entry.
    bool Resolve(int jkID, JikkyoStream& out) const;

private:
    std::map<int, JikkyoStream> m_map;

    void MergeChatStreams(const std::wstring& iniPath);
    static std::string WideToUtf8(const std::wstring& w);
};
