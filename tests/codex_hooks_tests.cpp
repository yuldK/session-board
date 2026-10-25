#include "core/claude_hooks.h"
#include "core/codex_hooks.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace session_board {
    namespace {
        int assertions { 0 };
        void require(const bool condition, const char* message)
        {
            ++assertions;
            if (!condition)
                throw std::runtime_error { message };
        }

        std::string read(const std::filesystem::path& path)
        {
            std::ifstream file { path, std::ios::binary };
            return { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
        }

        void registration(const std::filesystem::path& directory)
        {
            const auto home { directory / "codex" };
            const auto settings { home / "hooks.json" };
            const auto source { directory / "bundled" / "session-board.exe" };
            const auto install { directory / "Session Board's bin" };
            std::filesystem::create_directories(home);
            std::filesystem::create_directories(source.parent_path());
            std::ofstream { source, std::ios::binary } << "helper-v1";
            const std::string original = "{\r\n  \"extra\": {\"preserve\": true},\r\n  \"hooks\": {\"PreToolUse\": [{\"matcher\": \"Bash\", \"hooks\": [{\"type\": \"mcp_tool\", \"server_label\": "
                                         "\"other\", \"tool_name\": \"check\"}]}]}\r\n}\r\n";
            std::ofstream { settings, std::ios::binary } << original;
            const auto first { ensure_codex_hooks(settings, source, install) };
            require(first.changed && first.helper_updated && first.error.empty(), "First launch did not install hooks.");
            require(read(first.backup) == original, "Original bytes not backed up.");
            const auto registered { read(settings) };
            auto document { nlohmann::ordered_json::parse(registered) };
            require(document["extra"]["preserve"] && document["hooks"]["PreToolUse"][0]["hooks"][0]["type"] == "mcp_tool", "Unrelated settings lost.");
            for (const char* event : { "SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "PermissionRequest", "Stop", "Interrupt", "SessionEnd" })
            {
                const auto& handler { document["hooks"][event].back()["hooks"][0] };
                require(handler["timeout"] == 2 && handler["command"].get<std::string>().ends_with("session-board.exe\" --hook --provider codex"), "Missing event or changed definition.");
                require(handler["commandWindows"].get<std::string>().find("Board''s bin") != std::string::npos, "PowerShell path not escaped.");
            }
            const auto modified { std::filesystem::last_write_time(settings) };
            const auto second { ensure_codex_hooks(settings, source, install) };
            require(!second.changed && !second.helper_updated && second.error.empty() && read(settings) == registered, "Repeat launch rewrote trusted definitions.");
            require(std::filesystem::last_write_time(settings) == modified, "Repeat launch touched settings.");
            const auto saved { document["hooks"]["Stop"][0]["hooks"][0] };
            nlohmann::ordered_json reordered = nlohmann::ordered_json::object();
            for (const char* key : { "timeout", "commandWindows", "command", "type" })
                reordered[key] = saved[key];
            document["hooks"]["Stop"][0]["hooks"][0] = reordered;
            const auto different_order { document.dump(2) };
            std::ofstream { settings, std::ios::binary } << different_order;
            require(!ensure_codex_hooks(settings, source, install).changed && read(settings) == different_order, "Key order caused hook review.");
            document["hooks"]["Stop"].push_back(document["hooks"]["Stop"][0]);
            require(update_codex_hook_settings(document, install / "session-board.exe") && document["hooks"]["Stop"].size() == 1, "Duplicate hooks not collapsed.");
            require(update_codex_hook_settings(document, install / "session-board.exe", true) && document == nlohmann::ordered_json::parse(original), "Removal lost other handlers.");
            // 예전 helper 명령은 우리 항목으로 보고 새 명령으로 바꿔 끼운다.
            auto legacy { nlohmann::ordered_json::parse(original) };
            legacy["hooks"]["Stop"] = nlohmann::ordered_json::parse(R"([{"hooks":[{"type":"command","command":"\"C:/old/session-board-hook.exe\" --provider codex","timeout":2}]}])");
            require(update_codex_hook_settings(legacy, install / "session-board.exe"), "Legacy helper not replaced.");
            require(legacy["hooks"]["Stop"].size() == 1 && legacy["hooks"]["Stop"][0]["hooks"][0]["command"].get<std::string>().ends_with("--hook --provider codex"), "Legacy helper kept.");
            std::ofstream { hook_opt_out_file(install, "codex"), std::ios::binary } << "";
            require(ensure_codex_hooks(settings, source, install).disabled && read(settings) == different_order, "Explicit removal undone.");
            std::filesystem::remove(hook_opt_out_file(install, "codex"));
            const std::string broken { "{\"hooks\": [ // custom data" };
            std::ofstream { settings, std::ios::binary } << broken;
            const auto bad { ensure_codex_hooks(settings, source, install) };
            require(!bad.changed && !bad.error.empty() && read(settings) == broken, "Invalid config overwritten.");
            const auto fresh { directory / "fresh" / "hooks.json" };
            const auto clean { ensure_codex_hooks(fresh, source, install) };
            require(clean.changed && clean.backup.empty() && nlohmann::json::parse(read(fresh))["hooks"].size() == 8, "Fresh environment not configured.");
            const auto claude { ensure_claude_hooks(directory / "claude" / "settings.json", source, install) };
            require(claude.changed && !claude.helper_updated && claude.error.empty(), "Claude did not reuse shared helper.");
            require(!ensure_codex_hooks(fresh, source, install).changed, "Claude altered Codex definitions.");
            // 사용 중인 이전 helper를 교체하지 못하면 새 설정은 설치하지 않는다.
            const HANDLE locked { CreateFileW((install / "session-board.exe").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
            require(locked != INVALID_HANDLE_VALUE, "Cannot lock installed fixture.");
            std::ofstream { source, std::ios::binary } << "helper-v2";
            const auto blocked { directory / "blocked" / "hooks.json" };
            const auto blocked_result { ensure_codex_hooks(blocked, source, install) };
            CloseHandle(locked);
            require(!blocked_result.changed && !blocked_result.error.empty() && !std::filesystem::exists(blocked), "Failed helper replacement installed unusable hooks.");
            require(ensure_codex_hooks(blocked, source, install).changed && read(install / "session-board.exe") == "helper-v2", "Deferred update failed.");
        }
    } // namespace
} // namespace session_board

int main()
{
    const auto directory { std::filesystem::temp_directory_path() / (L"session-board-hook-install-test-" + std::to_wstring(GetCurrentProcessId())) };
    try
    {
        session_board::registration(directory);
        std::filesystem::remove_all(directory);
        std::cout << session_board::assertions << " hook installation checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::error_code ignored {};
        std::filesystem::remove_all(directory, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
