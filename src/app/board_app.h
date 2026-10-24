#pragma once

#include "app/widget_window.h"
#include "core/claude_hooks.h"
#include "core/claude_monitor.h"
#include "core/codex_hooks.h"
#include "core/codex_monitor.h"
#include "core/store.h"
#include "luil/luil.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>

namespace session_board {
    struct metrics_intent
    {
        float width { 0.0f };
        float height { 0.0f };
        float scale { 1.0f };
    };
    struct close_intent
    {};
    struct refresh_intent
    {};
    struct provider_intent
    {
        std::string provider {};
    };
    struct working_intent
    {};
    struct theme_intent
    {};
    struct widget_failed_intent
    {};
    struct scroll_intent
    {
        float value { 0.0f };
        bool absolute { false };
        // 상세 창 목록이면 참이다. 위젯과 상세 창은 스크롤을 따로 갖는다.
        bool detail { false };
    };
    struct widget_state_intent
    {
        bool collapsed { false };
        bool pinned { true };
    };
    struct expand_session_intent
    {
        std::u8string owner {};
    };
    struct select_session_intent
    {
        std::u8string key {};
    };
    struct open_folder_intent
    {
        std::u8string path {};
    };
    // 상세 창을 연다. 세션 열쇠가 있으면 그 세션을 고른 채로 연다.
    struct open_detail_intent
    {
        std::u8string key {};
    };
    struct close_detail_intent
    {};
    struct detail_metrics_intent
    {
        float width { 0.0f };
        float height { 0.0f };
        float scale { 1.0f };
    };
    struct backdrop_intent
    {
        widget_backdrop backdrop { widget_backdrop::blur };
    };
    struct opacity_intent
    {
        float value { 0.35f };
    };
    struct backdrop_settings_intent
    {};
    // 그 세션이 열린 Claude Code·Codex 창을 앞으로 가져온다.
    struct focus_session_intent
    {
        std::u8string key {};
    };

    [[nodiscard]] luil::caption_config board_caption();
    // 상세 창의 caption이다. 보조 창의 비클라이언트 판정과 tree의 caption이 같은 값을 쓴다.
    [[nodiscard]] luil::caption_config detail_caption(bool dark);

    class board_driver final : public luil::logic_driver
    {
    public:
        explicit board_driver(std::filesystem::path directory, bool open_detail = false, std::filesystem::path codex_directory = {}, std::filesystem::path claude_directory = {},
            std::optional<claude_hook_result> claude_hooks = std::nullopt, std::optional<codex_hook_result> codex_hooks = std::nullopt);
        ~board_driver() override;
        void start() override;
        void handle(luil::app_message message) override;
        [[nodiscard]] std::shared_ptr<const luil::ui_frame> make_frame() override;
        [[nodiscard]] luil::app_message make_close_message() override;
        [[nodiscard]] bool shutdown_completed() const override;
        [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next_tick() override;
        void tick(std::chrono::steady_clock::time_point now) override;

    private:
        void reload();
        // 주 창(위젯)의 tree다.
        [[nodiscard]] std::unique_ptr<luil::root_element> make_widget_tree(std::int64_t now);
        // 상세 창의 tree다. 보조 창 client의 (0,0)에서 시작한다.
        [[nodiscard]] std::unique_ptr<luil::root_element> make_detail_tree(std::int64_t now);
        // 세션 열쇠로 그 세션이 열린 창을 찾아 앞으로 가져온다. 못 찾으면 이유를 알림 줄에 남긴다.
        void focus_session(const std::u8string& key);
        // provider별 마지막 수신을 한 줄씩 `\n`으로 이은 글이다. 상세 화면 상태 표시줄과 위젯 바닥이 함께 쓴다.
        [[nodiscard]] std::u8string signal_text(std::int64_t now) const;
        std::filesystem::path directory_ {};
        load_result data_ {};
        std::filesystem::path codex_directory_ {};
        std::unique_ptr<codex_monitor> codex_ {};
        codex_observation codex_data_ {};
        std::filesystem::path claude_directory_ {};
        std::unique_ptr<claude_monitor> claude_ {};
        claude_observation claude_data_ {};
        // 앱 시작 때 시도한 Claude Hook 자동 등록 결과다. 시도하지 않은 실행에서는 비어 있다.
        std::optional<claude_hook_result> claude_hooks_ {};
        std::optional<codex_hook_result> codex_hooks_ {};
        metrics_intent metrics_ {};
        detail_metrics_intent detail_metrics_ {};
        // 창 바탕을 투명하게 둔 스타일이다. 위젯은 이 위에 반투명 영역을 그리고, 상세 창은 불투명 바탕을 다시 깐다.
        std::shared_ptr<const luil::ui_style> style_ {};
        std::string provider_ {};
        std::string error_ {};
        // 세션 창 이동처럼 잠깐 알릴 일이다. 다음 이동이나 새로 고침에서 지운다.
        std::u8string notice_ {};
        std::chrono::steady_clock::time_point notice_until_ {};
        bool working_only_ { false };
        bool dark_ { true };
        bool detail_open_ { false };
        bool collapsed_ { false };
        bool pinned_ { true };
        bool backdrop_settings_open_ { false };
        widget_backdrop backdrop_ { widget_backdrop::blur };
        float opacity_ { 0.35f };
        std::u8string expanded_session_ {};
        std::u8string selected_session_ {};
        float scroll_ { 0.0f };
        float detail_scroll_ { 0.0f };
        HANDLE changes_ { INVALID_HANDLE_VALUE };
        std::chrono::steady_clock::time_point next_tick_ {};
        std::atomic<bool> closed_ { false };
    };

    class board_delegate final : public luil::win32::window_delegate
    {
    public:
        void on_started(luil::app_host& host) override;
        void execute_app_ui_command(luil::app_host& host, const luil::app_ui_command& command) override;
        [[nodiscard]] luil::app_message make_window_metrics_message(float width, float height, float scale) override;

    private:
        widget_window window_ {};
    };

    class board_policy final : public luil::interaction_policy
    {
    public:
        [[nodiscard]] std::vector<luil::input_action> on_wheel(const luil::ui_tree& tree, const luil::mouse_wheel_event& event, float delta) override;
    };
} // namespace session_board
