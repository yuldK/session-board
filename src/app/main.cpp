#include "app/board_app.h"

#include <windows.h>

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
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
        config.caption = session_board::board_caption();
        // 주 창은 위젯이다. 상세 화면은 위젯에서 여는 독립 창이다.
        config.initial_width = session_board::widget_width;
        config.initial_height = session_board::widget_height;
        config.minimum_client_width = 320;
        config.minimum_client_height = 180;
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
        std::optional<session_board::codex_hook_result> codex_hook_result {};
        if (codex_hooks && discovery && !custom_data && !custom_codex && !codex_directory.empty())
            try
            {
                codex_hook_result = session_board::ensure_codex_hooks(codex_directory / L"hooks.json", session_board::bundled_hook_executable(), session_board::default_hook_install_directory());
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
                hook_result = session_board::ensure_claude_hooks(claude_directory / L"settings.json", session_board::bundled_hook_executable(), session_board::default_hook_install_directory());
            }
            catch (...)
            {
                hook_result = session_board::claude_hook_result { .error = "Claude Hook 설치 위치를 찾지 못했습니다." };
            }
        if (!claude_discovery || (custom_data && !custom_claude))
            claude_directory.clear();
        session_board::board_driver driver { std::move(directory), open_detail, std::move(codex_directory), std::move(claude_directory), std::move(hook_result), std::move(codex_hook_result) };
        session_board::board_delegate delegate {};
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
