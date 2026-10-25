#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace session_board {
    // 실행 파일에 실은 제3자 고지(`luil_generate_third_party_notices`)의 구성 요소 하나다.
    struct notice_component
    {
        std::string name {};
        std::string text {};
    };

    // 고지 원문을 `Component:` 절마다 나눈다. 첫 절 앞의 머리말은 버리고, 줄 끝은 `\n`으로 맞춘다.
    //  - Skia 패키지의 NOTICE.md도 같은 꼴의 절을 품으므로 그 안의 구성 요소도 한 줄씩 따로 선다.
    [[nodiscard]] std::vector<notice_component> parse_notices(std::string_view notices);

    // 원문을 한 줄에 `columns` 칸까지 나눈다. 칸은 글꼴 없이 센 어림이라 넓은 글자(한글·한자)는 두 칸이다.
    //  - 칸 안의 마지막 공백에서 먼저 자르고, 공백이 없으면 글자 경계에서 자른다.
    //  - 탭은 공백 넷으로 펴고 줄 끝의 `\r`은 지운다. 끝의 빈 줄은 남기지 않는다.
    [[nodiscard]] std::vector<std::string> wrap_notice_lines(std::string_view text, std::size_t columns);
} // namespace session_board
