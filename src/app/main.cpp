#include "app/board_app.h"
#include "app/resource.h"
#include "core/hook_command.h"

#include <windows.h>

#include <iostream>

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    // Hook으로 불리면 창을 만들지 않고 이벤트 하나만 저장한다. 도구 호출마다 불리므로 단일 실행·DPI·COM 준비보다 먼저 가른다.
    //  - 창이 없는 실행 파일이어도 부모가 넘긴 표준 입출력 핸들로 읽고 쓴다.
    if (const auto arguments { luil::win32::command_line_arguments() }; arguments.has_value() && arguments->size() > 1 && (*arguments)[1] == session_board::hook_argument)
        return session_board::run_hook_command({ arguments->begin() + 2, arguments->end() }, std::cin, std::cout, std::cerr);
    try
    {
        luil::win32::enable_per_monitor_dpi_awareness();
        const luil::win32::com_sta_scope com {};
        std::filesystem::path directory { session_board::default_data_directory() };
        bool open_detail { false };
        std::filesystem::path codex_directory { session_board::default_codex_directory() };
        bool custom_data { false };
        bool custom_codex { false };
        bool discovery { true };
        bool codex_hooks { true };
        std::filesystem::path claude_directory { session_board::default_claude_directory() };
        bool custom_claude { false };
        bool claude_discovery { true };
        bool claude_hooks { true };
        luil::win32::window_config config {};
        config.class_name = L"SessionBoard.Main";
        config.caption = session_board::widget_caption();
        config.icon_resource = IDI_SESSION_BOARD;
        // 주 창은 위젯이다. 상세 화면은 위젯에서 여는 독립 창이다.
        config.initial_width = session_board::widget_width;
        config.initial_height = session_board::widget_height;
        config.minimum_client_width = 320;
        config.minimum_client_height = 180;
        config.resize_border_thickness = 6;
        config.resize_corner_thickness = 14;
        if (const auto arguments { luil::win32::command_line_arguments() }; arguments.has_value())
            for (std::size_t index { 1 }; index < arguments->size(); ++index)
            {
                const auto& argument { (*arguments)[index] };
                if (argument == u8"--data-dir" && index + 1 < arguments->size())
                {
                    directory = std::filesystem::path { (*arguments)[++index] };
                    custom_data = true;
                }
                else if (argument == u8"--codex-home" && index + 1 < arguments->size())
                {
                    codex_directory = std::filesystem::path { (*arguments)[++index] };
                    custom_codex = true;
                }
                else if (argument == u8"--no-codex-discovery")
                    discovery = false;
                else if (argument == u8"--no-codex-hooks")
                    codex_hooks = false;
                else if (argument == u8"--claude-home" && index + 1 < arguments->size())
                {
                    claude_directory = std::filesystem::path { (*arguments)[++index] };
                    custom_claude = true;
                }
                else if (argument == u8"--no-claude-discovery")
                    claude_discovery = false;
                else if (argument == u8"--no-claude-hooks")
                    claude_hooks = false;
                else if (argument == u8"--detail")
                    open_detail = true;
                // 예전 실행 인자다. 이제 위젯이 기본이라 하는 일이 없다.
                else if (argument == u8"--compact")
                    continue;
                else if (argument.starts_with(u8"--position="))
                    config.initial_position = luil::win32::parse_window_position(std::u8string_view { argument }.substr(11));
            }
        // 같은 구성·같은 데이터 폴더의 위젯이 떠 있으면 그 창을 앞으로 부르고 끝낸다. Hook 등록보다 먼저 본다.
        session_board::single_instance instance { directory };
        if (!instance.acquire())
            return 0;
        // 테스트 데이터로 연 실행은 사용자의 위젯 자리를 바꾸지 않는다. `--position`을 주면 저장한 자리보다 앞선다.
        std::filesystem::path placement_file {};
        std::filesystem::path detail_placement_file {};
        std::filesystem::path appearance_file {};
        session_board::widget_appearance appearance {};
        if (!custom_data)
        {
            placement_file = directory.parent_path() / (L"widget-" + std::wstring { session_board::build_flavor() } + L".json");
            detail_placement_file = directory.parent_path() / (L"detail-" + std::wstring { session_board::build_flavor() } + L".json");
            appearance_file = directory.parent_path() / (L"appearance-" + std::wstring { session_board::build_flavor() } + L".json");
            appearance = session_board::load_widget_appearance(appearance_file);
        }
        if (!placement_file.empty() && !config.initial_position.has_value())
            config.initial_placement = session_board::load_window_placement(placement_file);
        std::optional<session_board::codex_hook_result> codex_hook_result {};
        if (codex_hooks && discovery && !custom_data && !custom_codex && !codex_directory.empty())
            try
            {
                codex_hook_result = session_board::ensure_codex_hooks(codex_directory / L"hooks.json", session_board::app_executable(), session_board::default_hook_install_directory());
            }
            catch (...)
            {
                codex_hook_result = session_board::codex_hook_result { .error = "Codex Hook 설치 위치를 찾지 못했습니다." };
            }
        if (!discovery || (custom_data && !custom_codex))
            codex_directory.clear();
        // 테스트용 데이터·Claude 홈을 지정한 실행은 사용자의 실제 Claude 설정을 바꾸지 않는다.
        std::optional<session_board::claude_hook_result> hook_result {};
        if (claude_hooks && claude_discovery && !custom_data && !custom_claude && !claude_directory.empty())
            try
            {
                hook_result = session_board::ensure_claude_hooks(claude_directory / L"settings.json", session_board::app_executable(), session_board::default_hook_install_directory());
            }
            catch (...)
            {
                hook_result = session_board::claude_hook_result { .error = "Claude Hook 설치 위치를 찾지 못했습니다." };
            }
        if (!claude_discovery || (custom_data && !custom_claude))
            claude_directory.clear();
        session_board::board_driver driver {
            std::move(directory),
            open_detail,
            std::move(codex_directory),
            std::move(claude_directory),
            std::move(hook_result),
            std::move(codex_hook_result),
            std::move(detail_placement_file),
            appearance,
            std::move(appearance_file),
        };
        const bool header_bottom { config.initial_placement.has_value() && session_board::load_widget_header_bottom(placement_file) };
        session_board::board_delegate delegate { std::move(placement_file), config.initial_placement.has_value(), &instance, std::move(appearance), header_bottom };
        session_board::board_policy policy {};
        luil::win32::window_environment environment {};
        environment.driver = &driver;
        environment.delegate = &delegate;
        environment.policy = &policy;
        return luil::win32::run_application_window(config, environment);
    }
    catch (...)
    {
        MessageBoxW(nullptr, L"세션 저장 폴더 또는 창을 준비하지 못했습니다.", L"Session Board", MB_OK | MB_ICONERROR);
        return 1;
    }
}
