#pragma once

#include "app/single_instance.h"
#include "app/tray_icon.h"
#include "app/widget_window.h"
#include "core/claude_desktop.h"
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
    struct theme_mode_intent
    {
        bool dark { true };
    };
    // 키 컬러를 이 id로 둔다 (luil 키 컬러 목록의 id).
    struct accent_intent
    {
        std::u8string id {};
    };
    // 위젯 머릿줄 아래의 테마 설정 줄을 펼치거나 접는다.
    struct theme_settings_intent
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
        // 참이면 이미 열려 있어도 닫지 않는다. 트레이 메뉴는 열린 상태를 모르고 연다.
        bool keep_open { false };
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
    // 위젯 창이 다듬어 둔 진하기다. 100%에서 DWM 합성을 꺼야 하므로 슬라이더는 위젯 창(UI thread)에 먼저 알리고,
    // 그쪽이 합성을 고친 뒤 이 값을 보낸다.
    struct opacity_intent
    {
        float value { widget_default_opacity };
    };
    struct backdrop_settings_intent
    {};
    // 그 세션이 열린 Claude Code·Codex 창을 앞으로 가져온다.
    struct focus_session_intent
    {
        std::u8string key {};
    };
    // 상세 창이 옮겨지거나 크기가 바뀌었다.
    struct detail_placement_intent
    {
        luil::win32::window_placement placement {};
    };

    // 위젯 외양이다. 다음 실행에 그대로 되살린다.
    struct widget_appearance
    {
        bool dark { true };
        std::u8string accent_id { luil::default_accent_id };
        widget_backdrop backdrop { widget_backdrop::blur };
        float opacity { widget_default_opacity };

        [[nodiscard]] bool operator==(const widget_appearance&) const = default;
    };

    // 없거나 깨진 항목, 목록에 없는 키 컬러는 기본값으로 물러선다. 파일이 없으면 모두 기본값이다.
    [[nodiscard]] widget_appearance load_widget_appearance(const std::filesystem::path& file);
    // 실패해도 위젯 동작을 막지 않는다. 다음에 바뀔 때 다시 쓴다.
    void save_widget_appearance(const std::filesystem::path& file, const widget_appearance& appearance) noexcept;

    // 창(위젯·상세)의 마지막 자리와 크기다. 값이 없거나 깨졌으면 빈 값이라 기본 자리로 연다.
    [[nodiscard]] std::optional<luil::win32::window_placement> load_window_placement(const std::filesystem::path& file);
    // 실패해도 창 동작을 막지 않는다. 다음 이동 때 다시 쓴다.
    void save_window_placement(const std::filesystem::path& file, const luil::win32::window_placement& placement) noexcept;

    [[nodiscard]] luil::caption_config board_caption();
    // 상세 창의 caption이다. 보조 창의 비클라이언트 판정과 tree의 caption이 같은 값을 쓴다.
    [[nodiscard]] luil::caption_config detail_caption(bool dark);

    class board_driver final : public luil::logic_driver
    {
    public:
        explicit board_driver(std::filesystem::path directory, bool open_detail = false, std::filesystem::path codex_directory = {}, std::filesystem::path claude_directory = {},
            std::optional<claude_hook_result> claude_hooks = std::nullopt, std::optional<codex_hook_result> codex_hooks = std::nullopt, std::filesystem::path detail_placement_file = {},
            widget_appearance appearance = {}, std::filesystem::path appearance_file = {});
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
        // 외양이 마지막으로 저장한 값과 다르면 저장한다. 진하기를 끄는 동안 쓰기가 몰리지 않게 tick과 종료 때만 부른다.
        void save_appearance();
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
        // Claude 데스크톱이 연 세션의 색인이다. 이동 단추가 창 대신 그 세션을 연다.
        std::unique_ptr<claude_desktop_index> desktop_ {};
        std::chrono::steady_clock::time_point desktop_refresh_ {};
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
        bool theme_settings_open_ { false };
        std::u8string accent_id_ { luil::default_accent_id };
        widget_backdrop backdrop_ { widget_backdrop::blur };
        float opacity_ { widget_default_opacity };
        // 상세 창의 마지막 자리다. 비어 있으면 위젯 왼쪽의 기본 자리로 연다.
        //  - 파일이 비어 있으면 저장하지 않는다 (테스트 데이터로 연 실행). 그래도 이번 실행 안에서는 다시 열 때 쓴다.
        std::filesystem::path detail_placement_file_ {};
        std::optional<luil::win32::window_placement> detail_placement_ {};
        // 비어 있으면 외양을 저장하지 않는다 (테스트 데이터로 연 실행).
        std::filesystem::path appearance_file_ {};
        widget_appearance saved_appearance_ {};
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
        // `placement_file`이 비어 있으면 자리를 저장하지 않는다 (테스트 데이터로 연 실행).
        // `restored`는 저장한 자리로 연 실행이다.
        // `appearance`의 흐림·진하기로 위젯 창의 DWM 합성을 처음부터 맞춘다.
        board_delegate(std::filesystem::path placement_file, bool restored, single_instance* instance, widget_appearance appearance = {});
        void on_started(luil::app_host& host) override;
        void execute_app_ui_command(luil::app_host& host, const luil::app_ui_command& command) override;
        [[nodiscard]] luil::app_message make_window_metrics_message(float width, float height, float scale) override;
        [[nodiscard]] luil::app_message make_window_placement_message(const luil::win32::window_placement& placement) override;

    private:
        widget_window window_ {};
        tray_icon tray_ {};
        std::filesystem::path placement_file_ {};
        bool restored_ { false };
        widget_appearance appearance_ {};
        single_instance* instance_ { nullptr };
    };

    class board_policy final : public luil::interaction_policy
    {
    public:
        [[nodiscard]] std::vector<luil::input_action> on_wheel(const luil::ui_tree& tree, const luil::mouse_wheel_event& event, float delta) override;
    };
} // namespace session_board
