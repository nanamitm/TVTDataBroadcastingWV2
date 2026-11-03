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

namespace TvtRemote {
class Agent {
public:
    struct Command { std::string request, key; std::uint64_t generation = 0; std::int64_t expires = 0; };
    Agent() = default;
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;
    ~Agent() { Stop(); }
    void UpdateState(std::string name, std::string channel, bool enabled, std::string mode) {
        std::lock_guard lock(m_mutex);
        m_state = {{"v", 1}, {"type", "state"}, {"name", std::move(name)},
            {"channel", std::move(channel)}, {"enabled", enabled}, {"mode", std::move(mode)},
            {"pid", GetCurrentProcessId()}};
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
private:
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
        if (type != "key" || !o.contains("request") || !o["request"].is_string()
            || !o.contains("key") || !o["key"].is_string() || !o.contains("expires") || !o["expires"].is_number()) return false;
        Command command;
        command.request = o["request"].get<std::string>(); command.key = o["key"].get<std::string>();
        command.expires = o["expires"].get<std::int64_t>();
        if (command.request.size() > 64 || command.key.size() > 16 || command.expires > Now() + 5000) return false;
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
