#include "core/claude_monitor.h"

#include "core/local_record.h"

#include <shlobj.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string_view>

namespace session_board {
    namespace {
        constexpr std::size_t maximum_line { 512 * 1024 };
        constexpr std::uintmax_t maximum_runtime_file { 64 * 1024 };

        std::string text_field(const nlohmann::json& value, const char* key, const std::size_t limit = 360)
        {
            const auto found { value.find(key) };
            return found != value.end() && found->is_string() ? compact_text(found->get<std::string>(), limit) : std::string {};
        }

        // 줄바꿈으로 첫 줄을 고를 수 있게 제어 문자를 바꾸지 않은 앞부분을 돌려준다.
        std::string raw_text(const nlohmann::json& value, const char* key)
        {
            const auto found { value.find(key) };
            return found != value.end() && found->is_string() ? found->get_ref<const std::string&>().substr(0, 4096) : std::string {};
        }

        bool flag(const nlohmann::json& value, const char* key)
        {
            const auto found { value.find(key) };
            return found != value.end() && found->is_boolean() && found->get<bool>();
        }

        std::int64_t number_field(const nlohmann::json& value, const char* key)
        {
            const auto found { value.find(key) };
            return found != value.end() && found->is_number_integer() ? found->get<std::int64_t>() : 0;
        }

        // 공개 답변의 첫 줄·첫 문장만 카드 본문으로 쓴다.
        std::string brief_text(std::string text)
        {
            const auto start { text.find_first_not_of(" \t\r\n") };
            text.erase(0, start == std::string::npos ? text.size() : start);
            const auto newline { text.find('\n') };
            const auto sentence { text.find(". ") };
            text.resize(std::min({ text.size(), newline == std::string::npos ? text.size() : newline, sentence == std::string::npos ? text.size() : sentence + 1 }));
            return compact_text(std::move(text), 240);
        }

        std::int64_t record_time(const nlohmann::json& record)
        {
            return iso_time_ms(text_field(record, "timestamp", 64));
        }

        std::string tool_activity(const std::string& name)
        {
            if (name == "Read" || name == "NotebookRead")
                return "파일 읽는 중 · " + name;
            if (name == "Glob" || name == "Grep" || name == "LS")
                return "파일 검색 중 · " + name;
            if (name == "Edit" || name == "MultiEdit" || name == "Write" || name == "NotebookEdit")
                return "코드 수정 중 · " + name;
            if (name == "Bash" || name == "PowerShell")
                return "명령 실행 중 · " + name;
            if (name == "WebFetch" || name == "WebSearch")
                return "웹 조회 중 · " + name;
            if (name == "Agent" || name == "Task")
                return "하위 에이전트 실행 중 · " + name;
            if (name == "AskUserQuestion")
                return "사용자 답변 대기";
            return "도구 실행 중 · " + name;
        }

        // `<name>값</name>`을 모두 찾는다. 작업 알림은 짧은 고정 형식이라 XML 해석기를 쓰지 않는다.
        std::vector<std::string> tag_values(const std::string_view block, const std::string_view name)
        {
            const std::string open { "<" + std::string { name } + ">" };
            const std::string close { "</" + std::string { name } + ">" };
            std::vector<std::string> result {};
            std::size_t position { 0 };
            while ((position = block.find(open, position)) != std::string_view::npos)
            {
                position += open.size();
                const auto end { block.find(close, position) };
                if (end == std::string_view::npos)
                    break;
                result.push_back(compact_text(std::string { block.substr(position, end - position) }, 512));
                position = end + close.size();
            }
            return result;
        }

        std::optional<session_status> task_status(const std::string& text)
        {
            if (text == "completed")
                return session_status::idle;
            if (text == "failed")
                return session_status::failed;
            if (text == "stopped" || text == "killed")
                return session_status::interrupted;
            return std::nullopt;
        }

        bool waiting_for_input(const std::string& waiting_for)
        {
            // 2.1.308의 등록 정보는 승인 대화상자 밖의 대기를 이 값들로 남긴다.
            return waiting_for == "input needed" || waiting_for == "dialog open" || waiting_for == "goal proposal";
        }

        std::string local_domain()
        {
            std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> name {};
            DWORD size { static_cast<DWORD>(name.size()) };
            if (!GetComputerNameW(name.data(), &size))
                return {};
            std::string result { "win32:" };
            for (DWORD index { 0 }; index < size; ++index)
            {
                const wchar_t character { name[index] };
                if (character > 127)
                    return {};
                result += static_cast<char>(character >= L'A' && character <= L'Z' ? character - L'A' + L'a' : character);
            }
            return result;
        }

        struct tracked_file
        {
            claude_transcript reader {};
            jsonl_tail tail { maximum_line };
            bool ready { false };
            std::optional<session_snapshot> published {};

            // 파일이 잘리거나 교체되면 처음부터 다시 읽는다. 다 읽을 때까지 이전 결과(`published`)를 유지한다.
            void read(const std::filesystem::path& path)
            {
                tail.read(path, [this] { reader = claude_transcript { reader.sidechain() }; }, [this](const nlohmann::json& value) { reader.consume(value); });
                ready = tail.ready();
            }
        };

        struct tracked_agent
        {
            tracked_file transcript {};
            std::filesystem::file_time_type meta_time {};
            std::optional<claude_agent_meta> meta {};
        };

