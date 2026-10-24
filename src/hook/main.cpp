#include "core/claude_monitor.h"
#include "core/host_window.h"
#include "core/store.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <iostream>
#include <string>

int wmain(const int argc, wchar_t** argv)
{
    bool strict { false };
    bool normalized { false };
    try
    {
        std::filesystem::path directory {};
        std::wstring provider { L"codex" };
        for (int index { 1 }; index < argc; ++index)
        {
            const std::wstring argument { argv[index] };
            if (argument == L"--normalized")
                normalized = true;
            else if (argument == L"--strict")
                strict = true;
            else if (argument == L"--data-dir" && index + 1 < argc)
                directory = argv[++index];
            else if (argument == L"--provider" && index + 1 < argc && (std::wstring { argv[index + 1] } == L"codex" || std::wstring { argv[index + 1] } == L"claude"))
                provider = argv[++index];
            else
                throw std::runtime_error { "Unknown argument." };
        }
        if (directory.empty())
            directory = session_board::default_data_directory();
        // 대화 전체나 큰 도구 출력을 끝없이 메모리에 읽지 않는다.
        constexpr std::size_t maximum_input { 4 * 1024 * 1024 };
        std::string input {};
        char buffer[4096] {};
        while (std::cin)
        {
            std::cin.read(buffer, sizeof(buffer));
            input.append(buffer, static_cast<std::size_t>(std::cin.gcount()));
            if (input.size() > maximum_input)
                throw std::runtime_error { "Hook payload is too large." };
        }
        // PowerShell 파이프가 보내는 UTF-8 BOM도 받는다.
        if (input.starts_with("\xef\xbb\xbf"))
            input.erase(0, 3);
        const auto payload { nlohmann::json::parse(input) };
        auto event { normalized ? std::optional { session_board::parse_event(payload) } : provider == L"claude" ? session_board::claude_event(payload) : session_board::codex_event(payload) };
        if (event.has_value())
        {
            // 세션 창으로 이동하는 단추가 쓸 창이다. 찾지 못해도 이벤트는 저장한다.
            if (event->host_window == 0)
                try
                {
                    const std::filesystem::path folder { std::u8string { event->cwd.begin(), event->cwd.end() } };
                    if (const auto host { session_board::find_host_window(folder.filename().wstring()) }; host.has_value())
                    {
                        event->host_window = host->window;
                        event->host_process_id = host->process_id;
                    }
                }
                catch (...)
                {}
            session_board::save_event(directory, *event);
        }
    }
    catch (...)
    {
        std::fputs("Session Board: hook event could not be saved.\n", stderr);
        std::cout << "{}\n";
        return strict ? 1 : 0;
    }
    // Stop을 포함한 hook이 유효한 JSON만 받도록 한다. 수집 실패도 AI 작업을 막지 않는다.
    std::cout << "{}\n";
    return 0;
}
