#pragma once

#include "core/widget_layout.h"

#include <windows.h>

#include <commctrl.h>

#include <cstdint>
#include <functional>

namespace session_board {
    inline constexpr std::uint32_t toggle_collapse_command { 2 };
    inline constexpr std::uint32_t toggle_pin_command { 3 };
    inline constexpr std::uint32_t backdrop_acrylic_command { 4 };
    inline constexpr std::uint32_t backdrop_transparent_command { 5 };
    // 진하기를 인자의 값으로 둔다 (자석에 붙음, 보조 기술의 값 정하기). 인자는 0~1의 수다.
    inline constexpr std::uint32_t opacity_command { 6 };
    // 진하기를 인자만큼 바꾼다 (끌기, 키).
    inline constexpr std::uint32_t opacity_change_command { 7 };
    inline constexpr std::uint32_t backdrop_blur_command { 8 };
    inline constexpr std::uint32_t header_automatic_command { 9 };
    inline constexpr std::uint32_t header_top_command { 10 };
    inline constexpr std::uint32_t header_bottom_command { 11 };
    inline constexpr float widget_default_opacity { 0.35f };
    inline constexpr int widget_width { 400 };
    inline constexpr int widget_height { 560 };
    // 머릿줄은 창 너비 전체를 덮는다. 접은 위젯은 이 높이만 남는다.
    inline constexpr int widget_caption_height { 40 };

    // 목록 영역 뒤를 어떻게 보일지다. 앱 그림의 투명도는 그대로 두고 DWM이 뒤를 합성하는 방식만 바꾼다.
    // 진하기가 100%면 이 값과 관계없이 효과를 끄고 불투명하게 그린다.
    //  - 위젯은 레이어드 창이라 시스템 배경 효과(DWMSBT)가 단색 판이 된다. 흐림 둘은 비공개 accent API로 건다.
    //    그 API가 없는 Windows에서는 둘 다 투명처럼 뒤가 그대로 비친다.
    enum class widget_backdrop
    {
        // 뒤를 흐리고 명암에 맞춘 색 막을 덧씌운다 (accent acrylic).
        acrylic,
        // 뒤가 그대로 비친다.
        transparent,
        // 뒤를 흐리기만 하고 색을 덧씌우지 않는다.
        blur,
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
        // `dark`는 지금 앱 테마가 어두운가다. acrylic 색 막을 명암에 맞춘다.
        //  - 머릿줄 위치를 위·아래로 정해 두었으면 저장한 방향보다 그것을 따른다.
        [[nodiscard]] bool attach(bool keep_size, bool header_bottom, header_placement placement, std::function<void()> layout_changed, std::function<bool()> dark);
        void set_backdrop(widget_backdrop backdrop);
        // 진하기를 0~1로 다듬어 둔다. 100%에 들어서거나 벗어날 때만 합성을 다시 건다.
        void set_opacity(float opacity);
        void toggle_collapse();
        // 창 사각형은 그대로 두고 머릿줄을 정한 끝으로 옮긴다. 자동이면 지금 방향에서 다시 맞춘다.
        void set_header_placement(header_placement placement);
        void toggle_pin();
        [[nodiscard]] HWND handle() const noexcept;
        [[nodiscard]] bool collapsed() const noexcept;
        [[nodiscard]] bool header_bottom() const noexcept;
        [[nodiscard]] header_placement placement() const noexcept;
        // 마지막으로 맞춘 창 사각형이다. luil이 크기 변경을 새 frame과 함께 내려고 미루는 동안에도 그 목표 자리다.
        [[nodiscard]] RECT bounds() const noexcept;
        [[nodiscard]] int notification_room() const noexcept;
        // 창 왼쪽·오른쪽으로 작업 영역 끝까지 남은 폭(물리 픽셀)이다. 세부 정보 팝업을 띄울 쪽을 고른다.
        [[nodiscard]] int side_room(bool left) const noexcept;
        // 접기 전 높이(물리 픽셀)다. 접힌 채 닫아도 다음 실행은 펼친 크기로 연다.
        [[nodiscard]] int expanded_height() const noexcept;
        [[nodiscard]] bool pinned() const noexcept;
        [[nodiscard]] widget_backdrop backdrop() const noexcept;
        [[nodiscard]] float opacity() const noexcept;

    private:
        // luil이 창 상태가 바뀔 때 DWM 테두리를 자기 값으로 되돌리므로 그 뒤에 다시 건다.
        void apply_composition();
        // 머릿줄 화면 좌표를 두고 작업 영역 안에 창을 맞춘다. 높이는 접기 전 높이다.
        [[nodiscard]] widget_layout fit(const RECT& work, int x, int header_y, int width, int height, int caption) const noexcept;
        void fit_position(WINDOWPOS& position);
        static LRESULT CALLBACK subclass(HWND window, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR reference);
        HWND window_ { nullptr };
        RECT bounds_ {};
        bool collapsed_ { false };
        bool pinned_ { true };
        widget_backdrop backdrop_ { widget_backdrop::acrylic };
        float opacity_ { widget_default_opacity };
        int expanded_height_ { widget_height };
        bool header_bottom_ { false };
        header_placement placement_ { header_placement::automatic };
        bool changing_collapse_ { false };
        bool dragging_caption_ { false };
        POINT drag_offset_ {};
        std::function<void()> layout_changed_ {};
        std::function<bool()> dark_ {};
        // acrylic 색 막을 건 명암이다. 테마가 바뀌면 다시 건다.
        bool tinted_dark_ { true };
    };
} // namespace session_board
