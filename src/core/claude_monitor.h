#pragma once

#include "core/session.h"

#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace session_board {
    // Claude Code Hook 원본 입력을 공통 규격 이벤트로 바꾼다. 서브에이전트와 상태가 아닌 알림은 버린다.
    [[nodiscard]] std::optional<session_event> claude_event(const nlohmann::json& input);

    // `<task-notification>`이 알린 백그라운드 작업의 끝이다. 에이전트와 명령이 같은 ID 공간을 쓴다.
    struct claude_task_outcome
    {
        session_status status { session_status::idle };
        std::int64_t time_ms { 0 };
    };

    class claude_transcript final
    {
    public:
        // sidechain이면 하위 에이전트 기록(`subagents/agent-<id>.jsonl`)을 읽는다. 부모 기록에서는 하위 레코드를 버린다.
        explicit claude_transcript(bool sidechain = false);
        void consume(const nlohmann::json& record);
        [[nodiscard]] const session_snapshot& snapshot() const noexcept;
        [[nodiscard]] bool sidechain() const noexcept;
        // 처음 받아들인 레코드의 시각이다. 하위 에이전트의 시작 시각으로 쓴다.
        [[nodiscard]] std::int64_t started_ms() const noexcept;
        [[nodiscard]] const std::map<std::string, claude_task_outcome>& task_outcomes() const noexcept;

    private:
        void public_message(const std::string& text);
        void tool_started(const std::string& id, const std::string& name);
        void tool_finished(const std::string& id);
        void background_started(const nlohmann::json& record, const std::string& tool_id, std::int64_t time_ms);
        // 글에 든 `<task-notification>`을 모두 해석한다. 하나라도 있으면 참이다.
        bool task_notification(const std::string& text, std::int64_t time_ms);
        void task_finished(const std::string& id, session_status status, std::int64_t time_ms);
        session_snapshot snapshot_ {};
        bool sidechain_ { false };
        std::int64_t started_ms_ { 0 };
        // 이번 turn의 마지막 공개 글이 묻고 끝났는가, 데스크톱 앱의 질문 상자(`wrap_up`의 `ask`)를 띄웠는가다.
        //  - turn이 끝날 때 둘 중 하나면 응답 대기다. 새 요청이 오면 거둔다.
        bool text_asks_ { false };
        bool box_asks_ { false };
        // 백그라운드로 띄운 명령의 tool_use ID와 설명이다. 결과 레코드의 작업 ID와 짝짓는다.
        std::map<std::string, std::string> background_requests_ {};
        // 작업 멈춤 도구의 tool_use ID와 멈출 작업 ID다.
        std::map<std::string, std::string> stop_requests_ {};
        std::map<std::string, claude_task_outcome> task_outcomes_ {};
    };

    // `subagents/agent-<id>.meta.json`의 내용이다. 하위 에이전트에게 준 프롬프트는 읽지 않는다.
    struct claude_agent_meta
    {
        std::string agent_type {};
        std::string description {};
        std::string tool_use_id {};
        std::uint32_t depth { 1 };
        bool background { false };
        bool finished { false };
    };

    [[nodiscard]] std::optional<claude_agent_meta> parse_claude_agent_meta(const nlohmann::json& input);
    // 메타·하위 기록·작업 알림을 하나의 하위 행으로 합친다. 기록을 아직 읽지 않았으면 transcript는 비어 있다.
    [[nodiscard]] session_child make_claude_agent_child(
        const std::string& agent_id, const claude_agent_meta& meta, const claude_transcript* transcript, std::int64_t meta_time_ms, const std::map<std::string, claude_task_outcome>& outcomes);

    // `<설정 루트>/sessions/<pid>.json`의 실행 중 세션 등록 정보다. 같은 폴더의 키 파일은 읽지 않는다.
    struct claude_runtime
    {
        std::uint32_t pid { 0 };
        std::uint64_t process_start { 0 };
        std::string session_id {};
        std::string cwd {};
        std::string name {};
        std::string status {};
        std::string waiting_for {};
        std::string version {};
        std::string domain {};
        // 이 프로세스가 세션을 연 시각이다. 그보다 앞서 끝난 응답은 이번에 처리한 것이 아니다. 모르면 0이다.
        std::int64_t started_ms { 0 };
        std::int64_t updated_ms { 0 };
    };

    struct claude_observation
    {
        std::vector<session_snapshot> sessions {};
        std::string error {};
        bool ready { false };
    };

    [[nodiscard]] std::filesystem::path default_claude_directory();
    [[nodiscard]] std::optional<claude_runtime> parse_claude_runtime(const nlohmann::json& input);
    [[nodiscard]] bool claude_process_alive(const claude_runtime& runtime);
    void apply_claude_runtime(session_snapshot& snapshot, const claude_runtime& runtime);
    void merge_claude_sessions(std::vector<session_snapshot>& sessions, const std::vector<session_snapshot>& observed);

    class claude_monitor final
    {
    public:
        explicit claude_monitor(std::filesystem::path directory);
        ~claude_monitor();
        [[nodiscard]] claude_observation snapshot() const;

    private:
        void run(std::stop_token stop, const std::filesystem::path& directory);
        mutable std::mutex mutex_ {};
        claude_observation observation_ {};
        std::mutex wait_mutex_ {};
        std::condition_variable_any wake_ {};
        std::jthread worker_ {};
    };
} // namespace session_board
