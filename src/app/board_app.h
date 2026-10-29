#pragma once

#include "app/single_instance.h"
#include "app/tray_icon.h"
#include "app/widget_window.h"
#include "core/antigravity_monitor.h"
#include "core/claude_desktop.h"
#include "core/claude_hooks.h"
#include "core/claude_monitor.h"
#include "core/codex_hooks.h"
#include "core/codex_monitor.h"
#include "core/notices.h"
#include "core/providers.h"
#include "core/session_updates.h"
#include "core/store.h"
#include "luil/luil.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>

namespace session_board {
    struct metrics_intent
    {
        float width { 0.0f };
        float height { 0.0f };
        float scale { 1.0f };
        bool header_bottom { false };
        float notification_room { 0.0f };
        // 창 왼쪽·오른쪽으로 남은 작업 영역 폭(물리 픽셀)이다. 세부 정보 팝업이 위젯을 덮지 않는 쪽을 고른다.
        float room_left { 0.0f };
        float room_right { 0.0f };
        // 접힘 상태는 크기와 한 메시지로 온다. luil은 이 크기를 처리한 첫 tree로 창 크기를 바꾸므로 그 tree가 새
        // 상태도 담아야 한다.
        bool collapsed { false };
    };
    struct close_intent
    {};
    struct refresh_intent
    {};
    struct acknowledge_update_intent
    {
        std::string provider {};
        std::string session_id {};
        std::uint64_t sequence { 0 };
    };
    // 응답을 기다리지 않는 알림을 모두 숨긴다.
    struct acknowledge_all_intent
    {};
    // 위젯의 최근 완료·종료 묶음을 비운다. 새 소식이 오면 그 세션은 다시 보인다.
    struct clear_completed_intent
    {};
    // 카드 위에 머무르면 옆에 세부 정보를 띄우고, 떠나면 닫는다.
    struct preview_intent
    {
        // 카드 element id다. 늦게 온 떠남이 다른 카드의 팝업을 닫지 않게 가른다.
        std::u8string owner {};
        std::u8string key {};
        // 카드가 놓인 표면이다. 비어 있으면 위젯 창, 아니면 접힌 알림 팝업이다.
        std::u8string surface {};
        bool open { false };
    };
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
        luil::theme_preference theme { luil::theme_preference::dark };
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
        bool updates { false };
    };
    struct widget_state_intent
    {
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
        widget_backdrop backdrop { widget_backdrop::acrylic };
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
    // 상세 창 본문을 세션 목록과 오픈 소스 라이선스 사이에서 바꾼다.
    struct licenses_intent
    {
        // 참이면 닫지 않고 상세 창까지 연다. 트레이 메뉴는 열린 상태를 모르고 연다.
        bool keep_open { false };
    };
    // 라이선스 화면에서 볼 구성 요소다 (고지의 절 순서).
    struct select_license_intent
    {
        std::size_t index { 0 };
    };
    struct license_scroll_intent
    {
        float value { 0.0f };
        bool absolute { false };
        // 참이면 원문, 거짓이면 구성 요소 목록이다.
        bool text { false };
    };

    // 위젯 외양이다. 다음 실행에 그대로 되살린다.
    struct widget_appearance
    {
        luil::theme_preference theme { luil::theme_preference::dark };
        std::u8string accent_id { luil::default_accent_id };
        widget_backdrop backdrop { widget_backdrop::acrylic };
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
    void save_window_placement(const std::filesystem::path& file, const luil::win32::window_placement& placement, bool header_bottom = false) noexcept;
    [[nodiscard]] bool load_widget_header_bottom(const std::filesystem::path& file) noexcept;

    [[nodiscard]] luil::caption_config board_caption();
    [[nodiscard]] luil::caption_config widget_caption();
    // 상세 창의 caption이다. 보조 창의 비클라이언트 판정과 tree의 caption이 같은 값을 쓴다.
    //  - `licenses`면 본문이 라이선스 화면이라 그 단추가 세션 목록으로 돌아가는 단추가 된다.
    [[nodiscard]] luil::caption_config detail_caption(bool dark, bool licenses = false);

    class board_driver final : public luil::logic_driver
    {
    public:
        explicit board_driver(std::filesystem::path directory, bool open_detail = false, std::filesystem::path codex_directory = {}, std::filesystem::path claude_directory = {},
            std::optional<claude_hook_result> claude_hooks = std::nullopt, std::optional<codex_hook_result> codex_hooks = std::nullopt, std::filesystem::path detail_placement_file = {},
            widget_appearance appearance = {}, std::filesystem::path appearance_file = {}, std::filesystem::path antigravity_directory = {});
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
        // 지금 보이는 테마가 어두운가다 (시스템이면 OS 앱 모드).
        [[nodiscard]] bool dark() const;
        // 외양이 마지막으로 저장한 값과 다르면 저장한다. 진하기를 끄는 동안 쓰기가 몰리지 않게 tick과 종료 때만 부른다.
        void save_appearance();
        // 주 창(위젯)의 tree다.
        [[nodiscard]] std::unique_ptr<luil::root_element> make_widget_tree(std::int64_t now);
        void make_update_popup(luil::ui_frame& frame, std::int64_t now);
        // 머무른 카드 옆의 세부 정보 팝업이다. 위젯을 덮지 않는 쪽에 자리가 없으면 띄우지 않는다.
        void make_preview_popup(luil::ui_frame& frame, std::int64_t now);
        // 상세 창의 tree다. 보조 창 client의 (0,0)에서 시작한다.
        [[nodiscard]] std::unique_ptr<luil::root_element> make_detail_tree(std::int64_t now);
        // 상세 창 본문 자리의 라이선스 화면이다. 왼쪽은 구성 요소, 오른쪽은 고른 것의 원문이다.
        //  - 흘린 양을 배치 뒤에 되읽도록 두 목록을 `list_view`·`text_view`로 알린다. 고지가 없으면 비워 둔다.
        [[nodiscard]] std::unique_ptr<luil::ui_element> make_license_body(float width, float height, luil::virtual_list_element*& list_view, luil::virtual_list_element*& text_view);
        // 세션 열쇠로 그 세션이 열린 창을 찾아 앞으로 가져온다. 못 찾으면 이유를 알림 줄에 남긴다.
        void focus_session(const std::u8string& key);
        // provider별 마지막 수신·수집 상태 한 줄씩이다. 상세 화면 상태 표시줄과 위젯 바닥이 함께 쓴다.
        [[nodiscard]] std::vector<std::pair<std::string, std::u8string>> signal_lines(std::int64_t now) const;
        std::filesystem::path directory_ {};
        load_result data_ {};
        session_updates updates_ {};
        std::filesystem::path codex_directory_ {};
        std::unique_ptr<codex_monitor> codex_ {};
        codex_observation codex_data_ {};
        std::filesystem::path claude_directory_ {};
        std::unique_ptr<claude_monitor> claude_ {};
        claude_observation claude_data_ {};
        std::filesystem::path antigravity_directory_ {};
        std::unique_ptr<antigravity_monitor> antigravity_ {};
        antigravity_observation antigravity_data_ {};
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
        // 시스템이면 OS 앱 모드를 따른다. 그리기는 luil이 풀고, 앱은 상세 창 밝기 단추에서만 지금 보이는 쪽을 묻는다.
        luil::theme_preference theme_ { luil::theme_preference::dark };
        bool detail_open_ { false };
        bool collapsed_ { false };
        bool pinned_ { true };
        bool backdrop_settings_open_ { false };
        bool theme_settings_open_ { false };
        std::u8string accent_id_ { luil::default_accent_id };
        widget_backdrop backdrop_ { widget_backdrop::acrylic };
        float opacity_ { widget_default_opacity };
        // 상세 창의 마지막 자리다. 비어 있으면 위젯 왼쪽의 기본 자리로 연다.
        //  - 파일이 비어 있으면 저장하지 않는다 (테스트 데이터로 연 실행). 그래도 이번 실행 안에서는 다시 열 때 쓴다.
        std::filesystem::path detail_placement_file_ {};
        std::optional<luil::win32::window_placement> detail_placement_ {};
        // 비어 있으면 외양을 저장하지 않는다 (테스트 데이터로 연 실행).
        std::filesystem::path appearance_file_ {};
        widget_appearance saved_appearance_ {};
        bool licenses_open_ { false };
        // 실행 파일 resource의 고지를 처음 열 때 한 번 나눠 둔다. 못 읽었으면 빈 목록이라 화면이 그렇게 말한다.
        std::optional<std::vector<notice_component>> licenses_ {};
        std::size_t license_ { 0 };
        float license_list_scroll_ { 0.0f };
        float license_text_scroll_ { 0.0f };
        // 고른 원문을 나눈 줄이다. 구성 요소나 한 줄 칸 수가 바뀔 때만 다시 나눈다.
        std::shared_ptr<const std::vector<luil::virtual_list_item>> license_lines_ {};
        std::size_t license_lines_index_ { 0 };
        std::size_t license_lines_columns_ { 0 };
        // 세부 정보를 띄운 카드다. 비어 있으면 팝업이 없다.
        preview_intent preview_ {};
        // 그 카드 윗변의 표면 안 자리(논리 픽셀)다. 카드를 그린 tree가 배치 뒤에 채운다. 화면에 없으면 비어 있다.
        std::optional<float> preview_top_ {};
        // 정리한 세션과 그때의 마지막 소식이다. 이보다 새 소식이 오면 다시 보인다.
        std::map<std::u8string, std::int64_t> cleared_ {};
        std::u8string expanded_session_ {};
        std::u8string selected_session_ {};
        float scroll_ { 0.0f };
        float detail_scroll_ { 0.0f };
        float update_scroll_ { 0.0f };
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
        board_delegate(std::filesystem::path placement_file, bool restored, single_instance* instance, widget_appearance appearance = {}, bool header_bottom = false);
        void on_started(luil::app_host& host) override;
        void execute_app_ui_command(luil::app_host& host, const luil::app_ui_command& command) override;
        [[nodiscard]] luil::app_message make_window_metrics_message(float width, float height, float scale) override;
        [[nodiscard]] luil::app_message make_window_placement_message(const luil::win32::window_placement& placement) override;

    private:
        widget_window window_ {};
        tray_icon tray_ {};
        std::filesystem::path placement_file_ {};
        bool restored_ { false };
        bool header_bottom_ { false };
        widget_appearance appearance_ {};
        single_instance* instance_ { nullptr };
    };

    class board_policy final : public luil::interaction_policy
    {
    public:
        [[nodiscard]] std::vector<luil::input_action> on_wheel(const luil::ui_tree& tree, const luil::mouse_wheel_event& event, float delta) override;
    };
} // namespace session_board