        // 부모 기록 하나에 딸린 `<세션>/subagents` 폴더다. 폴더가 바뀔 때만 다시 훑는다.
        struct agent_folder
        {
            std::filesystem::file_time_type scanned {};
            std::map<std::string, tracked_agent> agents {};
        };

        std::optional<nlohmann::json> small_json(const std::filesystem::path& path)
        {
            std::error_code error {};
            if (std::filesystem::file_size(path, error) > maximum_runtime_file || error)
                return std::nullopt;
            std::ifstream input { path, std::ios::binary };
            const std::string text { std::istreambuf_iterator<char> { input }, std::istreambuf_iterator<char> {} };
            auto value { nlohmann::json::parse(text, nullptr, false) };
            if (value.is_discarded())
                return std::nullopt;
            return value;
        }

        void read_agents(const std::filesystem::path& folder, agent_folder& state)
        {
            std::error_code error {};
            const auto changed { std::filesystem::last_write_time(folder, error) };
            if (error)
            {
                state = {};
                return;
            }
            // 폴더 시각은 파일이 생기거나 지워질 때 바뀐다. 기존 파일의 추가분은 아래에서 따로 읽는다.
            if (changed != state.scanned)
            {
                state.scanned = changed;
                std::vector<std::pair<std::filesystem::file_time_type, std::string>> found {};
                for (const auto& entry : std::filesystem::directory_iterator { folder, std::filesystem::directory_options::skip_permission_denied, error })
                {
                    constexpr std::string_view prefix { "agent-" };
                    constexpr std::string_view suffix { ".meta.json" };
                    const std::string name { entry.path().filename().string() };
                    if (name.size() <= prefix.size() + suffix.size() || !name.starts_with(prefix) || !name.ends_with(suffix) || name.size() > 160)
                        continue;
                    std::error_code time_error {};
                    const auto time { entry.last_write_time(time_error) };
                    if (!time_error)
                        found.emplace_back(time, name.substr(prefix.size(), name.size() - prefix.size() - suffix.size()));
                }
                std::ranges::sort(found, [](const auto& left, const auto& right) { return left.first > right.first; });
                if (found.size() > maximum_children)
                    found.resize(maximum_children);
                std::erase_if(state.agents, [&](const auto& item) { return std::ranges::none_of(found, [&](const auto& entry) { return entry.second == item.first; }); });
                for (const auto& entry : found)
                    if (const auto [item, inserted] { state.agents.try_emplace(entry.second) }; inserted)
                        item->second.transcript.reader = claude_transcript { true };
            }
            const auto cutoff { std::filesystem::file_time_type::clock::now() - std::chrono::milliseconds { widget_retention_ms } };
            for (auto& [id, agent] : state.agents)
            {
                const auto meta_path { folder / ("agent-" + id + ".meta.json") };
                std::error_code time_error {};
                const auto time { std::filesystem::last_write_time(meta_path, time_error) };
                if (time_error)
                    continue;
                if (time != agent.meta_time)
                {
                    agent.meta_time = time;
                    if (const auto value { small_json(meta_path) }; value.has_value())
                        agent.meta = parse_claude_agent_meta(*value);
                }
                // 끝난 하위는 한 번 다 읽으면 더 바뀌지 않는다. 오래전에 끝난 하위는 보이지 않으므로 큰 기록을 읽지 않는다.
                if (agent.meta.has_value() && !(agent.meta->finished && (time < cutoff || agent.transcript.ready)))
                    agent.transcript.read(folder / ("agent-" + id + ".jsonl"));
            }
        }

        std::map<std::string, claude_runtime> read_runtimes(const std::filesystem::path& directory)
        {
            std::map<std::string, claude_runtime> result {};
            const auto sessions { directory / "sessions" };
            if (!std::filesystem::exists(sessions))
                return result;
            for (const auto& entry : std::filesystem::directory_iterator { sessions, std::filesystem::directory_options::skip_permission_denied })
            {
                // `<pid>.json`만 읽는다. 같은 이름의 `.key` 파일은 자격 증명일 수 있어 열지 않는다.
                const std::wstring stem { entry.path().stem().wstring() };
                std::error_code error {};
                if (!entry.is_regular_file(error) || error || entry.path().extension() != L".json" || stem.empty()
                    || !std::ranges::all_of(stem, [](const wchar_t character) { return character >= L'0' && character <= L'9'; }))
                    continue;

                if (entry.file_size(error) > maximum_runtime_file || error)
                    continue;
                std::ifstream input { entry.path(), std::ios::binary };
                const std::string text { std::istreambuf_iterator<char> { input }, std::istreambuf_iterator<char> {} };
                const auto value { nlohmann::json::parse(text, nullptr, false) };
                if (value.is_discarded())
                    continue;
                if (auto runtime { parse_claude_runtime(value) }; runtime.has_value() && claude_process_alive(*runtime))
                {
                    const auto found { result.find(runtime->session_id) };
                    if (found == result.end() || found->second.updated_ms < runtime->updated_ms)
                        result[runtime->session_id] = std::move(*runtime);
                }
            }
            return result;
        }

