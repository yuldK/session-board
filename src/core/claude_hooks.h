#pragma once

#include "core/hook_installation.h"

#include <filesystem>
#include <string>

namespace session_board {
    using claude_hook_result = hook_install_result;
    // 이 파일이 있으면 앱이 Claude Hook을 다시 등록하지 않는다. 제거 스크립트가 만든다.
    [[nodiscard]] std::filesystem::path claude_hook_opt_out_file(const std::filesystem::path& install_directory);
    [[nodiscard]] std::string claude_hook_command(const std::filesystem::path& executable);
    [[nodiscard]] bool update_claude_hook_settings(nlohmann::ordered_json& document, const std::string& command, bool remove = false);
    // 앱 시작 때 호출한다. 필요한 경우에만 helper를 갱신하고 설정을 백업한 뒤 교체한다. 예외를 던지지 않는다.
    [[nodiscard]] claude_hook_result ensure_claude_hooks(const std::filesystem::path& settings_file, const std::filesystem::path& helper_source, const std::filesystem::path& install_directory);
} // namespace session_board
