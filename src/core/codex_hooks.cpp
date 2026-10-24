#include "core/codex_hooks.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>

namespace session_board {
    namespace {
        constexpr const char* events[] { "SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "PermissionRequest", "Stop", "Interrupt", "SessionEnd" };

        bool owned_handler(const nlohmann::ordered_json& handler)
        {
            if (!handler.is_object())
                return false;
            const auto found { handler.find("command") };
            if (found == handler.end() || !found->is_string())
                return false;
            const auto& command { found->get_ref<const std::string&>() };
            return command.find("session-board-hook.exe") != std::string::npos && command.ends_with("--provider codex");
        }

        std::size_t owned_count(const nlohmann::ordered_json& group)
        {
            if (!group.is_object() || !group.contains("hooks") || !group["hooks"].is_array())
                return 0;
            return static_cast<std::size_t>(std::count_if(group["hooks"].begin(), group["hooks"].end(), owned_handler));
        }
    } // namespace

    bool update_codex_hook_settings(nlohmann::ordered_json& document, const std::filesystem::path& executable, const bool remove)
    {
        if (document.is_null())
            document = nlohmann::ordered_json::object();
        if (!document.is_object())
            throw std::runtime_error { "Settings must be an object." };
        if (!document.contains("hooks"))
        {
            if (remove)
                return false;
            document["hooks"] = nlohmann::ordered_json::object();
        }
        auto& hooks { document["hooks"] };
        if (!hooks.is_object())
            throw std::runtime_error { "Unexpected hooks layout." };
        const auto encoded { executable.generic_u8string() };
        const std::string path { encoded.begin(), encoded.end() };
        std::string escaped {};
        for (const char character : path)
            escaped += character == '\'' ? "''" : std::string { character };
        // install-codex-hooks.ps1와 같은 정의를 사용해 이미 신뢰한 Hook의 hash를 유지한다.
        nlohmann::ordered_json handler = nlohmann::ordered_json::object();
        handler["type"] = "command";
        handler["command"] = "\"" + path + "\" --provider codex";
        handler["commandWindows"] = "& '" + escaped + "' --provider codex";
        handler["timeout"] = 2;
        const nlohmann::ordered_json desired { { "hooks", nlohmann::ordered_json::array({ handler }) } };
        bool changed { false };
        for (const auto* event : events)
        {
            const auto found { hooks.find(event) };
            if (found != hooks.end() && !found->is_array())
                throw std::runtime_error { "Unexpected hook event layout." };
            std::size_t owned { 0 };
            bool exact { false };
            if (found != hooks.end())
                for (const auto& group : *found)
                {
                    const auto count { owned_count(group) };
                    owned += count;
                    exact = exact || (count > 0 && nlohmann::json::parse(group.dump()) == nlohmann::json::parse(desired.dump()));
                }
            if ((!remove && owned == 1 && exact) || (remove && owned == 0))
                continue;
            auto groups { nlohmann::ordered_json::array() };
            if (found != hooks.end())
                for (auto group : *found)
                {
                    if (owned_count(group) > 0)
                    {
                        auto handlers { nlohmann::ordered_json::array() };
                        for (const auto& item : group["hooks"])
                            if (!owned_handler(item))
                                handlers.push_back(item);
                        if (handlers.empty())
                            continue;
                        group["hooks"] = std::move(handlers);
                    }
                    groups.push_back(std::move(group));
                }
            if (!remove)
                groups.push_back(desired);
            if (groups.empty())
                hooks.erase(event);
            else
                hooks[event] = std::move(groups);
            changed = true;
        }
        if (changed && hooks.empty())
            document.erase("hooks");
        return changed;
    }

    codex_hook_result ensure_codex_hooks(const std::filesystem::path& settings_file, const std::filesystem::path& helper_source, const std::filesystem::path& install_directory)
    {
        return ensure_hook_installation(settings_file, helper_source, install_directory, "codex",
            [](nlohmann::ordered_json& document, const std::filesystem::path& installed) { return update_codex_hook_settings(document, installed); });
    }
} // namespace session_board