        void discover(const std::filesystem::path& directory, const std::map<std::string, claude_runtime>& live, std::map<std::filesystem::path, tracked_file>& files)
        {
            const auto projects { directory / "projects" };
            if (!std::filesystem::exists(projects))
                return;
            using candidate = std::pair<std::filesystem::file_time_type, std::filesystem::path>;
            std::vector<candidate> candidates {};
            const auto cutoff { std::filesystem::file_time_type::clock::now() - std::chrono::milliseconds { widget_retention_ms } };
            for (const auto& project : std::filesystem::directory_iterator { projects, std::filesystem::directory_options::skip_permission_denied })
            {
                if (!project.is_directory())
                    continue;
                // 하위 에이전트 기록은 `<세션>/subagents` 아래에 있으므로 프로젝트 바로 아래 기록만 본다.
                for (const auto& entry : std::filesystem::directory_iterator { project.path(), std::filesystem::directory_options::skip_permission_denied })
                {
                    if (!entry.is_regular_file() || entry.path().extension() != L".jsonl")
                        continue;
                    const bool loaded { live.contains(entry.path().stem().string()) };
                    const auto updated { entry.last_write_time() };
                    if (loaded || updated >= cutoff)
                        candidates.emplace_back(loaded ? std::filesystem::file_time_type::max() : updated, entry.path());
                }
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

    std::optional<session_event> claude_event(const nlohmann::json& input)
    {
        if (!input.is_object())
            throw std::runtime_error { "Expected a hook object." };
        // 서브에이전트 안의 도구 Hook은 부모 session_id를 공유하므로 부모 상태에 섞지 않는다.
        if (!text_field(input, "agent_id", 512).empty())
            return std::nullopt;
        const std::string hook { text_field(input, "hook_event_name", 64) };
        const std::string tool { text_field(input, "tool_name", 128) };
        std::string event {};
        if (hook == "SessionStart")
            event = "session_started";
        else if (hook == "UserPromptSubmit")
            event = "turn_started";
        else if (hook == "PreToolUse")
            event = tool == "AskUserQuestion" ? "input_requested" : "tool_started";
        else if (hook == "PostToolUse" || hook == "PostToolUseFailure")
            event = "tool_finished";
        else if (hook == "PermissionRequest")
            event = "approval_requested";
        else if (hook == "Notification")
        {
            // idle_prompt는 응답 완료 약 60초 뒤의 알림일 뿐이고 permission_prompt는 PermissionRequest와 겹친다.
            const std::string type { text_field(input, "notification_type", 64) };
            if (type != "elicitation_dialog" && type != "elicitation_url_dialog")
                return std::nullopt;
            event = "input_requested";
        }
        else if (hook == "Stop")
            event = "turn_finished";
        else if (hook == "StopFailure")
            event = "failed";
        else if (hook == "SessionEnd")
            event = "session_ended";
        else
            return std::nullopt;

        nlohmann::json normalized { { "schema_version", 1 }, { "provider", "claude" }, { "session_id", text_field(input, "session_id", 513) }, { "event", event }, { "timestamp_ms", now_ms() } };
        normalized.update({ { "turn_id", text_field(input, "prompt_id", 512) }, { "cwd", text_field(input, "cwd", 4096) }, { "model", text_field(input, "model", 128) } });
        normalized.update({ { "tool_id", text_field(input, "tool_use_id", 512) }, { "tool_name", tool }, { "source", text_field(input, "source", 32) } });
        if (hook == "UserPromptSubmit")
            normalized["title"] = text_field(input, "prompt", 180);
        else if (hook == "StopFailure")
            normalized["failure"] = text_field(input, "error", 64);
        else if (hook == "Stop")
        {
            const auto found { input.find("last_assistant_message") };
            if (found != input.end() && found->is_string())
                normalized["summary"] = brief_text(found->get<std::string>().substr(0, 4096));
        }
        // 명령 인자·도구 출력·전체 대화는 저장하지 않는다.
        return parse_event(normalized);
    }

    claude_transcript::claude_transcript(const bool sidechain)
        : sidechain_ { sidechain }
    {}

    bool claude_transcript::sidechain() const noexcept
    {
        return sidechain_;
    }

    std::int64_t claude_transcript::started_ms() const noexcept
    {
        return started_ms_;
    }

    const std::map<std::string, claude_task_outcome>& claude_transcript::task_outcomes() const noexcept
    {
        return task_outcomes_;
    }

    bool claude_transcript::task_notification(const std::string& text, const std::int64_t time_ms)
    {
        constexpr std::string_view open { "<task-notification>" };
        bool found { false };
        std::size_t position { 0 };
        while ((position = text.find(open, position)) != std::string::npos)
        {
            const auto end { text.find("</task-notification>", position) };
            const std::string_view block { std::string_view { text }.substr(position, end == std::string::npos ? std::string::npos : end - position) };
            position += open.size();
            found = true;
            const auto statuses { tag_values(block, "status") };
            const auto status { statuses.empty() ? std::nullopt : task_status(statuses.front()) };
            if (!status.has_value())
                continue;
            // 재개된 에이전트는 같은 ID로 여러 번 알린다. 기록 순서대로 마지막 알림이 남는다.
            //  - 세션 재시작 뒤의 요약 알림은 ID 여럿을 담고, 그 자신의 ID는 `__`로 시작한다.
            for (const auto& id : tag_values(block, "task-id"))
                if (!id.empty() && !id.starts_with("__"))
                    task_finished(id, *status, time_ms);
        }
        return found;
    }

    void claude_transcript::task_finished(const std::string& id, const session_status status, const std::int64_t time_ms)
    {
        task_outcomes_[id] = { status, time_ms };
        for (auto& child : snapshot_.children)
            if (child.child_id == id)
            {
                child.status = status;
                child.tool_name.clear();
                child.updated_ms = std::max(child.updated_ms, time_ms);
            }
        // 오래 열린 세션도 알림 기록이 무한히 쌓이지 않게 한다.
        while (task_outcomes_.size() > 256)
            task_outcomes_.erase(std::ranges::min_element(task_outcomes_, {}, [](const auto& item) { return item.second.time_ms; }));
    }

    void claude_transcript::background_started(const nlohmann::json& record, const std::string& tool_id, const std::int64_t time_ms)
    {
        const auto request { background_requests_.find(tool_id) };
        if (request == background_requests_.end())
            return;
        const auto result { record.find("toolUseResult") };
        const auto task { result != record.end() && result->is_object() ? text_field(*result, "backgroundTaskId", 512) : std::string {} };
        if (!task.empty())
        {
            auto& child { upsert_child(snapshot_, child_kind::background_command, task) };
            child.parent_tool_id = tool_id;
            child.title = request->second;
            child.started_ms = time_ms;
            child.updated_ms = std::max(child.updated_ms, time_ms);
            const auto outcome { task_outcomes_.find(task) };
            child.status = outcome != task_outcomes_.end() ? outcome->second.status : session_status::running;
        }
        background_requests_.erase(request);
    }

    void claude_transcript::public_message(const std::string& text)
    {
        auto brief { brief_text(text) };
        if (!brief.empty())
            snapshot_.summary = std::move(brief);
    }

    void claude_transcript::tool_started(const std::string& id, const std::string& name)
    {
        snapshot_.turn_closed = false;
        snapshot_.tool_name = name;
        snapshot_.activity = tool_activity(name);
        snapshot_.status = name == "AskUserQuestion" ? session_status::waiting_input : session_status::running;
        if (!id.empty() && snapshot_.active_tools.size() < 64 && std::ranges::find(snapshot_.active_tools, id) == snapshot_.active_tools.end())
            snapshot_.active_tools.push_back(id);
    }

    void claude_transcript::tool_finished(const std::string& id)
    {
        if (std::ranges::find(snapshot_.active_tools, id) == snapshot_.active_tools.end())
            return;
        std::erase(snapshot_.active_tools, id);
        if (snapshot_.active_tools.empty())
        {
            snapshot_.tool_name.clear();
            snapshot_.activity = "분석 또는 응답 작성 중";
            snapshot_.status = session_status::running;
        }
    }

    void claude_transcript::consume(const nlohmann::json& record)
    {
        if (!record.is_object() || flag(record, "isSidechain") != sidechain_)
            return;
        const std::string type { text_field(record, "type", 64) };
        const std::string id { text_field(record, "sessionId", 512) };
        if (snapshot_.session_id.empty())
        {
            if (id.empty() || (type != "user" && type != "assistant" && type != "custom-title"))
                return;
            snapshot_.provider = "claude";
            snapshot_.session_id = id;
            snapshot_.observation_source = "claude-local";
            snapshot_.activity = "다음 요청 대기";
        }
        // 재개·분기 때 복사된 다른 세션의 기록은 이 카드에 섞지 않는다.
        if (!id.empty() && id != snapshot_.session_id)
            return;
        const auto time { record_time(record) };
        if (started_ms_ == 0 && time > 0)
            started_ms_ = time;
        // 작업 알림은 대기열 기록과 첨부로도 남는다. 같은 알림을 여러 번 적용해도 결과는 같다.
        if (!sidechain_ && (type == "queue-operation" || type == "attachment"))
        {
            const auto attachment { record.find("attachment") };
            std::string text {};
            if (type == "queue-operation")
                text = raw_text(record, "content");
            else if (attachment != record.end() && attachment->is_object())
                text = raw_text(*attachment, "prompt");
            if (task_notification(text, time))
                ++snapshot_.revision;
            return;
        }
        if (type == "custom-title")
        {
            const auto title { text_field(record, "customTitle", 180) };
            if (!title.empty() && title != snapshot_.name)
            {
                snapshot_.name = title;
                ++snapshot_.revision;
            }
            return;
        }
        if ((type != "user" && type != "assistant") || !record.contains("message") || !record["message"].is_object())
            return;
        const auto& message { record["message"] };
        const auto cwd { text_field(record, "cwd", 4096) };
        if (!cwd.empty())
            snapshot_.cwd = cwd;
        const nlohmann::json empty_content { nlohmann::json::array() };
        const auto& content { message.contains("content") ? message["content"] : empty_content };

        if (type == "user")
        {
            // 작업 알림은 사용자 요청처럼 들어오지만 새 turn이 아니다.
            bool notified { !sidechain_ && content.is_string() && task_notification(content.get_ref<const std::string&>().substr(0, 16 * 1024), time) };
            if (!sidechain_ && content.is_array())
                for (const auto& part : content)
                    if (part.is_object() && text_field(part, "type", 32) == "text")
                        notified = task_notification(raw_text(part, "text"), time) || notified;
            // compact 요약과 시스템이 넣은 메타 입력은 새 요청이 아니다.
            if (flag(record, "isMeta") || flag(record, "isCompactSummary"))
            {
                if (notified)
                    ++snapshot_.revision;
                return;
            }
            bool changed { notified };
            bool interrupted { false };
            if (content.is_array())
                for (const auto& part : content)
                {
                    if (!part.is_object())
                        continue;
                    const std::string part_type { text_field(part, "type", 32) };
                    if (part_type == "tool_result")
                    {
                        const auto tool_id { text_field(part, "tool_use_id", 512) };
                        background_started(record, tool_id, time);
                        if (const auto stop { stop_requests_.find(tool_id) }; stop != stop_requests_.end())
                        {
                            if (!flag(part, "is_error"))
                                task_finished(stop->second, session_status::interrupted, time);
                            stop_requests_.erase(stop);
                        }
                        tool_finished(tool_id);
                        changed = true;
                    }
                    else if (part_type == "text" && text_field(part, "text", 64).starts_with("[Request interrupted by user"))
                        interrupted = true;
                }
            const auto origin { record.find("origin") };
            const bool human { !notified && (origin != record.end() ? origin->is_object() && text_field(*origin, "kind", 32) == "human" : content.is_string()) };
            if (interrupted)
            {
                snapshot_.status = session_status::interrupted;
                snapshot_.turn_closed = true;
                snapshot_.active_tools.clear();
                snapshot_.tool_name.clear();
                snapshot_.activity = "사용자가 작업 중단";
                snapshot_.last_event = "interrupted";
                changed = true;
            }
            else if (human && !changed)
            {
                snapshot_.turn_id = text_field(record, "promptId", 512);
                snapshot_.turn_closed = false;
                snapshot_.status = session_status::running;
                snapshot_.failure.clear();
                snapshot_.summary.clear();
                snapshot_.tool_name.clear();
                snapshot_.active_tools.clear();
                snapshot_.activity = "작업 내용 수신 대기";
                snapshot_.last_event = "turn_started";
                snapshot_.awaiting_reply = false;
                text_asks_ = false;
                box_asks_ = false;
                changed = true;
            }
            if (!changed)
                return;
        }
        else
        {
            const auto model { text_field(message, "model", 128) };
            if (!model.empty() && !model.starts_with("<"))
                snapshot_.model = model;
            // API 오류는 assistant 기록으로 남지만 응답이 아니다. 작업 설명을 오류 문구로 덮지 않고 turn을 오류로 닫는다.
            //  - 이 기록의 stop_reason은 end_turn이 아니라서 따로 닫지 않으면 작업 중에 멈춰 남는다.
            if (flag(record, "isApiErrorMessage"))
            {
                std::string detail {};
                if (content.is_array())
                    for (const auto& part : content)
                        if (part.is_object() && text_field(part, "type", 32) == "text" && detail.empty())
                            detail = brief_text(raw_text(part, "text"));
                const auto failure { text_field(record, "error", 64) };
                snapshot_.status = session_status::failed;
                snapshot_.failure = std::ranges::all_of(failure, [](const char character) { return (character >= 'a' && character <= 'z') || character == '_'; }) ? failure : std::string {};
                snapshot_.turn_closed = true;
                snapshot_.active_tools.clear();
                snapshot_.tool_name.clear();
                snapshot_.activity = failure_label(snapshot_.failure) + (detail.empty() ? std::string {} : " · " + detail);
                snapshot_.last_event = "failed";
                if (time > 0)
                    snapshot_.updated_ms = std::max(snapshot_.updated_ms, time);
                ++snapshot_.revision;
                return;
            }
            if (content.is_array())
                for (const auto& part : content)
                {
                    if (!part.is_object())
                        continue;
                    // 비공개 thinking과 서버 도구 결과는 작업 설명으로 쓰지 않는다.
                    const std::string part_type { text_field(part, "type", 32) };
                    if (part_type == "text")
                    {
                        const auto text { raw_text(part, "text") };
                        public_message(text);
                        if (!text.empty())
                            text_asks_ = asks_user(text);
                    }
                    else if (part_type == "tool_use")
                    {
                        const auto tool_id { text_field(part, "id", 512) };
                        const auto name { text_field(part, "name", 128) };
                        tool_started(tool_id, name);
                        snapshot_.last_event = "tool_started";
                        // 데스크톱 앱의 마무리 상자에 물음을 실었으면 응답이 끝나도 사용자의 답을 기다린다.
                        if (const auto ask { part.find("input") }; name.ends_with("wrap_up") && ask != part.end() && ask->is_object() && !text_field(*ask, "ask", 512).empty())
                            box_asks_ = true;
                        // 백그라운드 명령은 결과가 곧바로 와서 도구로는 끝난 것처럼 보인다. 결과의 작업 ID로 하위 행을 만든다.
                        const auto input { part.find("input") };
                        if (sidechain_ || tool_id.empty() || input == part.end() || !input->is_object() || background_requests_.size() + stop_requests_.size() >= 64)
                            continue;
                        if (flag(*input, "run_in_background"))
                        {
                            const auto description { text_field(*input, "description", 180) };
                            background_requests_[tool_id] = description.empty() ? name : description;
                        }
                        // 직접 멈춘 작업은 완료 알림을 남기지 않는다. 멈춤 도구의 성공 결과로 닫는다.
                        else if (name == "TaskStop" || name == "KillShell")
                        {
                            auto task { text_field(*input, "task_id", 512) };
                            if (task.empty())
                                task = text_field(*input, "shell_id", 512);
                            if (!task.empty())
                                stop_requests_[tool_id] = std::move(task);
                        }
                    }
                }
            if (text_field(message, "stop_reason", 32) == "end_turn")
            {
                snapshot_.status = session_status::idle;
                snapshot_.turn_closed = true;
                snapshot_.active_tools.clear();
                snapshot_.tool_name.clear();
                snapshot_.awaiting_reply = text_asks_ || box_asks_;
                snapshot_.activity = snapshot_.awaiting_reply ? "응답 대기" : "응답 완료";
                snapshot_.last_event = "turn_finished";
            }
        }
        if (time > 0)
            snapshot_.updated_ms = std::max(snapshot_.updated_ms, time);
        ++snapshot_.revision;
    }

    const session_snapshot& claude_transcript::snapshot() const noexcept
    {
        return snapshot_;
    }

    std::filesystem::path default_claude_directory()
    {
        std::array<wchar_t, 32768> value {};
        const auto count { GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", value.data(), static_cast<DWORD>(value.size())) };
        if (count > 0 && count < value.size())
            return value.data();
        PWSTR profile { nullptr };
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)))
            return {};
        const auto result { std::filesystem::path { profile } / L".claude" };
        CoTaskMemFree(profile);
        return result;
    }

