#include "CommentLogWriter.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

static void Check(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

static void MakeLog(const fs::path& path, size_t bytes, std::chrono::hours age)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << std::string(bytes, 'x');
    out.close();
    Check(out.good(), "Writing log fixture failed");
    fs::last_write_time(path, fs::file_time_type::clock::now() - age);
}

int main()
{
    try {
        const auto folder = fs::current_path() / L"work/safety-tests" /
            (L"log-writer-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        const auto age = folder / L"age";
        const auto old = age / L"jk1/1700000000.txt";
        const auto recent = age / L"jk2/1700000001.txt";
        const auto notes = age / L"jk1/notes.txt";
        const auto unrelated = age / L"jk-backup/1700000002.txt";
        MakeLog(old, 100, 72h);
        MakeLog(recent, 100, 1h);
        MakeLog(notes, 100, 72h);
        MakeLog(unrelated, 100, 72h);
        CommentLogWriter writer;
        writer.Configure(age.wstring(), false, { 1, 0, 0 });
        writer.Write(3, 1700000010, "disabled");
        Check(fs::exists(old) && !fs::exists(age / L"jk3"), "Disabled logging modified disk");
        writer.Configure(age.wstring(), true);
        writer.Write(3, 1700000010, "unlimited");
        Check(fs::exists(old), "Default settings pruned a log");
        writer.Configure(age.wstring(), true, { 1, 0, 0 });
        writer.Write(3, 1700000011, "retention");
        Check(!fs::exists(old), "Expired log was not removed");
        Check(fs::exists(recent) && fs::exists(notes) && fs::exists(unrelated), "Retention removed an unrelated or new file");

        const auto total = folder / L"total";
        const auto oldest = total / L"jk1/1700000000.txt";
        const auto newest = total / L"jk2/1700000001.txt";
        MakeLog(oldest, 700 * 1024, 3h);
        MakeLog(newest, 700 * 1024, 1h);
        writer.Configure(total.wstring(), true, { 0, 1, 0 });
        writer.Write(3, 1700000010, "prune");
        Check(!fs::exists(oldest) && fs::exists(newest), "Total capacity did not remove the oldest log first");

        const auto rotation = folder / L"rotation";
        const std::string line(600 * 1024, 'x');
        const auto first = rotation / L"jk1/1700000000.txt";
        const auto second = rotation / L"jk1/1700000001.txt";
        writer.Configure(rotation.wstring(), true, { 0, 0, 1 });
        writer.Write(1, 1700000000, line);
        writer.Write(1, 1700000000, line);
        writer.Write(1, 1700000001, line);
        writer.Close();
        Check(fs::file_size(first) > 1024 * 1024, "Same-second comments could not be appended");
        Check(fs::file_size(second) < 1024 * 1024, "Next-second comment did not rotate");

        const auto rollingTotal = folder / L"rolling-total";
        writer.Configure(rollingTotal.wstring(), true, { 0, 1, 0 });
        writer.Write(1, 1700000000, line);
        writer.Write(1, 1700000001, line);
        Check(!fs::exists(rollingTotal / L"jk1/1700000000.txt"), "Total limit did not prune after rotation");
        Check(fs::exists(rollingTotal / L"jk1/1700000001.txt"), "Total limit did not preserve the new segment");

        const auto shared = folder / L"shared";
        CommentLogWriter otherWriter;
        otherWriter.Configure(shared.wstring(), true);
        otherWriter.Write(1, 1700000000, std::string(2 * 1024 * 1024, 'x'));
        writer.Configure(shared.wstring(), true, { 0, 1, 0 });
        writer.Write(2, 1700000010, "active");
        Check(fs::exists(shared / L"jk1/1700000000.txt"), "Cleanup removed an active file from another writer");
        Check(fs::exists(shared / L"jk2/1700000010.txt"), "Cleanup removed the current file");
        writer.Close();
        otherWriter.Close();

        std::cout << "Comment log retention, size limits, rotation and active file protection passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
