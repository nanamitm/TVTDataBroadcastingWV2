#pragma once
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 nanamitm
// Qt-free Windows plugin bridge. Include nlohmann/json.hpp (or the plugin's
// thirdparty/json.hpp) before this file. All TVTest calls remain on the UI thread.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace TvtRemote {
class Agent {
public:
    // A key command has `key` set. A control command (0.5.0+) has `control`
    // (e.g. "caption", "volume", "mute", "channel") and its `value` instead.
    // A channel-list request (0.6.0+) has `channels` set and is answered with
    // Reply(); since 0.7.0 `value` may hold the requested BonDriver file name.
    struct Command {
        std::string request, key, control; nlohmann::json value; bool channels = false;
        std::uint64_t generation = 0; std::int64_t expires = 0;
    };
    Agent() = default;
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;
    ~Agent() { Stop(); }
    // `controls` is an object of supported controls and their current values,
    // e.g. {"caption":true,"volume":40,"mute":false}. The broker forwards only
    // advertised controls, so omit it if the plugin cannot handle them.
    void UpdateState(std::string name, std::string channel, bool enabled, std::string mode,
                     std::string targetToken = {}, nlohmann::json controls = nullptr) {
        std::lock_guard lock(m_mutex);
        m_state = {{"v", 1}, {"type", "state"}, {"name", std::move(name)},
            {"channel", std::move(channel)}, {"enabled", enabled}, {"mode", std::move(mode)},
            {"pid", GetCurrentProcessId()}, {"targetToken", std::move(targetToken)}};
        if (controls.is_object()) m_state["controls"] = std::move(controls);
        ++m_stateVersion;
    }
    bool Start(const std::wstring& name = L"TVTDataBroadcastingRemote-v1") {
        Stop();
        m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_stop) return false;
        m_pipeName = L"\\\\.\\pipe\\" + name;
        try { m_thread = std::thread([this] { Run(); }); }
        catch (...) { CloseHandle(m_stop); m_stop = nullptr; return false; }
        return true;
    }
    void Stop() {
        if (m_stop) SetEvent(m_stop);
        if (m_thread.joinable()) m_thread.join();
        if (m_stop) { CloseHandle(m_stop); m_stop = nullptr; }
        std::lock_guard lock(m_mutex); m_commands.clear(); m_results.clear(); ++m_generation;
    }
    // Call on the UI thread. Expired/disconnected commands are discarded.
    bool Next(Command& command) {
        std::lock_guard lock(m_mutex);
        while (!m_commands.empty()) {
            command = std::move(m_commands.front()); m_commands.pop_front();
            if (command.generation == m_generation && command.expires > Now()) return true;
        }
        return false;
    }
    void Complete(const Command& command, bool dispatched) {
        std::lock_guard lock(m_mutex);
        if (command.generation != m_generation || m_results.size() >= 16) return;
        m_results.push_back({{"v", 1}, {"type", "result"}, {"request", command.request},
            {"status", dispatched ? "dispatched" : "unavailable"}});
    }
    // Sends `pages` (objects without v/type/request, each under 16 KiB once
    // encoded) for a request, followed by its result. Nothing is sent when the
    // pages would overflow the output queue.
    void Reply(const Command& command, std::vector<nlohmann::json> pages, bool dispatched) {
        std::lock_guard lock(m_mutex);
        if (command.generation != m_generation || m_results.size() + pages.size() + 1 > MaxResults) return;
        for (auto& page : pages) {
            page["v"] = 1; page["request"] = command.request;
            if (!page.contains("type")) page["type"] = "channelPage";
            m_results.push_back(std::move(page));
        }
        m_results.push_back({{"v", 1}, {"type", "result"}, {"request", command.request},
            {"status", dispatched ? "dispatched" : "unavailable"}});
    }
