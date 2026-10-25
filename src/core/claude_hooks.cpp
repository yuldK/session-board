#include "core/claude_hooks.h"

#include <shlobj.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace session_board {
    namespace {
        struct hook_spec
        {
            const char* event {};
            const char* matcher {};
        };

        // scripts/install-claude-hooks.ps1의 목록과 같게 유지한다.
        constexpr hook_spec hook_specs[] {
            { "SessionStart", nullptr },
            { "UserPromptSubmit", nullptr },
            { "PreToolUse", "*" },
            { "PostToolUse", "*" },
            { "PostToolUseFailure", "*" },
            { "PermissionRequest", "*" },
            { "Notification", "elicitation_dialog|elicitation_url_dialog" },
            { "Stop", nullptr },
            { "StopFailure", nullptr },
            { "SessionEnd", nullptr },
        };
        constexpr int hook_timeout_seconds { 5 };

        bool owned_handler(const nlohmann::ordered_json& handler)
        {
            if (!handler.is_object() || !handler.contains("command") || !handler["command"].is_string())
                return false;
            return owned_hook_command(handler["command"].get_ref<const std::string&>(), "claude");
        }

        std::size_t owned_count(const nlohmann::ordered_json& group)
        {
            if (!group.is_object() || !group.contains("hooks") || !group["hooks"].is_array())
                return 0;
            const auto& handlers { group["hooks"] };
            return static_cast<std::size_t>(std::count_if(handlers.begin(), handlers.end(), owned_handler));
        }

        nlohmann::ordered_json hook_group(const hook_spec& spec, const std::string& command)
        {
            nlohmann::ordered_json group = nlohmann::ordered_json::object();
            if (spec.matcher != nullptr)
                group["matcher"] = spec.matcher;
            nlohmann::ordered_json handler = nlohmann::ordered_json::object();
            handler["type"] = "command";
            handler["command"] = command;
            handler["timeout"] = hook_timeout_seconds;
            group["hooks"] = nlohmann::ordered_json::array({ handler });
            return group;
        }

        struct file_stamp
        {
            bool exists { false };
            FILETIME written {};
            std::uint64_t size { 0 };

            [[nodiscard]] bool operator==(const file_stamp& other) const noexcept
            {
                return exists == other.exists && size == other.size && CompareFileTime(&written, &other.written) == 0;
            }
        };

        file_stamp stamp(const std::filesystem::path& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data {};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            {
                const auto error { GetLastError() };
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                    return {};
                throw std::runtime_error { "Cannot inspect settings." };
            }
            return { true, data.ftLastWriteTime, ULARGE_INTEGER { data.nFileSizeLow, data.nFileSizeHigh }.QuadPart };
        }

        std::string read_text(const std::filesystem::path& path)
        {
            std::ifstream input { path, std::ios::binary };
            if (!input)
                throw std::runtime_error { "Cannot read file." };
            return { std::istreambuf_iterator<char> { input }, std::istreambuf_iterator<char> {} };
        }

        void replace_file(const std::filesystem::path& source, const std::filesystem::path& target)
        {
            if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                DeleteFileW(source.c_str());
                throw std::runtime_error { "Cannot replace file." };
            }
        }

        bool same_contents(const std::filesystem::path& left, const std::filesystem::path& right)
        {
            std::error_code error {};
            if (!std::filesystem::exists(right, error) || std::filesystem::file_size(left) != std::filesystem::file_size(right))
                return false;
            std::ifstream first { left, std::ios::binary };
            std::ifstream second { right, std::ios::binary };
            std::array<char, 64 * 1024> first_bytes {};
            std::array<char, 64 * 1024> second_bytes {};
            while (first && second)
            {
                first.read(first_bytes.data(), static_cast<std::streamsize>(first_bytes.size()));
                second.read(second_bytes.data(), static_cast<std::streamsize>(second_bytes.size()));
                if (first.gcount() != second.gcount() || !std::equal(first_bytes.begin(), first_bytes.begin() + first.gcount(), second_bytes.begin()))
                    return false;
            }
            return first.eof() && second.eof();
        }

        // Hook으로 실행 중인 사본은 교체할 수 없으므로 실패하면 기존 사본을 둔 채 다음 실행 때 다시 시도한다.
        bool install_helper(const std::filesystem::path& source, const std::filesystem::path& installed)
        {
            std::error_code error {};
            if (source.empty() || !std::filesystem::exists(source, error))
            {
                if (std::filesystem::exists(installed, error))
                    return false;
                throw std::runtime_error { "Helper source is missing." };
            }
            if (std::filesystem::equivalent(source, installed, error) || same_contents(source, installed))
                return false;
            std::filesystem::create_directories(installed.parent_path());
            const std::filesystem::path temporary { installed.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp" };
            if (!CopyFileW(source.c_str(), temporary.c_str(), FALSE))
                throw std::runtime_error { "Cannot copy helper." };
            replace_file(temporary, installed);
            // 배포본은 실행 파일 옆에, 다중 구성 빌드는 구성 폴더 한 단계 위에 라이선스가 있다.
            for (const auto& license : { source.parent_path() / L"json-LICENSE.txt", source.parent_path().parent_path() / L"json-LICENSE.txt" })
                if (std::filesystem::exists(license, error))
                {
                    std::filesystem::copy_file(license, installed.parent_path() / L"json-LICENSE.txt", std::filesystem::copy_options::overwrite_existing, error);
                    break;
                }
            return true;
        }

        class named_lock final
        {
        public:
            named_lock()
                : handle_ { CreateMutexW(nullptr, FALSE, L"Local\\SessionBoard-Hooks") }
            {
                if (handle_ == nullptr)
                    throw std::runtime_error { "Cannot create hook lock." };
                const DWORD wait { WaitForSingleObject(handle_, 2000) };
                if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
                {
                    CloseHandle(handle_);
                    throw std::runtime_error { "Hook lock timed out." };
                }
            }
            ~named_lock()
            {
                ReleaseMutex(handle_);
                CloseHandle(handle_);
            }
            named_lock(const named_lock&) = delete;
            named_lock& operator=(const named_lock&) = delete;

        private:
            HANDLE handle_ { nullptr };
        };

        std::wstring backup_suffix()
        {
            SYSTEMTIME time {};
            GetLocalTime(&time);
            std::array<wchar_t, 64> text {};
            swprintf_s(text.data(), text.size(), L".session-board-%04u%02u%02u-%02u%02u%02u-%03u.bak", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
            return text.data();
        }
    } // namespace

    std::filesystem::path default_hook_install_directory()
    {
        PWSTR value { nullptr };
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value)))
            throw std::runtime_error { "Cannot locate LocalAppData." };
        const std::filesystem::path result { std::filesystem::path { value } / L"SessionBoard" / L"bin" };
        CoTaskMemFree(value);
        return result;
    }

    std::filesystem::path app_executable()
    {
        std::array<wchar_t, 32768> module {};
        const auto count { GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size())) };
        if (count == 0 || count >= module.size())
            return {};
        return std::filesystem::path { module.data() };
    }

    bool owned_hook_command(const std::string_view command, const std::string_view provider)
    {
        const bool ours { command.find("session-board.exe") != std::string_view::npos || command.find("session-board-hook.exe") != std::string_view::npos };
        return ours && command.ends_with("--provider " + std::string { provider });
    }

    std::filesystem::path claude_hook_opt_out_file(const std::filesystem::path& install_directory)
    {
        return hook_opt_out_file(install_directory, "claude");
    }

    std::filesystem::path hook_opt_out_file(const std::filesystem::path& install_directory, const std::string_view provider)
    {
        return install_directory.parent_path() / (std::string { provider } + "-hooks.disabled");
    }

    std::string claude_hook_command(const std::filesystem::path& executable)
    {
        // 공백 없는 경로는 따옴표 없이 써서 Git Bash와 PowerShell 어느 Hook 셸에서도 그대로 실행되게 한다.
        const auto text { executable.generic_u8string() };
        const std::string path { text.begin(), text.end() };
        return (path.find(' ') == std::string::npos ? path : "\"" + path + "\"") + " --hook --provider claude";
    }

    bool update_claude_hook_settings(nlohmann::ordered_json& document, const std::string& command, const bool remove)
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
        bool changed { false };
        for (const auto& spec : hook_specs)
        {
            const auto desired { hook_group(spec, command) };
            const auto found { hooks.find(spec.event) };
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
            // 이미 원하는 등록이 하나뿐이면 다른 Hook의 순서를 건드리지 않는다.
            if ((!remove && owned == 1 && exact) || (remove && owned == 0))
                continue;

            auto groups { nlohmann::ordered_json::array() };
            if (found != hooks.end())
                for (auto group : *found)
                {
                    if (owned_count(group) > 0)
                    {
                        auto handlers { nlohmann::ordered_json::array() };
                        for (const auto& handler : group["hooks"])
                            if (!owned_handler(handler))
                                handlers.push_back(handler);
                        if (handlers.empty())
                            continue;
                        group["hooks"] = std::move(handlers);
                    }
                    groups.push_back(std::move(group));
                }
            if (!remove)
                groups.push_back(desired);
            if (groups.empty())
                hooks.erase(spec.event);
            else
                hooks[spec.event] = std::move(groups);
            changed = true;
        }
        if (changed && hooks.empty())
            document.erase("hooks");
        return changed;
    }

    claude_hook_result ensure_claude_hooks(const std::filesystem::path& settings_file, const std::filesystem::path& helper_source, const std::filesystem::path& install_directory)
    {
        return ensure_hook_installation(settings_file, helper_source, install_directory, "claude",
            [](nlohmann::ordered_json& document, const std::filesystem::path& installed) { return update_claude_hook_settings(document, claude_hook_command(installed)); });
    }

    hook_install_result ensure_hook_installation(const std::filesystem::path& settings_file, const std::filesystem::path& helper_source, const std::filesystem::path& install_directory,
        const std::string_view provider, const hook_settings_updater update)
    {
        hook_install_result result {};
        const std::string name { provider == "codex" ? "Codex" : "Claude" };
        try
        {
            std::error_code error {};
            if (std::filesystem::exists(hook_opt_out_file(install_directory, provider), error))
            {
                result.disabled = true;
                return result;
            }
            const named_lock lock {};
            const auto installed { install_directory / hook_executable_name };
            try
            {
                result.helper_updated = install_helper(helper_source, installed);
            }
            catch (...)
            {
                result.error = "Hook용 Session Board 사본을 갱신하지 못했습니다. 다음 실행 때 다시 시도합니다.";
                return result;
            }
            // 없는 실행 파일을 등록하면 모든 도구 호출마다 Hook 오류가 남으므로 등록하지 않는다.
            if (!std::filesystem::exists(installed, error))
            {
                result.error = "Hook용 Session Board 사본이 없어 " + name + " Hook을 등록하지 않았습니다.";
                return result;
            }

            const auto before { stamp(settings_file) };
            std::string text {};
            nlohmann::ordered_json document = nlohmann::ordered_json::object();
            if (before.exists)
            {
                if (before.size > 4 * 1024 * 1024)
                    throw std::runtime_error { "Settings are too large." };
                text = read_text(settings_file);
                if (text.starts_with("\xef\xbb\xbf"))
                    text.erase(0, 3);
                document = nlohmann::ordered_json::parse(text, nullptr, false);
                if (document.is_discarded() || !document.is_object())
                {
                    result.error = name + " 설정 파일을 해석하지 못해 Hook을 등록하지 않았습니다.";
                    return result;
                }
            }
            if (!update(document, installed))
                return result;

            const bool crlf { text.find("\r\n") != std::string::npos };
            std::string output { document.dump(2) };
            if (crlf)
                for (std::size_t position { output.find('\n') }; position != std::string::npos; position = output.find('\n', position + 2))
                    output.insert(position, 1, '\r');
            output += crlf ? "\r\n" : "\n";

            std::filesystem::create_directories(settings_file.parent_path());
            if (before.exists)
            {
                const std::filesystem::path backup { settings_file.wstring() + backup_suffix() };
                if (!CopyFileW(settings_file.c_str(), backup.c_str(), TRUE))
                    throw std::runtime_error { "Cannot back up settings." };
                result.backup = backup;
            }
            const std::filesystem::path temporary { settings_file.wstring() + L".session-board." + std::to_wstring(GetCurrentProcessId()) + L".tmp" };
            {
                std::ofstream file { temporary, std::ios::binary | std::ios::trunc };
                file.write(output.data(), static_cast<std::streamsize>(output.size()));
                if (!file)
                    throw std::runtime_error { "Cannot write settings." };
            }
            // Claude Code나 사용자가 그 사이에 설정을 바꿨으면 덮어쓰지 않고 다음 실행 때 다시 시도한다.
            if (!(stamp(settings_file) == before))
            {
                DeleteFileW(temporary.c_str());
                result.error = name + " 설정이 동시에 바뀌어 Hook 등록을 다음 실행으로 미뤘습니다.";
                return result;
            }
            replace_file(temporary, settings_file);
            result.changed = true;
        }
        catch (...)
        {
            result.error = name + " Hook을 자동 등록하지 못했습니다. 로컬 수집은 계속됩니다.";
        }
        return result;
    }
} // namespace session_board
