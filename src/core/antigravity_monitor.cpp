#include "core/antigravity_monitor.h"

#include "core/local_record.h"

#include <shlobj.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

namespace session_board {
    namespace {
        constexpr std::size_t maximum_line { 512 * 1024 };
        constexpr std::uintmax_t maximum_small_file { 64 * 1024 };
        // 제품마다 앱 데이터 폴더가 따로 있다. 있는 것만 읽는다.
        constexpr std::array<std::wstring_view, 3> product_folders { L"antigravity", L"antigravity-ide", L"antigravity-cli" };
        constexpr std::string_view background_marker { "running as a background task with task id: " };

        std::string text_field(const nlohmann::json& value, const char* key, const std::size_t limit = 360)
        {
            const auto found { value.find(key) };
            return found != value.end() && found->is_string() ? compact_text(found->get<std::string>(), limit) : std::string {};
        }

        // 태그와 줄을 찾을 수 있게 제어 문자를 바꾸지 않은 앞부분이다. 큰 원문은 앞쪽만 본다.
        std::string raw_text(const nlohmann::json& value, const char* key)
        {
            const auto found { value.find(key) };
            return found != value.end() && found->is_string() ? found->get_ref<const std::string&>().substr(0, 64 * 1024) : std::string {};
        }

        std::int64_t step_index(const nlohmann::json& record)
        {
            const auto found { record.find("step_index") };
            return found != record.end() && found->is_number_integer() ? found->get<std::int64_t>() : -1;
        }

        // 도구 인자의 값은 문자열이고, 그 안이 다시 JSON 문자열로 감싸여 있기도 하다.
        std::string argument(const nlohmann::json& arguments, const char* key, const std::size_t limit = 4096)
        {
            if (!arguments.is_object())
                return {};
            const auto found { arguments.find(key) };
            if (found == arguments.end() || !found->is_string())
                return {};
            std::string value { found->get<std::string>() };
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            {
                const auto inner { nlohmann::json::parse(value, nullptr, false) };
                if (inner.is_string())
                    value = inner.get<std::string>();
            }
            return compact_text(std::move(value), limit);
        }

        std::string trim(std::string text)
        {
            const auto start { text.find_first_not_of(" \t\r\n") };
            if (start == std::string::npos)
                return {};
            const auto end { text.find_last_not_of(" \t\r\n") };
            return text.substr(start, end - start + 1);
        }

        // 공개 답변의 첫 줄·첫 문장이다. 답변은 마크다운이라 제목·목록 기호와 강조 표시를 걷어 낸다.
        //  - 답변은 흔히 `### 결과` 같은 제목으로 시작한다. 뒤에 본문이 있으면 제목보다 본문 첫 문장이 작업을 말한다.
        std::string plain_brief(const std::string& text)
        {
            std::string line {};
            std::string heading {};
            std::size_t position { 0 };
            while (position < text.size() && line.empty())
            {
                const auto end { text.find('\n', position) };
                line = trim(text.substr(position, end == std::string::npos ? std::string::npos : end - position));
                position = end == std::string::npos ? text.size() : end + 1;
                // 표의 구분 줄이나 가로줄만 있는 줄은 문장이 아니다.
                if (!line.empty() && line.find_first_not_of("-|:=*_ ") == std::string::npos)
                    line.clear();
                if (line.starts_with('#'))
                {
                    if (heading.empty())
                        heading = line;
                    line.clear();
                }
            }
            if (line.empty())
                line = heading;
            while (!line.empty() && (line.front() == '#' || line.front() == '>'))
                line = trim(line.substr(1));
            if (line.starts_with("- ") || line.starts_with("* ") || line.starts_with("+ "))
                line = trim(line.substr(2));
            for (const std::string_view mark : { "**", "__", "`" })
                for (auto found { line.find(mark) }; found != std::string::npos; found = line.find(mark, found))
                    line.erase(found, mark.size());
            const auto sentence { line.find(". ") };
            if (sentence != std::string::npos)
                line.resize(sentence + 1);
            return compact_text(trim(std::move(line)), 240);
        }

