#pragma once
#include "pch.h"
#include <ctime>
#include <string>
#include <vector>

struct Comment {
    std::string text;
    std::string color;    // CSS color name
    std::string position; // "naka" / "ue" / "shita"
    std::string size;     // "small" / "medium" / "big"
    std::string userId;   // chat user_id (for user NG)
    std::string mail;     // raw mail field (for command NG, e.g. "184")
    bool refuge = false;  // from a refuge source (x_refuge / nx_jikkyo)
    time_t date = 0;      // Unix timestamp
};

// Utility: jikkyo channel detection and comment mail-field parsing
class CommentFetcher
{
public:
    // Returns jikkyo channel string (e.g. "jk101") from BS ServiceID.
    // Terrestrial channels must be set via JikkyoChannel= in the INI.
    // Returns empty string when the channel is not supported.
    static std::string DetectChannel(WORD networkId, WORD serviceId);

    // Parse jikkyo mail field into color / position / size.
    static void ParseMail(const std::string& mail,
                          std::string& color,
                          std::string& position,
                          std::string& size);
};
