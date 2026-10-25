#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace session_board {
    // 이 실행 파일의 빌드 구성 이름이다 ("release"·"debug"). 단일 실행 잠금과 창 배치 파일을 구성별로 가른다.
    [[nodiscard]] std::wstring_view build_flavor() noexcept;

    // 같은 빌드 구성·같은 데이터 폴더의 위젯은 하나만 띄운다.
    //  - Debug와 Release, 테스트용 `--data-dir`로 연 실행은 잠금 이름이 달라 서로 막지 않는다.
    //  - 잠금을 만들지 못하면 막지 않고 띄운다. 위젯을 못 띄우는 쪽이 둘 뜨는 쪽보다 나쁘다.
    class single_instance final
    {
    public:
        explicit single_instance(const std::filesystem::path& data_directory);
        ~single_instance();
        single_instance(const single_instance&) = delete;
        single_instance& operator=(const single_instance&) = delete;

        // 처음 뜬 실행이면 참이다. 이미 떠 있으면 그 위젯을 앞으로 부르고 거짓이다.
        [[nodiscard]] bool acquire() noexcept;
        // 다음 실행이 부를 때 답할 위젯 창이다.
        void bind_window(HWND window) noexcept;

    private:
        static LRESULT CALLBACK subclass(HWND window, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR reference);
        std::wstring name_ {};
        HANDLE mutex_ { nullptr };
        HWND window_ { nullptr };
        UINT activate_message_ { 0 };
        bool owned_ { false };
    };
} // namespace session_board
