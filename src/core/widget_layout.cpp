#include "core/widget_layout.h"

#include <algorithm>

namespace session_board {
    widget_layout fit_widget(const widget_area work, const int header_x, const int header_y, const int width, const int height, const int caption_height, bool header_bottom, const bool fixed,
        const int minimum_height) noexcept
    {
        const int work_width { std::max(1, work.right - work.left) };
        const int work_height { std::max(1, work.bottom - work.top) };
        const int caption { std::clamp(caption_height, 1, work_height) };
        const int x { std::clamp(header_x, work.left, work.right - std::clamp(width, 1, work_width)) };
        int y { std::clamp(header_y, work.top, work.bottom - caption) };
        // 정한 쪽은 뒤집어 피할 수 없으므로 목록이 최소 높이를 얻을 만큼 머릿줄을 화면 끝에서 뗀다.
        //  - 최소 높이보다 작게 맞추면 OS가 창 최소 크기로 다시 늘려 머릿줄이 엉뚱한 자리에 선다.
        if (fixed)
        {
            const int room { std::clamp(std::min(height, minimum_height), caption, work_height) };
            y = header_bottom ? std::max(y, work.top + room - caption) : std::min(y, work.bottom - room);
        }
        const int above { y + caption - work.top };
        const int below { work.bottom - y };
        const int current { header_bottom ? above : below };
        const int other { header_bottom ? below : above };
        if (!fixed && height > current && other > current)
            header_bottom = !header_bottom;
        const int fitted_height { std::clamp(height, caption, header_bottom ? above : below) };
        return { x, header_bottom ? y + caption - fitted_height : y, std::clamp(width, 1, work_width), fitted_height, header_bottom };
    }
} // namespace session_board
