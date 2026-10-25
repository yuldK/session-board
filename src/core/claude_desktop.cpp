#include "core/claude_desktop.h"

#include <windows.h>

#include <shlobj.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

namespace session_board {
    namespace {
        // `<루트>/<계정>/<조직>/local_*.json`이다. 더 깊이는 내려가지 않는다.
        constexpr int record_depth { 2 };

        std::filesystem::path known_folder(const KNOWNFOLDERID& id)
        {
            PWSTR value { nullptr };
            if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &value)))
                return {};
            std::filesystem::path result { value };
            CoTaskMemFree(value);
            return result;
        }

        // 앱이 받는 ID 꼴과 같다 (`^local_[A-Za-z0-9-]{1,64}$`).
        bool desktop_id(const std::string_view value)
        {
            constexpr std::string_view prefix { "local_" };
            if (!value.starts_with(prefix) || value.size() == prefix.size() || value.size() > prefix.size() + 64)
                return false;
            return std::ranges::all_of(value.substr(prefix.size()),
                [](const char character) { return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '-'; });
        }

        bool record_file(const std::filesystem::directory_entry& item)
        {
            std::error_code error {};
            if (!item.is_regular_file(error))
                return false;
            const auto name { item.path().filename().wstring() };
            return name.starts_with(L"local_") && item.path().extension() == L".json";
        }
    } // namespace

    claude_desktop_index::claude_desktop_index(std::vector<std::filesystem::path> roots)
        : roots_ { std::move(roots) }
    {}

    void claude_desktop_index::refresh()
    {
        std::set<std::filesystem::path> seen {};
        for (const auto& root : roots_)
        {
            std::error_code error {};
            std::filesystem::recursive_directory_iterator walk { root, std::filesystem::directory_options::skip_permission_denied, error };
            for (; !error && walk != std::filesystem::recursive_directory_iterator {}; walk.increment(error))
            {
                if (walk.depth() >= record_depth)
                    walk.disable_recursion_pending();
                if (walk.depth() != record_depth || !record_file(*walk))
                    continue;
                const auto path { walk->path() };
                seen.insert(path);
                std::error_code time_error {};
                const auto time { walk->last_write_time(time_error) };
                if (time_error)
                    continue;
                const auto known { entries_.find(path) };
                if (known != entries_.end() && known->second.time == time)
                    continue;
                // 데스크톱이 기록을 쓰는 중이면 이번에는 건너뛰고 다음에 다시 읽는다.
                try
                {
                    std::ifstream input { path, std::ios::binary };
                    const auto document { nlohmann::json::parse(input) };
                    entry value { .time = time };
                    value.session_id = document.value("sessionId", std::string {});
                    value.cli_session_id = document.value("cliSessionId", std::string {});
                    value.archived = document.value("isArchived", false);
                    entries_[path] = std::move(value);
                }
                catch (...)
                {
                    continue;
                }
            }
        }
        std::erase_if(entries_, [&](const auto& item) { return !seen.contains(item.first); });
    }

    std::string claude_desktop_index::find(const std::string_view cli_session_id) const
    {
        if (cli_session_id.empty())
            return {};
        for (const auto& [path, value] : entries_)
            if (!value.archived && value.cli_session_id == cli_session_id && desktop_id(value.session_id))
                return value.session_id;
        return {};
    }

    std::vector<std::filesystem::path> default_claude_desktop_roots()
    {
        std::vector<std::filesystem::path> candidates {};
        const auto local { known_folder(FOLDERID_LocalAppData) };
        const auto roaming { known_folder(FOLDERID_RoamingAppData) };
        // 폴더를 훑다 권한 오류가 나도 그때까지 찾은 위치는 쓴다.
        try
        {
            std::error_code error {};
            if (!local.empty())
            {
                for (const auto& item : std::filesystem::directory_iterator { local / L"Claude-Data", error })
                    candidates.push_back(item.path() / L"claude-code-sessions");
                for (const auto& item : std::filesystem::directory_iterator { local / L"Packages", error })
                    if (item.path().filename().wstring().starts_with(L"Claude_"))
                        candidates.push_back(item.path() / L"LocalCache" / L"Roaming" / L"Claude" / L"claude-code-sessions");
            }
        }
        catch (...)
        {}
        if (!roaming.empty())
            candidates.push_back(roaming / L"Claude" / L"claude-code-sessions");
        std::vector<std::filesystem::path> result {};
        std::ranges::copy_if(candidates, std::back_inserter(result), [](const auto& path) {
            std::error_code ignored {};
            return std::filesystem::is_directory(path, ignored);
        });
        return result;
    }

    std::wstring claude_desktop_link(const std::string_view desktop_session_id)
    {
        if (!desktop_id(desktop_session_id))
            return {};
        return L"claude://code/continue?session=" + std::wstring { desktop_session_id.begin(), desktop_session_id.end() };
    }
} // namespace session_board
