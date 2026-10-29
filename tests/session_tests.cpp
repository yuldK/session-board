#include "core/codex_monitor.h"
#include "core/notices.h"
#include "core/providers.h"
#include "core/session_updates.h"
#include "core/store.h"
#include "core/widget_layout.h"

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

        // 응답 글이 묻고 끝나는가다. 끝의 공백·마크다운 꾸밈은 걷고, 글 가운데의 물음은 묻는 말로 보지 않는다.
        void questions()
        {
            require(asks_user("바로 커밋할까요?"), "Plain question missed.");
            require(asks_user("고쳤습니다.\n\n**커밋할까요?**  \n"), "Decorated question missed.");
            require(asks_user("(다음에 할까요?)"), "Parenthesized question missed.");
            require(asks_user("어떠세요\xEF\xBC\x9F"), "Full-width question missed.");
            require(!asks_user("왜 그런지 확인했습니다. 고쳤습니다."), "Report shown as a question.");
            require(!asks_user("무엇이 문제였을까요? 원인은 캐시였습니다."), "Question in the middle shown as waiting.");
            require(!asks_user(""), "Empty answer shown as a question.");
            session_snapshot snapshot {};
            const auto apply = [&](const std::string& name, const std::string& summary) {
                session_event event {};
                event.provider = "claude";
                event.session_id = "questions";
                event.event = name;
                event.timestamp_ms = 1000;
                event.turn_id = "turn";
                event.summary = summary;
                static_cast<void>(apply_event(snapshot, event));
            };
            apply("turn_started", "");
            apply("turn_finished", "다음으로 넘어갈까요?");
            require(snapshot.awaiting_reply, "Hook question not kept.");
            require(parse_snapshot(serialize_snapshot(snapshot)).awaiting_reply, "Waiting for a reply lost in storage.");
            apply("turn_started", "");
            require(!snapshot.awaiting_reply, "New turn kept the old question.");
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

            // 끝난 응답이 있는 세션을 다시 열면 새 요청을 기다리는 세션이다. 지난 응답을 응답 완료로 남기지 않는다.
            session_snapshot answered {};
            require(apply_event(answered, event("turn_started")) && apply_event(answered, event("turn_finished")), "Answered turn rejected.");
            require(apply_event(answered, event("session_started")), "Reopen rejected.");
            require(answered.status == session_status::idle && !answered.turn_closed, "Reopened session shown as answered.");
            // 작업 중의 compact는 진행 중인 turn을 그대로 둔다.
            session_snapshot compacting {};
            require(apply_event(compacting, event("turn_started")) && apply_event(compacting, event("session_started")), "Compact rejected.");
            require(compacting.status == session_status::running && !compacting.turn_closed, "Compact reset a running turn.");
        }

        void failures_and_long_tools()
        {
            // 오류의 원인은 failed에만 남고, 정해진 꼴이 아니면 버린다. 다음 turn이 시작되면 지운다.
            const auto claude_event = [](const std::string& kind, const std::string& reason) {
                nlohmann::json input { { "schema_version", 1 }, { "provider", "claude" }, { "session_id", "test-session" }, { "turn_id", "turn-1" } };
                input["event"] = kind;
                input["timestamp_ms"] = now_ms();
                input["failure"] = reason;
                return parse_event(input);
            };
            const auto failure { claude_event("failed", "rate_limit") };
            require(failure.failure == "rate_limit", "Failure reason lost.");
            require(claude_event("failed", "Rate Limit!").failure.empty(), "Malformed failure reason kept.");
            const auto stray { claude_event("turn_finished", "rate_limit") };
            require(stray.failure.empty(), "Failure reason kept on a non-failure event.");
            session_snapshot snapshot {};
            require(apply_event(snapshot, event("turn_started", "claude")) && apply_event(snapshot, failure), "Failure rejected.");
            require(snapshot.status == session_status::failed && snapshot.failure == "rate_limit", "Failure reason not stored.");
            require(parse_snapshot(serialize_snapshot(snapshot)).failure == "rate_limit", "Failure reason not persisted.");
            require(failure_label("rate_limit") == "사용 한도 도달" && failure_label("새 종류") == "API 오류", "Failure label differs.");
            auto next { event("turn_started", "claude") };
            next.turn_id = "turn-2";
            require(apply_event(snapshot, next) && snapshot.failure.empty(), "Failure reason survived a new turn.");
            require(valid_provider("antigravity") && !valid_provider("gemini"), "Provider table differs.");

            // 열린 세션의 결과 없는 도구는 오래 돌아도 확인할 일이 아니다. 근거가 하나라도 없으면 예전처럼 의심한다.
            session_snapshot running {};
            running.status = session_status::running;
            running.updated_ms = now_ms() - 11 * 60 * 1000;
            running.active_tools = { "tool-1" };
            require(needs_attention(running, now_ms()) && !long_running_tool(running, now_ms()), "Hook-only long tool trusted.");
            running.runtime_loaded = true;
            require(!needs_attention(running, now_ms()) && long_running_tool(running, now_ms()), "Open session's long tool needs attention.");
            running.active_tools.clear();
            require(needs_attention(running, now_ms()), "Silent open session without a tool trusted.");
            running.status = session_status::waiting_approval;
            running.active_tools = { "tool-1" };
            require(needs_attention(running, now_ms()), "Long approval wait hidden.");

            // 열린 것을 본 세션이 닫혔으면 작업 중 상태를 종료로 확정한다. 끝난 상태는 그대로 둔다.
            session_snapshot vanished {};
            vanished.status = session_status::running;
            vanished.active_tools = { "tool-1" };
            vanished.children.push_back({ .kind = child_kind::background_command, .child_id = "task-1", .status = session_status::running, .background = true });
            close_vanished_session(vanished, 1000);
            require(vanished.status == session_status::ended && vanished.turn_closed && vanished.active_tools.empty(), "Vanished session left working.");
            require(vanished.children[0].status == session_status::ended && vanished.updated_ms == 1000, "Vanished session child left running.");
            session_snapshot finished {};
            finished.status = session_status::idle;
            finished.turn_closed = true;
            close_vanished_session(finished, 1000);
            require(finished.status == session_status::idle && finished.updated_ms == 0, "Finished session rewritten on close.");
        }

        void widget_anchoring()
        {
            const widget_area work { -1920, -200, 0, 880 };
            auto layout { fit_widget(work, -500, 700, 400, 560, 40, false) };
            require(layout.header_bottom && layout.y + layout.height - 40 == 700, "Bottom overflow moved the header while flipping upward.");
            layout = fit_widget(work, -500, -100, 400, 560, 40, true);
            require(!layout.header_bottom && layout.y == -100, "Top overflow moved the header while flipping downward.");
            layout = fit_widget(work, -500, 100, 400, 300, 40, true);
            require(layout.header_bottom, "A fitting upward list changed direction unnecessarily.");
            for (const int caption : { 40, 60, 80 })
                for (int y { work.top }; y <= work.bottom - caption; y += 17)
                    for (const bool bottom : { false, true })
                    {
                        const auto fitted { fit_widget(work, -500, y, 3000, 3000, caption, bottom) };
                        require(fitted.x >= work.left && fitted.x + fitted.width <= work.right && fitted.y >= work.top && fitted.y + fitted.height <= work.bottom,
                            "Large widget escaped the monitor work area.");
                        require(fitted.y + (fitted.header_bottom ? fitted.height - caption : 0) == y, "Clamping a large widget moved the header.");
                    }
            layout = fit_widget({ 0, 0, 200, 20 }, 500, 100, 400, 560, 40, false);
            require(layout.width == 200 && layout.height == 20 && layout.x == 0 && layout.y == 0, "Tiny work area was not respected.");
        }

        void hidden_updates()
        {
            session_updates updates {};
            session_snapshot session { .provider = "codex", .session_id = "update-test", .summary = "First", .status = session_status::running };
            updates.observe({ session }, true);
            session.summary = "Second";
            updates.observe({ session }, true);
            require(updates.pending().empty(), "Running progress generated a notification.");
            session.children.push_back({ .child_id = "child", .title = "Child" });
            updates.observe({ session }, true);
            require(updates.pending().empty(), "Running child generated a notification.");
            for (const auto status : { session_status::idle, session_status::interrupted, session_status::ended, session_status::failed })
            {
                session.status = status;
                updates.observe({ session }, true);
                require(updates.pending().empty(), "A status without a response request generated a notification.");
            }
            session.status = session_status::waiting_approval;
            updates.observe({ session }, true);
            require(updates.pending().size() == 1 && updates.pending().front().response_required, "Approval request was not announced.");
            const auto first { updates.pending().front().sequence };
            session.summary = "Question still waiting";
            ++session.revision;
            updates.observe({ session }, true);
            updates.acknowledge("codex", "update-test", first);
            require(updates.pending().size() == 1 && updates.pending().front().response_required, "Progress or acknowledgement hid an outstanding response.");
            session.status = session_status::running;
            updates.observe({ session }, true);
            require(updates.pending().front().session.summary == "Question still waiting", "Resolved response was replaced by running progress.");
            session.summary = "New progress is not a notification";
            updates.observe({ session }, true);
            require(updates.pending().front().session.summary == "Question still waiting", "Progress mutated a resolved request notification.");
            updates.acknowledge("codex", "update-test", updates.pending().front().sequence);
            updates.observe({ session }, true);
            require(updates.pending().empty(), "Running session reappeared after a resolved request was dismissed.");
            ++session.updated_ms;
            ++session.revision;
            updates.observe({ session }, true);
            require(updates.pending().empty(), "Timestamp-only update generated a notification.");
            updates.observe({ session }, false);
            require(updates.pending().empty(), "Opening the list did not mark updates as read.");
            session.summary = "Visible change";
            updates.observe({ session }, false);
            require(updates.pending().empty(), "Visible change generated a notification.");
            session.session_id = "new-session";
            updates.observe({ session }, true);
            require(updates.pending().empty(), "A new running session generated a notification.");
        }

        void completed_updates()
        {
            const auto now { std::chrono::steady_clock::now() };
            const auto finish = [](session_snapshot& session, const session_status status = session_status::idle) {
                session.status = status;
                session.turn_closed = true;
            };
            const auto work = [](session_snapshot& session, const std::string& turn) {
                session.status = session_status::running;
                session.turn_closed = false;
                session.turn_id = turn;
            };
            session_updates updates {};
            session_snapshot session { .provider = "claude", .session_id = "done-test", .turn_id = "turn-1", .title = "보고서 작성해 줘", .summary = "보고서를 작성했습니다." };
            finish(session);
            updates.observe({ session }, true, now);
            require(updates.pending().empty(), "An already finished session was announced on first sight.");
            work(session, "turn-1");
            updates.observe({ session }, true, now);
            require(updates.pending().empty(), "Running work was announced.");
            finish(session);
            updates.observe({ session }, true, now);
            require(updates.pending().size() == 1 && updates.pending().front().completed && !updates.pending().front().response_required, "Finished work was not announced.");
            updates.expire(now + std::chrono::hours { 1 });
            require(updates.pending().size() == 1, "A finished-work notification expired by itself.");
            session.summary = "요약이 바뀌었습니다.";
            updates.observe({ session }, true, now);
            require(updates.pending().front().session.summary == "요약이 바뀌었습니다.", "Finished-work notification kept a stale summary.");

            // 같은 turn이 잠깐 흔들려 다시 끝나도 한 번만 알린다.
            updates.acknowledge("claude", "done-test", updates.pending().front().sequence);
            require(updates.pending().empty(), "Closing a finished-work notification failed.");
            work(session, "turn-1");
            updates.observe({ session }, true, now);
            finish(session);
            updates.observe({ session }, true, now);
            require(updates.pending().empty(), "The same turn was announced twice.");

            // 다시 일하기 시작하면 지난 완료 알림을 거둔다.
            work(session, "turn-2");
            updates.observe({ session }, true, now);
            finish(session);
            updates.observe({ session }, true, now);
            require(updates.pending().size() == 1, "The next turn's completion was not announced.");
            work(session, "turn-3");
            updates.observe({ session }, true, now);
            require(updates.pending().empty(), "A completion stayed after the session resumed.");

            // 응답을 처리한 뒤 끝나면 처리됨 알림이 끝난 알림 하나로 바뀌고 저절로 사라지지 않는다.
            session.status = session_status::waiting_input;
            updates.observe({ session }, true, now);
            work(session, "turn-3");
            updates.observe({ session }, true, now);
            require(updates.pending().size() == 1 && updates.pending().front().expires_at.has_value(), "Handled response did not start expiry.");
            finish(session);
            updates.observe({ session }, true, now);
            require(updates.pending().size() == 1 && updates.pending().front().completed && !updates.pending().front().expires_at.has_value(), "Handled response did not become a completion.");
            updates.expire(now + std::chrono::hours { 1 });
            require(updates.pending().size() == 1, "Completion inherited the handled-response expiry.");

            // 오류로 멈춘 것은 알리고, 사용자가 중단했거나 세션이 닫힌 것은 알리지 않는다.
            session_updates others {};
            session_snapshot failed { .provider = "codex", .session_id = "fail-test", .status = session_status::running };
            session_snapshot stopped { .provider = "codex", .session_id = "stop-test", .status = session_status::running };
            session_snapshot closed { .provider = "antigravity", .session_id = "close-test", .status = session_status::running };
            others.observe({ failed, stopped, closed }, true, now);
            finish(failed, session_status::failed);
            finish(stopped, session_status::interrupted);
            finish(closed, session_status::ended);
            others.observe({ failed, stopped, closed }, true, now);
            require(others.pending().size() == 1 && others.pending().front().session.session_id == "fail-test" && others.pending().front().completed, "Failure or user stop announced wrongly.");
            // 응답이 필요한 알림이 끝난 알림보다 위다.
            session_snapshot asking { .provider = "claude", .session_id = "ask-test", .status = session_status::waiting_approval };
            others.observe({ failed, stopped, closed, asking }, true, now);
            require(others.pending().size() == 2 && others.pending().front().response_required, "Response request was not ordered first.");

            // 펼친 동안 작업 중이던 세션이 접은 뒤 끝나도 알린다. 펼치면 알림은 읽은 것이다.
            session_updates later {};
            session_snapshot visible { .provider = "codex", .session_id = "later-test", .status = session_status::running };
            later.observe({ visible }, false, now);
            finish(visible);
            later.observe({ visible }, true, now);
            require(later.pending().size() == 1 && later.pending().front().completed, "Completion right after collapsing was missed.");
            later.observe({ visible }, false, now);
            require(later.pending().empty(), "Opening the list did not mark completions as read.");
        }

        void persistent_responses()
        {
            const auto now { std::chrono::steady_clock::now() };
            session_updates updates {};
            session_snapshot session { .provider = "claude", .session_id = "response-test", .status = session_status::waiting_approval };
            updates.observe({ session }, false, now);
            updates.observe({ session }, true, now);
            require(updates.pending().size() == 1 && updates.pending().front().response_required, "Existing approval disappeared when collapsing.");
            updates.acknowledge("claude", "response-test", updates.pending().front().sequence);
            updates.expire(now + std::chrono::hours { 1 });
            require(updates.pending().size() == 1, "Unread response requirement was dismissed or timed out.");
            updates.clear();
            updates.observe({ session }, true, now);
            require(updates.pending().size() == 1, "Approval did not return after reopening and collapsing.");
            session.status = session_status::running;
            updates.observe({ session }, true, now);
            require(!updates.pending().front().response_required && updates.pending().front().expires_at.has_value(), "External approval did not start notification expiry.");
            session.summary = "Still working";
            updates.observe({ session }, true, now + std::chrono::seconds { 4 });
            updates.expire(now + std::chrono::seconds { 7 });
            require(updates.pending().size() == 1, "Resolved response vanished too early.");
            updates.expire(now + std::chrono::seconds { 8 });
            require(updates.pending().empty(), "Resolved response expiry was extended by ordinary updates.");
            session.status = session_status::waiting_input;
            updates.observe({ session }, true, now);
            require(updates.pending().front().response_required && !updates.pending().front().expires_at.has_value(), "New input request inherited an expiry.");
            updates.observe({}, true, now);
            updates.expire(now + std::chrono::seconds { 8 });
            require(updates.pending().empty(), "A removed session's approval stayed forever.");
            session.status = session_status::running;
            session.children.push_back({ .child_id = "waiting-child", .status = session_status::waiting_input });
            updates.observe({ session }, true, now);
            session_snapshot other { .provider = "codex", .session_id = "ordinary", .summary = "New update" };
            updates.observe({ session, other }, true, now);
            require(updates.pending().size() == 1 && updates.pending().front().response_required, "Ordinary progress polluted the pending response notifications.");
            session.children.front().status = session_status::running;
            updates.observe({ session, other }, true, now);
            updates.expire(now + std::chrono::seconds { 8 });
            require(updates.pending().empty(), "Resolved child notification did not expire or an ordinary session was announced.");

            // 묻고 끝난 응답도 답을 기다리는 알림이라 접힌 동안 밖에 띄운다. 답하지 않기로 하고 숨기면 그 응답은 다시 띄우지 않는다.
            session_updates replies {};
            session_snapshot asked { .provider = "claude", .session_id = "reply-test", .turn_id = "turn-1", .status = session_status::running };
            replies.observe({ asked }, true, now);
            asked.status = session_status::idle;
            asked.turn_closed = true;
            asked.awaiting_reply = true;
            replies.observe({ asked }, true, now);
            require(replies.pending().size() == 1 && replies.pending().front().response_required && !replies.pending().front().completed, "A question at the end of an answer was not kept as a response request.");
            replies.expire(now + std::chrono::hours { 1 });
            require(replies.pending().size() == 1, "A waiting question timed out.");
            replies.acknowledge("claude", "reply-test", replies.pending().front().sequence);
            require(replies.pending().empty(), "A waiting question could not be dismissed.");
            replies.observe({ asked }, true, now);
            require(replies.pending().empty(), "A dismissed question came back for the same answer.");
            asked.turn_id = "turn-2";
            asked.status = session_status::running;
            asked.turn_closed = false;
            asked.awaiting_reply = false;
            replies.observe({ asked }, true, now);
            asked.status = session_status::idle;
            asked.turn_closed = true;
            asked.awaiting_reply = true;
            replies.observe({ asked }, true, now);
            require(replies.pending().size() == 1 && replies.pending().front().response_required, "A new question after a dismissed one was not shown.");
            // 이미 묻고 끝나 있던 세션도 접는 순간 알린다.
            session_updates collapsed {};
            collapsed.observe({ asked }, false, now);
            collapsed.observe({ asked }, true, now);
            require(collapsed.pending().size() == 1 && collapsed.pending().front().response_required, "A waiting question disappeared when collapsing.");
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

        void notices()
        {
            const std::string rule { "----------------------------------------" };
            const std::string text { "머리말\n\n\n" + rule + "\nComponent: luil\n" + rule + "\r\nMIT License\r\n\r\n\n\n" + rule + "\nComponent: Skia and its bundled components\n" + rule + "\n소개\n"
                + rule + "\nComponent: Skia\n" + rule + "\nBSD\n\n\n" + rule + "\nComponent: nlohmann/json\n" + rule + "\nMIT\n" };
            const auto components { parse_notices(text) };
            require(components.size() == 4, "Notice sections not split.");
            require(components[0].name == "luil" && components[0].text == "MIT License", "Notice name or text not trimmed.");
            require(components[1].text == "소개" && components[2].name == "Skia", "Nested Skia section not split.");
            require(components[3].name == "nlohmann/json" && components[3].text == "MIT", "Last notice section lost.");
            require(parse_notices("고지 없음").empty(), "Text without sections produced components.");
            const auto crlf { parse_notices(rule + "\r\nComponent: zlib\r\n" + rule + "\r\nzlib License\r\n") };
            require(crlf.size() == 1 && crlf[0].name == "zlib" && crlf[0].text == "zlib License", "CRLF notices not split.");
            std::ifstream generated { SESSION_BOARD_NOTICES_FILE, std::ios::binary };
            const std::string embedded { std::istreambuf_iterator<char> { generated }, std::istreambuf_iterator<char> {} };
            const auto embedded_components { parse_notices(embedded) };
            require(std::ranges::any_of(embedded_components, [](const auto& component) { return component.name == "nlohmann/json" && component.text.find("MIT License") != std::string::npos; }),
                "Generated notices lost the nlohmann/json license.");
            require(std::ranges::any_of(embedded_components, [](const auto& component) { return component.name == "luil"; }), "Generated notices lost the luil license.");

            const auto lines { wrap_notice_lines("Permission is hereby granted, free of charge, to any person\r\n\n", 20) };
            require(lines.size() == 4 && lines[0] == "Permission is hereby" && lines[1] == "granted, free of" && lines[2] == "charge, to any" && lines[3] == "person", "Words not wrapped at spaces.");
            const auto tabs { wrap_notice_lines("a\r\n\tb", 20) };
            require(tabs.size() == 2 && tabs[0] == "a" && tabs[1] == "    b", "Tab or carriage return not normalized.");
            const auto long_word { wrap_notice_lines(std::string(40, 'x'), 16) };
            require(long_word.size() == 3 && long_word[0].size() == 16, "Long word not cut at the column.");
            const auto wide { wrap_notice_lines("가나다라마바사아자차카타파하", 16) };
            require(wide.size() == 2 && wide[0] == "가나다라마바사아", "Wide glyphs not counted as two columns.");
        }
    } // namespace
} // namespace session_board

int main()
{
    const auto directory { std::filesystem::temp_directory_path() / (L"session-board-test-" + std::to_wstring(GetCurrentProcessId())) };
    try
    {
        session_board::questions();
        session_board::lifecycle();
        session_board::failures_and_long_tools();
        session_board::widget_anchoring();
        session_board::hidden_updates();
        session_board::completed_updates();
        session_board::persistent_responses();
        session_board::notices();
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
