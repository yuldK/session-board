#include "core/claude_desktop.h"
#include "core/claude_hooks.h"
#include "core/claude_monitor.h"
#include "core/store.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

        const std::string session_id { "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee" };

        nlohmann::json hook(const std::string& name, nlohmann::json extra = nlohmann::json::object())
        {
            nlohmann::json input { { "session_id", session_id }, { "cwd", "E:/fixture" }, { "hook_event_name", name }, { "prompt_id", "prompt-1" } };
            input["transcript_path"] = "C:/fixture/transcript.jsonl";
            input.update(extra);
            return input;
        }

        std::uint64_t own_process_start()
        {
            FILETIME created {};
            FILETIME exited {};
            FILETIME kernel {};
            FILETIME user {};
            if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
                throw std::runtime_error { "Cannot read process time." };
            return ULARGE_INTEGER { created.dwLowDateTime, created.dwHighDateTime }.QuadPart;
        }

        std::string iso_now()
        {
            SYSTEMTIME time {};
            GetSystemTime(&time);
            std::array<char, 32> text {};
            std::snprintf(text.data(), text.size(), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
            return text.data();
        }

        nlohmann::json line(const std::string& type, nlohmann::json extra = nlohmann::json::object())
        {
            nlohmann::json record { { "type", type }, { "sessionId", session_id }, { "cwd", "E:/fixture" }, { "timestamp", iso_now() }, { "isSidechain", false } };
            record.update(extra);
            return record;
        }

        nlohmann::json prompt(const std::string& id, const std::string& text)
        {
            return line("user", { { "promptId", id }, { "origin", { { "kind", "human" } } }, { "message", { { "role", "user" }, { "content", text } } } });
        }

        nlohmann::json assistant(nlohmann::json content, const std::string& stop_reason = "tool_use")
        {
            return line("assistant", { { "message", { { "role", "assistant" }, { "model", "claude-fixture" }, { "content", std::move(content) }, { "stop_reason", stop_reason } } } });
        }

        nlohmann::json tool_result(const std::string& id)
        {
            return line("user", { { "promptId", "prompt-1" }, { "message", { { "role", "user" }, { "content", { { { "type", "tool_result" }, { "tool_use_id", id } } } } } } });
        }

        void hook_conversion(const std::filesystem::path& directory)
        {
            const auto started { claude_event(hook("SessionStart", { { "source", "resume" }, { "model", "claude-fixture" } })) };
            require(started.has_value() && started->provider == "claude" && started->event == "session_started" && started->source == "resume", "SessionStart not converted.");
            const auto submitted { claude_event(hook("UserPromptSubmit", { { "prompt", "요청 원문" } })) };
            require(submitted->event == "turn_started" && submitted->turn_id == "prompt-1" && submitted->title == "요청 원문", "Prompt turn ID not preserved.");
            const auto tool { claude_event(hook("PreToolUse", { { "tool_name", "Bash" }, { "tool_use_id", "toolu_1" }, { "tool_input", { { "command", "secret --token x" } } } })) };
            require(tool->event == "tool_started" && tool->tool_id == "toolu_1" && tool->summary.empty() && tool->title.empty(), "Tool input leaked or ID lost.");
            require(claude_event(hook("PreToolUse", { { "tool_name", "AskUserQuestion" }, { "tool_use_id", "toolu_q" } }))->event == "input_requested", "Question not input wait.");
            require(claude_event(hook("PostToolUseFailure", { { "tool_name", "Bash" }, { "tool_use_id", "toolu_1" }, { "error", "x" } }))->event == "tool_finished",
                "Tool failure treated as session failure.");
            require(claude_event(hook("PermissionRequest", { { "tool_name", "Bash" } }))->event == "approval_requested", "Permission not approval.");
            require(!claude_event(hook("Notification", { { "notification_type", "idle_prompt" } })).has_value(), "idle_prompt treated as wait.");
            require(!claude_event(hook("Notification", { { "notification_type", "permission_prompt" } })).has_value(), "Duplicate permission notification used.");
            require(claude_event(hook("Notification", { { "notification_type", "elicitation_dialog" } }))->event == "input_requested", "Elicitation not input wait.");
            require(!claude_event(hook("PreToolUse", { { "tool_name", "Read" }, { "agent_id", "agent-1" } })).has_value(), "Subagent hook merged into parent.");
            require(!claude_event(hook("MessageDisplay", { { "delta", "x" } })).has_value(), "Display hook unexpectedly handled.");
            require(!claude_event(hook("SubagentStop")).has_value(), "Subagent stop changed parent.");
            const auto stopped { claude_event(hook("Stop", { { "last_assistant_message", "\n구현을 마쳤습니다.\n자세한 내용은 아래와 같습니다." } })) };
            require(stopped->event == "turn_finished" && stopped->summary == "구현을 마쳤습니다.", "Stop summary not reduced to first line.");
            require(claude_event(hook("StopFailure"))->event == "failed", "API stop failure not visible.");
            require(claude_event(hook("SessionEnd", { { "reason", "other" } }))->event == "session_ended", "SessionEnd not converted.");

            session_snapshot snapshot {};
            require(apply_event(snapshot, *submitted), "Turn rejected.");
            require(apply_event(snapshot, *tool), "Tool rejected.");
            require(apply_event(snapshot, *stopped) && snapshot.status == session_status::idle && snapshot.summary == "구현을 마쳤습니다.", "Stop not applied.");
            require(!apply_event(snapshot, *claude_event(hook("PostToolUse", { { "tool_name", "Bash" }, { "tool_use_id", "toolu_1" } }))), "Late tool event reopened turn.");
            auto next { *claude_event(hook("UserPromptSubmit", { { "prompt", "다음 요청" }, { "prompt_id", "prompt-2" } })) };
            require(apply_event(snapshot, next) && snapshot.summary.empty(), "New turn kept previous summary.");
            require(!apply_event(snapshot, *stopped), "Previous Stop closed new turn.");

            save_event(directory, *submitted);
            auto codex { *submitted };
            codex.provider = "codex";
            save_event(directory, codex);
            const auto loaded { load_sessions(directory) };
            require(loaded.sessions.size() == 2 && loaded.invalid_files == 0, "Same ID across providers was merged.");
        }

        void transcript()
        {
            claude_transcript reader {};
            reader.consume(line("custom-title", { { "customTitle", "검증 대화" } }));
            reader.consume(prompt("prompt-1", "요청 원문은 본문에 쓰지 않는다"));
            const auto& view { reader.snapshot() };
            require(view.session_id == session_id && view.turn_id == "prompt-1" && view.status == session_status::running && view.name == "검증 대화", "Prompt did not start a turn.");
            require(view.summary.empty() && view.activity == "작업 내용 수신 대기", "Prompt text used as summary.");
            reader.consume(assistant({ { { "type", "thinking" }, { "thinking", "비공개 추론" } } }));
            reader.consume(line("api-request-blob", { { "message", { { "content", { { { "type", "text" }, { "text", "요청 사본" } } } } } } }));
            reader.consume(line("assistant", { { "isSidechain", true }, { "message", { { "content", { { { "type", "text" }, { "text", "하위 에이전트 답변" } } } } } } }));
            require(view.summary.empty(), "Private or duplicated text became summary.");
            reader.consume(assistant({ { { "type", "text" }, { "text", "설정 파일을 확인하겠습니다.\n세부 내용" } } }));
            require(view.summary == "설정 파일을 확인하겠습니다.", "Public progress not shown.");
            reader.consume(assistant({ { { "type", "tool_use" }, { "id", "toolu_1" }, { "name", "Read" } } }));
            reader.consume(assistant({ { { "type", "tool_use" }, { "id", "toolu_2" }, { "name", "Grep" } } }));
            require(view.active_tools.size() == 2 && view.activity == "파일 검색 중 · Grep", "Parallel tools lost.");
            reader.consume(tool_result("toolu_1"));
            require(view.active_tools.size() == 1 && view.status == session_status::running, "Parallel completion lost.");
            reader.consume(tool_result("toolu_2"));
            require(view.active_tools.empty() && view.activity == "분석 또는 응답 작성 중", "Tool completion not shown.");
            reader.consume(line("user", { { "isCompactSummary", true }, { "message", { { "role", "user" }, { "content", "요약" } } } }));
            reader.consume(line("user", { { "isMeta", true }, { "message", { { "role", "user" }, { "content", "메타" } } } }));
            require(view.turn_id == "prompt-1" && view.summary == "설정 파일을 확인하겠습니다.", "Compact or meta input started a turn.");
            reader.consume(line("assistant", { { "sessionId", "other-session" }, { "message", { { "content", { { { "type", "text" }, { "text", "다른 세션" } } } } } } }));
            require(view.summary == "설정 파일을 확인하겠습니다.", "Copied record from another session applied.");
            reader.consume(assistant({ { { "type", "text" }, { "text", "완료했습니다." } } }, "end_turn"));
            require(view.status == session_status::idle && view.turn_closed && view.summary == "완료했습니다.", "End of turn not shown.");
            reader.consume(prompt("prompt-2", "다음"));
            require(view.turn_id == "prompt-2" && view.summary.empty() && !view.turn_closed, "New turn not reset.");
            reader.consume(assistant({ { { "type", "tool_use" }, { "id", "toolu_3" }, { "name", "Bash" } } }));
            reader.consume(line("user", { { "message", { { "role", "user" }, { "content", { { { "type", "text" }, { "text", "[Request interrupted by user for tool use]" } } } } } } }));
            require(view.status == session_status::interrupted && view.active_tools.empty(), "Interruption not shown.");
        }

        nlohmann::json notification(const std::string& id, const std::string& status)
        {
            return nlohmann::json(
                "<task-notification>\n<task-id>" + id + "</task-id>\n<tool-use-id>toolu_x</tool-use-id>\n<status>" + status + "</status>\n<summary>요약</summary>\n</task-notification>");
        }

        void background_work()
        {
            claude_transcript reader {};
            reader.consume(prompt("prompt-1", "빌드해 줘"));
            const nlohmann::json build_input { { "command", "cmake --build" }, { "description", "Release 빌드" }, { "run_in_background", true } };
            reader.consume(assistant({ { { "type", "tool_use" }, { "id", "toolu_bg" }, { "name", "Bash" }, { "input", build_input } } }));
            reader.consume(assistant({ { { "type", "tool_use" }, { "id", "toolu_fg" }, { "name", "Bash" }, { "input", { { "command", "ls" }, { "description", "목록" } } } } }));
            auto started { tool_result("toolu_bg") };
            started["toolUseResult"] = { { "interrupted", false }, { "backgroundTaskId", "bg1" } };
            reader.consume(started);
            auto plain { tool_result("toolu_fg") };
            plain["toolUseResult"] = { { "stdout", "x" } };
            reader.consume(plain);
            const auto& view { reader.snapshot() };
            require(view.children.size() == 1 && view.children[0].child_id == "bg1" && view.children[0].title == "Release 빌드" && view.children[0].background
                    && view.children[0].status == session_status::running && view.children[0].parent_tool_id == "toolu_bg",
                "Background command not collected.");
            reader.consume(assistant({ { { "type", "tool_use" }, { "id", "toolu_bg2" }, { "name", "PowerShell" }, { "input", { { "description", "5분 테스트" }, { "run_in_background", true } } } } }));
            auto second { tool_result("toolu_bg2") };
            second["toolUseResult"] = { { "backgroundTaskId", "bg2" } };
            reader.consume(second);
            reader.consume(assistant({ { { "type", "tool_use" }, { "id", "toolu_stop" }, { "name", "TaskStop" }, { "input", { { "task_id", "bg2" } } } } }));
            reader.consume(tool_result("toolu_stop"));
            require(view.children.size() == 2 && view.children[1].status == session_status::interrupted, "Stopped background command kept running.");
            reader.consume(assistant({ { { "type", "text" }, { "text", "빌드를 백그라운드로 돌려 두었습니다." } } }, "end_turn"));
            require(view.status == session_status::idle && view.children[0].status == session_status::running, "Turn end stopped background command.");

            // 알림은 대기열·첨부·사용자 기록 어디로 와도 같게 적용하고, 새 turn으로 보지 않는다.
            reader.consume(line("queue-operation", { { "operation", "enqueue" }, { "content", notification("bg1", "failed") } }));
            require(view.children[0].status == session_status::failed && reader.task_outcomes().at("bg1").status == session_status::failed, "Queued notification not applied.");
            reader.consume(line("attachment", { { "attachment", { { "type", "queued_command" }, { "prompt", notification("agent-a", "completed") } } } }));
            require(reader.task_outcomes().contains("agent-a"), "Attachment notification not applied.");
            reader.consume(line("user", { { "message", { { "role", "user" }, { "content", notification("agent-b", "stopped") } } } }));
            require(view.turn_id == "prompt-1" && view.summary == "빌드를 백그라운드로 돌려 두었습니다." && view.status == session_status::idle, "Notification started a new turn.");
            require(reader.task_outcomes().at("agent-b").status == session_status::interrupted, "Stopped notification not mapped.");
            const std::string orphan {
                "<task-notification>\n<task-id>x1</task-id>\n<task-id>x2</task-id>\n<task-id>__orphan_summary__:shell</task-id>\n<status>stopped</status>\n</task-notification>"
            };
            reader.consume(line("queue-operation", { { "operation", "enqueue" }, { "content", orphan } }));
            require(reader.task_outcomes().contains("x2") && !reader.task_outcomes().contains("__orphan_summary__:shell"), "Orphan summary IDs not closed.");

            claude_transcript side { true };
            side.consume(line("user", { { "isSidechain", true }, { "agentId", "agent-a" }, { "message", { { "role", "user" }, { "content", "하위 지시문" } } } }));
            side.consume(line("assistant", { { "isSidechain", true }, { "message", { { "content", { { { "type", "text" }, { "text", "구조를 살펴보는 중입니다." } } } } } } }));
            side.consume(line("assistant", { { "isSidechain", true }, { "message", { { "content", { { { "type", "tool_use" }, { "id", "toolu_s" }, { "name", "Grep" } } } } } } }));
            side.consume(assistant({ { { "type", "text" }, { "text", "부모 기록" } } }));
            require(side.snapshot().summary == "구조를 살펴보는 중입니다." && side.snapshot().tool_name == "Grep" && side.started_ms() > 0, "Sidechain transcript not read.");

            nlohmann::json meta_input { { "agentType", "Explore" }, { "description", "신호 조사" }, { "toolUseId", "toolu_a" }, { "spawnDepth", 2 }, { "requestShape", "background" } };
            meta_input["prompt"] = "읽지 않는 지시문";
            const auto meta { parse_claude_agent_meta(meta_input) };
            require(meta.has_value() && meta->background && !meta->finished && meta->depth == 2 && meta->tool_use_id == "toolu_a", "Agent meta not parsed.");
            auto child { make_claude_agent_child("agent-a", *meta, &side, 1000, {}) };
            require(child.status == session_status::running && child.activity == "파일 검색 중 · Grep" && child.title == "신호 조사" && child.background, "Running agent child wrong.");
            auto finished { *meta };
            finished.finished = true;
            require(make_claude_agent_child("agent-a", finished, nullptr, 1000, {}).status == session_status::idle, "Finished agent not completed.");
            const auto done { make_claude_agent_child("agent-a", finished, &side, 1000, {}) };
            require(done.activity == "구조를 살펴보는 중입니다." && done.tool_name.empty(), "Finished agent kept running activity.");
            require(make_claude_agent_child("agent-b", finished, nullptr, 1000, reader.task_outcomes()).status == session_status::interrupted, "Stopped agent not shown.");
            const std::map<std::string, claude_task_outcome> stale { { "agent-a", { session_status::idle, 1 } } };
            require(make_claude_agent_child("agent-a", *meta, &side, 1000, stale).status == session_status::running, "Notification from before resume closed running agent.");
        }

        void runtime()
        {
            nlohmann::json base { { "pid", GetCurrentProcessId() }, { "sessionId", session_id }, { "cwd", "E:/fixture" }, { "status", "busy" }, { "name", "등록 이름" } };
            base["procStart"] = std::to_string(own_process_start());
            base["updatedAt"] = 1000;
            base["statusUpdatedAt"] = 2000;
            const auto parsed { parse_claude_runtime(base) };
            require(parsed.has_value() && parsed->updated_ms == 2000 && claude_process_alive(*parsed), "Own process not verified.");
            auto reused { base };
            reused["procStart"] = "1";
            require(!claude_process_alive(*parse_claude_runtime(reused)), "Reused PID accepted.");
            auto remote { base };
            remote["pidDomain"] = "wsl:not-this-host";
            require(!claude_process_alive(*parse_claude_runtime(remote)), "Other host accepted.");
            auto missing { base };
            missing.erase("procStart");
            require(!claude_process_alive(*parse_claude_runtime(missing)), "Unverifiable registry accepted.");
            require(!parse_claude_runtime({ { "pid", 1 } }).has_value(), "Registry without session ID accepted.");

            session_snapshot snapshot {};
            apply_claude_runtime(snapshot, *parsed);
            require(snapshot.runtime_loaded && snapshot.status == session_status::running && snapshot.name == "등록 이름", "Busy registry not applied.");
            auto waiting { *parsed };
            waiting.status = "waiting";
            waiting.waiting_for = "input needed";
            apply_claude_runtime(snapshot, waiting);
            require(snapshot.status == session_status::waiting_input, "Input wait not shown.");
            waiting.waiting_for = "approve Bash";
            apply_claude_runtime(snapshot, waiting);
            require(snapshot.status == session_status::waiting_approval, "Approval wait not shown.");
            auto resumed { *parsed };
            resumed.updated_ms = snapshot.updated_ms + 1;
            apply_claude_runtime(snapshot, resumed);
            require(snapshot.status == session_status::running && !snapshot.turn_closed, "New busy registry did not clear resolved approval wait.");
            auto stale_idle { resumed };
            stale_idle.status = "idle";
            --stale_idle.updated_ms;
            apply_claude_runtime(snapshot, stale_idle);
            require(snapshot.status == session_status::running && !snapshot.turn_closed, "Stale idle registry overwrote active transcript state.");
            auto idle { *parsed };
            idle.status = "idle";
            idle.updated_ms = resumed.updated_ms + 1;
            apply_claude_runtime(snapshot, idle);
            require(snapshot.status == session_status::idle && snapshot.turn_closed && snapshot.runtime_loaded, "Idle open session not kept.");
        }

        void merging()
        {
            session_snapshot local {};
            local.provider = "claude";
            local.session_id = session_id;
            local.turn_id = "prompt-1";
            local.status = session_status::running;
            local.summary = "로컬 진행";
            local.updated_ms = 1000;
            local.runtime_loaded = true;
            auto hooked { local };
            hooked.status = session_status::waiting_approval;
            hooked.last_event = "approval_requested";
            hooked.title = "요청 발췌";
            hooked.updated_ms = 2000;
            auto codex { hooked };
            codex.provider = "codex";
            std::vector<session_snapshot> sessions { codex, hooked };
            merge_claude_sessions(sessions, { local });
            require(sessions.size() == 2, "Providers merged.");
            const auto& merged { sessions[0].provider == "claude" ? sessions[0] : sessions[1] };
            require(merged.status == session_status::waiting_approval && merged.runtime_loaded && merged.summary == "로컬 진행" && merged.title == "요청 발췌", "Newer hook wait lost.");

            auto observed { local };
            observed.activity = "파일 읽는 중 · Read";
            auto same_state { local };
            same_state.last_event = "tool_started";
            same_state.updated_ms = 1005;
            std::vector<session_snapshot> agreeing { same_state };
            merge_claude_sessions(agreeing, { observed });
            require(agreeing.front().activity == "파일 읽는 중 · Read", "Slightly newer hook erased observed activity.");

            auto old_turn { hooked };
            old_turn.turn_id = "prompt-0";
            old_turn.last_event = "turn_finished";
            old_turn.status = session_status::idle;
            std::vector<session_snapshot> stale { old_turn };
            merge_claude_sessions(stale, { local });
            require(stale.front().status == session_status::running && stale.front().turn_id == "prompt-1", "Late hook from previous turn overwrote local state.");
        }

        void discovery(const std::filesystem::path& directory)
        {
            const auto home { directory / "claude-home" };
            const auto project { home / "projects" / "E--fixture" };
            std::filesystem::create_directories(home / "sessions");
            std::filesystem::create_directories(project / session_id / "subagents");
            const auto registry { home / "sessions" / (std::to_string(GetCurrentProcessId()) + ".json") };
            const auto write_registry = [&](const std::string& status) {
                nlohmann::json value { { "pid", GetCurrentProcessId() }, { "sessionId", session_id }, { "cwd", "E:\\fixture" }, { "status", status }, { "name", "열린 대화" } };
                value["procStart"] = std::to_string(own_process_start());
                value["updatedAt"] = now_ms();
                value["statusUpdatedAt"] = now_ms();
                std::ofstream { registry, std::ios::binary } << value.dump();
            };
            write_registry("busy");
            std::ofstream { home / "sessions" / (std::to_string(GetCurrentProcessId()) + ".secret.key"), std::ios::binary } << "not json";
            const nlohmann::json pending { { "pid", GetCurrentProcessId() }, { "sessionId", "registry-only" }, { "procStart", std::to_string(own_process_start()) }, { "status", "idle" } };
            std::ofstream { home / "sessions" / "1.json", std::ios::binary } << pending.dump();
            const auto path { project / (session_id + ".jsonl") };
            {
                std::ofstream file { path, std::ios::binary };
                file << prompt("prompt-1", "요청").dump() << '\n';
                file << assistant({ { { "type", "text" }, { "text", "기존 세션을 확인 중입니다." } } }).dump() << '\n';
            }
            std::ofstream { project / session_id / "subagents" / "agent-1.jsonl", std::ios::binary } << assistant({ { { "type", "text" }, { "text", "하위 에이전트" } } }).dump() << '\n';

            claude_monitor monitor { home };
            const auto wait = [&](const auto& predicate) {
                const auto deadline { std::chrono::steady_clock::now() + std::chrono::seconds { 6 } };
                while (std::chrono::steady_clock::now() < deadline)
                {
                    const auto view { monitor.snapshot() };
                    for (const auto& session : view.sessions)
                        if (session.session_id == session_id && predicate(session))
                            return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
                }
                return false;
            };
            require(wait([](const auto& session) {
                return session.runtime_loaded && session.summary == "기존 세션을 확인 중입니다." && session.name == "열린 대화" && session.status == session_status::running;
            }),
                "Existing session not automatically collected.");
            const auto view { monitor.snapshot() };
            require(view.ready && view.error.empty(), "Observation not ready.");
            require(std::ranges::any_of(view.sessions, [](const auto& session) { return session.session_id == "registry-only" && session.runtime_loaded; }), "Open session without transcript hidden.");
            require(std::ranges::none_of(view.sessions, [](const auto& session) { return session.summary == "하위 에이전트"; }), "Subagent transcript collected.");
            // 하위 기록은 부모 카드가 아닌 하위 행이 된다. 메타가 생기면 나타나고 끝 표시로 완료된다.
            const auto meta { project / session_id / "subagents" / "agent-1.meta.json" };
            std::ofstream { meta, std::ios::binary } << nlohmann::json { { "agentType", "Explore" }, { "description", "하위 조사" }, { "toolUseId", "toolu_9" } }.dump();
            require(wait([](const auto& session) {
                return session.children.size() == 1 && session.children[0].title == "하위 조사" && session.children[0].status == session_status::running
                    && session.summary == "기존 세션을 확인 중입니다.";
            }),
                "Subagent not collected as child.");
            std::ofstream { meta, std::ios::binary } << nlohmann::json { { "agentType", "Explore" }, { "description", "하위 조사" }, { "toolUseId", "toolu_9" }, { "finished", true } }.dump();
            require(wait([](const auto& session) { return session.children.size() == 1 && session.children[0].status == session_status::idle; }), "Finished subagent not completed.");

            std::ofstream { path, std::ios::binary | std::ios::app } << assistant({ { { "type", "text" }, { "text", "진행 메시지가 바뀌었습니다." } } }).dump();
            std::this_thread::sleep_for(std::chrono::milliseconds { 1100 });
            require(wait([](const auto& session) { return session.summary == "기존 세션을 확인 중입니다."; }), "Partial JSON line applied prematurely.");
            std::ofstream { path, std::ios::binary | std::ios::app } << '\n';
            require(wait([](const auto& session) { return session.summary == "진행 메시지가 바뀌었습니다."; }), "Appended progress not collected.");
            const auto replacement { project / "replacement.tmp" };
            const std::string replacement_text { prompt("prompt-2", "새 요청").dump() + '\n' + assistant({ { { "type", "text" }, { "text", "교체된 기록을 확인 중입니다." } } }).dump() };
            // 이전보다 큰 파일로 바꿔 크기 감소만 감지하는 수집기의 누락도 확인한다.
            std::ofstream { replacement, std::ios::binary } << replacement_text << std::string(static_cast<std::size_t>(std::filesystem::file_size(path)), ' ') << '\n';
            require(MoveFileExW(replacement.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE, "Transcript fixture replacement failed.");
            require(wait([](const auto& session) { return session.summary == "교체된 기록을 확인 중입니다."; }), "Replaced transcript not reloaded.");
            // 끝 표시를 남기지 못한 포그라운드 하위는 부모 응답이 끝나면 확인 불가로 닫힌다.
            std::ofstream { project / session_id / "subagents" / "agent-2.meta.json", std::ios::binary } << nlohmann::json { { "agentType", "Plan" }, { "description", "끝 표시 없음" } }.dump();
            const auto foreground = [](const auto& session, const session_status status) {
                return std::ranges::any_of(session.children, [status](const auto& child) { return child.child_id == "2" && child.status == status; });
            };
            require(wait([&](const auto& session) { return foreground(session, session_status::running); }), "Second subagent not collected.");
            write_registry("idle");
            require(wait([](const auto& session) { return session.status == session_status::idle && session.runtime_loaded; }), "Response completion removed open session.");
            require(wait([&](const auto& session) { return foreground(session, session_status::ended); }), "Foreground subagent left running after turn end.");
            std::filesystem::remove(registry);
            require(wait([](const auto& session) { return !session.runtime_loaded && session.summary == "교체된 기록을 확인 중입니다."; }), "Closed session not kept as recent.");
        }

        std::string file_text(const std::filesystem::path& path)
        {
            std::ifstream input { path, std::ios::binary };
            return { std::istreambuf_iterator<char> { input }, std::istreambuf_iterator<char> {} };
        }

        std::size_t backups(const std::filesystem::path& directory)
        {
            return static_cast<std::size_t>(std::ranges::count_if(std::filesystem::directory_iterator { directory }, [](const auto& entry) { return entry.path().extension() == L".bak"; }));
        }

        void hook_registration(const std::filesystem::path& directory)
        {
            const auto home { directory / "registration" / "claude" };
            const auto install { directory / "registration" / "SessionBoard" / "bin" };
            const auto source { directory / "registration" / "build" / "session-board.exe" };
            const auto settings { home / "settings.json" };
            std::filesystem::create_directories(home);
            std::filesystem::create_directories(source.parent_path());
            std::ofstream { source, std::ios::binary } << "helper-v1";
            const std::string original {
                "{\n  \"permissions\": {\n    \"deny\": [\n      \"Bash(git push)\"\n    ]\n  },\n  \"hooks\": {\n    \"PreToolUse\": [\n      {\n        \"matcher\": \"Bash\",\n"
                "        \"hooks\": [\n          {\n            \"type\": \"command\",\n            \"command\": \"python guard.py\"\n          }\n        ]\n      }\n    ]\n  },\n"
                "  \"알 수 없는 키\": {\n    \"값\": 1\n  }\n}\n"
            };
            std::ofstream { settings, std::ios::binary } << original;

            auto result { ensure_claude_hooks(settings, source, install) };
            require(result.changed && result.helper_updated && result.error.empty() && std::filesystem::exists(result.backup), "Hooks not registered on first start.");
            require(file_text(install / "session-board.exe") == "helper-v1", "Helper not installed.");
            require(file_text(result.backup) == original, "Backup differs from original settings.");
            auto document { nlohmann::ordered_json::parse(file_text(settings)) };
            require(document.begin().key() == "permissions" && document.contains("알 수 없는 키") && document["permissions"]["deny"].size() == 1, "Existing settings not preserved.");
            require(document["hooks"]["PreToolUse"].size() == 2 && document["hooks"]["PreToolUse"][0]["hooks"][0]["command"] == "python guard.py", "Existing hook lost.");
            const auto command { claude_hook_command(install / "session-board.exe") };
            require(command.ends_with("session-board.exe --hook --provider claude"), "Hook command does not call the app in hook mode.");
            // 예전 helper 명령은 우리 항목으로 보고 새 명령으로 바꿔 끼운다.
            auto legacy { nlohmann::ordered_json::parse(R"({"hooks":{"Stop":[{"hooks":[{"type":"command","command":"C:/old/session-board-hook.exe --provider claude"}]}]}})") };
            require(update_claude_hook_settings(legacy, command) && legacy["hooks"]["Stop"].size() == 1 && legacy["hooks"]["Stop"][0]["hooks"][0]["command"] == command, "Legacy helper not replaced.");
            for (const char* event :
                { "SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "PostToolUseFailure", "PermissionRequest", "Notification", "Stop", "StopFailure", "SessionEnd" })
                require(document["hooks"].contains(event) && document["hooks"][event].back()["hooks"][0]["command"] == command, "Hook event missing.");
            require(document["hooks"]["Notification"].back()["matcher"] == "elicitation_dialog|elicitation_url_dialog", "Notification matcher missing.");
            require(!document["hooks"].contains("MessageDisplay") && !document["hooks"].contains("SubagentStop"), "Unwanted hook registered.");
            require(file_text(settings).find("\r\n") == std::string::npos, "Line endings changed.");

            const auto registered { file_text(settings) };
            result = ensure_claude_hooks(settings, source, install);
            require(!result.changed && !result.helper_updated && result.error.empty() && file_text(settings) == registered && backups(home) == 1, "Repeated start rewrote settings.");

            std::ofstream { source, std::ios::binary } << "helper-v2";
            result = ensure_claude_hooks(settings, source, install);
            require(result.helper_updated && !result.changed && file_text(install / "session-board.exe") == "helper-v2", "Updated helper not installed.");

            document = nlohmann::ordered_json::parse(registered);
            document["hooks"]["Stop"].push_back(document["hooks"]["Stop"].back());
            require(update_claude_hook_settings(document, command) && document["hooks"]["Stop"].size() == 1, "Duplicate registration not collapsed.");
            require(update_claude_hook_settings(document, command, true) && document == nlohmann::ordered_json::parse(original), "Removal did not restore settings.");
            require(!update_claude_hook_settings(document, command, true), "Repeated removal changed settings.");

            std::ofstream { claude_hook_opt_out_file(install), std::ios::binary } << "";
            std::ofstream { settings, std::ios::binary } << original;
            result = ensure_claude_hooks(settings, source, install);
            require(result.disabled && !result.changed && file_text(settings) == original, "Opt-out ignored.");
            std::filesystem::remove(claude_hook_opt_out_file(install));

            const std::string broken { "{ \"hooks\": [ // 주석\n" };
            std::ofstream { settings, std::ios::binary } << broken;
            result = ensure_claude_hooks(settings, source, install);
            require(!result.changed && !result.error.empty() && file_text(settings) == broken, "Invalid settings overwritten.");

            const auto fresh { directory / "registration" / "fresh" };
            result = ensure_claude_hooks(fresh / "settings.json", fresh / "missing.exe", fresh / "bin");
            require(!result.changed && !result.error.empty() && !std::filesystem::exists(fresh / "settings.json"), "Hooks registered without a helper.");
            std::filesystem::create_directories(fresh);
            std::ofstream { fresh / "settings.json", std::ios::binary } << "{\r\n  \"model\": \"x\"\r\n}\r\n";
            result = ensure_claude_hooks(fresh / "settings.json", source, fresh / "bin");
            const auto crlf { file_text(fresh / "settings.json") };
            require(result.changed && crlf.find("\r\n") != std::string::npos && crlf.find("\r\r") == std::string::npos && crlf.find('\n') == crlf.find("\r\n") + 1, "CRLF settings not preserved.");
        }

        void desktop_index(const std::filesystem::path& directory)
        {
            const auto root { directory / "desktop" / "claude-code-sessions" };
            const auto folder { root / "account" / "organization" };
            std::filesystem::create_directories(folder);
            const auto write = [&](const std::string& name, const nlohmann::json& value) { std::ofstream { folder / name, std::ios::binary } << value.dump(); };
            write("local_11111111-2222-3333-4444-555555555555.json", { { "sessionId", "local_11111111-2222-3333-4444-555555555555" }, { "cliSessionId", session_id }, { "isArchived", false } });
            write("local_archived.json", { { "sessionId", "local_archived" }, { "cliSessionId", "archived-cli" }, { "isArchived", true } });
            write("local_bad.json", { { "sessionId", "local_bad id" }, { "cliSessionId", "bad-cli" } });
            std::ofstream { folder / "local_broken.json", std::ios::binary } << "{ \"sessionId\": ";
            // 정해진 깊이 밖의 기록은 읽지 않는다.
            std::ofstream { root / "local_shallow.json", std::ios::binary } << nlohmann::json { { "sessionId", "local_shallow" }, { "cliSessionId", "shallow-cli" } }.dump();

            claude_desktop_index index { { root, directory / "missing" } };
            index.refresh();
            require(index.find(session_id) == "local_11111111-2222-3333-4444-555555555555", "Desktop session not mapped.");
            require(index.find("archived-cli").empty(), "Archived desktop session mapped.");
            require(index.find("bad-cli").empty(), "Malformed desktop ID mapped.");
            require(index.find("shallow-cli").empty() && index.find("").empty(), "Unexpected desktop mapping.");
            require(claude_desktop_link("local_11111111-2222-3333-4444-555555555555") == L"claude://code/continue?session=local_11111111-2222-3333-4444-555555555555", "Desktop link differs.");
            require(claude_desktop_link("local_bad id").empty() && claude_desktop_link(session_id).empty(), "Invalid desktop link built.");

            std::filesystem::remove(folder / "local_11111111-2222-3333-4444-555555555555.json");
            index.refresh();
            require(index.find(session_id).empty(), "Removed desktop session still mapped.");
        }
    } // namespace
} // namespace session_board

int main()
{
    const auto directory { std::filesystem::temp_directory_path() / (L"session-board-claude-test-" + std::to_wstring(GetCurrentProcessId())) };
    try
    {
        std::filesystem::remove_all(directory);
        session_board::hook_conversion(directory / "store");
        session_board::transcript();
        session_board::background_work();
        session_board::runtime();
        session_board::merging();
        session_board::discovery(directory);
        session_board::hook_registration(directory);
        session_board::desktop_index(directory);
        std::filesystem::remove_all(directory);
        std::cout << session_board::assertions << " Claude assertions passed.\n";
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
