#include "pch.h"
#include "CommentFetcher.h"
#include "thirdparty/json.hpp"
#include <winhttp.h>
#include <sstream>
#include <unordered_map>

#pragma comment(lib, "winhttp.lib")

// BS (NetworkID=4): ServiceID -> jikkyo channel string
static const std::unordered_map<WORD, const char*> kBSChannels = {
    { 101, "jk191" }, // NHK BS1
    { 103, "jk193" }, // NHK BS Premium
    { 141, "jk141" }, // BS Nittele
    { 151, "jk151" }, // BS Asahi
    { 161, "jk161" }, // BS-TBS
    { 171, "jk171" }, // BS TV Tokyo
    { 181, "jk181" }, // BS Fuji
    { 211, "jk211" }, // WOWOW Live
    { 222, "jk222" }, // Star Channel 1
    { 236, "jk236" }, // BS Animax
    { 252, "jk252" }, // BS SKY Perfect
    { 265, "jk265" }, // AT-X
    { 268, "jk268" }, // Disney Channel
    { 333, "jk333" }, // BS11
    { 341, "jk341" }, // TwellV
};

// Terrestrial: ASCII prefix in service name -> jikkyo channel
// Used only when the service name contains an ASCII-identifiable substring.
// For ambiguous channels, set JikkyoChannel= in the INI file directly.
static const std::vector<std::pair<const wchar_t*, const char*>> kASCIIKeywords = {
    { L"NTV",       "jk4"  },
    { L"TBS",       "jk6"  },
    { L"TX ",       "jk7"  }, // TV Tokyo (space avoids TX-prefix false match)
    { L"CX",        "jk8"  },
    { L"TOKYO MX",  "jk9"  },
    { L"MBS",       "jk13" },
    { L"ABC",       "jk15" },
    { L"ytv",       "jk14" },
    { L"CBC",       "jk20" },
};

// --- HTTP GET (HTTPS via WinHTTP) ---

/*static*/ std::string CommentFetcher::HttpGet(const std::wstring& host, const std::wstring& path)
{
    std::string result;
    HINTERNET hSession = WinHttpOpen(L"TVTDataBroadcastingWV2/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (hRequest) {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD avail = 0;
                do {
                    avail = 0;
                    WinHttpQueryDataAvailable(hRequest, &avail);
                    if (avail == 0) break;
                    std::string buf(avail, '\0');
                    DWORD read = 0;
                    WinHttpReadData(hRequest, buf.data(), avail, &read);
                    result.append(buf, 0, read);
                } while (avail > 0);
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return result;
}

// --- Parse jikkyo mail field ---

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

// --- Fetch from jikkyo.tsukumijima.net ---

std::vector<Comment> CommentFetcher::Fetch(const std::string& channel, time_t from, time_t to)
{
    std::wostringstream path;
    path << L"/api/kakolog/"
         << std::wstring(channel.begin(), channel.end())
         << L"?starttime=" << from << L"&endtime=" << to << L"&format=json";

    std::string json = HttpGet(L"jikkyo.tsukumijima.net", path.str());
    if (json.empty()) return {};

    std::vector<Comment> result;
    try {
        auto root   = nlohmann::json::parse(json);
        auto& packet = root.at("packet");
        if (!packet.contains("chat")) return result;

        auto& chats = packet["chat"];
        auto process = [this, &result](const nlohmann::json& chat) {
            if (!chat.contains("#text")) return;
            time_t date = std::stoll(chat.value("@date", "0"));
            if (date <= m_lastSent) return;

            Comment c;
            c.text = chat["#text"].get<std::string>();
            if (c.text.empty()) return;
            ParseMail(chat.value("@mail", ""), c.color, c.position, c.size);
            result.push_back(std::move(c));
        };

        if (chats.is_array()) {
            for (auto& chat : chats) process(chat);
        } else if (chats.is_object()) {
            process(chats);
        }
    } catch (...) {}

    return result;
}

// --- Background fetch loop ---

void CommentFetcher::FetchLoop()
{
    constexpr DWORD kIntervalMs = 60000;

    while (m_running) {
        std::string channel;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            channel = m_channel;
        }

        if (!channel.empty()) {
            time_t now  = time(nullptr);
            time_t from = m_lastSent > 0 ? m_lastSent : now - 5;
            time_t to   = now + 5;

            auto comments = Fetch(channel, from, to);
            if (!comments.empty()) {
                m_lastSent = to - 5;
                if (m_callback) m_callback(std::move(comments));
            }
        }

        WaitForSingleObject(m_wakeEvent, kIntervalMs);
    }
}

// --- Public interface ---

/*static*/ std::string CommentFetcher::DetectChannel(
    WORD networkId, WORD serviceId, const std::wstring& serviceName)
{
    // BS: identified by standardised ServiceID
    if (networkId == 4) {
        auto it = kBSChannels.find(serviceId);
        if (it != kBSChannels.end()) return it->second;
        return "";
    }
    // Terrestrial: match ASCII substrings in service name
    for (auto& kv : kASCIIKeywords) {
        if (serviceName.find(kv.first) != std::wstring::npos) return kv.second;
    }
    return "";
}

void CommentFetcher::SetChannel(const std::string& jikkyoChannel)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_channel != jikkyoChannel) {
        m_channel  = jikkyoChannel;
        m_lastSent = 0;
        if (m_wakeEvent) SetEvent(m_wakeEvent);
    }
}

void CommentFetcher::Start()
{
    if (m_running) return;
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_running   = true;
    m_thread    = std::thread([this] { FetchLoop(); });
}

void CommentFetcher::Stop()
{
    if (!m_running) return;
    m_running = false;
    if (m_wakeEvent) SetEvent(m_wakeEvent);
    if (m_thread.joinable()) m_thread.join();
    if (m_wakeEvent) { CloseHandle(m_wakeEvent); m_wakeEvent = nullptr; }
    m_lastSent = 0;
}

CommentFetcher::~CommentFetcher()
{
    Stop();
}
