#pragma once

#include <string_view>

namespace session_board {
    // 세션을 모으는 AI 하나다. provider 이름을 곳곳에서 비교하지 않고 이 표 하나에서 찾는다.
    struct provider_info
    {
        // 이벤트·스냅숏·필터 값에 쓰는 이름이다.
        std::string_view id {};
        std::u8string_view display {};
        // 로컬 기록 수집기가 `observation_source`에 남기는 값과 그 설명이다.
        std::string_view local_source {};
        std::u8string_view local_text {};
    };

    // 화면의 필터·상태 줄이 이 순서를 따른다.
    inline constexpr provider_info providers[] {
        { "codex", u8"Codex", "codex-local", u8"Codex 로컬 기록" },
        { "claude", u8"Claude", "claude-local", u8"Claude 로컬 기록" },
        { "antigravity", u8"Antigravity", "antigravity-local", u8"Antigravity 로컬 기록" },
    };

    // 표에 없으면 nullptr이다.
    [[nodiscard]] constexpr const provider_info* find_provider(const std::string_view id) noexcept
    {
        for (const auto& provider : providers)
            if (provider.id == id)
                return &provider;
        return nullptr;
    }

    [[nodiscard]] constexpr bool valid_provider(const std::string_view id) noexcept
    {
        return find_provider(id) != nullptr;
    }
} // namespace session_board
