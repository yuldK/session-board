#include "app/widget_window.h"

#include <dwmapi.h>

#include <algorithm>
#include <cwchar>
#include <iterator>

namespace session_board {
    namespace {
        constexpr UINT_PTR subclass_id { 1 };
        constexpr int widget_minimum_width { 320 };
        constexpr int widget_minimum_height { 180 };

        BOOL CALLBACK find_main_window(const HWND window, const LPARAM result)
        {
            wchar_t name[80] {};
            GetClassNameW(window, name, static_cast<int>(std::size(name)));
            if (std::wcscmp(name, L"SessionBoard.Main") == 0)
            {
                *reinterpret_cast<HWND*>(result) = window;
                return FALSE;
            }
            return TRUE;
        }

        // 흐림을 거는 Windows 비공개 API다 (user32의 SetWindowCompositionAttribute, WCA_ACCENT_POLICY).
        //  - 레이어드 창에서는 공개 API(DWMWA_SYSTEMBACKDROP_TYPE)가 흐림 없는 단색 판이 되어 Acrylic도 이것으로 건다.
        //  - 모양은 널리 쓰이는 비공개 정의를 따른다. 없는 Windows에서는 함수가 비어 효과 없이 뒤가 비친다.
        constexpr int accent_disabled { 0 };
        constexpr int accent_blur_behind { 3 };
        constexpr int accent_acrylic_blur_behind { 4 };
        constexpr int window_composition_accent_policy { 19 };
        // acrylic 색 막(AABBGGRR)이다. 플래그 0과 이 값이 시스템 Acrylic과 가장 가까운 밝기다 (luil 실측).
        constexpr DWORD acrylic_dark_tint { 0x40202020 };
        constexpr DWORD acrylic_light_tint { 0x40f3f3f3 };

        struct accent_policy
        {
            int state { 0 };
            int flags { 0 };
            DWORD gradient { 0 };
            int animation { 0 };
        };

        struct composition_data
        {
            int attribute { 0 };
            void* data { nullptr };
            SIZE_T size { 0 };
        };

        using set_composition = BOOL(WINAPI*)(HWND, composition_data*);

        set_composition composition_function()
        {
            static const auto function { reinterpret_cast<set_composition>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute")) };
            return function;
        }

        void set_accent(const HWND window, const int state, const DWORD tint = 0)
        {
            const auto function { composition_function() };
            if (function == nullptr)
                return;
            accent_policy policy { .state = state, .gradient = tint };
            composition_data data { .attribute = window_composition_accent_policy, .data = &policy, .size = sizeof(policy) };
            function(window, &data);
        }

        // 창 상태가 바뀌어 luil이 DWM 테두리를 다시 거는 메시지들이다.
        bool resets_composition(const UINT message) noexcept
        {
            return message == WM_ACTIVATE || message == WM_SIZE || message == WM_DPICHANGED || message == WM_DWMCOMPOSITIONCHANGED || message == WM_THEMECHANGED || message == WM_SETTINGCHANGE;
        }
    } // namespace

    widget_window::~widget_window()
    {
        if (window_ != nullptr)
            RemoveWindowSubclass(window_, subclass, subclass_id);
    }

