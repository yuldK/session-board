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
    [[nodiscard]] std::filesystem::path bundled_hook_executable();
    [[nodiscard]] std::filesystem::path hook_opt_out_file(const std::filesystem::path& install_directory, std::string_view provider);
    // 두 provider가 같은 helper를 설치하므로 복사·백업·동시 수정 감지를 한 경로에서 처리한다.
    [[nodiscard]] hook_install_result ensure_hook_installation(const std::filesystem::path& settings_file, const std::filesystem::path& helper_source, const std::filesystem::path& install_directory,
        std::string_view provider, hook_settings_updater update);
} // namespace session_board
