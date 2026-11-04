#pragma once
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 nanamitm
#include "RemoteAgent.h"
#include <functional>

namespace TvtRemote {
// Owns a separate message-only window so the remote bridge remains available
// when the data-broadcasting renderer is disabled. Construct/use on the UI thread.
class Win32Bridge {
public:
    using State = std::function<nlohmann::json()>;
    using Dispatch = std::function<bool(const std::string&)>;
    // Applies a control such as ("volume", 40). Return false when it cannot be
    // applied now. Advertise supported controls in the state's "controls" object.
    using Control = std::function<bool(const std::string& name, const nlohmann::json& value)>;
    // Returns the channel list (0.6.0) as an array of tuning spaces:
    // [{"space":0,"name":"UHF","channels":[{...}, ...]}, ...]. Spaces without
    // channels are omitted from the reply. Advertise "channel" in controls.
    using ChannelList = std::function<nlohmann::json()>;
    ~Win32Bridge() { Stop(); }
    bool Start(HINSTANCE module, State state, Dispatch dispatch,
               const std::wstring& pipe = L"TVTDataBroadcastingRemote-v1") {
        return Start(module, std::move(state), std::move(dispatch), Control{}, ChannelList{}, pipe);
    }
    bool Start(HINSTANCE module, State state, Dispatch dispatch, Control control,
               const std::wstring& pipe = L"TVTDataBroadcastingRemote-v1") {
        return Start(module, std::move(state), std::move(dispatch), std::move(control), ChannelList{}, pipe);
    }
    bool Start(HINSTANCE module, State state, Dispatch dispatch, Control control, ChannelList channels,
               const std::wstring& pipe = L"TVTDataBroadcastingRemote-v1") {
        Stop(); m_module = module; m_state = std::move(state); m_dispatch = std::move(dispatch);
        m_control = std::move(control); m_channels = std::move(channels);
        WNDCLASSW wc{}; wc.lpfnWndProc = WindowProc; wc.hInstance = module; wc.lpszClassName = ClassName;
        m_class = RegisterClassW(&wc);
        if (!m_class) { Stop(); return false; }
        m_window = CreateWindowExW(0, ClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, this);
        if (!m_window) { Stop(); return false; }
        Refresh();
        if (!m_agent.Start(pipe) || !SetTimer(m_window, 1, 50, nullptr)) { Stop(); return false; }
        return true;
    }
    void Stop() {
        if (m_window) KillTimer(m_window, 1);
        m_agent.Stop();
        if (m_window) { DestroyWindow(m_window); m_window = nullptr; }
        if (m_class) { UnregisterClassW(ClassName, m_module); m_class = 0; }
        m_dispatch = {}; m_control = {}; m_channels = {}; m_state = {}; m_lastState = nullptr; m_lastRefresh = 0;
    }
    // Splits tuning spaces into channelPage bodies that stay well below the
    // 16 KiB message limit. A channel that alone exceeds the budget is dropped.
    static std::vector<nlohmann::json> PaginateChannels(const nlohmann::json& spaces, std::size_t budget = 12000) {
        std::vector<nlohmann::json> pages;
        if (!spaces.is_array()) return pages;
        int order = 0;
        for (const auto& space : spaces) {
            if (!space.is_object() || !space.contains("channels") || !space["channels"].is_array() || space["channels"].empty()) continue;
            const nlohmann::json base{{"space", space.value("space", 0)}, {"order", order++},
                {"spaceName", space.value("name", std::string())}, {"channels", nlohmann::json::array()}};
            const auto overhead = base.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).size() + 128;
            auto page = base; std::size_t size = overhead;
            for (const auto& channel : space["channels"]) {
                const auto entry = channel.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).size() + 1;
                if (overhead + entry > budget) continue;
                if (size + entry > budget || page["channels"].size() >= 200) {
                    pages.push_back(std::move(page)); page = base; size = overhead;
                }
                page["channels"].push_back(channel); size += entry;
            }
            if (!page["channels"].empty()) pages.push_back(std::move(page));
        }
        return pages;
    }
private:
    void Refresh() {
        const auto state = m_state();
        if (state != m_lastState) {
            m_agent.UpdateState(state.value("name", "TVTest"), state.value("channel", ""),
                state.value("enabled", false), state.value("mode", "bml"), state.value("targetToken", ""),
                state.contains("controls") && state["controls"].is_object() ? state["controls"] : nlohmann::json());
            m_lastState = state;
        }
        m_lastRefresh = GetTickCount64();
    }
    void Tick() {
        if (GetTickCount64() - m_lastRefresh >= 1000) Refresh();
        Agent::Command command;
        bool controlled = false;
        for (int i = 0; i < 4 && m_agent.Next(command); ++i) {
            if (command.channels) {
                std::vector<nlohmann::json> pages;
                bool listed = false;
                try { if (m_channels) { pages = PaginateChannels(m_channels()); listed = true; } }
                catch (...) { OutputDebugStringW(L"[TVTRemote] Channel list failed\n"); }
                m_agent.Reply(command, std::move(pages), listed);
                continue;
            }
            bool dispatched = false;
            try {
                if (command.control.empty()) dispatched = m_dispatch(command.key);
                else if (m_control) { dispatched = m_control(command.control, command.value); controlled = true; }
            }
            catch (...) { OutputDebugStringW(L"[TVTRemote] Command dispatch failed\n"); }
            m_agent.Complete(command, dispatched);
        }
        // Report the new caption/volume/channel state promptly instead of on the 1 s poll.
        if (controlled) Refresh();
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* creation = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        auto* self = reinterpret_cast<Win32Bridge*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self && message == WM_TIMER && wparam == 1) {
            try { self->Tick(); } catch (...) { OutputDebugStringW(L"[TVTRemote] UI bridge failed\n"); }
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    inline static constexpr wchar_t ClassName[] = L"TVTDataBroadcastingRemote Plugin Bridge v1";
    HINSTANCE m_module = nullptr;
    ATOM m_class = 0;
    HWND m_window = nullptr;
    Agent m_agent;
    State m_state;
    Dispatch m_dispatch;
    Control m_control;
    ChannelList m_channels;
    nlohmann::json m_lastState;
    ULONGLONG m_lastRefresh = 0;
};
}