    std::optional<claude_runtime> parse_claude_runtime(const nlohmann::json& input)
    {
        if (!input.is_object())
            return std::nullopt;
        claude_runtime result {};
        const auto pid { number_field(input, "pid") };
        result.session_id = text_field(input, "sessionId", 513);
        if (pid <= 0 || pid > 0xffffffffll || result.session_id.empty() || result.session_id.size() > 512)
            return std::nullopt;
        result.pid = static_cast<std::uint32_t>(pid);
        // 프로세스 생성 시각(FILETIME)이다. 버전에 따라 이름이 다를 수 있다.
        for (const char* key : { "procStart", "procStartFt" })
        {
            const auto found { input.find(key) };
            if (found == input.end())
                continue;
            if (found->is_number_unsigned() || found->is_number_integer())
                result.process_start = found->get<std::uint64_t>();
            else if (found->is_string())
                try
                {
                    result.process_start = std::stoull(found->get<std::string>());
                }
                catch (...)
                {}
            if (result.process_start != 0)
                break;
        }
        result.cwd = text_field(input, "cwd", 4096);
        result.name = text_field(input, "name", 180);
        result.status = text_field(input, "status", 32);
        result.waiting_for = text_field(input, "waitingFor", 64);
        result.version = text_field(input, "version", 32);
        result.domain = text_field(input, "pidDomain", 128);
        result.started_ms = number_field(input, "startedAt");
        result.updated_ms = std::max(number_field(input, "updatedAt"), number_field(input, "statusUpdatedAt"));
        return result;
    }

