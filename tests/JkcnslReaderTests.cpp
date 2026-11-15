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

// jkcnslが実体参照にした本文は、NicoJKと同じく元の文字に戻して流す。
static void CheckEntityDecoding()
{
    Comment c;
    if (!JkcnslReader::ParseChatLine(
            "-<chat date=\"1\" user_id=\"5ch:x\">&gt;&gt;38 a&amp;b &lt;c&gt; &quot;d&quot; &apos;e&apos;&#10;f&#13;</chat>", c))
        throw std::runtime_error("Entity chat line was not parsed");
    if (c.text != ">>38 a&b <c> \"d\" 'e'\nf\r")
        throw std::runtime_error("Entity references were not decoded");
    // 1回だけ戻す。&amp;gt; は &gt; のまま、知らない参照や裸の & は残す。
    if (JkcnslReader::DecodeEntityReference("&amp;gt; &nbsp; & &") != "&gt; &nbsp; & &")
        throw std::runtime_error("Entity references were decoded more than once");
    if (c.raw.find("&gt;&gt;38") == std::string::npos)
        throw std::runtime_error("Raw chat tag for the logfile must stay encoded");
}

// 混合接続では過去コメントの受信状態をニコニコと避難所で別々に持つ(NicoJK同様)。
// 片方が過去コメントを送っている間も、もう片方の新しいコメントは過去扱いにしない。
static void CheckPastChatTracking()
{
    PastChatTracker t;
    t.OnLine("-<x_past_chat_begin refuge=\"1\" />");
    if (!t.IsPast(true) || t.IsPast(false))
        throw std::runtime_error("Refuge backfill marked niconico comments as past");
    t.OnLine("-<x_past_chat_begin />");
    t.OnLine("-<x_past_chat_end refuge=\"1\" />");
    if (t.IsPast(true) || !t.IsPast(false))
        throw std::runtime_error("Ending refuge backfill ended niconico backfill");
    // 片方だけの切断でその接続先の状態を戻す
    t.OnLine("-<x_disconnect status=\"1\" />");
    if (t.IsPast(false)) throw std::runtime_error("x_disconnect did not reset niconico backfill");
    // chat本文に制御タグ名が含まれていても状態を変えない
    t.OnLine("-<chat date=\"1\">&lt;x_past_chat_begin&gt;</chat>");
    t.OnLine("-<chat date=\"1\"><x_past_chat_begin /></chat>");
    if (t.IsPast(false) || t.IsPast(true)) throw std::runtime_error("A chat body changed the backfill state");
    t.OnLine("-<x_past_chat_begin refuge=\"1\" />");
    t.Reset();
    if (t.IsPast(true)) throw std::runtime_error("Reset kept the backfill state");
}

// 接続先ごとの<x_room>/<x_disconnect>を、NicoJKの状態行に必要な情報まで読む。
static void CheckRoomLines()
{
    JkcnslReader::RoomEvent e;
    if (!JkcnslReader::ParseRoomLine("-<x_room refuge=\"1\" is_logged_in=\"1\" nickname=\"a&amp;b\" />", e) ||
        e.disconnect || !e.refuge || !e.loggedIn || e.nickname != "a&b")
        throw std::runtime_error("Refuge x_room was not parsed");
    if (!JkcnslReader::ParseRoomLine("-<x_room nickname=\"\" />", e) || e.refuge || e.loggedIn || !e.nickname.empty())
        throw std::runtime_error("niconico x_room was not parsed");
    if (!JkcnslReader::ParseRoomLine("-<x_disconnect status=\"2\" refuge=\"1\" />", e) ||
        !e.disconnect || !e.refuge || e.status != 2)
        throw std::runtime_error("x_disconnect was not parsed");
    // x_refuge属性やchat本文の文字列を接続先や制御タグと取り違えない
    if (!JkcnslReader::ParseRoomLine("-<x_room x_refuge=\"1\" />", e) || e.refuge)
        throw std::runtime_error("x_refuge was taken as refuge");
    if (JkcnslReader::ParseRoomLine("-<chat date=\"1\"><x_room refuge=\"1\" /></chat>", e) ||
        JkcnslReader::ParseRoomLine("-<x_roomy />", e) || JkcnslReader::ParseRoomLine("*opened", e))
        throw std::runtime_error("A non-room line was parsed as a room event");
}

// 子プロセス経由でも、各コメントの接続先に応じた過去フラグが付く。
static void CheckPastFlagsFromStream(const std::wstring& executable)
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Comment> got;
    JkcnslReader reader;
    reader.SetCallback([&](std::vector<Comment> comments) {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& c : comments) got.push_back(std::move(c));
        changed.notify_one();
    });
    if (!reader.Start(executable, "Lmixpast")) throw std::runtime_error("Start failed");
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return got.size() >= 3; }))
            throw std::runtime_error("Mixed backfill comments timed out");
    }
    reader.Stop();
    // refugeの過去コメント / その間に届いたnicoの新しいコメント / 終了後のrefugeの新しいコメント
    if (!(got[0].refuge && got[0].past) || got[1].refuge || got[1].past || !got[2].refuge || got[2].past)
        throw std::runtime_error("Past flags were not tracked per source");
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
            if (command == "Lmixpast")
                std::cout << "-<x_past_chat_begin refuge=\"1\" />\r\n"
                             "-<chat date=\"100\" user_id=\"5ch:a\" x_refuge=\"1\">old</chat>\r\n"
                             "-<chat date=\"101\" user_id=\"a:b\">live</chat>\r\n"
                             "-<x_past_chat_end refuge=\"1\" />\r\n"
                             "-<chat date=\"102\" user_id=\"5ch:c\" x_refuge=\"1\">new</chat>\r\n" << std::flush;
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
        CheckEntityDecoding();
        CheckPastChatTracking();
        CheckRoomLines();
        CheckPastFlagsFromStream(path);
        std::cout << "JkcnslReader status tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