        std::string tag_body(const std::string& text, const std::string_view name)
        {
            const std::string open { "<" + std::string { name } + ">" };
            const auto start { text.find(open) };
            if (start == std::string::npos)
                return {};
            const auto end { text.find("</" + std::string { name } + ">", start) };
            return text.substr(start + open.size(), end == std::string::npos ? std::string::npos : end - start - open.size());
        }

        std::string first_line(const std::string& text, const std::size_t limit)
        {
            const auto body { trim(text) };
            return compact_text(body.substr(0, body.find('\n')), limit);
        }

        // ``The user changed setting `Model Selection` from None to <모델>.``의 모델 이름이다.
        std::string selected_model(const std::string& text)
        {
            constexpr std::string_view key { "`Model Selection` from " };
            const auto setting { text.find(key) };
            if (setting == std::string::npos)
                return {};
            const auto to { text.find(" to ", setting + key.size()) };
            if (to == std::string::npos)
                return {};
            const auto start { to + 4 };
            auto end { text.find(". ", start) };
            end = std::min(end, text.find('\n', start));
            std::string model { trim(text.substr(start, end == std::string::npos ? std::string::npos : end - start)) };
            if (model.ends_with('.'))
                model.pop_back();
            return compact_text(std::move(model), 128);
        }

        std::string tool_activity(const std::string& name)
        {
            constexpr std::array read { "read_file", "view_file", "view_file_outline", "view_code_item" };
            constexpr std::array search { "list_dir", "find_by_name", "grep_search", "codebase_search", "search_files" };
            constexpr std::array edit { "write_file", "write_to_file", "edit_file", "replace_file_content", "multi_replace_file_content" };
            constexpr std::array command { "run_command", "send_command_input" };
            if (std::ranges::find(read, name) != read.end())
                return "파일 읽는 중 · " + name;
            if (std::ranges::find(search, name) != search.end())
                return "파일 검색 중 · " + name;
            if (std::ranges::find(edit, name) != edit.end())
                return "코드 수정 중 · " + name;
            if (std::ranges::find(command, name) != command.end())
                return "명령 실행 중 · " + name;
            if (name == "search_web" || name == "read_url_content" || name.starts_with("browser_"))
                return "웹 조회 중 · " + name;
            if (name == "ask_question")
                return "사용자 답변 대기";
            if (name == "invoke_subagent")
                return "하위 에이전트 실행 중 · " + name;
            return "도구 실행 중 · " + name;
        }

        bool absolute_path(const std::string& path)
        {
            return path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' && (path[2] == '\\' || path[2] == '/');
        }

        // 비교용으로 구분자와 영문 대소문자를 맞추고 끝 구분자를 뗀다.
        std::string comparable_path(std::string path)
        {
            for (char& character : path)
                if (character == '/')
                    character = '\\';
                else if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character - 'A' + 'a');
            while (path.size() > 3 && path.ends_with('\\'))
                path.pop_back();
            return path;
        }

        int hex_value(const char character)
        {
            if (character >= '0' && character <= '9')
                return character - '0';
            if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;
            if (character >= 'A' && character <= 'F')
                return character - 'A' + 10;
            return -1;
        }

        // `file:///e%3A/Programming/…`를 `e:\Programming\…`로 바꾼다. 다른 호스트를 가리키는 주소는 버린다.
        std::string file_uri_path(const std::string& uri)
        {
            constexpr std::string_view scheme { "file:///" };
            if (!uri.starts_with(scheme))
                return {};
            std::string result {};
            for (std::size_t index { scheme.size() }; index < uri.size(); ++index)
                if (uri[index] == '%' && index + 2 < uri.size() && hex_value(uri[index + 1]) >= 0 && hex_value(uri[index + 2]) >= 0)
                {
                    result += static_cast<char>(hex_value(uri[index + 1]) * 16 + hex_value(uri[index + 2]));
                    index += 2;
                }
                else
                    result += uri[index] == '/' ? '\\' : uri[index];
            return absolute_path(result) ? compact_text(std::move(result), 4096) : std::string {};
        }