    std::optional<claude_agent_meta> parse_claude_agent_meta(const nlohmann::json& input)
    {
        if (!input.is_object())
            return std::nullopt;
        claude_agent_meta result {};
        result.agent_type = text_field(input, "agentType", 128);
        result.description = text_field(input, "description", 180);
        result.tool_use_id = text_field(input, "toolUseId", 512);
        result.depth = static_cast<std::uint32_t>(std::clamp<std::int64_t>(number_field(input, "spawnDepth"), 1, 16));
        result.background = text_field(input, "requestShape", 32) == "background";
        result.finished = flag(input, "finished");
        return result;
    }

    session_child make_claude_agent_child(
        const std::string& agent_id, const claude_agent_meta& meta, const claude_transcript* transcript, const std::int64_t meta_time_ms, const std::map<std::string, claude_task_outcome>& outcomes)
    {
        session_child child {};
        child.kind = child_kind::agent;
        child.child_id = agent_id;
        child.parent_tool_id = meta.tool_use_id;
        child.agent_type = meta.agent_type;
        child.title = meta.description;
        child.background = meta.background;
        child.depth = meta.depth;
        child.updated_ms = meta_time_ms;
        if (transcript != nullptr)
        {
            const auto& view { transcript->snapshot() };
            child.started_ms = transcript->started_ms();
            child.updated_ms = std::max(child.updated_ms, view.updated_ms);
            child.tool_name = view.tool_name;
            // 도구를 쓰는 중이면 그 동작을, 아니면 마지막 공개 진행 문장을 보인다.
            child.activity = !view.tool_name.empty() || view.summary.empty() ? view.activity : view.summary;
        }
        if (child.started_ms == 0)
            child.started_ms = child.updated_ms;
        // 알림은 실패·중지를 알려 준다. 끝나지 않은 하위의 오래된 알림은 재개 전의 것이다.
        const auto outcome { outcomes.find(agent_id) };
        const bool known { outcome != outcomes.end() && (meta.finished || outcome->second.time_ms >= child.updated_ms) };
        child.status = known ? outcome->second.status : meta.finished ? session_status::idle : session_status::running;
        // 끝난 하위에 "작성 중" 같은 진행 문구를 남기지 않는다. 결과를 도구로 넘기고 끝나는 에이전트는 마지막 답변 문장이 없다.
        if (!is_working(child.status))
        {
            child.tool_name.clear();
            child.activity = transcript != nullptr ? transcript->snapshot().summary : std::string {};
        }
        return child;
    }

