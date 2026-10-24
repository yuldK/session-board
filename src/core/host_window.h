#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace session_board {
    // 세션이 열린 최상위 창이다 (Claude Code·Codex가 도는 터미널이나 데스크톱 앱).
    struct host_window
    {
        std::uint64_t window { 0 };
        std::uint32_t process_id { 0 };
    };

    // 지금 프로세스(Hook)를 띄운 쪽에서 사용자가 보는 창을 찾는다.
    //  - 콘솔에 붙어 있으면 그 콘솔을 그리는 창이다 (Windows Terminal은 의사 콘솔 창의 소유자다).
    //  - 아니면 부모 프로세스를 거슬러 올라가며 보이는 최상위 창을 찾는다. 한 프로세스에 창이 여럿이면
    //    제목에 작업 폴더 이름이 든 창을 고른다.
    //  - 못 찾으면 비어 있다. 이동 단추를 위한 덤이라 실패해도 이벤트는 그대로 저장한다.
    [[nodiscard]] std::optional<host_window> find_host_window(std::wstring_view folder_hint);

    // 기록해 둔 창을 앞으로 가져온다. 창이 사라졌거나 다른 프로세스의 창이 됐으면 거짓이다.
    [[nodiscard]] bool focus_host_window(const host_window& target);
} // namespace session_board