        std::optional<std::string> small_text(const std::filesystem::path& path)
        {
            std::error_code error {};
            if (std::filesystem::file_size(path, error) > maximum_small_file || error)
                return std::nullopt;
            std::ifstream input { path, std::ios::binary };
            if (!input)
                return std::nullopt;
            return std::string { std::istreambuf_iterator<char> { input }, std::istreambuf_iterator<char> {} };
        }

        std::vector<std::filesystem::path> product_directories(const std::filesystem::path& home)
        {
            std::vector<std::filesystem::path> result {};
            for (const auto name : product_folders)
            {
                std::error_code error {};
                if (std::filesystem::is_directory(home / name, error))
                    result.push_back(home / name);
            }
            return result;
        }

        bool conversation_id(const std::wstring& text)
        {
            return text.size() == 36 && std::ranges::all_of(text, [](const wchar_t character) { return character == L'-' || (character < 128 && std::isxdigit(static_cast<int>(character)) != 0); });
        }

        // 열린 대화와 그 잠금 파일의 시각이다. 잠금 파일은 대화를 열 때 생긴다.
        std::map<std::string, std::int64_t> live_conversations(const std::vector<std::filesystem::path>& products)
        {
            std::map<std::string, std::int64_t> result {};
            for (const auto& product : products)
            {
                std::error_code error {};
                const auto presence { product / L"presence" };
                if (!std::filesystem::is_directory(presence, error))
                    continue;
                for (const auto& entry : std::filesystem::directory_iterator { presence, std::filesystem::directory_options::skip_permission_denied, error })
                {
                    const std::wstring stem { entry.path().stem().wstring() };
                    if (entry.path().extension() != L".lock" || !conversation_id(stem) || !file_lock_held(entry.path()))
                        continue;
                    std::error_code time_error {};
                    const auto time { entry.last_write_time(time_error) };
                    result[entry.path().stem().string()] = time_error ? now_ms() : file_time_ms(time);
                }
            }
            return result;
        }

        std::vector<std::string> project_roots(const std::filesystem::path& home)
        {
            std::vector<std::string> result {};
            std::error_code error {};
            const auto projects { home / L"config" / L"projects" };
            if (!std::filesystem::is_directory(projects, error))
                return result;
            for (const auto& entry : std::filesystem::directory_iterator { projects, std::filesystem::directory_options::skip_permission_denied, error })
            {
                if (entry.path().extension() != L".json")
                    continue;
                const auto text { small_text(entry.path()) };
                const auto value { text.has_value() ? nlohmann::json::parse(*text, nullptr, false) : nlohmann::json {} };
                for (auto& root : parse_antigravity_project_roots(value))
                    if (std::ranges::find(result, root) == result.end() && result.size() < 256)
                        result.push_back(std::move(root));
            }
            return result;
        }

        struct tracked_file
        {
            antigravity_transcript reader {};
            jsonl_tail tail { maximum_line };
            bool ready { false };
            std::optional<session_snapshot> published {};
            std::vector<std::string> paths {};

            void read(const std::filesystem::path& path)
            {
                tail.read(path, [this] { reader = antigravity_transcript { reader.snapshot().session_id }; }, [this](const nlohmann::json& value) { reader.consume(value); });
                ready = tail.ready();
            }
        };

        // `<제품>/brain/<대화 ID>/.system_generated/logs/transcript.jsonl`의 대화 ID다.
        std::string transcript_id(const std::filesystem::path& path)
        {
            return path.parent_path().parent_path().parent_path().filename().string();
        }