    bool claude_process_alive(const claude_runtime& runtime)
    {
        // 등록 파일의 존재만으로는 열린 세션이 아니다. PID 재사용을 생성 시각으로 거른다.
        if (runtime.process_start == 0 || (!runtime.domain.empty() && runtime.domain != local_domain()))
            return false;
        const HANDLE process { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, runtime.pid) };
        if (process == nullptr)
            return false;
        FILETIME created {};
        FILETIME exited {};
        FILETIME kernel {};
        FILETIME user {};
        DWORD code { 0 };
        const bool running { GetProcessTimes(process, &created, &exited, &kernel, &user) && GetExitCodeProcess(process, &code) && code == STILL_ACTIVE };
        const bool alive { running && ULARGE_INTEGER { created.dwLowDateTime, created.dwHighDateTime }.QuadPart == runtime.process_start };
        CloseHandle(process);
        return alive;
    }

    void apply_claude_runtime(session_snapshot& snapshot, const claude_runtime& runtime)
    {
        if (snapshot.session_id.empty())
        {
            snapshot.provider = "claude";
            snapshot.session_id = runtime.session_id;
            snapshot.activity = "다음 요청 대기";
        }
        // 기록의 cwd는 셸 이동을 따라가므로 열린 세션은 등록된 작업 폴더를 쓴다.
        if (!runtime.cwd.empty())
            snapshot.cwd = runtime.cwd;
        if (!runtime.name.empty())
            snapshot.name = runtime.name;
        snapshot.observation_source = "claude-local";
        snapshot.runtime_loaded = true;
        // 기록의 마지막 소식이 세션을 연 시각보다 이르면 열어 보기만 한 세션이다. 그 뒤로 처리한 요청이 없다.
        const bool opened_only { (snapshot.turn_id.empty() && snapshot.summary.empty()) || snapshot.updated_ms < runtime.started_ms };
        const bool current_status { runtime.updated_ms >= snapshot.updated_ms };
        snapshot.updated_ms = std::max(snapshot.updated_ms, runtime.updated_ms);
        if (!current_status)
            return;
        if (runtime.status == "busy")
        {
            if (snapshot.status != session_status::running)
            {
                snapshot.status = session_status::running;
                snapshot.activity = "분석 또는 응답 작성 중";
            }
            snapshot.turn_closed = false;
        }
        else if (runtime.status == "idle")
        {
            // 열기 전의 응답·중단·오류는 지난 일이다. 재개 Hook과 같이 새 요청을 기다리는 세션으로 보인다.
            if (opened_only)
            {
                snapshot.status = session_status::idle;
                snapshot.turn_closed = false;
                snapshot.failure.clear();
                snapshot.activity = "다음 요청 대기";
                snapshot.last_event = "session_started";
            }
            else
            {
                if (snapshot.status != session_status::interrupted)
                {
                    snapshot.status = session_status::idle;
                    snapshot.activity = snapshot.awaiting_reply ? "응답 대기" : "응답 완료";
                }
                snapshot.turn_closed = true;
            }
            snapshot.active_tools.clear();
            snapshot.tool_name.clear();
        }
        else if (runtime.status == "waiting")
        {
            const bool input { waiting_for_input(runtime.waiting_for) };
            snapshot.status = input ? session_status::waiting_input : session_status::waiting_approval;
            snapshot.activity = runtime.waiting_for.empty() ? (input ? "사용자 답변 대기" : "실행 승인 대기") : "사용자 대기 · " + runtime.waiting_for;
            snapshot.turn_closed = false;
        }
    }

