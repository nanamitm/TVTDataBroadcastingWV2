// Run this executable without arguments. It also acts as a fake jkcnsl child
// when JkcnslReader launches it with -p, so no network or login is required.
#include "JkcnslReader.h"
#include "AsyncLifetime.h"
#include <condition_variable>
#include <iostream>
#include <optional>
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

static void CheckQueuedCommentsAfterSwitch(const std::wstring& executable)
{
    struct Notification { AsyncLifetime::Ticket ticket; std::vector<Comment> comments; };
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Notification> pending;
    AsyncLifetime lifetime;
    JkcnslReader reader;
    auto start = [&] {
        lifetime.Begin();
        const auto ticket = lifetime.GetTicket();
        reader.SetCallback([&, ticket](std::vector<Comment> comments) {
            std::lock_guard<std::mutex> lock(mutex);
            pending.push_back({ ticket, std::move(comments) });
            changed.notify_one();
        });
        if (!reader.Start(executable, "Lcomment")) throw std::runtime_error("Start failed");
    };
    auto wait = [&](size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return pending.size() >= count; }))
            throw std::runtime_error("Comment notification timed out");
    };
    start();
    wait(1);
    lifetime.Cancel(); // UI switches channel before consuming the queued comment.
    reader.Stop();
    start();
    wait(2);
    reader.Stop();
    if (AsyncLifetime::IsCurrent(pending[0].ticket) || !AsyncLifetime::IsCurrent(pending[1].ticket))
        throw std::runtime_error("A queued comment from the previous stream was accepted");
    lifetime.Cancel(); // Playback or plugin disable must reject even the latest queued comment.
    if (AsyncLifetime::IsCurrent(pending[1].ticket))
        throw std::runtime_error("A queued live comment survived stream teardown");
}

// つながらないままウォッチドッグが20秒ごとに張り直しても、実況ログの状態行は増えない。
static void CheckStatusLogThrottling()
{
    std::vector<Status> logged;
    std::optional<Status> last;
    auto feed = [&](Status status) {
        if (!JkcnslReader::ShouldLogStatus(last, status)) return;
        last = status;
        logged.push_back(status);
    };
    for (int retry = 0; retry < 10; ++retry) { feed(Status::Connecting); feed(Status::Failed); }
    if (logged != std::vector<Status>{ Status::Connecting, Status::Failed })
        throw std::runtime_error("Repeated connection failures kept filling the log");
    // 復帰と、その後の明示的な切断は状態が変わるので残す。
    feed(Status::Connecting);
    feed(Status::Connected);
    feed(Status::Connected);
    feed(Status::Disconnected);
    if (logged != std::vector<Status>{ Status::Connecting, Status::Failed,
                                       Status::Connected, Status::Disconnected })
        throw std::runtime_error("A real connection change was dropped from the log");
    // 切断後の張り直しは、失敗待ちではないので接続開始を残す。
    feed(Status::Connecting);
    if (logged.size() != 5 || logged.back() != Status::Connecting)
        throw std::runtime_error("A reconnect after a clean disconnect was dropped");
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
            if (command == "Lcomment")
                std::cout << "-<chat date=\"101\" user_id=\"test\">comment</chat>\r\n" << std::flush;
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
        CheckQueuedCommentsAfterSwitch(path);
        CheckStatusLogThrottling();
        std::cout << "JkcnslReader status tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