        void discover(const std::vector<std::filesystem::path>& products, const std::map<std::string, std::int64_t>& live, std::map<std::filesystem::path, tracked_file>& files)
        {
            using candidate = std::pair<std::filesystem::file_time_type, std::filesystem::path>;
            std::vector<candidate> candidates {};
            const auto cutoff { std::filesystem::file_time_type::clock::now() - std::chrono::hours { 48 } };
            for (const auto& product : products)
            {
                std::error_code error {};
                const auto brain { product / L"brain" };
                if (!std::filesystem::is_directory(brain, error))
                    continue;
                for (const auto& folder : std::filesystem::directory_iterator { brain, std::filesystem::directory_options::skip_permission_denied, error })
                {
                    if (!conversation_id(folder.path().filename().wstring()))
                        continue;
                    const auto path { folder.path() / L".system_generated" / L"logs" / L"transcript.jsonl" };
                    std::error_code time_error {};
                    const auto updated { std::filesystem::last_write_time(path, time_error) };
                    if (time_error)
                        continue;
                    const bool loaded { live.contains(folder.path().filename().string()) };
                    if (loaded || updated >= cutoff)
                        candidates.emplace_back(loaded ? std::filesystem::file_time_type::max() : updated, path);
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
                if (const auto [item, inserted] { files.try_emplace(path) }; inserted)
                    item->second.reader = antigravity_transcript { transcript_id(path) };
            }
            std::erase_if(files, [&](const auto& item) { return !keep.contains(item.first); });
        }

        struct annotation
        {
            std::filesystem::file_time_type time {};
            std::string title {};
        };

        // 대화 이름은 사용자가 바꿀 수 있어 파일 시각이 바뀔 때만 다시 읽는다.
        std::string conversation_name(const std::vector<std::filesystem::path>& products, const std::string& id, std::map<std::string, annotation>& cache)
        {
            for (const auto& product : products)
            {
                const auto path { product / L"annotations" / (std::filesystem::path { id }.wstring() + L".pbtxt") };
                std::error_code error {};
                const auto time { std::filesystem::last_write_time(path, error) };
                if (error)
                    continue;
                auto& item { cache[id] };
                if (item.time != time)
                {
                    item.time = time;
                    const auto text { small_text(path) };
                    item.title = text.has_value() ? parse_antigravity_title(*text) : std::string {};
                }
                return item.title;
            }
            return {};
        }
    } // namespace

    antigravity_transcript::antigravity_transcript(std::string session_id)
    {
        snapshot_.provider = "antigravity";
        snapshot_.session_id = std::move(session_id);
        snapshot_.observation_source = "antigravity-local";
        snapshot_.activity = "다음 요청 대기";
    }

    const session_snapshot& antigravity_transcript::snapshot() const noexcept
    {
        return snapshot_;
    }

    const std::vector<std::string>& antigravity_transcript::paths() const noexcept
    {
        return paths_;
    }

    void antigravity_transcript::remember_path(std::string path)
    {
        if (path.starts_with("file:///"))
            path = file_uri_path(path);
        if (!absolute_path(path))
            return;
        std::erase(paths_, path);
        paths_.push_back(std::move(path));
        if (paths_.size() > 32)
            paths_.erase(paths_.begin());
    }

    void antigravity_transcript::tool_started(const std::string& id, const std::string& name, const nlohmann::json& arguments)
    {
        snapshot_.turn_closed = false;
        for (const char* key : { "AbsolutePath", "Path", "TargetFile", "Cwd", "DirectoryPath", "SearchPath" })
            remember_path(argument(arguments, key));
        pending_.push_back({ id, name, argument(arguments, "toolSummary", 180) });
        if (name == "invoke_subagent")
        {
            auto& child { upsert_child(snapshot_, child_kind::agent, id) };
            child.agent_type = argument(arguments, "TypeName", 128);
            child.title = argument(arguments, "Role", 180);
            if (child.title.empty())
                child.title = child.agent_type;
            child.parent_tool_id = id;
            child.status = session_status::running;
            child.started_ms = snapshot_.updated_ms;
            child.updated_ms = snapshot_.updated_ms;
        }
        if (!id.empty() && snapshot_.active_tools.size() < 64 && std::ranges::find(snapshot_.active_tools, id) == snapshot_.active_tools.end())
            snapshot_.active_tools.push_back(id);
        snapshot_.tool_name = name;
        snapshot_.activity = tool_activity(name);
        snapshot_.status = name == "ask_question" ? session_status::waiting_input : session_status::running;
        snapshot_.last_event = name == "ask_question" ? "input_requested" : "tool_started";
    }

