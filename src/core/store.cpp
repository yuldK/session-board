#include "core/store.h"

#include <shlobj.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace session_board {
    namespace {
        class handle final
        {
        public:
            explicit handle(const HANDLE value)
                : value_ { value }
            {}
            ~handle()
            {
                if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
                    CloseHandle(value_);
            }
            handle(const handle&) = delete;
            handle& operator=(const handle&) = delete;
            [[nodiscard]] HANDLE get() const
            {
                return value_;
            }

        private:
            HANDLE value_ { INVALID_HANDLE_VALUE };
        };

        std::string read_file(const std::filesystem::path& path)
        {
            const handle file { CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
            if (file.get() == INVALID_HANDLE_VALUE)
                throw std::runtime_error { "Cannot open snapshot." };
            LARGE_INTEGER size {};
            if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 || size.QuadPart > 65'536)
                throw std::runtime_error { "Snapshot is too large." };
            std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
            DWORD read { 0 };
            if (!ReadFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &read, nullptr) || read != text.size())
                throw std::runtime_error { "Cannot read snapshot." };
            return text;
        }

        void write_atomic(const std::filesystem::path& path, const std::string& text)
        {
            const std::filesystem::path temporary { path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp" };
            try
            {
                {
                    const handle file { CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
                    DWORD written { 0 };
                    if (file.get() == INVALID_HANDLE_VALUE || !WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) || written != text.size()
                        || !FlushFileBuffers(file.get()))
                        throw std::runtime_error { "Cannot write snapshot." };
                }
                if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    throw std::runtime_error { "Cannot replace snapshot." };
            }
            catch (...)
            {
                DeleteFileW(temporary.c_str());
                throw;
            }
        }
    } // namespace

    std::filesystem::path default_data_directory()
    {
        PWSTR value { nullptr };
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value)))
            throw std::runtime_error { "Cannot locate LocalAppData." };
        const std::filesystem::path result { std::filesystem::path { value } / L"SessionBoard" / L"sessions" };
        CoTaskMemFree(value);
        return result;
    }

    std::string stable_key(const std::string_view value)
    {
        std::uint64_t hash { 14695981039346656037ull };
        for (const char character : value)
        {
            hash ^= static_cast<unsigned char>(character);
            hash *= 1099511628211ull;
        }
        std::ostringstream output {};
        output << std::hex << std::setw(16) << std::setfill('0') << hash;
        return output.str();
    }

    void save_event(const std::filesystem::path& directory, const session_event& event)
    {
        std::filesystem::create_directories(directory);
        const std::string key { event.provider + "-" + stable_key(event.session_id) };
        const auto absolute { std::filesystem::weakly_canonical(directory).u8string() };
        const std::string path_text { absolute.begin(), absolute.end() };
        const std::string mutex_id { stable_key(path_text + key) };
        const std::wstring mutex_name { L"Local\\SessionBoard-" + std::wstring { mutex_id.begin(), mutex_id.end() } };
        const handle mutex { CreateMutexW(nullptr, FALSE, mutex_name.c_str()) };
        if (mutex.get() == nullptr)
            throw std::runtime_error { "Cannot create session lock." };
        const DWORD wait { WaitForSingleObject(mutex.get(), 1000) };
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
            throw std::runtime_error { "Session lock timed out." };
        try
        {
            const std::filesystem::path path { directory / (key + ".json") };
            session_snapshot snapshot {};
            if (std::filesystem::exists(path))
                snapshot = parse_snapshot(nlohmann::json::parse(read_file(path)));
            if (apply_event(snapshot, event))
                write_atomic(path, serialize_snapshot(snapshot).dump(4) + "\r\n");
        }
        catch (...)
        {
            ReleaseMutex(mutex.get());
            throw;
        }
        ReleaseMutex(mutex.get());
    }

    load_result load_sessions(const std::filesystem::path& directory)
    {
        load_result result {};
        if (!std::filesystem::exists(directory))
            return result;
        std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> paths {};
        for (const auto& entry : std::filesystem::directory_iterator { directory })
            if (entry.is_regular_file() && entry.path().extension() == L".json")
                paths.emplace_back(entry.last_write_time(), entry.path());
        std::ranges::sort(paths, [](const auto& left, const auto& right) { return left.first > right.first; });
        result.truncated = paths.size() > 512;
        if (paths.size() > 512)
            paths.resize(512);
        for (const auto& [time, path] : paths)
        {
            static_cast<void>(time);
            try
            {
                result.sessions.push_back(parse_snapshot(nlohmann::json::parse(read_file(path))));
            }
            catch (...)
            {
                ++result.invalid_files;
            }
        }
        std::ranges::sort(result.sessions, [](const auto& left, const auto& right) { return left.updated_ms > right.updated_ms; });
        return result;
    }
} // namespace session_board
