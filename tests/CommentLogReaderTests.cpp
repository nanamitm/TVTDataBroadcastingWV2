#include "CommentLogReader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void Check(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

static std::string Chat(int date, const char* text)
{
    return "<chat date=\"" + std::to_string(date) + "\" user_id=\"test\">" + text + "</chat>";
}

static void Write(const std::filesystem::path& file, const std::string& text, bool append = false)
{
    std::ofstream out(file, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    out << text;
    Check(out.good(), "Writing the log fixture failed");
}

int main()
{
    try {
        wchar_t executable[32768];
        Check(GetModuleFileNameW(nullptr, executable, 32768) != 0, "Executable path failed");
        const auto folder = std::filesystem::path(executable).parent_path() /
            (L"log-reader-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(folder / L"jk1");
        const auto first = folder / L"jk1/0000000100.txt";
        Write(first, Chat(101, "first") + "\r\n");
        CommentLogReader reader;
        Check(reader.Configure(folder.wstring(), 1), "Configure failed");
        reader.Seek(100);
        Check(reader.Read(100, 101).size() == 1, "Initial comment missing");
        Write(first, Chat(102, "appended") + "\r\n", true);
        auto appended = reader.Read(101, 102);
        Check(appended.size() == 1 && appended[0].text == "appended", "Appended comment missing");
        Check(reader.Read(102, 103).empty(), "A consumed comment was duplicated");

        Write(first, "<chat date=\"104\" user_id=\"test\">par", true);
        Check(reader.Read(103, 104).empty(), "An unfinished line was emitted");
        Write(first, "tial</chat>\r\n", true);
        auto partial = reader.Read(103, 104);
        Check(partial.size() == 1 && partial[0].text == "partial", "A split line was lost");
        Write(first, Chat(105, "no newline"), true);
        Check(reader.Read(104, 105).size() == 1, "A complete final line needs no newline");
        Write(first, "\r\n" + Chat(106, "after newline") + "\r\n", true);
        Check(reader.Read(105, 106).size() == 1, "Newline completion duplicated or lost a comment");

        const auto second = folder / L"jk1/0000000107.txt";
        Write(second, Chat(108, "rotated") + "\r\n");
        Sleep(1100); // File discovery is intentionally limited to once per second.
        auto rotated = reader.Read(106, 108);
        Check(rotated.size() == 1 && rotated[0].text == "rotated", "A newly created log was not discovered");
        reader.Seek(100);
        Check(reader.Read(100, 106).size() == 5, "Backward seek lost or duplicated comments");

        CommentLogReader empty;
        Check(!empty.Configure(folder.wstring(), 2), "An empty channel unexpectedly has logs");
        std::filesystem::create_directories(folder / L"jk2");
        Write(folder / L"jk2/0000000200.txt", Chat(201, "created later") + "\n");
        Sleep(1100);
        Check(empty.Read(200, 201).size() == 1, "Logs created after Configure were ignored");

        // Exercise incremental reads and seeks across the 64KiB read boundary.
        std::filesystem::create_directories(folder / L"jk3");
        std::string large;
        for (int t = 1000; t < 5000; ++t) large += Chat(t, "chunked") + "\n";
        Write(folder / L"jk3/0000001000.txt", large);
        CommentLogReader chunked;
        Check(chunked.Configure(folder.wstring(), 3), "Chunked Configure failed");
        chunked.Seek(3500);
        Check(chunked.Read(3500, 4999).size() == 1499, "Chunked seek/read lost comments");
        chunked.Seek(999);
        Check(chunked.Read(999, 4999).size() == 4000, "Chunked read duplicated or lost comments");
        std::cout << "CommentLogReader incremental playback tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