    void antigravity_transcript::tool_finished(const session_status outcome)
    {
        if (pending_.empty())
            return;
        const auto done { pending_.front() };
        pending_.erase(pending_.begin());
        std::erase(snapshot_.active_tools, done.id);
        for (auto& child : snapshot_.children)
            if (child.kind == child_kind::agent && child.child_id == done.id && is_working(child.status))
            {
                child.status = outcome == session_status::failed ? session_status::failed : session_status::idle;
                child.updated_ms = std::max(child.updated_ms, snapshot_.updated_ms);
            }
        snapshot_.last_event = "tool_finished";
        if (pending_.empty())
        {
            snapshot_.tool_name.clear();
            snapshot_.activity = "분석 또는 응답 작성 중";
            snapshot_.status = session_status::running;
            return;
        }
        // 남은 도구 중 답을 기다리는 것이 있으면 그 상태가 먼저다.
        const auto waiting { std::ranges::find_if(pending_, [](const auto& tool) { return tool.name == "ask_question"; }) };
        const auto& shown { waiting != pending_.end() ? *waiting : pending_.back() };
        snapshot_.tool_name = shown.name;
        snapshot_.activity = tool_activity(shown.name);
        snapshot_.status = waiting != pending_.end() ? session_status::waiting_input : session_status::running;
    }

    void antigravity_transcript::background_started(const std::string& content)
    {
        const auto marker { content.find(background_marker) };
        const auto start { marker + background_marker.size() };
        const auto end { content.find_first_of(" \t\r\n", start) };
        const auto task { compact_text(content.substr(start, end == std::string::npos ? std::string::npos : end - start), 512) };
        // 결과를 기다리던 도구는 백그라운드로 넘어가며 끝난다. 작업 설명(`Task Description`)은 명령 원문이라 쓰지 않는다.
        const std::string title { pending_.empty() ? std::string {} : pending_.front().summary };
        if (!task.empty())
        {
            auto& child { upsert_child(snapshot_, child_kind::background_command, task) };
            child.parent_tool_id = pending_.empty() ? std::string {} : pending_.front().id;
            if (!title.empty())
                child.title = title;
            child.status = session_status::running;
            child.started_ms = snapshot_.updated_ms;
            child.updated_ms = snapshot_.updated_ms;
        }
        tool_finished(session_status::idle);
    }

    void antigravity_transcript::system_message(const std::string& content, const std::int64_t time_ms)
    {
        // `[Message] … sender=<작업 ID> … content=Task id "<작업 ID>" finished with result: …`이다. 사용자 turn이 아니다.
        constexpr std::string_view key { "sender=" };
        const auto sender { content.find(key) };
        if (sender == std::string::npos)
            return;
        const auto start { sender + key.size() };
        const auto end { content.find_first_of(" \t\r\n", start) };
        const auto id { content.substr(start, end == std::string::npos ? std::string::npos : end - start) };
        const auto finished { content.find("\" finished", start) != std::string::npos };
        const auto stopped { content.find("\" was killed", start) != std::string::npos || content.find("\" was canceled", start) != std::string::npos };
        if (id.empty() || (!finished && !stopped))
            return;
        for (auto& child : snapshot_.children)
            if (child.child_id == id && is_working(child.status))
            {
                child.status = stopped ? session_status::interrupted : session_status::idle;
                child.tool_name.clear();
                child.updated_ms = std::max(child.updated_ms, time_ms);
            }
    }

