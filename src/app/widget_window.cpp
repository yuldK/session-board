#include "app/widget_window.h"
#include "core/widget_layout.h"

#include <dwmapi.h>

#include <algorithm>
#include <cwchar>
#include <iterator>

namespace session_board {
    namespace {
        constexpr UINT_PTR subclass_id { 1 };
        constexpr UINT_PTR layout_timer_id { 0x53424c59 };
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

        // 색 없는 흐림을 거는 Windows 비공개 API다 (user32의 SetWindowCompositionAttribute, WCA_ACCENT_POLICY).
        //  - 공개 API(DWMWA_SYSTEMBACKDROP_TYPE)는 Acrylic의 색 막을 뺄 수 없어 이것을 쓴다.
        //  - 모양은 널리 쓰이는 비공개 정의를 따른다. 없는 Windows에서는 함수가 비어 Acrylic으로 물러선다.
        constexpr int accent_disabled { 0 };
        constexpr int accent_blur_behind { 3 };
        constexpr int window_composition_accent_policy { 19 };

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

        void set_accent(const HWND window, const int state)
        {
            const auto function { composition_function() };
            if (function == nullptr)
                return;
            accent_policy policy { .state = state };
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

    bool widget_window::attach(const bool keep_size, const bool header_bottom, std::function<void()> layout_changed, std::function<bool(std::uint64_t)> layout_ready)
    {
        EnumThreadWindows(GetCurrentThreadId(), find_main_window, reinterpret_cast<LPARAM>(&window_));
        if (window_ == nullptr)
            return false;
        if (!SetWindowSubclass(window_, subclass, subclass_id, reinterpret_cast<DWORD_PTR>(this)))
            return false;
        header_bottom_ = header_bottom;
        layout_changed_ = std::move(layout_changed);
        layout_ready_ = std::move(layout_ready);
        const LONG_PTR visible { GetWindowLongPtrW(window_, GWL_STYLE) & WS_VISIBLE };
        SetWindowLongPtrW(window_, GWL_STYLE, WS_POPUP | WS_THICKFRAME | visible);
        const LONG_PTR extended { GetWindowLongPtrW(window_, GWL_EXSTYLE) };
        SetWindowLongPtrW(window_, GWL_EXSTYLE, (extended & ~(WS_EX_APPWINDOW | WS_EX_LAYERED)) | WS_EX_TOOLWINDOW);
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

    void widget_window::apply_composition() const
    {
        if (window_ == nullptr)
            return;
        // 100%면 비칠 자리가 없으므로 Acrylic도 알파 합성도 끈다. DWM이 뒤를 읽어 섞지 않는다.
        const bool opaque { fully_opaque(opacity_) };
        const bool plain { backdrop_ == widget_backdrop::blur && !opaque && composition_function() != nullptr };
        const bool blur { backdrop_ != widget_backdrop::transparent && !opaque && !plain };
        // Acrylic은 창 전체를 DWM 합성면으로 넓히고 깐다. 그림이 투명한 자리마다 흐린 뒤가 보인다.
        // 투명·색 없는 흐림·불투명은 합성면을 넓히지 않는다 — 넓히면 DWM이 테마 색 판을 깔아 뒤가 비치지 않는다.
        const MARGINS margins { blur ? MARGINS { -1, -1, -1, -1 } : MARGINS {} };
        DwmExtendFrameIntoClientArea(window_, &margins);
        const DWM_SYSTEMBACKDROP_TYPE type { blur ? DWMSBT_TRANSIENTWINDOW : DWMSBT_NONE };
        DwmSetWindowAttribute(window_, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type));
        // 빈 흐림 영역으로 blur-behind를 켜면 흐림 없이 그림의 알파대로만 합성한다. 색 없는 흐림도 이 위에 흐림만 더한다.
        const HRGN empty { CreateRectRgn(0, 0, -1, -1) };
        const DWM_BLURBEHIND behind { .dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION, .fEnable = blur || opaque ? FALSE : TRUE, .hRgnBlur = empty };
        DwmEnableBlurBehindWindow(window_, &behind);
        if (empty != nullptr)
            DeleteObject(empty);
        set_accent(window_, plain ? accent_blur_behind : accent_disabled);
        // 비치는 가장자리에 OS 테두리 선이 남지 않게 한다.
        const COLORREF border { DWMWA_COLOR_NONE };
        DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    }

    void widget_window::toggle_collapse()
    {
        RECT box {};
        GetWindowRect(window_, &box);
        if (pending_bounds_.has_value())
            box = *pending_bounds_;
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
        const auto layout { fit_widget({ work.left, work.top, work.right, work.bottom }, box.left, header_y, box.right - box.left, collapsed_ ? expanded_height_ : height, caption, header_bottom_) };
        header_bottom_ = layout.header_bottom;
        const int target_height { collapsed_ ? caption : layout.height };
        const int target_y { collapsed_ && header_bottom_ ? layout.y + layout.height - caption : layout.y };
        pending_bounds_ = RECT { layout.x, target_y, layout.x + layout.width, target_y + target_height };
        ++layout_revision_;
        SetTimer(window_, layout_timer_id, 10, nullptr);
    }

    void widget_window::apply_pending_layout()
    {
        if (!pending_bounds_.has_value() || !layout_ready_ || !layout_ready_(layout_revision_))
            return;
        const RECT box { *pending_bounds_ };
        pending_bounds_.reset();
        KillTimer(window_, layout_timer_id);
        changing_collapse_ = true;
        // 새 크기의 tree가 준비된 뒤에만 창을 움직인다. 이전 비트맵을 새 원점에 복사하면 아래 머릿줄이 위로 튄다.
        SetWindowPos(window_, nullptr, box.left, box.top, box.right - box.left, box.bottom - box.top, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW | SWP_NOCOPYBITS);
        changing_collapse_ = false;
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOCHILDREN);
    }

