#pragma once

#include <windows.h>

#include <functional>

namespace session_board {
    // 알림 영역의 아이콘이다. 위젯은 작업 표시줄에 서지 않으므로 숨긴 위젯을 다시 부르고 앱을 끝낼 자리가 여기다.
    //  - 아이콘을 누르면 위젯을 숨기거나 보인다. 오른쪽 단추·메뉴 키는 메뉴를 연다.
    //  - 등록에 실패해도 위젯은 그대로 돈다. 탐색기가 다시 뜨면 다시 등록한다.
    class tray_icon final
    {
    public:
        tray_icon() = default;
        ~tray_icon();
        tray_icon(const tray_icon&) = delete;
        tray_icon& operator=(const tray_icon&) = delete;
        // 위젯 창의 메시지로 아이콘 알림을 받는다. `open_detail`은 메뉴의 상세 화면 항목이 UI thread에서 부른다.
        void attach(HWND window, std::function<void()> open_detail);

    private:
        void add() noexcept;
        void remove() noexcept;
        void toggle_widget() const;
        void show_menu(POINT anchor);
        static LRESULT CALLBACK subclass(HWND window, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR reference);
        HWND window_ { nullptr };
        HICON icon_ { nullptr };
        UINT callback_message_ { 0 };
        UINT created_message_ { 0 };
        bool added_ { false };
        // 창을 닫기 시작했으면 탐색기가 다시 떠도 아이콘을 되살리지 않는다.
        bool closing_ { false };
        std::function<void()> open_detail_ {};
    };
} // namespace session_board
