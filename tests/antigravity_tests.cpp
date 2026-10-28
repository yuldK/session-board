#include "core/antigravity_monitor.h"
#include "core/local_record.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace session_board {
    namespace {
        int assertions { 0 };
        void require(const bool condition, const char* message)
        {
            ++assertions;
            if (!condition)
                throw std::runtime_error { message };
        }

        const std::string conversation { "9e3b15f7-0ad9-41aa-8e30-6e10cf3f7552" };

        nlohmann::json step(const std::int64_t index, const std::string& source, const std::string& type, const std::string& status, nlohmann::json extra = nlohmann::json::object())
        {
            nlohmann::json record { { "step_index", index }, { "source", source }, { "type", type }, { "status", status }, { "created_at", "2026-10-28T12:32:11Z" } };
            record.update(extra);
            return record;
        }

        nlohmann::json call(const std::string& name, nlohmann::json arguments = nlohmann::json::object())
        {
            return { { "name", name }, { "args", std::move(arguments) } };
        }

        void transcript()
        {
            antigravity_transcript reader { conversation };
            const auto& view { reader.snapshot() };
            require(view.provider == "antigravity" && view.session_id == conversation && view.observation_source == "antigravity-local", "Identity not set.");
            std::string request { "<USER_REQUEST>\nAntigravity를 붙여 줘\n두 번째 줄은 제목이 아니다\n</USER_REQUEST>\n" };
            request += "<ADDITIONAL_METADATA>The current local time is now.</ADDITIONAL_METADATA>\n<USER_SETTINGS_CHANGE>The user changed setting `Model Selection` from None to ";
            request += "Gemini 4 Argon (Medium). No need to comment.</USER_SETTINGS_CHANGE>";
            reader.consume(step(0, "USER_EXPLICIT", "USER_INPUT", "DONE", { { "content", request } }));
            require(view.title == "Antigravity를 붙여 줘" && view.model == "Gemini 4 Argon (Medium)" && view.turn_id == "step-0", "User request not parsed.");
            require(view.status == session_status::running && !view.turn_closed && view.summary.empty() && view.updated_ms == iso_time_ms("2026-10-28T12:32:11Z"), "Turn not started.");

            // 비공개 thinking은 쓰지 않고 공개 content의 첫 문장만 쓴다. 도구 인자의 명령은 어디에도 남기지 않는다.
            auto planner { step(1, "MODEL", "PLANNER_RESPONSE", "DONE", { { "thinking", "비공개 추론" }, { "content", "**구조를 먼저 확인하겠습니다.** 그다음 고칩니다." } }) };
            planner["tool_calls"].push_back(call("read_file", { { "Path", "\"E:\\\\fixture\\\\src\\\\a.cpp\"" }, { "StartLine", "1" } }));
            planner["tool_calls"].push_back(call("run_command", { { "CommandLine", "Remove-Item secret" }, { "toolSummary", "\"파일 목록 조회\"" }, { "Cwd", "E:\\fixture" } }));
            reader.consume(planner);
            require(view.summary == "구조를 먼저 확인하겠습니다." && view.active_tools.size() == 2 && view.activity == "명령 실행 중 · run_command", "Planner tools not started.");
            require(reader.paths().size() == 2 && reader.paths().front() == "E:\\fixture\\src\\a.cpp", "Tool paths not remembered.");
            reader.consume(step(2, "MODEL", "GENERIC", "DONE", { { "content", "파일 내용" } }));
            require(view.active_tools.size() == 1 && view.activity == "명령 실행 중 · run_command", "First tool result not applied in order.");

            // 백그라운드로 넘어간 명령은 하위 행이다. 작업 ID 전체를 쓰고 제목은 모델이 쓴 설명이다.
            const std::string task { conversation + "/task-3" };
            std::string started { "Created At: 2026-10-28T21:32:18+09:00\nTool is running as a background task with task id: " + task };
            started += "\nTask Description: Remove-Item secret\nLog output: x";
            const auto background { step(3, "MODEL", "GENERIC", "RUNNING", { { "content", started } }) };
            reader.consume(background);
            require(view.active_tools.empty() && view.activity == "분석 또는 응답 작성 중" && view.status == session_status::running, "Background start kept the tool waiting.");
            require(view.children.size() == 1 && view.children[0].child_id == task && view.children[0].kind == child_kind::background_command, "Background command not a child.");
            require(view.children[0].title == "파일 목록 조회" && view.children[0].status == session_status::running, "Background child title differs.");
            reader.consume(background);
            require(view.children.size() == 1, "Replayed step applied twice.");

            reader.consume(step(4, "MODEL", "PLANNER_RESPONSE", "DONE", { { "content", "" }, { "tool_calls", { call("ask_question") } } }));
            require(view.status == session_status::waiting_input && view.activity == "사용자 답변 대기", "Question not shown as input wait.");
            reader.consume(step(5, "MODEL", "GENERIC", "ERROR", { { "content", "도구 실패" } }));
            require(view.status == session_status::running && view.failure.empty(), "Single tool error failed the session.");

            // 백그라운드 완료 알림은 새 turn이 아니다. 잘린 알림도 앞부분만으로 닫는다.
            std::string message { "<SYSTEM_MESSAGE> [Message] timestamp=2026-10-28T12:32:26Z sender=" + task + " priority=MESSAGE_PRIORITY_HIGH" };
            message += " content=Task id \"" + task + "\" finished with result: The tool";
            reader.consume(step(6, "SYSTEM", "SYSTEM_MESSAGE", "DONE", { { "truncated_fields", { "content" } }, { "content", message } }));
            require(view.children[0].status == session_status::idle && view.turn_id == "step-0" && view.status == session_status::running, "Background completion mishandled.");

            // 끝나지 않은 같은 단계는 다시 받고, 끝난 모양으로 한 번만 적용한다. 답변의 제목보다 본문 첫 문장이 작업을 말한다.
            reader.consume(step(7, "MODEL", "PLANNER_RESPONSE", "RUNNING", { { "content", "### 작성 중" } }));
            require(!view.turn_closed && view.summary == "작성 중", "Running response closed the turn.");
            reader.consume(step(7, "MODEL", "PLANNER_RESPONSE", "DONE", { { "content", "### 결과\n\n**완료했습니다.** 추가로 확인할 것은 없습니다." } }));
            require(view.status == session_status::idle && view.turn_closed && view.activity == "응답 완료" && view.summary == "완료했습니다.", "Final answer not closed.");
            reader.consume(step(8, "SYSTEM", "CHECKPOINT", "DONE", { { "content", "# USER Objective:\n다른 제목" } }));
            require(view.title == "Antigravity를 붙여 줘", "Checkpoint replaced the request title.");

            // 하위 에이전트 호출은 결과가 올 때까지 하위 행이다.
            reader.consume(step(9, "USER_EXPLICIT", "USER_INPUT", "DONE", { { "content", "<USER_REQUEST>\n조사해 줘\n</USER_REQUEST>" } }));
            reader.consume(step(10, "MODEL", "PLANNER_RESPONSE", "DONE", { { "tool_calls", { call("invoke_subagent", { { "Role", "\"코드 조사\"" }, { "TypeName", "research" } }) } } }));
            const auto agent { std::ranges::find_if(view.children, [](const auto& child) { return child.kind == child_kind::agent; }) };
            require(agent != view.children.end() && agent->title == "코드 조사" && agent->status == session_status::running, "Subagent call not a child.");
            reader.consume(step(11, "MODEL", "GENERIC", "DONE"));
            require(std::ranges::any_of(view.children, [](const auto& child) { return child.kind == child_kind::agent && child.status == session_status::idle; }), "Subagent result not closed.");

            antigravity_transcript headless { conversation };
            headless.consume(step(0, "SYSTEM", "CHECKPOINT", "DONE", { { "content", "요약\n# USER Objective:\n체크포인트 제목\n다음" } }));
            require(headless.snapshot().title == "체크포인트 제목", "Checkpoint title not used as fallback.");
            headless.consume(nlohmann::json::array());
            headless.consume({ { "step_index", "x" }, { "source", 3 } });
            require(headless.snapshot().status == session_status::idle, "Malformed record changed state.");
        }

        void annotations_and_roots()
        {
            require(parse_antigravity_title(R"(title:"Antigravity \"검토\""  last_user_view_time:{seconds:1793190758  nanos:997000000})") == "Antigravity \"검토\"", "Escaped title differs.");
            require(parse_antigravity_title(R"(title:"\355\225\234 \\ 끝")") == "한 \\ 끝", "Octal UTF-8 title differs.");
            require(parse_antigravity_title(R"(subtitle:"아님"  title:"맞음")") == "맞음", "Other field taken as title.");
            require(parse_antigravity_title(R"(last_user_view_time:{seconds:1})").empty() && parse_antigravity_title(R"(title:"잘림)").empty(), "Missing or cut title not empty.");

            const nlohmann::json folder { { "projectResources", { { "resources", { { { "folderUri", "file:///e%3A/Programming/p1" } } } } } } };
            const nlohmann::json git { { "projectResources", { { "resources", { { { "gitFolder", { { "folderUri", "file:///e%3A/Programming/Lusain" }, { "defaultBranch", "main" } } } } } } } } };
            require(parse_antigravity_project_roots(folder) == std::vector<std::string> { "e:\\Programming\\p1" }, "Folder root differs.");
            require(parse_antigravity_project_roots(git) == std::vector<std::string> { "e:\\Programming\\Lusain" }, "Git folder root differs.");
            require(parse_antigravity_project_roots({ { "id", "outside" } }).empty() && parse_antigravity_project_roots(nlohmann::json::array()).empty(), "Project without resources gave a root.");
            const nlohmann::json remote { { "projectResources", { { "resources", { { { "folderUri", "file://server/share" } } } } } } };
            require(parse_antigravity_project_roots(remote).empty(), "Remote URI used as a local root.");

            const std::vector<std::string> roots { "e:\\Programming\\p1", "e:\\Programming\\Lusain", "e:\\Programming\\Lusain\\session-board" };
            require(match_antigravity_root({ "C:\\Other\\x.txt", "E:/Programming/Lusain/session-board/src/a.cpp" }, roots) == "e:\\Programming\\Lusain\\session-board", "Longest root not chosen.");
            require(match_antigravity_root({ "E:\\Programming\\p10\\a.cpp" }, roots).empty(), "Sibling folder matched a root.");
            require(match_antigravity_root({ "e:\\programming\\p1\\x", "C:\\elsewhere" }, roots) == "e:\\Programming\\p1", "Older matching path not used.");
            require(match_antigravity_root({}, roots).empty(), "Empty paths matched.");
        }

        void merging()
        {
            session_snapshot hook {};
            hook.provider = "antigravity";
            hook.session_id = conversation;
            hook.cwd = "E:\\fixture";
            hook.model = "gemini-fixture";
            hook.host_window = 42;
            hook.host_process_id = 7;
            hook.status = session_status::running;
            hook.updated_ms = now_ms();
            session_snapshot local {};
            local.provider = "antigravity";
            local.session_id = conversation;
            local.status = session_status::idle;
            local.summary = "완료했습니다.";
            local.updated_ms = now_ms() - 1000;
            session_snapshot other { hook };
            other.provider = "codex";
            std::vector<session_snapshot> sessions { hook, other };
            merge_antigravity_sessions(sessions, { local });
            require(sessions.size() == 2, "Same ID across providers merged.");
            const auto merged { std::ranges::find_if(sessions, [](const auto& session) { return session.provider == "antigravity"; }) };
            require(merged->status == session_status::idle && merged->summary == "완료했습니다.", "Local record not used as the source of state.");
            require(merged->cwd == "E:\\fixture" && merged->model == "gemini-fixture" && merged->host_window == 42 && merged->host_process_id == 7, "Hook-only fields lost.");
        }

        void write_lines(const std::filesystem::path& path, const std::vector<nlohmann::json>& records, const bool append = false)
        {
            std::ofstream file { path, std::ios::binary | (append ? std::ios::app : std::ios::trunc) };
            for (const auto& record : records)
                file << record.dump() << '\n';
        }

        void discovery(const std::filesystem::path& directory)
        {
            const auto home { directory / "gemini" };
            const auto product { home / "antigravity" };
            const auto logs { product / "brain" / conversation / ".system_generated" / "logs" };
            std::filesystem::create_directories(logs);
            std::filesystem::create_directories(product / "presence");
            std::filesystem::create_directories(product / "annotations");
            std::filesystem::create_directories(home / "config" / "projects");
            std::ofstream { product / "annotations" / (conversation + ".pbtxt"), std::ios::binary } << R"(title:"열린 대화"  last_user_view_time:{seconds:1  nanos:2})";
            std::ofstream { home / "config" / "projects" / "p.json", std::ios::binary }
                << nlohmann::json { { "projectResources", { { "resources", { { { "folderUri", "file:///e%3A/fixture" } } } } } } }.dump();
            std::ofstream { home / "config" / "projects" / "broken.json", std::ios::binary } << "{ not json";
            const auto path { logs / "transcript.jsonl" };
            auto first_tool { step(1, "MODEL", "PLANNER_RESPONSE", "DONE", { { "content", "기존 대화를 확인 중입니다." } }) };
            first_tool["tool_calls"].push_back(call("read_file", { { "Path", "e:\\fixture\\a.txt" } }));
            write_lines(path, { step(0, "USER_EXPLICIT", "USER_INPUT", "DONE", { { "content", "<USER_REQUEST>\n요청\n</USER_REQUEST>" } }), first_tool });
            std::ofstream { path, std::ios::binary | std::ios::app } << "{ broken line\n";

            // 닫힌 대화의 잠금 파일은 남아 있어도 열린 대화가 아니다.
            const auto lock { product / "presence" / (conversation + ".lock") };
            std::ofstream { lock }.close();
            require(!file_lock_held(lock), "Released presence lock considered open.");
            const auto close_handle { [](void* handle) { CloseHandle(handle); } };
            std::unique_ptr<void, decltype(close_handle)> held { CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
            require(held.get() != INVALID_HANDLE_VALUE && file_lock_held(lock), "Exclusive presence lock not detected.");
            // 기록이 아직 없는 열린 대화도 잠금만으로 보인다.
            const std::string fresh { "11111111-2222-3333-4444-555555555555" };
            const auto fresh_lock { product / "presence" / (fresh + ".lock") };
            std::ofstream { fresh_lock }.close();
            std::unique_ptr<void, decltype(close_handle)> fresh_held { CreateFileW(fresh_lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
            // `presence`가 없는 제품의 기록은 최근 기록으로만 보인다.
            const std::string ide { "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee" };
            const auto ide_logs { home / "antigravity-ide" / "brain" / ide / ".system_generated" / "logs" };
            std::filesystem::create_directories(ide_logs);
            write_lines(ide_logs / "transcript.jsonl",
                { step(0, "USER_EXPLICIT", "USER_INPUT", "DONE", { { "created_at", "2099-01-01T00:00:00Z" }, { "content", "<USER_REQUEST>\nIDE 요청\n</USER_REQUEST>" } }) });

            antigravity_monitor monitor { home };
            const auto wait = [&](const std::string& id, const auto& predicate) {
                const auto deadline { std::chrono::steady_clock::now() + std::chrono::seconds { 6 } };
                while (std::chrono::steady_clock::now() < deadline)
                {
                    const auto view { monitor.snapshot() };
                    for (const auto& session : view.sessions)
                        if (session.session_id == id && predicate(session))
                            return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
                }
                return false;
            };
            require(wait(conversation,
                        [](const auto& session) {
                            return session.runtime_loaded && session.name == "열린 대화" && session.summary == "기존 대화를 확인 중입니다." && session.cwd == "e:\\fixture"
                                && session.status == session_status::running && session.activity == "파일 읽는 중 · read_file";
                        }),
                "Open conversation not collected.");
            require(wait(fresh, [](const auto& session) { return session.runtime_loaded && session.activity == "다음 요청 대기"; }), "Open conversation without a transcript hidden.");
            require(wait(ide, [](const auto& session) { return !session.runtime_loaded && session.title == "IDE 요청"; }), "Product without presence not collected as recent.");
            require(monitor.snapshot().ready && monitor.snapshot().error.empty(), "Observation not ready.");

            // 끝에 줄바꿈이 없는 줄은 다음 읽기까지 미룬다.
            const auto answer { step(3, "MODEL", "PLANNER_RESPONSE", "DONE", { { "content", "읽기를 마쳤습니다." } }).dump() };
            write_lines(path, { step(2, "MODEL", "GENERIC", "DONE") }, true);
            std::ofstream { path, std::ios::binary | std::ios::app } << answer;
            require(wait(conversation, [](const auto& session) { return session.activity == "분석 또는 응답 작성 중"; }), "Tool result not collected.");
            std::this_thread::sleep_for(std::chrono::milliseconds { 1100 });
            require(wait(conversation, [](const auto& session) { return !session.turn_closed; }), "Partial line applied prematurely.");
            std::ofstream { path, std::ios::binary | std::ios::app } << '\n';
            require(wait(conversation, [](const auto& session) { return session.turn_closed && session.summary == "읽기를 마쳤습니다." && session.runtime_loaded; }), "Answer not collected.");

            // 응답 도중에 닫힌 대화는 작업 중으로 남지 않는다.
            write_lines(path, { step(4, "USER_EXPLICIT", "USER_INPUT", "DONE", { { "content", "<USER_REQUEST>\n다음\n</USER_REQUEST>" } }) }, true);
            require(wait(conversation, [](const auto& session) { return session.status == session_status::running; }), "Next turn not collected.");
            held.reset();
            require(wait(conversation, [](const auto& session) { return !session.runtime_loaded && session.status == session_status::ended; }), "Closed conversation left working.");
            fresh_held.reset();
        }
    } // namespace
} // namespace session_board

int main()
{
    const auto directory { std::filesystem::temp_directory_path() / (L"session-board-antigravity-test-" + std::to_wstring(GetCurrentProcessId())) };
    try
    {
        std::filesystem::remove_all(directory);
        session_board::transcript();
        session_board::annotations_and_roots();
        session_board::merging();
        session_board::discovery(directory);
        std::filesystem::remove_all(directory);
        std::cout << session_board::assertions << " Antigravity assertions passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::error_code ignored {};
        std::filesystem::remove_all(directory, ignored);
        std::fputs(error.what(), stderr);
        std::fputs("\n", stderr);
        return 1;
    }
}
