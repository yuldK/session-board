#include "app/widget_window.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <iterator>

namespace session_board {
    namespace {
        constexpr UINT_PTR subclass_id { 1 };
        constexpr int widget_minimum_width { 320 };
        constexpr int widget_minimum_height { 180 };
        constexpr int widget_border { 6 };
        constexpr int widget_grip { 14 };

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

    bool widget_window::attach(const bool keep_size)
    {
        EnumThreadWindows(GetCurrentThreadId(), find_main_window, reinterpret_cast<LPARAM>(&window_));
        if (window_ == nullptr)
            return false;
        if (!SetWindowSubclass(window_, subclass, subclass_id, reinterpret_cast<DWORD_PTR>(this)))
            return false;
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
        const bool blur { backdrop_ == widget_backdrop::blur && !opaque };
        // 흐림은 창 전체를 DWM 합성면으로 넓히고 Acrylic을 깐다. 그림이 투명한 자리마다 흐린 뒤가 보인다.
        // 투명·불투명은 합성면을 넓히지 않는다 — 넓히면 DWM이 테마 색 판을 깔아 뒤가 비치지 않는다.
        const MARGINS margins { blur ? MARGINS { -1, -1, -1, -1 } : MARGINS {} };
        DwmExtendFrameIntoClientArea(window_, &margins);
        const DWM_SYSTEMBACKDROP_TYPE type { blur ? DWMSBT_TRANSIENTWINDOW : DWMSBT_NONE };
        DwmSetWindowAttribute(window_, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type));
        // 빈 흐림 영역으로 blur-behind를 켜면 흐림 없이 그림의 알파대로만 합성한다.
        const HRGN empty { CreateRectRgn(0, 0, -1, -1) };
        const DWM_BLURBEHIND behind { .dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION, .fEnable = blur || opaque ? FALSE : TRUE, .hRgnBlur = empty };
        DwmEnableBlurBehindWindow(window_, &behind);
        if (empty != nullptr)
            DeleteObject(empty);
        // 비치는 가장자리에 OS 테두리 선이 남지 않게 한다.
        const COLORREF border { DWMWA_COLOR_NONE };
        DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    }

    void widget_window::toggle_collapse()
    {
        RECT box {};
        GetWindowRect(window_, &box);
        if (!collapsed_)
            expanded_height_ = box.bottom - box.top;
        collapsed_ = !collapsed_;
        const int height { collapsed_ ? MulDiv(widget_caption_height, static_cast<int>(GetDpiForWindow(window_)), 96) : expanded_height_ };
        SetWindowPos(window_, nullptr, 0, 0, box.right - box.left, height, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
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
            return result;
        }
        if (message == WM_NCHITTEST)
        {
            POINT point { GET_X_LPARAM(data), GET_Y_LPARAM(data) };
            ScreenToClient(window, &point);
            RECT client {};
            GetClientRect(window, &client);
            const int edge { MulDiv(widget_border, dpi, 96) };
            const int grip { MulDiv(widget_grip, dpi, 96) };
            if (point.x >= client.right - grip && point.y >= client.bottom - grip)
                return HTBOTTOMRIGHT;
            const bool left { point.x < edge };
            const bool right { point.x >= client.right - edge };
            const bool top { point.y < edge };
            const bool bottom { point.y >= client.bottom - edge };
            if (top && left)
                return HTTOPLEFT;
            if (top && right)
                return HTTOPRIGHT;
            if (bottom && left)
                return HTBOTTOMLEFT;
            if (bottom && right)
                return HTBOTTOMRIGHT;
            if (left)
                return HTLEFT;
            if (right)
                return HTRIGHT;
            if (top)
                return HTTOP;
            if (bottom)
                return HTBOTTOM;
            if (point.y < MulDiv(widget_caption_height, dpi, 96) && point.x < client.right - MulDiv(widget_control_width, dpi, 96))
                return HTCAPTION;
            return HTCLIENT;
        }
        const LRESULT result { DefSubclassProc(window, message, word, data) };
        if (resets_composition(message))
            state->apply_composition();
        return result;
    }
} // namespace session_board
