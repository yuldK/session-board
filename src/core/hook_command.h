#pragma once

#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace session_board {
    // 앱을 Hook으로 실행할 때 첫 인자다. 이 인자로 시작하면 창을 만들지 않고 이벤트 하나를 저장한 뒤 끝난다.
    inline constexpr std::u8string_view hook_argument { u8"--hook" };

    // Hook 이벤트 하나를 표준 입력에서 읽어 저장한다. `arguments`는 `--hook` 뒤의 인자다.
    //  - `--provider claude|codex`(기본 codex), `--normalized`, `--strict`, `--data-dir <폴더>`를 받는다.
    //  - 출력은 늘 `{}` 한 줄이다. Stop을 포함한 Hook이 유효한 JSON만 받는다.
    //  - 실패해도 0을 돌려 AI 작업을 막지 않는다. `--strict`일 때만 1이다 (테스트용).
    [[nodiscard]] int run_hook_command(const std::vector<std::u8string>& arguments, std::istream& input, std::ostream& output, std::ostream& errors);
} // namespace session_board
