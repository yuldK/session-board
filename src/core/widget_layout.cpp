#include "core/widget_layout.h"

#include <algorithm>

namespace session_board {
    widget_layout fit_widget(const widget_area work, const int header_x, const int header_y, const int width, const int height, const int caption_height, bool header_bottom) noexcept
    {
        const int work_width { std::max(1, work.right - work.left) };
        const int work_height { std::max(1, work.bottom - work.top) };
        const int caption { std::clamp(caption_height, 1, work_height) };
        const int x { std::clamp(header_x, work.left, work.right - std::clamp(width, 1, work_width)) };
        const int y { std::clamp(header_y, work.top, work.bottom - caption) };
        const int above { y + caption - work.top };
        const int below { work.bottom - y };
        const int current { header_bottom ? above : below };
        const int other { header_bottom ? below : above };
        if (height > current && other > current)
            header_bottom = !header_bottom;
        const int fitted_height { std::clamp(height, caption, header_bottom ? above : below) };
        return { x, header_bottom ? y + caption - fitted_height : y, std::clamp(width, 1, work_width), fitted_height, header_bottom };
    }
} // namespace session_board
