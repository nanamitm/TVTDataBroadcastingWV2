// Run this executable without arguments. It also acts as a fake jkcnsl child
// when JkcnslReader launches it with -p, so no network or login is required.
#include "JkcnslReader.h"
#include <condition_variable>
#include <iostream>
#include <stdexcept>

using Status = JkcnslReader::Status;

static void CheckStream(const std::wstring& executable, const std::string& command,
                        const std::vector<Status>& expected, bool stopWhileConnected = false)
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Status> actual;
    JkcnslReader reader;
    reader.SetStatusCallback([&](Status status) {
        std::lock_guard<std::mutex> lock(mutex);
        actual.push_back(status);
        changed.notify_one();
    });
    if (!reader.Start(executable, command)) throw std::runtime_error("Start failed");
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(5), [&] {
            return actual.size() >= (stopWhileConnected ? 1 : expected.size());
        })) throw std::runtime_error("Status notification timed out");
    }
    reader.Stop();
    if (actual != expected) throw std::runtime_error("Unexpected or duplicate status notification");
    if (reader.IsConnected() || reader.IsStreamOpen()) throw std::runtime_error("Stream remained open");
}

int main(int argc, char**)
{
    if (argc > 1) {
        std::string command;
        std::getline(std::cin, command);
        if (!command.empty() && command.back() == '\r') command.pop_back();
        if (command == "Lfail") std::cout << "!connection failed\r\n" << std::flush;
        else if (command == "Lexit") return 0;
        else {
            // Repeated open/data markers must not create repeated connection logs.
            std::cout << "*opened\r\n*opened\r\n-data\r\n" << std::flush;
            if (command == "Lclose") std::cout << ".\r\n" << std::flush;
            if (command == "Lerror") std::cout << "!\r\n.\r\n" << std::flush;
        }
        while (std::getline(std::cin, command)) {
            if (!command.empty() && command[0] == 'q') break;
        }
        return 0;
    }
    try {
        wchar_t path[32768];
        if (!GetModuleFileNameW(nullptr, path, 32768)) throw std::runtime_error("Executable path failed");
        CheckStream(path, "Lclose", { Status::Connected, Status::Disconnected });
        CheckStream(path, "Lfail", { Status::Failed });
        CheckStream(path, "Lerror", { Status::Connected, Status::Failed });
        CheckStream(path, "Lexit", { Status::Failed });
        CheckStream(path, "Lwait", { Status::Connected, Status::Disconnected }, true);
        std::cout << "JkcnslReader status tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
