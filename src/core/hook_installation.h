#pragma once

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace session_board {
    struct hook_install_result
    {
        bool changed { false };
        bool disabled { false };
        bool helper_updated { false };
        std::filesystem::path backup {};
        std::string error {};
    };

    using hook_settings_updater = bool (*)(nlohmann::ordered_json&, const std::filesystem::path&);
    [[nodiscard]] std::filesystem::path default_hook_install_directory();
    // Hook으로 등록할 실행 파일의 이름이다. 앱이 `--hook` 인자로 Hook을 처리하므로 앱 자신이다.
    inline constexpr std::wstring_view hook_executable_name { L"session-board.exe" };
    // 지금 실행 중인 앱 실행 파일이다. 앱은 자신을 설치 폴더로 복사해 Hook으로 등록한다.
    [[nodiscard]] std::filesystem::path app_executable();
    // 우리가 등록한 Hook 명령인가다. 예전 helper(`session-board-hook.exe`) 명령도 우리 것으로 보고 새 명령으로 바꿔 끼운다.
    [[nodiscard]] bool owned_hook_command(std::string_view command, std::string_view provider);
    [[nodiscard]] std::filesystem::path hook_opt_out_file(const std::filesystem::path& install_directory, std::string_view provider);
    // 두 provider가 같은 앱 사본을 Hook으로 쓰므로 복사·백업·동시 수정 감지를 한 경로에서 처리한다.
    [[nodiscard]] hook_install_result ensure_hook_installation(const std::filesystem::path& settings_file, const std::filesystem::path& helper_source, const std::filesystem::path& install_directory,
        std::string_view provider, hook_settings_updater update);
} // namespace session_board