    void antigravity_transcript::consume(const nlohmann::json& record)
    {
        if (!record.is_object())
            return;
        const auto step { step_index(record) };
        // 같은 단계는 끝난 모양으로 한 번만 적용한다. 다시 읽은 줄이 도구를 두 번 세지 않는다.
        if (step >= 0 && step <= last_step_)
            return;
        const std::string source { text_field(record, "source", 32) };
        const std::string type { text_field(record, "type", 64) };
        const std::string status { text_field(record, "status", 32) };
        const auto time { iso_time_ms(text_field(record, "created_at", 64)) };
        if (time > 0)
            snapshot_.updated_ms = std::max(snapshot_.updated_ms, time);
        const std::string content { raw_text(record, "content") };
        bool settled { status == "DONE" || status == "ERROR" };

        if (source == "USER_EXPLICIT" && type == "USER_INPUT")
        {
            snapshot_.turn_id = step >= 0 ? "step-" + std::to_string(step) : std::string {};
            const auto request { first_line(tag_body(content, "USER_REQUEST"), 180) };
            if (!request.empty())
                snapshot_.title = request;
            if (const auto model { selected_model(tag_body(content, "USER_SETTINGS_CHANGE")) }; !model.empty())
                snapshot_.model = model;
            snapshot_.status = session_status::running;
            snapshot_.turn_closed = false;
            snapshot_.summary.clear();
            snapshot_.awaiting_reply = false;
            snapshot_.tool_name.clear();
            snapshot_.active_tools.clear();
            pending_.clear();
            snapshot_.activity = "작업 내용 수신 대기";
            snapshot_.last_event = "turn_started";
        }
        else if (source == "MODEL" && type == "PLANNER_RESPONSE")
        {
            // 비공개 thinking은 읽지 않는다. 공개 content만 작업 설명이다.
            if (const auto brief { plain_brief(content) }; !brief.empty())
                snapshot_.summary = brief;
            const auto calls { record.find("tool_calls") };
            if (calls != record.end() && calls->is_array() && !calls->empty())
            {
                std::size_t index { 0 };
                for (const auto& call : *calls)
                {
                    if (call.is_object())
                    {
                        const auto name { text_field(call, "name", 128) };
                        const auto arguments { call.find("args") };
                        tool_started(std::to_string(step) + ":" + std::to_string(index), name.empty() ? std::string { "tool" } : name, arguments != call.end() ? *arguments : nlohmann::json {});
                    }
                    ++index;
                }
                settled = true;
            }
            else if (status == "DONE")
            {
                snapshot_.status = session_status::idle;
                snapshot_.turn_closed = true;
                snapshot_.active_tools.clear();
                snapshot_.tool_name.clear();
                pending_.clear();
                // 마지막 공개 답이 묻고 끝났으면 사용자의 답을 기다린다.
                snapshot_.awaiting_reply = asks_user(content);
                snapshot_.activity = snapshot_.awaiting_reply ? "응답 대기" : "응답 완료";
                snapshot_.last_event = "turn_finished";
                close_children(snapshot_, session_status::ended, snapshot_.updated_ms, true);
            }
        }
        else if (source == "MODEL" && type == "GENERIC")
        {
            if (status == "RUNNING" && content.find(background_marker) != std::string::npos)
            {
                background_started(content);
                settled = true;
            }
            // 도구 하나의 실패는 세션 전체의 실패가 아니다.
            else if (settled)
                tool_finished(status == "ERROR" ? session_status::failed : session_status::idle);
        }
        else if (source == "SYSTEM" && type == "SYSTEM_MESSAGE")
            system_message(content, time);
        else if (source == "SYSTEM" && type == "CHECKPOINT" && snapshot_.title.empty())
        {
            constexpr std::string_view objective { "# USER Objective:" };
            if (const auto found { content.find(objective) }; found != std::string::npos)
                snapshot_.title = first_line(content.substr(found + objective.size()), 180);
        }
        else
            return;
        if (settled && step >= 0)
            last_step_ = step;
        ++snapshot_.revision;
    }