    bool widget_window::attach(const bool keep_size, const bool header_bottom, const header_placement placement, std::function<void()> layout_changed, std::function<bool()> dark)
    {
        EnumThreadWindows(GetCurrentThreadId(), find_main_window, reinterpret_cast<LPARAM>(&window_));
        if (window_ == nullptr)
            return false;
        if (!SetWindowSubclass(window_, subclass, subclass_id, reinterpret_cast<DWORD_PTR>(this)))
            return false;
        placement_ = placement;
        header_bottom_ = placement == header_placement::automatic ? header_bottom : placement == header_placement::bottom;
        layout_changed_ = std::move(layout_changed);
        dark_ = std::move(dark);
        tinted_dark_ = dark_ && dark_();
        GetWindowRect(window_, &bounds_);
        const LONG_PTR visible { GetWindowLongPtrW(window_, GWL_STYLE) & WS_VISIBLE };
        SetWindowLongPtrW(window_, GWL_STYLE, WS_POPUP | WS_THICKFRAME | visible);
        // 레이어드 표시는 luil이 세운 WS_EX_LAYERED에 기댄다. 그것은 남긴다.
        const LONG_PTR extended { GetWindowLongPtrW(window_, GWL_EXSTYLE) };
        SetWindowLongPtrW(window_, GWL_EXSTYLE, (extended & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW);
        const DWM_WINDOW_CORNER_PREFERENCE corner { DWMWCP_ROUND };
        DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        const UINT dpi { GetDpiForWindow(window_) };
        const UINT size_flag { keep_size ? static_cast<UINT>(SWP_NOSIZE) : 0u };
        SetWindowPos(
            window_, HWND_TOPMOST, 0, 0, MulDiv(widget_width, static_cast<int>(dpi), 96), MulDiv(widget_height, static_cast<int>(dpi), 96), SWP_NOMOVE | SWP_NOACTIVATE | SWP_FRAMECHANGED | size_flag);
        apply_composition();
        return true;
    }

    void widget_window::set_backdrop(const widget_backdrop backdrop)
    {
        backdrop_ = backdrop;
        apply_composition();
    }

    void widget_window::set_opacity(const float opacity)
    {
        const bool was_opaque { fully_opaque(opacity_) };
        opacity_ = std::clamp(opacity, 0.0f, 1.0f);
        if (was_opaque != fully_opaque(opacity_))
            apply_composition();
    }

    void widget_window::apply_composition()
    {
        if (window_ == nullptr)
            return;
        // 레이어드 창은 그림의 알파대로 뒤와 섞이므로 투명은 따로 걸 것이 없다. 흐림만 accent로 깐다.
        //  - 100%면 비칠 자리가 없으므로 흐림도 끈다. DWM이 뒤를 읽어 섞지 않는다.
        //  - 흐림 판은 그림의 알파와 관계없이 창 사각형 전체에 깔리고, 모서리는 luil이 건 둥근 모서리가 깎는다.
        const bool opaque { fully_opaque(opacity_) };
        tinted_dark_ = dark_ && dark_();
        int accent { accent_disabled };
        if (!opaque && backdrop_ == widget_backdrop::acrylic)
            accent = accent_acrylic_blur_behind;
        else if (!opaque && backdrop_ == widget_backdrop::blur)
            accent = accent_blur_behind;
        set_accent(window_, accent, accent == accent_acrylic_blur_behind ? (tinted_dark_ ? acrylic_dark_tint : acrylic_light_tint) : 0);
        // 비치는 가장자리에 OS 테두리 선이 남지 않게 한다.
        const COLORREF border { DWMWA_COLOR_NONE };
        DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    }

    void widget_window::toggle_collapse()
    {
        // luil이 크기 변경을 새 frame과 함께 내려고 미루는 동안에는 창 사각형이 아직 옛 자리다. 목표 자리에서 셈한다.
        const RECT box { bounds_ };
        if (!collapsed_)
            expanded_height_ = box.bottom - box.top;
        const int caption { MulDiv(widget_caption_height, static_cast<int>(GetDpiForWindow(window_)), 96) };
        const int header_y { header_bottom_ ? box.bottom - caption : box.top };
        MONITORINFO monitor { .cbSize = sizeof(monitor) };
        if (!GetMonitorInfoW(MonitorFromRect(&box, MONITOR_DEFAULTTONEAREST), &monitor))
            return;
        collapsed_ = !collapsed_;
        const int height { collapsed_ ? caption : expanded_height_ };
        const auto& work { monitor.rcWork };
        const auto layout { fit(work, box.left, header_y, box.right - box.left, collapsed_ ? expanded_height_ : height, caption) };
        header_bottom_ = layout.header_bottom;
        const int target_height { collapsed_ ? caption : layout.height };
        const int target_y { collapsed_ && header_bottom_ ? layout.y + layout.height - caption : layout.y };
        // 레이어드 표시라 luil이 이 크기의 tree가 올 때까지 미뤘다가 그 frame과 한 번에 낸다. 접힘 상태는 그 크기의
        // metrics에 함께 실리므로 새 크기의 첫 tree가 새 상태도 담는다.
        changing_collapse_ = true;
        SetWindowPos(window_, nullptr, layout.x, target_y, layout.width, target_height, SWP_NOACTIVATE | SWP_NOZORDER);
        changing_collapse_ = false;
    }

    widget_layout widget_window::fit(const RECT& work, const int x, const int header_y, const int width, const int height, const int caption) const noexcept
    {
        const int minimum { MulDiv(widget_minimum_height, static_cast<int>(GetDpiForWindow(window_)), 96) };
        return fit_widget({ work.left, work.top, work.right, work.bottom }, x, header_y, width, height, caption, header_bottom_, placement_ != header_placement::automatic, minimum);
    }

    void widget_window::fit_position(WINDOWPOS& position)
    {
        const RECT box { bounds_ };
        const bool moving { (position.flags & SWP_NOMOVE) == 0 };
        const bool sizing { (position.flags & SWP_NOSIZE) == 0 };
        if (!moving && !sizing)
            return;
        const int width { sizing ? position.cx : box.right - box.left };
        const int height { sizing ? position.cy : box.bottom - box.top };
        const int caption { MulDiv(widget_caption_height, static_cast<int>(GetDpiForWindow(window_)), 96) };
        const int x { moving ? position.x : box.left };
        const int y { moving ? position.y : box.top };
        const int header_y { header_bottom_ ? y + height - caption : y };
        const RECT header { x, header_y, x + width, header_y + caption };
        MONITORINFO monitor { .cbSize = sizeof(monitor) };
        if (!GetMonitorInfoW(MonitorFromRect(&header, MONITOR_DEFAULTTONEAREST), &monitor))
            return;
        const auto& work { monitor.rcWork };
        const auto layout { fit(work, x, header_y, width, collapsed_ ? expanded_height_ : height, caption) };
        header_bottom_ = layout.header_bottom;
        const int fitted_header { layout.y + (header_bottom_ ? layout.height - caption : 0) };
        position.x = layout.x;
        position.y = collapsed_ ? fitted_header : layout.y;
        position.cx = layout.width;
        position.cy = collapsed_ ? std::min(caption, layout.height) : layout.height;
        position.flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        if (!changing_collapse_)
            expanded_height_ = layout.height;
        bounds_ = { position.x, position.y, position.x + position.cx, position.y + position.cy };
    }

    void widget_window::set_header_placement(const header_placement placement)
    {
        placement_ = placement;
        if (placement != header_placement::automatic)
            header_bottom_ = placement == header_placement::bottom;
        // 같은 사각형으로 다시 맞추면 바뀐 끝에 머릿줄을 두고 목록을 반대쪽으로 편다. 접혀 있으면 머릿줄 자리가 그대로다.
        const RECT box { bounds_ };
        SetWindowPos(window_, nullptr, box.left, box.top, box.right - box.left, box.bottom - box.top, SWP_NOZORDER | SWP_NOACTIVATE);
        // 사각형이 그대로면 옮김 알림이 오지 않는다. 머릿줄 방향이 바뀐 것은 여기서 알린다.
        if (layout_changed_)
            layout_changed_();
    }

    void widget_window::toggle_pin()
    {
        pinned_ = !pinned_;
        SetWindowPos(window_, pinned_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    HWND widget_window::handle() const noexcept
    {
        return window_;
    }

    bool widget_window::collapsed() const noexcept
    {
        return collapsed_;
    }

    bool widget_window::header_bottom() const noexcept
    {
        return header_bottom_;
    }

    header_placement widget_window::placement() const noexcept
    {
        return placement_;
    }

    RECT widget_window::bounds() const noexcept
    {
        return bounds_;
    }

    int widget_window::notification_room() const noexcept
    {
        const RECT box { bounds_ };
        MONITORINFO monitor { .cbSize = sizeof(monitor) };
        if (!GetMonitorInfoW(MonitorFromRect(&box, MONITOR_DEFAULTTONEAREST), &monitor))
            return 0;
        return std::max(0L, header_bottom_ ? box.top - monitor.rcWork.top : monitor.rcWork.bottom - box.bottom);
    }

    int widget_window::side_room(const bool left) const noexcept
    {
        const RECT box { bounds_ };
        MONITORINFO monitor { .cbSize = sizeof(monitor) };
        if (!GetMonitorInfoW(MonitorFromRect(&box, MONITOR_DEFAULTTONEAREST), &monitor))
            return 0;
        return std::max(0L, left ? box.left - monitor.rcWork.left : monitor.rcWork.right - box.right);
    }

    int widget_window::expanded_height() const noexcept
    {
        return expanded_height_;
    }

    bool widget_window::pinned() const noexcept
    {
        return pinned_;
    }

    widget_backdrop widget_window::backdrop() const noexcept
    {
        return backdrop_;
    }

    float widget_window::opacity() const noexcept
    {
        return opacity_;
    }

    LRESULT CALLBACK widget_window::subclass(const HWND window, const UINT message, const WPARAM word, const LPARAM data, const UINT_PTR id, const DWORD_PTR reference)
    {
        auto* const state { reinterpret_cast<widget_window*>(reference) };
        if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass(window, subclass, id);
            state->window_ = nullptr;
            return DefSubclassProc(window, message, word, data);
        }
        const int dpi { static_cast<int>(GetDpiForWindow(window)) };
        if (message == WM_NCCALCSIZE)
            return 0;
        // DWM은 비활성 창의 Acrylic을 단색 판으로 바꾼다. 위젯은 대개 비활성이므로 늘 활성으로 알린다.
        // lParam -1은 비클라이언트 영역을 다시 그리지 않게 한다.
        if (message == WM_NCACTIVATE)
            return DefWindowProcW(window, message, TRUE, -1);
        if (message == WM_GETMINMAXINFO)
        {
            const LRESULT result { DefSubclassProc(window, message, word, data) };
            auto* const limits { reinterpret_cast<MINMAXINFO*>(data) };
            const int minimum_height { state->collapsed_ ? widget_caption_height : widget_minimum_height };
            limits->ptMinTrackSize = { MulDiv(widget_minimum_width, dpi, 96), MulDiv(minimum_height, dpi, 96) };
            MONITORINFO monitor { .cbSize = sizeof(monitor) };
            if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
            {
                limits->ptMaxTrackSize = { monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top };
                limits->ptMinTrackSize.x = std::min(limits->ptMinTrackSize.x, limits->ptMaxTrackSize.x);
                limits->ptMinTrackSize.y = std::min(limits->ptMinTrackSize.y, limits->ptMaxTrackSize.y);
            }
            return result;
        }
        if (message == WM_WINDOWPOSCHANGING)
            state->fit_position(*reinterpret_cast<WINDOWPOS*>(data));
        if (message == WM_NCLBUTTONDOWN && word == HTCAPTION)
        {
            RECT box {};
            GetWindowRect(window, &box);
            POINT cursor {};
            GetCursorPos(&cursor);
            const int caption { MulDiv(widget_caption_height, dpi, 96) };
            state->drag_offset_ = { cursor.x - box.left, cursor.y - (state->header_bottom_ ? box.bottom - caption : box.top) };
            state->dragging_caption_ = true;
            const LRESULT result { DefSubclassProc(window, message, word, data) };
            state->dragging_caption_ = false;
            return result;
        }
        if (message == WM_MOVING && state->dragging_caption_)
        {
            auto& box { *reinterpret_cast<RECT*>(data) };
            POINT cursor {};
            GetCursorPos(&cursor);
            const int width { box.right - box.left };
            const int height { box.bottom - box.top };
            const int caption { MulDiv(widget_caption_height, dpi, 96) };
            box.left = cursor.x - state->drag_offset_.x;
            box.top = cursor.y - state->drag_offset_.y - (state->header_bottom_ ? height - caption : 0);
            box.right = box.left + width;
            box.bottom = box.top + height;
        }
        if (message == WM_NCHITTEST && state->collapsed_)
        {
            // 끌기·단추 판정은 게시된 캡션 tree를 따르는 luil에 맡긴다. 접힌 동안은 크기 조절만 막는다.
            const LRESULT hit { DefSubclassProc(window, message, word, data) };
            return hit >= HTLEFT && hit <= HTBOTTOMRIGHT ? HTCLIENT : hit;
        }
        const LRESULT result { DefSubclassProc(window, message, word, data) };
        // 자리만 옮긴 배치는 OS가 바로 옮기므로 머릿줄 방향·알림 자리를 여기서 알린다.
        // 크기가 바뀌는 배치는 luil이 미루며 그 크기의 metrics를 알린다. 그때 여기 오는 것은 옮기지 않은 알림이다.
        if (message == WM_WINDOWPOSCHANGED && state->layout_changed_ && (reinterpret_cast<const WINDOWPOS*>(data)->flags & SWP_NOMOVE) == 0)
            state->layout_changed_();
        if (message == WM_DISPLAYCHANGE || message == WM_SETTINGCHANGE)
        {
            const RECT box { state->bounds_ };
            SetWindowPos(window, nullptr, box.left, box.top, box.right - box.left, box.bottom - box.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        // 테마 변경은 luil이 새 frame을 그리며 반영한다. 명암이 바뀌었으면 acrylic 색 막과 테두리를 다시 건다.
        if (resets_composition(message) || (message == WM_PAINT && state->dark_ && state->dark_() != state->tinted_dark_))
            state->apply_composition();
        return result;
    }
} // namespace session_board
