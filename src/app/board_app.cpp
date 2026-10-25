#include "app/board_app.h"

#include "core/host_window.h"
#include "luil/generated/codicons.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/utils/SkParsePath.h"

#include <nlohmann/json.hpp>

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <functional>
#include <string>
#include <utility>

namespace session_board {
    namespace {
        constexpr auto kind_layout { luil::application_element_kind(0) };
        constexpr auto kind_label { luil::application_element_kind(1) };
        constexpr auto kind_button { luil::application_element_kind(2) };
        constexpr auto kind_badge { luil::application_element_kind(3) };
        // 상세 화면의 치수다 (논리 픽셀). 행과 머리글은 luil의 목록 치수를 따른다.
        constexpr float toolbar_height { 48.0f };
        constexpr float status_bar_height { 24.0f };
        constexpr float group_header_height { 28.0f };
        constexpr float session_row_height { 48.0f };
        constexpr float child_row_height { 40.0f };
        constexpr float child_indent { 20.0f };
        constexpr float row_inset { 16.0f };
        constexpr float row_mark_width { 3.0f };
        constexpr float status_cell_width { 116.0f };
        constexpr float age_cell_width { 56.0f };
        constexpr float detail_width { 390.0f };
        constexpr float detail_inset { 20.0f };
        constexpr float field_height { 28.0f };
        constexpr float field_name_width { 64.0f };
        constexpr float filter_width { 264.0f };

        std::u8string u8(const std::string_view text)
        {
            return { text.begin(), text.end() };
        }

        std::unique_ptr<luil::label_element> label(const std::u8string& id, std::u8string text, const float size = 12.0f, const luil::label_color_role color = luil::label_color_role::primary,
            const luil::text_align align = luil::text_align::start, const luil::text_elide elide = luil::text_elide::end)
        {
            const luil::label_config config { .text = text, .font_size = size, .color = color, .align = align, .elide = elide };
            auto result { std::make_unique<luil::label_element>(luil::ui_element_id { kind_label, id }, config) };
            result->set_tooltip(std::move(text));
            result->set_hit_opaque(false);
            return result;
        }

