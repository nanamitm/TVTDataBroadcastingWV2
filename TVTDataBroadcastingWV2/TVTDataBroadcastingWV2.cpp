#include "pch.h"

#define TVTEST_PLUGIN_CLASS_IMPLEMENT
#include "thirdparty/TVTestPlugin.h"
#include "resource.h"
#include <queue>
#include <optional>
#include <wil/stl.h>
#include <wil/win32_helpers.h>
#include "NVRAMSettingsDialog.h"
#include "proxy.h"
#include "InputDialog.h"
#include "OneSeg.h"
#include "CommentFetcher.h"
#include "CommentNG.h"
#include "NetworkServiceIDTable.h"
#include "JkcnslReader.h"
#include "JkcnslLogin.h"
#include "JkcnslSettings.h"
#include "JikkyoStreamTable.h"
#include "CommentLogWriter.h"
#include <shellapi.h>

using namespace Microsoft::WRL;

// #pragma comment(lib, "ntdll.lib")
// extern "C" ULONG DbgPrint(PCSTR Format, ...);

// メッセージウィンドウ向けメッセージ
#define WM_APP_PACKET (WM_APP + 0)
#define WM_APP_RESIZE (WM_APP + 1)
#define WM_APP_RESPONSE (WM_APP + 2)
#define WM_APP_INPUT (WM_APP + 3)
#define WM_APP_ENABLE_PLUGIN (WM_APP + 4)
#define WM_APP_COMMENTS (WM_APP + 5)
#define WM_APP_LOGIN (WM_APP + 6)
#define WM_APP_AUTH (WM_APP + 7)
#define WM_APP_CONN (WM_APP + 8)

// サイドパネル向けメッセージ
#define WM_APP_ON_PANEL_COLOR_CHANGE (WM_APP + 0)
#define WM_APP_ON_PANEL_SIZE (WM_APP + 1)

struct DeferralResponse
{
    wil::com_ptr<ICoreWebView2Deferral> deferral;
    wil::com_ptr<ICoreWebView2WebResourceRequestedEventArgs> args;
    DWORD statusCode;
    std::wstring statusCodeText;
    std::wstring headers;
    std::vector<BYTE> content;
};

#define IDT_SHOW_EVR_WINDOW 1
#define IDT_RESIZE 2

struct UsedKey
{
    bool basic;
    bool dataButton;
    bool numericTuning;
    bool special1;
    bool special2;
};

struct Status
{
    std::wstring url;
    bool receiving = false;
    bool loading = false;
};

struct PacketQueue
{
private:
    std::mutex queueMutex;
    std::vector<BYTE> currentBlock;
    std::queue<std::vector<BYTE>> queue;
    std::atomic<bool> invalidate;
    std::unordered_set<WORD> pidsToExclude;
    std::unordered_map<WORD, int> pcrPIDCandidates;
    int pcrPID = -1;
    DWORD pcr = 0;
    DWORD lastBlockPCR = 0;
public:
    static constexpr size_t packetSize = 188;
    static constexpr size_t packetBlockSize = packetSize * 500;
    static constexpr size_t maxQueueLength = 100;

    PacketQueue()
    {
        currentBlock.reserve(packetBlockSize);
    }

    // TSスレッドでのみ呼び出せる
    bool enqueuePacket(BYTE* packet)
    {
        if (this->invalidate.exchange(false))
        {
            this->currentBlock.clear();
        }
        // 8-bit sync byte
        // 1-bit TEI
        // 1-bit PUSI
        // 1-bit priority
        // 13-bit PID
        bool transportErrorIndicator = !!(packet[1] & 0x80);
        WORD pid = ((packet[1] << 8) | packet[2]) & 0x1fff;
        int adaptationFieldControl = (packet[3] >> 4) & 0x03;
        bool pcrFlag = false;
        if (!transportErrorIndicator && !!(adaptationFieldControl & 2))
        {
            int adaptationLength = packet[4];
            pcrFlag = !!(packet[5] & 0x10);
            if (adaptationLength >= 6 && pcrFlag)
            {
                // 参照するPCRのPIDを適当に選ぶ
                if (pid != this->pcrPID)
                {
                    auto it = this->pcrPIDCandidates.find(pid);
                    if (it == this->pcrPIDCandidates.end())
                    {
                        it = this->pcrPIDCandidates.emplace(pid, 0).first;
                    }
                    // 最初に3回出現するか、参照済みPCRが現れずに5回出現したPCRを使う
                    it->second++;
                    if ((this->pcrPID < 0 && it->second >= 3) || it->second >= 5)
                    {
                        this->pcrPID = pid;
                    }
                }
                if (pid == this->pcrPID)
                {
                    // PCRを取得する。時計演算の便利のため下位1bitは捨てる
                    this->pcrPIDCandidates.clear();
                    this->pcr = (static_cast<DWORD>(packet[6]) << 24) | (packet[7] << 16) | (packet[8] << 8) | packet[9];
                }
            }
        }

        bool acceptPacket;
        {
            // std::mutexの内部はSRWLock等なので頻繁に呼んでも別に気にしなくてよい
            std::lock_guard<std::mutex> lock(this->queueMutex);
            acceptPacket = this->pidsToExclude.count(pid) == 0;
        }
        if (acceptPacket)
        {
            this->currentBlock.insert(this->currentBlock.end(), packet, packet + this->packetSize);
        }
        else if (pcrFlag)
        {
            // 除外されているPIDにPCRが含まれていればPCRのみをキューに加える
            BYTE pcr_packet[packetSize] = {};
            pcr_packet[0] = 0x47;
            // adaptation_field_control=0b10なのでCIは加算されない、0固定で間に合わせる
            pcr_packet[1] = pid >> 8;
            pcr_packet[2] = pid & 0xff;
            pcr_packet[3] = 2 << 4;
            pcr_packet[4] = packetSize - 5; // adaptation_field_length
            pcr_packet[5] = 0x10; // PCR
            memcpy(pcr_packet + 6, packet + 6, 6);
            memset(pcr_packet + 12, 0xff, sizeof(pcr_packet) - 12);
            this->currentBlock.insert(this->currentBlock.end(), pcr_packet, pcr_packet + sizeof(pcr_packet));
        }

        // PCRが100ミリ秒以上進めばキューに加える
        // キューには100*maxQueueLengthミリ秒分ほど貯められる
        if (this->currentBlock.size() >= this->packetBlockSize ||
            (!this->currentBlock.empty() && (this->pcr - this->lastBlockPCR) >= 45 * 100))
        {
            {
                std::lock_guard<std::mutex> lock(this->queueMutex);
                this->queue.push(std::move(this->currentBlock));
                if (this->queue.size() > this->maxQueueLength)
                {
                    // 古いものを中身再利用して捨てる
                    this->currentBlock.swap(this->queue.front());
                    this->queue.pop();
                }
            }
            this->currentBlock.clear();
            this->currentBlock.reserve(this->packetBlockSize);
            this->lastBlockPCR = this->pcr;
            return true;
        }
        return false;
    }

    // どのスレッドからも呼び出せる
    void clear()
    {
        std::lock_guard<std::mutex> lock(this->queueMutex);
        std::queue<std::vector<BYTE>>().swap(this->queue);
        this->invalidate = true;
    }

    // どのスレッドからも呼び出せる
    std::optional<std::vector<BYTE>> pop()
    {
        std::lock_guard<std::mutex> lock(this->queueMutex);
        if (this->queue.empty())
        {
            return std::nullopt;
        }
        auto r = std::move(this->queue.front());
        this->queue.pop();
        return r;
    }

    // どのスレッドからも呼び出せる
    void setPIDsToExclude(std::unordered_set<WORD> pids)
    {
        std::lock_guard<std::mutex> lock(this->queueMutex);
        this->pidsToExclude.swap(pids);
    }
};

struct Audio
{
    std::optional<BYTE> componentId;
    std::optional<int> index;
    std::optional<TVTest::DualMonoChannel> dualMonoChannel;

    Audio()
    {
    }

    Audio(BYTE componentId, TVTest::DualMonoChannel dualMonoChannel)
    {
        this->componentId = componentId;
        if (dualMonoChannel != TVTest::DUAL_MONO_CHANNEL_INVALID)
        {
            this->dualMonoChannel = dualMonoChannel;
        }
    }

    Audio(int index)
    {
        if (index != -1)
        {
            this->index = index;
        }
    }

    std::optional<int> getChannelId()
    {
        if (this->dualMonoChannel.has_value())
        {
            switch (this->dualMonoChannel.value())
            {
                // 第一チャンネル
            case TVTest::DUAL_MONO_CHANNEL_MAIN:
                return 1;
                // 第二チャンネル
            case TVTest::DUAL_MONO_CHANNEL_SUB:
                return 2;
                // 両方
            case TVTest::DUAL_MONO_CHANNEL_BOTH:
                return 3;
            }
        }
        return std::nullopt;
    }

    void setChannelId(int channelId)
    {
        switch (channelId)
        {
            // 第一チャンネル
        case 1:
            this->dualMonoChannel = TVTest::DUAL_MONO_CHANNEL_MAIN;
            break;
            // 第二チャンネル
        case 2:
            this->dualMonoChannel = TVTest::DUAL_MONO_CHANNEL_SUB;
            break;
            // 両方
        case 3:
            this->dualMonoChannel = TVTest::DUAL_MONO_CHANNEL_BOTH;
            break;
        default:
            this->dualMonoChannel = std::nullopt;
            break;
        }
    }
};

// Plugins/TVTDataBroadcastingWV2.tvtp
class CDataBroadcastingWV2 : public TVTest::CTVTestPlugin, TVTest::CTVTestEventHandler
{
    // Plugins/TVTDataBroadcastingWV2.ini
    std::wstring iniFile;
    // Plugins/TVTDataBroadcastingWV2/
    std::wstring baseDirectory;
    // Plugins/TVTDataBroadcastingWV2/resources/
    std::wstring resourceDirectory;
    // Plugins/TVTDataBroadcastingWV2/WebView2Data/
    std::wstring webView2DataDirectory;
    // Plugins/TVTDataBroadcastingWV2/WebView2/
    std::wstring webView2Directory;

    std::atomic<bool> webViewLoaded;
    PacketQueue packetQueue;
    std::vector<WCHAR> packetsToJsonBuf;

    HWND hRemoteWnd = nullptr;
    HWND hPanelWnd = nullptr;
    HWND hVideoWnd = nullptr;
    HWND hWebViewWnd = nullptr;
    HWND hViewWnd = nullptr;
    HWND hContainerWnd = nullptr;
    HWND hMessageWnd = nullptr;
    HWND hOneSegWnd = nullptr;
    HBRUSH   hbrPanelBack   = nullptr;
    COLORREF panelBackColor = RGB(0xF0, 0xF0, 0xF0);
    COLORREF panelTextColor = RGB(0x00, 0x00, 0x00);
    HBRUSH hbrBRGYBacks[4] = {};
    HFONT hPanelFont = nullptr;
    UINT panelInitialDpi;
    std::vector<std::pair<HWND, RECT>> panelItems;
    // 勢いパネル
    struct MomentumChannel {
        int id;
        std::string name;
        std::string video;
        int force;
        std::string programTitle;
    };
    std::vector<MomentumChannel> momentumChannels;
    HWND hMomentumPanel = nullptr;
    wil::com_ptr<ICoreWebView2Environment>  webViewEnv;
    wil::com_ptr<ICoreWebView2Controller>   momentumWebViewController;
    wil::com_ptr<ICoreWebView2>             momentumWebView;
    bool momentumWebViewReady   = false;
    bool momentumWebViewPending = false;
    wil::com_ptr<IBasicVideo> basicVideo;
    wil::com_ptr<IBaseFilter> vmr7Renderer;
    wil::com_ptr<IBaseFilter> vmr9Renderer;
    bool invisible = false;
    RECT videoRect = {};
    RECT containerRect = {};
    TVTest::ServiceInfo currentService = {};
    bool currentServiceIsOneSeg = false;
    TVTest::ChannelInfo currentChannel = {};
    Status status;
    bool deferWebView = false;
    const int MAX_VOLUME = 100;
    int currentVolume = MAX_VOLUME;
    bool useTVTestVolume = true;
    bool useTVTestChannelCommand = true;
    UsedKey usedKey;
    // iniのEnableNetworkが1であればProxySessionが初期化されenableNetworkもtrueになる
    // ただしenableNetworkの方は実行時にボタンで切り替えられる
    std::unique_ptr<ProxySession> proxySession;
    bool enableNetwork = true;
    std::unique_ptr<InputDialog> inputDialog;
    Audio mainAudio;
    bool isPlayingMainAudio = true;
    std::unique_ptr<OneSegWindow> oneSegWindow;
    bool oneSegWindowIsShown;
    virtual bool OnChannelChange();
    virtual bool OnServiceChange();
    virtual bool OnServiceUpdate();
    virtual bool OnCommand(int ID);
    virtual bool OnPluginEnable(bool fEnable);
    virtual void OnFilterGraphInitialized(TVTest::FilterGraphInfo* pInfo);
    virtual void OnFilterGraphFinalize(TVTest::FilterGraphInfo* pInfo);
    virtual bool OnStatusItemDraw(TVTest::StatusItemDrawInfo* pInfo);
    virtual bool OnFullscreenChange(bool fFullscreen);
    virtual bool OnPluginSettings(HWND hwndOwner);
    virtual bool OnColorChange();
    virtual bool OnPanelItemNotify(TVTest::PanelItemEventInfo* pInfo);
    virtual bool OnVolumeChange(int Volume, bool fMute);
    virtual bool OnAudioStreamChange(int Stream);
    virtual bool OnStereoModeChange(int StereoMode);
    virtual void OnAudioFormatChange();
    virtual void OnDarkModeChanged(bool fDarkMode);

    HWND GetFullscreenWindow();
    void RestoreVideoWindow();
    void ResizeVideoWindow();
    void Tune();
    void InitWebView2();
    bool caption = false;
    bool restoreCaptionState = false;
    void SetCaptionState(bool enable);
    JkcnslReader   m_jkcnslReader;
    JkcnslLogin    m_jkcnslLogin;
    CommentNG      m_commentNg;
    JikkyoStreamTable m_chTable;
    CommentLogWriter  m_logWriter;
    int            m_currentJkID = -1; // jkID of the current channel (for logging)
    bool           m_mixing = false; // RefugeMixing active for the current stream
    DWORD          m_lastPostTick = 0;
    std::wstring   m_lastPostComm;
    std::wstring GetJkcnslPath() const;
    void OnLoginEvent(JkcnslLogin::Event ev, const std::string& message);
    void RefreshAuthState();   // query jkcnsl login state on a worker thread
    void PushAuthState();      // push the cached auth state to the momentum panel
    void PushNgUsers();        // push the NG user list to the momentum panel
    bool m_loggedIn = false;
    bool m_streamConnected = false;
    bool m_postTargetRefuge = false; // jkcnsl cache_server_url set => posting to refuge
    std::wstring m_loginMail;
    std::string DetectJkChannel() const;
    std::string DetectJkChannelFor(WORD networkId, WORD serviceId) const;
    void SwitchToMomentumChannel(int index);
    void SwitchToMomentumChannelById(int id);
    void SendComments(std::vector<Comment> comments);
    void PostComment(const std::wstring& input);
    void UpdateCommentChannel();
    void UpdateCaptionState(bool showIndicator);
    void UpdateVolume();
    std::wstring GetIniItem(const wchar_t* key, const wchar_t* def);
    INT GetIniItem(const wchar_t* key, INT def);
    bool SetIniItem(const wchar_t* key, const wchar_t* data);
    void Disable(bool finalize);
    void EnablePanelButtons(bool enable);
    HRESULT Proxy(ICoreWebView2WebResourceRequestedEventArgs* args, LPCWSTR proxyUrl);
    void UpdateCommentToggle();
    void UpdateNetworkToggleButton(HWND hWnd);
    void SetNetworkState(bool enable);
    void UpdateNetworkState();
    void UpdateAudioStream();
    void RestoreMainAudio();
    void SelectAudio(Audio audio);
    Audio GetSelectedAudio();
    void ShowRemoteControlDialog();
    void CreateMessageWindow();
    void CreateOneSegWindow();
    void DestroyOneSegWindow();
    void UpdateDigitButton();
    void SetPanelFont();

    wil::com_ptr<ICoreWebView2Controller> webViewController;
    wil::com_ptr<ICoreWebView2> webView;

    static LRESULT CALLBACK EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void* pClientData);
    static INT_PTR CALLBACK RemoteControlDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData);
    static INT_PTR CALLBACK PanelRemoteControlDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData);
    static INT_PTR CALLBACK PanelMomentumDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData);
    void InitMomentumWebView(HWND hwnd);
    void CreateMomentumWebViewController(HWND hwnd);
    void SendMomentumTheme();
    void SendMomentumChannels();
    static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData);
    static BOOL CALLBACK StreamCallback(BYTE* pData, void* pClientData);
    static LRESULT CALLBACK MessageWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static BOOL CALLBACK WindowMessageCallback(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* pResult, void* pUserData);

public:
    virtual bool GetPluginInfo(TVTest::PluginInfo* pInfo);
    virtual bool Initialize();
    virtual bool Finalize();
};

std::string wstrToUTF8String(const wchar_t* ws)
{
    auto size = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &result[0], size, nullptr, nullptr);
    result.resize(size - 1);
    return result;
}

std::wstring utf8StrToWString(const char* s)
{
    auto size = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &result[0], size);
    result.resize(size - 1);
    return result;
}

bool CDataBroadcastingWV2::GetPluginInfo(TVTest::PluginInfo* pInfo)
{
    pInfo->Type = TVTest::PLUGIN_TYPE_NORMAL;
    pInfo->Flags = TVTest::PLUGIN_FLAG_DISABLEONSTART | TVTest::PLUGIN_FLAG_HASSETTINGS;
    pInfo->pszPluginName = L"TVTDataBroadcastingWV2";
    pInfo->pszCopyright = L"2022-2023 otya";
#ifdef TVTDATABROADCASTINGWV2_VERSION
    pInfo->pszDescription = L"データ放送を表示" TVTDATABROADCASTINGWV2_VERSION;
#else
    pInfo->pszDescription = L"データ放送を表示";
#endif
    return true;
}

BOOL CALLBACK CDataBroadcastingWV2::StreamCallback(BYTE* pData, void* pClientData)
{
    auto pThis = (CDataBroadcastingWV2*)pClientData;
    if (pThis->packetQueue.enqueuePacket(pData))
    {
        PostMessageW(pThis->hMessageWnd, WM_APP_PACKET, 0, 0);
    }
    return TRUE;
}

bool CDataBroadcastingWV2::OnServiceChange()
{
    return OnServiceUpdate();
}

bool CDataBroadcastingWV2::OnChannelChange()
{
    return OnServiceUpdate();
}

bool CDataBroadcastingWV2::OnServiceUpdate()
{
    int numServices;
    auto serviceIndex = this->m_pApp->GetService(&numServices);
    if (serviceIndex == -1)
    {
        return true;
    }
    auto lastNetworkID = this->currentChannel.NetworkID;
    auto lastServiceID = this->currentService.ServiceID;
    this->m_pApp->GetCurrentChannelInfo(&this->currentChannel);
    // 過去のTVTestには音声ストリームが4より多く含まれていた場合範囲外アクセスが発生する問題があったので適当なパディングを入れる
    struct
    {
        TVTest::ServiceInfo serviceInfo;
        WORD padding[128];
    } currentService = {};
    this->m_pApp->GetServiceInfo(serviceIndex, &currentService.serviceInfo);
    this->currentService = currentService.serviceInfo;
    std::unordered_set<WORD> pesPIDList;
    for (auto i = 0; i < numServices; i++)
    {
        struct
        {
            TVTest::ServiceInfo serviceInfo;
            WORD padding[128];
        } serviceInfo = {};
        if (this->m_pApp->GetServiceInfo(i, &serviceInfo.serviceInfo))
        {
            TVTest::ElementaryStreamInfoList videoESList = {};
            TVTest::ElementaryStreamInfoList audioESList = {};
            if (this->m_pApp->GetElementaryStreamInfoList(&videoESList, TVTest::ES_MEDIA_VIDEO, serviceInfo.serviceInfo.ServiceID))
            {
                for (auto i = 0; i < videoESList.ESCount; i++)
                {
                    pesPIDList.insert(videoESList.ESList[i].PID);
                }
                this->m_pApp->MemoryFree(videoESList.ESList);
            }
            if (this->m_pApp->GetElementaryStreamInfoList(&audioESList, TVTest::ES_MEDIA_AUDIO, serviceInfo.serviceInfo.ServiceID))
            {
                for (auto i = 0; i < audioESList.ESCount; i++)
                {
                    pesPIDList.insert(audioESList.ESList[i].PID);
                }
                this->m_pApp->MemoryFree(audioESList.ESList);
            }
            pesPIDList.insert(serviceInfo.serviceInfo.VideoPID);
            for (auto j = 0; j < serviceInfo.serviceInfo.NumAudioPIDs && j < _countof(serviceInfo.serviceInfo.AudioPID); j++)
            {
                pesPIDList.insert(serviceInfo.serviceInfo.AudioPID[j]);
            }
        }
    }
    // 動画、音声のPESは不要なので削っておく
    this->packetQueue.setPIDsToExclude(std::move(pesPIDList));

    if (this->currentChannel.NetworkID != lastNetworkID ||
        this->currentService.ServiceID != lastServiceID)
    {
        this->currentServiceIsOneSeg = false;
        this->packetQueue.clear();
        this->UpdateCommentChannel();
    }
    Tune();
    return true;
}

