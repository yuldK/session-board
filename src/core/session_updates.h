#pragma once

#include "core/session.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace session_board {
    struct session_update
    {
        session_snapshot session {};
        std::uint64_t sequence { 0 };
        bool response_required { false };
        std::optional<std::chrono::steady_clock::time_point> expires_at {};
    };

    // 접힌 동안 사용자 응답이 필요한 세션만 유지한다. 응답 처리 뒤에는 잠깐 완료 상태로 남긴다.
    class session_updates final
    {
    public:
        void observe(const std::vector<session_snapshot>& sessions, bool hidden, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
        void acknowledge(const std::string& provider, const std::string& session_id, std::uint64_t sequence);
        void expire(std::chrono::steady_clock::time_point now);
        void clear() noexcept;
        [[nodiscard]] const std::vector<session_update>& pending() const noexcept;

    private:
        std::vector<session_update> pending_ {};
        std::uint64_t sequence_ { 0 };
    };
} // namespace session_board
