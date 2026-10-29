#include "core/session_updates.h"

#include <algorithm>
#include <set>

namespace session_board {
    bool awaits_reply(const session_snapshot& session)
    {
        return session.status == session_status::idle && session.turn_closed && session.awaiting_reply;
    }

    namespace {
        bool needs_response(const session_snapshot& session)
        {
            const auto waiting = [](const session_status status) { return status == session_status::waiting_approval || status == session_status::waiting_input; };
            return waiting(session.status) || std::ranges::any_of(session.children, [&](const auto& child) { return waiting(child.status); });
        }

        // 응답이 끝났거나 오류로 멈췄다. 사용자가 직접 중단했거나 세션이 닫힌 것은 사용자가 이미 아는 일이라 알리지 않는다.
        bool finished(const session_snapshot& session)
        {
            return (session.status == session_status::idle && session.turn_closed) || session.status == session_status::failed;
        }

        // 확인할 일, 끝난 일, 처리된 응답 순이다. 같은 종류는 최근 것이 위다.
        int rank(const session_update& update)
        {
            return update.response_required ? 0 : update.completed ? 1 : 2;
        }
    } // namespace

    void session_updates::observe(const std::vector<session_snapshot>& sessions, const bool hidden, const std::chrono::steady_clock::time_point now)
    {
        // 펼친 동안에도 작업 중이었는지는 따라간다. 접은 직후 끝난 작업도 알려야 한다.
        std::map<session_key, bool> working {};
        std::set<session_key> current {};
        std::set<session_key> resumed {};
        for (const auto& session : sessions)
        {
            const session_key key { session.provider, session.session_id };
            const auto previous { working_.find(key) };
            const bool was_working { previous != working_.end() && previous->second };
            working[key] = is_working(session.status);
            current.insert(key);
            if (!hidden)
                continue;
            const auto dismissed { dismissed_replies_.find(key) };
            const bool reply_dismissed { dismissed != dismissed_replies_.end() && dismissed->second == session.turn_id };
            const bool response_required { needs_response(session) || (awaits_reply(session) && !reply_dismissed) };
            const auto pending { std::ranges::find_if(pending_, [&](const auto& item) { return item.session.provider == session.provider && item.session.session_id == session.session_id; }) };
            if (response_required)
            {
                if (pending == pending_.end())
                    pending_.push_back({ .session = session, .sequence = ++sequence_, .response_required = true });
                else
                {
                    if (!pending->response_required)
                        pending->sequence = ++sequence_;
                    pending->session = session;
                    pending->response_required = true;
                    pending->completed = false;
                    pending->expires_at.reset();
                }
            }
            else if (was_working && finished(session) && !awaits_reply(session))
            {
                auto& announced { completed_turns_[key] };
                const bool repeated { !session.turn_id.empty() && announced == session.turn_id };
                announced = session.turn_id;
                if (pending != pending_.end())
                {
                    // 처리된 응답 알림은 끝난 알림으로 바뀐다. 같은 세션의 알림은 하나다.
                    if (!pending->completed)
                        pending->sequence = ++sequence_;
                    pending->session = session;
                    pending->response_required = false;
                    pending->completed = true;
                    pending->expires_at.reset();
                }
                else if (!repeated)
                    pending_.push_back({ .session = session, .sequence = ++sequence_, .completed = true });
            }
            else if (pending != pending_.end() && pending->response_required)
            {
                // 작업 중으로 돌아와도 진행 갱신 알림으로 바꾸지 않는다. 마지막 요청을 처리됨으로만 남긴다.
                pending->response_required = false;
                pending->expires_at = now + std::chrono::seconds { 8 };
            }
            else if (pending != pending_.end() && pending->completed)
            {
                // 다시 일하기 시작했으면 지난 완료는 사용자가 이미 이어 간 일이다.
                if (is_working(session.status))
                    resumed.insert(key);
                else
                    pending->session = session;
            }
        }
        working_ = std::move(working);
        if (!hidden)
        {
            clear();
            return;
        }
        std::erase_if(pending_, [&](const auto& item) { return item.completed && resumed.contains({ item.session.provider, item.session.session_id }); });
        for (auto& item : pending_)
            if (item.response_required && !current.contains({ item.session.provider, item.session.session_id }))
            {
                item.response_required = false;
                item.expires_at = now + std::chrono::seconds { 8 };
            }
        expire(now);
        std::ranges::stable_sort(pending_, [](const auto& left, const auto& right) { return rank(left) != rank(right) ? rank(left) < rank(right) : left.sequence > right.sequence; });
    }

    void session_updates::acknowledge(const std::string& provider, const std::string& session_id, const std::uint64_t sequence)
    {
        std::erase_if(pending_, [&](const auto& item) {
            if (item.session.provider != provider || item.session.session_id != session_id || item.sequence != sequence)
                return false;
            // 묻고 끝난 응답은 답하지 않기로 할 수 있다. 그 turn은 다시 띄우지 않는다.
            if (item.response_required && awaits_reply(item.session))
            {
                dismissed_replies_[{ provider, session_id }] = item.session.turn_id;
                return true;
            }
            return !item.response_required;
        });
    }

    void session_updates::expire(const std::chrono::steady_clock::time_point now)
    {
        std::erase_if(pending_, [&](const auto& item) { return !item.response_required && item.expires_at.has_value() && now >= *item.expires_at; });
    }

    void session_updates::clear() noexcept
    {
        pending_.clear();
    }

    const std::vector<session_update>& session_updates::pending() const noexcept
    {
        return pending_;
    }
} // namespace session_board
