#include "pch.h"
#include "CommentFetcher.h"
#include "thirdparty/json.hpp"
#include <winhttp.h>
#include <sstream>
#include <unordered_map>

#pragma comment(lib, "winhttp.lib")

// ---- チャンネル検出テーブル ----

// BS (NetworkID = 4) ServiceID → 実況チャンネル
static const std::unordered_map<WORD, const char*> kBSChannels = {
    { 101, "jk191" }, // NHK BS1
    { 103, "jk193" }, // NHK BSプレミアム / NHK BS
    { 141, "jk141" }, // BS日テレ
    { 151, "jk151" }, // BS朝日
    { 161, "jk161" }, // BS-TBS
    { 171, "jk171" }, // BSテレ東
    { 181, "jk181" }, // BSフジ
    { 211, "jk211" }, // WOWOWライブ
    { 222, "jk222" }, // スターチャンネル1
    { 236, "jk236" }, // BSアニマックス
    { 252, "jk252" }, // BSスカパー!
    { 265, "jk265" }, // AT-X
    { 268, "jk268" }, // ディズニーチャンネル
    { 333, "jk333" }, // BS11
    { 341, "jk341" }, // TwellV
};

// 地上波：サービス名のキーワード → 実況チャンネル（関東系）
static const std::vector<std::pair<std::wstring, const char*>> kTerrestrialKeywords = {
    { L"NHK総合",       "jk1"  },
    { L"NHK-G",         "jk1"  },
    { L"NHK教育",       "jk2"  },
    { L"NHKEテレ",      "jk2"  },
    { L"Eテレ",         "jk2"  },
    { L"日本テレビ",    "jk4"  },
    { L"日テレ",        "jk4"  },
    { L"NTV",           "jk4"  },
    { L"テレビ朝日",    "jk5"  },
    { L"テレ朝",        "jk5"  },
    { L"EX",            "jk5"  },
    { L"TBSテレビ",     "jk6"  },
    { L"TBS",           "jk6"  },
    { L"テレビ東京",    "jk7"  },
    { L"テレ東",        "jk7"  },
    { L"TX",            "jk7"  },
    { L"フジテレビ",    "jk8"  },
    { L"CX",            "jk8"  },
    { L"TOKYO MX",      "jk9"  },
    { L"東京MX",        "jk9"  },
    { L"MBS",           "jk13" },
    { L"毎日放送",      "jk13" },
    { L"朝日放送",      "jk15" },
    { L"ABC",           "jk15" },
    { L"関西テレビ",    "jk17" },
    { L"読売テレビ",    "jk14" },
    { L"ytv",           "jk14" },
    { L"テレビ大阪",    "jk19" },
    { L"CBC",           "jk20" },
    { L"東海テレビ",    "jk21" },
    { L"メ～テレ",      "jk22" },
    { L"中京テレビ",    "jk23" },
    { L"テレビ愛知",    "jk24" },
};

/*static*/ std::string CommentFetcher::DetectChannel(WORD networkId, WORD serviceId, const std::wstring& serviceName)
{
    // BS
    if (networkId == 4) {
        auto it = kBSChannels.find(serviceId);
        if (it != kBSChannels.end()) return it->second;
        return "";
    }
    // 地上波：サービス名でキーワードマッチ
    for (auto& [kw, ch] : kTerrestrialKeywords) {
        if (serviceName.find(kw) != std::wstring::npos) return ch;
    }
    return "";
}

// ---- HTTP GET (HTTPS) ----

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
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD size = 0;
                do {
                    size = 0;
                    WinHttpQueryDataAvailable(hRequest, &size);
                    if (size == 0) break;
                    std::string buf(size, '\0');
                    DWORD read = 0;
                    WinHttpReadData(hRequest, buf.data(), size, &read);
                    result.append(buf, 0, read);
                } while (size > 0);
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return result;
}

// ---- mailフィールド解析 ----

/*static*/ void CommentFetcher::ParseMail(const std::string& mail,
    std::string& color, std::string& position, std::string& size)
{
    static const std::unordered_map<std::string, std::string> colorNames = {
        {"white","white"}, {"red","red"}, {"blue","blue"}, {"yellow","yellow"},
        {"green","green"}, {"cyan","cyan"}, {"purple","purple"}, {"black","black"},
        {"niconicowhite","white"}, {"cadetblue","cadetblue"}, {"maroon","maroon"},
        {"fuchsia","fuchsia"}, {"lime","lime"}, {"olive","olive"}, {"navy","navy"},
        {"teal","teal"}, {"silver","silver"}, {"gray","gray"}, {"orange","orange"},
        {"midori","midori"},
    };

    color = "white";
    position = "naka";
    size = "medium";

    std::istringstream ss(mail);
    std::string token;
    while (ss >> token) {
        if (token == "ue") { position = "ue"; continue; }
        if (token == "shita") { position = "shita"; continue; }
        if (token == "small") { size = "small"; continue; }
        if (token == "big") { size = "big"; continue; }
        auto it = colorNames.find(token);
        if (it != colorNames.end()) color = it->second;
    }
}

// ---- コメント取得・パース ----

std::vector<Comment> CommentFetcher::Fetch(const std::string& channel, time_t from, time_t to)
{
    std::wostringstream path;
    path << L"/api/kakolog/" << std::wstring(channel.begin(), channel.end())
         << L"?starttime=" << from << L"&endtime=" << to << L"&format=json";

    std::string json = HttpGet(L"jikkyo.tsukumijima.net", path.str());
    if (json.empty()) return {};

    std::vector<Comment> result;
    try {
        auto root = nlohmann::json::parse(json);
        auto& packet = root.at("packet");
        if (!packet.contains("chat")) return result;

        auto& chats = packet["chat"];
        // chat が配列でなく単一オブジェクトのこともある
        auto process = [&](const nlohmann::json& chat) {
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

// ---- フェッチループ ----

void CommentFetcher::FetchLoop()
{
    constexpr DWORD kIntervalMs = 60000; // 60秒ごとに取得

    while (m_running) {
        std::string channel;
        {
            std::lock_guard lock(m_mutex);
            channel = m_channel;
        }

        if (!channel.empty()) {
            time_t now = time(nullptr);
            time_t from = m_lastSent > 0 ? m_lastSent : now - 5;
            time_t to   = now + 5; // 少し先まで取得

            auto comments = Fetch(channel, from, to);

            if (!comments.empty()) {
                // 取得した最新コメントの日時を記録
                // （APIが日付順に返すと仮定して最後の要素を使う）
                // 実際はFetch内で@dateを記録するが、ここではシンプルにtoを使う
                m_lastSent = to - 5;
                if (m_callback) m_callback(std::move(comments));
            }
        }

        // 次回まで待機（StopでwakeEventがセットされると即時終了）
        WaitForSingleObject(m_wakeEvent, kIntervalMs);
    }
}

// ---- 公開メソッド ----

void CommentFetcher::SetChannel(const std::string& jikkyoChannel)
{
    std::lock_guard lock(m_mutex);
    if (m_channel != jikkyoChannel) {
        m_channel = jikkyoChannel;
        m_lastSent = 0; // チャンネル変更でリセット
        if (m_wakeEvent) SetEvent(m_wakeEvent); // 即座に取得開始
    }
}

void CommentFetcher::Start()
{
    if (m_running) return;
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_running = true;
    m_thread = std::thread([this] { FetchLoop(); });
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
