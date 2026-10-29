#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace session_board {
    enum class session_status
    {
        idle,
        running,
        waiting_approval,
        waiting_input,
        interrupted,
        ended,
        failed,
    };

    struct session_event
    {
        std::string provider {};
        std::string session_id {};
        std::string event {};
        std::int64_t timestamp_ms { 0 };
        std::string turn_id {};
        std::string cwd {};
        std::string model {};
        std::string title {};
        std::string summary {};
        std::string tool_id {};
        std::string tool_name {};
        std::string source {};
        // 이 세션이 열린 최상위 창과 그 프로세스다. Hook이 실행될 때 찾아 싣는다. 모르면 0이다.
        std::uint64_t host_window { 0 };
        std::uint32_t host_process_id { 0 };
        // 값이 있으면 부모 세션이 아닌 그 하위 에이전트·백그라운드 작업에 적용한다.
        std::string child_id {};
        // agent 또는 background_command다. 하위 모델의 child_kind와 같은 이름을 쓴다.
        std::string child_type {};
        std::string parent_tool_id {};
        std::string agent_type {};
        bool background { false };
        // child_finished의 끝난 모양이다: completed, failed, stopped.
        std::string outcome {};
        // failed의 원인이다. Claude `StopFailure`의 오류 종류(`rate_limit` 등)를 그대로 싣는다.
        std::string failure {};
    };

    enum class child_kind
    {
        agent,
        background_command,
    };

    // 부모 세션이 띄운 하위 에이전트나 백그라운드 명령이다. 부모 카드 안의 한 줄로 보인다.
    //  - status의 idle은 완료, interrupted는 중지, ended는 끝을 확인하지 못한 채 닫힘이다.
    struct session_child
    {
        child_kind kind { child_kind::agent };
        std::string child_id {};
        std::string parent_tool_id {};
        std::string agent_type {};
        std::string title {};
        std::string activity {};
        std::string tool_name {};
        session_status status { session_status::running };
        bool background { false };
        std::uint32_t depth { 1 };
        std::int64_t started_ms { 0 };
        std::int64_t updated_ms { 0 };
    };

    inline constexpr std::size_t maximum_children { 24 };

    struct session_snapshot
    {
        std::string provider {};
        std::string session_id {};
        std::string turn_id {};
        std::string cwd {};
        std::string model {};
        std::string title {};
        std::string summary {};
        std::string last_event {};
        std::string tool_name {};
        session_status status { session_status::idle };
        std::int64_t updated_ms { 0 };
        std::uint64_t revision { 0 };
        bool turn_closed { false };
        // 끝난 응답이 사용자에게 묻고 끝났다 (물음표로 끝난 마지막 글, 데스크톱 앱의 질문 상자). 응답 대기와 응답 완료를 가른다.
        //  - 새 요청이 시작되면 거둔다.
        bool awaiting_reply { false };
        std::vector<std::string> active_tools {};
        std::string name {};
        std::string activity {};
        std::string observation_source {};
        bool runtime_loaded { false };
        std::uint64_t host_window { 0 };
        std::uint32_t host_process_id { 0 };
        std::vector<session_child> children {};
        // 데스크톱 앱에서 이 세션을 여는 ID다. 저장하지 않고 로컬 기록을 읽을 때마다 채운다.
        //  - Claude는 데스크톱이 붙인 `local_…` ID, Codex는 데스크톱 앱이 연 스레드의 세션 ID 그대로다.
        std::string desktop_session_id {};
        // 오류로 끝난 원인이다. 오류가 아닌 상태로 바뀌면 비운다.
        std::string failure {};
    };

    [[nodiscard]] std::int64_t now_ms();
    [[nodiscard]] std::string compact_text(std::string text, std::size_t limit);
    [[nodiscard]] session_event parse_event(const nlohmann::json& input);
    [[nodiscard]] std::optional<session_event> codex_event(const nlohmann::json& input);
    [[nodiscard]] bool apply_event(session_snapshot& snapshot, const session_event& event);
    [[nodiscard]] nlohmann::json serialize_snapshot(const session_snapshot& snapshot);
    [[nodiscard]] session_snapshot parse_snapshot(const nlohmann::json& input);
    [[nodiscard]] std::string_view status_name(session_status status);
    [[nodiscard]] bool needs_attention(const session_snapshot& snapshot, std::int64_t time_ms);
    // 열린 세션의 도구 하나가 오래 도는 중인가다. 결과가 아직 없는 도구와 살아 있는 세션이 근거라 `상태 확인 필요`로 보지 않는다.
    [[nodiscard]] bool long_running_tool(const session_snapshot& snapshot, std::int64_t time_ms);
    // 열려 있던 세션이 닫혔는데 마지막 상태가 작업 중이면 종료로 확정한다. 닫힌 세션은 더 이상 소식을 남기지 못한다.
    void close_vanished_session(session_snapshot& snapshot, std::int64_t time_ms);
    // 닫힘을 확정하기 전에 기다리는 시간이다. 쓰는 도중의 등록 파일이나 잠금을 다시 잡는 순간을 닫힘으로 읽지 않는다.
    inline constexpr std::int64_t vanished_grace_ms { 2000 };
    // 오류 종류를 화면에 쓰는 말로 바꾼다. 모르는 값은 일반 오류로 말한다.
    [[nodiscard]] std::string failure_label(std::string_view failure);
    [[nodiscard]] bool is_working(session_status status);
    // 응답 글이 사용자에게 묻고 끝나는가다. 끝의 공백과 마크다운 꾸밈을 걷은 뒤 물음표로 끝나면 참이다.
    [[nodiscard]] bool asks_user(std::string_view text);
    inline constexpr std::int64_t widget_retention_ms { 15 * 60 * 1000 };
    [[nodiscard]] bool is_widget_visible(const session_snapshot& snapshot, std::int64_t time_ms);

    // 하위가 있으면 고치고, 없으면 상한 안에서 새로 만든다. 넘치면 끝난 하위부터 버린다.
    session_child& upsert_child(session_snapshot& snapshot, child_kind kind, const std::string& child_id);
    // 부모 turn이 끝나거나 세션이 닫힐 때 아직 도는 하위를 닫는다. 백그라운드 하위는 keep_background면 둔다.
    void close_children(session_snapshot& snapshot, session_status status, std::int64_t time_ms, bool keep_background);
    // 같은 하위는 더 최근 것을 남기고 나머지는 합친다. 로컬 기록과 Hook 경로가 각자 아는 하위를 잃지 않는다.
    void merge_children(std::vector<session_child>& target, const std::vector<session_child>& other);
    [[nodiscard]] std::string_view child_kind_name(child_kind kind);
    [[nodiscard]] bool child_needs_attention(const session_child& child, std::int64_t time_ms);
    // 부모와 하위 중 가장 최근 소식이다. 카드의 나이와 위젯 유지 기간이 이 값을 쓴다.
    [[nodiscard]] std::int64_t latest_update(const session_snapshot& snapshot);
    [[nodiscard]] std::size_t working_children(const session_snapshot& snapshot);
    [[nodiscard]] bool children_need_attention(const session_snapshot& snapshot, std::int64_t time_ms);
} // namespace session_board