    void widget_window::fit_position(WINDOWPOS& position)
    {
        if (pending_bounds_.has_value() && !changing_collapse_)
        {
            position.flags |= SWP_NOMOVE | SWP_NOSIZE;
            return;
        }
        RECT box {};
        GetWindowRect(window_, &box);
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
        const auto layout { fit_widget({ work.left, work.top, work.right, work.bottom }, x, header_y, width, collapsed_ ? expanded_height_ : height, caption, header_bottom_) };
        header_bottom_ = layout.header_bottom;
        const int fitted_header { layout.y + (header_bottom_ ? layout.height - caption : 0) };
        position.x = layout.x;
        position.y = collapsed_ ? fitted_header : layout.y;
        position.cx = layout.width;
        position.cy = collapsed_ ? std::min(caption, layout.height) : layout.height;
        position.flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        if (!changing_collapse_)
            expanded_height_ = layout.height;
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

    std::uint64_t widget_window::layout_revision() const noexcept
    {
        return layout_revision_;
    }

    std::optional<RECT> widget_window::pending_bounds() const noexcept
    {
        return pending_bounds_;
    }

    int widget_window::notification_room() const noexcept
    {
        RECT box {};
        GetWindowRect(window_, &box);
        if (pending_bounds_.has_value())
            box = *pending_bounds_;
        MONITORINFO monitor { .cbSize = sizeof(monitor) };
        if (!GetMonitorInfoW(MonitorFromRect(&box, MONITOR_DEFAULTTONEAREST), &monitor))
            return 0;
        return std::max(0L, header_bottom_ ? box.top - monitor.rcWork.top : monitor.rcWork.bottom - box.bottom);
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
            KillTimer(window, layout_timer_id);
            RemoveWindowSubclass(window, subclass, id);
            state->window_ = nullptr;
            return DefSubclassProc(window, message, word, data);
        }
        if (message == WM_TIMER && word == layout_timer_id)
        {
            state->apply_pending_layout();
            return 0;
        }
        if ((message == WM_PAINT || message == WM_PRINTCLIENT) && state->pending_bounds_.has_value())
        {
            state->apply_pending_layout();
            if (state->pending_bounds_.has_value())
            {
                if (message == WM_PAINT)
                    ValidateRect(window, nullptr);
                return 0;
            }
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
        if (message == WM_WINDOWPOSCHANGED && state->layout_changed_)
            state->layout_changed_();
        if (message == WM_DISPLAYCHANGE || message == WM_SETTINGCHANGE)
        {
            RECT box {};
            GetWindowRect(window, &box);
            SetWindowPos(window, nullptr, box.left, box.top, box.right - box.left, box.bottom - box.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (resets_composition(message))
            state->apply_composition();
        return result;
    }
} // namespace session_board
