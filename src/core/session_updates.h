#pragma once

#include "core/session.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace session_board {
    struct session_update
    {
        session_snapshot session {};
        std::uint64_t sequence { 0 };
        bool response_required { false };
        // 작업 중이던 세션의 응답이 끝났거나 오류로 멈췄다. 사용자가 닫거나 그 세션이 다시 일할 때까지 남는다.
        bool completed { false };
        std::optional<std::chrono::steady_clock::time_point> expires_at {};
    };

    // 접힌 동안 알릴 세션을 유지한다.
    //  - 사용자 응답이 필요한 세션: 응답이 처리되면 잠깐 `응답 처리됨`으로 남긴다.
    //  - 작업 중이던 세션의 응답 완료·오류: 무엇을 요청했고 어떻게 끝났는지 놓치지 않게 닫을 때까지 남긴다.
    //  - 진행 갱신은 알리지 않는다.
    class session_updates final
    {
    public:
        void observe(const std::vector<session_snapshot>& sessions, bool hidden, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
        void acknowledge(const std::string& provider, const std::string& session_id, std::uint64_t sequence);
        void expire(std::chrono::steady_clock::time_point now);
        void clear() noexcept;
        [[nodiscard]] const std::vector<session_update>& pending() const noexcept;

    private:
        using session_key = std::pair<std::string, std::string>;
        std::vector<session_update> pending_ {};
        std::uint64_t sequence_ { 0 };
        // 지난 관측에서 그 세션이 작업 중이었는가다. 처음 보는 세션의 완료는 알리지 않는다.
        std::map<session_key, bool> working_ {};
        // 이미 알린 완료의 turn이다. 상태가 잠깐 흔들려 같은 turn이 다시 끝나도 두 번 알리지 않는다.
        std::map<session_key, std::string> completed_turns_ {};
    };
} // namespace session_board
