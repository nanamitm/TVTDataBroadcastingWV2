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
    ~Win32Bridge() { Stop(); }
    bool Start(HINSTANCE module, State state, Dispatch dispatch,
               const std::wstring& pipe = L"TVTDataBroadcastingRemote-v1") {
        return Start(module, std::move(state), std::move(dispatch), Control{}, pipe);
    }
    bool Start(HINSTANCE module, State state, Dispatch dispatch, Control control,
               const std::wstring& pipe = L"TVTDataBroadcastingRemote-v1") {
        Stop(); m_module = module; m_state = std::move(state); m_dispatch = std::move(dispatch);
        m_control = std::move(control);
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
        m_dispatch = {}; m_control = {}; m_state = {}; m_lastState = nullptr; m_lastRefresh = 0;
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
            bool dispatched = false;
            try {
                if (command.control.empty()) dispatched = m_dispatch(command.key);
                else if (m_control) { dispatched = m_control(command.control, command.value); controlled = true; }
            }
            catch (...) { OutputDebugStringW(L"[TVTRemote] Command dispatch failed\n"); }
            m_agent.Complete(command, dispatched);
        }
        // Report the new caption/volume state promptly instead of on the 1 s poll.
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
    nlohmann::json m_lastState;
    ULONGLONG m_lastRefresh = 0;
};
}
