#include "core/session.h"

#include "core/providers.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace session_board {
    namespace {
        // 이만큼 소식이 없는 작업 중 상태는 그대로 믿지 않는다.
        constexpr std::int64_t stale_after_ms { 10 * 60 * 1000 };

        constexpr std::string_view events[] {
            "session_started",
            "turn_started",
            "tool_started",
            "tool_finished",
            "approval_requested",
            "input_requested",
            "turn_finished",
            "interrupted",
            "session_ended",
            "failed",
            "child_started",
            "child_finished",
        };

        // 하위에 적용할 수 있는 이벤트다. turn·세션의 시작과 끝은 부모만 갖는다.
        constexpr std::string_view child_events[] {
            "child_started",
            "child_finished",
            "tool_started",
            "tool_finished",
            "approval_requested",
            "input_requested",
        };

        constexpr session_status statuses[] {
            session_status::idle,
            session_status::running,
            session_status::waiting_approval,
            session_status::waiting_input,
            session_status::interrupted,
            session_status::ended,
            session_status::failed,
        };

        std::string field(const nlohmann::json& input, const char* key, const std::size_t limit = 1024)
        {
            const auto item { input.find(key) };
            if (item == input.end() || item->is_null())
                return {};
            if (!item->is_string())
                throw std::runtime_error { "Expected a string field." };
            return compact_text(item->get<std::string>(), limit);
        }

        // 없거나 숫자가 아니면 0이다. 창 정보는 이동 단추를 위한 덤이라 이벤트를 버리지 않는다.
        std::uint64_t host_number(const nlohmann::json& input, const char* key)
        {
            const auto item { input.find(key) };
            if (item == input.end() || !item->is_number_integer())
                return 0;
            if (item->is_number_unsigned())
                return item->get<std::uint64_t>();
            const auto value { item->get<std::int64_t>() };
            return value > 0 ? static_cast<std::uint64_t>(value) : 0;
        }

        void validate_identity(const std::string& provider, const std::string& id)
        {
            if (!valid_provider(provider) || id.empty() || id.size() > 512)
                throw std::runtime_error { "Invalid session identity." };
        }

        bool boolean_field(const nlohmann::json& input, const char* key)
        {
            const auto item { input.find(key) };
            if (item == input.end() || item->is_null())
                return false;
            if (!item->is_boolean())
                throw std::runtime_error { "Expected a boolean field." };
            return item->get<bool>();
        }

        session_status parse_status(const std::string& text)
        {
            for (const session_status candidate : statuses)
                if (text == status_name(candidate))
                    return candidate;
            throw std::runtime_error { "Invalid status." };
        }

        child_kind parse_child_kind(const std::string& text)
        {
            if (text.empty() || text == child_kind_name(child_kind::agent))
                return child_kind::agent;
            if (text == child_kind_name(child_kind::background_command))
                return child_kind::background_command;
            throw std::runtime_error { "Invalid child kind." };
        }

        bool apply_child_event(session_snapshot& snapshot, const session_event& event)
        {
            if (snapshot.status == session_status::ended)
                return false;
            const auto kind { parse_child_kind(event.child_type) };
            const auto found { std::ranges::find_if(snapshot.children, [&](const auto& child) { return child.kind == kind && child.child_id == event.child_id; }) };
            if (found != snapshot.children.end() && event.timestamp_ms < found->updated_ms)
                return false;
            const bool start { event.event == "child_started" };
            // 끝난 하위의 늦은 도구 이벤트는 되살리지 않는다. 다시 시작은 재개된 에이전트다.
            if (found != snapshot.children.end() && !is_working(found->status) && !start && event.event != "child_finished")
                return false;

            if (snapshot.session_id.empty())
            {
                snapshot.provider = event.provider;
                snapshot.session_id = event.session_id;
                snapshot.cwd = event.cwd;
                snapshot.model = event.model;
            }
            auto& child { upsert_child(snapshot, kind, event.child_id) };
            if (child.started_ms == 0 || start)
                child.started_ms = event.timestamp_ms;
            child.updated_ms = event.timestamp_ms;
            if (!event.parent_tool_id.empty())
                child.parent_tool_id = event.parent_tool_id;
            if (!event.agent_type.empty())
                child.agent_type = event.agent_type;
            if (!event.title.empty())
                child.title = event.title;
            if (!event.summary.empty())
                child.activity = event.summary;
            child.background = child.background || event.background;

            if (start)
            {
                child.status = session_status::running;
                child.tool_name.clear();
            }
            else if (event.event == "child_finished")
            {
                child.status = event.outcome == "failed" ? session_status::failed : event.outcome == "stopped" ? session_status::interrupted : session_status::idle;
                child.tool_name.clear();
            }
            else if (event.event == "tool_started")
            {
                child.status = session_status::running;
                child.tool_name = event.tool_name;
            }
            else if (event.event == "tool_finished")
            {
                child.status = session_status::running;
                child.tool_name.clear();
            }
            else
            {
                child.status = event.event == "approval_requested" ? session_status::waiting_approval : session_status::waiting_input;
                child.tool_name = event.tool_name;
            }
            ++snapshot.revision;
            return true;
        }

        nlohmann::json serialize_child(const session_child& child)
        {
            nlohmann::json result { { "kind", child_kind_name(child.kind) }, { "child_id", child.child_id }, { "parent_tool_id", child.parent_tool_id }, { "agent_type", child.agent_type } };
            result.update({ { "title", child.title }, { "activity", child.activity }, { "tool_name", child.tool_name }, { "status", status_name(child.status) } });
            result.update({ { "background", child.background }, { "depth", child.depth }, { "started_ms", child.started_ms }, { "updated_ms", child.updated_ms } });
            return result;
        }

        session_child parse_child(const nlohmann::json& input)
        {
            session_child child {};
            child.kind = parse_child_kind(field(input, "kind", 32));
            child.child_id = field(input, "child_id", 513);
            if (child.child_id.empty() || child.child_id.size() > 512)
                throw std::runtime_error { "Invalid child identity." };
            child.parent_tool_id = field(input, "parent_tool_id", 512);
            child.agent_type = field(input, "agent_type", 128);
            child.title = field(input, "title", 180);
            child.activity = field(input, "activity", 240);
            child.tool_name = field(input, "tool_name", 128);
            child.status = parse_status(field(input, "status", 32));
            child.background = boolean_field(input, "background");
            child.depth = input.value("depth", 1u);
            child.started_ms = input.value("started_ms", std::int64_t { 0 });
            child.updated_ms = input.value("updated_ms", std::int64_t { 0 });
            return child;
        }

        // 넘치면 가장 오래 전에 끝난 하위를, 그것도 없으면 가장 오래된 하위를 버린다.
        void trim_children(std::vector<session_child>& children, const std::size_t limit)
        {
            while (children.size() > limit)
            {
                auto victim { children.end() };
                for (auto item { children.begin() }; item != children.end(); ++item)
                    if (!is_working(item->status) && (victim == children.end() || item->updated_ms < victim->updated_ms))
                        victim = item;
                if (victim == children.end())
                    victim = std::ranges::min_element(children, {}, &session_child::updated_ms);
                children.erase(victim);
            }
        }
    } // namespace

    std::int64_t now_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string compact_text(std::string text, const std::size_t limit)
    {
        for (char& character : text)
            if (static_cast<unsigned char>(character) < 32)
                character = ' ';
        if (text.size() > limit)
        {
            std::size_t end { limit };
            while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
                --end;
            text.resize(end);
        }
        return text;
    }

    session_event parse_event(const nlohmann::json& input)
    {
        if (!input.is_object() || input.value("schema_version", 0) != 1)
            throw std::runtime_error { "Unsupported event schema." };
        session_event result {};
        result.provider = field(input, "provider", 16);
        result.session_id = field(input, "session_id", 513);
        validate_identity(result.provider, result.session_id);
        result.event = field(input, "event", 64);
        if (std::ranges::find(events, result.event) == std::end(events))
            throw std::runtime_error { "Unsupported event." };
        if (!input.at("timestamp_ms").is_number_integer())
            throw std::runtime_error { "Expected an integer timestamp." };
        result.timestamp_ms = input.at("timestamp_ms").get<std::int64_t>();
        if (result.timestamp_ms <= 0 || result.timestamp_ms > now_ms() + 300'000)
            throw std::runtime_error { "Invalid timestamp." };
        result.turn_id = field(input, "turn_id", 512);
        result.cwd = field(input, "cwd", 4096);
        result.model = field(input, "model", 128);
        result.title = field(input, "title", 180);
        result.summary = field(input, "summary", 360);
        result.tool_id = field(input, "tool_id", 512);
        result.tool_name = field(input, "tool_name", 128);
        result.source = field(input, "source", 32);
        result.host_window = host_number(input, "host_window");
        result.host_process_id = static_cast<std::uint32_t>(host_number(input, "host_process_id"));
        result.child_id = field(input, "child_id", 513);
        result.child_type = field(input, "child_type", 32);
        result.parent_tool_id = field(input, "parent_tool_id", 512);
        result.agent_type = field(input, "agent_type", 128);
        result.background = boolean_field(input, "background");
        result.outcome = field(input, "outcome", 32);
        static_cast<void>(parse_child_kind(result.child_type));
        if (result.child_id.size() > 512)
            throw std::runtime_error { "Invalid child identity." };
        if (result.child_id.empty() ? result.event.starts_with("child_") : std::ranges::find(child_events, result.event) == std::end(child_events))
            throw std::runtime_error { "Unsupported child event." };
        if (!result.outcome.empty() && result.outcome != "completed" && result.outcome != "failed" && result.outcome != "stopped")
            throw std::runtime_error { "Invalid child outcome." };
        // 원인은 화면의 말을 고르는 열쇠다. 정해진 꼴이 아니면 버리고 이벤트는 받는다.
        result.failure = field(input, "failure", 64);
        if (result.event != "failed"
            || !std::ranges::all_of(result.failure, [](const char character) { return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_'; }))
            result.failure.clear();
        return result;
    }

    std::optional<session_event> codex_event(const nlohmann::json& input)
    {
        if (!input.is_object())
            throw std::runtime_error { "Expected a hook object." };
        // 서브에이전트의 hook은 부모 session_id를 공유하므로 부모 상태에 섞지 않는다.
        if (!field(input, "agent_id").empty())
            return std::nullopt;
        const std::string hook { field(input, "hook_event_name") };
        std::string event {};
        if (hook == "SessionStart")
            event = "session_started";
        else if (hook == "UserPromptSubmit")
            event = "turn_started";
        else if (hook == "PreToolUse")
            event = field(input, "tool_name").find("request_user_input") != std::string::npos ? "input_requested" : "tool_started";
        else if (hook == "PostToolUse")
            event = "tool_finished";
        else if (hook == "PermissionRequest")
            event = "approval_requested";
        else if (hook == "Stop")
            event = "turn_finished";
        else if (hook == "Interrupt")
            event = "interrupted";
        else if (hook == "SessionEnd")
            event = "session_ended";
        else
            return std::nullopt;

        nlohmann::json normalized { { "schema_version", 1 }, { "provider", "codex" }, { "session_id", field(input, "session_id", 513) }, { "event", event }, { "timestamp_ms", now_ms() },
            { "turn_id", field(input, "turn_id", 512) }, { "cwd", field(input, "cwd", 4096) }, { "model", field(input, "model", 128) }, { "tool_id", field(input, "tool_use_id", 512) },
            { "tool_name", field(input, "tool_name", 128) }, { "source", field(input, "source", 32) } };
        if (hook == "UserPromptSubmit")
            normalized["title"] = field(input, "prompt", 180);
        // 명령 인자·도구 출력·전체 대화는 저장하지 않는다.
        return parse_event(normalized);
    }

    bool apply_event(session_snapshot& snapshot, const session_event& event)
    {
        if (!snapshot.session_id.empty() && (snapshot.session_id != event.session_id || snapshot.provider != event.provider))
            throw std::runtime_error { "Session identity collision." };
        // 하위 이벤트는 부모의 시각과 상태를 건드리지 않는다. 백그라운드 하위는 부모 turn과 엇갈려 온다.
        if (!event.child_id.empty())
            return apply_child_event(snapshot, event);
        if (event.timestamp_ms < snapshot.updated_ms)
            return false;
        const bool new_turn { event.event == "turn_started" };
        const bool tool_event { event.event == "tool_started" || event.event == "tool_finished" || event.event == "approval_requested" || event.event == "input_requested" };
        if (!new_turn && event.event != "session_started" && event.event != "session_ended" && !snapshot.turn_id.empty() && !event.turn_id.empty() && snapshot.turn_id != event.turn_id)
            return false;
        if (tool_event && (snapshot.turn_closed || (!snapshot.turn_id.empty() && !event.turn_id.empty() && snapshot.turn_id != event.turn_id)))
            return false;
        if (snapshot.status == session_status::ended && event.event != "session_started" && !new_turn)
            return false;

        snapshot.provider = event.provider;
        snapshot.session_id = event.session_id;
        snapshot.updated_ms = event.timestamp_ms;
        snapshot.last_event = event.event;
        ++snapshot.revision;
        if (!event.cwd.empty())
            snapshot.cwd = event.cwd;
        if (!event.model.empty())
            snapshot.model = event.model;
        if (event.host_window != 0)
        {
            snapshot.host_window = event.host_window;
            snapshot.host_process_id = event.host_process_id;
        }
        if ((new_turn || snapshot.title.empty()) && !event.title.empty())
            snapshot.title = event.title;
        if (!event.turn_id.empty())
            snapshot.turn_id = event.turn_id;
        if (new_turn || !event.summary.empty())
            snapshot.summary = event.summary;

        if (event.event != "failed")
            snapshot.failure.clear();
        if (new_turn)
        {
            snapshot.status = session_status::running;
            snapshot.turn_closed = false;
            snapshot.active_tools.clear();
            snapshot.tool_name.clear();
        }
        else if (event.event == "session_started")
        {
            // 세션을 열기만 해도 재개 Hook이 온다. 열기 전에 끝난 응답을 이번에 처리한 것처럼 `응답 완료`로 남기지 않는다.
            //  - 작업 중의 compact는 진행 중인 turn을 그대로 둔다.
            if (!is_working(snapshot.status))
            {
                snapshot.status = session_status::idle;
                snapshot.turn_closed = false;
            }
        }
        else if (event.event == "tool_started")
        {
            snapshot.status = session_status::running;
            snapshot.tool_name = event.tool_name;
            const std::string key { event.tool_id.empty() ? event.tool_name : event.tool_id };
            if (!key.empty() && std::ranges::find(snapshot.active_tools, key) == snapshot.active_tools.end() && snapshot.active_tools.size() < 64)
                snapshot.active_tools.push_back(key);
        }
        else if (event.event == "tool_finished")
        {
            snapshot.status = session_status::running;
            std::erase(snapshot.active_tools, event.tool_id.empty() ? event.tool_name : event.tool_id);
            if (snapshot.active_tools.empty())
                snapshot.tool_name.clear();
            else
                snapshot.tool_name = "Other active tools";
        }
        else if (event.event == "approval_requested" || event.event == "input_requested")
        {
            snapshot.status = event.event == "approval_requested" ? session_status::waiting_approval : session_status::waiting_input;
            snapshot.tool_name = event.tool_name;
        }
        else
        {
            snapshot.status = event.event == "turn_finished" ? session_status::idle
                : event.event == "interrupted"               ? session_status::interrupted
                : event.event == "failed"                    ? session_status::failed
                                                             : session_status::ended;
            snapshot.turn_closed = true;
            snapshot.active_tools.clear();
            snapshot.tool_name.clear();
            if (event.event == "failed")
                snapshot.failure = event.failure;
            // 포그라운드 하위는 부모 turn 안에서 끝나야 한다. 남아 있으면 끝을 놓친 것이다.
            // 백그라운드 하위는 turn을 넘어 돌고, 세션이 닫힐 때만 함께 닫는다.
            close_children(snapshot, event.event == "interrupted" ? session_status::interrupted : session_status::ended, event.timestamp_ms, event.event != "session_ended");
        }
        return true;
    }

    std::string_view status_name(const session_status status)
    {
        switch (status)
        {
        case session_status::idle:
            return "idle";
        case session_status::running:
            return "running";
        case session_status::waiting_approval:
            return "waiting_approval";
        case session_status::waiting_input:
            return "waiting_input";
        case session_status::interrupted:
            return "interrupted";
        case session_status::ended:
            return "ended";
        case session_status::failed:
            return "failed";
        }
        throw std::runtime_error { "Invalid status." };
    }

    nlohmann::json serialize_snapshot(const session_snapshot& snapshot)
    {
        nlohmann::json result { { "schema_version", 1 }, { "provider", snapshot.provider }, { "session_id", snapshot.session_id }, { "turn_id", snapshot.turn_id }, { "cwd", snapshot.cwd },
            { "model", snapshot.model }, { "title", snapshot.title }, { "summary", snapshot.summary }, { "last_event", snapshot.last_event }, { "tool_name", snapshot.tool_name },
            { "status", status_name(snapshot.status) }, { "updated_ms", snapshot.updated_ms }, { "revision", snapshot.revision }, { "turn_closed", snapshot.turn_closed },
            { "active_tools", snapshot.active_tools }, { "name", snapshot.name }, { "activity", snapshot.activity }, { "host_window", snapshot.host_window },
            { "host_process_id", snapshot.host_process_id } };
        if (!snapshot.failure.empty())
            result["failure"] = snapshot.failure;
        if (!snapshot.children.empty())
        {
            auto& children { result["children"] = nlohmann::json::array() };
            for (const auto& child : snapshot.children)
                children.push_back(serialize_child(child));
        }
        return result;
    }

    session_snapshot parse_snapshot(const nlohmann::json& input)
    {
        if (input.value("schema_version", 0) != 1)
            throw std::runtime_error { "Unsupported snapshot schema." };
        session_snapshot snapshot {};
        snapshot.provider = field(input, "provider", 16);
        snapshot.session_id = field(input, "session_id", 513);
        validate_identity(snapshot.provider, snapshot.session_id);
        snapshot.turn_id = field(input, "turn_id", 512);
        snapshot.cwd = field(input, "cwd", 4096);
        snapshot.model = field(input, "model", 128);
        snapshot.title = field(input, "title", 180);
        snapshot.summary = field(input, "summary", 360);
        snapshot.name = field(input, "name", 180);
        snapshot.activity = field(input, "activity", 180);
        snapshot.last_event = field(input, "last_event", 64);
        snapshot.tool_name = field(input, "tool_name", 128);
        snapshot.host_window = host_number(input, "host_window");
        snapshot.host_process_id = static_cast<std::uint32_t>(host_number(input, "host_process_id"));
        snapshot.updated_ms = input.at("updated_ms").get<std::int64_t>();
        snapshot.revision = input.at("revision").get<std::uint64_t>();
        snapshot.turn_closed = input.at("turn_closed").get<bool>();
        snapshot.active_tools = input.at("active_tools").get<std::vector<std::string>>();
        if (snapshot.active_tools.size() > 64)
            throw std::runtime_error { "Too many tools." };
        snapshot.status = parse_status(field(input, "status", 32));
        snapshot.failure = field(input, "failure", 64);
        // 하위 목록이 없는 이전 스냅숏도 읽는다.
        if (const auto children { input.find("children") }; children != input.end() && children->is_array())
        {
            if (children->size() > maximum_children)
                throw std::runtime_error { "Too many children." };
            for (const auto& child : *children)
                snapshot.children.push_back(parse_child(child));
        }
        return snapshot;
    }

    bool is_working(const session_status status)
    {
        return status == session_status::running || status == session_status::waiting_approval || status == session_status::waiting_input;
    }

    bool needs_attention(const session_snapshot& snapshot, const std::int64_t time_ms)
    {
        // 긴 명령과 비정상 종료는 hook만으로 구별할 수 없다. 완료로 추정하지 않는다.
        //  - 로컬 관측이 세션이 살아 있고 도구 결과가 아직 없다고 말하면 긴 명령이다.
        return is_working(snapshot.status) && time_ms - snapshot.updated_ms >= stale_after_ms && !long_running_tool(snapshot, time_ms);
    }

    bool long_running_tool(const session_snapshot& snapshot, const std::int64_t time_ms)
    {
        return snapshot.runtime_loaded && snapshot.status == session_status::running && !snapshot.active_tools.empty() && time_ms - snapshot.updated_ms >= stale_after_ms;
    }

    void close_vanished_session(session_snapshot& snapshot, const std::int64_t time_ms)
    {
        if (!is_working(snapshot.status))
            return;
        snapshot.status = session_status::ended;
        snapshot.turn_closed = true;
        snapshot.active_tools.clear();
        snapshot.tool_name.clear();
        snapshot.activity = "세션 닫힘 · 마지막 작업 보관 중";
        snapshot.last_event = "session_ended";
        snapshot.updated_ms = std::max(snapshot.updated_ms, time_ms);
        close_children(snapshot, session_status::ended, time_ms, false);
    }

    std::string failure_label(const std::string_view failure)
    {
        constexpr std::pair<std::string_view, std::string_view> labels[] {
            { "rate_limit", "사용 한도 도달" },
            { "overloaded", "서버 과부하" },
            { "authentication_failed", "인증 실패" },
            { "oauth_org_not_allowed", "조직 접근 불가" },
            { "account_on_hold", "계정 보류" },
            { "billing_error", "결제 문제" },
            { "invalid_request", "잘못된 요청" },
            { "model_not_found", "모델 없음" },
            { "server_error", "서버 오류" },
            { "max_output_tokens", "출력 길이 초과" },
            { "cloud_credential_error", "클라우드 자격 증명 오류" },
        };
        for (const auto& [key, text] : labels)
            if (key == failure)
                return std::string { text };
        return "API 오류";
    }

    bool is_widget_visible(const session_snapshot& snapshot, const std::int64_t time_ms)
    {
        return snapshot.runtime_loaded || is_working(snapshot.status) || working_children(snapshot) > 0 || time_ms - latest_update(snapshot) < widget_retention_ms;
    }

    std::string_view child_kind_name(const child_kind kind)
    {
        return kind == child_kind::background_command ? "background_command" : "agent";
    }

    session_child& upsert_child(session_snapshot& snapshot, const child_kind kind, const std::string& child_id)
    {
        const auto found { std::ranges::find_if(snapshot.children, [&](const auto& child) { return child.kind == kind && child.child_id == child_id; }) };
        if (found != snapshot.children.end())
            return *found;
        trim_children(snapshot.children, maximum_children - 1);
        auto& child { snapshot.children.emplace_back() };
        child.kind = kind;
        child.child_id = child_id;
        child.background = kind == child_kind::background_command;
        return child;
    }

    void close_children(session_snapshot& snapshot, const session_status status, const std::int64_t time_ms, const bool keep_background)
    {
        for (auto& child : snapshot.children)
            if (is_working(child.status) && !(keep_background && child.background))
            {
                child.status = status;
                child.tool_name.clear();
                child.updated_ms = std::max(child.updated_ms, time_ms);
            }
    }

    void merge_children(std::vector<session_child>& target, const std::vector<session_child>& other)
    {
        for (const auto& child : other)
        {
            const auto found { std::ranges::find_if(target, [&](const auto& item) { return item.kind == child.kind && item.child_id == child.child_id; }) };
            if (found == target.end())
                target.push_back(child);
            else if (child.updated_ms > found->updated_ms)
            {
                // 한쪽 경로만 아는 설명은 새 상태로 덮어도 남긴다.
                auto merged { child };
                for (auto member : { &session_child::parent_tool_id, &session_child::agent_type, &session_child::title, &session_child::activity })
                    if ((merged.*member).empty())
                        merged.*member = (*found).*member;
                merged.background = merged.background || found->background;
                if (found->started_ms != 0 && (merged.started_ms == 0 || found->started_ms < merged.started_ms))
                    merged.started_ms = found->started_ms;
                *found = std::move(merged);
            }
        }
        trim_children(target, maximum_children);
    }

    bool child_needs_attention(const session_child& child, const std::int64_t time_ms)
    {
        if (child.status == session_status::waiting_approval || child.status == session_status::waiting_input)
            return true;
        if (child.status == session_status::failed)
            return time_ms - child.updated_ms < widget_retention_ms;
        // 백그라운드 명령은 끝날 때까지 소식이 없는 것이 정상이다. 에이전트만 오래된 진행을 의심한다.
        return child.kind == child_kind::agent && child.status == session_status::running && time_ms - child.updated_ms >= 10 * 60 * 1000;
    }

    std::int64_t latest_update(const session_snapshot& snapshot)
    {
        std::int64_t result { snapshot.updated_ms };
        for (const auto& child : snapshot.children)
            result = std::max(result, child.updated_ms);
        return result;
    }

    std::size_t working_children(const session_snapshot& snapshot)
    {
        return static_cast<std::size_t>(std::ranges::count_if(snapshot.children, [](const auto& child) { return is_working(child.status); }));
    }

    bool children_need_attention(const session_snapshot& snapshot, const std::int64_t time_ms)
    {
        return std::ranges::any_of(snapshot.children, [time_ms](const auto& child) { return child_needs_attention(child, time_ms); });
    }
} // namespace session_board
