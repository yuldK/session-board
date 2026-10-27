#include "core/session_updates.h"

#include <algorithm>
#include <set>

namespace session_board {
    namespace {
        bool needs_response(const session_snapshot& session)
        {
            const auto waiting = [](const session_status status) { return status == session_status::waiting_approval || status == session_status::waiting_input; };
            return waiting(session.status) || std::ranges::any_of(session.children, [&](const auto& child) { return waiting(child.status); });
        }

    } // namespace

    void session_updates::observe(const std::vector<session_snapshot>& sessions, const bool hidden, const std::chrono::steady_clock::time_point now)
    {
        if (!hidden)
        {
            clear();
            return;
        }
        std::set<std::pair<std::string, std::string>> current {};
        for (const auto& session : sessions)
        {
            const auto key { std::make_pair(session.provider, session.session_id) };
            current.insert(key);
            const bool response_required { needs_response(session) };
            const auto pending { std::ranges::find_if(pending_, [&](const auto& item) { return item.session.provider == session.provider && item.session.session_id == session.session_id; }) };
            if (response_required)
            {
                if (pending == pending_.end())
                    pending_.push_back({ session, ++sequence_, true });
                else
                {
                    if (!pending->response_required)
                        pending->sequence = ++sequence_;
                    pending->session = session;
                    pending->response_required = true;
                    pending->expires_at.reset();
                }
            }
            else if (pending != pending_.end() && pending->response_required)
            {
                // 작업 중으로 돌아와도 진행 갱신 알림으로 바꾸지 않는다. 마지막 요청을 처리됨으로만 남긴다.
                pending->response_required = false;
                pending->expires_at = now + std::chrono::seconds { 8 };
            }
        }
        for (auto& item : pending_)
            if (item.response_required && !current.contains({ item.session.provider, item.session.session_id }))
            {
                item.response_required = false;
                item.expires_at = now + std::chrono::seconds { 8 };
            }
        expire(now);
        std::ranges::stable_sort(
            pending_, [](const auto& left, const auto& right) { return left.response_required != right.response_required ? left.response_required : left.sequence > right.sequence; });
    }

    void session_updates::acknowledge(const std::string& provider, const std::string& session_id, const std::uint64_t sequence)
    {
        std::erase_if(pending_, [&](const auto& item) { return !item.response_required && item.session.provider == provider && item.session.session_id == session_id && item.sequence == sequence; });
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