    std::string parse_antigravity_title(const std::string_view text)
    {
        constexpr std::string_view key { "title:\"" };
        std::size_t position { 0 };
        while ((position = text.find(key, position)) != std::string_view::npos)
        {
            // `subtitle:"` 같은 다른 필드의 끝부분과 헷갈리지 않는다.
            if (position == 0 || text[position - 1] == ' ' || text[position - 1] == '\t' || text[position - 1] == '\n' || text[position - 1] == '{')
                break;
            position += key.size();
        }
        if (position == std::string_view::npos)
            return {};
        std::string result {};
        for (std::size_t index { position + key.size() }; index < text.size(); ++index)
        {
            const char character { text[index] };
            if (character == '"')
                return compact_text(std::move(result), 180);
            if (character != '\\' || index + 1 >= text.size())
            {
                result += character;
                continue;
            }
            const char escaped { text[++index] };
            if (escaped >= '0' && escaped <= '7')
            {
                // 8진수 바이트다. 한글 같은 UTF-8 글자는 바이트마다 `\NNN`으로 적힌다.
                int value { escaped - '0' };
                for (int count { 0 }; count < 2 && index + 1 < text.size() && text[index + 1] >= '0' && text[index + 1] <= '7'; ++count)
                    value = value * 8 + (text[++index] - '0');
                result += static_cast<char>(value & 0xff);
            }
            else if (escaped == 'x' && index + 2 < text.size() && hex_value(text[index + 1]) >= 0 && hex_value(text[index + 2]) >= 0)
            {
                result += static_cast<char>(hex_value(text[index + 1]) * 16 + hex_value(text[index + 2]));
                index += 2;
            }
            else
                result += escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped == 'r' ? '\r' : escaped;
        }
        // 닫는 따옴표가 없으면 잘린 파일이다.
        return {};
    }

    std::vector<std::string> parse_antigravity_project_roots(const nlohmann::json& project)
    {
        std::vector<std::string> result {};
        if (!project.is_object())
            return result;
        const auto resources_owner { project.find("projectResources") };
        if (resources_owner == project.end() || !resources_owner->is_object())
            return result;
        const auto resources { resources_owner->find("resources") };
        if (resources == resources_owner->end() || !resources->is_array())
            return result;
        for (const auto& resource : *resources)
        {
            if (!resource.is_object())
                continue;
            // 폴더 그대로이거나, git 폴더면 한 단계 안에 같은 키가 있다.
            std::string uri { text_field(resource, "folderUri", 4096) };
            if (const auto git { resource.find("gitFolder") }; uri.empty() && git != resource.end() && git->is_object())
                uri = text_field(*git, "folderUri", 4096);
            if (auto path { file_uri_path(uri) }; !path.empty())
                result.push_back(std::move(path));
        }
        return result;
    }

    std::string match_antigravity_root(const std::vector<std::string>& paths, const std::vector<std::string>& roots)
    {
        for (auto path { paths.rbegin() }; path != paths.rend(); ++path)
        {
            const auto candidate { comparable_path(*path) };
            const std::string* best { nullptr };
            std::size_t best_size { 0 };
            for (const auto& root : roots)
            {
                const auto prefix { comparable_path(root) };
                const bool inside { candidate == prefix || (candidate.starts_with(prefix) && (prefix.ends_with('\\') || candidate[prefix.size()] == '\\')) };
                if (inside && prefix.size() > best_size)
                {
                    best = &root;
                    best_size = prefix.size();
                }
            }
            if (best != nullptr)
                return *best;
        }
        return {};
    }