private:
    static constexpr std::size_t MaxResults = 64;
    using Json = nlohmann::json;
    static std::int64_t Now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    bool Stopped() const { return WaitForSingleObject(m_stop, 0) == WAIT_OBJECT_0; }
    bool Transfer(HANDLE pipe, bool writing, void* data, DWORD size, DWORD& transferred) {
        OVERLAPPED operation{}; operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return false;
        BOOL ok = writing ? WriteFile(pipe, data, size, &transferred, &operation)
                          : ReadFile(pipe, data, size, &transferred, &operation);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            HANDLE events[] = {m_stop, operation.hEvent};
            if (WaitForMultipleObjects(2, events, FALSE, 1000) == WAIT_OBJECT_0 + 1)
                ok = GetOverlappedResult(pipe, &operation, &transferred, FALSE);
            else { CancelIoEx(pipe, &operation); GetOverlappedResult(pipe, &operation, &transferred, TRUE); ok = FALSE; }
        }
        CloseHandle(operation.hEvent); return ok != FALSE;
    }
    bool Write(HANDLE pipe, const Json& object) {
        auto bytes = object.dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
        if (bytes.size() > 16384) return false;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD written = 0;
            if (!Transfer(pipe, true, bytes.data() + offset, DWORD(bytes.size() - offset), written) || !written) return false;
            offset += written;
        }
        return true;
    }
    bool Receive(const std::string& line) {
        const auto o = Json::parse(line, nullptr, false);
        if (!o.is_object() || !o.contains("v") || o["v"] != 1 || !o.contains("type") || !o["type"].is_string()) return false;
        const auto type = o["type"].get<std::string>();
        if (type == "registered") return true;
        const bool control = type == "control";
        const bool channels = type == "channels";
        if ((type != "key" && !control && !channels) || !o.contains("request") || !o["request"].is_string()
            || !o.contains("expires") || !o["expires"].is_number()) return false;
        Command command;
        if (channels) {
            command.channels = true;
            // Optional BonDriver file name whose list is requested (0.7.0).
            if (o.contains("driver")) {
                if (!o["driver"].is_string() || o["driver"].get<std::string>().size() > 512) return false;
                command.value = o["driver"];
            }
        } else if (control) {
            if (!o.contains("name") || !o["name"].is_string() || !o.contains("value")
                || !(o["value"].is_boolean() || o["value"].is_number_integer() || o["value"].is_object())) return false;
            command.control = o["name"].get<std::string>(); command.value = o["value"];
            if (command.control.empty() || command.control.size() > 32 || command.value.dump().size() > 512) return false;
        } else {
            if (!o.contains("key") || !o["key"].is_string()) return false;
            command.key = o["key"].get<std::string>();
            if (command.key.size() > 16) return false;
        }
        command.request = o["request"].get<std::string>();
        command.expires = o["expires"].get<std::int64_t>();
        if (command.request.size() > 64 || command.expires > Now() + 5000) return false;
        std::lock_guard lock(m_mutex);
        if (m_commands.size() >= 16) return false;
        command.generation = m_generation; m_commands.push_back(std::move(command)); return true;
    }
    void Session(HANDLE pipe) {
        std::uint64_t sentVersion;
        Json state;
        { std::lock_guard lock(m_mutex); state = m_state; sentVersion = m_stateVersion; }
        if (state.empty()) return;
        state["type"] = "register";
        if (!Write(pipe, state)) return;
        std::string buffer;
        auto lastHeartbeat = GetTickCount64();
        while (!Stopped()) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return;
            if (available) {
                char bytes[4096]; DWORD read = 0;
                if (!Transfer(pipe, false, bytes, std::min<DWORD>(available, sizeof(bytes)), read) || !read) return;
                buffer.append(bytes, read);
                for (auto end = buffer.find('\n'); end != std::string::npos; end = buffer.find('\n')) {
                    if (end > 16384 || !Receive(buffer.substr(0, end))) return;
                    buffer.erase(0, end + 1);
                }
                if (buffer.size() > 16384) return;
            }
            std::deque<Json> results;
            { std::lock_guard lock(m_mutex);
                results.swap(m_results);
                if (sentVersion != m_stateVersion) { state = m_state; sentVersion = m_stateVersion; }
                else state = Json();
            }
            if (!state.is_null() && !Write(pipe, state)) return;
            for (const auto& r : results) if (!Write(pipe, r)) return;
            if (GetTickCount64() - lastHeartbeat >= 5000) {
                if (!Write(pipe, {{"v", 1}, {"type", "heartbeat"}})) return;
                lastHeartbeat = GetTickCount64();
            }
            WaitForSingleObject(m_stop, 20);
        }
    }
    void Run() noexcept {
        while (!Stopped()) {
            HANDLE pipe = CreateFileW(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                try { Session(pipe); } catch (...) { /* disconnect malformed or failed sessions */ }
                CancelIoEx(pipe, nullptr); CloseHandle(pipe);
                { std::lock_guard lock(m_mutex); ++m_generation; m_commands.clear(); m_results.clear(); }
            }
            WaitForSingleObject(m_stop, 1000);
        }
    }
    HANDLE m_stop = nullptr;
    std::thread m_thread;
    std::wstring m_pipeName;
    std::mutex m_mutex;
    Json m_state;
    std::uint64_t m_stateVersion = 0, m_generation = 0;
    std::deque<Command> m_commands;
    std::deque<Json> m_results;
};
}
