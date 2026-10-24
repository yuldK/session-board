#pragma once

#include <windows.h>

#include <commctrl.h>

#include <cstdint>

namespace session_board {
    inline constexpr std::uint32_t toggle_collapse_command { 2 };
    inline constexpr std::uint32_t toggle_pin_command { 3 };
    inline constexpr std::uint32_t backdrop_blur_command { 4 };
    inline constexpr std::uint32_t backdrop_transparent_command { 5 };
    inline constexpr std::uint32_t backdrop_opaque_command { 6 };
    inline constexpr int widget_width { 400 };
    inline constexpr int widget_height { 560 };
    inline constexpr int widget_padding { 8 };
    inline constexpr int widget_caption_height { 36 };
    // 머릿줄 오른쪽 단추 줄의 폭이다. 이 자리는 끌기가 아니라 단추로 답한다.
    inline constexpr int widget_control_width { 164 };

    // 목록 영역 뒤를 어떻게 보일지다. 앱 그림의 투명도는 그대로 두고 DWM이 뒤를 합성하는 방식만 바꾼다.
    enum class widget_backdrop
    {
        // 뒤를 흐린다 (Windows 11 Acrylic).
        blur,
        // 뒤가 그대로 비친다.
        transparent,
        // 비치지 않는다.
        opaque,
    };

    // 주 창을 위젯으로 쓴다: 테두리·작업 표시줄 없이 항상 위에 뜨고, 그림의 투명도대로 뒤가 비친다.
    // 상세 화면은 따로 뜨는 독립 창이라 이 창은 위젯 모양에서 바뀌지 않는다.
    class widget_window final
    {
    public:
        widget_window() = default;
        ~widget_window();
        widget_window(const widget_window&) = delete;
        widget_window& operator=(const widget_window&) = delete;
        [[nodiscard]] bool attach();
        void set_backdrop(widget_backdrop backdrop);
        void toggle_collapse();
        void toggle_pin();
        [[nodiscard]] bool collapsed() const noexcept;
        [[nodiscard]] bool pinned() const noexcept;
        [[nodiscard]] widget_backdrop backdrop() const noexcept;

    private:
        // luil이 창 상태가 바뀔 때 DWM 테두리를 자기 값으로 되돌리므로 그 뒤에 다시 건다.
        void apply_composition() const;
        static LRESULT CALLBACK subclass(HWND window, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR reference);
        HWND window_ { nullptr };
        bool collapsed_ { false };
        bool pinned_ { true };
        widget_backdrop backdrop_ { widget_backdrop::blur };
        int expanded_height_ { widget_height };
    };
} // namespace session_board
