#pragma once

namespace session_board {
    struct widget_area
    {
        int left { 0 };
        int top { 0 };
        int right { 0 };
        int bottom { 0 };
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
    [[nodiscard]] widget_layout fit_widget(widget_area work, int header_x, int header_y, int width, int height, int caption_height, bool header_bottom) noexcept;
} // namespace session_board