    void merge_claude_sessions(std::vector<session_snapshot>& sessions, const std::vector<session_snapshot>& observed)
    {
        for (auto local : observed)
        {
            const auto found { std::ranges::find_if(sessions, [&](const auto& session) { return session.provider == "claude" && session.session_id == local.session_id; }) };
            if (found == sessions.end())
                sessions.push_back(std::move(local));
            else
            {
                if (local.title.empty())
                    local.title = found->title;
                if (local.model.empty())
                    local.model = found->model;
                // 세션 창은 Hook만 안다. 로컬 기록으로 덮어도 잃지 않는다.
                local.host_window = found->host_window;
                local.host_process_id = found->host_process_id;
                // Hook이 더 최근이면 승인·입력 대기처럼 로컬 기록에 없는 상태를 우선한다.
                if (found->updated_ms > local.updated_ms)
                {
                    const bool different_turn { !local.turn_id.empty() && !found->turn_id.empty() && local.turn_id != found->turn_id };
                    if (!different_turn || found->last_event == "turn_started")
                    {
                        if (different_turn || (local.summary.empty() && !found->summary.empty()))
                            local.summary = found->summary;
                        // Hook은 같은 도구의 기록보다 몇 ms 늦게 저장되므로 상태가 같으면 관측한 동작 설명을 유지한다.
                        if (different_turn || found->status != local.status)
                            local.activity.clear();
                        local.turn_id = found->turn_id;
                        local.last_event = found->last_event;
                        local.status = found->status;
                        local.turn_closed = found->turn_closed;
                        local.tool_name = found->tool_name;
                        local.active_tools = found->active_tools;
                        local.failure = found->failure;
                        local.updated_ms = found->updated_ms;
                    }
                }
                merge_children(local.children, found->children);
                *found = std::move(local);
            }
        }
        std::ranges::stable_sort(sessions, [](const auto& left, const auto& right) { return left.updated_ms > right.updated_ms; });
    }

