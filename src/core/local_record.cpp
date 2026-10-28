#include "core/local_record.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

namespace session_board {
    namespace {
        constexpr std::uint64_t read_budget { 8 * 1024 * 1024 };

        struct native_file
        {
            HANDLE value { INVALID_HANDLE_VALUE };
            ~native_file()
            {
                if (value != INVALID_HANDLE_VALUE)
                    CloseHandle(value);
            }
        };

        bool digits(const std::string_view text)
        {
            return !text.empty() && std::ranges::all_of(text, [](const char character) { return character >= '0' && character <= '9'; });
        }

        int number(const std::string_view text)
        {
            int result { 0 };
            for (const char character : text)
                result = result * 10 + (character - '0');
            return result;
        }
    } // namespace

    std::int64_t iso_time_ms(const std::string_view text)
    {
        if (text.size() < 19 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':')
            return 0;
        constexpr std::array<std::pair<std::size_t, std::size_t>, 6> fields { { { 0, 4 }, { 5, 2 }, { 8, 2 }, { 11, 2 }, { 14, 2 }, { 17, 2 } } };
        for (const auto& [offset, length] : fields)
            if (!digits(text.substr(offset, length)))
                return 0;
        SYSTEMTIME time {};
        time.wYear = static_cast<WORD>(number(text.substr(0, 4)));
        time.wMonth = static_cast<WORD>(number(text.substr(5, 2)));
        time.wDay = static_cast<WORD>(number(text.substr(8, 2)));
        time.wHour = static_cast<WORD>(number(text.substr(11, 2)));
        time.wMinute = static_cast<WORD>(number(text.substr(14, 2)));
        time.wSecond = static_cast<WORD>(number(text.substr(17, 2)));
        if (text.size() >= 23 && text[19] == '.' && digits(text.substr(20, 3)))
            time.wMilliseconds = static_cast<WORD>(number(text.substr(20, 3)));
        FILETIME file {};
        if (!SystemTimeToFileTime(&time, &file))
            return 0;
        const ULARGE_INTEGER ticks { file.dwLowDateTime, file.dwHighDateTime };
        return static_cast<std::int64_t>(ticks.QuadPart / 10'000) - 11'644'473'600'000;
    }

    std::int64_t file_time_ms(const std::filesystem::file_time_type time)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::clock_cast<std::chrono::system_clock>(time).time_since_epoch()).count();
    }

    bool file_lock_held(const std::filesystem::path& file)
    {
        const native_file input { CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
        if (input.value == INVALID_HANDLE_VALUE)
            return GetLastError() == ERROR_SHARING_VIOLATION;
        OVERLAPPED overlapped {};
        const bool free { LockFileEx(input.value, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped) != FALSE };
        const DWORD error { GetLastError() };
        if (free)
            UnlockFileEx(input.value, 0, 1, 0, &overlapped);
        return !free && error == ERROR_LOCK_VIOLATION;
    }

    jsonl_tail::jsonl_tail(const std::size_t maximum_line)
        : maximum_line_ { maximum_line }
    {}

    bool jsonl_tail::ready() const noexcept
    {
        return ready_;
    }

    void jsonl_tail::read(const std::filesystem::path& path, const std::function<void()>& restart, const std::function<void(const nlohmann::json&)>& consume)
    {
        const native_file input { CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
        BY_HANDLE_FILE_INFORMATION info {};
        if (input.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(input.value, &info))
            return;
        const std::uint64_t size { (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow };
        const std::array<unsigned long, 3> identity { info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow };
        if (size < position_ || (has_identity_ && identity_ != identity))
        {
            *this = jsonl_tail { maximum_line_ };
            restart();
        }
        identity_ = identity;
        has_identity_ = true;
        LARGE_INTEGER offset {};
        offset.QuadPart = static_cast<LONGLONG>(position_);
        if (!SetFilePointerEx(input.value, offset, nullptr, FILE_BEGIN))
            return;
        std::array<char, 32 * 1024> buffer {};
        const auto until { std::min(size, position_ + read_budget) };
        while (position_ < until)
        {
            DWORD count { 0 };
            if (!ReadFile(input.value, buffer.data(), static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(), until - position_)), &count, nullptr) || count == 0)
                break;
            position_ += count;
            for (DWORD index { 0 }; index < count; ++index)
            {
                const char character { buffer[static_cast<std::size_t>(index)] };
                if (character == '\n')
                {
                    if (!dropping_ && !partial_.empty())
                    {
                        const auto value { nlohmann::json::parse(partial_, nullptr, false) };
                        if (!value.is_discarded())
                            try
                            {
                                consume(value);
                            }
                            catch (...)
                            {}
                    }
                    partial_.clear();
                    dropping_ = false;
                }
                else if (!dropping_)
                {
                    if (partial_.size() < maximum_line_)
                        partial_ += character;
                    else
                    {
                        partial_.clear();
                        partial_.shrink_to_fit();
                        dropping_ = true;
                    }
                }
            }
        }
        ready_ = position_ >= size;
    }
} // namespace session_board
