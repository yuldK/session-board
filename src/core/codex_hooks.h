#pragma once

#include "core/hook_installation.h"

namespace session_board {
    using codex_hook_result = hook_install_result;
    [[nodiscard]] bool update_codex_hook_settings(nlohmann::ordered_json& document, const std::filesystem::path& executable, bool remove = false);
    [[nodiscard]] codex_hook_result ensure_codex_hooks(const std::filesystem::path& settings_file, const std::filesystem::path& helper_source, const std::filesystem::path& install_directory);
} // namespace session_board