void CDataBroadcastingWV2::RestoreVideoWindow()
{
    RECT r;
    if (GetClientRect(hContainerWnd, &r))
    {
        PostMessageW(hContainerWnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right - r.left, r.bottom - r.top));
    }
}

BOOL CALLBACK CDataBroadcastingWV2::WindowMessageCallback(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* pResult, void* pUserData)
{
    if (uMsg == WM_SIZE)
    {
        auto pThis = (CDataBroadcastingWV2*)pUserData;
        if (pThis->hContainerWnd && pThis->hMessageWnd)
        {
#if 0
            if (!pThis->invisible)
            {
                // 無理やり動画ウィンドウを移動させている都合上リサイズ時に位置大きさが初期化されてしまうので一時的に非表示にさせる
                SetWindowPos(pThis->hVideoWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
            }
#endif
            // 実際に動画ウィンドウの大きさが変わるのはメッセージ処理後なのでPostMessageでやり過ごす
            PostMessageW(pThis->hMessageWnd, WM_APP_RESIZE, 0, 0);
        }
    }
    return FALSE;
}

std::wstring CDataBroadcastingWV2::GetIniItem(const wchar_t* key, const wchar_t* def)
{
    DWORD size = 100;
    std::wstring item(size, 0);
    while (true)
    {
        auto result = GetPrivateProfileStringW(L"TVTDataBroadcastingWV2", key, def, &item[0], size, this->iniFile.c_str());
        if (result + 1 != size)
        {
            item.resize(result);
            return item;
        }
        size *= 2;
        item.resize(size);
    }
}
INT CDataBroadcastingWV2::GetIniItem(const wchar_t* key, INT def)
{
    return GetPrivateProfileIntW(L"TVTDataBroadcastingWV2", key, def, this->iniFile.c_str());
}

bool CDataBroadcastingWV2::SetIniItem(const wchar_t* key, const wchar_t* data)
{
    return WritePrivateProfileStringW(L"TVTDataBroadcastingWV2", key, data, this->iniFile.c_str());
}

bool CDataBroadcastingWV2::Initialize()
{
    auto filename = wil::GetModuleFileNameW<std::wstring>(g_hinstDLL);
    std::filesystem::path path(filename);
    path.replace_extension();
    baseDirectory = path;
    resourceDirectory = path / L"resources";
    webView2DataDirectory = path / L"WebView2Data";
    webView2Directory = path / L"WebView2";
    path.replace_extension(L".ini");
    this->iniFile = path;
    m_pApp->SetEventCallback(EventCallback, this);
    m_pApp->RegisterCommand(IDC_KEY_D, L"DataButton", L"dボタン");
    m_pApp->RegisterCommand(IDC_KEY_D_OR_ENABLE_PLUGIN, L"DataButtonOrEnablePlugin", L"プラグイン有効/dボタン");
    m_pApp->RegisterCommand(IDC_KEY_UP, L"Up", L"↑");
    m_pApp->RegisterCommand(IDC_KEY_DOWN, L"Down", L"↓");
    m_pApp->RegisterCommand(IDC_KEY_LEFT, L"Left", L"←");
    m_pApp->RegisterCommand(IDC_KEY_RIGHT, L"Right", L"→");
    m_pApp->RegisterCommand(IDC_KEY_0, L"Digit0", L"0");
    m_pApp->RegisterCommand(IDC_KEY_1, L"Digit1", L"1");
    m_pApp->RegisterCommand(IDC_KEY_2, L"Digit2", L"2");
    m_pApp->RegisterCommand(IDC_KEY_3, L"Digit3", L"3");
    m_pApp->RegisterCommand(IDC_KEY_4, L"Digit4", L"4");
    m_pApp->RegisterCommand(IDC_KEY_5, L"Digit5", L"5");
    m_pApp->RegisterCommand(IDC_KEY_6, L"Digit6", L"6");
    m_pApp->RegisterCommand(IDC_KEY_7, L"Digit7", L"7");
    m_pApp->RegisterCommand(IDC_KEY_8, L"Digit8", L"8");
    m_pApp->RegisterCommand(IDC_KEY_9, L"Digit9", L"9");
    m_pApp->RegisterCommand(IDC_KEY_10, L"Digit10", L"10");
    m_pApp->RegisterCommand(IDC_KEY_11, L"Digit11", L"11");
    m_pApp->RegisterCommand(IDC_KEY_12, L"Digit12", L"12");
    m_pApp->RegisterCommand(IDC_KEY_ENTER, L"Enter", L"決定");
    m_pApp->RegisterCommand(IDC_KEY_BACK, L"Back", L"戻る");
    m_pApp->RegisterCommand(IDC_KEY_BLUE, L"BlueButton", L"青");
    m_pApp->RegisterCommand(IDC_KEY_RED, L"RedButton", L"赤");
    m_pApp->RegisterCommand(IDC_KEY_GREEN, L"GreenButton", L"緑");
    m_pApp->RegisterCommand(IDC_KEY_YELLOW, L"YellowButton", L"黄");
    m_pApp->RegisterCommand(IDC_KEY_RELOAD, L"Reload", L"再読み込み");
    m_pApp->RegisterCommand(IDC_KEY_DEVTOOL, L"OpenDevTools", L"開発者ツール");
    m_pApp->RegisterCommand(IDC_ENABLE_CAPTION, L"EnableCaption", L"字幕表示");
    m_pApp->RegisterCommand(IDC_DISABLE_CAPTION, L"DisableCaption", L"字幕非表示");
    m_pApp->RegisterCommand(IDC_TOGGLE_CAPTION, L"ToggleCaption", L"字幕表示/非表示切替");
    m_pApp->RegisterCommand(IDC_SHOW_REMOTE_CONTROL, L"ShowRemoteControl", L"リモコン表示");
    m_pApp->RegisterCommand(IDC_TASKMANAGER, L"TaskManager", L"タスクマネージャー");
    m_pApp->RegisterPluginIconFromResource(g_hinstDLL, MAKEINTRESOURCEW(IDB_PLUGIN));
    TVTest::PanelItemInfo panel = {};
    panel.Size = sizeof(panel);
    panel.Style = TVTest::PANEL_ITEM_STYLE_NEEDFOCUS;
    panel.pszIDText = L"TVTDataBroadcastingWV2Panel";
    panel.pszTitle = L"データ放送";
    panel.ID = 1;
    panel.hbmIcon = (HBITMAP)LoadImageW(g_hinstDLL, MAKEINTRESOURCEW(IDB_PLUGIN), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    m_pApp->RegisterPanelItem(&panel);
    TVTest::PanelItemInfo panel2 = {};
    panel2.Size = sizeof(panel2);
    panel2.Style = TVTest::PANEL_ITEM_STYLE_NEEDFOCUS;
    panel2.pszIDText = L"TVTDataBroadcastingWV2MomentumPanel";
    panel2.pszTitle = L"実況勢い";
    panel2.ID = 2;
    panel2.hbmIcon = (HBITMAP)LoadImageW(g_hinstDLL, MAKEINTRESOURCEW(IDB_PLUGIN), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    m_pApp->RegisterPanelItem(&panel2);
    TVTest::StatusItemInfo statusItemInfo = {};
    statusItemInfo.Size = sizeof(statusItemInfo);
    statusItemInfo.ID = 1;
    statusItemInfo.Flags = 0;
    statusItemInfo.Style = TVTest::STATUS_ITEM_STYLE_VARIABLEWIDTH;
    statusItemInfo.pszIDText = L"DataBroadcastingStatus";
    statusItemInfo.pszName = L"データ放送ステータス";
    statusItemInfo.MinWidth = 0;
    statusItemInfo.MaxWidth = -1;
    statusItemInfo.DefaultWidth = TVTest::StatusItemWidthByFontSize(10);
    statusItemInfo.MinHeight = 0;
    m_pApp->RegisterStatusItem(&statusItemInfo);
    this->useTVTestVolume = this->GetIniItem(L"UseTVTestVolume", true);
    this->useTVTestChannelCommand = this->GetIniItem(L"UseTVTestChannelCommand", true);
    this->restoreCaptionState = this->GetIniItem(L"RestoreCaptionState", 0) != 0;
    if (this->restoreCaptionState)
    {
        this->caption = this->GetIniItem(L"AutoEnableCaption", 0) != 0;
    }
    if (this->GetIniItem(L"AutoEnable", 0))
    {
        // Initialize()でプラグインを有効にするかPLUGIN_FLAG_ENABLEDEFAULTだとサイドパネルのプラグインボタンが押された状態にならないので遅延する
        this->CreateMessageWindow();
        PostMessageW(this->hMessageWnd, WM_APP_ENABLE_PLUGIN, 0, 0);
    }
    return true;
}

bool CDataBroadcastingWV2::Finalize()
{
    this->Disable(true);
    return true;
}

LRESULT CALLBACK CDataBroadcastingWV2::MessageWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    auto pThis = (CDataBroadcastingWV2*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    if (!pThis)
    {
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
    switch (uMsg)
    {
    case WM_TIMER:
    {
        switch (wParam)
        {
        case IDT_SHOW_EVR_WINDOW:
        {
            SetWindowPos(pThis->hVideoWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            KillTimer(hWnd, wParam);
            break;
        }
        case IDT_RESIZE:
        {
            if (pThis->webViewController && !pThis->oneSegWindowIsShown)
            {
                // FIXME: Fullscreen
                // Containerウィンドウが非表示になった場合Viewウィンドウを親にする
                if (!IsWindowVisible(pThis->hWebViewWnd))
                {
                    pThis->webViewController->put_ParentWindow(pThis->hViewWnd);
                }
                // Containerウィンドウが表示されていてViewウィンドウが親ならばContainerウィンドウを親にする
                else if (IsWindowVisible(pThis->hContainerWnd))
                {
                    HWND parent;
                    if (SUCCEEDED(pThis->webViewController->get_ParentWindow(&parent)))
                    {
                        if (parent != pThis->hContainerWnd)
                        {
                            pThis->webViewController->put_ParentWindow(pThis->hContainerWnd);
                        }
                    }
                }
                RECT rect;
                if (GetClientRect(pThis->hContainerWnd, &rect))
                {
                    if (memcmp(&pThis->containerRect, &rect, sizeof(RECT)))
                    {
                        pThis->containerRect = rect;
                        pThis->webViewController->put_Bounds(rect);
                    }
                }
                if (!pThis->invisible)
                {
                    pThis->ResizeVideoWindow();
                }
            }
            break;
        }
        }
        break;
    }
    case WM_APP_RESIZE:
    {
        if (pThis->webViewController && !pThis->oneSegWindowIsShown)
        {
#if 0
            if (pThis->hVideoWnd)
            {
                SetTimer(pThis->hMessageWnd, IDT_SHOW_EVR_WINDOW, 50, nullptr);
            }
#endif
            RECT rect;
            if (GetClientRect(pThis->hContainerWnd, &rect))
            {
                pThis->containerRect = rect;
                RECT prev;
                if (SUCCEEDED(pThis->webViewController->get_Bounds(&prev)))
                {
                    if (memcmp(&prev, &rect, sizeof(prev)))
                    {
                        pThis->webViewController->put_Bounds(rect);
                    }
                }
            }
        }
        break;
    }
    case WM_APP_PACKET:
    {
        if (!pThis->webView || !pThis->webViewLoaded)
        {
            // キューを消費しない
            break;
        }
        // スレッドが失速して回復したときなどに応答を維持するためメッセージごとの処理数を制限
        for (int popCount = 0; popCount < 5; popCount++)
        {
            auto packets = pThis->packetQueue.pop();
            if (!packets)
            {
                break;
            }
            WCHAR head[] = LR"({"type":"streamBase64","data":")";
            WCHAR tail[] = LR"("})";
            auto packetBlockSize = packets.value().size();
            auto packetSize = pThis->packetQueue.packetSize;
            size_t size = _countof(head) - 1 + (packetBlockSize + 2) / 3 * 4 /* Base64 */ + _countof(tail) + 1;
            if (pThis->packetsToJsonBuf.size() < size)
            {
                pThis->packetsToJsonBuf.resize(size);
            }
            {
                auto buf = pThis->packetsToJsonBuf.data();
                wcscpy_s(buf, size, head);
                size_t pos = 0;
                pos += wcslen(head);
                auto buffer = packets.value().data();
                static const WCHAR base64[66] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
                for (size_t i = 0; i < packetBlockSize; i += 3)
                {
                    buf[pos] = base64[buffer[i] >> 2];
                    pos += 1;
                    buf[pos] = base64[((buffer[i] & 3) << 4) | (i + 1 < packetBlockSize ? buffer[i + 1] >> 4 : 0)];
                    pos += 1;
                    buf[pos] = base64[i + 1 < packetBlockSize ? ((buffer[i + 1] & 15) << 2) |
                                                                (i + 2 < packetBlockSize ? buffer[i + 2] >> 6 : 0) : 64];
                    pos += 1;
                    buf[pos] = base64[i + 2 < packetBlockSize ? buffer[i + 2] & 63 : 64];
                    pos += 1;
                }
                wcscpy_s(buf + pos, size - pos, tail);
                auto hr = pThis->webView->PostWebMessageAsJson(buf);
            }
        }
        break;
    }
    case WM_APP_RESPONSE:
    {
        auto response = std::unique_ptr<DeferralResponse>((DeferralResponse*)lParam);
        wil::com_ptr<ICoreWebView2Environment> env;
        auto hr = pThis->webView.query<ICoreWebView2_2>()->get_Environment(env.put());
        if (FAILED(hr))
        {
            response->deferral->Complete();
            break;
        }
        wil::com_ptr<ICoreWebView2WebResourceResponse> webResponse;
        wil::com_ptr<IStream> stm;
        auto  hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_NODISCARD, response->content.size());
        if (!hGlobal)
        {
            response->deferral->Complete();
            break;
        }
        auto mem = GlobalLock(hGlobal);
        memcpy(mem, response->content.data(), response->content.size());
        GlobalUnlock(hGlobal);
        if (FAILED(CreateStreamOnHGlobal(hGlobal, TRUE, stm.put())))
        {
            response->deferral->Complete();
            break;
        }
        std::wistringstream iss(response->headers);
        std::wostringstream replacedHeader;
        std::wstring header;
        while (std::getline(iss, header))
        {
            if (!_wcsnicmp(header.c_str(), L"Access-Control-Allow-Origin", wcslen(L"Access-Control-Allow-Origin")))
            {
            }
            else if (header != L"\r")
            {
                replacedHeader << header;
                replacedHeader << L"\n";
            }
        }
        hr = env->CreateWebResourceResponse(stm.get(), response->statusCode, response->statusCodeText.c_str(), replacedHeader.str().c_str(), webResponse.put());
        if (FAILED(hr))
        {
            response->deferral->Complete();
            break;
        }
        wil::com_ptr<ICoreWebView2HttpResponseHeaders> responseHeaders;
        hr = webResponse->get_Headers(responseHeaders.put());
        if (FAILED(hr))
        {
            response->deferral->Complete();
            break;
        }
        responseHeaders->AppendHeader(L"Access-Control-Allow-Origin", L"https://tvtdatabroadcastingwv2.invalid");
        response->args->put_Response(webResponse.get());
        response->deferral->Complete();
        break;
    }
    case WM_APP_INPUT:
    {
        TVTest::ShowDialogInfo Info;

        Info.Flags = TVTest::SHOW_DIALOG_FLAG_MODELESS;
        Info.hinst = g_hinstDLL;
        Info.pszTemplate = MAKEINTRESOURCE(IDD_INPUT);
        Info.pMessageFunc = InputDialog::DlgProc;
        pThis->inputDialog = std::unique_ptr<InputDialog>((InputDialog*)lParam);
        Info.pClientData = pThis->inputDialog.get();
        Info.hwndOwner = pThis->m_pApp->GetFullscreen() ? pThis->GetFullscreenWindow() : pThis->m_pApp->GetAppWindow();

        auto hWnd = (HWND)pThis->m_pApp->ShowDialog(&Info);
        if (hWnd)
        {
            RECT dialogRect;
            RECT rect;
            if (GetWindowRect(hWnd, &dialogRect) && GetWindowRect(Info.hwndOwner, &rect))
            {
                // 中央に配置
                auto x = (rect.right + rect.left) / 2 - (dialogRect.right - dialogRect.left) / 2;
                auto y = (rect.bottom + rect.top) / 2 - (dialogRect.bottom - dialogRect.top) / 2;
                RECT moved = { x, y, x + dialogRect.right - dialogRect.left, y + dialogRect.bottom - dialogRect.top };
                // 範囲外の場合や作業領域からはみ出る場合は移動させない
                auto monitor = MonitorFromRect(&moved, MONITOR_DEFAULTTONULL);
                MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
                if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
                {
                    if (monitorInfo.rcWork.left <= moved.left && monitorInfo.rcWork.top <= moved.top && monitorInfo.rcWork.right >= moved.right && monitorInfo.rcWork.bottom >= moved.bottom)
                    {
                        SetWindowPos(hWnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
                    }
                }
            }
            ShowWindow(hWnd, SW_SHOW);
        }
        break;
    }
    case WM_APP_ENABLE_PLUGIN:
        pThis->m_pApp->EnablePlugin(true);
        return 0;
    case WM_APP_COMMENTS:
    {
        auto comments = std::unique_ptr<std::vector<Comment>>(reinterpret_cast<std::vector<Comment>*>(wParam));
        pThis->SendComments(std::move(*comments));
        break;
    }
    case WM_APP_LOGIN:
    {
        auto ev = std::unique_ptr<JkcnslLoginEvent>(reinterpret_cast<JkcnslLoginEvent*>(wParam));
        pThis->OnLoginEvent(ev->event, ev->message);
        break;
    }
    case WM_APP_AUTH:
    {
        auto info = std::unique_ptr<JkcnslSettings::LoginInfo>(reinterpret_cast<JkcnslSettings::LoginInfo*>(wParam));
        pThis->m_loggedIn = info->loggedIn;
        pThis->m_loginMail = utf8StrToWString(info->mail.c_str());
        // m_postTargetRefuge is driven by the chosen per-channel source
        // (UpdateCommentChannel), not the global cache_server_url.
        pThis->PushAuthState();
        break;
    }
    case WM_APP_CONN:
    {
        pThis->m_streamConnected = (wParam != 0);
        pThis->PushAuthState();
        break;
    }
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

HWND CDataBroadcastingWV2::GetFullscreenWindow()
{
    TVTest::HostInfo info;
    std::wstring appName(L"TVTest");
    if (this->m_pApp->GetHostInfo(&info))
    {
        appName = info.pszAppName;
    }
    struct Args
    {
        std::wstring fullscreenClass;
        HWND containerWnd;
    } args = { appName + L" Fullscreen" , nullptr };
    // フルスクリーンのとき
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hWnd, LPARAM lParam) -> BOOL {
        auto args = (Args*)lParam;
        WCHAR className[100];
        if (GetClassNameW(hWnd, className, _countof(className)))
        {
            if (!wcscmp(className, args->fullscreenClass.c_str()))
            {
                args->containerWnd = hWnd;
                return false;
            }
        }
        return true;
    }, (LPARAM)&args);
    return args.containerWnd;
}

void CDataBroadcastingWV2::OnFilterGraphInitialized(TVTest::FilterGraphInfo* pInfo)
{
    bool isRenderless = false;
    // VMR9
    if (SUCCEEDED(pInfo->pGraphBuilder->FindFilterByName(L"VMR9", this->vmr9Renderer.put())))
    {
        auto filterConfig = this->vmr9Renderer.query<IVMRFilterConfig9>();
        VMR9Mode mode;
        if (FAILED(filterConfig->GetRenderingMode((DWORD*)&mode)) || mode != VMR9Mode_Windowless)
        {
            // VMR9 (Renderless)
            // TVTest Video Container側の大きさを変えればいいけど字幕の位置も変わってしまう
            this->vmr9Renderer = nullptr;
            this->m_pApp->AddLog(L"VMR9 (Renderless)は非推奨", TVTest::LOG_TYPE_WARNING);
            isRenderless = true;
        }
    }
    // VMR7
    else if (SUCCEEDED(pInfo->pGraphBuilder->FindFilterByName(L"VMR7", this->vmr7Renderer.put())) || SUCCEEDED(pInfo->pGraphBuilder->FindFilterByName(L"VMR", this->vmr7Renderer.put())))
    {
        auto filterConfig = this->vmr7Renderer.try_query<IVMRFilterConfig>();
        VMRMode mode;
        if (filterConfig && (FAILED(filterConfig->GetRenderingMode((DWORD*)&mode)) || mode != VMRMode_Windowless))
        {
            // VMR7 (Renderless)
            // TVTest Video Container側の大きさを変えればいいけど字幕の位置も変わってしまう
            this->vmr7Renderer = nullptr;
            this->m_pApp->AddLog(L"VMR7 (Renderless)は非推奨", TVTest::LOG_TYPE_WARNING);
            isRenderless = true;
        }
    }
    // システムデフォルト
    else if (SUCCEEDED(pInfo->pGraphBuilder->QueryInterface(this->basicVideo.put())))
    {
        long l, t, w, h;
        if (FAILED(this->basicVideo->GetDestinationPosition(&l, &t, &w, &h)))
        {
            // EVR
            this->basicVideo = nullptr;
        }
    }
    std::vector<HWND> childWindows;
    TVTest::HostInfo info;
    std::wstring appName(L"TVTest");
    if (this->m_pApp->GetHostInfo(&info))
    {
        appName = info.pszAppName;
    }
    auto splitterClass = appName + L" Splitter";
    auto viewClass = appName + L" View";
    auto videoContainerClass = appName + L" Video Container";
    this->hViewWnd = FindWindowExW(FindWindowExW(this->m_pApp->GetAppWindow(), nullptr, splitterClass.c_str(), nullptr), nullptr, viewClass.c_str(), nullptr);
    this->hContainerWnd = FindWindowExW(this->hViewWnd, nullptr, videoContainerClass.c_str(), nullptr);
    if (!this->hContainerWnd)
    {
        auto fullscreenWnd = this->GetFullscreenWindow();
        this->hViewWnd = FindWindowExW(FindWindowExW(fullscreenWnd, nullptr, splitterClass.c_str(), nullptr), nullptr, viewClass.c_str(), nullptr);
        this->hContainerWnd = FindWindowExW(this->hViewWnd, nullptr, videoContainerClass.c_str(), nullptr);
    }
    // まず動画ウィンドウをクラス名で検索してみる
    // 0.10
    this->hVideoWnd = FindWindowExW(this->hContainerWnd, nullptr, L"LibISDB EVR Video Window", nullptr);
    if (this->hVideoWnd == nullptr)
    {
        // 0.9
        this->hVideoWnd = FindWindowExW(this->hContainerWnd, nullptr, L"Bon DTV EVR Video Window", nullptr);
    }
    if (this->hVideoWnd == nullptr)
    {
        this->hVideoWnd = FindWindowExW(this->hContainerWnd, nullptr, L"madVR", nullptr);
    }
    // 見つからなければNotification BarでもなくPseudo OSDでもないウィンドウを動画ウィンドウとする
    if (this->hVideoWnd == nullptr)
    {
        auto notifBarClass = appName + L" Notification Bar";
        auto pseudoOSDClass = appName + L" Pseudo OSD";
        EnumChildWindows(this->hContainerWnd, [](HWND hWnd, LPARAM lParam) -> BOOL {
            auto childWindows = (std::vector<HWND>*)lParam;
            childWindows->push_back(hWnd);
            return true;
        }, (LPARAM)&childWindows);
        for (auto it = std::rbegin(childWindows); it != std::rend(childWindows); ++it)
        {
            auto hWnd = *it;
            if (GetParent(hWnd) != this->hContainerWnd)
            {
                continue;
            }
            WCHAR className[100];
            if (GetClassNameW(hWnd, className, _countof(className)))
            {
                if (className == notifBarClass || className == pseudoOSDClass)
                {
                    continue;
                }
                if (hWnd == hWebViewWnd)
                {
                    continue;
                }
                this->hVideoWnd = hWnd;
            }
        }
    }
    if (isRenderless)
    {
        this->hVideoWnd = this->hContainerWnd;
        this->hContainerWnd = GetParent(this->hContainerWnd);
    }
    if (this->deferWebView)
    {
        this->deferWebView = false;
        this->InitWebView2();
    }
    if (this->hVideoWnd)
    {
        // madVR EVR EVR (Custom Renderer)
        SetWindowPos(this->hVideoWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (this->basicVideo)
    {
        this->hVideoWnd = nullptr;
    }
    this->ResizeVideoWindow();
}

void CDataBroadcastingWV2::OnFilterGraphFinalize(TVTest::FilterGraphInfo* pInfo)
{
    this->basicVideo = nullptr;
    this->vmr7Renderer = nullptr;
    this->vmr9Renderer = nullptr;
    this->hVideoWnd = nullptr;
}

void CDataBroadcastingWV2::InitWebView2()
{
    if (!this->hContainerWnd)
    {
        this->deferWebView = true;
        return;
    }
    LPCWSTR webView2Directory = nullptr;
    if (std::filesystem::is_directory(std::filesystem::path(this->webView2Directory) / L"EBWebView"))
    {
        webView2Directory = this->webView2Directory.c_str();
    }
    auto resourceDirectory = this->GetIniItem(L"ResourceDirectory", this->resourceDirectory.c_str());
    if (!std::filesystem::is_directory(resourceDirectory))
    {
        MessageBoxW(this->m_pApp->GetAppWindow(), (L"リソースディレクトリが見つかりません。\n" + resourceDirectory).c_str(), L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
        this->m_pApp->EnablePlugin(false);
        return;
    }
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(
        L"--autoplay-policy=no-user-gesture-required "
        L"--disable-features=msSmartScreenProtection "
        // IWebView2Controller3::put_RasterizationScaleに1.0を指定すればいいはずだけどリサイズがうまくいかなかったり挙動が謎なのでコマンドラインを使う
        // 副作用として開発者ツールやタスクマネージャーのウィンドウ枠含めたスケーリングが100%固定になる
        L"--force-device-scale-factor=1"
    );
    auto result = CreateCoreWebView2EnvironmentWithOptions(webView2Directory, this->webView2DataDirectory.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, resourceDirectory](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
        if (!env)
        {
            wchar_t buf[9]{};
            _itow_s(result, buf, 16);
            MessageBoxW(this->m_pApp->GetAppWindow(), (std::wstring(L"WebView2を初期化できませんでした。(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)\nHRESULT = 0x") + buf).c_str(), L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
            this->m_pApp->EnablePlugin(false);
            return S_OK;
        }
        // 環境を保存し、保留中の勢いパネルがあればコントローラーを作成
        this->webViewEnv = env;
        if (this->momentumWebViewPending && this->hMomentumPanel)
        {
            this->momentumWebViewPending = false;
            this->CreateMomentumWebViewController(this->hMomentumPanel);
        }
        wil::unique_cotaskmem_string ver;
        if (SUCCEEDED(env->get_BrowserVersionString(ver.put())))
        {
            this->m_pApp->AddLog((std::wstring(L"WebView2 version: ") + ver.get()).c_str(), TVTest::LOG_TYPE_INFORMATION);
        }
        env->CreateCoreWebView2Controller(this->hMessageWnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [env, this, resourceDirectory](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            if (FAILED(result))
            {
                wchar_t buf[9]{};
                _itow_s(result, buf, 16);
                MessageBoxW(this->m_pApp->GetAppWindow(), (std::wstring(L"WebView2を初期化できませんでした。(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)\nHRESULT = 0x") + buf).c_str(), L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
                this->m_pApp->EnablePlugin(false);
                return S_OK;
            }
            if (controller != nullptr) {
                this->webViewController = controller;
                this->webViewController->get_CoreWebView2(this->webView.put());
            }
            auto hWebViewWnd = FindWindowExW(this->hMessageWnd, nullptr, nullptr, nullptr);
            this->hWebViewWnd = hWebViewWnd;
            // ICoreWebView2_3, ICoreWebView2Controller2: 1.0.774.44
            auto controller2 = this->webViewController.try_query<ICoreWebView2Controller2>();
            if (!controller2)
            {
                MessageBoxW(this->m_pApp->GetAppWindow(), L"WebView2のバージョンが古すぎます。", L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
            }
            else
            {
                COREWEBVIEW2_COLOR c = { };
                auto ff = controller2->put_DefaultBackgroundColor(c);
            }
            wil::com_ptr<ICoreWebView2Settings> settings;
            this->webView->get_Settings(settings.put());
            settings->put_IsScriptEnabled(TRUE);
            settings->put_AreDefaultScriptDialogsEnabled(TRUE);
            settings->put_IsWebMessageEnabled(TRUE);

            this->webViewController->put_ParentWindow(this->hContainerWnd);
            RECT bounds;
            GetClientRect(this->hContainerWnd, &bounds);
            this->webViewController->put_Bounds(bounds);
            SetWindowPos(hWebViewWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

            EventRegistrationToken token;
            auto webView3 = this->webView.query<ICoreWebView2_3>();
            webView3->SetVirtualHostNameToFolderMapping(L"TVTDataBroadcastingWV2.invalid", resourceDirectory.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);

            // 仮想ホスト以外へのリクエストは全てブロックする
            // 通信が有効になっている場合は内部のプロキシへの通信のみ許可する
            webView->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
            this->webView->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                wil::com_ptr<ICoreWebView2WebResourceRequest> request;
                HRESULT hr = args->get_Request(request.put());
                if (FAILED(hr))
                {
                    return hr;
                }
                wil::unique_cotaskmem_string uri;
                hr = request->get_Uri(uri.put());
                if (FAILED(hr))
                {
                    return hr;
                }
                auto get = L"https://TVTDataBroadcastingWV2-api.invalid/api/get/";
                auto post = L"https://TVTDataBroadcastingWV2-api.invalid/api/post/";
                LPCWSTR proxyUrl = nullptr;
                if (!_wcsnicmp(get, uri.get(), wcslen(get)))
                {
                    proxyUrl = uri.get() + wcslen(get);
                }
                else if (!_wcsnicmp(post, uri.get(), wcslen(post)))
                {
                    proxyUrl = uri.get() + wcslen(post);
                }
                if (this->proxySession && proxyUrl && this->enableNetwork)
                {
                    return this->Proxy(args, proxyUrl);
                }
                else if (_wcsnicmp(L"https://TVTDataBroadcastingWV2.invalid/", uri.get(), wcslen(L"https://TVTDataBroadcastingWV2.invalid/")))
                {
                    wil::com_ptr<ICoreWebView2Environment> env;
                    hr = this->webView.query<ICoreWebView2_2>()->get_Environment(env.put());
                    if (FAILED(hr))
                    {
                        return hr;
                    }
                    wil::com_ptr<ICoreWebView2WebResourceResponse> response;
                    hr = env->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", response.put());
                    if (FAILED(hr))
                    {
                        return hr;
                    }
                    return args->put_Response(response.get());
                }
                return S_OK;
            }).Get(), &token);

            this->webView->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this, hWebViewWnd](ICoreWebView2* webview, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                wil::unique_cotaskmem_string message;
                if (SUCCEEDED(args->get_WebMessageAsJson(message.put())))
                {
                    auto messageUTF8 = wstrToUTF8String(message.get());

                    auto a = nlohmann::json::parse(messageUTF8);
                    auto type = a["type"].get<std::string>();
                    if (type == "videoChanged")
                    {
                        auto left = a["left"].get<double>();
                        auto right = a["right"].get<double>();
                        auto top = a["top"].get<double>();
                        auto bottom = a["bottom"].get<double>();
                        auto invisible = a["invisible"].get<bool>();
                        RECT r;
                        r.left = (int)std::floor(left);
                        r.right = (int)std::ceil(right);
                        r.top = (int)std::floor(top);
                        r.bottom = (int)std::ceil(bottom);

                        this->invisible = invisible;
                        this->videoRect = r;
                        this->ResizeVideoWindow();
                    }
                    else if (type == "invisible")
                    {
                        auto invisible = a["invisible"].get<bool>();
                        this->invisible = invisible;
                        this->ResizeVideoWindow();
                    }
                    else if (type == "status")
                    {
                        auto url = utf8StrToWString(a["url"].get<std::string>().c_str());
                        auto receiving = a["receiving"].get<bool>();
                        auto loading = a["loading"].get<bool>();
                        this->status.url = url;
                        this->status.receiving = receiving;
                        this->status.loading = loading;
                        this->m_pApp->StatusItemNotify(1, TVTest::STATUS_ITEM_NOTIFY_REDRAW);
                    }
                    else if (type == "tune")
                    {
                        auto originalNetworkId = a["originalNetworkId"].get<int>();
                        auto transportStreamId = a["transportStreamId"].get<int>();
                        auto serviceId = a["serviceId"].get<int>();
                        TVTest::ChannelSelectInfo info = {};
                        info.Size = sizeof(info);
                        info.Flags = TVTest::CHANNEL_SELECT_FLAG_STRICTSERVICE | TVTest::CHANNEL_SELECT_FLAG_ALLOWDISABLED;
                        // FIXME: original_network_idでない
                        info.NetworkID = originalNetworkId;
                        info.TransportStreamID = transportStreamId;
                        info.ServiceID = serviceId;
                        info.Channel = -1;
                        info.Space = -1;
                        if (!this->m_pApp->SelectChannel(&info))
                        {
                            int numSpace = 0;
                            this->m_pApp->GetTuningSpace(&numSpace);
                            bool success = false;
                            for (int space = 0; space < numSpace && !success; space++)
                            {
                                for (int channel = 0; ; channel++)
                                {
                                    TVTest::ChannelInfo channelInfo = {};
                                    if (!this->m_pApp->GetChannelInfo(space, channel, &channelInfo))
                                    {
                                        break;
                                    }
                                    // FIXME: original_network_idでない
                                    if (channelInfo.NetworkID == originalNetworkId && channelInfo.TransportStreamID == transportStreamId)
                                    {
                                        success = this->m_pApp->SetChannel(space, channel, serviceId);
                                        break;
                                    }
                                }
                            }
                            if (!success)
                            {
                                auto msg = (L"データ放送からの選局に失敗しました。映像/音声のないサービスまたはチャンネルスキャンされていない可能性があります。(original_network_id=" + std::to_wstring(originalNetworkId) + L",transport_stream_id=" + std::to_wstring(transportStreamId) + L",service_id=" + std::to_wstring(serviceId) + L")");
                                MessageBoxW(this->m_pApp->GetFullscreen() ? this->GetFullscreenWindow() : this->m_pApp->GetAppWindow(), msg.c_str(), nullptr, MB_ICONERROR | MB_OK);
                                this->m_pApp->AddLog(msg.c_str(), TVTest::LOG_TYPE_ERROR);
                                if (this->webView)
                                {
                                    this->webView->Reload();
                                }
                            }
                        }
                    }
                    else if (type == "usedKeyList")
                    {
                        auto&& usedKeyList = a["usedKeyList"];
                        if (usedKeyList.is_object())
                        {
                            UsedKey usedKey{};
                            usedKey.basic = usedKeyList["basic"].is_boolean();
                            usedKey.dataButton = usedKeyList["data-button"].is_boolean();
                            usedKey.numericTuning = usedKeyList["numeric-tuning"].is_boolean();
                            usedKey.special1 = usedKeyList["special-1"].is_boolean();
                            usedKey.special2 = usedKeyList["special-2"].is_boolean();
                            this->usedKey = usedKey;
                        }
                    }
                    else if (type == "input")
                    {
                        if (this->hMessageWnd)
                        {
                            auto&& allowedCharacters = a["allowedCharacters"];
                            auto cb = [this](std::unique_ptr<WCHAR[]> value)
                            {
                                if (!this->webView)
                                {
                                    return;
                                }
                                if (value)
                                {
                                    nlohmann::json msg{ { "type", "changeInput" }, { "value", wstrToUTF8String(value.get()) } };
                                    std::stringstream ss;
                                    ss << msg;
                                    auto wjson = utf8StrToWString(ss.str().c_str());
                                    this->webView->PostWebMessageAsJson(wjson.c_str());
                                }
                                else
                                {
                                    nlohmann::json msg{ { "type", "cancelInput" } };
                                    std::stringstream ss;
                                    ss << msg;
                                    auto wjson = utf8StrToWString(ss.str().c_str());
                                    this->webView->PostWebMessageAsJson(wjson.c_str());
                                }
                            };
                            auto inputDialog = new InputDialog(
                                utf8StrToWString(a["characterType"].get<std::string>().c_str()),
                                allowedCharacters.is_string() ? std::optional(utf8StrToWString(allowedCharacters.get<std::string>().c_str())) : std::nullopt,
                                a["maxLength"].get<int>(),
                                utf8StrToWString(a["value"].get<std::string>().c_str()),
                                std::move(cb),
                                utf8StrToWString(a["inputMode"].get<std::string>().c_str())
                            );
                            PostMessageW(this->hMessageWnd, WM_APP_INPUT, 0, (LPARAM)inputDialog);
                        }
                    }
                    else if (type == "cancelInput")
                    {
                        this->inputDialog = nullptr;
                    }
                    else if (type == "changeAudioStream")
                    {
                        auto componentId = a["componentId"].get<int>();
                        auto index = a["index"].get<int>();
                        auto&& channelId = a["channelId"];
                        if (componentId == -1)
                        {
                            this->RestoreMainAudio();
                        }
                        else
                        {
                            Audio audio;
                            audio.componentId = (BYTE)componentId;
                            audio.index = index;
                            this->isPlayingMainAudio = false;
                            if (channelId.is_number_integer())
                            {
                                audio.setChannelId(channelId.get<int>());
                            }
                            this->SelectAudio(audio);
                        }
                    }
                    else if (type == "changeMainAudioStream")
                    {
                        auto componentId = a["componentId"].get<int>();
                        auto index = a["index"].get<int>();
                        auto&& channelId = a["channelId"];
                        Audio audio;
                        audio.componentId = (BYTE)componentId;
                        audio.index = index;
                        if (channelId.is_number_integer())
                        {
                            audio.setChannelId(channelId.get<int>());
                        }
                        if (this->isPlayingMainAudio)
                        {
                            this->SelectAudio(audio);
                        }
                    }
                    else if (type == "serviceInfo")
                    {
                        auto cProfile = a["cProfile"].get<bool>();
                        auto serviceId = a["serviceId"].get<int>();
                        auto networkId = a["networkId"].get<int>();
                        if (this->currentService.ServiceID == serviceId && this->currentChannel.NetworkID == networkId)
                        {
                            this->currentServiceIsOneSeg = cProfile;
                            if (!cProfile)
                            {
                                this->DestroyOneSegWindow();
                            }
                        }
                    }
                    else if (type == "startBrowser")
                    {
                        auto uri = a["uri"].get<std::string>();
                        auto fullscreen = a["fullscreen"].get<bool>();
                        if (uri.starts_with("http://") || uri.starts_with("https://"))
                        {
#if 0
                            if (fullscreen)
                            {
                                this->DestroyOneSegWindow();
                            }
#endif
                            auto wuri = utf8StrToWString(uri.c_str());
                            ShellExecuteW(nullptr, L"open", wuri.c_str(), nullptr, nullptr, SW_SHOW);
                        }
                    }
                    else if (type == "channelsUpdate")
                    {
                        this->momentumChannels.clear();
                        for (auto& ch : a["channels"])
                        {
                            MomentumChannel mc;
                            mc.id    = ch["id"].get<int>();
                            mc.name  = ch["name"].get<std::string>();
                            mc.video = ch["video"].get<std::string>();
                            mc.force = ch["force"].get<int>();
                            auto& pt = ch["programTitle"];
                            mc.programTitle = pt.is_null() ? "" : pt.get<std::string>();
                            this->momentumChannels.push_back(std::move(mc));
                        }
                        this->SendMomentumChannels();
                    }
                    else if (type == "addNgUser")
                    {
                        auto& v = a["userId"];
                        if (v.is_string()) this->m_commentNg.AddUser(v.get<std::string>());
                    }
                    else if (type == "removeNgUser")
                    {
                        auto& v = a["userId"];
                        if (v.is_string()) this->m_commentNg.RemoveUser(v.get<std::string>());
                    }
                    else if (type == "addNgRegex")
                    {
                        auto& v = a["pattern"];
                        if (v.is_string()) this->m_commentNg.AddRegex(v.get<std::string>());
                    }
                    else if (type == "reloadNg")
                    {
                        this->m_commentNg.Load(this->iniFile);
                    }
                }
                return S_OK;
            }).Get(), &token);

            this->webView->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                if (!this->webViewLoaded)
                {
                    // 動画ウィンドウといい感じに合成させるために必要 (Windows 8以降じゃないと動かないはず)
                    SetWindowLongW(this->hWebViewWnd, GWL_EXSTYLE, GetWindowLongW(this->hWebViewWnd, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TRANSPARENT);
                    this->webViewController->put_IsVisible(true);
                }
                this->UpdateCaptionState(false);
                this->UpdateVolume();
                // Send channels WebSocket config
                {
                    std::wstring channelsWsUriW = this->GetIniItem(L"ChannelsWsUri", L"ws://localhost:5000/api/channels/ws");
                    std::string channelsWsUri(channelsWsUriW.begin(), channelsWsUriW.end());
                    nlohmann::json chMsg{
                        { "type",   "channelsConfig" },
                        { "ws_uri", channelsWsUri     },
                    };
                    std::stringstream chSs;
                    chSs << chMsg;
                    auto chJson = utf8StrToWString(chSs.str().c_str());
                    this->webView->PostWebMessageAsJson(chJson.c_str());
                }
                // Send comment config (opacity + duration + shadow)
                {
                    int opacityPct = this->GetIniItem(L"CommentOpacity", 100);
                    opacityPct = std::max(0, std::min(100, opacityPct));
                    int durationMs = this->GetIniItem(L"CommentDuration", 4000);
                    durationMs = std::max(1000, std::min(5000, durationMs));
                    std::wstring shadowColorW = this->GetIniItem(L"CommentShadowColor", L"rgba(0,0,0,0.7)");
                    std::string shadowColor(shadowColorW.begin(), shadowColorW.end());
                    bool shadowEnabled  = this->GetIniItem(L"CommentShadow",  1) != 0;
                    bool outlineEnabled = this->GetIniItem(L"CommentOutline", 0) != 0;
                    int fontSizeMedium  = this->GetIniItem(L"CommentFontSize", 24);
                    fontSizeMedium = std::max(8, std::min(64, fontSizeMedium));
                    nlohmann::json cfgMsg{
                        { "type",              "commentConfig"    },
                        { "opacity",           opacityPct / 100.0 },
                        { "duration_ms",       durationMs         },
                        { "shadow_color",      shadowColor        },
                        { "shadow_enabled",    shadowEnabled      },
                        { "outline_enabled",   outlineEnabled     },
                        { "font_size_medium",  fontSizeMedium     },
                    };
                    std::stringstream cfgSs;
                    cfgSs << cfgMsg;
                    auto cfgJson = utf8StrToWString(cfgSs.str().c_str());
                    this->webView->PostWebMessageAsJson(cfgJson.c_str());
                }
                if (this->proxySession)
                {
                    this->UpdateNetworkState();
                }
                this->UpdateAudioStream();
                this->webViewLoaded = true;
                if (this->oneSegWindowIsShown)
                {
                    this->webView->PostWebMessageAsJson(LR"({"type":"launchOneSeg"})");
                }
                return S_OK;
            }).Get(), &token);
            this->Tune();
            return S_OK;
        }).Get());
        return S_OK;
    }).Get());
    if (FAILED(result))
    {
        wchar_t buf[9]{};
        _itow_s(result, buf, 16);

        if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        {
            MessageBoxW(this->m_pApp->GetAppWindow(), (std::wstring(L"WebView2を初期化できませんでした。(CreateCoreWebView2EnvironmentWithOptions)\nWebView2がインストールされていない可能性があります。\nHRESULT = 0x") + buf).c_str(), L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
        }
        else
        {
            MessageBoxW(this->m_pApp->GetAppWindow(), (std::wstring(L"WebView2を初期化できませんでした。(CreateCoreWebView2EnvironmentWithOptions)\nHRESULT = 0x") + buf).c_str(), L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
        }
        this->m_pApp->EnablePlugin(false);
    }
}

// cors無効コマンドライン引数を追加するなどでWebView2側に任せることも可能ではあるけど融通が利かないためWinHTTPを使う あとバージョン依存の問題があるらしい?
HRESULT CDataBroadcastingWV2::Proxy(ICoreWebView2WebResourceRequestedEventArgs* args, LPCWSTR proxyUrl)
{
    wil::com_ptr<ICoreWebView2WebResourceRequest> request;
    HRESULT hr = args->get_Request(request.put());
    if (FAILED(hr))
    {
        return hr;
    }
    wil::unique_cotaskmem_string method;
    hr = request->get_Method(method.put());
    if (FAILED(hr))
    {
        return hr;
    }
    wil::com_ptr<IStream> content;
    hr = request->get_Content(content.put());
    std::vector<BYTE> contentBuffer;
    ULONG contentSize;
    if (SUCCEEDED(hr) && content)
    {
        // fetchから呼んだ場合CMemStreamなので同期的に取得する
        STATSTG statstg = {};
        hr = content->Stat(&statstg, STATFLAG_NONAME);
        auto size = statstg.cbSize.QuadPart;
        if (SUCCEEDED(hr) && size <= std::numeric_limits<ULONG>::max())
        {
            contentBuffer.resize((ULONG)size);
            content->Read(contentBuffer.data(), (ULONG)size, &contentSize);
            contentBuffer.resize(contentSize);
        }
    }
    wil::com_ptr<ICoreWebView2HttpRequestHeaders> headers;
    hr = request->get_Headers(headers.put());
    if (FAILED(hr))
    {
        return hr;
    }
    wil::com_ptr<ICoreWebView2HttpHeadersCollectionIterator> iterator;
    hr = headers->GetIterator(iterator.put());
    if (FAILED(hr))
    {
        return hr;
    }
    BOOL hasCurrent = false;
    std::vector<std::pair<wil::unique_cotaskmem_string, wil::unique_cotaskmem_string>> headersCo;
    std::vector<std::pair<LPCWSTR, LPCWSTR>> headersPtr
    {
        { L"Pragma", L"no-cache" },
        { L"Accept-Language", L"ja" },
    };
    while (SUCCEEDED(iterator->get_HasCurrentHeader(&hasCurrent)) && hasCurrent)
    {
        wil::unique_cotaskmem_string name;
        wil::unique_cotaskmem_string value;
        if (SUCCEEDED(iterator->GetCurrentHeader(name.put(), value.put())))
        {
            if (!_wcsicmp(name.get(), L"if-modified-since") || !_wcsicmp(name.get(), L"cache-control") || !_wcsicmp(name.get(), L"content-type"))
            {
                headersPtr.push_back({ name.get(), value.get() });
                headersCo.push_back({ std::move(name), std::move(value) });
            }
            BOOL hasNext;
            if (FAILED(iterator->MoveNext(&hasNext)))
            {
                break;
            }
        }
    }
    wil::com_ptr<ICoreWebView2Deferral> deferral;
    args->GetDeferral(deferral.put());
    if (!ProxyRequest::RequestAsync(*this->proxySession.get(), proxyUrl, method.get(), std::move(contentBuffer), headersPtr, [deferral]() -> void
    {
        // error
        deferral->Complete();
    }, [this, args = wil::com_ptr<ICoreWebView2WebResourceRequestedEventArgs>(args), deferral](DWORD statusCode, LPCWSTR statusCodeText, LPCWSTR headers, size_t contentLength, BYTE* content) -> void
    {
        if (this->hMessageWnd)
        {
            auto response = new DeferralResponse
            {
                deferral,
                args,
                statusCode,
                std::wstring(statusCodeText),
                std::wstring(headers),
                std::vector<BYTE>(content, content + contentLength),
            };
            PostMessageW(this->hMessageWnd, WM_APP_RESPONSE, 0, (LPARAM)response);
        }
        else
        {
            deferral->Complete();
        }
        return;
    }))
    {
        deferral->Complete();
    }
    return S_OK;
}

void CDataBroadcastingWV2::ResizeVideoWindow()
{
    if (this->invisible || this->oneSegWindowIsShown)
    {
        this->RestoreVideoWindow();
    }
    else
    {
        if (this->videoRect.right - this->videoRect.left && this->videoRect.bottom - this->videoRect.top)
        {
            if (this->vmr9Renderer)
            {
                auto vmr9WindowlessControl = this->vmr9Renderer.query<IVMRWindowlessControl9>();
                vmr9WindowlessControl->SetVideoPosition(nullptr, &this->videoRect);
            }
            else if (this->vmr7Renderer)
            {
                auto vmr7WindowlessControl = this->vmr7Renderer.query<IVMRWindowlessControl>();
                vmr7WindowlessControl->SetVideoPosition(nullptr, &this->videoRect);
            }
            else if (this->basicVideo)
            {
                this->basicVideo->SetDestinationPosition(this->videoRect.left, this->videoRect.top, this->videoRect.right - this->videoRect.left, this->videoRect.bottom - this->videoRect.top);
            }
            else
            {
                RECT rect;
                if (GetClientRect(this->hVideoWnd, &rect))
                {
                    if (memcmp(&rect, &this->videoRect, sizeof(rect)))
                    {
                        SetWindowPos(this->hVideoWnd, HWND_BOTTOM, this->videoRect.left, this->videoRect.top, this->videoRect.right - this->videoRect.left, this->videoRect.bottom - this->videoRect.top, SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                    }
                }
            }
        }
    }
}

void CDataBroadcastingWV2::Disable(bool finalize)
{
    this->RestoreMainAudio();
    this->RestoreVideoWindow();
    m_pApp->SetStreamCallback(TVTest::STREAM_CALLBACK_REMOVE, StreamCallback, this);
    if (!finalize)
    {
        m_pApp->SetWindowMessageCallback(nullptr, this);
    }
    this->oneSegWindow = nullptr;
    if (this->hRemoteWnd)
    {
        auto hWnd = this->hRemoteWnd;
        this->hRemoteWnd = nullptr;
        DestroyWindow(hWnd);
    }
    if (this->webViewController)
    {
        this->webView->Stop();
        this->webView.reset();
        this->webViewController->Close();
        this->webViewController.reset();
    }
    this->webViewLoaded = false;

    this->packetQueue.clear();

    if (this->hMessageWnd)
    {
        auto hWnd = this->hMessageWnd;
        DestroyWindow(hWnd);
        this->hMessageWnd = nullptr;
    }

    this->proxySession = nullptr;

    this->inputDialog = nullptr;

    if (finalize)
    {
        auto hWnd = this->hPanelWnd;
        this->hPanelWnd = nullptr;
        DestroyWindow(hWnd);
        if (this->hbrPanelBack)
        {
            DeleteObject(this->hbrPanelBack);
            this->hbrPanelBack = nullptr;
        }
        for (size_t i = 0; i < _countof(this->hbrBRGYBacks); i++)
        {
            if (this->hbrBRGYBacks[i])
            {
                DeleteObject(this->hbrBRGYBacks[i]);
                this->hbrBRGYBacks[i] = nullptr;
            }
        }
        if (this->hPanelFont)
        {
            DeleteObject(this->hPanelFont);
            this->hPanelFont = nullptr;
        }
    }
}

void CDataBroadcastingWV2::CreateMessageWindow()
{
    if (this->hMessageWnd != nullptr)
    {
        return;
    }
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = MessageWndProc;
    wc.hInstance = g_hinstDLL;
    wc.lpszClassName = L"TVTDataBroadcastingWV2 Message Window";
    RegisterClassExW(&wc);
    this->hMessageWnd = CreateWindowExW(0, L"TVTDataBroadcastingWV2 Message Window", L"TVTDataBroadcastingWV2 Message Window", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    SetWindowLongPtrW(this->hMessageWnd, GWLP_USERDATA, (LONG_PTR)this);
}

bool CDataBroadcastingWV2::OnPluginEnable(bool fEnable)
{
    this->status = {};
    if (fEnable)
    {
        this->CreateMessageWindow();
        if (!this->GetIniItem(L"DisableRemoteControl", 0))
        {
            this->ShowRemoteControlDialog();
        }
        m_pApp->SetStreamCallback(0, StreamCallback, this);
        m_pApp->SetWindowMessageCallback(WindowMessageCallback, this);
        if (this->useTVTestVolume)
        {
            this->OnVolumeChange(this->m_pApp->GetVolume(), this->m_pApp->GetMute());
        }
        InitWebView2();
        SetTimer(this->hMessageWnd, IDT_RESIZE, 1000, nullptr);
        this->EnablePanelButtons(true);
        if (this->GetIniItem(L"EnableNetwork", 0))
        {
            this->proxySession = std::unique_ptr<ProxySession>(new ProxySession());
        }
        {
            auto enableComment = this->GetIniItem(L"EnableComment", 0);
            char logbuf[128];
            sprintf_s(logbuf, "[TVTDataBroadcastingWV2] EnableComment=%d", enableComment);
            OutputDebugStringA(logbuf);
            OutputDebugStringA("\n");
            if (enableComment)
            {
                this->m_commentNg.Load(this->iniFile);
                this->m_jkcnslReader.SetCallback([this](std::vector<Comment> comments) {
                    PostMessageW(this->hMessageWnd, WM_APP_COMMENTS, reinterpret_cast<WPARAM>(
                        new std::vector<Comment>(std::move(comments))), 0);
                });
                this->m_jkcnslReader.SetConnectionCallback([this](bool connected) {
                    PostMessageW(this->hMessageWnd, WM_APP_CONN, connected ? 1 : 0, 0);
                });
                this->m_jkcnslLogin.SetCallback([this](JkcnslLogin::Event ev, std::string msg) {
                    PostMessageW(this->hMessageWnd, WM_APP_LOGIN, reinterpret_cast<WPARAM>(
                        new JkcnslLoginEvent{ ev, std::move(msg) }), 0);
                });
                // NicoJK-style connection model: refuge via the R command +
                // RefugeUri, nicovideo via the L command + chatStreamID. This
                // does NOT use jkcnsl's cache_server_url, so clear it (otherwise
                // L{chatStreamID} would be routed through the cache).
                this->m_chTable.Load(this->iniFile);
                JkcnslSettings::SetCacheServerUrl(this->GetJkcnslPath(), "");
                // Comment log recording (NicoJK-compatible). Use a folder
                // separate from NicoJK's to avoid conflicts.
                {
                    bool logEnabled = this->GetIniItem(L"LogfileMode", 0) != 0;
                    auto logFolder  = this->GetIniItem(L"LogfileFolder", L"");
                    this->m_logWriter.Configure(logFolder, logEnabled);
                }
                this->RefreshAuthState();
                this->UpdateCommentChannel();
            }
        }
    }
    else
    {
        this->m_jkcnslReader.Stop();
        this->m_logWriter.Close();
        this->EnablePanelButtons(false);
        this->Disable(false);
    }
    return true;
}

void CDataBroadcastingWV2::Tune()
{
    if (this->webView && this->currentService.ServiceID)
    {
        this->status = {};
        wil::unique_cotaskmem_string source;
        if (SUCCEEDED(this->webView->get_Source(source.put())))
        {
            std::wstring baseUrl(L"https://TVTDataBroadcastingWV2.invalid/TVTDataBroadcastingWV2.html?serviceId=");
            baseUrl += std::to_wstring(this->currentService.ServiceID);
            baseUrl += L"&networkId=";
            baseUrl += std::to_wstring(this->currentChannel.NetworkID);
            if (_wcsicmp(source.get(), baseUrl.c_str()))
            {
                this->oneSegWindow = nullptr;
                this->RestoreVideoWindow();
                this->webView->Navigate(baseUrl.c_str());
                this->usedKey.basic = true;
                this->usedKey.dataButton = true;
                this->usedKey.numericTuning = false;
            }
        }
    }
}

LRESULT CALLBACK CDataBroadcastingWV2::EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void* pClientData)
{
    CDataBroadcastingWV2* pThis = static_cast<CDataBroadcastingWV2*>(pClientData);
    return pThis->HandleEvent(Event, lParam1, lParam2, pClientData);
}

bool CDataBroadcastingWV2::OnStatusItemDraw(TVTest::StatusItemDrawInfo* pInfo)
{
    std::wstring statusItem;
    if ((pInfo->Flags & TVTest::STATUS_ITEM_DRAW_FLAG_PREVIEW) == 0)
    {
        if (this->status.receiving)
        {
            statusItem += L"データ取得中...";
        }
        statusItem += this->status.url;
        if (this->status.loading)
        {
            statusItem += L"を読み込み中...";
        }
    }
    else
    {
        statusItem = L"データ取得中...";
    }
    this->m_pApp->ThemeDrawText(pInfo->pszStyle, pInfo->hdc, statusItem.c_str(),
        pInfo->DrawRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
        pInfo->Color);

    return true;
}

bool CDataBroadcastingWV2::OnFullscreenChange(bool fFullscreen)
{
    if (this->hContainerWnd && this->hMessageWnd)
    {
        this->inputDialog = nullptr;
#if 0
        if (!this->invisible)
        {
            // 無理やり動画ウィンドウを移動させている都合上リサイズ時に位置大きさが初期化されてしまうので一時的に非表示にさせる
            SetWindowPos(this->hVideoWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
        }
#endif
        // 実際に動画ウィンドウの大きさが変わるのはメッセージ処理後なのでPostMessageでやり過ごす
        PostMessageW(this->hMessageWnd, WM_APP_RESIZE, 0, 0);
    }
    return true;
}

void CDataBroadcastingWV2::UpdateCaptionState(bool showIndicator)
{
    if (!this->webView)
    {
        return;
    }
    nlohmann::json msg{ { "type", "caption" }, { "enable", this->caption }, { "showIndicator", showIndicator } };
    std::stringstream ss;
    ss << msg;
    auto wjson = utf8StrToWString(ss.str().c_str());
    this->webView->PostWebMessageAsJson(wjson.c_str());
}

void CDataBroadcastingWV2::UpdateVolume()
{
    if (!this->webView)
    {
        return;
    }
    nlohmann::json msg{ { "type", "volume" }, { "value", this->useTVTestVolume ? this->currentVolume / (double)MAX_VOLUME : 1.0 } };
    std::stringstream ss;
    ss << msg;
    auto wjson = utf8StrToWString(ss.str().c_str());
    this->webView->PostWebMessageAsJson(wjson.c_str());
}

void CDataBroadcastingWV2::SendComments(std::vector<Comment> comments)
{
    if (comments.empty()) return;

    // arr  = comments to render on the canvas (NG-filtered)
    // logArr = all comments for the panel log list (NG ones flagged)
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json logArr = nlohmann::json::array();
    for (auto& c : comments)
    {
        // Record raw chat to the logfile (live comments only, before replace/NG).
        if (!c.past) this->m_logWriter.Write(this->m_currentJkID, c.date, c.raw);

        this->m_commentNg.ApplyReplace(c.text); // [CustomReplace] before NG/display
        // Past (backfilled) comments go to the log only, never flow on the canvas.
        if (!c.past && !this->m_commentNg.IsNG(c))
        {
            arr.push_back({
                { "text",     c.text     },
                { "color",    c.color    },
                { "position", c.position },
                { "size",     c.size     },
                { "date",     c.date     },
            });
        }
        // nb = NG by regex/command (fixed); user NG is applied live in the panel.
        logArr.push_back({
            { "text",   c.text   },
            { "color",  c.color  },
            { "userId", c.userId },
            { "date",   c.date   },
            { "refuge", c.refuge },
            { "past",   c.past   },
            { "nb",     this->m_commentNg.IsNGExceptUser(c) },
        });
    }

    // Flowing comments -> data-broadcasting WebView canvas.
    if (this->webView && this->webViewLoaded && !arr.empty())
    {
        nlohmann::json msg{ { "type", "comments" }, { "comments", arr } };
        std::stringstream ss; ss << msg;
        this->webView->PostWebMessageAsJson(utf8StrToWString(ss.str().c_str()).c_str());
    }

    // Comment log list -> momentum panel WebView.
    if (this->momentumWebView && this->momentumWebViewReady)
    {
        nlohmann::json m{ { "type", "commentLog" }, { "items", logArr } };
        std::string script = "_update(" + m.dump() + ")";
        this->momentumWebView->ExecuteScript(utf8StrToWString(script.c_str()).c_str(), nullptr);
    }
}

// Post a comment to the jikkyo server via the open jkcnsl stream.
// input may carry an optional "[mail]" prefix (e.g. "[shita green]text") to set
// color/position/size, mirroring NicoJK's posting box convention.
void CDataBroadcastingWV2::PostComment(const std::wstring& input)
{
    constexpr int   POST_COMMENT_MAX      = 76;   // jkcnsl/nicovideo limit
    constexpr DWORD POST_COMMENT_INTERVAL = 2000; // anti-spam guard (ms)

    // Japanese must be authored as wide literals: the project compiles without
    // /utf-8, so narrow Japanese literals would be CP932 and break json.dump().
    auto sendResult = [this](const char* status, const wchar_t* message) {
        if (!this->momentumWebView || !this->momentumWebViewReady) return;
        nlohmann::json j{ { "type", "postResult" }, { "status", status },
                          { "message", wstrToUTF8String(message) } };
        std::string script = "_update(" + j.dump() + ")";
        this->momentumWebView->ExecuteScript(utf8StrToWString(script.c_str()).c_str(), nullptr);
    };

    if (!this->m_jkcnslReader.IsRunning()) { sendResult("error", L"コメントサーバに接続していません"); return; }

    // Split optional [mail] prefix from the comment body.
    std::wstring mail, comm;
    if (!input.empty() && input[0] == L'[')
    {
        auto close = input.find(L']');
        if (close != std::wstring::npos) { mail = input.substr(1, close - 1); comm = input.substr(close + 1); }
        else                             { comm = input; }
    }
    else { comm = input; }

    if (comm.empty()) return;
    if (GetTickCount() - this->m_lastPostTick < POST_COMMENT_INTERVAL) { sendResult("error", L"投稿間隔が短すぎます"); return; }
    if (static_cast<int>(comm.size()) >= POST_COMMENT_MAX)            { sendResult("error", L"コメントが長すぎます"); return; }
    if (comm == this->m_lastPostComm)                                { sendResult("error", L"前回と同じコメントです"); return; }

    // Build "[mail]comment"; in mixing mode append the post target (nico/refuge)
    // to the mail field so jkcnsl routes the post to the right upstream.
    std::wstring targetTok = this->m_mixing ? (this->m_postTargetRefuge ? L" refuge" : L" nico") : L"";
    std::wstring body = L"[" + mail + targetTok + L"]" + comm;
    std::wstring cleaned;
    cleaned.reserve(body.size());
    for (wchar_t ch : body)
    {
        if (ch == L'\t' || ch == L'\n') cleaned.push_back(L'\x1e');
        else if (ch != L'\r')           cleaned.push_back(ch);
    }

    std::string payload = wstrToUTF8String(cleaned.c_str());
    if (this->m_jkcnslReader.Post(payload))
    {
        this->m_lastPostTick = GetTickCount();
        this->m_lastPostComm = comm;
        sendResult("ok", L"投稿しました");
    }
    else
    {
        sendResult("error", L"投稿に失敗しました");
    }
}

std::string CDataBroadcastingWV2::DetectJkChannelFor(WORD networkId, WORD serviceId) const
{
    // Key format used in NicoJK.ini [Channels]: 0x{NetCat}{ServiceID_hex}
    // NetCat = 0x4 for BS (NetworkID==4), 0xF for terrestrial
    WORD  netCat = (networkId == 4) ? 4 : 0xF;
    DWORD key    = ((DWORD)netCat << 16) | serviceId;

    wchar_t keyStr[16];
    swprintf_s(keyStr, L"0x%X", key);

    // Try our INI [Channels], then NicoJK.ini [Channels]
    const std::wstring paths[] = {
        this->iniFile,
        std::filesystem::path(this->baseDirectory).parent_path().wstring() + L"\\NicoJK.ini",
    };
    for (auto& path : paths) {
        wchar_t valStr[16] = {};
        DWORD len = GetPrivateProfileStringW(L"Channels", keyStr, L"", valStr, 16, path.c_str());
        if (len == 0) continue;

        const wchar_t* val = valStr;
        if (*val == L'+') val++; // strip preferred marker
        int jkNum = _wtoi(val);
        if (jkNum <= 0) return ""; // 0 or -1 = explicitly unmapped
        char buf[16];
        sprintf_s(buf, "jk%d", jkNum);
        return buf;
    }

    // Fallback: built-in table
    // NetworkID=0 はスキャン未実施を意味するため、地上波として見つからない場合はBSとしても検索する
    auto result = CommentFetcher::DetectChannel(networkId, serviceId);
    if (!result.empty()) return result;
    if (networkId != 4) {
        return CommentFetcher::DetectChannel(4, serviceId);
    }
    return "";
}

std::string CDataBroadcastingWV2::DetectJkChannel() const
{
    return this->DetectJkChannelFor(
        this->currentChannel.NetworkID,
        this->currentService.ServiceID);
}

void CDataBroadcastingWV2::UpdateCommentChannel()
{
    // Determine jikkyo channel (e.g. "jk141"); parse the numeric jkID.
    std::string video = this->DetectJkChannel();
    if (video.empty()) return;
    int jkID = 0;
    for (char c : video) { if (c >= '0' && c <= '9') jkID = jkID * 10 + (c - '0'); }
    if (jkID <= 0) return;
    this->m_currentJkID = jkID;

    JikkyoStream st;
    bool have = this->m_chTable.Resolve(jkID, st);

    std::string refugeUri = wstrToUTF8String(this->GetIniItem(L"RefugeUri", L"").c_str());
    bool mixing  = this->GetIniItem(L"RefugeMixing", 0) != 0;
    bool dropFwd = this->GetIniItem(L"DropForwardedComment", 0) != 0;
    bool postToRefuge = this->GetIniItem(L"PostToRefuge", 0) != 0;
    const std::string cookie; // empty: jkcnsl uses its stored login session

    // NicoJK connection decision (NicoJK.cpp ~4305):
    //   refuge available + RefugeUri set  -> R command (optionally mixed with nico)
    //   else nico chatStreamID + (no RefugeUri or mixing) -> L command
    std::string cmd;
    bool isMix = false, targetRefuge = false;
    if (have && !st.refugeChatStreamID.empty() && !refugeUri.empty())
    {
        std::string uri = refugeUri;
        std::string jkStr = "jk" + std::to_string(jkID);
        for (size_t i; (i = uri.find("{jkID}")) != std::string::npos;)
            uri.replace(i, 6, jkStr);
        for (size_t i; (i = uri.find("{chatStreamID}")) != std::string::npos;)
            uri.replace(i, 14, st.refugeChatStreamID);

        isMix = mixing && !st.chatStreamID.empty();
        int type = (dropFwd || isMix) ? 2 : 1;
        cmd = "R" + std::to_string(type) + " " + uri;
        if (isMix) cmd += " " + st.chatStreamID + " " + cookie; // R2 {uri} {nicoId} {cookie}
        targetRefuge = isMix ? postToRefuge : true;
    }
    else if (have && !st.chatStreamID.empty() && (refugeUri.empty() || mixing))
    {
        cmd = "L" + st.chatStreamID + (cookie.empty() ? "" : (" " + cookie));
        targetRefuge = false;
    }
    else
    {
        // Channel not supported (no stream id) -> disconnect, like NicoJK.
        char logbuf[128];
        sprintf_s(logbuf, "[TVTDataBroadcastingWV2] UpdateCommentChannel jk%d: no stream, disconnect", jkID);
        OutputDebugStringA(logbuf); OutputDebugStringA("\n");
        if (this->m_jkcnslReader.IsRunning()) this->m_jkcnslReader.Stop();
        return;
    }

    {
        char logbuf[256];
        sprintf_s(logbuf, "[TVTDataBroadcastingWV2] UpdateCommentChannel jk%d cmd=%s", jkID, cmd.c_str());
        OutputDebugStringA(logbuf); OutputDebugStringA("\n");
    }

    // No change: already streaming this exact command.
    if (this->m_jkcnslReader.IsRunning() && this->m_jkcnslReader.GetChannel() == cmd) return;

    // Clear on-screen comments when the stream changes.
    if (this->webView)
    {
        nlohmann::json msg{ { "type", "clearComments" } };
        std::stringstream ss; ss << msg;
        this->webView->PostWebMessageAsJson(utf8StrToWString(ss.str().c_str()).c_str());
    }

    this->m_mixing = isMix;
    this->m_postTargetRefuge = targetRefuge;
    if (this->m_jkcnslReader.IsRunning()) this->m_jkcnslReader.Stop();
    this->m_jkcnslReader.Start(this->GetJkcnslPath(), cmd);
    this->PushAuthState(); // reflect the new post target colour
}

// jkcnsl.exe is always in the TVTest folder (parent of Plugins directory).
// baseDirectory = "...\\Plugins\\TVTDataBroadcastingWV2"
// parent = "...\\Plugins", parent.parent = TVTest folder
std::wstring CDataBroadcastingWV2::GetJkcnslPath() const
{
    return (std::filesystem::path(this->baseDirectory)
        .parent_path().parent_path() / L"jkcnsl.exe").wstring();
}

// Maps a login event to panel UI text (Japanese authored as wide literals) and
// pushes it to the momentum panel. Runs on the UI thread (via WM_APP_LOGIN).
void CDataBroadcastingWV2::OnLoginEvent(JkcnslLogin::Event ev, const std::string& message)
{
    if (!this->momentumWebView || !this->momentumWebViewReady) return;

    const wchar_t* state = L"progress";
    std::wstring text;
    switch (ev)
    {
    case JkcnslLogin::Event::Progress:
        state = L"progress";
        if      (message == "start-login") text = L"ログインを開始しています…";
        else if (message == "start-clear") text = L"ログイン情報を削除しています…";
        else if (!message.empty())         text = utf8StrToWString(message.c_str()); // jkcnsl line (UTF-8)
        break;
    case JkcnslLogin::Event::Need2FA:
        state = L"need2fa";
        text  = L"2段階認証コードを入力して送信してください。";
        break;
    case JkcnslLogin::Event::Success:
        state = L"success";
        text  = (message == "clear") ? L"ログイン情報を削除しました。"
                                     : L"ニコニコログインに成功しました。チャンネル切替または再接続後に反映されます。";
        break;
    case JkcnslLogin::Event::Failure:
        state = L"failure";
        if      (message == "cancel")     text = L"ログインを中止しました。";
        else if (message == "disconnect") text = L"jkcnslとの通信が切断されました。";
        else if (message == "clear")      text = L"ログイン情報の削除に失敗しました。";
        else                              text = L"ログインに失敗しました。";
        break;
    }

    nlohmann::json j{ { "type", "loginStatus" },
                      { "state", wstrToUTF8String(state) },
                      { "message", wstrToUTF8String(text.c_str()) } };
    std::string script = "_update(" + j.dump() + ")";
    this->momentumWebView->ExecuteScript(utf8StrToWString(script.c_str()).c_str(), nullptr);

    // A completed login/clear changes the authenticated state.
    if (ev == JkcnslLogin::Event::Success) this->RefreshAuthState();
}

// Queries jkcnsl's login state on a detached worker (the query spawns jkcnsl,
// which can take a second), then marshals the result back via WM_APP_AUTH.
void CDataBroadcastingWV2::RefreshAuthState()
{
    std::wstring path = this->GetJkcnslPath();
    HWND hwnd = this->hMessageWnd;
    std::thread([path, hwnd]() {
        JkcnslSettings::LoginInfo info;
        JkcnslSettings::QueryLogin(path, info);
        PostMessageW(hwnd, WM_APP_AUTH, reinterpret_cast<WPARAM>(new JkcnslSettings::LoginInfo(info)), 0);
    }).detach();
}

void CDataBroadcastingWV2::PushAuthState()
{
    if (!this->momentumWebView || !this->momentumWebViewReady) return;
    // Post box colour by target (NicoJK-style): nico vs refuge, INI-configurable.
    std::wstring nicoColW   = this->GetIniItem(L"NicoEditBoxColor",   L"#bbbbff");
    std::wstring refugeColW = this->GetIniItem(L"RefugeEditBoxColor", L"#ffbbbb");
    std::wstring boxColW    = this->m_postTargetRefuge ? refugeColW : nicoColW;

    nlohmann::json j{ { "type", "authState" },
                      { "loggedIn", this->m_loggedIn },
                      { "connected", this->m_streamConnected },
                      { "target", this->m_postTargetRefuge ? "refuge" : "nico" },
                      { "boxColor", wstrToUTF8String(boxColW.c_str()) },
                      { "mail", wstrToUTF8String(this->m_loginMail.c_str()) } };
    std::string script = "_update(" + j.dump() + ")";
    this->momentumWebView->ExecuteScript(utf8StrToWString(script.c_str()).c_str(), nullptr);
}

void CDataBroadcastingWV2::PushNgUsers()
{
    if (!this->momentumWebView || !this->momentumWebViewReady) return;
    nlohmann::json users = nlohmann::json::array();
    for (const auto& u : this->m_commentNg.GetUsers()) users.push_back(u);
    nlohmann::json j{ { "type", "ngUsers" }, { "users", users } };
    std::string script = "_update(" + j.dump() + ")";
    this->momentumWebView->ExecuteScript(utf8StrToWString(script.c_str()).c_str(), nullptr);
}

void CDataBroadcastingWV2::SetCaptionState(bool enable)
{
    this->caption = enable;
    if (this->restoreCaptionState)
    {
        this->SetIniItem(L"AutoEnableCaption", enable ? L"1" : L"0");
    }
    if (this->hRemoteWnd)
    {
        SendDlgItemMessageW(this->hRemoteWnd, IDC_TOGGLE_CAPTION, BM_SETCHECK, this->caption ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (this->hPanelWnd)
    {
        SendDlgItemMessageW(this->hPanelWnd, IDC_TOGGLE_CAPTION, BM_SETCHECK, this->caption ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    this->UpdateCaptionState(true);
}

void CDataBroadcastingWV2::UpdateNetworkToggleButton(HWND hWnd)
{
    auto label = this->enableNetwork ? L"通信機能を無効化" : L"通信機能を有効化";
    SetDlgItemTextW(hWnd, IDC_TOGGLE_NETWORK, label);
}

void CDataBroadcastingWV2::SetNetworkState(bool enable)
{
    this->enableNetwork = enable;
    if (this->hRemoteWnd)
    {
        this->UpdateNetworkToggleButton(this->hRemoteWnd);
    }
    if (this->hPanelWnd)
    {
        this->UpdateNetworkToggleButton(this->hPanelWnd);
    }
    this->UpdateNetworkState();
}

void CDataBroadcastingWV2::UpdateNetworkState()
{
    if (!this->webView)
    {
        return;
    }
    nlohmann::json msg{ { "type", "enableNetwork" }, { "enable", this->enableNetwork } };
    std::stringstream ss;
    ss << msg;
    auto wjson = utf8StrToWString(ss.str().c_str());
    this->webView->PostWebMessageAsJson(wjson.c_str());
}

enum class UsedKeyType
{
    Unused,
    Basic,
    DataButton,
    NumericTuning,
    Special1,
};

struct CommandInfo
{
    int keyCode;
    UsedKeyType usedKeyType;
    LPCWSTR commandName;
};

static std::unordered_map<int, CommandInfo> commandList
{
    { IDC_KEY_UP, { 1, UsedKeyType::Basic } },
    { IDC_KEY_DOWN, { 2, UsedKeyType::Basic } },
    { IDC_KEY_LEFT, { 3, UsedKeyType::Basic } },
    { IDC_KEY_RIGHT, { 4, UsedKeyType::Basic } },
    { IDC_KEY_ENTER, { 18, UsedKeyType::Basic } },
    { IDC_KEY_BACK, { 19, UsedKeyType::Basic } },
    { IDC_KEY_BLUE, { 21, UsedKeyType::DataButton } },
    { IDC_KEY_RED, { 22, UsedKeyType::DataButton } },
    { IDC_KEY_GREEN, { 23, UsedKeyType::DataButton } },
    { IDC_KEY_YELLOW, { 24, UsedKeyType::DataButton } },
    { IDC_KEY_0, { 5, UsedKeyType::NumericTuning } },
    { IDC_KEY_1, { 6, UsedKeyType::NumericTuning, L"Channel1" } },
    { IDC_KEY_2, { 7, UsedKeyType::NumericTuning, L"Channel2" } },
    { IDC_KEY_3, { 8, UsedKeyType::NumericTuning, L"Channel3" } },
    { IDC_KEY_4, { 9, UsedKeyType::NumericTuning, L"Channel4" } },
    { IDC_KEY_5, { 10, UsedKeyType::NumericTuning, L"Channel5" } },
    { IDC_KEY_6, { 11, UsedKeyType::NumericTuning, L"Channel6" } },
    { IDC_KEY_7, { 12, UsedKeyType::NumericTuning, L"Channel7" } },
    { IDC_KEY_8, { 13, UsedKeyType::NumericTuning, L"Channel8" } },
    { IDC_KEY_9, { 14, UsedKeyType::NumericTuning, L"Channel9" } },
    { IDC_KEY_10, { 15, UsedKeyType::NumericTuning, L"Channel10" } },
    { IDC_KEY_11, { 16, UsedKeyType::NumericTuning, L"Channel11" } },
    { IDC_KEY_12, { 17, UsedKeyType::NumericTuning, L"Channel12" } },
};

static std::unordered_map<int, CommandInfo> oneSegCommandList
{
    { IDC_KEY_UP, { 1, UsedKeyType::Basic } },
    { IDC_KEY_DOWN, { 2, UsedKeyType::Basic } },
    { IDC_KEY_LEFT, { 3, UsedKeyType::Basic } },
    { IDC_KEY_RIGHT, { 4, UsedKeyType::Basic } },
    { IDC_KEY_ENTER, { 18, UsedKeyType::Basic } },
    { IDC_KEY_BACK, { 19, UsedKeyType::Basic } },
    { IDC_KEY_0, { 5, UsedKeyType::NumericTuning } },
    { IDC_KEY_1, { 6, UsedKeyType::NumericTuning, L"Channel1" } },
    { IDC_KEY_2, { 7, UsedKeyType::NumericTuning, L"Channel2" } },
    { IDC_KEY_3, { 8, UsedKeyType::NumericTuning, L"Channel3" } },
    { IDC_KEY_4, { 9, UsedKeyType::NumericTuning, L"Channel4" } },
    { IDC_KEY_5, { 10, UsedKeyType::NumericTuning, L"Channel5" } },
    { IDC_KEY_6, { 11, UsedKeyType::NumericTuning, L"Channel6" } },
    { IDC_KEY_7, { 12, UsedKeyType::NumericTuning, L"Channel7" } },
    { IDC_KEY_8, { 13, UsedKeyType::NumericTuning, L"Channel8" } },
    { IDC_KEY_9, { 14, UsedKeyType::NumericTuning, L"Channel9" } },
    { IDC_KEY_10, { 101, UsedKeyType::Special1, L"Channel10" } },
    { IDC_KEY_11, { 16, UsedKeyType::Unused, L"Channel11" } },
    { IDC_KEY_12, { 102, UsedKeyType::Special1, L"Channel12" } },
};

void CDataBroadcastingWV2::ShowRemoteControlDialog()
{
    if (!this->hRemoteWnd)
    {
        TVTest::ShowDialogInfo Info;

        Info.Flags = TVTest::SHOW_DIALOG_FLAG_MODELESS;
        Info.hinst = g_hinstDLL;
        Info.pszTemplate = MAKEINTRESOURCE(IDD_REMOTE_CONTROL);
        Info.pMessageFunc = RemoteControlDlgProc;
        Info.pClientData = this;
        Info.hwndOwner = this->m_pApp->GetAppWindow();

        if ((HWND)this->m_pApp->ShowDialog(&Info) != nullptr && this->hRemoteWnd != nullptr)
        {
            ShowWindow(this->hRemoteWnd, SW_SHOW);
        }
    }
}

bool CDataBroadcastingWV2::OnCommand(int ID)
{
    if (this->m_pApp->IsPluginEnabled())
    {
        switch (ID)
        {
        case IDC_SHOW_REMOTE_CONTROL:
        {
            this->ShowRemoteControlDialog();
            return true;
        }
        case IDC_KEY_SETTINGS:
        {
            if (this->m_pApp->GetFullscreen())
            {
                return this->OnPluginSettings(this->GetFullscreenWindow());
            }
            return this->OnPluginSettings(this->m_pApp->GetAppWindow());
        }
        case IDC_TOGGLE_NETWORK:
        {
            this->SetNetworkState(!this->enableNetwork);
            return true;
        }
        case IDC_TOGGLE_CAPTION:
            this->SetCaptionState(!this->caption);
            return true;
        case IDC_ENABLE_CAPTION:
            this->SetCaptionState(true);
            return true;
        case IDC_DISABLE_CAPTION:
            this->SetCaptionState(false);
            return true;
        }
    }
    if (!this->webView)
    {
        if (ID == IDC_KEY_D_OR_ENABLE_PLUGIN)
        {
            if (!this->m_pApp->IsPluginEnabled())
            {
                this->m_pApp->EnablePlugin(true);
                return true;
            }
        }
        auto command = commandList.find(ID);
        if (command != commandList.end())
        {
            if (command->second.commandName)
            {
                if (this->useTVTestChannelCommand)
                {
                    this->m_pApp->DoCommand(command->second.commandName);
                }
            }
        }
        return false;
    }
    switch (ID)
    {
    case IDC_KEY_D_OR_ENABLE_PLUGIN:
    case IDC_KEY_D:
        if (this->currentServiceIsOneSeg)
        {
            this->CreateOneSegWindow();
            this->webView->PostWebMessageAsJson(LR"({"type":"launchOneSeg"})");
        }
        else
        {
            this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":20})");
        }
        break;
    case IDC_KEY_DEVTOOL:
        this->webView->OpenDevToolsWindow();
        break;
    case IDC_KEY_RELOAD:
        this->webView->Reload();
        break;
    case IDC_TASKMANAGER:
    {
        auto webView6 = this->webView.try_query<ICoreWebView2_6>();
        if (webView6)
        {
            webView6->OpenTaskManagerWindow();
        }
        break;
    }
    default:
    {
        auto&& list = this->oneSegWindowIsShown ? oneSegCommandList : commandList;
        auto command = list.find(ID);
        if (command != list.end())
        {
            bool post = false;
            switch (command->second.usedKeyType)
            {
            case UsedKeyType::Basic:
                post = this->usedKey.basic;
                break;
            case UsedKeyType::DataButton:
                post = this->usedKey.dataButton;
                break;
            case UsedKeyType::NumericTuning:
                post = this->usedKey.numericTuning;
                break;
            case UsedKeyType::Special1:
                post = this->usedKey.special1;
                break;
            }
            if (post)
            {
                nlohmann::json msg{ { "type", "key" }, { "keyCode", command->second.keyCode } };
                std::stringstream ss;
                ss << msg;
                auto wjson = utf8StrToWString(ss.str().c_str());
                this->webView->PostWebMessageAsJson(wjson.c_str());
            }
            else if (command->second.commandName)
            {
                if (this->useTVTestChannelCommand)
                {
                    this->m_pApp->DoCommand(command->second.commandName);
                }
            }
        }
    }
    }
    return true;
}

INT_PTR CALLBACK CDataBroadcastingWV2::RemoteControlDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData)
{
    CDataBroadcastingWV2* pThis = static_cast<CDataBroadcastingWV2*>(pClientData);
    switch (uMsg) {
    case WM_INITDIALOG:
    {
        pThis->hRemoteWnd = hDlg;
        if (pThis->caption)
        {
            SendDlgItemMessageW(hDlg, IDC_TOGGLE_CAPTION, BM_SETCHECK, BST_CHECKED, 0);
        }
        if (pThis->GetIniItem(L"EnableNetwork", 0))
        {
            SetWindowLongW(GetDlgItem(hDlg, IDC_TOGGLE_NETWORK), GWL_STYLE, GetWindowLongW(GetDlgItem(hDlg, IDC_TOGGLE_NETWORK), GWL_STYLE) | WS_VISIBLE);
            pThis->UpdateNetworkToggleButton(hDlg);
        }
        pThis->UpdateDigitButton();
        return 1;
    }
    case WM_COMMAND:
    {
        if (LOWORD(wParam) == IDC_TOGGLE_CAPTION)
        {
            if (SendMessageW((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED)
                pThis->OnCommand(IDC_ENABLE_CAPTION);
            else
                pThis->OnCommand(IDC_DISABLE_CAPTION);
        }
        else if (LOWORD(wParam) == IDC_TOGGLE_COMMENT)
        {
            bool enabled = SendMessageW((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED;
            pThis->SetIniItem(L"CommentEnabled", enabled ? L"1" : L"0");
            pThis->UpdateCommentToggle();
        }
        else
        {
            pThis->OnCommand(LOWORD(wParam));
        }
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    {
        static const COLORREF crBRGY[4] = { RGB(0, 114, 214), RGB(201, 0, 0), RGB(27, 135, 0), RGB(227, 178, 0) };
        int id = GetDlgCtrlID((HWND)lParam);
        int i = id == IDC_STATIC_BLUE ? 0 : id == IDC_STATIC_RED ? 1 :
                id == IDC_STATIC_GREEN ? 2 : id == IDC_STATIC_YELLOW ? 3 : -1;
        if (i >= 0)
        {
            // スタティックコントロールの背景色を変える
            SetBkColor((HDC)wParam, crBRGY[i]);
            if (!pThis->hbrBRGYBacks[i])
            {
                pThis->hbrBRGYBacks[i] = CreateSolidBrush(crBRGY[i]);
            }
            return (INT_PTR)pThis->hbrBRGYBacks[i];
        }
        break;
    }
    case WM_CLOSE:
    {
        DestroyWindow(hDlg);
        return 1;
    }
    case WM_DESTROY:
    {
        pThis->hRemoteWnd = nullptr;
        return 1;
    }
    }
    return 0;
}

void CDataBroadcastingWV2::EnablePanelButtons(bool enable)
{
    if (!this->hPanelWnd)
    {
        return;
    }
    struct Args
    {
        bool enable;
        bool useTVTestChannelCommand;
    };
    Args args = { enable, this->useTVTestChannelCommand };
    EnumChildWindows(this->hPanelWnd, [](HWND hWnd, LPARAM lParam) -> BOOL {
        auto args = (Args*)lParam;
        auto id = GetDlgCtrlID(hWnd);
        if (id != IDC_KEY_D_OR_ENABLE_PLUGIN && id != IDC_KEY_SETTINGS)
        {
            if (args->useTVTestChannelCommand)
            {
                auto command = commandList.find(id);
                if (command == commandList.end() || !command->second.commandName)
                {
                    EnableWindow(hWnd, args->enable);
                }
                else
                {
                    EnableWindow(hWnd, true);
                }
            }
            else
            {
                EnableWindow(hWnd, args->enable);
            }
        }
        return true;
    }, (LPARAM)&args);
}

INT_PTR CALLBACK CDataBroadcastingWV2::PanelRemoteControlDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData)
{
    CDataBroadcastingWV2* pThis = static_cast<CDataBroadcastingWV2*>(pClientData);
    switch (uMsg)
    {
    case WM_INITDIALOG:
    {
        pThis->hPanelWnd = hDlg;
        if (pThis->caption)
        {
            SendDlgItemMessageW(hDlg, IDC_TOGGLE_CAPTION, BM_SETCHECK, BST_CHECKED, 0);
        }
        if (pThis->GetIniItem(L"EnableNetwork", 0))
        {
            SetWindowLongW(GetDlgItem(hDlg, IDC_TOGGLE_NETWORK), GWL_STYLE, GetWindowLongW(GetDlgItem(hDlg, IDC_TOGGLE_NETWORK), GWL_STYLE) | WS_VISIBLE);
            pThis->UpdateNetworkToggleButton(hDlg);
        }
        // コメント表示トグルの初期状態
        if (pThis->GetIniItem(L"CommentEnabled", 1))
            SendDlgItemMessageW(hDlg, IDC_TOGGLE_COMMENT, BM_SETCHECK, BST_CHECKED, 0);
        pThis->panelInitialDpi = GetDpi(hDlg);
        // アイテムの初期位置を記録
        pThis->panelItems.clear();
        EnumChildWindows(hDlg, [](HWND hWnd, LPARAM lParam) -> BOOL {
            RECT rect;
            if (GetWindowRect(hWnd, &rect))
            {
                ((std::vector<std::pair<HWND, RECT>>*)lParam)->emplace_back(hWnd, rect);
            }
            return true;
        }, (LPARAM)&pThis->panelItems);
        SendMessageW(hDlg, WM_APP_ON_PANEL_COLOR_CHANGE, 0, 0);
        SendMessageW(hDlg, WM_APP_ON_PANEL_SIZE, 0, 0);
        return 1;
    }
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
        return (INT_PTR)pThis->hbrPanelBack;
    case WM_DESTROY:
    {
        pThis->hPanelWnd = nullptr;
        return 1;
    }
    case WM_SIZE:
    {
        SendMessageW(hDlg, WM_APP_ON_PANEL_SIZE, 0, 0);
        break;
    }
    case WM_APP_ON_PANEL_COLOR_CHANGE:
    {
        COLORREF crBack = pThis->m_pApp->GetColor(L"PanelBack");
        if (pThis->hbrPanelBack)
        {
            DeleteObject(pThis->hbrPanelBack);
        }
        pThis->hbrPanelBack = CreateSolidBrush(crBack);
        if (pThis->m_pApp->GetDarkModeStatus() & TVTest::DARK_MODE_STATUS_PANEL_SUPPORTED)
        {
            // 必要に応じてボタンをダークモードにする
            bool dark = pThis->m_pApp->IsDarkModeColor(crBack);
            pThis->m_pApp->SetWindowDarkMode(hDlg, dark);
            for (const auto& item : pThis->panelItems)
            {
                WCHAR className[100];
                if (GetClassNameW(item.first, className, _countof(className)) && !_wcsicmp(className, L"BUTTON"))
                {
                    pThis->m_pApp->SetWindowDarkMode(item.first, dark);
                }
            }
        }
        InvalidateRect(hDlg, nullptr, TRUE);
        return 0;
    }
    case WM_APP_ON_PANEL_SIZE:
    {
        RECT clientRect;
        if (!pThis->panelItems.empty() && GetClientRect(hDlg, &clientRect))
        {
            // アイテムの縦方向が領域に収まらないときは縮める
            RECT unionRect = pThis->panelItems.front().second;
            for (const auto& item : pThis->panelItems)
            {
                if (GetWindowLongW(item.first, GWL_STYLE) & WS_VISIBLE)
                {
                    RECT rect = unionRect;
                    UnionRect(&unionRect, &rect, &item.second);
                }
            }
            UINT dpi = GetDpi(hDlg);
            double scaleY = (double)clientRect.bottom / MulDiv(unionRect.bottom - unionRect.top, dpi, pThis->panelInitialDpi);
            HDWP hDwp = BeginDeferWindowPos(pThis->panelItems.size());
            scaleY = scaleY < 0.6 ? 0.6 : scaleY < 1 ? scaleY : 1;
            for (const auto& item : pThis->panelItems)
            {
                int x = item.second.left - unionRect.left;
                int y = (int)((item.second.top - unionRect.top) * scaleY);
                int width = item.second.right - item.second.left;
                int height = (int)((item.second.bottom - unionRect.top) * scaleY) - y;
                hDwp = DeferWindowPos(hDwp, item.first, nullptr, MulDiv(x, dpi, pThis->panelInitialDpi), MulDiv(y, dpi, pThis->panelInitialDpi), MulDiv(width, dpi, pThis->panelInitialDpi), MulDiv(height, dpi, pThis->panelInitialDpi), SWP_NOZORDER | SWP_NOACTIVATE);
            }
            EndDeferWindowPos(hDwp);
        }
        return 0;
    }
    case WM_DPICHANGED:
    {
        pThis->SetPanelFont();
        SendMessageW(hDlg, WM_APP_ON_PANEL_SIZE, 0, 0);
        return 0;
    }
    }
    return RemoteControlDlgProc(hDlg, uMsg, wParam, lParam, pClientData);
}

INT_PTR CALLBACK CDataBroadcastingWV2::SettingsDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData)
{
    CDataBroadcastingWV2* pThis = static_cast<CDataBroadcastingWV2*>(pClientData);
    switch (uMsg) {
    case WM_INITDIALOG:
    {
        if (pThis->GetIniItem(L"DisableRemoteControl", 0))
        {
            SendDlgItemMessageW(hDlg, IDC_CHECK_DISABLE_REMOTECON, BM_SETCHECK, BST_CHECKED, 0);
        }
        if (pThis->GetIniItem(L"AutoEnable", 0))
        {
            SendDlgItemMessageW(hDlg, IDC_CHECK_AUTOENABLE, BM_SETCHECK, BST_CHECKED, 0);
        }
        if (pThis->restoreCaptionState)
        {
            SendDlgItemMessageW(hDlg, IDC_CHECK_RESTORE_CAPTION_STATE, BM_SETCHECK, BST_CHECKED, 0);
        }
        {
            int opacity = pThis->GetIniItem(L"CommentOpacity", 100);
            SetDlgItemInt(hDlg, IDC_EDIT_COMMENT_OPACITY, std::max(0, std::min(100, opacity)), FALSE);
            int duration = pThis->GetIniItem(L"CommentDuration", 4000);
            SetDlgItemInt(hDlg, IDC_EDIT_COMMENT_DURATION, std::max(1000, std::min(5000, duration)), FALSE);
        }
        if (pThis->GetIniItem(L"UseTVTestVolume", 1))
        {
            SendDlgItemMessageW(hDlg, IDC_CHECK_USE_TVTEST_VOLUME, BM_SETCHECK, BST_CHECKED, 0);
        }
        if (pThis->GetIniItem(L"UseTVTestChannelCommand", 1))
        {
            SendDlgItemMessageW(hDlg, IDC_CHECK_USE_TVTEST_CHANNEL_COMMAND, BM_SETCHECK, BST_CHECKED, 0);
        }
        return 1;
    }
    case WM_COMMAND:
    {
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            if (LOWORD(wParam) == IDOK)
            {
                auto disableRemoteControl = SendDlgItemMessageW(hDlg, IDC_CHECK_DISABLE_REMOTECON, BM_GETCHECK, 0, 0);
                if (!pThis->SetIniItem(L"DisableRemoteControl", disableRemoteControl ? L"1" : L"0"))
                {
                    MessageBoxW(hDlg, L"設定を保存できませんでした", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
                    EndDialog(hDlg, IDCANCEL);
                    return 1;
                }
                auto autoEnable = SendDlgItemMessageW(hDlg, IDC_CHECK_AUTOENABLE, BM_GETCHECK, 0, 0);
                if (!pThis->SetIniItem(L"AutoEnable", autoEnable ? L"1" : L"0"))
                {
                    MessageBoxW(hDlg, L"設定を保存できませんでした", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
                    EndDialog(hDlg, IDCANCEL);
                    return 1;
                }
                pThis->useTVTestVolume = SendDlgItemMessageW(hDlg, IDC_CHECK_USE_TVTEST_VOLUME, BM_GETCHECK, 0, 0);
                if (!pThis->SetIniItem(L"UseTVTestVolume", pThis->useTVTestVolume ? L"1" : L"0"))
                {
                    MessageBoxW(hDlg, L"設定を保存できませんでした", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
                    EndDialog(hDlg, IDCANCEL);
                    return 1;
                }
                pThis->UpdateVolume();
                pThis->useTVTestChannelCommand = SendDlgItemMessageW(hDlg, IDC_CHECK_USE_TVTEST_CHANNEL_COMMAND, BM_GETCHECK, 0, 0);
                if (!pThis->SetIniItem(L"UseTVTestChannelCommand", pThis->useTVTestChannelCommand ? L"1" : L"0"))
                {
                    MessageBoxW(hDlg, L"設定を保存できませんでした", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
                    EndDialog(hDlg, IDCANCEL);
                    return 1;
                }
                pThis->restoreCaptionState = SendDlgItemMessageW(hDlg, IDC_CHECK_RESTORE_CAPTION_STATE, BM_GETCHECK, 0, 0) != 0;
                if (!pThis->SetIniItem(L"RestoreCaptionState", pThis->restoreCaptionState ? L"1" : L"0"))
                {
                    MessageBoxW(hDlg, L"設定を保存できませんでした", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
                    EndDialog(hDlg, IDCANCEL);
                    return 1;
                }
                {
                    BOOL valid = FALSE;
                    int opacity = (int)GetDlgItemInt(hDlg, IDC_EDIT_COMMENT_OPACITY, &valid, FALSE);
                    if (!valid) opacity = 100;
                    opacity = std::max(0, std::min(100, opacity));
                    WCHAR opacityStr[8];
                    swprintf_s(opacityStr, L"%d", opacity);
                    if (!pThis->SetIniItem(L"CommentOpacity", opacityStr))
                    {
                        MessageBoxW(hDlg, L"設定を保存できませんでした", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
                        EndDialog(hDlg, IDCANCEL);
                        return 1;
                    }
                    BOOL validD = FALSE;
                    int duration = (int)GetDlgItemInt(hDlg, IDC_EDIT_COMMENT_DURATION, &validD, FALSE);
                    if (!validD) duration = 4000;
                    duration = std::max(1000, std::min(5000, duration));
                    WCHAR durationStr[8];
                    swprintf_s(durationStr, L"%d", duration);
                    if (!pThis->SetIniItem(L"CommentDuration", durationStr))
                    {
                        MessageBoxW(hDlg, L"設定を保存できませんでした", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
                        EndDialog(hDlg, IDCANCEL);
                        return 1;
                    }
                    // Apply both settings immediately to WebView2
                    if (pThis->webView && pThis->webViewLoaded)
                    {
                        nlohmann::json cfgMsg{
                            { "type",        "commentConfig"    },
                            { "opacity",     opacity / 100.0    },
                            { "duration_ms", duration           },
                        };
                        std::stringstream cfgSs;
                        cfgSs << cfgMsg;
                        auto cfgJson = utf8StrToWString(cfgSs.str().c_str());
                        pThis->webView->PostWebMessageAsJson(cfgJson.c_str());
                    }
                }
                pThis->EnablePanelButtons(pThis->m_pApp->IsPluginEnabled());
            }
            EndDialog(hDlg, LOWORD(wParam));
        }
        else if (LOWORD(wParam) == IDC_BUTTON_NVRAM_SETTING)
        {
            if (!pThis->webView)
            {
                MessageBoxW(hDlg, L"プラグインが有効の間のみ郵便番号・保存領域の設定を行えます。", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
            }
            else
            {
                TVTest::ShowDialogInfo Info;

                Info.Flags = 0;
                Info.hinst = g_hinstDLL;
                Info.pszTemplate = MAKEINTRESOURCE(IDD_SETTING_NVRAM);
                Info.pMessageFunc = NVRAMSettingsDialog::DlgProc;
                std::unique_ptr< NVRAMSettingsDialog> dialog(new NVRAMSettingsDialog(pThis->webView));
                Info.pClientData = dialog.get();
                Info.hwndOwner = hDlg;

                pThis->m_pApp->ShowDialog(&Info);
            }
        }
        return 1;
    }
    case WM_CLOSE:
    {
        EndDialog(hDlg, IDCANCEL);
        return 1;
    }
    case WM_DESTROY:
    {
        return 1;
    }
    }
    return 0;
}

bool CDataBroadcastingWV2::OnPluginSettings(HWND hwndOwner)
{
    TVTest::ShowDialogInfo Info;

    Info.Flags = 0;
    Info.hinst = g_hinstDLL;
    Info.pszTemplate = MAKEINTRESOURCE(IDD_SETTING);
    Info.pMessageFunc = SettingsDlgProc;
    Info.pClientData = this;
    Info.hwndOwner = hwndOwner;

    auto result = this->m_pApp->ShowDialog(&Info);
    return result == IDOK;
}

bool CDataBroadcastingWV2::OnColorChange()
{
    if (this->hPanelWnd)
    {
        SendMessageW(this->hPanelWnd, WM_APP_ON_PANEL_COLOR_CHANGE, 0, 0);
    }
    COLORREF newBack = this->m_pApp->GetColor(L"PanelBack");
    COLORREF newText = this->m_pApp->GetColor(L"PanelText");
    if (newBack != this->panelBackColor || newText != this->panelTextColor)
    {
        this->panelBackColor = newBack;
        this->panelTextColor = newText;
        this->SendMomentumTheme();
    }
    return true;
}

bool CDataBroadcastingWV2::OnPanelItemNotify(TVTest::PanelItemEventInfo* pInfo)
{
    switch (pInfo->Event)
    {
    case TVTest::PANEL_ITEM_EVENT_CREATE:
    {
        auto createEventInfo = CONTAINING_RECORD(pInfo, TVTest::PanelItemCreateEventInfo, EventInfo);
        if (pInfo->ID == 2)
        {
            HWND hWnd = CreateDialogParamW(g_hinstDLL, MAKEINTRESOURCEW(IDD_MOMENTUM_PANEL), createEventInfo->hwndParent,
                [](HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) -> INT_PTR {
                if (uMsg == WM_INITDIALOG)
                    SetWindowLongPtrW(hDlg, GWLP_USERDATA, lParam);
                auto pClientData = (void*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);
                return pClientData ? PanelMomentumDlgProc(hDlg, uMsg, wParam, lParam, pClientData) : 0;
            }, (LPARAM)this);
            ShowWindow(hWnd, SW_SHOW);
            createEventInfo->hwndItem = hWnd;
            this->hMomentumPanel = hWnd;
            break;
        }
        TVTest::ShowDialogInfo Info;
        Info.Flags = TVTest::SHOW_DIALOG_FLAG_MODELESS;
        Info.hinst = g_hinstDLL;
        Info.pszTemplate = MAKEINTRESOURCE(IDD_REMOTE_CONTROL_PANEL);
        Info.pMessageFunc = PanelRemoteControlDlgProc;
        Info.pClientData = this;
        Info.hwndOwner = createEventInfo->hwndParent;
        // できればこのようにしたいが、親がダイアログでない限りWS_CHILDなダイアログはうまくダークモードにならない
        // auto hWnd = (HWND)this->m_pApp->ShowDialog(&Info);
        // 本体側のダークモード関連の処理と競合するため素のダイアログを使う
        HWND hWnd = CreateDialogParamW(Info.hinst, Info.pszTemplate, Info.hwndOwner,
                                       [](HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) -> INT_PTR {
            if (uMsg == WM_INITDIALOG)
            {
                SetWindowLongPtrW(hDlg, GWLP_USERDATA, lParam);
            }
            auto pClientData = (void*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);
            return pClientData ? PanelRemoteControlDlgProc(hDlg, uMsg, wParam, lParam, pClientData) : 0;
        }, (LPARAM)Info.pClientData);
        ShowWindow(hWnd, SW_SHOW);
        createEventInfo->hwndItem = hWnd;
        this->EnablePanelButtons(this->m_pApp->IsPluginEnabled());
    }
    [[fallthrough]];
    case TVTest::PANEL_ITEM_EVENT_FONTCHANGED:
    {
        this->SetPanelFont();
        break;
    }
    }
    return true;
}

void CDataBroadcastingWV2::SetPanelFont()
{
    // フォントが大きすぎるとはみ出してしまうのでフォントの大きさに合わせてボタンの大きさを変更すべきではある
    // TVTestがやっているように全部自前で描画して処理するのは大変なのでダイアログで妥協
    if (this->hPanelFont)
    {
        DeleteObject(this->hPanelFont);
    }
    LOGFONTW lf;
    this->m_pApp->GetFont(L"PanelFont", &lf, GetDpi(this->hPanelWnd));
    this->hPanelFont = CreateFontIndirectW(&lf);
    SendMessageW(this->hPanelWnd, WM_SETFONT, (WPARAM)this->hPanelFont, true);
    EnumChildWindows(this->hPanelWnd, [](HWND hWnd, LPARAM lParam) -> BOOL {
        auto hFont = (HFONT)lParam;
        SendMessageW(hWnd, WM_SETFONT, (WPARAM)hFont, 0);
        return true;
    }, (LPARAM)this->hPanelFont);
}

void CDataBroadcastingWV2::UpdateCommentToggle()
{
    if (!this->webView) return;
    bool enabled = this->GetIniItem(L"CommentEnabled", 1) != 0;
    double opacity = enabled ? this->GetIniItem(L"CommentOpacity", 100) / 100.0 : 0.0;
    nlohmann::json msg{{"type", "commentConfig"}, {"opacity", opacity}};
    std::stringstream ss; ss << msg;
    auto wstr = utf8StrToWString(ss.str().c_str());
    this->webView->PostWebMessageAsJson(wstr.c_str());
}

static const wchar_t kMomentumHtml[] = LR"HTML(<!DOCTYPE html><html><head><meta charset="utf-8"><style>
:root{--bg:#f0f0f0;--fg:#000;--sb:rgba(128,128,128,.5);--hov:rgba(128,128,128,.15);--sel:rgba(128,128,128,.3)}
*{box-sizing:border-box;margin:0;padding:0}
[hidden]{display:none!important}
body{background:var(--bg);color:var(--fg);font:9pt "Meiryo UI",sans-serif;overflow:hidden;
     height:100vh;display:flex;flex-direction:column;position:relative}
#post{display:flex;align-items:center;gap:4px;padding:3px 4px;border-top:1px solid rgba(128,128,128,.3)}
#pi{flex:1;min-width:0;font:inherit;color:var(--fg);background:var(--bg);
    border:1px solid var(--sb);border-radius:3px;padding:2px 4px}
#pi:focus{outline:none;border-color:var(--fg)}
#pi:disabled{opacity:.5;cursor:not-allowed;background:rgba(128,128,128,.18)}
#cb{flex:0 0 auto;width:24px;height:24px;border:1px solid var(--sb);border-radius:3px;cursor:pointer;
    background:var(--bg);color:var(--fg);font-size:12pt;line-height:1;display:flex;align-items:center;justify-content:center}
#cb.open{background:var(--hov)}
#cmdpop{position:absolute;left:4px;right:4px;bottom:34px;z-index:10;display:flex;gap:6px;padding:6px;
        background:var(--bg);border:1px solid var(--sb);border-radius:5px;box-shadow:0 2px 8px rgba(0,0,0,.3)}
#cmdpop .po{flex:1;min-width:0;display:flex;flex-direction:column;gap:4px}
.pg{display:flex;align-items:center;gap:4px;min-height:20px;flex-wrap:wrap}
.pl{width:3em;flex-shrink:0;opacity:.7;font-size:9pt;text-align:right}
.cg{display:grid;grid-template-columns:repeat(5,20px);gap:3px}
.cc{width:20px;height:16px;border-radius:3px;cursor:pointer;border:1px solid var(--sb);
    display:flex;align-items:center;justify-content:center;font-size:9pt;color:#fff;text-shadow:0 1px 1px rgba(0,0,0,.65)}
.cc.on::after{content:'✓'}
.cc.c0{background:#fff;border-color:#aaa;color:#111;text-shadow:none}
.cc.c1{background:#e00}.cc.c2{background:#f7a}.cc.c3{background:#f80}.cc.c4{background:#fd0}
.cc.c5{background:#0b0}.cc.c6{background:#0cc}.cc.c7{background:#00e}.cc.c8{background:#808}.cc.c9{background:#222}
.tb{flex-shrink:0;padding:0 4px;height:16px;line-height:16px;border:1px solid var(--fg);border-radius:3px;
    cursor:pointer;background:transparent;color:var(--fg);font-size:9pt;opacity:.6}
.tb.on{background:var(--fg);color:var(--bg);opacity:1}
.db{margin-left:auto;flex-shrink:0;padding:0 6px;height:18px;border:1px solid var(--sb);border-radius:3px;
    cursor:pointer;background:transparent;color:inherit;font-size:9pt;opacity:.75}
#pv{width:92px;min-height:62px;flex-shrink:0;border:1px solid var(--sb);border-radius:4px;position:relative;overflow:hidden}
#pv::before{content:'';position:absolute;left:8px;right:8px;top:50%;border-top:1px dashed rgba(128,128,128,.35)}
#pt{position:absolute;left:50%;transform:translateX(-50%);white-space:nowrap;max-width:80px;overflow:hidden;
    text-overflow:ellipsis;font-weight:bold;text-shadow:0 1px 2px rgba(0,0,0,.28);font-size:11pt;color:var(--fg)}
#pt.ue{top:6px}#pt.naka{top:50%;transform:translate(-50%,-50%)}#pt.shita{bottom:6px}
#pt.big{font-size:14pt}#pt.small{font-size:9pt}
#pr{font-size:8pt;white-space:nowrap;overflow:hidden;max-width:40%}
#pr.ok{color:#3a3}#pr.error{color:#d44}
#lb{flex:0 0 auto;font:inherit;color:var(--fg);background:var(--bg);
    border:1px solid var(--sb);border-radius:3px;padding:2px 6px;cursor:pointer}
#lb:hover{background:var(--hov)}
#lb.authin{border-color:#3a8a3a;color:#3a8a3a}
#login{display:flex;flex-direction:column;gap:3px;padding:5px 4px;
       border-top:1px solid rgba(128,128,128,.3)}
#login input{font:inherit;color:var(--fg);background:var(--bg);
       border:1px solid var(--sb);border-radius:3px;padding:2px 4px}
#login input:focus{outline:none;border-color:var(--fg)}
#login .row{display:flex;gap:4px}
#login button{font:inherit;color:var(--fg);background:var(--bg);
       border:1px solid var(--sb);border-radius:3px;padding:2px 8px;cursor:pointer}
#login button:hover{background:var(--hov)}
#ls{font-size:8pt;min-height:1.2em;white-space:normal;word-break:break-all}
#ls.success{color:#3a3}#ls.failure{color:#d44}
#w{flex:1;min-height:0;overflow-y:auto}
#tabs{display:flex;flex:0 0 auto;border-bottom:1px solid rgba(128,128,128,.3)}
#tabs button{flex:1;font:inherit;color:var(--fg);background:transparent;border:none;
   padding:3px 0;cursor:pointer;opacity:.6;border-bottom:2px solid transparent}
#tabs button.on{opacity:1;border-bottom-color:var(--fg);font-weight:bold}
#tabs button:hover{background:var(--hov)}
#log{flex:1;min-height:0;overflow-y:auto;font-size:9pt;padding:1px 0}
#log::-webkit-scrollbar{width:8px}
#log::-webkit-scrollbar-thumb{background:var(--sb);border-radius:4px}
.le{display:flex;gap:5px;padding:1px 5px;white-space:nowrap}
.le:hover{background:var(--hov)}
.le .lt{flex:0 0 auto;opacity:.55;font-size:8pt;font-variant-numeric:tabular-nums}
.le .lm{flex:0 0 auto;font-size:8pt;font-family:monospace;opacity:.85}
.le .lm.rf{color:#c33}.le .lm.nc{color:#36c}
.le .lx{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis}
.le.ng{opacity:.4}.le.ng .lx{text-decoration:line-through}
.le.past{opacity:.65;font-style:italic}
table{width:100%;border-collapse:collapse;table-layout:fixed}
col.c0{width:56px}col.c1{width:90px}col.c2{width:44px}col.c3{width:auto}
th{position:sticky;top:0;background:var(--bg);text-align:left;padding:2px 4px;
   border-bottom:1px solid rgba(128,128,128,.3);cursor:pointer;user-select:none;white-space:nowrap;overflow:hidden}
th:hover{background:var(--hov)}
td{padding:1px 4px;white-space:nowrap;overflow:hidden}
tr:hover td{background:var(--hov)}
tr.sel td{background:var(--sel)}
.pe{color:#9acd32}
#w::-webkit-scrollbar{width:8px}
#w::-webkit-scrollbar-track{background:transparent}
#w::-webkit-scrollbar-thumb{background:var(--sb);border-radius:4px}
#w::-webkit-scrollbar-thumb:hover{background:var(--fg);opacity:.6}
</style></head><body>
<div id="tabs"><button id="tabF" class="on">勢い</button><button id="tabL">ログ</button></div>
<div id="w"><table>
<colgroup><col class="c0"><col class="c1"><col class="c2"><col class="c3"></colgroup>
<thead><tr>
<th onclick="srt(0)">実況番号<span id="a0"></span></th>
<th onclick="srt(1)">局名<span id="a1"></span></th>
<th onclick="srt(2)">勢い<span id="a2">▼</span></th>
<th onclick="srt(3)">番組<span id="a3"></span></th>
</tr></thead>
<tbody id="tb"></tbody>
</table></div>
<div id="log" hidden></div>
<div id="login" hidden>
<input id="lm" type="text" placeholder="メールアドレス" autocomplete="off">
<input id="lp" type="password" placeholder="パスワード" autocomplete="off">
<div class="row" id="otprow" hidden>
  <input id="lo" type="text" inputmode="numeric" placeholder="2段階認証コード" style="flex:1">
  <button id="losend">送信</button>
</div>
<div class="row">
  <button id="ldo">ログイン</button>
  <button id="lcancel">中止</button>
  <button id="lclear" style="margin-left:auto">情報削除</button>
</div>
<div id="ls"></div>
</div>
<div id="cmdpop" hidden>
<div class="po">
<div class="pg"><span class="pl">色</span><div class="cg"><div class="cc c0 on" data-c="" title="白"></div><div class="cc c1" data-c="red" title="赤"></div><div class="cc c2" data-c="pink" title="ピンク"></div><div class="cc c3" data-c="orange" title="橙"></div><div class="cc c4" data-c="yellow" title="黄"></div><div class="cc c5" data-c="green" title="緑"></div><div class="cc c6" data-c="cyan" title="水色"></div><div class="cc c7" data-c="blue" title="青"></div><div class="cc c8" data-c="purple" title="紫"></div><div class="cc c9" data-c="black" title="黒"></div></div></div>
<div class="pg"><span class="pl">位置</span><button class="tb on" data-p="">流れる</button><button class="tb" data-p="ue">上</button><button class="tb" data-p="shita">下</button></div>
<div class="pg"><span class="pl">サイズ</span><button class="tb" data-s="big">大</button><button class="tb on" data-s="">普通</button><button class="tb" data-s="small">小</button></div>
<div class="pg"><span class="pl"></span><button id="anon" class="tb" title="184(匿名)で投稿">匿名(184)</button><button id="db" class="db" title="白・流れる・普通に戻す">リセット</button></div>
</div>
<div id="pv"><div id="pt" class="naka">コメント</div></div>
</div>
<div id="post"><button id="cb" title="コマンド選択">▷</button><input id="pi" type="text" maxlength="75" placeholder="コメントを投稿 (Enterで送信)"><span id="pr"></span><button id="lb" title="ニコニコログイン">設定</button></div>
<script>
let ch=[],sc=2,sa=false,sid=null;
function fc(v){return v<=0?'#808080':v<=50?'#008000':v<=100?'#0080FF':v<=200?'#FF8000':'#FF0000'}
function srt(c){sc===c?sa=!sa:(sc=c,sa=c!==2);render();window.chrome.webview.postMessage({cmd:'sortChanged',col:sc,asc:sa});}
function render(){
  const s=[...ch].sort((a,b)=>{
    const d=sc===0?a.id-b.id:sc===2?a.force-b.force:sc===1?(a.name||'').localeCompare(b.name||''):(a.programTitle||'').localeCompare(b.programTitle||'');
    return sa?d:-d;
  });
  for(let i=0;i<4;i++)document.getElementById('a'+i).textContent=i===sc?(sa?'▲':'▼'):'';
  const tb=document.getElementById('tb');
  tb.innerHTML='';
  s.forEach(c=>{
    const col=fc(c.force),tr=document.createElement('tr');
    if(c.id===sid)tr.className='sel';
    tr.innerHTML='<td style="color:'+col+'">'+( c.video||'')+'</td><td>'+(c.name||'')+'</td>'
      +'<td style="color:'+col+'">'+(c.force>=0?c.force:'???')+'</td><td class="pe">'+(c.programTitle||'')+'</td>';
    tr.addEventListener('click',()=>{sid=c.id;render();window.chrome.webview.postMessage({cmd:'select',id:c.id});});
    tb.appendChild(tr);
  });
}
let prTimer=null;
function showResult(status,msg){
  const pr=document.getElementById('pr');
  pr.textContent=msg;pr.className=status;
  clearTimeout(prTimer);
  prTimer=setTimeout(()=>{pr.textContent='';pr.className='';},4000);
}
const pi=document.getElementById('pi');
let mcol='',mpos='',msz='',manon=false;
pi.addEventListener('keydown',e=>{
  if(e.key==='Enter'&&!e.isComposing){
    const t=pi.value;
    if(t.trim()!==''){
      const mail=[manon?'184':'',mcol,mpos,msz].filter(Boolean).join(' ');
      window.chrome.webview.postMessage({cmd:'post',text:t,mail});
      pi.value='';
    }
    e.preventDefault();
  }
});
const $=id=>document.getElementById(id);
// コマンド選択(色/位置/サイズ) ボタン
(function(){
  let open=false;
  const TR={'':{'':'▷','big':'▶','small':'▹'},'ue':{'':'△','big':'▲','small':'▵'},'shita':{'':'▽','big':'▼','small':'▿'}};
  const CL={'':'','red':'#d00','pink':'#e88','orange':'#e70','yellow':'#b90','green':'#090','cyan':'#088','blue':'#00b','purple':'#707','black':'#555'};
  const pop=$('cmdpop'),cb=$('cb');
  function upd(){
    cb.textContent=TR[mpos][msz];const col=CL[mcol];
    cb.style.color=col||'';cb.style.borderColor=col?col+'99':'';
    const pt=$('pt');pt.style.color=col||'var(--fg)';pt.className=(mpos||'naka')+(msz?' '+msz:'');
  }
  function close(){if(open){open=false;cb.classList.remove('open');pop.hidden=true;}}
  cb.addEventListener('mousedown',e=>e.preventDefault());
  cb.addEventListener('click',()=>{open=!open;cb.classList.toggle('open',open);pop.hidden=!open;});
  pop.addEventListener('mousedown',e=>{e.preventDefault();e.stopPropagation();});
  pi.addEventListener('focus',close);
  $('w').addEventListener('mousedown',close);
  const cc=[...pop.querySelectorAll('[data-c]')];
  cc.forEach(b=>b.addEventListener('click',()=>{mcol=b.dataset.c;cc.forEach(x=>x.classList.toggle('on',x.dataset.c===mcol));upd();}));
  const pb=[...pop.querySelectorAll('[data-p]')];
  pb.forEach(b=>b.addEventListener('click',()=>{mpos=b.dataset.p;pb.forEach(x=>x.classList.toggle('on',x.dataset.p===mpos));upd();}));
  const sb=[...pop.querySelectorAll('[data-s]')];
  sb.forEach(b=>b.addEventListener('click',()=>{msz=b.dataset.s;sb.forEach(x=>x.classList.toggle('on',x.dataset.s===msz));upd();}));
  const an=$('anon');
  an.addEventListener('click',()=>{manon=!manon;an.classList.toggle('on',manon);});
  $('db').addEventListener('click',()=>{mcol='';mpos='';msz='';manon=false;an.classList.remove('on');cc.forEach(x=>x.classList.toggle('on',x.dataset.c===''));pb.forEach(x=>x.classList.toggle('on',x.dataset.p===''));sb.forEach(x=>x.classList.toggle('on',x.dataset.s===''));upd();});
  upd();
})();
let authKnown=false;
function pickFg(hex){
  let h=(hex||'').replace('#','');
  if(h.length===3)h=h.split('').map(c=>c+c).join('');
  const r=parseInt(h.substr(0,2),16)||0,g=parseInt(h.substr(2,2),16)||0,b=parseInt(h.substr(4,2),16)||0;
  return (r*299+g*587+b*114)/1000>=160?'#000':'#fff';
}
function setAuth(loggedIn,connected,mail,boxColor){
  authKnown=true;
  // NicoJK流: 未接続なら投稿欄を隠す
  pi.style.display=connected?'':'none';
  if(connected){
    pi.disabled=!loggedIn;
    if(loggedIn&&boxColor){
      pi.style.background=boxColor;pi.style.color=pickFg(boxColor);pi.style.borderColor=boxColor;
    }else{
      pi.style.background='';pi.style.color='';pi.style.borderColor='';
    }
    pi.placeholder=loggedIn?'コメントを投稿 (Enterで送信)':'ログインが必要です';
  }
  const lb=$('lb');
  lb.classList.toggle('authin',loggedIn);
  lb.title=loggedIn?('ログイン中'+(mail?': '+mail:'')):'ニコニコログイン';
}
// 状態が分かるまで投稿欄は隠す
pi.style.display='none';
function setLogin(s,msg){
  const ls=$('ls');ls.textContent=msg||'';
  ls.className=(s==='success'||s==='failure')?s:'';
  $('otprow').hidden=(s!=='need2fa');
  if(s==='need2fa')$('lo').focus();
  if(s==='success'||s==='failure')$('lo').value='';
  // 成功したらフォームを自動的に畳む(メッセージを少し見せてから)
  if(s==='success')setTimeout(()=>{$('login').hidden=true;},2000);
}
$('lb').addEventListener('click',()=>{$('login').hidden=!$('login').hidden;});
$('ldo').addEventListener('click',()=>{
  const mail=$('lm').value.trim(),password=$('lp').value;
  if(mail===''||password===''){setLogin('failure','メールとパスワードを入力してください。');return;}
  setLogin('progress','');window.chrome.webview.postMessage({cmd:'login',mail,password});
});
$('losend').addEventListener('click',()=>{
  const otp=$('lo').value.trim();
  if(otp!=='')window.chrome.webview.postMessage({cmd:'loginOtp',otp});
});
$('lo').addEventListener('keydown',e=>{if(e.key==='Enter'&&!e.isComposing){$('losend').click();e.preventDefault();}});
$('lclear').addEventListener('click',()=>{window.chrome.webview.postMessage({cmd:'loginClear'});});
$('lcancel').addEventListener('click',()=>{window.chrome.webview.postMessage({cmd:'loginCancel'});});
// コメントログ一覧 / タブ切替
const logEl=$('log');
let atBottom=true;
logEl.addEventListener('scroll',()=>{atBottom=logEl.scrollTop+logEl.clientHeight>=logEl.scrollHeight-8;});
function showTab(f){
  $('tabF').classList.toggle('on',f);$('tabL').classList.toggle('on',!f);
  $('w').hidden=!f;logEl.hidden=f;
  if(!f&&atBottom)logEl.scrollTop=logEl.scrollHeight;
}
$('tabF').addEventListener('click',()=>showTab(true));
$('tabL').addEventListener('click',()=>showTab(false));
let ngUsers=new Set();
function applyNg(e){
  const u=e.dataset.u;
  const ng=e.dataset.nb==='1'||(u&&ngUsers.has(u));
  e.classList.toggle('ng',ng);
}
function logAdd(items){
  const frag=document.createDocumentFragment();
  (items||[]).forEach(d=>{
    const e=document.createElement('div');e.className='le'+(d.past?' past':'');
    if(d.userId)e.dataset.u=d.userId;
    e.dataset.nb=d.nb?'1':'0';applyNg(e);
    const tm=new Date((d.date||0)*1000);
    const lt=document.createElement('span');lt.className='lt';
    lt.textContent=('0'+tm.getHours()).slice(-2)+':'+('0'+tm.getMinutes()).slice(-2)+':'+('0'+tm.getSeconds()).slice(-2);
    const lm=document.createElement('span');lm.className='lm '+(d.refuge?'rf':'nc');
    lm.textContent=(d.userId||'').substring(0,3);
    if(d.userId)lm.title=d.userId;
    const lx=document.createElement('span');lx.className='lx';
    if(d.color)lx.style.color=d.color;lx.textContent=d.text;
    e.append(lt,lm,lx);frag.appendChild(e);
  });
  logEl.appendChild(frag);
  while(logEl.childElementCount>500)logEl.removeChild(logEl.firstChild);
  if(!logEl.hidden&&atBottom)logEl.scrollTop=logEl.scrollHeight;
}
function setNgUsers(list){
  ngUsers=new Set(list||[]);
  [...logEl.children].forEach(applyNg);
}
logEl.addEventListener('contextmenu',e=>{
  e.preventDefault();const it=e.target.closest('.le');if(!it||!it.dataset.u)return;
  const u=it.dataset.u;
  if(ngUsers.has(u)){
    if(confirm('このユーザのNGを解除しますか?'))window.chrome.webview.postMessage({cmd:'unNgUser',userId:u});
  }else{
    if(confirm('このユーザをNGに追加しますか?'))window.chrome.webview.postMessage({cmd:'ngUser',userId:u});
  }
});
function _update(m){
  if(m.type==='channelsUpdate'){ch=m.channels;render();}
  else if(m.type==='sortConfig'){sc=m.col;sa=m.asc;render();}
  else if(m.type==='commentLog'){logAdd(m.items);}
  else if(m.type==='ngUsers'){setNgUsers(m.users);}
  else if(m.type==='postResult'){showResult(m.status,m.message);}
  else if(m.type==='loginStatus'){setLogin(m.state,m.message);}
  else if(m.type==='authState'){setAuth(m.loggedIn,m.connected,m.mail,m.boxColor);}
  else if(m.type==='thm'){
    const s=document.documentElement.style;
    s.setProperty('--bg',m.bg);s.setProperty('--fg',m.fg);
    if(m.sb)s.setProperty('--sb',m.sb);
    document.body.style.background=m.bg;
  }
}
</script></body></html>)HTML";

static std::string colorToHex(COLORREF c)
{
    char buf[8];
    sprintf_s(buf, "#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

/*static*/ INT_PTR CALLBACK CDataBroadcastingWV2::PanelMomentumDlgProc(
    HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData)
{
    auto pThis = static_cast<CDataBroadcastingWV2*>(pClientData);
    switch (uMsg)
    {
    case WM_INITDIALOG:
        pThis->panelBackColor = pThis->m_pApp->GetColor(L"PanelBack");
        pThis->panelTextColor = pThis->m_pApp->GetColor(L"PanelText");
        pThis->InitMomentumWebView(hDlg);
        return TRUE;
    case WM_SIZE:
        if (pThis->momentumWebViewController)
        {
            RECT rc;
            GetClientRect(hDlg, &rc);
            pThis->momentumWebViewController->put_Bounds(rc);
        }
        return TRUE;
    case WM_DESTROY:
        pThis->momentumWebViewReady = false;
        pThis->momentumWebView = nullptr;
        if (pThis->momentumWebViewController)
        {
            pThis->momentumWebViewController->Close();
            pThis->momentumWebViewController = nullptr;
        }
        pThis->hMomentumPanel = nullptr;
        return TRUE;
    }
    return FALSE;
}

void CDataBroadcastingWV2::InitMomentumWebView(HWND hwnd)
{
    if (this->webViewEnv)
        this->CreateMomentumWebViewController(hwnd);
    else
        this->momentumWebViewPending = true; // データ放送 WebView2 の環境作成後に起動
}

void CDataBroadcastingWV2::CreateMomentumWebViewController(HWND hwnd)
{
    this->webViewEnv->CreateCoreWebView2Controller(hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this, hwnd](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
                if (FAILED(hr) || !IsWindow(hwnd)) return S_OK;

                this->momentumWebViewController = ctrl;
                ctrl->get_CoreWebView2(this->momentumWebView.put());

                wil::com_ptr<ICoreWebView2Settings> settings;
                this->momentumWebView->get_Settings(settings.put());
                settings->put_IsScriptEnabled(TRUE);
                settings->put_AreDefaultContextMenusEnabled(TRUE);
                settings->put_IsStatusBarEnabled(FALSE);
                settings->put_AreDevToolsEnabled(TRUE);

                RECT rc; GetClientRect(hwnd, &rc);
                ctrl->put_Bounds(rc);
                ctrl->put_IsVisible(TRUE);

                EventRegistrationToken token;
                this->momentumWebView->add_WebMessageReceived(
                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            wil::unique_cotaskmem_string msg;
                            args->get_WebMessageAsJson(msg.put());
                            try {
                                // Proper wide->UTF-8 (naive truncation would corrupt
                                // Japanese in posted comments / credentials).
                                auto j = nlohmann::json::parse(wstrToUTF8String(msg.get()));
                                auto cmd = j["cmd"].get<std::string>();
                                if (cmd == "select")
                                    this->SwitchToMomentumChannelById(j["id"].get<int>());
                                else if (cmd == "post")
                                {
                                    std::string text = j["text"].get<std::string>();
                                    std::string mail = j.value("mail", "");
                                    // Reuse PostComment's "[mail]text" parsing.
                                    std::string input = mail.empty() ? text : ("[" + mail + "]" + text);
                                    this->PostComment(utf8StrToWString(input.c_str()));
                                }
                                else if (cmd == "login")
                                    this->m_jkcnslLogin.Login(this->GetJkcnslPath(),
                                        j["mail"].get<std::string>(), j["password"].get<std::string>());
                                else if (cmd == "loginOtp")
                                    this->m_jkcnslLogin.SubmitOtp(j["otp"].get<std::string>());
                                else if (cmd == "loginClear")
                                    this->m_jkcnslLogin.ClearLogin(this->GetJkcnslPath());
                                else if (cmd == "loginCancel")
                                    this->m_jkcnslLogin.Cancel();
                                else if (cmd == "sortChanged")
                                {
                                    this->SetIniItem(L"MomentumSortColumn", std::to_wstring(j["col"].get<int>()).c_str());
                                    this->SetIniItem(L"MomentumSortAscending", j["asc"].get<bool>() ? L"1" : L"0");
                                }
                                else if (cmd == "ngUser")
                                {
                                    auto& v = j["userId"];
                                    if (v.is_string() && this->m_commentNg.AddUser(v.get<std::string>()))
                                        this->PushNgUsers();
                                }
                                else if (cmd == "unNgUser")
                                {
                                    auto& v = j["userId"];
                                    if (v.is_string() && this->m_commentNg.RemoveUser(v.get<std::string>()))
                                        this->PushNgUsers();
                                }
                            } catch (...) {}
                            return S_OK;
                        }
                    ).Get(), &token);

                this->momentumWebView->add_NavigationCompleted(
                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                            this->momentumWebViewReady = true;
                            this->SendMomentumTheme();
                            // Restore the saved sort order before sending channels.
                            {
                                int col = this->GetIniItem(L"MomentumSortColumn", 2);
                                bool asc = this->GetIniItem(L"MomentumSortAscending", 0) != 0;
                                nlohmann::json sj{ { "type", "sortConfig" }, { "col", col }, { "asc", asc } };
                                std::string script = "_update(" + sj.dump() + ")";
                                this->momentumWebView->ExecuteScript(utf8StrToWString(script.c_str()).c_str(), nullptr);
                            }
                            this->SendMomentumChannels();
                            this->PushAuthState();
                            this->PushNgUsers();
                            return S_OK;
                        }
                    ).Get(), &token);

                this->momentumWebView->NavigateToString(kMomentumHtml);
                return S_OK;
            }
        ).Get());
}

void CDataBroadcastingWV2::SendMomentumTheme()
{
    if (!this->momentumWebView || !this->momentumWebViewReady) return;
    COLORREF bg = this->panelBackColor, fg = this->panelTextColor;
    COLORREF sb = RGB((GetRValue(bg)+GetRValue(fg))/2, (GetGValue(bg)+GetGValue(fg))/2, (GetBValue(bg)+GetBValue(fg))/2);
    nlohmann::json msg{{"type","thm"},{"bg",colorToHex(bg)},{"fg",colorToHex(fg)},{"sb",colorToHex(sb)}};
    std::string script = "_update(" + msg.dump() + ")";
    this->momentumWebView->ExecuteScript(utf8StrToWString(script.c_str()).c_str(), nullptr);
}

void CDataBroadcastingWV2::SendMomentumChannels()
{
    if (!this->momentumWebView || !this->momentumWebViewReady) return;
    nlohmann::json channels = nlohmann::json::array();
    for (const auto& ch : this->momentumChannels)
    {
        channels.push_back({
            {"id", ch.id}, {"name", ch.name}, {"video", ch.video}, {"force", ch.force},
            {"programTitle", ch.programTitle.empty() ? nlohmann::json(nullptr) : nlohmann::json(ch.programTitle)},
        });
    }
    nlohmann::json msg{{"type","channelsUpdate"},{"channels",channels}};
    std::string script = "_update(" + msg.dump() + ")";
    this->momentumWebView->ExecuteScript(utf8StrToWString(script.c_str()).c_str(), nullptr);
}

void CDataBroadcastingWV2::SwitchToMomentumChannelById(int id)
{
    for (int i = 0; i < (int)this->momentumChannels.size(); i++)
    {
        if (this->momentumChannels[i].id == id)
        {
            this->SwitchToMomentumChannel(i);
            return;
        }
    }
}

void CDataBroadcastingWV2::SwitchToMomentumChannel(int index)
{
    if (index < 0 || index >= (int)this->momentumChannels.size()) return;
    const auto& target = this->momentumChannels[index];
    const std::string& targetJk = target.video;
    if (targetJk.empty()) return;

    // Step 1: 全チューニングスペースを走査（NicoJKと同方式）
    int spaceNum = 0;
    this->m_pApp->GetTuningSpace(&spaceNum);

    for (int space = 0; space < spaceNum; space++)
    {
        for (int channel = 0; ; channel++)
        {
            TVTest::ChannelInfo info = {};
            info.Size = sizeof(info);
            if (!this->m_pApp->GetChannelInfo(space, channel, &info)) break;

            if (this->DetectJkChannelFor(info.NetworkID, info.ServiceID) == targetJk)
            {
                TVTest::ChannelSelectInfo sel = {};
                sel.Size      = sizeof(sel);
                sel.Flags     = TVTest::CHANNEL_SELECT_FLAG_STRICTSERVICE;
                sel.Channel   = -1;
                // 地上波はSpaceで指定、BS/CSはNetworkIDで指定（NicoJKと同方式）
                if (0x7880 <= info.NetworkID && info.NetworkID <= 0x7FEF) {
                    sel.Space = space;
                } else {
                    sel.Space     = -1;
                    sel.NetworkID = info.NetworkID;
                }
                sel.ServiceID = info.ServiceID;
                this->m_pApp->SelectChannel(&sel);
                return;
            }
        }
    }

    // Step 2: 同一BonDriverに見つからない場合のフォールバック
    // テーブルのNetworkID+ServiceIDでTVTestのチャンネルDBを検索（BS↔地上波跨ぎ）
    if (targetJk.size() > 2 && targetJk[0] == 'j' && targetJk[1] == 'k')
    {
        int jkNum = std::stoi(targetJk.substr(2));
        for (int j = 0; j < (int)std::size(DEFAULT_NTSID_TABLE); j++)
        {
            if ((DEFAULT_NTSID_TABLE[j].jkID & ~0x40000000) != jkNum) continue;

            WORD netCat   = DEFAULT_NTSID_TABLE[j].ntsID & 0xFFFF;
            WORD serviceId = (WORD)(DEFAULT_NTSID_TABLE[j].ntsID >> 16);
            WORD networkId = (netCat == 0x0004) ? 4 : 0;

            TVTest::ChannelSelectInfo sel = {};
            sel.Size      = sizeof(sel);
            sel.Flags     = TVTest::CHANNEL_SELECT_FLAG_STRICTSERVICE;
            sel.NetworkID = networkId;
            sel.ServiceID = serviceId;
            sel.Space     = -1;
            sel.Channel   = -1;
            if (this->m_pApp->SelectChannel(&sel)) return;
        }
    }
}

bool CDataBroadcastingWV2::OnVolumeChange(int Volume, bool fMute)
{
    this->currentVolume = fMute ? 0 : Volume;
    this->UpdateVolume();
    return true;
}

void CDataBroadcastingWV2::UpdateAudioStream()
{
    if (this->isPlayingMainAudio)
    {
        this->mainAudio = this->GetSelectedAudio();
    }
    if (!this->webView)
    {
        return;
    }
    nlohmann::json msg{ { "type", "mainAudioStreamChanged" } };
    if (this->mainAudio.componentId.has_value())
    {
        msg["componentId"] = this->mainAudio.componentId.value();
    }
    else if (this->mainAudio.index.has_value())
    {
        auto index = this->mainAudio.index.value();
        msg["index"] = index;
        if (index >= 0 && _countof(this->currentService.AudioPID) > index && this->currentService.NumAudioPIDs > index)
        {
            msg.push_back({ "pid", this->currentService.AudioPID[index] });
        }
    }
    else
    {
        return;
    }
    if (this->mainAudio.getChannelId().has_value())
    {
        msg["channelId"] = this->mainAudio.getChannelId().value();
    }
    std::stringstream ss;
    ss << msg;
    auto wjson = utf8StrToWString(ss.str().c_str());
    this->webView->PostWebMessageAsJson(wjson.c_str());
}

// Index=1からサービスを変更したとしてもOnAudioStreamChangeは呼ばれない
// ただしOnStereoModeChangeは複数回呼ばれるので問題なさそう

bool CDataBroadcastingWV2::OnAudioStreamChange(int Stream)
{
    UNREFERENCED_PARAMETER(Stream);
    this->UpdateAudioStream();
    return true;
}


bool CDataBroadcastingWV2::OnStereoModeChange(int StereoMode)
{
    UNREFERENCED_PARAMETER(StereoMode);
    this->UpdateAudioStream();
    return true;
}

void CDataBroadcastingWV2::OnAudioFormatChange()
{
    this->UpdateAudioStream();
}

void CDataBroadcastingWV2::RestoreMainAudio()
{
    if (this->isPlayingMainAudio)
    {
        return;
    }
    this->isPlayingMainAudio = true;
    this->SelectAudio(this->mainAudio);
}

void CDataBroadcastingWV2::SelectAudio(Audio audio)
{
    if (audio.componentId.has_value())
    {
        TVTest::AudioSelectInfo asi = { sizeof(asi) };
        asi.ComponentTag = audio.componentId.value();
        asi.Flags = TVTest::AUDIO_SELECT_FLAG_COMPONENT_TAG;
        if (audio.dualMonoChannel.has_value())
        {
            asi.Flags |= TVTest::AUDIO_SELECT_FLAG_DUAL_MONO;
            asi.DualMono = audio.dualMonoChannel.value();
        }
        this->m_pApp->SelectAudio(&asi);
    }
    else if (audio.index.has_value())
    {
        this->m_pApp->SetAudioStream(audio.index.value());
    }
}

Audio CDataBroadcastingWV2::GetSelectedAudio()
{
    TVTest::AudioSelectInfo asi = { sizeof(asi) };
    if (this->m_pApp->GetSelectedAudio(&asi))
    {
        if (asi.Index == -1)
        {
            return Audio();
        }
        return Audio(asi.ComponentTag, asi.DualMono);
    }
    auto index = this->m_pApp->GetAudioStream();
    return Audio(index);
}

void CDataBroadcastingWV2::CreateOneSegWindow()
{
    if (this->hOneSegWnd != nullptr)
    {
        return;
    }
    this->oneSegWindowIsShown = true;
    this->oneSegWindow = std::make_unique<OneSegWindow>(
        this->m_pApp->GetAppWindow(),
        g_hinstDLL,
        this->hContainerWnd,
        this->webViewController,
        [this]()
        {
            this->oneSegWindowIsShown = false;
            this->webViewController->put_ParentWindow(this->hContainerWnd);
            PostMessageW(this->hMessageWnd, WM_APP_RESIZE, 0, 0);
            this->hOneSegWnd = nullptr;
            if (this->currentServiceIsOneSeg)
            {
                this->webView->Reload();
            }
            this->UpdateDigitButton();
        }
    );
    this->hOneSegWnd = this->oneSegWindow->GetWindowHandle();
    this->OnDarkModeChanged(this->m_pApp->GetDarkModeStatus() & TVTest::DARK_MODE_STATUS_DIALOG_DARK);
    this->UpdateDigitButton();
}

void CDataBroadcastingWV2::DestroyOneSegWindow()
{
    this->oneSegWindow = nullptr;
}

void CDataBroadcastingWV2::OnDarkModeChanged(bool fDarkMode)
{
    if (this->hOneSegWnd != nullptr)
    {
        this->m_pApp->SetWindowDarkMode(this->hOneSegWnd, this->m_pApp->GetDarkModeStatus() & TVTest::DARK_MODE_STATUS_DIALOG_DARK);
    }
    this->OnColorChange();
}

void CDataBroadcastingWV2::UpdateDigitButton()
{
    if (this->oneSegWindowIsShown)
    {
        SetDlgItemTextW(this->hRemoteWnd, IDC_KEY_10, L"＊/10");
        SetDlgItemTextW(this->hRemoteWnd, IDC_KEY_12, L"＃/12");
        SetDlgItemTextW(this->hPanelWnd, IDC_KEY_10, L"＊/10");
        SetDlgItemTextW(this->hPanelWnd, IDC_KEY_12, L"＃/12");
    }
    else
    {
        SetDlgItemTextW(this->hRemoteWnd, IDC_KEY_10, L"10");
        SetDlgItemTextW(this->hRemoteWnd, IDC_KEY_12, L"12");
        SetDlgItemTextW(this->hPanelWnd, IDC_KEY_10, L"10");
        SetDlgItemTextW(this->hPanelWnd, IDC_KEY_12, L"12");
    }
}

TVTest::CTVTestPlugin* CreatePluginClass()
{
    return new CDataBroadcastingWV2;
}
