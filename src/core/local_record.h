#pragma once

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace session_board {
    // ISO 8601 UTC 시각(`2026-10-28T12:32:11Z`, 소수 초는 앞 세 자리까지)을 Unix epoch ms로 바꾼다. 읽지 못하면 0이다.
    [[nodiscard]] std::int64_t iso_time_ms(std::string_view text);
    [[nodiscard]] std::int64_t file_time_ms(std::filesystem::file_time_type time);
    // 다른 프로세스가 그 파일을 열어 둔 채 막고 있는가다. 공유를 막고 열었거나 첫 바이트를 잠갔으면 참이다.
    //  - 우리는 모든 공유를 허락하고 열어 확인하므로 잠근 쪽이 같은 순간 다시 열어도 막지 않는다.
    [[nodiscard]] bool file_lock_held(const std::filesystem::path& file);

    // JSONL 기록의 추가분만 읽는다. 세 수집기가 같은 규칙으로 따라간다.
    //  - 끝에 줄바꿈이 없는 마지막 줄은 다음 읽기까지 미룬다. 기록이 쓰는 도중의 줄을 반쪽으로 해석하지 않는다.
    //  - 파일이 잘리거나 다른 파일로 교체되면 `restart`를 부르고 처음부터 다시 읽는다.
    //  - 상한보다 긴 줄은 상태 해석에 쓰지 않는 큰 원문이라 줄 끝까지 버린다.
    class jsonl_tail final
    {
    public:
        explicit jsonl_tail(std::size_t maximum_line);
        void read(const std::filesystem::path& path, const std::function<void()>& restart, const std::function<void(const nlohmann::json&)>& consume);
        // 지금까지의 파일 끝까지 읽었는가다. 처음 읽는 큰 기록은 여러 주기에 나눠 읽는다.
        [[nodiscard]] bool ready() const noexcept;

    private:
        std::size_t maximum_line_ { 0 };
        std::uint64_t position_ { 0 };
        std::array<unsigned long, 3> identity_ {};
        bool has_identity_ { false };
        std::string partial_ {};
        bool dropping_ { false };
        bool ready_ { false };
    };
} // namespace session_board