    claude_monitor::claude_monitor(std::filesystem::path directory)
        : worker_ { [this, directory = std::move(directory)](const std::stop_token stop) { run(stop, directory); } }
    {}

    claude_monitor::~claude_monitor()
    {
        worker_.request_stop();
        wake_.notify_all();
    }

    claude_observation claude_monitor::snapshot() const
    {
        const std::lock_guard lock { mutex_ };
        return observation_;
    }

    void claude_monitor::run(const std::stop_token stop, const std::filesystem::path& directory)
    {
        std::map<std::filesystem::path, tracked_file> files {};
        std::map<std::filesystem::path, agent_folder> agent_folders {};
        std::set<std::string> known_live {};
        // 이번 실행에서 열린 것을 본 세션과, 그 뒤 닫힌 것을 처음 본 시각이다.
        std::set<std::string> seen_live {};
        std::map<std::string, std::int64_t> closed_at {};
        auto next_discovery { std::chrono::steady_clock::time_point {} };
        while (!stop.stop_requested())
        {
            claude_observation result {};
            result.ready = true;
            try
            {
                const auto live { read_runtimes(directory) };
                const auto now { std::chrono::steady_clock::now() };
                const bool unknown_live { std::ranges::any_of(live, [&](const auto& item) { return !known_live.contains(item.first); }) };
                // 새로 열린 세션은 다음 정기 탐색을 기다리지 않는다.
                if (now >= next_discovery || unknown_live)
                {
                    discover(directory, live, files);
                    std::erase_if(agent_folders, [&](const auto& item) { return !files.contains(item.first); });
                    known_live.clear();
                    for (const auto& item : live)
                        known_live.insert(item.first);
                    next_discovery = now + std::chrono::seconds { 5 };
                }
                std::set<std::string> covered {};
                for (auto& [path, file] : files)
                {
                    if (stop.stop_requested())
                        break;
                    try
                    {
                        file.read(path);
                        if (file.ready && !file.reader.snapshot().session_id.empty())
                            file.published = file.reader.snapshot();
                        const auto runtime { live.find(path.stem().string()) };
                        if (!file.published.has_value())
                        {
                            if (runtime != live.end())
                                result.ready = false;
                            continue;
                        }
                        auto session { *file.published };
                        const auto time { now_ms() };
                        if (const auto own { live.find(session.session_id) }; own != live.end())
                        {
                            apply_claude_runtime(session, own->second);
                            covered.insert(session.session_id);
                            seen_live.insert(session.session_id);
                            closed_at.erase(session.session_id);
                        }
                        // 열린 것을 본 세션의 등록이 사라졌으면 프로세스가 끝났다. 등록을 남기지 않는 판에서는 열린 것을 본 적이 없어 건드리지 않는다.
                        else if (seen_live.contains(session.session_id))
                        {
                            const auto closed { closed_at.try_emplace(session.session_id, time).first->second };
                            if (time - closed >= vanished_grace_ms)
                                close_vanished_session(session, closed);
                        }
                        auto& folder { agent_folders[path] };
                        read_agents(path.parent_path() / path.stem() / L"subagents", folder);
                        std::vector<session_child> agents {};
                        for (const auto& [id, agent] : folder.agents)
                        {
                            if (!agent.meta.has_value())
                                continue;
                            const auto* reader { agent.transcript.ready ? &agent.transcript.reader : nullptr };
                            const auto child { make_claude_agent_child(id, *agent.meta, reader, file_time_ms(agent.meta_time), file.reader.task_outcomes()) };
                            agents.push_back(child);
                        }
                        merge_children(session.children, agents);
                        // 위젯 유지 기간이 지난 끝난 하위는 부모 카드에서도 뺀다. 에이전트와 명령이 같은 기준을 쓴다.
                        std::erase_if(session.children, [time](const auto& child) { return !is_working(child.status) && time - child.updated_ms >= widget_retention_ms; });
                        // 포그라운드 하위는 부모 turn 안에서 끝난다. 끝 표시 없이 남은 하위는 매 주기 메타로 다시 만들어지므로 늦게 온 끝 표시가 이긴다.
                        if (!is_working(session.status))
                            close_children(session, session.status == session_status::interrupted ? session_status::interrupted : session_status::ended, time, true);
                        // 닫힌 세션의 하위는 더 이상 소식을 남기지 못한다. 진행 중으로 남겨 두지 않는다.
                        if (!session.runtime_loaded)
                            close_children(session, session_status::ended, time, false);
                        if (!session.runtime_loaded && time - latest_update(session) >= widget_retention_ms)
                            continue;
                        result.sessions.push_back(std::move(session));
                    }
                    catch (...)
                    {
                        result.error = "일부 Claude 로컬 기록을 읽지 못했습니다.";
                    }
                }
                // 아직 요청이 없어 기록 파일이 없는 열린 세션도 등록 정보만으로 보인다.
                for (const auto& [id, runtime] : live)
                    if (!covered.contains(id) && std::ranges::none_of(files, [&](const auto& file) { return file.first.stem().string() == id; }))
                    {
                        session_snapshot session {};
                        apply_claude_runtime(session, runtime);
                        result.sessions.push_back(std::move(session));
                    }
            }
            catch (...)
            {
                result.error = "Claude 자동 수집 경로를 읽지 못했습니다. Hooks 수집은 계속됩니다.";
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
