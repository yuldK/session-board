#include "app/single_instance.h"

#include "core/store.h"

#include <commctrl.h>

namespace session_board {
    namespace {
        constexpr UINT_PTR subclass_id { 2 };
        constexpr DWORD wait_ms { 3000 };

        struct window_search
        {
            const wchar_t* name { nullptr };
            HWND result { nullptr };
        };

        BOOL CALLBACK find_instance_window(const HWND window, const LPARAM reference)
        {
            auto& search { *reinterpret_cast<window_search*>(reference) };
            if (GetPropW(window, search.name) == nullptr)
                return TRUE;
            search.result = window;
            return FALSE;
        }

        // 같은 폴더를 다른 표기(대소문자·상대 경로)로 줘도 같은 잠금이 되게 한다.
        std::string directory_key(const std::filesystem::path& data_directory)
        {
            std::error_code error {};
            std::wstring text { std::filesystem::weakly_canonical(data_directory, error).wstring() };
            if (error)
                text = std::filesystem::absolute(data_directory).lexically_normal().wstring();
            if (!text.empty())
                CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
            const auto bytes { std::filesystem::path { text }.u8string() };
            return stable_key(std::string { bytes.begin(), bytes.end() });
        }
    } // namespace

    std::wstring_view build_flavor() noexcept
    {
#ifdef NDEBUG
        return L"release";
#else
        return L"debug";
#endif
    }

    single_instance::single_instance(const std::filesystem::path& data_directory)
    {
        try
        {
            const std::string key { directory_key(data_directory) };
            name_ = L"SessionBoard." + std::wstring { build_flavor() } + L"." + std::wstring { key.begin(), key.end() };
        }
        catch (...)
        {
            return;
        }
        activate_message_ = RegisterWindowMessageW(L"SessionBoard.Activate.v1");
        mutex_ = CreateMutexW(nullptr, FALSE, (L"Local\\" + name_).c_str());
    }

    single_instance::~single_instance()
    {
        if (window_ != nullptr)
        {
            RemovePropW(window_, name_.c_str());
            RemoveWindowSubclass(window_, subclass, subclass_id);
        }
        if (owned_)
            ReleaseMutex(mutex_);
        if (mutex_ != nullptr)
            CloseHandle(mutex_);
    }

    bool single_instance::acquire() noexcept
    {
        if (mutex_ == nullptr || activate_message_ == 0)
            return true;
        const ULONGLONG started { GetTickCount64() };
        while (true)
        {
            const DWORD result { WaitForSingleObject(mutex_, 0) };
            owned_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
            if (owned_)
                return true;
            window_search search { .name = name_.c_str() };
            EnumWindows(find_instance_window, reinterpret_cast<LPARAM>(&search));
            if (search.result != nullptr)
            {
                DWORD process { 0 };
                GetWindowThreadProcessId(search.result, &process);
                // 다시 실행한 쪽이 가진 전경 전환 권한을 떠 있는 위젯에 넘긴다.
                AllowSetForegroundWindow(process);
                PostMessageW(search.result, activate_message_, 0, 0);
                return false;
            }
            // 먼저 뜬 실행이 아직 창을 만들거나 닫는 중이면 잠금과 창을 다시 본다.
            if (GetTickCount64() - started >= wait_ms)
                return false;
            Sleep(25);
        }
    }

    void single_instance::bind_window(const HWND window) noexcept
    {
        if (!owned_ || window_ != nullptr || window == nullptr)
            return;
        if (!SetWindowSubclass(window, subclass, subclass_id, reinterpret_cast<DWORD_PTR>(this)))
            return;
        if (!SetPropW(window, name_.c_str(), reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(GetCurrentProcessId()))))
        {
            RemoveWindowSubclass(window, subclass, subclass_id);
            return;
        }
        window_ = window;
    }

    LRESULT CALLBACK single_instance::subclass(const HWND window, const UINT message, const WPARAM word, const LPARAM data, const UINT_PTR id, const DWORD_PTR reference)
    {
        auto* const state { reinterpret_cast<single_instance*>(reference) };
        if (message == state->activate_message_)
        {
            if (IsIconic(window))
                ShowWindow(window, SW_RESTORE);
            else
                ShowWindow(window, SW_SHOWNA);
            SetForegroundWindow(window);
            return 0;
        }
        if (message == WM_NCDESTROY)
        {
            RemovePropW(window, state->name_.c_str());
            RemoveWindowSubclass(window, subclass, id);
            state->window_ = nullptr;
        }
        return DefSubclassProc(window, message, word, data);
    }
} // namespace session_board
