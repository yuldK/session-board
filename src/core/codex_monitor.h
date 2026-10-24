#pragma once

#include "core/session.h"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace session_board {
    class codex_rollout final
    {
    public:
        void consume(const nlohmann::json& record);
        [[nodiscard]] const session_snapshot& snapshot() const noexcept;
        [[nodiscard]] bool root_session() const noexcept;

    private:
        void public_message(const std::string& text);
        void tool_started(const std::string& id, const std::string& name);
        void tool_finished(const std::string& id);
        session_snapshot snapshot_ {};
        bool root_ { false };
    };

    struct codex_observation
    {
        std::vector<session_snapshot> sessions {};
        std::string error {};
        bool ready { false };
    };

    [[nodiscard]] std::filesystem::path default_codex_directory();
    [[nodiscard]] bool codex_writer_active(const std::filesystem::path& lock_file);
    void merge_codex_sessions(std::vector<session_snapshot>& sessions, const std::vector<session_snapshot>& observed);

    class codex_monitor final
    {
    public:
        explicit codex_monitor(std::filesystem::path directory);
        ~codex_monitor();
        [[nodiscard]] codex_observation snapshot() const;

    private:
        void run(std::stop_token stop, const std::filesystem::path& directory);
        mutable std::mutex mutex_ {};
        codex_observation observation_ {};
        std::mutex wait_mutex_ {};
        std::condition_variable_any wake_ {};
        std::jthread worker_ {};
    };
} // namespace session_board