    std::filesystem::path default_antigravity_directory()
    {
        PWSTR profile { nullptr };
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)))
            return {};
        const auto result { std::filesystem::path { profile } / L".gemini" };
        CoTaskMemFree(profile);
        return result;
    }

    void merge_antigravity_sessions(std::vector<session_snapshot>& sessions, const std::vector<session_snapshot>& observed)
    {
        for (auto local : observed)
        {
            const auto found { std::ranges::find_if(sessions, [&](const auto& session) { return session.provider == "antigravity" && session.session_id == local.session_id; }) };
            if (found == sessions.end())
                sessions.push_back(std::move(local));
            else
            {
                // 상태·도구·작업 설명은 로컬 기록이 기준이다. Hook은 기록이 모르는 작업 폴더·창·모델만 보탠다.
                local.host_window = found->host_window;
                local.host_process_id = found->host_process_id;
                for (auto member : { &session_snapshot::cwd, &session_snapshot::model, &session_snapshot::title })
                    if ((local.*member).empty())
                        local.*member = (*found).*member;
                merge_children(local.children, found->children);
                *found = std::move(local);
            }
        }
        std::ranges::stable_sort(sessions, [](const auto& left, const auto& right) { return left.updated_ms > right.updated_ms; });
    }

    antigravity_monitor::antigravity_monitor(std::filesystem::path directory)
        : worker_ { [this, directory = std::move(directory)](const std::stop_token stop) { run(stop, directory); } }
    {}

    antigravity_monitor::~antigravity_monitor()
    {
        worker_.request_stop();
        wake_.notify_all();
    }

    antigravity_observation antigravity_monitor::snapshot() const
    {
        const std::lock_guard lock { mutex_ };
        return observation_;
    }

    void antigravity_monitor::run(const std::stop_token stop, const std::filesystem::path& directory)
    {
        std::map<std::filesystem::path, tracked_file> files {};
        std::map<std::string, annotation> names {};
        std::vector<std::string> roots {};
        std::set<std::string> known_live {};
        // 이번 실행에서 열린 것을 본 대화와, 그 뒤 잠금이 풀린 것을 처음 본 시각이다.
        std::set<std::string> seen_live {};
        std::map<std::string, std::int64_t> closed_at {};
        auto next_discovery { std::chrono::steady_clock::time_point {} };
        while (!stop.stop_requested())
        {
            antigravity_observation result {};
            result.ready = true;
            try
            {
                const auto products { product_directories(directory) };
                const auto live { live_conversations(products) };
                const auto now { std::chrono::steady_clock::now() };
                const bool unknown_live { std::ranges::any_of(live, [&](const auto& item) { return !known_live.contains(item.first); }) };
                // 새로 연 대화는 다음 정기 탐색을 기다리지 않는다.
                if (now >= next_discovery || unknown_live)
                {
                    discover(products, live, files);
                    roots = project_roots(directory);
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
                        const auto& id { file.reader.snapshot().session_id };
                        if (file.ready)
                        {
                            file.published = file.reader.snapshot();
                            file.paths = file.reader.paths();
                        }
                        if (!file.published.has_value())
                        {
                            if (live.contains(id))
                                result.ready = false;
                            continue;
                        }
                        auto session { *file.published };
                        const auto time { now_ms() };
                        covered.insert(session.session_id);
                        session.runtime_loaded = live.contains(session.session_id);
                        session.name = conversation_name(products, session.session_id, names);
                        if (session.cwd.empty())
                            session.cwd = match_antigravity_root(file.paths, roots);
                        if (session.runtime_loaded)
                        {
                            seen_live.insert(session.session_id);
                            closed_at.erase(session.session_id);
                        }
                        else if (seen_live.contains(session.session_id))
                        {
                            const auto closed { closed_at.try_emplace(session.session_id, time).first->second };
                            if (time - closed >= vanished_grace_ms)
                                close_vanished_session(session, closed);
                        }
                        // 위젯 유지 기간이 지난 끝난 하위는 부모 카드에서도 뺀다.
                        std::erase_if(session.children, [time](const auto& child) { return !is_working(child.status) && time - child.updated_ms >= widget_retention_ms; });
                        if (!session.runtime_loaded)
                            close_children(session, session_status::ended, time, false);
                        if (!session.runtime_loaded && time - latest_update(session) >= widget_retention_ms)
                            continue;
                        result.sessions.push_back(std::move(session));
                    }
                    catch (...)
                    {
                        result.error = "일부 Antigravity 로컬 기록을 읽지 못했습니다.";
                    }
                }
                // 아직 요청이 없어 기록이 없는 열린 대화도 잠금만으로 보인다.
                for (const auto& [id, opened] : live)
                    if (!covered.contains(id))
                    {
                        antigravity_transcript empty { id };
                        auto session { empty.snapshot() };
                        session.runtime_loaded = true;
                        session.updated_ms = opened;
                        session.name = conversation_name(products, id, names);
                        result.sessions.push_back(std::move(session));
                    }
            }
            catch (...)
            {
                result.error = "Antigravity 자동 수집 경로를 읽지 못했습니다.";
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
