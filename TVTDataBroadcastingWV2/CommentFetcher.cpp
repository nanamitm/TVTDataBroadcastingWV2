#include "pch.h"
#include "CommentFetcher.h"
#include "NetworkServiceIDTable.h"
#include <sstream>
#include <unordered_set>

/*static*/ std::string CommentFetcher::DetectChannel(WORD networkId, WORD serviceId)
{
    // ntsID format: (serviceId << 16) | networkCategory
    // BS(NetworkID==4) -> 0x0004, terrestrial -> 0x000F
    WORD  netCat = (networkId == 4) ? 0x0004 : 0x000F;
    DWORD ntsID  = ((DWORD)serviceId << 16) | netCat;

    // Binary search (table is sorted by ntsID)
    int lo = 0, hi = (int)std::size(DEFAULT_NTSID_TABLE) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (DEFAULT_NTSID_TABLE[mid].ntsID == ntsID) {
            int jkID = DEFAULT_NTSID_TABLE[mid].jkID & ~0x40000000; // strip JKID_PRIOR
            if (jkID <= 0) return "";
            char buf[16];
            sprintf_s(buf, "jk%d", jkID);
            return buf;
        } else if (DEFAULT_NTSID_TABLE[mid].ntsID < ntsID) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return "";
}

/*static*/ void CommentFetcher::ParseMail(const std::string& mail,
    std::string& color, std::string& position, std::string& size)
{
    static const std::unordered_set<std::string> kColors = {
        "white", "red", "blue", "yellow", "green", "cyan", "purple", "black",
        "niconicowhite", "cadetblue", "maroon", "fuchsia", "lime", "olive",
        "navy", "teal", "silver", "gray", "orange", "midori",
    };

    color    = "white";
    position = "naka";
    size     = "medium";

    std::istringstream ss(mail);
    std::string token;
    while (ss >> token) {
        if (token == "ue")    { position = "ue";    continue; }
        if (token == "shita") { position = "shita"; continue; }
        if (token == "small") { size = "small";     continue; }
        if (token == "big")   { size = "big";       continue; }
        if (kColors.count(token)) color = token;
    }
}
