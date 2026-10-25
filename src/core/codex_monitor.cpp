#include "core/codex_monitor.h"

#include <shlobj.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <string_view>

namespace session_board {
    namespace {
        constexpr std::size_t maximum_line { 256 * 1024 };
        constexpr std::uint64_t read_budget { 8 * 1024 * 1024 };

        std::string text_field(const nlohmann::json& value, const char* key, const std::size_t limit = 360)
        {
            const auto found { value.find(key) };
            return found != value.end() && found->is_string() ? compact_text(found->get<std::string>(), limit) : std::string {};
        }

        std::string public_text(const nlohmann::json& value)
        {
            if (value.is_string())
                return value.get<std::string>();
            if (!value.is_array())
                return {};
            std::string result {};
            for (const auto& part : value)
                if (part.is_object() && (part.value("type", "") == "Text" || part.value("type", "") == "output_text"))
                    result += text_field(part, "text", 1024);
            return result;
        }

        std::int64_t record_time(const nlohmann::json& record)
        {
            const std::string text { text_field(record, "timestamp", 64) };
            if (text.size() < 19)
                return 0;
            SYSTEMTIME time {};
            try
            {
                time.wYear = static_cast<WORD>(std::stoi(text.substr(0, 4)));
                time.wMonth = static_cast<WORD>(std::stoi(text.substr(5, 2)));
                time.wDay = static_cast<WORD>(std::stoi(text.substr(8, 2)));
                time.wHour = static_cast<WORD>(std::stoi(text.substr(11, 2)));
                time.wMinute = static_cast<WORD>(std::stoi(text.substr(14, 2)));
                time.wSecond = static_cast<WORD>(std::stoi(text.substr(17, 2)));
                if (text.size() >= 23 && text[19] == '.')
                    time.wMilliseconds = static_cast<WORD>(std::stoi(text.substr(20, 3)));
            }
            catch (...)
            {
                return 0;
            }
            FILETIME file {};
            if (!SystemTimeToFileTime(&time, &file))
                return 0;
            const ULARGE_INTEGER ticks { file.dwLowDateTime, file.dwHighDateTime };
            return static_cast<std::int64_t>(ticks.QuadPart / 10'000) - 11'644'473'600'000;
        }

        std::string tool_activity(const std::string& name)
        {
            if (name.find("apply_patch") != std::string::npos)
                return "코드 수정 중 · apply_patch";
            if (name == "FileChange")
                return "파일 변경 중";
            if (name.find("request_user_input") != std::string::npos)
                return "사용자 답변 대기";
            if (name.find("exec") != std::string::npos || name == "CommandExecution" || name == "Bash")
                return "명령·도구 실행 중 · " + name;
            return "도구 실행 중 · " + name;
        }

        struct tracked_file
        {
            codex_rollout reader {};
            std::uint64_t position { 0 };
            std::string partial {};
            bool dropping { false };
            bool ready { false };
            std::optional<session_snapshot> published {};

            void read(const std::filesystem::path& path)
            {
                const auto size { std::filesystem::file_size(path) };
                if (size < position)
                    *this = tracked_file {};
                std::ifstream input { path, std::ios::binary };
                if (!input)
                    return;
                input.seekg(static_cast<std::streamoff>(position));
                std::array<char, 32 * 1024> buffer {};
                const auto until { std::min(size, position + read_budget) };
                while (position < until && input)
                {
                    input.read(buffer.data(), static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), until - position)));
                    const auto count { input.gcount() };
                    position += static_cast<std::uint64_t>(count);
                    for (std::streamsize index { 0 }; index < count; ++index)
                    {
                        const char character { buffer[static_cast<std::size_t>(index)] };
                        if (character == '\n')
                        {
                            if (!dropping && !partial.empty())
                            {
                                const auto value { nlohmann::json::parse(partial, nullptr, false) };
                                if (!value.is_discarded())
                                    try
                                    {
                                        reader.consume(value);
                                    }
                                    catch (...)
                                    {}
                            }
                            partial.clear();
                            dropping = false;
                        }
                        else if (!dropping)
                        {
                            if (partial.size() < maximum_line)
                                partial += character;
                            else
                            {
                                partial.clear();
                                dropping = true;
                            }
                        }
                    }
                }
                ready = position >= size;
            }
        };

        std::set<std::string> loaded_threads(const std::filesystem::path& directory)
        {
            std::set<std::string> result {};
            const auto locks { directory / "thread-writer-locks" };
            if (!std::filesystem::exists(locks))
                return result;
            for (const auto& entry : std::filesystem::directory_iterator { locks })
                if (entry.path().extension() == L".lock" && entry.path().stem().string().size() == 36 && codex_writer_active(entry.path()))
                    result.insert(entry.path().stem().string());
            return result;
        }

        std::map<std::string, std::string> thread_names(const std::filesystem::path& directory)
        {
            std::map<std::string, std::string> result {};
            std::ifstream input { directory / "session_index.jsonl", std::ios::binary };
            std::string line {};
            while (std::getline(input, line))
            {
                if (line.size() > maximum_line)
                    continue;
                const auto value { nlohmann::json::parse(line, nullptr, false) };
                if (value.is_object())
                    result[text_field(value, "id", 512)] = text_field(value, "thread_name", 180);
            }
            return result;
        }

        void discover(const std::filesystem::path& directory, const std::set<std::string>& loaded, std::map<std::filesystem::path, tracked_file>& files)
        {
            const auto sessions { directory / "sessions" };
            if (!std::filesystem::exists(sessions))
                return;
            using candidate = std::pair<std::filesystem::file_time_type, std::filesystem::path>;
            std::vector<candidate> candidates {};
            const auto cutoff { std::filesystem::file_time_type::clock::now() - std::chrono::hours { 48 } };
            for (const auto& entry : std::filesystem::recursive_directory_iterator { sessions, std::filesystem::directory_options::skip_permission_denied })
            {
                if (!entry.is_regular_file() || entry.path().extension() != L".jsonl")
                    continue;
                const std::string name { entry.path().stem().string() };
                const bool live { std::ranges::any_of(loaded, [&](const auto& id) { return name.ends_with(id); }) };
                const auto updated { entry.last_write_time() };
                if (live || updated >= cutoff)
                    candidates.emplace_back(live ? std::filesystem::file_time_type::max() : updated, entry.path());
            }
            std::ranges::sort(candidates, [](const auto& left, const auto& right) { return left.first > right.first; });
            if (candidates.size() > 64)
                candidates.resize(64);
            std::set<std::filesystem::path> keep {};
            for (const auto& [time, path] : candidates)
            {
                static_cast<void>(time);
                keep.insert(path);
                files.try_emplace(path);
            }
            std::erase_if(files, [&](const auto& item) { return !keep.contains(item.first); });
        }
    } // namespace

    void codex_rollout::public_message(const std::string& text)
    {
        std::string brief { text };
        const auto newline { brief.find('\n') };
        const auto sentence { brief.find(". ") };
        brief.resize(std::min({ brief.size(), newline == std::string::npos ? brief.size() : newline, sentence == std::string::npos ? brief.size() : sentence + 1 }));
        brief = compact_text(std::move(brief), 240);
        if (!brief.empty())
            snapshot_.summary = std::move(brief);
    }

    void codex_rollout::tool_started(const std::string& id, const std::string& name)
    {
        if (snapshot_.turn_closed)
            return;
        snapshot_.tool_name = name;
        snapshot_.activity = tool_activity(name);
        snapshot_.status = name.find("request_user_input") == std::string::npos ? session_status::running : session_status::waiting_input;
        if (!id.empty() && snapshot_.active_tools.size() < 64 && std::ranges::find(snapshot_.active_tools, id) == snapshot_.active_tools.end())
            snapshot_.active_tools.push_back(id);
    }

    void codex_rollout::tool_finished(const std::string& id)
    {
        if (snapshot_.turn_closed)
            return;
        std::erase(snapshot_.active_tools, id);
        if (snapshot_.active_tools.empty())
        {
            snapshot_.tool_name.clear();
            snapshot_.activity = "분석 또는 응답 작성 중";
            snapshot_.status = session_status::running;
        }
    }

    void codex_rollout::consume(const nlohmann::json& record)
    {
        if (!record.is_object() || !record.contains("payload") || !record["payload"].is_object())
            return;
        const auto& payload { record["payload"] };
        const std::string type { text_field(record, "type", 64) };
        if (type == "session_meta")
        {
            const auto source { payload.find("source") };
            root_ = source == payload.end() || !source->is_object();
            snapshot_.provider = "codex";
            snapshot_.session_id = text_field(payload, "id", 512);
            // Codex 데스크톱 앱이 연 스레드만 앱에서 바로 연다. CLI·IDE 세션은 그 창을 앞으로 가져와야 한다.
            snapshot_.desktop_session_id = text_field(payload, "originator", 64) == "Codex Desktop" ? snapshot_.session_id : std::string {};
            snapshot_.cwd = text_field(payload, "cwd", 4096);
            snapshot_.observation_source = "codex-local";
            snapshot_.updated_ms = record_time(record);
            snapshot_.activity = "다음 요청 대기";
            return;
        }
        if (!root_ || snapshot_.session_id.empty())
            return;
        if (type == "turn_context")
        {
            snapshot_.model = text_field(payload, "model", 128);
            const auto cwd { text_field(payload, "cwd", 4096) };
            if (!cwd.empty())
                snapshot_.cwd = cwd;
            return;
        }
        const auto update = [&] {
            const auto time { record_time(record) };
            if (time > 0)
                snapshot_.updated_ms = std::max(snapshot_.updated_ms, time);
            ++snapshot_.revision;
        };
        const std::string event { text_field(payload, "type", 80) };
        if (type == "event_msg")
        {
            if (event == "task_started")
            {
                snapshot_.turn_id = text_field(payload, "turn_id", 512);
                snapshot_.turn_closed = false;
                snapshot_.status = session_status::running;
                snapshot_.summary.clear();
                snapshot_.tool_name.clear();
                snapshot_.active_tools.clear();
                snapshot_.activity = "작업 내용 수신 대기";
            }
            else if (event == "task_complete" || event == "turn_aborted" || event == "task_failed")
            {
                const auto turn { text_field(payload, "turn_id", 512) };
                if (!turn.empty() && !snapshot_.turn_id.empty() && turn != snapshot_.turn_id)
                    return;
                snapshot_.turn_closed = true;
                snapshot_.status = event == "task_complete" ? session_status::idle : event == "turn_aborted" ? session_status::interrupted : session_status::failed;
                snapshot_.tool_name.clear();
                snapshot_.active_tools.clear();
                snapshot_.activity = event == "task_complete" ? "응답 완료" : event == "turn_aborted" ? "사용자가 작업 중단" : "작업 오류";
                if (event == "task_complete")
                    public_message(text_field(payload, "last_agent_message", 1024));
            }
            else if (event == "agent_message")
                public_message(text_field(payload, "message", 1024));
            else if (event == "exec_approval_request" || event == "apply_patch_approval_request" || event == "request_user_input")
            {
                snapshot_.status = event == "request_user_input" ? session_status::waiting_input : session_status::waiting_approval;
                snapshot_.activity = event == "request_user_input" ? "사용자 답변 대기" : "사용자 실행 승인 대기";
            }
            else if ((event == "item_started" || event == "item_completed") && payload.contains("item") && payload["item"].is_object())
            {
                const auto& item { payload["item"] };
                const std::string item_type { text_field(item, "type", 64) };
                if (item_type == "AgentMessage" && event == "item_completed" && text_field(item, "phase", 32) != "analysis")
                    public_message(public_text(item.value("content", nlohmann::json::array())));
                else if (item_type == "CommandExecution" || item_type == "FileChange" || item_type == "McpToolCall" || item_type == "DynamicToolCall")
                    if (event == "item_started")
                        tool_started(text_field(item, "id", 512), item_type);
                    else
                        tool_finished(text_field(item, "id", 512));
                else
                    return;
            }
            else if (event == "exec_command_begin" || event == "patch_apply_begin")
                tool_started(text_field(payload, "call_id", 512), event == "exec_command_begin" ? "Bash" : "apply_patch");
            else if (event == "exec_command_end" || event == "patch_apply_end")
                tool_finished(text_field(payload, "call_id", 512));
            else
                return;
            snapshot_.last_event = event;
            update();
        }
        else if (type == "response_item")
        {
            if (event == "message" && text_field(payload, "role", 32) == "assistant" && text_field(payload, "phase", 32) != "analysis")
                public_message(public_text(payload.value("content", nlohmann::json::array())));
            else if (event == "function_call" || event == "custom_tool_call")
            {
                std::string name { text_field(payload, "name", 128) };
                if (name == "functions.exec" && payload.contains("input") && payload["input"].is_string()
                    && payload["input"].get_ref<const std::string&>().find("tools.apply_patch(") != std::string::npos)
                    name = "apply_patch";
                tool_started(text_field(payload, "call_id", 512), name);
            }
            else if (event == "function_call_output" || event == "custom_tool_call_output")
                tool_finished(text_field(payload, "call_id", 512));
            else
                return;
            update();
        }
    }

    const session_snapshot& codex_rollout::snapshot() const noexcept
    {
        return snapshot_;
    }

    bool codex_rollout::root_session() const noexcept
    {
        return root_;
    }

    std::filesystem::path default_codex_directory()
    {
        std::array<wchar_t, 32768> value {};
        const auto count { GetEnvironmentVariableW(L"CODEX_HOME", value.data(), static_cast<DWORD>(value.size())) };
        if (count > 0 && count < value.size())
            return value.data();
        PWSTR profile { nullptr };
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)))
            return {};
        const auto result { std::filesystem::path { profile } / L".codex" };
        CoTaskMemFree(profile);
        return result;
    }

    bool codex_writer_active(const std::filesystem::path& lock_file)
    {
        const HANDLE file { CreateFileW(lock_file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
        if (file == INVALID_HANDLE_VALUE)
            return GetLastError() == ERROR_SHARING_VIOLATION;
        OVERLAPPED overlapped {};
        const bool free { LockFileEx(file, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped) != FALSE };
        const DWORD error { GetLastError() };
        if (free)
            UnlockFileEx(file, 0, 1, 0, &overlapped);
        CloseHandle(file);
        return !free && error == ERROR_LOCK_VIOLATION;
    }

    std::wstring codex_desktop_link(const std::string_view thread_id)
    {
        constexpr std::array<std::size_t, 4> dashes { 8, 13, 18, 23 };
        if (thread_id.size() != 36)
            return {};
        for (std::size_t index { 0 }; index < thread_id.size(); ++index)
        {
            const char character { thread_id[index] };
            const bool dash { std::ranges::find(dashes, index) != dashes.end() };
            const bool hex { (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F') };
            if (dash ? character != '-' : !hex)
                return {};
        }
        return L"codex://threads/" + std::wstring { thread_id.begin(), thread_id.end() };
    }

    void merge_codex_sessions(std::vector<session_snapshot>& sessions, const std::vector<session_snapshot>& observed)
    {
        for (auto local : observed)
        {
            const auto found { std::ranges::find_if(sessions, [&](const auto& session) { return session.provider == "codex" && session.session_id == local.session_id; }) };
            if (found == sessions.end())
                sessions.push_back(std::move(local));
            else
            {
                if (local.title.empty())
                    local.title = found->title;
                // 세션 창은 Hook만 안다. 로컬 기록으로 덮어도 잃지 않는다.
                local.host_window = found->host_window;
                local.host_process_id = found->host_process_id;
                if (found->updated_ms > local.updated_ms)
                {
                    const bool different_turn { !local.turn_id.empty() && !found->turn_id.empty() && local.turn_id != found->turn_id };
                    if (!different_turn || found->last_event == "turn_started")
                    {
                        if (different_turn)
                            local.summary = found->summary;
                        local.turn_id = found->turn_id;
                        local.last_event = found->last_event;
                        local.status = found->status;
                        local.turn_closed = found->turn_closed;
                        local.tool_name = found->tool_name;
                        local.active_tools = found->active_tools;
                        local.updated_ms = found->updated_ms;
                        local.activity.clear();
                    }
                }
                // 하위 행은 아직 Hook 경로만 안다. 로컬 기록으로 덮어도 잃지 않는다.
                merge_children(local.children, found->children);
                *found = std::move(local);
            }
        }
        std::ranges::stable_sort(sessions, [](const auto& left, const auto& right) { return left.updated_ms > right.updated_ms; });
    }

    codex_monitor::codex_monitor(std::filesystem::path directory)
        : worker_ { [this, directory = std::move(directory)](const std::stop_token stop) { run(stop, directory); } }
    {}

    codex_monitor::~codex_monitor()
    {
        worker_.request_stop();
        wake_.notify_all();
    }

    codex_observation codex_monitor::snapshot() const
    {
        const std::lock_guard lock { mutex_ };
        return observation_;
    }

    void codex_monitor::run(const std::stop_token stop, const std::filesystem::path& directory)
    {
        std::map<std::filesystem::path, tracked_file> files {};
        std::map<std::string, std::string> names {};
        auto next_discovery { std::chrono::steady_clock::time_point {} };
        while (!stop.stop_requested())
        {
            codex_observation result {};
            result.ready = true;
            try
            {
                const auto loaded { loaded_threads(directory) };
                const auto now { std::chrono::steady_clock::now() };
                if (now >= next_discovery)
                {
                    discover(directory, loaded, files);
                    names = thread_names(directory);
                    next_discovery = now + std::chrono::seconds { 5 };
                }
                for (auto& [path, file] : files)
                {
                    if (stop.stop_requested())
                        break;
                    try
                    {
                        file.read(path);
                        if (!file.reader.root_session())
                            continue;
                        if (file.ready)
                            file.published = file.reader.snapshot();
                        if (!file.published.has_value())
                        {
                            if (loaded.contains(file.reader.snapshot().session_id))
                                result.ready = false;
                            continue;
                        }
                        auto session { *file.published };
                        session.runtime_loaded = loaded.contains(session.session_id);
                        if (const auto name { names.find(session.session_id) }; name != names.end())
                            session.name = name->second;
                        if (!session.runtime_loaded && now_ms() - session.updated_ms >= widget_retention_ms)
                            continue;
                        result.sessions.push_back(std::move(session));
                    }
                    catch (...)
                    {
                        result.error = "일부 Codex 로컬 기록을 읽지 못했습니다.";
                    }
                }
            }
            catch (...)
            {
                result.error = "Codex 자동 수집 경로를 읽지 못했습니다. Hooks 수집은 계속됩니다.";
            }
            {
                const std::lock_guard lock { mutex_ };
                observation_ = std::move(result);
            }
            std::unique_lock wait { wait_mutex_ };
            wake_.wait_for(wait, stop, std::chrono::seconds { 1 }, [] { return false; });
        }
    }
} // namespace session_board