        // 팔레트 역할 하나로 칠한 면이다. 구분선·상태 점·선택 표식이 모두 이 꼴이다.
        std::unique_ptr<luil::panel_element> fill(const std::u8string& id, std::function<luil::ui_color(const luil::ui_color_palette&)> color, const float radius = 0.0f)
        {
            auto result { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, id }, luil::panel_config { .background = std::move(color), .corner_radius = radius }) };
            result->set_hit_opaque(false);
            return result;
        }

        std::unique_ptr<luil::panel_element> divider(const std::u8string& id)
        {
            return fill(id, [](const luil::ui_color_palette& palette) { return palette.divider; });
        }

        // 자식에게 넘기는 팔레트만 바꾼다. 키 컬러로 채운 머릿줄 위의 글·아이콘을 `accent_foreground`로 그리는 데 쓴다.
        //  - 라벨·단추의 그리기를 베껴 쓰지 않고 역할 색만 갈아 끼우므로 테마와 고대비를 그대로 따라간다.
        class palette_scope final : public luil::ui_element
        {
        public:
            palette_scope(const std::u8string& id, std::function<void(luil::ui_color_palette&)> tint, std::unique_ptr<luil::ui_element> content)
                : ui_element { luil::ui_element_id { kind_layout, id } }
                , tint_ { std::move(tint) }
            {
                set_hit_opaque(false);
                add_child(std::move(content));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                for (const auto& child : children())
                    child->arrange(context.for_child(context.slot));
            }

            [[nodiscard]] std::optional<luil::size_f> preferred_size(const luil::measure_context& context, const float available_width) const override
            {
                return children().empty() ? std::nullopt : children().front()->preferred_size(context, available_width);
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                luil::ui_color_palette palette { context.palette };
                tint_(palette);
                luil::draw_context tinted {
                    .canvas = context.canvas,
                    .codicon_typeface = context.codicon_typeface,
                    .ui_typeface = context.ui_typeface,
                    .code_typeface = context.code_typeface,
                    .fonts = context.fonts,
                    .palette = palette,
                    .scale = context.scale,
                    .now = context.now,
                    .maximized = context.maximized,
                    .fullscreen = context.fullscreen,
                    .metrics = context.metrics,
                    .reduce_motion = context.reduce_motion,
                };
                draw_children(tinted, interaction);
            }

        private:
            std::function<void(luil::ui_color_palette&)> tint_ {};
        };

        // Simple Icons(CC0)의 Claude·OpenAI 표식 경로다 (24×24 viewBox).
        constexpr char claude_mark_path[] = "m4.7144 15.9555 4.7174-2.6471.079-.2307-.079-.1275h-.2307l-.7893-.0486-2.6956-.0729-2.3375-.0971-2.2646-.1214-.5707-.1215-.5343-.7042.0546-.3522.4797-"
                                            ".3218.686.0608 1.5179.1032 2.2767.1578 1.6514.0972 2.4468.255h.3886l.0546-.1579-.1336-.0971-.1032-.0972L6.973 9.8356l-2.55-1.6879-1.3356-.9714-.7225-."
                                            "4918-.3643-.4614-.1578-1.0078.6557-.7225.8803.0607.2246.0607.8925.686 1.9064 1.4754 2.4893 1.8336.3643.3035.1457-.1032.0182-.0728-.164-.2733-1.3539-2."
                                            "4467-1.445-2.4893-.6435-1.032-.17-.6194c-.0607-.255-.1032-.4674-.1032-.7285L6.287.1335 6.6997 0l.9957.1336.419.3642.6192 1.4147 1.0018 2.2282 1.5543 3"
                                            ".0296.4553.8985.2429.8318.091.255h.1579v-.1457l.1275-1.706.2368-2.0947.2307-2.6957.0789-.7589.3764-.9107.7468-.4918.5828.2793.4797.686-.0668.4433-.285"
                                            "3 1.8517-.5586 2.9021-.3643 1.9429h.2125l.2429-.2429.9835-1.3053 1.6514-2.0643.7286-.8196.85-.9046.5464-.4311h1.0321l.759 1.1293-.34 1.1657-1.0625 1.3"
                                            "478-.8804 1.1414-1.2628 1.7-.7893 1.36.0729.1093.1882-.0183 2.8535-.607 1.5421-.2794 1.8396-.3157.8318.3886.091.3946-.3278.8075-1.967.4857-2.3072.4614"
                                            "-3.4364.8136-.0425.0304.0486.0607 1.5482.1457.6618.0364h1.621l3.0175.2247.7892.522.4736.6376-.079.4857-1.2142.6193-1.6393-.3886-3.825-.9107-1.3113-.32"
                                            "79h-.1822v.1093l1.0929 1.0686 2.0035 1.8092 2.5075 2.3314.1275.5768-.3218.4554-.34-.0486-2.2039-1.6575-.85-.7468-1.9246-1.621h-.1275v.17l.4432.6496 2."
                                            "3436 3.5214.1214 1.0807-.17.3521-.6071.2125-.6679-.1214-1.3721-1.9246L14.38 17.959l-1.1414-1.9428-.1397.079-.674 7.2552-.3156.3703-.7286.2793-.6071-.4"
                                            "614-.3218-.7468.3218-1.4753.3886-1.9246.3157-1.53.2853-1.9004.17-.6314-.0121-.0425-.1397.0182-1.4328 1.9672-2.1796 2.9446-1.7243 1.8456-.4128.164-.716"
                                            "4-.3704.0667-.6618.4008-.5889 2.386-3.0357 1.4389-1.882.929-1.0868-.0062-.1579h-.0546l-6.3385 4.1164-1.1293.1457-.4857-.4554.0608-.7467.2307-.2429 1.9"
                                            "064-1.3114Z";
        constexpr char codex_mark_path[] = "M22.2819 9.8211a5.9847 5.9847 0 0 0-.5157-4.9108 6.0462 6.0462 0 0 0-6.5098-2.9A6.0651 6.0651 0 0 0 4.9807 4.1818a5.9847 5.9847 0 0 0-3.9977 2.9 6.046"
                                           "2 6.0462 0 0 0 .7427 7.0966 5.98 5.98 0 0 0 .511 4.9107 6.051 6.051 0 0 0 6.5146 2.9001A5.9847 5.9847 0 0 0 13.2599 24a6.0557 6.0557 0 0 0 5.7718-4.20"
                                           "58 5.9894 5.9894 0 0 0 3.9977-2.9001 6.0557 6.0557 0 0 0-.7475-7.0729zm-9.022 12.6081a4.4755 4.4755 0 0 1-2.8764-1.0408l.1419-.0804 4.7783-2.7582a.794"
                                           "8.7948 0 0 0 .3927-.6813v-6.7369l2.02 1.1686a.071.071 0 0 1 .038.052v5.5826a4.504 4.504 0 0 1-4.4945 4.4944zm-9.6607-4.1254a4.4708 4.4708 0 0 1-.5346-"
                                           "3.0137l.142.0852 4.783 2.7582a.7712.7712 0 0 0 .7806 0l5.8428-3.3685v2.3324a.0804.0804 0 0 1-.0332.0615L9.74 19.9502a4.4992 4.4992 0 0 1-6.1408-1.6464"
                                           "zM2.3408 7.8956a4.485 4.485 0 0 1 2.3655-1.9728V11.6a.7664.7664 0 0 0 .3879.6765l5.8144 3.3543-2.0201 1.1685a.0757.0757 0 0 1-.071 0l-4.8303-2.7865A4."
                                           "504 4.504 0 0 1 2.3408 7.872zm16.5963 3.8558L13.1038 8.364 15.1192 7.2a.0757.0757 0 0 1 .071 0l4.8303 2.7913a4.4944 4.4944 0 0 1-.6765 8.1042v-5.6772a"
                                           ".79.79 0 0 0-.407-.667zm2.0107-3.0231l-.142-.0852-4.7735-2.7818a.7759.7759 0 0 0-.7854 0L9.409 9.2297V6.8974a.0662.0662 0 0 1 .0284-.0615l4.8303-2.786"
                                           "6a4.4992 4.4992 0 0 1 6.6802 4.66zM8.3065 12.863l-2.02-1.1638a.0804.0804 0 0 1-.038-.0567V6.0742a4.4992 4.4992 0 0 1 7.3757-3.4537l-.142.0805L8.704 5."
                                           "459a.7948.7948 0 0 0-.3927.6813zm1.0976-2.3654l2.602-1.4998 2.6069 1.4998v2.9994l-2.5974 1.4997-2.6067-1.4997Z";

        // Claude·Codex 표식이다. 상표라 아이콘 글꼴(codicons)에 없으므로 SVG 경로를 그린다.
        //  - Simple Icons에 Codex 표식이 없어 OpenAI 표식을 쓴다.
        //  - 바로 뒤의 글이 provider 이름을 말하므로 보조 기술에는 드러내지 않는다 (이름을 두 번 읽지 않게).
        //  - 경로는 처음 그릴 때 한 번만 푼다.
        class provider_mark final : public luil::ui_element
        {
        public:
            provider_mark(const std::u8string& id, const std::string_view provider)
                : ui_element { luil::ui_element_id { kind_label, id } }
                , codex_ { provider == "codex" }
            {
                set_hit_opaque(false);
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot&) const override
            {
                const luil::rect_f box { bounds() };
                const float side { std::min(box.width, box.height) };
                if (side <= 0.0f)
                    return;
                // Claude는 고유색이 있어 테마와 관계없이 그 색이고, Codex는 단색 표식이라 글자색을 따른다.
                SkPaint paint { luil::solid_paint(codex_ ? context.palette.primary_foreground : luil::make_ui_color(0xd9, 0x77, 0x57)) };
                paint.setAntiAlias(true);
                context.canvas.save();
                context.canvas.translate(box.x + (box.width - side) / 2.0f, box.y + (box.height - side) / 2.0f);
                context.canvas.scale(side / 24.0f, side / 24.0f);
                context.canvas.drawPath(codex_ ? codex_path() : claude_path(), paint);
                context.canvas.restore();
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::none };
            }

        private:
            [[nodiscard]] static const SkPath& claude_path()
            {
                static const SkPath path { SkParsePath::FromSVGString(claude_mark_path).value_or(SkPath {}) };
                return path;
            }

            [[nodiscard]] static const SkPath& codex_path()
            {
                static const SkPath path { SkParsePath::FromSVGString(codex_mark_path).value_or(SkPath {}) };
                return path;
            }

            bool codex_ { false };
        };

        // 표식으로 시작하는 한 줄이다. provider 이름은 표식 뒤에도 글로 남겨 읽기·검색·툴팁이 그대로다.
        //  - `fit`이면 글 길이만큼만 차지한다 (상태 표시줄처럼 맞춤으로 쌓는 자리).
        std::unique_ptr<luil::stack_element> provider_line(
            const std::u8string& id, const std::string_view provider, std::u8string text, const float size, const luil::label_color_role color = luil::label_color_role::dim, const bool fit = false)
        {
            const luil::stack_config config { .direction = luil::stack_direction::row, .spacing = 4.0f, .cross_alignment = luil::stack_alignment::center };
            auto line { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-line" }, config) };
            const float mark { size + 1.0f };
            line->add(std::make_unique<provider_mark>(id + u8"-mark", provider), { .length = mark, .cross_length = mark });
            if (fit)
                line->add_fit(label(id, std::move(text), size, color));
            else
                line->add_flexible(label(id, std::move(text), size, color));
            return line;
        }

        // 앱 UI 명령의 인자로 싣는 수다. 같은 프로세스 안에서만 오가므로 짧은 십진 표기면 된다.
        std::u8string number_text(const float value)
        {
            std::array<char, 32> buffer {};
            const auto [end, error] { std::to_chars(buffer.data(), buffer.data() + buffer.size(), value) };
            return error == std::errc {} ? u8(std::string_view { buffer.data(), end }) : std::u8string { u8"0" };
        }

        std::optional<float> parse_number(const std::u8string_view text)
        {
            float value { 0.0f };
            const auto* const first { reinterpret_cast<const char*>(text.data()) };
            const auto [end, error] { std::from_chars(first, first + text.size(), value) };
            if (error != std::errc {} || end != first + text.size() || !std::isfinite(value))
                return std::nullopt;
            return value;
        }

        std::unique_ptr<luil::panel_element> panel(const std::u8string& id, std::unique_ptr<luil::ui_element> content)
        {
            luil::panel_config config {};
            config.background = [](const luil::ui_color_palette& palette) { return palette.surface_background; };
            config.corner_radius = 8.0f;
            config.border = [](const luil::ui_color_palette& palette) { return palette.divider; };
            auto result { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, id }, config) };
            result->set_content(std::move(content));
            return result;
        }

        // 데스크톱 앱에서 그 세션을 여는 주소다. 데스크톱 앱이 연 세션이 아니면 비어 있다.
        std::wstring desktop_link(const session_snapshot& session)
        {
            if (session.provider == "claude")
                return claude_desktop_link(session.desktop_session_id);
            if (session.provider == "codex" && !session.desktop_session_id.empty())
                return codex_desktop_link(session.desktop_session_id);
            return {};
        }

        // 이동 단추가 할 수 있는 일이다. 데스크톱 세션이면 앱에서 그 세션을 열고, 아니면 Hook이 기록한 창을 앞으로 가져온다.
        bool can_focus(const session_snapshot& session)
        {
            return !desktop_link(session).empty() || session.host_window != 0;
        }

        std::u8string focus_tooltip(const session_snapshot& session, const std::u8string_view window_text)
        {
            if (!desktop_link(session).empty())
                return session.provider == "claude" ? u8"Claude 데스크톱에서 이 세션을 엽니다" : u8"Codex 앱에서 이 스레드를 엽니다";
            if (session.host_window != 0)
                return std::u8string { window_text };
            return u8"창 정보가 없습니다 — Hook으로 받은 세션과 데스크톱 앱 세션만 이동할 수 있습니다";
        }

        std::u8string state_text(const session_snapshot& session, const std::int64_t now)
        {
            if (needs_attention(session, now))
                return u8"상태 확인 필요";
            switch (session.status)
            {
            case session_status::idle:
                return session.turn_closed ? u8"응답 완료" : u8"대기 중";
            case session_status::running:
                return u8"작업 중";
            case session_status::waiting_approval:
                return u8"승인 대기";
            case session_status::waiting_input:
                return u8"입력 대기";
            case session_status::interrupted:
                return u8"중단됨";
            case session_status::ended:
                return u8"세션 종료";
            case session_status::failed:
                return u8"오류";
            }
            return {};
        }

        std::u8string age_text(const std::int64_t updated, const std::int64_t now)
        {
            const auto seconds { std::max<std::int64_t>(0, (now - updated) / 1000) };
            if (seconds < 10)
                return u8"방금";
            if (seconds < 60)
                return u8(std::to_string(seconds)) + u8"초 전";
            if (seconds < 3600)
                return u8(std::to_string(seconds / 60)) + u8"분 전";
            if (seconds < 86'400)
                return u8(std::to_string(seconds / 3600)) + u8"시간 전";
            return u8(std::to_string(seconds / 86'400)) + u8"일 전";
        }

        std::u8string task_text(const session_snapshot& session)
        {
            if (!session.summary.empty())
                return u8(session.summary);
            if (!session.name.empty())
                return u8(session.name);
            if (!session.tool_name.empty())
                return u8"도구로 작업 진행 중";
            return session.turn_closed ? u8"작업 내용 기록 없음" : u8"작업 내용 수신 대기";
        }

        float add_wrapped(luil::stack_element& stack, const std::u8string& id, const std::u8string& text, const float width, const float size, const std::size_t maximum_lines,
            const luil::label_color_role color = luil::label_color_role::primary)
        {
            const float line_height { size + 8.0f };
            const auto layout { luil::make_text_layout({ .text = text, .font_size = size, .width = std::max(20.0f, width), .wrap = true, .line_height = line_height }) };
            const auto lines { layout.lines() };
            const auto count { std::min(lines.size(), maximum_lines) };
            for (std::size_t index { 0 }; index < count; ++index)
            {
                const auto& line { lines[index] };
                std::u8string part { text.substr(line.begin, line.end - line.begin) };
                if (index + 1 == count && count < lines.size())
                    part += u8"…";
                stack.add(label(id + u8"-" + u8(std::to_string(index)), std::move(part), size, color), line_height);
            }
            return static_cast<float>(count) * line_height;
        }

        std::u8string project_text(const session_snapshot& session)
        {
            const auto last { session.cwd.find_last_not_of("/\\") };
            if (last == std::string::npos)
                return u8"폴더 정보 없음";
            const auto separator { session.cwd.find_last_of("/\\", last) };
            return u8(session.cwd.substr(separator == std::string::npos ? 0 : separator + 1, last - (separator == std::string::npos ? 0 : separator + 1) + 1));
        }

        std::u8string source_text(const session_snapshot& session)
        {
            return std::u8string { session.observation_source == "claude-local" ? u8"Claude 로컬 기록" : u8"Codex 로컬 기록" } + (session.runtime_loaded ? u8" · 열린 세션" : u8" · 최근 활동");
        }

        std::u8string own_activity_text(const session_snapshot& session, const std::int64_t now)
        {
            if (!session.activity.empty())
                return u8(session.activity);
            if (needs_attention(session, now))
                return u8"마지막 상태 · " + u8(std::string { status_name(session.status) }) + (session.tool_name.empty() ? u8"" : u8" · " + u8(session.tool_name));
            if (!session.tool_name.empty())
                return (session.status == session_status::waiting_approval       ? u8"명령 실행 승인 대기 · "
                               : session.status == session_status::waiting_input ? u8"사용자 답변 대기 · "
                                                                                 : u8"도구 실행 중 · ")
                    + u8(session.tool_name);
            if (session.status == session_status::running)
                return u8"분석 또는 응답 작성 중";
            if (session.status == session_status::waiting_approval)
                return u8"사용자 실행 승인 대기";
            if (session.status == session_status::waiting_input)
                return u8"사용자 답변 대기";
            if (session.status == session_status::ended)
                return u8"세션 종료 · 마지막 작업 보관 중";
            if (session.turn_closed)
                return u8"마지막 작업 보관 중 · " + state_text(session, now);
            return u8"다음 요청 대기";
        }

        // 부모가 쉬어도 하위가 돌면 그 사실을 덧붙인다. 부모의 상태 자체는 바꾸지 않는다.
        std::u8string activity_text(const session_snapshot& session, const std::int64_t now)
        {
            auto text { own_activity_text(session, now) };
            const auto working { working_children(session) };
            if (working == 0 || is_working(session.status))
                return text;
            const auto background { std::ranges::count_if(session.children, [](const auto& child) { return child.background && is_working(child.status); }) };
            return text + (static_cast<std::size_t>(background) == working ? u8" · 백그라운드 " : u8" · 하위 ") + u8(std::to_string(working)) + u8"개 진행 중";
        }

        std::u8string elapsed_text(const std::int64_t started, const std::int64_t now)
        {
            const auto seconds { std::max<std::int64_t>(0, (now - started) / 1000) };
            if (seconds < 60)
                return u8(std::to_string(seconds)) + u8"초째";
            if (seconds < 3600)
                return u8(std::to_string(seconds / 60)) + u8"분째";
            return u8(std::to_string(seconds / 3600)) + u8"시간째";
        }

        std::u8string child_state_text(const session_child& child, const std::int64_t now)
        {
            if (child.status == session_status::running && child_needs_attention(child, now))
                return u8"상태 확인 필요";
            switch (child.status)
            {
            case session_status::idle:
                return u8"완료";
            case session_status::running:
                return child.kind == child_kind::background_command ? u8"실행 중" : u8"작업 중";
            case session_status::waiting_approval:
                return u8"승인 대기";
            case session_status::waiting_input:
                return u8"입력 대기";
            case session_status::interrupted:
                return u8"중지됨";
            case session_status::ended:
                return u8"확인 불가";
            case session_status::failed:
                return u8"실패";
            }
            return {};
        }

        std::u8string child_kind_text(const session_child& child)
        {
            if (child.kind == child_kind::background_command)
                return u8"백그라운드";
            const std::u8string type { child.agent_type.empty() ? std::u8string { u8"하위 에이전트" } : u8(child.agent_type) };
            return child.background ? u8"백그라운드 · " + type : type;
        }

        std::u8string child_title(const session_child& child)
        {
            if (!child.title.empty())
                return u8(child.title);
            return child.kind == child_kind::background_command ? u8"백그라운드 명령" : u8"하위 작업";
        }

        std::u8string child_activity_text(const session_child& child)
        {
            if (!child.activity.empty())
                return u8(child.activity);
            return child.tool_name.empty() ? std::u8string {} : u8"도구 실행 중 · " + u8(child.tool_name);
        }

        // 도는 하위는 얼마나 돌았는지, 끝난 하위는 언제 끝났는지 말한다.
        std::u8string child_time_text(const session_child& child, const std::int64_t now)
        {
            return is_working(child.status) && child.started_ms > 0 ? elapsed_text(child.started_ms, now) : age_text(child.updated_ms, now);
        }

        // 확인할 하위, 도는 하위, 끝난 하위 순이다. 도는 하위는 먼저 시작한 것이, 끝난 하위는 최근 것이 위다.
        std::vector<const session_child*> ordered_children(const session_snapshot& session, const std::int64_t now)
        {
            std::vector<const session_child*> result {};
            for (const auto& child : session.children)
                result.push_back(&child);
            const auto rank = [now](const session_child* child) { return child_needs_attention(*child, now) ? 0 : is_working(child->status) ? 1 : 2; };
            std::ranges::stable_sort(result, [&](const auto* left, const auto* right) {
                if (rank(left) != rank(right))
                    return rank(left) < rank(right);
                return rank(left) == 2 ? left->updated_ms > right->updated_ms : left->started_ms < right->started_ms;
            });
            return result;
        }

        struct sized_element
        {
            std::unique_ptr<luil::ui_element> element {};
            float height { 0.0f };
        };

        std::u8string session_key(const session_snapshot& session)
        {
            return u8(session.provider + "-" + stable_key(session.session_id));
        }

        // 하위 행의 열쇠다. 상세 화면의 선택과 UIA 이름이 이 값을 쓴다.
        std::u8string child_key(const session_snapshot& session, const session_child& child)
        {
            return session_key(session) + u8"-child-" + u8(stable_key(std::string { child_kind_name(child.kind) } + ":" + child.child_id));
        }

        std::u8string provider_text(const session_snapshot& session)
        {
            return session.provider == "codex" ? u8"Codex" : u8"Claude";
        }

        // 상세 화면 목록의 묶음이다. 내 차례인 세션이 맨 위에 선다.
        enum class session_group
        {
            attention,
            running,
            other,
        };

        bool own_attention(const session_snapshot& session, const std::int64_t now)
        {
            return session.status == session_status::waiting_approval || session.status == session_status::waiting_input || session.status == session_status::failed || needs_attention(session, now);
        }

        // 묶음은 부모와 하위를 합쳐 정한다. 하위가 승인을 기다리면 부모 카드가 확인 필요로 올라간다.
        session_group group_of(const session_snapshot& session, const std::int64_t now)
        {
            if (own_attention(session, now) || children_need_attention(session, now))
                return session_group::attention;
            if (session.status == session_status::running || working_children(session) > 0)
                return session_group::running;
            return session_group::other;
        }

        struct group_heading
        {
            std::u8string id {};
            std::u8string name {};
            std::u8string hint {};
        };

        group_heading heading_of(const session_group group)
        {
            switch (group)
            {
            case session_group::attention:
                return { u8"group-attention", u8"확인 필요", u8"승인·입력 대기 · 10분 넘게 소식 없음 · 오류" };
            case session_group::running:
                return { u8"group-running", u8"진행 중", u8"" };
            case session_group::other:
                return { u8"group-other", u8"대기 · 완료 · 종료", u8"" };
            }
            return {};
        }

        // 상태 칸이다. 경고·오류는 글리프가 붙는 배지로, 그 밖은 점과 글로 말한다.
        // 중립 배지는 notice 바탕이라 붉게 읽히므로 평상 상태에는 쓰지 않는다.
        std::unique_ptr<luil::ui_element> status_view(const std::u8string& id, const std::u8string& text, const bool attention, const bool error, const bool running, const bool closed)
        {
            const luil::stack_config config { .direction = luil::stack_direction::row, .spacing = 6.0f, .cross_alignment = luil::stack_alignment::center };
            auto cell { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-cell" }, config) };
            if (attention)
            {
                const auto tone { error ? luil::badge_tone::error : luil::badge_tone::warning };
                auto badge { std::make_unique<luil::badge_element>(luil::ui_element_id { kind_badge, id }, luil::badge_config { .text = text, .tone = tone }) };
                badge->set_hit_opaque(false);
                cell->add_fit(std::move(badge));
                cell->add_flexible_gap();
                return cell;
            }
            // 끝난 세션은 속이 빈 점이다. 점의 모양만으로도 진행·완료·종료가 갈린다.
            luil::panel_config dot_config {};
            dot_config.background = [running, closed](const luil::ui_color_palette& palette) {
                if (closed)
                    return luil::with_alpha(palette.surface_background, 0.0f);
                return running ? palette.accent : palette.secondary_foreground;
            };
            dot_config.corner_radius = 4.0f;
            if (closed)
                dot_config.border = [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; };
            auto dot { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, id + u8"-dot" }, dot_config) };
            dot->set_hit_opaque(false);
            cell->add(std::move(dot), { .length = 8.0f, .cross_length = 8.0f });
            cell->add_flexible(label(id, text, 11.0f, running ? luil::label_color_role::primary : luil::label_color_role::dim));
            return cell;
        }

        std::unique_ptr<luil::ui_element> status_cell(const std::u8string& id, const session_snapshot& session, const std::int64_t now)
        {
            if (!own_attention(session, now) && children_need_attention(session, now))
            {
                // 부모 자신은 멀쩡하므로 배지는 하위의 사정을 말한다.
                const auto children { ordered_children(session, now) };
                const auto& child { *children.front() };
                return status_view(id, u8"하위 " + child_state_text(child, now), true, child.status == session_status::failed, false, false);
            }
            const bool closed { session.status == session_status::ended || session.status == session_status::interrupted };
            return status_view(id, state_text(session, now), own_attention(session, now), session.status == session_status::failed, session.status == session_status::running, closed);
        }

        std::unique_ptr<luil::ui_element> child_status_cell(const std::u8string& id, const session_child& child, const std::int64_t now)
        {
            const bool attention { child_needs_attention(child, now) };
            return status_view(id, child_state_text(child, now), attention, child.status == session_status::failed, child.status == session_status::running, !is_working(child.status));
        }

        // 목록 한 행이다: 요청 제목과 그 아래 흐린 한 줄, 상태 칸, 마지막 활동.
        std::unique_ptr<luil::ui_element> session_row(const session_snapshot& session, const std::int64_t now, const bool selected)
        {
            const std::u8string id { session_key(session) };
            const luil::stack_config row_config {
                .direction = luil::stack_direction::row, .spacing = 12.0f, .padding = { row_inset - row_mark_width, 0.0f, row_inset, 0.0f }, .cross_alignment = luil::stack_alignment::center
            };
            auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-row" }, row_config) };
            auto text { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-text" }, luil::stack_config { .spacing = 3.0f }) };
            const std::u8string title { task_text(session) };
            text->add(label(id + u8"-title", title, 12.0f), 17.0f);
            text->add(provider_line(id + u8"-meta", session.provider, provider_text(session) + u8" · " + project_text(session) + u8" · " + activity_text(session, now), 11.0f), 16.0f);
            row->add_flexible(std::move(text));
            row->add(status_cell(id + u8"-status", session, now), status_cell_width);
            row->add(label(id + u8"-age", age_text(latest_update(session), now), 11.0f, luil::label_color_role::dim, luil::text_align::end), age_cell_width);

            // 고른 행은 옅은 채움과 왼쪽 표식이 한 쌍이다.
            auto line { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-line" }, luil::stack_config { .direction = luil::stack_direction::row }) };
            if (selected)
                line->add(fill(id + u8"-mark", [](const luil::ui_color_palette& palette) { return palette.accent; }), row_mark_width);
            else
                line->add_gap(row_mark_width);
            line->add_flexible(std::move(row));

            luil::panel_config surface {};
            surface.background = [selected](const luil::ui_color_palette& palette) { return selected ? palette.row_selection_background : luil::with_alpha(palette.surface_background, 0.0f); };
            auto result { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, id }, surface) };
            result->set_content(std::move(line));
            result->set_hit_opaque(true);
            result->set_cursor(luil::ui_cursor::hand);
            result->set_access_name(provider_text(session) + u8" · " + state_text(session, now) + u8" · " + title);
            result->set_tooltip(title);
            result->set_action(luil::ui_trigger::left_click, luil::make_message_action(select_session_intent { id }));
            return result;
        }

        // 상세 목록의 하위 행이다. 부모 행 바로 아래에 들여 넣고 왼쪽 연결선으로 부모에 잇는다.
        std::unique_ptr<luil::ui_element> child_row(const session_snapshot& session, const session_child& child, const std::int64_t now, const bool selected)
        {
            const std::u8string id { child_key(session, child) };
            const luil::edge_insets padding { row_inset - row_mark_width + child_indent, 0.0f, row_inset, 0.0f };
            const luil::stack_config row_config { .direction = luil::stack_direction::row, .spacing = 12.0f, .padding = padding, .cross_alignment = luil::stack_alignment::center };
            auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-row" }, row_config) };
            row->add(fill(id + u8"-link", [](const luil::ui_color_palette& palette) { return luil::with_alpha(palette.accent, 0.5f); }), 1.0f);
            auto text { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-text" }, luil::stack_config { .spacing = 2.0f }) };
            const std::u8string title { child_title(child) };
            text->add(label(id + u8"-title", title, 12.0f), 16.0f);
            const auto activity { child_activity_text(child) };
            text->add(label(id + u8"-meta", child_kind_text(child) + (activity.empty() ? u8"" : u8" · " + activity), 11.0f, luil::label_color_role::dim), 15.0f);
            row->add_flexible(std::move(text));
            row->add(child_status_cell(id + u8"-status", child, now), status_cell_width);
            row->add(label(id + u8"-age", child_time_text(child, now), 11.0f, luil::label_color_role::dim, luil::text_align::end), age_cell_width);

            auto line { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-line" }, luil::stack_config { .direction = luil::stack_direction::row }) };
            if (selected)
                line->add(fill(id + u8"-mark", [](const luil::ui_color_palette& palette) { return palette.accent; }), row_mark_width);
            else
                line->add_gap(row_mark_width);
            line->add_flexible(std::move(row));

            luil::panel_config surface {};
            surface.background = [selected](const luil::ui_color_palette& palette) { return selected ? palette.row_selection_background : luil::with_alpha(palette.surface_background, 0.0f); };
            auto result { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, id }, surface) };
            result->set_content(std::move(line));
            result->set_hit_opaque(true);
            result->set_cursor(luil::ui_cursor::hand);
            result->set_access_name(u8"하위 · " + child_kind_text(child) + u8" · " + child_state_text(child, now) + u8" · " + title);
            result->set_tooltip(title);
            result->set_action(luil::ui_trigger::left_click, luil::make_message_action(select_session_intent { id }));
            return result;
        }

        std::unique_ptr<luil::ui_element> group_header(const std::u8string& id, const std::u8string& name, const std::size_t count, const std::u8string& hint)
        {
            const luil::stack_config config {
                .direction = luil::stack_direction::row, .spacing = 8.0f, .padding = { row_inset, 0.0f, row_inset, 0.0f }, .cross_alignment = luil::stack_alignment::center
            };
            auto content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-content" }, config) };
            content->add_fit(label(id + u8"-name", name, 11.0f));
            content->add_fit(label(id + u8"-count", u8(std::to_string(count)), 11.0f, luil::label_color_role::dim));
            content->add_flexible(label(id + u8"-hint", hint, 11.0f, luil::label_color_role::dim, luil::text_align::end));
            luil::panel_config surface {};
            surface.background = [](const luil::ui_color_palette& palette) { return luil::with_alpha(palette.primary_foreground, 0.035f); };
            auto result { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, id }, surface) };
            result->set_content(std::move(content));
            return result;
        }

        // 상세 패널의 이름·값 한 줄이다.
        std::unique_ptr<luil::ui_element> detail_field(const std::u8string& id, const std::u8string& name, const std::u8string& value, const luil::text_elide elide = luil::text_elide::end)
        {
            const luil::stack_config config { .direction = luil::stack_direction::row, .spacing = 12.0f, .cross_alignment = luil::stack_alignment::center };
            auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id }, config) };
            row->add(label(id + u8"-name", name, 11.0f, luil::label_color_role::dim), field_name_width);
            row->add_flexible(label(id + u8"-value", value.empty() ? u8"(없음)" : value, 12.0f, luil::label_color_role::primary, luil::text_align::start, elide));
            return row;
        }

        // 고른 세션의 모든 정보다. 목록 행이 줄여 보인 것을 여기서 자르지 않고 보인다.
        std::unique_ptr<luil::ui_element> detail_pane(const session_snapshot& session, const std::int64_t now, const float height)
        {
            constexpr float inner_width { detail_width - 2.0f * detail_inset };
            const luil::stack_config content_config { .padding = { detail_inset, 16.0f, detail_inset, 16.0f } };
            auto content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"detail-body" }, content_config) };
            float used { 32.0f };
            const auto add = [&](std::unique_ptr<luil::ui_element> element, const float length) {
                content->add(std::move(element), length);
                used += length;
            };
            const auto gap = [&](const float length) {
                content->add_gap(length);
                used += length;
            };

            const luil::stack_config header_config { .direction = luil::stack_direction::row, .spacing = 8.0f, .cross_alignment = luil::stack_alignment::center };
            auto header { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"detail-header" }, header_config) };
            header->add_flexible(provider_line(u8"detail-source", session.provider, provider_text(session) + u8" · " + project_text(session), 11.0f));
            header->add_fit(label(u8"detail-age", age_text(latest_update(session), now), 11.0f, luil::label_color_role::dim));
            add(std::move(header), 16.0f);
            gap(8.0f);
            auto heading { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"detail-heading" }, luil::stack_config {}) };
            const float heading_height { add_wrapped(*heading, u8"detail-title", task_text(session), inner_width, 14.0f, 3) };
            add(std::move(heading), heading_height);
            gap(6.0f);
            add(status_cell(u8"detail-status", session, now), 18.0f);
            gap(14.0f);

            std::u8string tool { u8(session.tool_name) };
            if (session.active_tools.size() > 1)
                tool += u8" 외 " + u8(std::to_string(session.active_tools.size() - 1)) + u8"개 병행";
            std::vector<std::pair<std::u8string, std::u8string>> fields { { u8"현재 동작", activity_text(session, now) }, { u8"도구", tool } };
            if (!session.name.empty())
                fields.emplace_back(u8"대화", u8(session.name));
            fields.emplace_back(u8"작업 폴더", u8(session.cwd));
            fields.emplace_back(u8"모델", u8(session.model));
            fields.emplace_back(u8"세션 ID", u8(session.session_id));
            if (!session.observation_source.empty())
                fields.emplace_back(u8"수집", source_text(session));
            add(divider(u8"detail-rule"), 1.0f);
            for (std::size_t index { 0 }; index < fields.size(); ++index)
            {
                const std::u8string id { u8"detail-field-" + u8(std::to_string(index)) };
                const auto& [name, value] { fields[index] };
                const bool path { name == u8"작업 폴더" || name == u8"세션 ID" };
                add(detail_field(id, name, value, path ? luil::text_elide::middle : luil::text_elide::end), field_height);
                add(divider(id + u8"-rule"), 1.0f);
            }

            constexpr float button_row_height { 28.0f };
            constexpr float request_padding { 10.0f };
            gap(14.0f);
            add(label(u8"detail-request-name", u8"요청", 11.0f, luil::label_color_role::dim), 16.0f);
            gap(6.0f);
            // 요청 글은 남는 높이만큼 펼친다. 단추 줄을 밀어내지 않는다.
            const float room { height - used - 2.0f * request_padding - 14.0f - button_row_height };
            const auto lines { static_cast<std::size_t>(std::clamp(room / 20.0f, 1.0f, 12.0f)) };
            const luil::stack_config request_config { .padding = luil::edge_insets::all(request_padding) };
            auto request { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"detail-request" }, request_config) };
            const std::u8string request_text { session.title.empty() ? task_text(session) : u8(session.title) };
            const float request_height { add_wrapped(*request, u8"detail-request-text", request_text, inner_width - 2.0f * request_padding, 12.0f, lines) };
            luil::panel_config request_surface_config {};
            request_surface_config.background = [](const luil::ui_color_palette& palette) { return palette.input_background; };
            request_surface_config.corner_radius = 4.0f;
            auto request_surface { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, u8"detail-request-surface" }, request_surface_config) };
            request_surface->set_content(std::move(request));
            add(std::move(request_surface), request_height + 2.0f * request_padding);
            gap(14.0f);

            const luil::stack_config actions_config { .direction = luil::stack_direction::row, .spacing = 8.0f };
            auto actions { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"detail-actions" }, actions_config) };
            const luil::text_button_config focus_config { .text = u8"세션 창으로 이동", .visual = luil::text_button_visual::accent, .glyph = luil::codicons::icon_link_external };
            auto focus { std::make_unique<luil::text_button_element>(luil::ui_element_id { kind_button, u8"detail-focus" }, focus_config) };
            focus->set_cursor(luil::ui_cursor::hand);
            focus->set_tooltip(focus_tooltip(session, u8"이 세션이 열린 Claude Code·Codex 창을 앞으로 가져옵니다"));
            focus->set_enabled(can_focus(session));
            focus->set_action(luil::ui_trigger::left_click, luil::make_message_action(focus_session_intent { session_key(session) }));
            actions->add_fit(std::move(focus));
            if (!session.cwd.empty())
            {
                const luil::text_button_config open_config { .text = u8"폴더 열기", .glyph = luil::codicons::icon_folder_opened };
                auto open { std::make_unique<luil::text_button_element>(luil::ui_element_id { kind_button, u8"detail-open-folder" }, open_config) };
                open->set_cursor(luil::ui_cursor::hand);
                open->set_tooltip(u8"탐색기에서 작업 폴더를 엽니다");
                open->set_action(luil::ui_trigger::left_click, luil::make_message_action(open_folder_intent { u8(session.cwd) }));
                actions->add_fit(std::move(open));
            }
            const luil::text_button_config copy_config { .text = u8"ID 복사", .glyph = luil::codicons::icon_copy };
            auto copy { std::make_unique<luil::text_button_element>(luil::ui_element_id { kind_button, u8"detail-copy-id" }, copy_config) };
            copy->set_cursor(luil::ui_cursor::hand);
            copy->set_tooltip(u8"세션 ID를 클립보드에 복사합니다");
            copy->set_action(
                luil::ui_trigger::left_click, [text = u8(session.session_id)](const luil::ui_action_context&) -> std::vector<luil::input_action> { return { luil::clipboard_copy_request { text } }; });
            actions->add_fit(std::move(copy));
            content->add(std::move(actions), button_row_height);
            content->add_flexible_gap();

            auto result { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, u8"detail" }, luil::panel_config {}) };
            result->set_content(std::move(content));
            return result;
        }

        // 고른 하위의 정보다. 하위에게 준 지시문은 기록하지 않으므로 요청 칸 대신 설명을 보인다.
        std::unique_ptr<luil::ui_element> child_detail_pane(const session_snapshot& session, const session_child& child, const std::int64_t now)
        {
            constexpr float inner_width { detail_width - 2.0f * detail_inset };
            const luil::stack_config content_config { .padding = { detail_inset, 16.0f, detail_inset, 16.0f } };
            auto content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"detail-body" }, content_config) };
            const luil::stack_config header_config { .direction = luil::stack_direction::row, .spacing = 8.0f, .cross_alignment = luil::stack_alignment::center };
            auto header { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"detail-header" }, header_config) };
            header->add_flexible(provider_line(u8"detail-source", session.provider, provider_text(session) + u8" · " + project_text(session) + u8" · 하위", 11.0f));
            header->add_fit(label(u8"detail-age", age_text(child.updated_ms, now), 11.0f, luil::label_color_role::dim));
            content->add(std::move(header), 16.0f);
            content->add_gap(8.0f);
            auto heading { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"detail-heading" }, luil::stack_config {}) };
            const float heading_height { add_wrapped(*heading, u8"detail-title", child_title(child), inner_width, 14.0f, 3) };
            content->add(std::move(heading), heading_height);
            content->add_gap(6.0f);
            content->add(child_status_cell(u8"detail-status", child, now), 18.0f);
            content->add_gap(14.0f);

            std::vector<std::pair<std::u8string, std::u8string>> fields { { u8"종류", child_kind_text(child) }, { u8"현재 동작", child_activity_text(child) }, { u8"도구", u8(child.tool_name) } };
            fields.emplace_back(u8"시작", child.started_ms > 0 ? age_text(child.started_ms, now) : std::u8string {});
            if (is_working(child.status) && child.started_ms > 0)
                fields.emplace_back(u8"경과", elapsed_text(child.started_ms, now));
            fields.emplace_back(u8"부모 세션", task_text(session));
            fields.emplace_back(u8"하위 ID", u8(child.child_id));
            fields.emplace_back(u8"부모 도구", u8(child.parent_tool_id));
            if (child.depth > 1)
                fields.emplace_back(u8"깊이", u8(std::to_string(child.depth)));
            content->add(divider(u8"detail-rule"), 1.0f);
            for (std::size_t index { 0 }; index < fields.size(); ++index)
            {
                const std::u8string id { u8"detail-field-" + u8(std::to_string(index)) };
                const auto& [name, value] { fields[index] };
                const bool identity { name == u8"하위 ID" || name == u8"부모 도구" };
                content->add(detail_field(id, name, value, identity ? luil::text_elide::middle : luil::text_elide::end), field_height);
                content->add(divider(id + u8"-rule"), 1.0f);
            }
            content->add_gap(14.0f);

            // 하위는 자기 창이 없다. 부모 세션의 창으로 이동한다.
            const luil::stack_config actions_config { .direction = luil::stack_direction::row, .spacing = 8.0f };
            auto actions { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"detail-actions" }, actions_config) };
            const luil::text_button_config focus_config { .text = u8"부모 세션 창으로 이동", .visual = luil::text_button_visual::accent, .glyph = luil::codicons::icon_link_external };
            auto focus { std::make_unique<luil::text_button_element>(luil::ui_element_id { kind_button, u8"detail-focus" }, focus_config) };
            focus->set_cursor(luil::ui_cursor::hand);
            focus->set_tooltip(focus_tooltip(session, u8"이 하위를 띄운 세션의 창을 앞으로 가져옵니다"));
            focus->set_enabled(can_focus(session));
            focus->set_action(luil::ui_trigger::left_click, luil::make_message_action(focus_session_intent { session_key(session) }));
            actions->add_fit(std::move(focus));
            const luil::text_button_config copy_config { .text = u8"ID 복사", .glyph = luil::codicons::icon_copy };
            auto copy { std::make_unique<luil::text_button_element>(luil::ui_element_id { kind_button, u8"detail-copy-id" }, copy_config) };
            copy->set_cursor(luil::ui_cursor::hand);
            copy->set_tooltip(u8"하위 ID를 클립보드에 복사합니다");
            copy->set_action(
                luil::ui_trigger::left_click, [text = u8(child.child_id)](const luil::ui_action_context&) -> std::vector<luil::input_action> { return { luil::clipboard_copy_request { text } }; });
            actions->add_fit(std::move(copy));
            content->add(std::move(actions), 28.0f);
            content->add_flexible_gap();

            auto result { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, u8"detail" }, luil::panel_config {}) };
            result->set_content(std::move(content));
            return result;
        }

        // 위젯 치수다 (논리 픽셀).
        constexpr float widget_gap { 6.0f };
        constexpr float widget_icon_button { 24.0f };
        constexpr float widget_group_header { 28.0f };
        constexpr float widget_footer_height { 30.0f };
        constexpr float widget_setting_row { 26.0f };
        // 진하기의 자석이다. 0%는 바탕 없이 글만 뜨고, 100%는 DWM 합성을 끈 불투명 창이다.
        constexpr std::array<float, 6> widget_opacity_magnets { 0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f };
        constexpr float widget_row_inset { 12.0f };
        constexpr float widget_theme_choice_width { 140.0f };

        std::unique_ptr<luil::button_element> widget_button(const std::u8string& id, const std::u8string& name, const char32_t glyph, luil::ui_action action, const bool active = false)
        {
            const luil::button_config config { .glyph = glyph, .icon_size = 14.0f, .corner_radius = 3.0f, .active = active };
            auto result { std::make_unique<luil::button_element>(luil::ui_element_id { kind_button, id }, config) };
            result->set_tooltip(name);
            result->set_cursor(luil::ui_cursor::hand);
            result->set_action(luil::ui_trigger::left_click, std::move(action));
            return result;
        }

        luil::ui_action command_action(const std::uint32_t command)
        {
            return [command](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                if (command == 0)
                    return { luil::ui_command::application_exit };
                return { luil::app_ui_command { command } };
            };
        }

        // 키 컬러로 채운 머릿줄 위의 색이다. 글·아이콘·hover가 모두 `accent_foreground`에서 나온다.
        void caption_tint(luil::ui_color_palette& palette)
        {
            const luil::ui_color ink { palette.accent_foreground };
            palette.primary_foreground = ink;
            palette.secondary_foreground = luil::with_alpha(ink, 0.8f);
            palette.button_hover_background = luil::with_alpha(ink, 0.16f);
            palette.button_hover_foreground = ink;
            palette.button_pressed_background = luil::with_alpha(ink, 0.28f);
            palette.active_toggle_background = luil::with_alpha(ink, 0.22f);
            palette.accent = ink;
            palette.accent_emphasis_foreground = ink;
        }

        // 묶음 머리글의 글은 옅은 키 컬러 바탕 위의 강조 글자색이다.
        void heading_tint(luil::ui_color_palette& palette)
        {
            palette.primary_foreground = palette.accent_emphasis_foreground;
        }

        std::u8string widget_owner(const session_snapshot& session)
        {
            return u8("compact-" + session.provider + "-" + stable_key(session.session_id));
        }

        // 위젯 하위 행이다: 상태 점, 종류, 설명, 오른쪽에 경과 시간이나 확인 배지.
        std::unique_ptr<luil::ui_element> widget_child_line(const std::u8string& id, const session_child& child, const std::int64_t now)
        {
            const luil::stack_config config { .direction = luil::stack_direction::row, .spacing = 6.0f, .cross_alignment = luil::stack_alignment::center };
            auto line { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id }, config) };
            const bool attention { child_needs_attention(child, now) };
            const bool working { is_working(child.status) };
            // 끝난 하위는 속이 빈 점이다. 부모 카드의 상태 점과 같은 말을 쓴다.
            luil::panel_config dot_config {};
            dot_config.background = [working](const luil::ui_color_palette& palette) { return working ? palette.accent : luil::with_alpha(palette.surface_background, 0.0f); };
            dot_config.corner_radius = 3.5f;
            if (!working)
                dot_config.border = [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; };
            auto dot { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, id + u8"-dot" }, dot_config) };
            dot->set_hit_opaque(false);
            line->add(std::move(dot), { .length = 7.0f, .cross_length = 7.0f });
            // 백그라운드는 부모 응답이 끝난 뒤에도 남는 일이라 강조 글자색으로 가른다.
            auto kind { label(id + u8"-kind", child_kind_text(child), 11.0f, child.background ? luil::label_color_role::primary : luil::label_color_role::dim) };
            if (child.background)
                line->add_fit(std::make_unique<palette_scope>(id + u8"-kind-tint", heading_tint, std::move(kind)));
            else
                line->add_fit(std::move(kind));
            const std::u8string done { working || attention ? std::u8string {} : u8" · " + child_state_text(child, now) };
            line->add_flexible(label(id + u8"-title", child_title(child) + done, 11.0f));
            if (attention)
            {
                const auto tone { child.status == session_status::failed ? luil::badge_tone::error : luil::badge_tone::warning };
                auto badge { std::make_unique<luil::badge_element>(luil::ui_element_id { kind_badge, id + u8"-badge" }, luil::badge_config { .text = child_state_text(child, now), .tone = tone }) };
                badge->set_hit_opaque(false);
                line->add_fit(std::move(badge));
            }
            else
                line->add_fit(label(id + u8"-age", child_time_text(child, now), 11.0f, luil::label_color_role::dim));
            return line;
        }

        // 위젯 카드 안의 하위 목록이다. 접힌 카드는 도는 하위·확인할 하위·막 끝난 하위만 보이고 나머지는 한 줄로 줄인다.
        float add_widget_children(luil::stack_element& content, const session_snapshot& session, const std::u8string& owner, const std::int64_t now, const bool expanded)
        {
            if (session.children.empty())
                return 0.0f;
            constexpr float row_height { 20.0f };
            constexpr float note_height { 16.0f };
            constexpr std::int64_t recent_ms { 2 * 60 * 1000 };
            auto rows { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, owner + u8"-children" }, luil::stack_config {}) };
            float height { 0.0f };
            const auto background { std::ranges::count_if(session.children, [](const auto& child) { return child.background; }) };
            std::u8string summary { u8"하위 " + u8(std::to_string(session.children.size())) };
            if (background > 0)
                summary += u8" · 백그라운드 " + u8(std::to_string(background));
            rows->add(label(owner + u8"-children-summary", summary, 11.0f, luil::label_color_role::dim), note_height);
            height += note_height;
            std::size_t hidden { 0 };
            for (const auto* child : ordered_children(session, now))
            {
                if (!expanded && !is_working(child->status) && !child_needs_attention(*child, now) && now - child->updated_ms >= recent_ms)
                {
                    ++hidden;
                    continue;
                }
                const std::u8string id { owner + u8"-child-" + u8(stable_key(std::string { child_kind_name(child->kind) } + ":" + child->child_id)) };
                rows->add(widget_child_line(id, *child, now), row_height);
                height += row_height;
                const auto activity { child_activity_text(*child) };
                if (expanded && !activity.empty())
                {
                    // 상태 점 너비만큼 들여 종류 글자와 줄을 맞춘다.
                    const luil::stack_config detail_config { .direction = luil::stack_direction::row, .padding = { 13.0f, 0.0f, 0.0f, 0.0f } };
                    auto detail { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-detail" }, detail_config) };
                    detail->add_flexible(label(id + u8"-activity", activity, 11.0f, luil::label_color_role::dim));
                    rows->add(std::move(detail), note_height);
                    height += note_height;
                }
            }
            if (hidden > 0)
            {
                rows->add(label(owner + u8"-children-more", u8"끝난 하위 " + u8(std::to_string(hidden)) + u8"개 더 · 눌러서 펼치기", 11.0f, luil::label_color_role::dim), note_height);
                height += note_height;
            }
            const luil::stack_config block_config { .direction = luil::stack_direction::row, .spacing = 10.0f, .padding = { 3.0f, 0.0f, 0.0f, 0.0f } };
            auto block { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, owner + u8"-children-block" }, block_config) };
            block->add(fill(owner + u8"-children-link", [](const luil::ui_color_palette& palette) { return luil::with_alpha(palette.accent, 0.5f); }), 1.0f);
            block->add_flexible(std::move(rows));
            content.add_gap(6.0f);
            content.add(std::move(block), height);
            return 6.0f + height;
        }

        // 위젯 묶음 안의 한 행이다: 상태·출처·시각과 이동 단추, 요청 두 줄, 지금 하는 일. 누르면 펼친다.
        sized_element widget_row(const session_snapshot& session, const std::int64_t now, const float width, const bool expanded)
        {
            const std::u8string owner { widget_owner(session) };
            const std::u8string title { task_text(session) };
            const luil::stack_config content_config { .padding = { widget_row_inset, 8.0f, 6.0f, 8.0f } };
            auto content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, owner + u8"-body" }, content_config) };
            const luil::stack_config top_config { .direction = luil::stack_direction::row, .spacing = 8.0f, .cross_alignment = luil::stack_alignment::center };
            auto top { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, owner + u8"-top" }, top_config) };
            top->add(status_cell(owner + u8"-status", session, now), 112.0f);
            top->add_flexible(provider_line(owner + u8"-source", session.provider, provider_text(session) + u8" · " + project_text(session), 11.0f));
            top->add_fit(label(owner + u8"-age", age_text(latest_update(session), now), 11.0f, luil::label_color_role::dim));
            const std::u8string focus_name { focus_tooltip(session, provider_text(session) + u8" 세션 창으로 이동") };
            auto focus { widget_button(owner + u8"-focus", focus_name, luil::codicons::icon_link_external, luil::make_message_action(focus_session_intent { session_key(session) })) };
            focus->set_enabled(can_focus(session));
            top->add(std::move(focus), { .length = widget_icon_button, .cross_length = widget_icon_button });
            content->add(std::move(top), widget_icon_button);
            content->add_gap(2.0f);
            const float inner_width { width - widget_row_inset - 6.0f };
            float height { 16.0f + widget_icon_button + 2.0f };
            height += add_wrapped(*content, owner + u8"-request", title, inner_width, 12.0f, expanded ? 16 : 2);
            content->add(label(owner + u8"-activity", activity_text(session, now), 11.0f, luil::label_color_role::dim), 16.0f);
            height += 16.0f;
            height += add_widget_children(*content, session, owner, now, expanded);
            if (expanded)
            {
                content->add_gap(6.0f);
                height += 6.0f;
                if (!session.name.empty())
                    height += add_wrapped(*content, owner + u8"-name", u8"대화 · " + u8(session.name), inner_width, 11.0f, 4, luil::label_color_role::dim);
                if (!session.observation_source.empty())
                    height += add_wrapped(*content, owner + u8"-source-detail", u8"수집 · " + source_text(session), inner_width, 11.0f, 2, luil::label_color_role::dim);
                height += add_wrapped(*content, owner + u8"-folder", u8"작업 폴더 · " + (session.cwd.empty() ? u8"정보 없음" : u8(session.cwd)), inner_width, 11.0f, 8, luil::label_color_role::dim);
                height += add_wrapped(*content, owner + u8"-identity", u8"세션 · " + u8(session.session_id), inner_width, 11.0f, 4, luil::label_color_role::dim);
            }
            luil::panel_config surface {};
            surface.background = [expanded](const luil::ui_color_palette& palette) { return expanded ? palette.row_selection_background : luil::with_alpha(palette.surface_background, 0.0f); };
            auto row { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, owner }, surface) };
            row->set_content(std::move(content));
            row->set_hit_opaque(true);
            row->set_cursor(luil::ui_cursor::hand);
            row->set_access_name(provider_text(session) + u8" · " + state_text(session, now) + u8" · " + title);
            row->set_tooltip(title + u8"\n" + u8(session.cwd) + u8"\n클릭해서 " + (expanded ? u8"접기" : u8"펼치기"));
            row->set_action(luil::ui_trigger::left_click, luil::make_message_action(expand_session_intent { owner }));
            return { std::move(row), height };
        }

        // 묶음 머리글이다: 키 컬러 막대와 강조 글자, 옅은 키 컬러 바탕. 채운 머릿줄과도 행과도 갈린다.
        std::unique_ptr<luil::ui_element> widget_group_heading(const std::u8string& id, const std::u8string& name, const std::size_t count, const std::u8string& hint)
        {
            const luil::stack_config config {
                .direction = luil::stack_direction::row, .spacing = 6.0f, .padding = { widget_row_inset, 0.0f, widget_row_inset, 0.0f }, .cross_alignment = luil::stack_alignment::center
            };
            auto content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-content" }, config) };
            content->add(fill(id + u8"-mark", [](const luil::ui_color_palette& palette) { return palette.accent; }, 1.5f), { .length = 3.0f, .cross_length = 12.0f });
            const luil::stack_config name_config { .direction = luil::stack_direction::row, .spacing = 6.0f, .cross_alignment = luil::stack_alignment::center };
            auto names { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-names" }, name_config) };
            names->add_fit(label(id, name, 12.0f));
            names->add_fit(label(id + u8"-count", u8(std::to_string(count)), 11.0f));
            content->add_fit(std::make_unique<palette_scope>(id + u8"-tint", heading_tint, std::move(names)));
            content->add_flexible(label(id + u8"-hint", hint, 11.0f, luil::label_color_role::dim, luil::text_align::end));
            luil::panel_config surface {};
            surface.background = [](const luil::ui_color_palette& palette) { return luil::with_alpha(palette.accent_soft, 0.12f); };
            auto result { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, id + u8"-surface" }, surface) };
            result->set_content(std::move(content));
            return result;
        }
    } // namespace

    luil::caption_config board_caption()
    {
        luil::caption_config caption {};
        caption.title = u8"Session Board";
        caption.minimize_tooltip = u8"최소화";
        caption.maximize_tooltip = u8"최대화 / 복원";
        caption.close_tooltip = u8"닫기";
        return caption;
    }

    luil::caption_config detail_caption(const bool dark)
    {
        auto caption { board_caption() };
        caption.title = u8"Session Board · 상세";
        caption.close_tooltip = u8"상세 화면 닫기";
        caption.app_buttons.push_back({ .key = u8"theme", .glyph = luil::codicons::icon_color_mode, .tooltip = dark ? u8"밝게" : u8"어둡게", .action = luil::make_message_action(theme_intent {}) });
        caption.app_buttons.push_back({ .key = u8"refresh", .glyph = luil::codicons::icon_refresh, .tooltip = u8"새로 고침", .action = luil::make_message_action(refresh_intent {}) });
        return caption;
    }

    board_driver::board_driver(std::filesystem::path directory, const bool open_detail, std::filesystem::path codex_directory, std::filesystem::path claude_directory,
        std::optional<claude_hook_result> claude_hooks, std::optional<codex_hook_result> codex_hooks, std::filesystem::path detail_placement_file)
        : directory_ { std::move(directory) }
        , codex_directory_ { std::move(codex_directory) }
        , claude_directory_ { std::move(claude_directory) }
        , claude_hooks_ { std::move(claude_hooks) }
        , codex_hooks_ { std::move(codex_hooks) }
        , detail_open_ { open_detail }
        , detail_placement_file_ { std::move(detail_placement_file) }
    {
        if (!detail_placement_file_.empty())
            detail_placement_ = load_window_placement(detail_placement_file_);
        // 프레임을 지우는 창 바탕을 투명하게 둔다. 색은 남겨 두어 상세 창이 알파만 되돌려 불투명 바탕을 깐다.
        auto style { std::make_shared<luil::ui_style>(luil::default_ui_style()) };
        style->dark.window_background = luil::with_alpha(style->dark.window_background, 0.0f);
        style->light.window_background = luil::with_alpha(style->light.window_background, 0.0f);
        style_ = std::move(style);
    }

    board_driver::~board_driver()
    {
        if (changes_ != INVALID_HANDLE_VALUE)
            FindCloseChangeNotification(changes_);
    }

    void board_driver::start()
    {
        if (!codex_directory_.empty())
            codex_ = std::make_unique<codex_monitor>(codex_directory_);
        if (!claude_directory_.empty())
        {
            claude_ = std::make_unique<claude_monitor>(claude_directory_);
            desktop_ = std::make_unique<claude_desktop_index>(default_claude_desktop_roots());
        }
        try
        {
            std::filesystem::create_directories(directory_);
            changes_ = FindFirstChangeNotificationW(directory_.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
            reload();
        }
        catch (...)
        {
            error_ = "Cannot open the session data directory.";
        }
        next_tick_ = std::chrono::steady_clock::now() + std::chrono::seconds { 1 };
    }

    void board_driver::reload()
    {
        try
        {
            data_ = load_sessions(directory_);
            error_.clear();
        }
        catch (...)
        {
            error_ = "Cannot read session data. Check the data directory.";
        }
        if (codex_ != nullptr)
        {
            codex_data_ = codex_->snapshot();
            merge_codex_sessions(data_.sessions, codex_data_.sessions);
        }
        if (claude_ != nullptr)
        {
            claude_data_ = claude_->snapshot();
            merge_claude_sessions(data_.sessions, claude_data_.sessions);
        }
        if (desktop_ != nullptr)
        {
            // 데스크톱 기록은 대화가 이어지는 동안 자주 바뀐다. 바뀐 파일만 읽지만 훑는 것도 몇 초에 한 번이면 된다.
            const auto now { std::chrono::steady_clock::now() };
            if (now >= desktop_refresh_)
            {
                desktop_->refresh();
                desktop_refresh_ = now + std::chrono::seconds { 3 };
            }
            for (auto& session : data_.sessions)
                if (session.provider == "claude")
                    session.desktop_session_id = desktop_->find(session.session_id);
        }
    }

    void board_driver::handle(luil::app_message message)
    {
        if (message.get<close_intent>() != nullptr)
            closed_.store(true);
        else if (const auto* metrics { message.get<metrics_intent>() }; metrics != nullptr)
            metrics_ = *metrics;
        else if (message.get<refresh_intent>() != nullptr)
            reload();
        else if (const auto* filter { message.get<provider_intent>() }; filter != nullptr)
        {
            provider_ = filter->provider;
            detail_scroll_ = 0.0f;
        }
        else if (message.get<working_intent>() != nullptr)
        {
            working_only_ = !working_only_;
            detail_scroll_ = 0.0f;
        }
        else if (message.get<theme_intent>() != nullptr)
            dark_ = !dark_;
        else if (const auto* mode { message.get<theme_mode_intent>() }; mode != nullptr)
            dark_ = mode->dark;
        else if (const auto* accent { message.get<accent_intent>() }; accent != nullptr)
            accent_id_ = accent->id;
        else if (message.get<widget_failed_intent>() != nullptr)
            error_ = "The widget window could not be prepared.";
        else if (const auto* detail { message.get<open_detail_intent>() }; detail != nullptr)
        {
            detail_open_ = !detail_open_ || !detail->key.empty();
            if (!detail->key.empty())
                selected_session_ = detail->key;
        }
        else if (message.get<close_detail_intent>() != nullptr)
            detail_open_ = false;
        else if (const auto* detail_metrics { message.get<detail_metrics_intent>() }; detail_metrics != nullptr)
            detail_metrics_ = *detail_metrics;
        else if (const auto* backdrop { message.get<backdrop_intent>() }; backdrop != nullptr)
            backdrop_ = backdrop->backdrop;
        else if (const auto* opacity { message.get<opacity_intent>() }; opacity != nullptr)
            opacity_ = std::clamp(opacity->value, 0.0f, 1.0f);
        // 설정 줄은 한 번에 하나만 펼친다. 둘이 함께 위젯 높이를 잡아먹지 않게 한다.
        else if (message.get<backdrop_settings_intent>() != nullptr)
        {
            backdrop_settings_open_ = !backdrop_settings_open_;
            theme_settings_open_ = false;
        }
        else if (message.get<theme_settings_intent>() != nullptr)
        {
            theme_settings_open_ = !theme_settings_open_;
            backdrop_settings_open_ = false;
        }
        else if (const auto* placement { message.get<detail_placement_intent>() }; placement != nullptr)
        {
            detail_placement_ = placement->placement;
            if (!detail_placement_file_.empty())
                save_window_placement(detail_placement_file_, placement->placement);
        }
        else if (const auto* focus { message.get<focus_session_intent>() }; focus != nullptr)
            focus_session(focus->key);
        else if (const auto* widget { message.get<widget_state_intent>() }; widget != nullptr)
        {
            collapsed_ = widget->collapsed;
            pinned_ = widget->pinned;
        }
        else if (const auto* expand { message.get<expand_session_intent>() }; expand != nullptr)
            expanded_session_ = expanded_session_ == expand->owner ? std::u8string {} : expand->owner;
        else if (const auto* select { message.get<select_session_intent>() }; select != nullptr)
            selected_session_ = select->key;
        else if (const auto* folder { message.get<open_folder_intent>() }; folder != nullptr)
        {
            // 탐색기를 띄우지 못해도 화면은 그대로 둔다. 폴더가 지워졌을 수 있다.
            const std::filesystem::path path { folder->path };
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (const auto* scroll { message.get<scroll_intent>() }; scroll != nullptr)
        {
            float& offset { scroll->detail ? detail_scroll_ : scroll_ };
            offset = std::max(0.0f, scroll->absolute ? scroll->value : offset + scroll->value);
        }
    }

    void board_driver::focus_session(const std::u8string& key)
    {
        const auto found { std::ranges::find_if(data_.sessions, [&](const auto& session) { return session_key(session) == key; }) };
        notice_until_ = std::chrono::steady_clock::now() + std::chrono::seconds { 8 };
        if (found == data_.sessions.end())
            notice_ = u8"세션을 찾지 못했습니다. 목록이 바뀌었을 수 있습니다.";
        // 데스크톱 세션은 창을 앞으로 가져와도 다른 세션이 보일 수 있어 앱에 그 세션을 열라고 한다.
        else if (const auto link { desktop_link(*found) }; !link.empty())
        {
            const auto result { reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", link.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) };
            if (result > 32)
                notice_.clear();
            else
                notice_ = provider_text(*found) + u8" 앱에서 세션을 열지 못했습니다.";
        }
        else if (found->host_window == 0)
            notice_ = u8"이 세션의 창 정보가 없습니다 — Hook으로 받은 세션과 데스크톱 앱 세션만 이동할 수 있습니다.";
        else if (!focus_host_window({ .window = found->host_window, .process_id = found->host_process_id }))
            notice_ = u8"세션 창을 찾지 못했습니다 — 창이 닫혔을 수 있습니다.";
        else
            notice_.clear();
    }

    std::optional<std::chrono::steady_clock::time_point> board_driver::next_tick()
    {
        return next_tick_;
    }

    void board_driver::tick(const std::chrono::steady_clock::time_point now)
    {
        if (changes_ == INVALID_HANDLE_VALUE || codex_ != nullptr || claude_ != nullptr)
            reload();
        else if (WaitForSingleObject(changes_, 0) == WAIT_OBJECT_0)
        {
            FindNextChangeNotification(changes_);
            reload();
        }
        next_tick_ = now + std::chrono::seconds { 1 };
    }

    std::shared_ptr<const luil::ui_frame> board_driver::make_frame()
    {
        const auto now { now_ms() };
        if (!notice_.empty() && std::chrono::steady_clock::now() >= notice_until_)
            notice_.clear();
        auto frame { std::make_shared<luil::ui_frame>() };
        frame->appearance.theme = dark_ ? luil::theme_preference::dark : luil::theme_preference::light;
        frame->appearance.accent_id = accent_id_;
        frame->style = style_;
        frame->tree = std::make_shared<const luil::ui_tree>(make_widget_tree(now));
        if (detail_open_)
        {
            // 위젯으로 지켜보다가 자세히 볼 때 여는 독립 창이다. 작업 표시줄에 따로 선다.
            luil::ui_window detail {};
            detail.id = u8"detail";
            detail.caption = detail_caption(dark_);
            detail.x = -1140.0f;
            detail.width = 1120.0f;
            detail.height = 800.0f;
            detail.minimum_width = 820.0f;
            detail.minimum_height = 560.0f;
            detail.independent = true;
            // 만들 때만 쓰인다. 닫았다 다시 열어도, 다음 실행에서도 마지막 자리로 연다.
            detail.initial_placement = detail_placement_;
            detail.placement = [](const luil::win32::window_placement& placement) { return luil::app_message { detail_placement_intent { placement } }; };
            detail.tree = std::make_shared<const luil::ui_tree>(make_detail_tree(now));
            detail.metrics = [](const float width, const float height, const float scale) { return luil::app_message { detail_metrics_intent { width, height, scale } }; };
            detail.close = [] { return luil::make_app_action(close_detail_intent {}); };
            frame->windows.push_back(std::move(detail));
        }
        return frame;
    }

    std::unique_ptr<luil::root_element> board_driver::make_detail_tree(const std::int64_t now)
    {
        const float scale { detail_metrics_.scale > 0.0f ? detail_metrics_.scale : (metrics_.scale > 0.0f ? metrics_.scale : 1.0f) };
        const float width { detail_metrics_.width > 0.0f ? detail_metrics_.width : 1120.0f };
        const float height { detail_metrics_.height > 0.0f ? detail_metrics_.height : 800.0f };
        auto root { std::make_unique<luil::root_element>() };
        root->arrange({ { 0.0f, 0.0f, width, height }, scale });
        // 프레임은 투명하게 지워지므로 상세 창은 창 바탕을 불투명하게 다시 깐다.
        auto ground { fill(u8"detail-ground", [](const luil::ui_color_palette& palette) { return luil::with_alpha(palette.window_background, 1.0f); }) };
        ground->arrange({ { 0.0f, 0.0f, width, height }, scale });
        root->add(std::move(ground));
        // 창 전체의 명령은 caption의 앱 단추로 둔다. 본문은 세션만 다룬다.
        const auto caption_config { detail_caption(dark_) };
        const float caption_height { luil::caption_element::height_for(caption_config) * scale };
        auto caption { std::make_unique<luil::caption_element>(caption_config) };
        caption->arrange({ { 0.0f, 0.0f, width, caption_height }, scale });
        root->add(std::move(caption));

        std::array<std::size_t, 3> provider_counts {};
        std::size_t attention { 0 };
        std::size_t running { 0 };
        for (const auto& session : data_.sessions)
        {
            ++provider_counts[0];
            ++provider_counts[session.provider == "codex" ? 1 : 2];
            const auto group { group_of(session, now) };
            attention += group == session_group::attention ? 1 : 0;
            running += group == session_group::running ? 1 : 0;
        }

        auto body { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"body" }, luil::stack_config {}) };
        const luil::stack_config toolbar_config {
            .direction = luil::stack_direction::row, .spacing = 12.0f, .padding = { row_inset, 0.0f, row_inset, 0.0f }, .cross_alignment = luil::stack_alignment::center
        };
        auto toolbar { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"toolbar" }, toolbar_config) };
        // 값은 `filter-` 뒤에 provider를 붙인 것이다. UIA 검증이 이 이름으로 필터를 찾는다.
        luil::choice_group_config filter { .owner = u8"provider-filter", .style = luil::choice_style::toggle, .name = u8"AI 종류" };
        filter.items.push_back({ u8"filter-all", u8"전체 " + u8(std::to_string(provider_counts[0])) });
        filter.items.push_back({ u8"filter-codex", u8"Codex " + u8(std::to_string(provider_counts[1])) });
        filter.items.push_back({ u8"filter-claude", u8"Claude " + u8(std::to_string(provider_counts[2])) });
        filter.selected = provider_.empty() ? u8"filter-all" : u8"filter-" + u8(provider_);
        filter.select = [](const std::u8string& value) {
            const std::u8string_view name { std::u8string_view { value }.substr(7) };
            return luil::make_app_action(provider_intent { name == u8"all" ? std::string {} : std::string { name.begin(), name.end() } });
        };
        toolbar->add(std::make_unique<luil::choice_group_element>(std::move(filter)), { .length = filter_width, .cross_length = luil::choice_toggle_height });
        toolbar->add_fit(std::make_unique<luil::check_element>(
            luil::check_config { .owner = u8"filter-working", .label = u8"작업 중만", .checked = working_only_, .toggle = luil::make_message_action(working_intent {}) }));
        toolbar->add_flexible_gap();
        const auto add_count = [&](const std::u8string& id, const std::u8string& text, const bool warning) {
            toolbar->add(
                fill(id + u8"-dot", [warning](const luil::ui_color_palette& palette) { return warning ? palette.warning_accent : palette.accent; }, 4.0f), { .length = 8.0f, .cross_length = 8.0f });
            toolbar->add_fit(label(id, text, 11.0f, luil::label_color_role::dim));
        };
        add_count(u8"attention-count", u8"확인 필요 " + u8(std::to_string(attention)), true);
        add_count(u8"running-count", u8"진행 중 " + u8(std::to_string(running)), false);
        body->add(std::move(toolbar), toolbar_height);
        body->add(divider(u8"toolbar-rule"), 1.0f);

        std::vector<const session_snapshot*> visible {};
        for (const auto& session : data_.sessions)
            if ((provider_.empty() || provider_ == session.provider) && (!working_only_ || is_working(session.status) || working_children(session) > 0))
                visible.push_back(&session);
        std::ranges::stable_sort(visible, [now](const auto* left, const auto* right) { return group_of(*left, now) < group_of(*right, now); });
        // 고른 세션이 걸러져 사라지면 맨 위 세션을 보인다. 선택은 앱 상태라 다시 보이면 되돌아온다.
        //  - 하위 행을 골랐으면 그 하위와 부모를 함께 기억한다.
        const session_snapshot* detail { nullptr };
        const session_child* detail_child { nullptr };
        for (const auto* session : visible)
        {
            if (session_key(*session) == selected_session_)
                detail = session;
            for (const auto& child : session->children)
                if (child_key(*session, child) == selected_session_)
                {
                    detail = session;
                    detail_child = &child;
                }
        }
        if (detail == nullptr && !visible.empty())
            detail = visible.front();

        const float body_height { (height - caption_height) / scale - toolbar_height - status_bar_height - 2.0f };
        auto content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"content" }, luil::stack_config { .direction = luil::stack_direction::row }) };
        luil::scroll_area_element* scroll_metrics { nullptr };
        if (visible.empty())
        {
            const luil::stack_config config { .spacing = 6.0f, .padding = luil::edge_insets::all(24.0f), .main_alignment = luil::stack_main_alignment::center };
            auto empty { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"empty-body" }, config) };
            empty->add(
                label(u8"empty-title", data_.sessions.empty() ? u8"아직 수신한 세션이 없습니다" : u8"이 조건에 맞는 세션이 없습니다", 18.0f, luil::label_color_role::primary, luil::text_align::center),
                28.0f);
            empty->add(
                label(u8"empty-help", data_.sessions.empty() ? u8"열린 Codex·Claude 세션이 없습니다. 새 대화를 시작하거나 요청을 보내면 표시됩니다." : u8"필터를 바꾸거나 작업 중만 보기를 끄세요.",
                    12.0f, luil::label_color_role::dim, luil::text_align::center),
                20.0f);
            content->add_flexible(panel(u8"empty", std::move(empty)));
            detail_scroll_ = 0.0f;
        }
        else
        {
            auto rows { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"rows" }, luil::stack_config {}) };
            float content_height { 0.0f };
            std::size_t rule_count { 0 };
            const auto add_row = [&](std::unique_ptr<luil::ui_element> element, const float length) {
                rows->add(std::move(element), length);
                rows->add(divider(u8"rule-" + u8(std::to_string(++rule_count))), 1.0f);
                content_height += length + 1.0f;
            };
            for (const auto group : { session_group::attention, session_group::running, session_group::other })
            {
                const auto heading { heading_of(group) };
                const auto count { std::ranges::count_if(visible, [&](const auto* session) { return group_of(*session, now) == group; }) };
                if (count == 0)
                    continue;
                add_row(group_header(heading.id, heading.name, static_cast<std::size_t>(count), heading.hint), group_header_height);
                for (const auto* session : visible)
                    if (group_of(*session, now) == group)
                    {
                        add_row(session_row(*session, now, session == detail && detail_child == nullptr), session_row_height);
                        // 하위는 묶음과 관계없이 부모를 따라간다.
                        for (const auto* child : ordered_children(*session, now))
                            add_row(child_row(*session, *child, now, child == detail_child), child_row_height);
                    }
            }
            std::unique_ptr<luil::ui_element> list { std::move(rows) };
            if (content_height > body_height)
            {
                luil::scroll_area_config config {};
                config.owner = u8"session-scroll";
                config.content_height = content_height;
                config.scroll_offset = detail_scroll_;
                config.scroll = [](const float delta) { return luil::make_app_action(scroll_intent { delta, false, true }); };
                config.scroll_to = [](const float offset) { return luil::make_app_action(scroll_intent { offset, true, true }); };
                auto scroll { std::make_unique<luil::scroll_area_element>(config) };
                scroll->set_content(std::move(list));
                scroll_metrics = scroll.get();
                list = std::move(scroll);
            }
            else
                detail_scroll_ = 0.0f;
            auto surface { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, u8"list" }, luil::panel_config {}) };
            surface->set_content(std::move(list));
            content->add_flexible(std::move(surface));
            content->add(divider(u8"detail-divider"), 1.0f);
            content->add(detail_child != nullptr ? child_detail_pane(*detail, *detail_child, now) : detail_pane(*detail, now, body_height), detail_width);
        }
        body->add(std::move(content), body_height);
        body->add(divider(u8"status-rule"), 1.0f);

        // 상태 표시줄: provider별 마지막 수신, 오류는 같은 자리에서 오류색으로 말한다.
        const luil::stack_config status_config {
            .direction = luil::stack_direction::row, .spacing = 16.0f, .padding = { row_inset, 0.0f, row_inset, 0.0f }, .cross_alignment = luil::stack_alignment::center
        };
        auto status { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"status-bar" }, status_config) };
        if (!notice_.empty() || !error_.empty() || data_.invalid_files > 0)
        {
            std::u8string problem { u8(error_) };
            if (!notice_.empty())
                problem = notice_;
            else if (error_.empty())
                problem = u8"읽지 못한 세션 파일 " + u8(std::to_string(data_.invalid_files)) + u8"개 · 정상 세션은 계속 표시합니다.";
            status->add_fit(label(u8"notice", problem, 11.0f, luil::label_color_role::error));
        }
        else
        {
            const auto signal { signal_text(now) };
            const auto split { signal.find(u8'\n') };
            status->add_fit(provider_line(u8"signal-codex", "codex", signal.substr(0, split), 11.0f, luil::label_color_role::dim, true));
            if (split != std::u8string::npos)
                status->add_fit(provider_line(u8"signal-claude", "claude", signal.substr(split + 1), 11.0f, luil::label_color_role::dim, true));
        }
        std::u8string location { directory_.u8string() + u8" · 세션 " + u8(std::to_string(data_.sessions.size())) + u8"개" };
        if (data_.truncated)
            location += u8" · 최근 512개 표시";
        status->add_flexible(label(u8"data-path", std::move(location), 11.0f, luil::label_color_role::dim, luil::text_align::end, luil::text_elide::middle));
        body->add(std::move(status), status_bar_height);

        body->arrange({ { 0.0f, caption_height, width, height - caption_height }, scale });
        if (scroll_metrics != nullptr)
            detail_scroll_ = scroll_metrics->metrics().scroll_offset;
        root->add(std::move(body));
        return root;
    }

    std::unique_ptr<luil::root_element> board_driver::make_widget_tree(const std::int64_t now)
    {
        constexpr float caption_height { static_cast<float>(widget_caption_height) };
        const float scale { metrics_.scale > 0.0f ? metrics_.scale : 1.0f };
        const float width { metrics_.width > 0.0f ? metrics_.width : static_cast<float>(widget_width) * scale };
        const float height { metrics_.height > 0.0f ? metrics_.height : static_cast<float>(widget_height) * scale };
        const float inner_width { width / scale };
        auto root { std::make_unique<luil::root_element>() };
        root->arrange({ { 0.0f, 0.0f, width, height }, scale });

        std::vector<const session_snapshot*> visible {};
        for (const auto& session : data_.sessions)
            if (is_widget_visible(session, now))
                visible.push_back(&session);
        // 응답이 끝났어도 하위가 돌면 진행 중 묶음에 둔다.
        const auto active { [](const session_snapshot* session) { return (!session->turn_closed && session->status != session_status::ended) || working_children(*session) > 0; } };
        std::ranges::stable_sort(visible, [&](const auto* left, const auto* right) { return active(left) > active(right); });
        const auto attention { std::ranges::count_if(visible, [now](const auto* session) { return group_of(*session, now) == session_group::attention; }) };
        const auto running { std::ranges::count_if(visible, [now](const auto* session) { return group_of(*session, now) == session_group::running; }) };

        // 머릿줄과 바닥 줄이 창 너비 전체를 덮고 그 사이를 목록 영역이 채운다. 창 모서리는 DWM이 둥글게 자른다.
        auto content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-content" }, luil::stack_config {}) };

        // 머릿줄: 키 컬러로 채워 목록과 가른다. 빈 자리를 잡아 창을 옮긴다 (widget_window의 HTCAPTION).
        const luil::stack_config caption_config { .direction = luil::stack_direction::row, .spacing = 2.0f, .padding = { 12.0f, 0.0f, 8.0f, 0.0f }, .cross_alignment = luil::stack_alignment::center };
        auto caption { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-header" }, caption_config) };
        caption->add(std::make_unique<luil::glyph_element>(luil::ui_element_id { kind_label, u8"widget-grip" }, luil::glyph_config { .glyph = luil::codicons::icon_gripper, .font_size = 14.0f }),
            { .length = 16.0f, .cross_length = 16.0f });
        caption->add_gap(4.0f);
        caption->add_flexible(label(u8"widget-title", u8"Session Board", 14.0f));
        if (inner_width >= 340.0f)
            caption->add_fit(label(u8"widget-count", u8"확인 " + u8(std::to_string(attention)) + u8" · 진행 " + u8(std::to_string(running)), 11.0f, luil::label_color_role::dim));
        caption->add_gap(4.0f);
        const auto add_button = [&](std::unique_ptr<luil::button_element> button) { caption->add(std::move(button), { .length = widget_icon_button, .cross_length = widget_icon_button }); };
        add_button(widget_button(
            u8"widget-detail", detail_open_ ? u8"상세 화면 닫기" : u8"상세 화면 열기", luil::codicons::icon_editor_layout, luil::make_message_action(open_detail_intent {}), detail_open_));
        add_button(widget_button(u8"widget-theme", u8"테마 · 키 컬러", luil::codicons::icon_color_mode, luil::make_message_action(theme_settings_intent {}), theme_settings_open_));
        add_button(widget_button(u8"widget-backdrop", u8"배경 효과 · 진하기", luil::codicons::icon_symbol_color, luil::make_message_action(backdrop_settings_intent {}), backdrop_settings_open_));
        add_button(widget_button(u8"widget-pin", pinned_ ? u8"항상 위에 표시 해제" : u8"항상 위에 표시", luil::codicons::icon_pin, command_action(toggle_pin_command), pinned_));
        add_button(widget_button(u8"widget-collapse", collapsed_ ? u8"목록 펼치기" : u8"목록 접기", collapsed_ ? luil::codicons::icon_chevron_down : luil::codicons::icon_chevron_up,
            command_action(toggle_collapse_command)));
        add_button(widget_button(u8"compact-close", u8"위젯 닫기", luil::codicons::icon_chrome_close, command_action(0)));
        luil::panel_config caption_surface {};
        caption_surface.background = [](const luil::ui_color_palette& palette) { return palette.accent; };
        auto caption_panel { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, u8"widget-caption" }, caption_surface) };
        caption_panel->set_content(std::make_unique<palette_scope>(u8"widget-caption-tint", caption_tint, std::move(caption)));
        content->add(std::move(caption_panel), caption_height);

        luil::scroll_area_element* scroll_metrics { nullptr };
        if (!collapsed_)
        {
            const luil::stack_config region_config { .spacing = widget_gap, .padding = luil::edge_insets::all(widget_gap) };
            auto region { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-region-body" }, region_config) };
            float used { caption_height + 1.0f + widget_footer_height + 2.0f * widget_gap };
            const luil::stack_config row_config { .direction = luil::stack_direction::row, .spacing = 8.0f, .cross_alignment = luil::stack_alignment::center };
            if (theme_settings_open_)
            {
                // 밝기와 키 컬러다. 키 컬러는 앱 전체 외양이라 상세 창도 함께 바뀐다.
                const luil::stack_config settings_config { .spacing = 4.0f, .padding = { 10.0f, 6.0f, 10.0f, 10.0f } };
                auto settings { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-theme-settings" }, settings_config) };
                luil::choice_group_config mode { .owner = u8"widget-theme-mode", .style = luil::choice_style::toggle, .name = u8"테마" };
                mode.items.push_back({ u8"theme-light", u8"밝게" });
                mode.items.push_back({ u8"theme-dark", u8"어둡게" });
                mode.selected = dark_ ? u8"theme-dark" : u8"theme-light";
                mode.select = [](const std::u8string& value) { return luil::make_app_action(theme_mode_intent { value == u8"theme-dark" }); };
                auto mode_row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-theme-row" }, row_config) };
                mode_row->add(label(u8"widget-theme-name", u8"테마", 11.0f, luil::label_color_role::dim), 40.0f);
                mode_row->add(std::make_unique<luil::choice_group_element>(std::move(mode)), { .length = widget_theme_choice_width, .cross_length = luil::choice_toggle_height });
                mode_row->add_flexible_gap();
                settings->add(std::move(mode_row), widget_setting_row);
                settings->add(label(u8"widget-accent-name", u8"키 컬러", 11.0f, luil::label_color_role::dim), 16.0f);
                luil::accent_picker_config accents { .owner = u8"widget-accent", .selected = accent_id_, .name = u8"키 컬러" };
                accents.select = [](const std::u8string& id) { return luil::make_app_action(accent_intent { id }); };
                // 견본 줄 수는 폭으로 정해지므로 실제로 받을 폭(목록 영역 여백·설정 여백·테두리를 뺀 값)으로 높이를 잰다.
                const float picker_width { inner_width - 2.0f * widget_gap - 20.0f - 2.0f };
                const float picker_height { luil::accent_picker_element::height_for(accents, picker_width) };
                settings->add(std::make_unique<luil::accent_picker_element>(std::move(accents)), picker_height);
                const float settings_height { 16.0f + widget_setting_row + 4.0f + 16.0f + 4.0f + picker_height };
                region->add(panel(u8"widget-theme-surface", std::move(settings)), settings_height);
                used += settings_height + widget_gap;
            }
            if (backdrop_settings_open_)
            {
                // 흐림과 진하기다. 팝업을 띄우지 않고 머릿줄 아래에 펼친다 — 위젯 창은 바탕이 투명하다.
                const luil::stack_config settings_config { .spacing = 4.0f, .padding = { 10.0f, 6.0f, 10.0f, 6.0f } };
                auto settings { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-settings" }, settings_config) };
                // DWM 합성은 창을 가진 UI thread에서 바꿔야 하므로 스위치도 앱 UI 명령으로 답한다.
                const bool blur { backdrop_ == widget_backdrop::blur };
                luil::check_config effect { .owner = u8"widget-blur", .style = luil::check_style::toggle_switch, .label = u8"흐림", .checked = blur };
                effect.toggle = command_action(blur ? backdrop_transparent_command : backdrop_blur_command);
                // 100%에서는 비칠 자리가 없어 스위치가 하는 일이 없다. 값은 남겨 두고 진하기를 내리면 다시 따른다.
                const bool opaque { fully_opaque(opacity_) };
                effect.tooltip = opaque ? u8"진하기가 100%라 뒤가 비치지 않습니다" : u8"끄면 뒤가 흐리지 않고 그대로 비칩니다";
                auto effect_check { std::make_unique<luil::check_element>(std::move(effect)) };
                effect_check->set_enabled(!opaque);
                auto effect_row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-backdrop-row" }, row_config) };
                effect_row->add(label(u8"widget-backdrop-name", u8"배경", 11.0f, luil::label_color_role::dim), 40.0f);
                effect_row->add_flexible(std::move(effect_check));
                settings->add(std::move(effect_row), widget_setting_row);
                // 끌면 자석 지점에 붙고, Ctrl을 누른 채 끌면 붙지 않는다.
                //  - 값은 위젯 창이 먼저 받는다. 100%를 넘나들 때 DWM 합성을 그 창의 thread에서 바꿔야 한다.
                luil::slider_config level { .minimum = 0.0f, .maximum = 1.0f, .value = opacity_ };
                level.change = [](const float delta) { return luil::input_action { luil::app_ui_command { opacity_change_command, number_text(delta) } }; };
                level.change_to = [](const float value) { return luil::input_action { luil::app_ui_command { opacity_command, number_text(value) } }; };
                level.magnets.assign(widget_opacity_magnets.begin(), widget_opacity_magnets.end());
                auto slider { std::make_unique<luil::slider_element>(luil::ui_element_id { kind_button, u8"widget-opacity" }, std::move(level)) };
                // 슬라이더는 보조 기술 이름을 tooltip에서만 읽는다.
                slider->set_tooltip(u8"진하기");
                auto level_row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-opacity-row" }, row_config) };
                level_row->add(label(u8"widget-opacity-name", u8"진하기", 11.0f, luil::label_color_role::dim), 40.0f);
                level_row->add_flexible(std::move(slider));
                level_row->add(label(u8"widget-opacity-value", u8(std::to_string(std::lround(opacity_ * 100.0f))) + u8"%", 11.0f, luil::label_color_role::dim, luil::text_align::end), 36.0f);
                settings->add(std::move(level_row), widget_setting_row);
                constexpr float settings_height { 12.0f + 2.0f * widget_setting_row + 4.0f };
                region->add(panel(u8"widget-settings-surface", std::move(settings)), settings_height);
                used += settings_height + widget_gap;
            }

            // 목록 영역: 진하기만큼의 바탕만 깐다. 흐림·투명이면 그 뒤로 바탕 화면이 비친다.
            auto rows { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-rows" }, luil::stack_config { .spacing = widget_gap }) };
            float content_height { 0.0f };
            const auto add_block = [&](std::unique_ptr<luil::ui_element> block, const float block_height) {
                if (content_height > 0.0f)
                    content_height += widget_gap;
                rows->add(std::move(block), block_height);
                content_height += block_height;
            };
            const float row_width { inner_width - 2.0f * widget_gap - 2.0f };
            if (visible.empty())
            {
                const luil::stack_config empty_config { .spacing = 4.0f, .padding = luil::edge_insets::all(16.0f), .main_alignment = luil::stack_main_alignment::center };
                auto empty { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-empty-body" }, empty_config) };
                empty->add(
                    label(u8"widget-empty", data_.sessions.empty() ? u8"아직 수신한 세션이 없습니다" : u8"진행 중인 세션이 없습니다", 14.0f, luil::label_color_role::primary, luil::text_align::center),
                    22.0f);
                empty->add(label(u8"widget-empty-guide", u8"새 요청을 보내면 여기에 나타납니다.", 11.0f, luil::label_color_role::dim, luil::text_align::center), 18.0f);
                add_block(panel(u8"widget-empty-surface", std::move(empty)), 80.0f);
            }
            for (const bool current : { true, false })
            {
                std::vector<const session_snapshot*> members {};
                for (const auto* session : visible)
                    if (active(session) == current)
                        members.push_back(session);
                if (members.empty())
                    continue;
                const std::u8string group_id { current ? u8"widget-active-heading" : u8"widget-recent-heading" };
                auto group { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, group_id + u8"-rows" }, luil::stack_config {}) };
                float group_height { widget_group_header };
                group->add(widget_group_heading(group_id, current ? u8"진행 중 · 대기" : u8"최근 완료 · 종료", members.size(), current ? u8"" : u8"15분간 유지"), widget_group_header);
                for (const auto* session : members)
                {
                    group->add(divider(widget_owner(*session) + u8"-rule"), 1.0f);
                    auto row { widget_row(*session, now, row_width, expanded_session_ == widget_owner(*session)) };
                    group->add(std::move(row.element), row.height);
                    group_height += 1.0f + row.height;
                }
                add_block(panel(group_id + u8"-surface", std::move(group)), group_height + 2.0f);
            }

            const float viewport_height { std::max(0.0f, height / scale - used) };
            if (content_height > viewport_height)
            {
                luil::scroll_area_config config {};
                config.owner = u8"widget-scroll";
                config.content_height = content_height;
                config.scroll_offset = scroll_;
                config.scroll = [](const float delta) { return luil::make_app_action(scroll_intent { delta, false }); };
                config.scroll_to = [](const float value) { return luil::make_app_action(scroll_intent { value, true }); };
                auto scroll { std::make_unique<luil::scroll_area_element>(config) };
                scroll->set_content(std::move(rows));
                scroll_metrics = scroll.get();
                region->add_flexible(std::move(scroll));
            }
            else
            {
                scroll_ = 0.0f;
                region->add(std::move(rows), content_height);
                region->add_flexible_gap();
            }

            luil::panel_config region_surface {};
            region_surface.background = [alpha = fully_opaque(opacity_) ? 1.0f : opacity_](const luil::ui_color_palette& palette) { return luil::with_alpha(palette.window_background, alpha); };
            auto region_panel { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, u8"widget-region" }, region_surface) };
            region_panel->set_content(std::move(region));
            content->add_flexible(std::move(region_panel));

            // 바닥 줄: provider별 마지막 수신이다. 알릴 일·오류가 있으면 같은 자리에서 오류색으로 말한다.
            // 오른쪽 끝은 크기 조절 표시 자리로 비운다.
            const luil::stack_config footer_config {
                .direction = luil::stack_direction::row, .spacing = 12.0f, .padding = { 12.0f, 0.0f, 18.0f, 0.0f }, .cross_alignment = luil::stack_alignment::center
            };
            auto footer { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-footer-body" }, footer_config) };
            if (!notice_.empty() || !error_.empty() || data_.invalid_files > 0)
            {
                const std::u8string problem { !notice_.empty() ? notice_ : error_.empty() ? u8"읽지 못한 세션 파일이 있습니다 · 상세 화면 확인" : u8(error_) };
                footer->add_flexible(label(u8"widget-signal", problem, 11.0f, luil::label_color_role::error));
            }
            else
            {
                const auto signal { signal_text(now) };
                const auto split { signal.find(u8'\n') };
                footer->add_flexible(provider_line(u8"widget-signal", "codex", signal.substr(0, split), 11.0f));
                if (split != std::u8string::npos)
                    footer->add_flexible(provider_line(u8"widget-signal-claude", "claude", signal.substr(split + 1), 11.0f));
            }
            luil::panel_config footer_surface {};
            footer_surface.background = [](const luil::ui_color_palette& palette) { return palette.surface_background; };
            auto footer_panel { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, u8"widget-footer" }, footer_surface) };
            footer_panel->set_content(std::move(footer));
            content->add(divider(u8"widget-footer-rule"), 1.0f);
            content->add(std::move(footer_panel), widget_footer_height);
        }
        content->arrange({ { 0.0f, 0.0f, width, height }, scale });
        if (scroll_metrics != nullptr)
            scroll_ = scroll_metrics->metrics().scroll_offset;
        root->add(std::move(content));
        if (!collapsed_)
        {
            // 오른쪽 아래 모서리를 끌면 widget_window가 크기 조절로 답한다. 이 표시는 자리를 알릴 뿐이다.
            const luil::glyph_config grip { .glyph = luil::codicons::icon_grabber, .font_size = 12.0f, .description = u8"크기 조절" };
            auto resize { std::make_unique<luil::glyph_element>(luil::ui_element_id { kind_label, u8"widget-resize" }, grip) };
            resize->set_tooltip(u8"크기 조절");
            resize->arrange({ { width - 14.0f * scale, height - 14.0f * scale, 14.0f * scale, 14.0f * scale }, scale });
            root->add(std::move(resize));
        }
        return root;
    }

    std::u8string board_driver::signal_text(const std::int64_t now) const
    {
        const auto received = [&](const std::string_view provider) {
            const auto found { std::ranges::find_if(data_.sessions, [&](const auto& session) { return session.provider == provider; }) };
            return found == data_.sessions.end() ? std::u8string { u8"아직 수신 없음" } : u8"마지막 수신 " + age_text(found->updated_ms, now);
        };
        std::u8string codex { u8"Codex · " + received("codex") };
        if (codex_ != nullptr)
            codex = !codex_data_.error.empty() ? u8(codex_data_.error)
                : codex_data_.ready            ? u8"Codex · 자동 수집 " + u8(std::to_string(codex_data_.sessions.size())) + u8"개"
                                               : std::u8string { u8"Codex · 로컬 세션 탐색 중" };
        std::u8string claude { u8"Claude · " + received("claude") };
        if (codex_hooks_.has_value())
            codex += !codex_hooks_->error.empty() ? u8" · " + u8(codex_hooks_->error)
                : codex_hooks_->disabled          ? std::u8string { u8" · Hooks 꺼짐" }
                : codex_hooks_->changed           ? std::u8string { u8" · /hooks 신뢰 필요" }
                                                  : std::u8string { u8" · Hooks 등록됨" };
        if (claude_ != nullptr)
            claude = !claude_data_.error.empty() ? u8(claude_data_.error)
                : claude_data_.ready             ? u8"Claude · 자동 수집 " + u8(std::to_string(claude_data_.sessions.size())) + u8"개"
                                                 : std::u8string { u8"Claude · 로컬 세션 탐색 중" };
        // 자동 등록이 실패해도 로컬 관측은 계속되므로 같은 줄에 덧붙인다.
        if (claude_hooks_.has_value())
            claude += !claude_hooks_->error.empty() ? u8" · " + u8(claude_hooks_->error)
                : claude_hooks_->disabled           ? std::u8string { u8" · Hooks 꺼짐" }
                : claude_hooks_->changed            ? std::u8string { u8" · Hooks 등록됨" }
                                                    : std::u8string { u8" · Hooks 병행" };
        return codex + u8"\n" + claude;
    }

    luil::app_message board_driver::make_close_message()
    {
        return luil::app_message { close_intent {} };
    }

    bool board_driver::shutdown_completed() const
    {
        return closed_.load();
    }

    std::optional<luil::win32::window_placement> load_window_placement(const std::filesystem::path& file)
    {
        try
        {
            std::ifstream input { file, std::ios::binary };
            if (!input)
                return std::nullopt;
            const auto value { nlohmann::json::parse(input) };
            luil::win32::window_placement placement {};
            placement.x = value.at("x").get<int>();
            placement.y = value.at("y").get<int>();
            placement.width = value.at("width").get<int>();
            placement.height = value.at("height").get<int>();
            placement.maximized = value.value("maximized", false);
            if (!placement.valid() || !luil::win32::placement_in_screen_range(placement))
                return std::nullopt;
            return placement;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    void save_window_placement(const std::filesystem::path& file, const luil::win32::window_placement& placement) noexcept
    {
        try
        {
            nlohmann::json value { { "x", placement.x }, { "y", placement.y }, { "width", placement.width }, { "height", placement.height } };
            if (placement.maximized)
                value["maximized"] = true;
            std::filesystem::create_directories(file.parent_path());
            // 쓰다 끊겨도 옛 파일이 남도록 임시 파일을 다 쓴 뒤 바꿔 끼운다.
            const std::filesystem::path temporary { file.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp" };
            {
                std::ofstream output { temporary, std::ios::binary | std::ios::trunc };
                output << value.dump();
                if (!output)
                    return;
            }
            MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        catch (...)
        {}
    }

    board_delegate::board_delegate(std::filesystem::path placement_file, const bool restored, single_instance* instance)
        : placement_file_ { std::move(placement_file) }
        , restored_ { restored }
        , instance_ { instance }
    {}

    void board_delegate::on_started(luil::app_host& host)
    {
        if (!window_.attach(restored_))
        {
            host.post_app_message(luil::app_message { widget_failed_intent {} });
            return;
        }
        if (instance_ != nullptr)
            instance_->bind_window(window_.handle());
    }

    luil::app_message board_delegate::make_window_placement_message(const luil::win32::window_placement& placement)
    {
        if (placement_file_.empty() || placement.maximized || placement.fullscreen)
            return {};
        auto saved { placement };
        if (window_.collapsed())
            saved.height = window_.expanded_height();
        save_window_placement(placement_file_, saved);
        return {};
    }

    void board_delegate::execute_app_ui_command(luil::app_host& host, const luil::app_ui_command& command)
    {
        if (command.command == toggle_collapse_command || command.command == toggle_pin_command)
        {
            if (command.command == toggle_collapse_command)
                window_.toggle_collapse();
            else
                window_.toggle_pin();
            host.post_app_message(luil::app_message { widget_state_intent { window_.collapsed(), window_.pinned() } });
        }
        else if (command.command == backdrop_blur_command || command.command == backdrop_transparent_command)
        {
            // DWM 합성은 창을 가진 UI thread에서 바꾸고, 그릴 바탕의 진하기는 앱 상태로 알린다.
            const auto backdrop { command.command == backdrop_blur_command ? widget_backdrop::blur : widget_backdrop::transparent };
            window_.set_backdrop(backdrop);
            host.post_app_message(luil::app_message { backdrop_intent { backdrop } });
        }
        else if (command.command == opacity_command || command.command == opacity_change_command)
        {
            // 진하기의 정본은 위젯 창이 든다. 100%에 들어서거나 벗어나면 그 자리에서 합성을 바꾸고 다듬은 값만 앱에 알린다.
            //  - 끌기의 델타도 여기서 쌓으므로 앱 상태가 늦어도 값이 겹쳐 더해지지 않는다.
            if (const auto value { parse_number(command.argument) }; value.has_value())
            {
                window_.set_opacity(command.command == opacity_command ? *value : window_.opacity() + *value);
                host.post_app_message(luil::app_message { opacity_intent { window_.opacity() } });
            }
        }
    }

    luil::app_message board_delegate::make_window_metrics_message(const float width, const float height, const float scale)
    {
        return luil::app_message { metrics_intent { width, height, scale } };
    }

    std::vector<luil::input_action> board_policy::on_wheel(const luil::ui_tree& tree, const luil::mouse_wheel_event& event, const float delta)
    {
        return luil::route_wheel(tree, event, delta);
    }
} // namespace session_board
