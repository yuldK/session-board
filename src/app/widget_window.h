#pragma once

#include <windows.h>

#include <commctrl.h>

#include <cstdint>

namespace session_board {
    inline constexpr std::uint32_t toggle_collapse_command { 2 };
    inline constexpr std::uint32_t toggle_pin_command { 3 };
    inline constexpr std::uint32_t backdrop_blur_command { 4 };
    inline constexpr std::uint32_t backdrop_transparent_command { 5 };
    // 진하기를 인자의 값으로 둔다 (자석에 붙음, 보조 기술의 값 정하기). 인자는 0~1의 수다.
    inline constexpr std::uint32_t opacity_command { 6 };
    // 진하기를 인자만큼 바꾼다 (끌기, 키).
    inline constexpr std::uint32_t opacity_change_command { 7 };
    inline constexpr float widget_default_opacity { 0.35f };
    inline constexpr int widget_width { 400 };
    inline constexpr int widget_height { 560 };
    // 머릿줄은 창 너비 전체를 덮는다. 접은 위젯은 이 높이만 남는다.
    inline constexpr int widget_caption_height { 40 };
    // 머릿줄 오른쪽 단추 줄의 폭이다. 이 자리는 끌기가 아니라 단추로 답한다.
    inline constexpr int widget_control_width { 172 };

    // 목록 영역 뒤를 어떻게 보일지다. 앱 그림의 투명도는 그대로 두고 DWM이 뒤를 합성하는 방식만 바꾼다.
    // 진하기가 100%면 이 값과 관계없이 DWM 합성을 끄고 불투명 창으로 둔다.
    enum class widget_backdrop
    {
        // 뒤를 흐린다 (Windows 11 Acrylic).
        blur,
        // 뒤가 그대로 비친다.
        transparent,
    };

    // 진하기가 100%로 보이는 값이다. 표시는 백분율로 반올림하므로 판정도 같은 눈금을 쓴다.
    [[nodiscard]] inline bool fully_opaque(const float opacity) noexcept
    {
        return opacity >= 0.995f;
    }

    // 주 창을 위젯으로 쓴다: 테두리·작업 표시줄 없이 항상 위에 뜨고, 그림의 투명도대로 뒤가 비친다.
    // 상세 화면은 따로 뜨는 독립 창이라 이 창은 위젯 모양에서 바뀌지 않는다.
    class widget_window final
    {
    public:
        widget_window() = default;
        ~widget_window();
        widget_window(const widget_window&) = delete;
        widget_window& operator=(const widget_window&) = delete;
        // 저장한 배치로 연 창이면 `keep_size`를 켠다. luil이 이미 그 크기로 세웠다.
        [[nodiscard]] bool attach(bool keep_size);
        void set_backdrop(widget_backdrop backdrop);
        // 진하기를 0~1로 다듬어 둔다. 100%에 들어서거나 벗어날 때만 합성을 다시 건다.
        void set_opacity(float opacity);
        void toggle_collapse();
        void toggle_pin();
        [[nodiscard]] HWND handle() const noexcept;
        [[nodiscard]] bool collapsed() const noexcept;
        // 접기 전 높이(물리 픽셀)다. 접힌 채 닫아도 다음 실행은 펼친 크기로 연다.
        [[nodiscard]] int expanded_height() const noexcept;
        [[nodiscard]] bool pinned() const noexcept;
        [[nodiscard]] widget_backdrop backdrop() const noexcept;
        [[nodiscard]] float opacity() const noexcept;

    private:
        // luil이 창 상태가 바뀔 때 DWM 테두리를 자기 값으로 되돌리므로 그 뒤에 다시 건다.
        void apply_composition() const;
        static LRESULT CALLBACK subclass(HWND window, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR reference);
        HWND window_ { nullptr };
        bool collapsed_ { false };
        bool pinned_ { true };
        widget_backdrop backdrop_ { widget_backdrop::blur };
        float opacity_ { widget_default_opacity };
        int expanded_height_ { widget_height };
    };
} // namespace session_board
