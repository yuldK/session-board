#pragma once

#include "core/session.h"

#include <filesystem>
#include <vector>

namespace session_board {
    struct load_result
    {
        std::vector<session_snapshot> sessions {};
        std::size_t invalid_files { 0 };
        bool truncated { false };
    };

    [[nodiscard]] std::filesystem::path default_data_directory();
    [[nodiscard]] std::string stable_key(std::string_view value);
    void save_event(const std::filesystem::path& directory, const session_event& event);
    [[nodiscard]] load_result load_sessions(const std::filesystem::path& directory);
} // namespace session_board
