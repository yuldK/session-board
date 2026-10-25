#include "core/hook_command.h"

#include "core/claude_monitor.h"
#include "core/host_window.h"
#include "core/store.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <stdexcept>

namespace session_board {
    int run_hook_command(const std::vector<std::u8string>& arguments, std::istream& input, std::ostream& output, std::ostream& errors)
    {
        bool strict { false };
        bool normalized { false };
        try
        {
            std::filesystem::path directory {};
            std::u8string provider { u8"codex" };
            for (std::size_t index { 0 }; index < arguments.size(); ++index)
            {
                const auto& argument { arguments[index] };
                if (argument == u8"--normalized")
                    normalized = true;
                else if (argument == u8"--strict")
                    strict = true;
                else if (argument == u8"--data-dir" && index + 1 < arguments.size())
                    directory = std::filesystem::path { arguments[++index] };
                else if (argument == u8"--provider" && index + 1 < arguments.size() && (arguments[index + 1] == u8"codex" || arguments[index + 1] == u8"claude"))
                    provider = arguments[++index];
                else
                    throw std::runtime_error { "Unknown argument." };
            }
            if (directory.empty())
                directory = default_data_directory();
            // 대화 전체나 큰 도구 출력을 끝없이 메모리에 읽지 않는다.
            constexpr std::size_t maximum_input { 4 * 1024 * 1024 };
            std::string text {};
            char buffer[4096] {};
            while (input)
            {
                input.read(buffer, sizeof(buffer));
                text.append(buffer, static_cast<std::size_t>(input.gcount()));
                if (text.size() > maximum_input)
                    throw std::runtime_error { "Hook payload is too large." };
            }
            // PowerShell 파이프가 보내는 UTF-8 BOM도 받는다.
            if (text.starts_with("\xef\xbb\xbf"))
                text.erase(0, 3);
            const auto payload { nlohmann::json::parse(text) };
            auto event { normalized ? std::optional { parse_event(payload) } : provider == u8"claude" ? claude_event(payload) : codex_event(payload) };
            if (event.has_value())
            {
                // 세션 창으로 이동하는 단추가 쓸 창이다. 찾지 못해도 이벤트는 저장한다.
                if (event->host_window == 0)
                    try
                    {
                        const std::filesystem::path folder { std::u8string { event->cwd.begin(), event->cwd.end() } };
                        if (const auto host { find_host_window(folder.filename().wstring()) }; host.has_value())
                        {
                            event->host_window = host->window;
                            event->host_process_id = host->process_id;
                        }
                    }
                    catch (...)
                    {}
                save_event(directory, *event);
            }
        }
        catch (...)
        {
            errors << "Session Board: hook event could not be saved.\n";
            output << "{}\n";
            output.flush();
            return strict ? 1 : 0;
        }
        // Stop을 포함한 hook이 유효한 JSON만 받도록 한다. 수집 실패도 AI 작업을 막지 않는다.
        output << "{}\n";
        output.flush();
        return 0;
    }
} // namespace session_board
