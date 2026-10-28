#pragma once

#include "core/session.h"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace session_board {
    // `brain/<대화 ID>/.system_generated/logs/transcript.jsonl` 한 줄씩을 세션 상태로 바꾼다.
    //  - 기록에는 대화 ID가 없어 폴더 이름으로 받는다.
    //  - 비공개 `thinking`, 도구 인자의 명령·내용, 도구 결과는 쓰지 않는다. 경로 인자만 작업 폴더를 찾는 데 쓴다.
    class antigravity_transcript final
    {
    public:
        explicit antigravity_transcript(std::string session_id = {});
        void consume(const nlohmann::json& record);
        [[nodiscard]] const session_snapshot& snapshot() const noexcept;
        // 도구 인자에 나온 절대 경로다. 최근 것이 뒤에 온다.
        [[nodiscard]] const std::vector<std::string>& paths() const noexcept;

    private:
        void tool_started(const std::string& id, const std::string& name, const nlohmann::json& arguments);
        void tool_finished(session_status outcome);
        void background_started(const std::string& content);
        void system_message(const std::string& content, std::int64_t time_ms);
        void remember_path(std::string path);
        session_snapshot snapshot_ {};
        // 결과 기록을 아직 받지 못한 도구다. 결과는 호출 순서대로 하나씩 온다.
        struct pending_tool
        {
            std::string id {};
            std::string name {};
            // 백그라운드로 넘어갈 때 하위 행 제목으로 쓴다 (모델이 쓴 짧은 설명).
            std::string summary {};
        };
        std::vector<pending_tool> pending_ {};
        std::vector<std::string> paths_ {};
        // 끝난 모양으로 적용한 마지막 단계(`step_index`)다.
        std::int64_t last_step_ { -1 };
    };

    // `annotations/<대화 ID>.pbtxt`의 `title:"…"`이다. protobuf 텍스트 이스케이프를 푼다. 없으면 비어 있다.
    [[nodiscard]] std::string parse_antigravity_title(std::string_view text);
    // `config/projects/*.json`의 프로젝트 루트(`projectResources.resources[].folderUri`)다. `file:///` 주소만 경로로 바꾼다.
    [[nodiscard]] std::vector<std::string> parse_antigravity_project_roots(const nlohmann::json& project);
    // 경로들을 최근 것부터 루트와 맞춰 처음 맞은 루트를 돌려준다. 가장 긴 루트가 이긴다. 맞는 것이 없으면 비어 있다.
    [[nodiscard]] std::string match_antigravity_root(const std::vector<std::string>& paths, const std::vector<std::string>& roots);

    struct antigravity_observation
    {
        std::vector<session_snapshot> sessions {};
        std::string error {};
        bool ready { false };
    };

    // `%USERPROFILE%/.gemini`다.
    [[nodiscard]] std::filesystem::path default_antigravity_directory();
    void merge_antigravity_sessions(std::vector<session_snapshot>& sessions, const std::vector<session_snapshot>& observed);

    // 홈 아래의 제품 폴더(`antigravity`, `antigravity-ide`, `antigravity-cli`)를 함께 본다.
    //  - 열린 대화는 `presence/<대화 ID>.lock`을 그 앱이 막고 있는지로 가른다. `presence`가 없는 제품은 최근 기록으로만 보인다.
    class antigravity_monitor final
    {
    public:
        explicit antigravity_monitor(std::filesystem::path directory);
        ~antigravity_monitor();
        [[nodiscard]] antigravity_observation snapshot() const;

    private:
        void run(std::stop_token stop, const std::filesystem::path& directory);
        mutable std::mutex mutex_ {};
        antigravity_observation observation_ {};
        std::mutex wait_mutex_ {};
        std::condition_variable_any wake_ {};
        std::jthread worker_ {};
    };
} // namespace session_board
