#include "core/codex_monitor.h"
#include "core/store.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace session_board {
    namespace {
        int assertions { 0 };
        void require(const bool condition, const char* message)
        {
            ++assertions;
            if (!condition)
                throw std::runtime_error { message };
        }

        session_event event(const std::string& kind, const std::string& provider = "codex")
        {
            return parse_event({ { "schema_version", 1 }, { "provider", provider }, { "session_id", "test-session" }, { "event", kind }, { "timestamp_ms", now_ms() }, { "turn_id", "turn-1" } });
        }

        void lifecycle()
        {
            session_snapshot snapshot {};
            require(apply_event(snapshot, event("turn_started")), "Start rejected.");
            require(snapshot.status == session_status::running, "Turn must run.");
            auto tool { event("tool_started") };
            tool.tool_id = "tool-1";
            tool.tool_name = "exec_command";
            require(apply_event(snapshot, tool), "Tool rejected.");
            tool.tool_id = "tool-2";
            require(apply_event(snapshot, tool), "Parallel tool rejected.");
            require(snapshot.active_tools.size() == 2, "Parallel tools lost.");
            require(apply_event(snapshot, event("approval_requested")), "Approval rejected.");
            require(snapshot.status == session_status::waiting_approval, "Approval not visible.");
            tool.event = "tool_finished";
            require(apply_event(snapshot, tool), "Tool result rejected.");
            require(snapshot.active_tools.size() == 1 && snapshot.status == session_status::running, "Parallel completion lost.");
            require(apply_event(snapshot, event("turn_finished")), "Stop rejected.");
            require(snapshot.turn_closed && snapshot.status == session_status::idle, "Stop did not close turn.");
            require(!apply_event(snapshot, tool), "Late result reactivated finished turn.");
            auto next { event("turn_started") };
            next.turn_id = "turn-2";
            require(apply_event(snapshot, next), "Second turn rejected.");
            require(!apply_event(snapshot, tool), "Previous turn mutated new turn.");
            require(!apply_event(snapshot, event("turn_finished")), "Old stop closed new turn.");
            require(apply_event(snapshot, event("session_ended")), "End rejected.");
            require(!apply_event(snapshot, event("tool_started")), "Ended session reactivated.");
            require(apply_event(snapshot, event("session_started")), "Resume rejected.");
            require(snapshot.status == session_status::idle && !snapshot.turn_closed, "Resume failed.");
        }

        void validation()
        {
            const nlohmann::json raw {
                { "hook_event_name", "UserPromptSubmit" },
                { "session_id", "codex-1" },
                { "prompt", "한글 작업\n두 번째 줄" },
                { "cwd", "E:/개발" },
                { "turn_id", "t1" },
            };
            const auto normalized { codex_event(raw) };
            require(normalized.has_value() && normalized->provider == "codex" && normalized->event == "turn_started", "Codex adapter failed.");
            require(normalized->title.find('\n') == std::string::npos, "Multiline title leaked.");
            auto subagent { raw };
            subagent["agent_id"] = "child";
            require(!codex_event(subagent).has_value(), "Subagent mutated parent.");
            require(!codex_event({ { "hook_event_name", "Unknown" } }).has_value(), "Unknown hook accepted.");
            bool rejected { false };
            try
            {
                static_cast<void>(parse_event({ { "schema_version", 1 }, { "provider", "unknown" }, { "session_id", "id" } }));
            }
            catch (...)
            {
                rejected = true;
            }
            require(rejected, "Unknown provider accepted.");
            require(compact_text("한글", 4) == "한", "UTF-8 truncated mid-character.");
            session_snapshot snapshot {};
            auto start { event("turn_started", "claude") };
            start.timestamp_ms -= 700'000;
            require(apply_event(snapshot, start), "Old start rejected.");
            require(needs_attention(snapshot, now_ms()), "Stale running session hidden.");
            auto older { start };
            --older.timestamp_ms;
            require(!apply_event(snapshot, older), "Old event overwrote new state.");
            const auto restored { parse_snapshot(serialize_snapshot(snapshot)) };
            require(restored.provider == "claude" && restored.status == session_status::running, "Snapshot roundtrip failed.");

            // 세션 창은 덤이다. 실린 이벤트만 바꾸고, 없는 이벤트가 기록을 지우지 않으며, 숫자가 아니면 버린다.
            auto hosted { parse_event({ { "schema_version", 1 }, { "provider", "claude" }, { "session_id", "test-session" }, { "event", "tool_started" }, { "timestamp_ms", now_ms() },
                { "turn_id", start.turn_id }, { "tool_name", "Bash" }, { "host_window", 4242 }, { "host_process_id", 77 } }) };
            require(hosted.host_window == 4242 && hosted.host_process_id == 77, "Host window was not parsed.");
            require(apply_event(snapshot, hosted), "Hosted event rejected.");
            auto plain { event("tool_finished", "claude") };
            plain.turn_id = start.turn_id;
            require(apply_event(snapshot, plain) && snapshot.host_window == 4242, "Event without host cleared the window.");
            const auto with_host { parse_snapshot(serialize_snapshot(snapshot)) };
            require(with_host.host_window == 4242 && with_host.host_process_id == 77, "Host window roundtrip failed.");
            const auto text_host { parse_event(
                { { "schema_version", 1 }, { "provider", "codex" }, { "session_id", "id" }, { "event", "turn_started" }, { "timestamp_ms", now_ms() }, { "host_window", "not a number" } }) };
            require(text_host.host_window == 0, "Invalid host window accepted.");
        }

        void storage(const std::filesystem::path& directory)
        {
            save_event(directory, event("turn_started"));
            save_event(directory, event("turn_started", "claude"));
            require(load_sessions(directory).sessions.size() == 2, "Providers with same id collided.");
            constexpr int workers { 8 };
            constexpr int iterations { 8 };
            std::atomic<int> failures { 0 };
            std::vector<std::thread> threads {};
            for (int worker { 0 }; worker < workers; ++worker)
                threads.emplace_back([&] {
                    for (int index { 0 }; index < iterations; ++index)
                        try
                        {
                            save_event(directory, event("tool_finished"));
                        }
                        catch (...)
                        {
                            ++failures;
                        }
                });
            for (auto& thread : threads)
                thread.join();
            require(failures.load() == 0, "Concurrent writer failed.");
            const auto loaded { load_sessions(directory) };
            require(loaded.sessions.size() == 2 && loaded.invalid_files == 0, "Concurrent writes corrupted snapshot.");
            std::ofstream { directory / "broken.json" } << "{ incomplete";
            const auto partial { load_sessions(directory) };
            require(partial.sessions.size() == 2 && partial.invalid_files == 1, "Corrupt file hid valid sessions.");
            auto traversal { event("turn_started") };
            traversal.session_id = "../../outside";
            save_event(directory, traversal);
            require(load_sessions(directory).sessions.size() == 3, "Opaque id was not stored safely.");
        }

        void widget_lifecycle()
        {
            const auto now { now_ms() };
            session_snapshot snapshot {};
            auto first { event("turn_started") };
            first.title = "첫 요청";
            require(apply_event(snapshot, first), "Initial request rejected.");
            auto next { first };
            next.title = "현재 요청";
            next.turn_id = "turn-2";
            require(apply_event(snapshot, next) && snapshot.title == next.title, "Latest request was not shown.");
            auto tool { next };
            tool.event = "tool_started";
            tool.title = "이전 제목";
            require(apply_event(snapshot, tool) && snapshot.title == next.title, "Tool changed current request.");
            require(is_widget_visible(snapshot, now + widget_retention_ms * 2), "Long running session disappeared.");
            auto finish { next };
            finish.event = "turn_finished";
            finish.title.clear();
            require(apply_event(snapshot, finish), "Response end rejected.");
            require(is_widget_visible(snapshot, now + widget_retention_ms - 1), "Finished session disappeared early.");
            require(!is_widget_visible(snapshot, now + widget_retention_ms), "Finished retention did not expire.");
            finish.event = "session_ended";
            require(apply_event(snapshot, finish), "Session end rejected.");
            require(snapshot.title == next.title && is_widget_visible(snapshot, now + 1000), "Session end removed current request.");
            require(!is_widget_visible(snapshot, now + widget_retention_ms), "Ended retention did not expire.");
        }

        void children(const std::filesystem::path& directory)
        {
            const auto base { now_ms() - 60 * 1000 };
            std::int64_t clock { base };
            const auto at = [&](nlohmann::json input) {
                input.update({ { "schema_version", 1 }, { "provider", "claude" }, { "session_id", "parent" }, { "turn_id", "turn-1" }, { "timestamp_ms", ++clock } });
                return parse_event(input);
            };
            session_snapshot snapshot {};
            require(apply_event(snapshot, at({ { "event", "turn_started" }, { "title", "부모 요청" } })), "Parent turn rejected.");
            const auto parent_time { snapshot.updated_ms };
            require(
                apply_event(snapshot,
                    at({ { "event", "child_started" }, { "child_id", "agent-1" }, { "child_type", "agent" }, { "agent_type", "Explore" }, { "title", "신호 조사" }, { "parent_tool_id", "toolu_1" } })),
                "Child start rejected.");
            require(snapshot.children.size() == 1 && snapshot.children[0].status == session_status::running && snapshot.children[0].agent_type == "Explore", "Child not created.");
            require(snapshot.updated_ms == parent_time && snapshot.title == "부모 요청" && snapshot.status == session_status::running, "Child event changed parent.");
            require(latest_update(snapshot) == clock, "Child activity not counted as latest update.");
            require(apply_event(snapshot, at({ { "event", "approval_requested" }, { "child_id", "agent-1" }, { "tool_name", "Bash" } })), "Child approval rejected.");
            require(snapshot.status == session_status::running && children_need_attention(snapshot, clock), "Child approval not raised to parent.");
            require(apply_event(snapshot, at({ { "event", "child_started" }, { "child_id", "build" }, { "child_type", "background_command" }, { "title", "Release 빌드" } })),
                "Background start rejected.");
            require(snapshot.children[1].background && working_children(snapshot) == 2, "Background command not marked.");

            // 부모 turn이 끝나면 포그라운드 하위는 끝을 놓친 것으로 닫고 백그라운드는 남긴다.
            require(apply_event(snapshot, at({ { "event", "turn_finished" } })), "Parent stop rejected.");
            require(snapshot.children[0].status == session_status::ended && snapshot.children[1].status == session_status::running, "Turn end closed wrong children.");
            require(is_widget_visible(snapshot, clock + widget_retention_ms * 2), "Session with running background hidden.");
            require(!child_needs_attention(snapshot.children[1], clock + 60 * 60 * 1000), "Quiet background command marked stale.");
            require(apply_event(snapshot, at({ { "event", "child_finished" }, { "child_id", "agent-1" } })) && snapshot.children[0].status == session_status::idle,
                "Late finish did not replace unknown end.");
            require(!apply_event(snapshot, at({ { "event", "tool_started" }, { "child_id", "agent-1" }, { "tool_name", "Read" } })), "Late tool event revived finished child.");
            require(
                apply_event(snapshot, at({ { "event", "child_finished" }, { "child_id", "build" }, { "child_type", "background_command" }, { "outcome", "failed" } })), "Background failure rejected.");
            require(snapshot.children[1].status == session_status::failed && children_need_attention(snapshot, clock), "Background failure not visible.");
            require(!children_need_attention(snapshot, clock + widget_retention_ms), "Old failure kept parent in attention.");
            auto old { at({ { "event", "child_started" }, { "child_id", "build" }, { "child_type", "background_command" } }) };
            old.timestamp_ms = base;
            require(!apply_event(snapshot, old), "Older child event applied.");

            session_child stale {};
            stale.status = session_status::running;
            stale.updated_ms = clock;
            require(child_needs_attention(stale, clock + 10 * 60 * 1000), "Silent agent not marked stale.");

            const auto restored { parse_snapshot(serialize_snapshot(snapshot)) };
            require(restored.children.size() == 2 && restored.children[1].kind == child_kind::background_command && restored.children[0].title == "신호 조사", "Children not stored.");
            auto legacy { serialize_snapshot(snapshot) };
            legacy.erase("children");
            require(parse_snapshot(legacy).children.empty(), "Snapshot without children rejected.");

            bool rejected { false };
            try
            {
                static_cast<void>(at({ { "event", "child_started" } }));
            }
            catch (const std::exception&)
            {
                rejected = true;
            }
            require(rejected, "Child event without child ID accepted.");
            rejected = false;
            try
            {
                static_cast<void>(at({ { "event", "turn_finished" }, { "child_id", "agent-1" } }));
            }
            catch (const std::exception&)
            {
                rejected = true;
            }
            require(rejected, "Parent-only event accepted for child.");

            for (int index { 0 }; index < 30; ++index)
                require(apply_event(snapshot, at({ { "event", "child_started" }, { "child_id", "many-" + std::to_string(index) } })), "Bulk child rejected.");
            require(snapshot.children.size() == maximum_children, "Child cap not kept.");
            require(std::ranges::none_of(snapshot.children, [](const auto& child) { return child.child_id == "agent-1"; }), "Finished child kept over running ones.");

            std::vector<session_child> local { snapshot.children.front() };
            auto hooked { local.front() };
            hooked.title.clear();
            hooked.status = session_status::waiting_input;
            ++hooked.updated_ms;
            auto extra { hooked };
            extra.child_id = "only-hook";
            merge_children(local, { hooked, extra });
            require(local.size() == 2 && local[0].status == session_status::waiting_input && local[0].title == snapshot.children.front().title, "Child merge lost state or title.");

            require(apply_event(snapshot, at({ { "event", "session_ended" } })) && working_children(snapshot) == 0, "Session end left children running.");

            // Hook helper 경로: 하위 이벤트도 스냅숏 파일에 남는다.
            save_event(directory, at({ { "event", "turn_started" } }));
            save_event(directory, at({ { "event", "child_started" }, { "child_id", "agent-9" }, { "title", "저장 확인" } }));
            const auto loaded { load_sessions(directory) };
            const auto parent { std::ranges::find_if(loaded.sessions, [](const auto& session) { return session.session_id == "parent"; }) };
            require(parent != loaded.sessions.end() && parent->children.size() == 1 && parent->children[0].title == "저장 확인", "Child event not saved.");
        }

        nlohmann::json record(const std::string& type, nlohmann::json payload)
        {
            SYSTEMTIME now {};
            GetSystemTime(&now);
            std::array<char, 64> timestamp {};
            std::snprintf(timestamp.data(), timestamp.size(), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
            return { { "timestamp", timestamp.data() }, { "type", type }, { "payload", std::move(payload) } };
        }

        void codex_observation_parser()
        {
            codex_rollout reader {};
            reader.consume(record("session_meta", { { "id", "live-thread" }, { "cwd", "E:/workspace" }, { "source", "vscode" } }));
            require(reader.root_session() && reader.snapshot().session_id == "live-thread", "Existing desktop session not discovered.");
            reader.consume(record("event_msg", { { "type", "task_started" }, { "turn_id", "live-turn" } }));
            require(reader.snapshot().status == session_status::running, "Live turn not running.");
            reader.consume(record("response_item", { { "type", "message" }, { "role", "user" }, { "content", { { { "type", "input_text" }, { "text", "사용자 요청문" } } } } }));
            require(reader.snapshot().summary.empty(), "User prompt became work summary.");
            reader.consume(record("response_item",
                { { "type", "message" }, { "role", "assistant" }, { "phase", "commentary" },
                    { "content", { { { "type", "output_text" }, { "text", "자동 수집기를 구현하고 있습니다. 다음 문장." } } } } }));
            require(reader.snapshot().summary == "자동 수집기를 구현하고 있습니다.", "Public progress was not summarized.");
            reader.consume(record(
                "response_item", { { "type", "message" }, { "role", "assistant" }, { "phase", "analysis" }, { "content", { { { "type", "output_text" }, { "text", "private reasoning" } } } } }));
            require(reader.snapshot().summary.find("private") == std::string::npos, "Private reasoning was exposed.");
            reader.consume(record("response_item", { { "type", "custom_tool_call" }, { "call_id", "call-1" }, { "name", "functions.exec" }, { "input", "await tools.apply_patch('...');" } }));
            require(reader.snapshot().activity.find("코드 수정") != std::string::npos, "Patch activity not recognized.");
            reader.consume(record("event_msg", { { "type", "exec_approval_request" } }));
            require(reader.snapshot().status == session_status::waiting_approval, "Approval not represented.");
            reader.consume(record("response_item", { { "type", "custom_tool_call_output" }, { "call_id", "call-1" }, { "output", "sensitive tool output" } }));
            require(reader.snapshot().active_tools.empty() && reader.snapshot().summary.find("sensitive") == std::string::npos, "Tool output leaked or tool remained active.");
            reader.consume(record("event_msg", { { "type", "task_complete" }, { "turn_id", "old-turn" } }));
            require(!reader.snapshot().turn_closed, "Old turn end closed live turn.");
            reader.consume(record("event_msg", { { "type", "task_complete" }, { "turn_id", "live-turn" }, { "last_agent_message", "자동 수집 구현을 완료했습니다." } }));
            require(reader.snapshot().turn_closed && reader.snapshot().summary == "자동 수집 구현을 완료했습니다.", "Completion lost work summary.");
            auto local { reader.snapshot() };
            local.runtime_loaded = true;
            require(is_widget_visible(local, now_ms() + widget_retention_ms * 2), "Open idle session disappeared.");
            std::vector<session_snapshot> merged { local };
            merged.front().observation_source.clear();
            merged.front().updated_ms = local.updated_ms + 1;
            merged.front().status = session_status::waiting_input;
            merge_codex_sessions(merged, { local });
            require(
                merged.size() == 1 && merged.front().status == session_status::waiting_input && merged.front().summary == local.summary, "Hook and discovery were duplicated or latest state lost.");
            auto old_end { merged.front() };
            old_end.turn_id = "old-turn";
            old_end.last_event = "session_ended";
            old_end.status = session_status::ended;
            merged = { old_end };
            merge_codex_sessions(merged, { local });
            require(merged.front().turn_id == local.turn_id && merged.front().status == local.status, "Old hook turn closed observed current turn.");
            old_end.last_event = "turn_started";
            old_end.turn_id = "new-turn";
            old_end.summary.clear();
            merged = { old_end };
            merge_codex_sessions(merged, { local });
            require(merged.front().turn_id == "new-turn" && merged.front().summary.empty(), "New hook turn displayed previous work description.");
            codex_rollout child {};
            child.consume(record("session_meta", { { "id", "child-thread" }, { "source", { { "subagent", "parent" } } } }));
            require(!child.root_session(), "Subagent was exposed as root session.");

            // 데스크톱 앱이 연 스레드만 앱 주소로 연다. CLI 스레드는 창 이동에 맡긴다.
            const std::string thread { "01a172a5-0e1d-7fa0-ad2e-3dfbf33ced95" };
            codex_rollout desktop {};
            desktop.consume(record("session_meta", { { "id", thread }, { "originator", "Codex Desktop" }, { "source", "vscode" } }));
            require(desktop.snapshot().desktop_session_id == thread, "Desktop thread not marked.");
            codex_rollout terminal {};
            terminal.consume(record("session_meta", { { "id", thread }, { "originator", "codex_cli_rs" }, { "source", "cli" } }));
            require(terminal.snapshot().desktop_session_id.empty(), "CLI thread marked as desktop.");
            merged = { terminal.snapshot() };
            merge_codex_sessions(merged, { desktop.snapshot() });
            require(merged.front().desktop_session_id == thread, "Desktop thread lost on merge.");
            require(codex_desktop_link(thread) == L"codex://threads/01a172a5-0e1d-7fa0-ad2e-3dfbf33ced95", "Codex link differs.");
            require(codex_desktop_link("live-thread").empty() && codex_desktop_link("01a172a5-0e1d-7fa0-ad2e-3dfbf33ced9/").empty(), "Invalid Codex link built.");
        }

        void codex_discovery(const std::filesystem::path& directory)
        {
            const auto codex_home { directory / "codex-home" };
            const auto sessions { codex_home / "sessions" };
            const auto locks { codex_home / "thread-writer-locks" };
            std::filesystem::create_directories(sessions);
            std::filesystem::create_directories(locks);
            const std::string id { "11111111-2222-3333-4444-555555555555" };
            const auto path { sessions / ("rollout-" + id + ".jsonl") };
            const auto lock_file { locks / (id + ".lock") };
            std::ofstream { lock_file }.close();
            require(!codex_writer_active(lock_file), "Stale lock file was considered active.");
            const auto close_handle { [](void* handle) { CloseHandle(handle); } };
            std::unique_ptr<void, decltype(close_handle)> handle { CreateFileW(
                lock_file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
            require(handle.get() != INVALID_HANDLE_VALUE, "Cannot open fixture lock.");
            OVERLAPPED overlap {};
            require(LockFileEx(handle.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap) != FALSE, "Cannot hold fixture writer lock.");
            require(codex_writer_active(lock_file), "Held writer lock not detected.");
            {
                std::ofstream file { path, std::ios::binary };
                file << record("session_meta", { { "id", id }, { "source", "cli" }, { "cwd", "E:/fixture" } }).dump() << '\n';
                file << record("event_msg", { { "type", "task_started" }, { "turn_id", "fixture-turn" } }).dump() << '\n';
                file << record("response_item",
                    { { "type", "message" }, { "role", "assistant" }, { "phase", "commentary" }, { "content", { { { "type", "output_text" }, { "text", "실시간 세션을 확인 중입니다." } } } } })
                            .dump()
                     << '\n';
            }
            codex_monitor monitor { codex_home };
            const auto wait = [&](const auto& predicate) {
                const auto deadline { std::chrono::steady_clock::now() + std::chrono::seconds { 6 } };
                while (std::chrono::steady_clock::now() < deadline)
                {
                    const auto view { monitor.snapshot() };
                    if (view.sessions.size() == 1 && predicate(view.sessions.front()))
                        return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
                }
                return false;
            };
            require(wait([](const auto& session) { return session.runtime_loaded && session.summary == "실시간 세션을 확인 중입니다."; }), "Existing session not automatically collected.");
            const auto completed { record("event_msg", { { "type", "task_complete" }, { "turn_id", "fixture-turn" }, { "last_agent_message", "수집 완료" } }).dump() };
            std::ofstream { path, std::ios::binary | std::ios::app } << completed;
            std::this_thread::sleep_for(std::chrono::milliseconds { 1100 });
            require(monitor.snapshot().sessions.front().status == session_status::running, "Partial JSON record was applied prematurely.");
            std::ofstream { path, std::ios::binary | std::ios::app } << '\n';
            require(wait([](const auto& session) { return session.turn_closed && session.summary == "수집 완료"; }), "Appended completion not collected.");
            UnlockFileEx(handle.get(), 0, 1, 0, &overlap);
            require(wait([](const auto& session) { return !session.runtime_loaded && session.turn_closed; }), "Released writer lock remained loaded.");
        }
    } // namespace
} // namespace session_board

int main()
{
    const auto directory { std::filesystem::temp_directory_path() / (L"session-board-test-" + std::to_wstring(GetCurrentProcessId())) };
    try
    {
        session_board::lifecycle();
        session_board::validation();
        session_board::widget_lifecycle();
        session_board::children(directory / "children");
        session_board::codex_observation_parser();
        session_board::codex_discovery(directory);
        session_board::storage(directory);
        std::filesystem::remove_all(directory);
        std::cout << session_board::assertions << " checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
