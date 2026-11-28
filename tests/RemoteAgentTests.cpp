#include "../TVTDataBroadcastingWV2/thirdparty/json.hpp"
#include "../TVTDataBroadcastingWV2/remote/RemoteAgent.h"
#include <iostream>
#include <stdexcept>
#include <string>

static void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

// Reads one '\n'-terminated line from the server end, waiting up to 5 seconds.
static std::string ReadLine(HANDLE pipe)
{
    std::string line;
    const auto deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
        if (!available) { Sleep(10); continue; }
        char ch = 0; DWORD read = 0;
        if (!ReadFile(pipe, &ch, 1, &read, nullptr) || !read) break;
        if (ch == '\n') return line;
        line.push_back(ch);
    }
    return line;
}

int main()
{
    try {
        // A pipe served by this process runs as the current user and is accepted.
        const std::wstring name = L"TVTDataBroadcastingRemoteTest-" + std::to_wstring(GetCurrentProcessId());
        const std::wstring path = L"\\\\.\\pipe\\" + name;
        HANDLE server = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 16384, 16384, 0, nullptr);
        Check(server != INVALID_HANDLE_VALUE, "test pipe server could not be created");
        HANDLE client = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        Check(client != INVALID_HANDLE_VALUE, "test pipe client could not connect");
        Check(TvtRemote::Agent::ServerIsCurrentUser(client), "a pipe server running as the current user must be accepted");
        CloseHandle(client);
        DisconnectNamedPipe(server);

        // Handles that are not pipes have no server process to verify.
        HANDLE file = CreateFileW(L"NUL", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "NUL could not be opened");
        Check(!TvtRemote::Agent::ServerIsCurrentUser(file), "a non-pipe handle must be rejected");
        CloseHandle(file);

        // The agent still registers with a broker running as the same user.
        TvtRemote::Agent agent;
        agent.UpdateState("Test", "channel", true, "bml", "token");
        Check(agent.Start(name), "agent could not start");
        Check(ConnectNamedPipe(server, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED, "agent did not connect");
        const auto line = ReadLine(server);
        const auto message = nlohmann::json::parse(line, nullptr, false);
        Check(message.is_object() && message.value("type", "") == "register" && message.value("name", "") == "Test",
            "agent must register with a same-user broker");
        agent.Stop();
        CloseHandle(server);
        std::cout << "Remote agent pipe server identity tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
