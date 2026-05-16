#include "pch.h"
#include "CommentFetcher.h"
#include <sstream>
#include <unordered_map>

// BS (NetworkID=4): ServiceID -> jikkyo channel
// Only channels supported by jkcnsl L command (jk1-jk9, jk101, jk211)
static const std::unordered_map<WORD, const char*> kBSChannels = {
    { 101, "jk101" }, // NHK BS1
    { 211, "jk211" }, // WOWOW Live
};

/*static*/ std::string CommentFetcher::DetectChannel(WORD networkId, WORD serviceId)
{
    if (networkId == 4) {
        auto it = kBSChannels.find(serviceId);
        if (it != kBSChannels.end()) return it->second;
    }
    return "";
}

/*static*/ void CommentFetcher::ParseMail(const std::string& mail,
    std::string& color, std::string& position, std::string& size)
{
    static const std::unordered_map<std::string, std::string> kColors = {
        {"white","white"}, {"red","red"}, {"blue","blue"}, {"yellow","yellow"},
        {"green","green"}, {"cyan","cyan"}, {"purple","purple"}, {"black","black"},
        {"niconicowhite","white"}, {"cadetblue","cadetblue"}, {"maroon","maroon"},
        {"fuchsia","fuchsia"}, {"lime","lime"}, {"olive","olive"}, {"navy","navy"},
        {"teal","teal"}, {"silver","silver"}, {"gray","gray"}, {"orange","orange"},
        {"midori","midori"},
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
        auto it = kColors.find(token);
        if (it != kColors.end()) color = it->second;
    }
}
