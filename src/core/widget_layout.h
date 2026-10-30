#pragma once

namespace session_board {
    struct widget_area
    {
        int left { 0 };
        int top { 0 };
        int right { 0 };
        int bottom { 0 };
    };

    // 머릿줄을 위젯의 어느 끝에 둘지다. 자동은 목록이 들어갈 수 있는 쪽으로 뒤집는다.
    enum class header_placement
    {
        automatic,
        top,
        bottom,
    };

    struct widget_layout
    {
        int x { 0 };
        int y { 0 };
        int width { 0 };
        int height { 0 };
        bool header_bottom { false };
    };

    // 머릿줄의 화면 좌표를 유지하며, 들어갈 수 있는 쪽으로 목록을 펼친다.
    //  - `fixed`면 방향을 뒤집지 않고 그쪽에 남은 자리로 목록 높이를 줄인다. 그 자리가 `minimum_height`보다 좁으면 머릿줄을 옮긴다.
    [[nodiscard]] widget_layout fit_widget(widget_area work, int header_x, int header_y, int width, int height, int caption_height, bool header_bottom, bool fixed, int minimum_height) noexcept;
} // namespace session_board
