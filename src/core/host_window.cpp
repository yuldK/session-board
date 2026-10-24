#include "core/host_window.h"

#include <windows.h>

#include <tlhelp32.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace session_board {
    namespace {
        // 부모를 이만큼만 거슬러 올라간다. 셸·런처를 몇 겹 거쳐도 이 안에서 창을 만난다.
        constexpr int maximum_depth { 16 };

        bool is_user_window(const HWND window)
        {
            if (IsWindowVisible(window) == FALSE || GetWindow(window, GW_OWNER) != nullptr)
                return false;
            if ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0)
                return false;
            return GetWindowTextLengthW(window) > 0;
        }

        std::wstring window_title(const HWND window)
        {
            std::wstring title(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
            title.resize(static_cast<std::size_t>(GetWindowTextW(window, title.data(), static_cast<int>(title.size()))));
            return title;
        }

        struct window_search
        {
            DWORD process_id { 0 };
            std::vector<HWND> windows {};
        };

        BOOL CALLBACK collect_window(const HWND window, const LPARAM data)
        {
            auto* const search { reinterpret_cast<window_search*>(data) };
            DWORD owner { 0 };
            GetWindowThreadProcessId(window, &owner);
            if (owner == search->process_id && is_user_window(window))
                search->windows.push_back(window);
            return TRUE;
        }

        std::unordered_map<DWORD, DWORD> parent_map()
        {
            std::unordered_map<DWORD, DWORD> parents {};
            const HANDLE snapshot { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
            if (snapshot == INVALID_HANDLE_VALUE)
                return parents;
            PROCESSENTRY32W entry { .dwSize = sizeof(PROCESSENTRY32W) };
            for (BOOL more { Process32FirstW(snapshot, &entry) }; more != FALSE; more = Process32NextW(snapshot, &entry))
                parents[entry.th32ProcessID] = entry.th32ParentProcessID;
            CloseHandle(snapshot);
            return parents;
        }

        host_window describe(const HWND window)
        {
            DWORD process { 0 };
            GetWindowThreadProcessId(window, &process);
            return { .window = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(window)), .process_id = process };
        }
    } // namespace

    std::optional<host_window> find_host_window(const std::wstring_view folder_hint)
    {
        if (const HWND console { GetConsoleWindow() }; console != nullptr)
        {
            const HWND root { GetAncestor(console, GA_ROOTOWNER) };
            if (root != nullptr && IsWindowVisible(root) != FALSE)
                return describe(root);
        }
        const auto parents { parent_map() };
        DWORD process { GetCurrentProcessId() };
        for (int depth { 0 }; depth < maximum_depth; ++depth)
        {
            const auto parent { parents.find(process) };
            if (parent == parents.end() || parent->second == 0 || parent->second == process)
                break;
            process = parent->second;
            window_search search { .process_id = process };
            EnumWindows(collect_window, reinterpret_cast<LPARAM>(&search));
            if (search.windows.empty())
                continue;
            if (!folder_hint.empty())
                for (const HWND window : search.windows)
                    if (window_title(window).find(folder_hint) != std::wstring::npos)
                        return describe(window);
            return describe(search.windows.front());
        }
        return std::nullopt;
    }

    bool focus_host_window(const host_window& target)
    {
        const HWND window { reinterpret_cast<HWND>(static_cast<std::uintptr_t>(target.window)) };
        if (target.window == 0 || IsWindow(window) == FALSE)
            return false;
        // 창 핸들은 다시 쓰인다. 기록한 프로세스의 창일 때만 옮긴다.
        DWORD process { 0 };
        GetWindowThreadProcessId(window, &process);
        if (process != target.process_id)
            return false;
        if (IsIconic(window) != FALSE)
            ShowWindow(window, SW_RESTORE);
        return SetForegroundWindow(window) != FALSE;
    }
} // namespace session_board
