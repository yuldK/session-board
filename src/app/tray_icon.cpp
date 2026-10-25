#include "app/tray_icon.h"

#include "app/resource.h"
#include "app/single_instance.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <cwchar>

namespace session_board {
    namespace {
        constexpr UINT_PTR subclass_id { 3 };
        constexpr UINT icon_id { 1 };
        constexpr UINT toggle_item { 1 };
        constexpr UINT detail_item { 2 };
        constexpr UINT exit_item { 3 };
        constexpr UINT licenses_item { 4 };

        NOTIFYICONDATAW icon_data(const HWND window) noexcept
        {
            NOTIFYICONDATAW data {};
            data.cbSize = sizeof(data);
            data.hWnd = window;
            data.uID = icon_id;
            return data;
        }
    } // namespace

    tray_icon::~tray_icon()
    {
        if (window_ != nullptr)
        {
            remove();
            RemoveWindowSubclass(window_, subclass, subclass_id);
        }
        if (icon_ != nullptr)
            DestroyIcon(icon_);
    }

    void tray_icon::attach(const HWND window, std::function<void()> open_detail, std::function<void()> open_licenses)
    {
        if (window == nullptr || window_ != nullptr)
            return;
        // luil이 주 창에서 WM_APP 대역을 쓰므로 알림은 등록한 메시지로 받는다.
        callback_message_ = RegisterWindowMessageW(L"SessionBoard.Tray.v1");
        created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
        if (callback_message_ == 0 || !SetWindowSubclass(window, subclass, subclass_id, reinterpret_cast<DWORD_PTR>(this)))
            return;
        window_ = window;
        open_detail_ = std::move(open_detail);
        open_licenses_ = std::move(open_licenses);
        // 알림 영역은 작은 아이콘 크기로 그린다. 큰 그림을 줄이면 흐려진다.
        icon_
            = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_SESSION_BOARD), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        add();
    }

    void tray_icon::add() noexcept
    {
        auto data { icon_data(window_) };
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = callback_message_;
        data.hIcon = icon_;
        // 디버그·릴리스 위젯이 함께 뜨면 알림 영역에서 둘을 가른다.
        wcsncpy_s(data.szTip, build_flavor() == L"release" ? L"Session Board" : L"Session Board (debug)", _TRUNCATE);
        added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        if (!added_)
            return;
        // 버전 4는 아이콘 자리를 wParam으로 주고 키보드 선택과 메뉴 키를 따로 알린다.
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }

    void tray_icon::remove() noexcept
    {
        if (!added_)
            return;
        auto data { icon_data(window_) };
        Shell_NotifyIconW(NIM_DELETE, &data);
        added_ = false;
    }

    void tray_icon::toggle_widget() const
    {
        if (IsWindowVisible(window_) != FALSE && IsIconic(window_) == FALSE)
        {
            ShowWindow(window_, SW_HIDE);
            return;
        }
        ShowWindow(window_, IsIconic(window_) != FALSE ? SW_RESTORE : SW_SHOWNA);
        SetForegroundWindow(window_);
    }

    void tray_icon::show_menu(const POINT anchor)
    {
        const HMENU menu { CreatePopupMenu() };
        if (menu == nullptr)
            return;
        const bool visible { IsWindowVisible(window_) != FALSE && IsIconic(window_) == FALSE };
        AppendMenuW(menu, MF_STRING, toggle_item, visible ? L"위젯 숨기기" : L"위젯 보이기");
        AppendMenuW(menu, MF_STRING, detail_item, L"상세 화면 열기");
        AppendMenuW(menu, MF_STRING, licenses_item, L"오픈 소스 라이선스");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, exit_item, L"종료");
        SetMenuDefaultItem(menu, toggle_item, FALSE);
        // 메뉴 밖을 눌러 닫으려면 메뉴의 주인 창이 전경이어야 한다. 끝난 뒤 빈 메시지로 메뉴 상태를 정리한다.
        SetForegroundWindow(window_);
        const UINT align { GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0 ? static_cast<UINT>(TPM_RIGHTALIGN) : static_cast<UINT>(TPM_LEFTALIGN) };
        const UINT item { static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | align, anchor.x, anchor.y, window_, nullptr)) };
        PostMessageW(window_, WM_NULL, 0, 0);
        DestroyMenu(menu);
        // 종료는 위젯 닫기 단추와 같이 주 창 닫힘으로 앱을 끝낸다.
        if (item == toggle_item)
            toggle_widget();
        else if (item == detail_item && open_detail_)
            open_detail_();
        else if (item == licenses_item && open_licenses_)
            open_licenses_();
        else if (item == exit_item)
            PostMessageW(window_, WM_CLOSE, 0, 0);
    }

    LRESULT CALLBACK tray_icon::subclass(const HWND window, const UINT message, const WPARAM word, const LPARAM data, const UINT_PTR id, const DWORD_PTR reference)
    {
        auto* const state { reinterpret_cast<tray_icon*>(reference) };
        if (message == state->callback_message_)
        {
            const UINT event { LOWORD(data) };
            if (event == NIN_SELECT)
                state->toggle_widget();
            // Enter는 키 선택을 두 번 알리는 일이 있어 숨기지 않고 보이기만 한다.
            else if (event == NIN_KEYSELECT && IsWindowVisible(window) == FALSE)
                state->toggle_widget();
            else if (event == WM_CONTEXTMENU)
                state->show_menu({ GET_X_LPARAM(word), GET_Y_LPARAM(word) });
            return 0;
        }
        if (message == state->created_message_ && state->created_message_ != 0)
        {
            // 탐색기가 다시 뜨면 예전 아이콘은 이미 사라졌다. 남아 있어도 추가가 실패하지 않게 먼저 내린다.
            state->remove();
            if (!state->closing_)
                state->add();
        }
        else if (message == WM_CLOSE || (message == WM_ENDSESSION && word != FALSE))
        {
            // luil은 닫힘에서 app host를 거둔다. 그 뒤 메뉴로 거둔 host에 메시지를 보내지 않게 아이콘을 먼저 내린다.
            state->closing_ = true;
            state->remove();
        }
        else if (message == WM_NCDESTROY)
        {
            state->remove();
            RemoveWindowSubclass(window, subclass, id);
            state->window_ = nullptr;
        }
        return DefSubclassProc(window, message, word, data);
    }
} // namespace session_board
