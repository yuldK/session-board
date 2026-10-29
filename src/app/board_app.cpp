#include "app/board_app.h"

#include "core/host_window.h"
#include "luil/generated/codicons.h"
#include "win32/resource_ids.h"

#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/effects/SkGradient.h"
#include "include/effects/SkImageFilters.h"
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
        // 라이선스 화면의 치수다 (논리 픽셀). 원문 줄은 luil 기본 행(본문 글꼴, 양옆 8)을 쓴다.
        constexpr float license_list_width { 300.0f };
        constexpr float license_row_height { 28.0f };
        constexpr float license_line_height { 18.0f };
        constexpr float license_row_padding { 8.0f };
        // 본문 글꼴 한 글자의 어림 폭이다 (글자 크기 배수). 넘치는 줄은 기본 행이 잘라 그리므로 넉넉히 잡는다.
        constexpr float license_glyph_width { 0.62f };

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

        // 누를 수 있는 판 안 글의 툴팁을 뺀다.
        //  - luil은 툴팁이 달린 요소를 hit를 흡수하지 않게 해도 누를 대상으로 본다. 행·카드 안의 라벨이 툴팁을 가지면 그 자리의
        //    누름을 라벨이 받아 행의 클릭이 서지 않는다. 글 전체는 판 자신의 툴팁이나 세부 정보가 보인다.
        //  - 누를 수 있는 단추의 툴팁은 무엇을 하는지 알리므로 남긴다.
        void strip_tooltips(const luil::ui_element& element)
        {
            for (const auto& child : element.children())
            {
                if (child->action(luil::ui_trigger::left_click) == nullptr)
                    child->set_tooltip({});
                strip_tooltips(*child);
            }
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
                luil::draw_context tinted { luil::derive_draw_context(context, palette, context.scale) };
                draw_children(tinted, interaction);
            }

        private:
            std::function<void(luil::ui_color_palette&)> tint_ {};
        };

        // 알림 popup의 백판이다. popup 뒤에 acrylic이 섰을 때만 판과 카드를 옅게 해 효과를 비춘다.
        //  - 효과가 섰는지는 표면만 안다 (`draw_context::backdrop`). 고대비·투명 효과 꺼짐처럼 서지 않으면 지금처럼 불투명하다.
        //  - 진하기는 위젯 설정과 관계없이 고정이다. 접힌 위젯 밖에 떠서 뒤 창의 글이 비치면 읽기 어렵다.
        class popup_backing final : public luil::ui_element
        {
        public:
            popup_backing(const std::u8string& id, std::unique_ptr<luil::ui_element> content)
                : ui_element { luil::ui_element_id { kind_layout, id } }
            {
                add_child(std::move(content));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                for (const auto& child : children())
                    child->arrange(context.for_child(context.slot));
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const luil::rect_f box { bounds() };
                const SkRect area { SkRect::MakeXYWH(box.x, box.y, box.width, box.height) };
                context.canvas.drawRect(area, luil::solid_paint(luil::with_alpha(context.palette.window_background, context.backdrop ? backing_alpha : 1.0f)));
                if (!context.backdrop)
                {
                    draw_children(context, interaction);
                    return;
                }
                luil::ui_color_palette palette { context.palette };
                palette.surface_background = luil::with_alpha(palette.surface_background, card_alpha);
                luil::draw_context tinted { luil::derive_draw_context(context, palette, context.scale) };
                draw_children(tinted, interaction);
            }

        private:
            // 아크릴이 이미 뒤를 흐리고 명암에 맞춘 막을 덧씌운다. 판은 옅게 깔아 뒤의 색이 번져 비치게 한다.
            static constexpr float backing_alpha { 0.25f };
            static constexpr float card_alpha { 0.5f };
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

        // Simple Icons(CC0)의 Google Gemini 표식 경로다 (24×24 viewBox). Antigravity는 따로 표식이 없어 Gemini 표식을 쓴다.
        constexpr char gemini_mark_path[] = "M11.04 19.32Q12 21.51 12 24q0-2.49.93-4.68.96-2.19 2.58-3.81t3.81-2.55Q21.51 12 24 12q-2.49 0-4.68-.93a12.3 12.3 0 0 1-3.81-2.58 12.3 12.3 0 0 1-2.58-3."
                                            "81Q12 2.49 12 0q0 2.49-.96 4.68-.93 2.19-2.55 3.81a12.3 12.3 0 0 1-3.81 2.58Q2.49 12 0 12q2.49 0 4.68.96 2.19.93 3.81 2.55t2.55 3.81";

        // provider 표식의 경로다. 처음 그릴 때 한 번만 푼다.
        const SkPath& provider_path(const std::string_view provider)
        {
            static const SkPath claude { SkParsePath::FromSVGString(claude_mark_path).value_or(SkPath {}) };
            static const SkPath codex { SkParsePath::FromSVGString(codex_mark_path).value_or(SkPath {}) };
            static const SkPath gemini { SkParsePath::FromSVGString(gemini_mark_path).value_or(SkPath {}) };
            return provider == "codex" ? codex : provider == "antigravity" ? gemini : claude;
        }

        // 상자 가운데에 정사각형으로 provider 표식을 그린다.
        //  - Claude·Gemini는 고유색이 있어 테마와 관계없이 그 색이고, Codex는 단색 표식이라 `mono`를 따른다.
        void draw_provider_mark(luil::draw_context& context, const std::string_view provider, const luil::rect_f& box, const luil::ui_color mono)
        {
            const float side { std::min(box.width, box.height) };
            if (side <= 0.0f)
                return;
            const luil::ui_color color { provider == "codex" ? mono : provider == "antigravity" ? luil::make_ui_color(0x8e, 0x75, 0xb2) : luil::make_ui_color(0xd9, 0x77, 0x57) };
            SkPaint paint { luil::solid_paint(color) };
            paint.setAntiAlias(true);
            context.canvas.save();
            context.canvas.translate(box.x + (box.width - side) / 2.0f, box.y + (box.height - side) / 2.0f);
            context.canvas.scale(side / 24.0f, side / 24.0f);
            context.canvas.drawPath(provider_path(provider), paint);
            context.canvas.restore();
        }

        // provider 표식이다. 상표라 아이콘 글꼴(codicons)에 없으므로 SVG 경로를 그린다.
        //  - Simple Icons에 Codex 표식이 없어 OpenAI 표식을 쓴다.
        //  - 바로 뒤의 글이 provider 이름을 말하므로 보조 기술에는 드러내지 않는다 (이름을 두 번 읽지 않게).
        class provider_mark final : public luil::ui_element
        {
        public:
            provider_mark(const std::u8string& id, const std::string_view provider)
                : ui_element { luil::ui_element_id { kind_label, id } }
                , provider_ { provider }
            {
                set_hit_opaque(false);
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot&) const override
            {
                draw_provider_mark(context, provider_, bounds(), context.palette.primary_foreground);
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::none };
            }

        private:
            std::string provider_ {};
        };

        // provider 필터의 칸 하나다. 전체 칸은 글을, provider 칸은 표식과 세션 수를 그린다.
        //  - 모양은 luil 선택 묶음의 칸을 따른다: 고른 칸만 밝은 판이 뜨고 글이 진해진다.
        //  - 표식만으로는 이름을 말하지 못하므로 툴팁과 보조 기술 이름에 provider 이름을 둔다.
        struct filter_chip
        {
            std::u8string value {};
            std::string provider {};
            std::u8string text {};
            std::u8string name {};
        };

        constexpr float filter_mark_size { 14.0f };
        constexpr float filter_mark_gap { 5.0f };
        constexpr float filter_chip_padding { 10.0f };

        class filter_chip_element final : public luil::ui_element
        {
        public:
            filter_chip_element(filter_chip chip, const bool selected)
                : ui_element { luil::ui_element_id { luil::ui_element_kind::choice, chip.value } }
                , chip_ { std::move(chip) }
                , selected_ { selected }
            {
                set_search_label(chip_.name);
                set_tooltip(chip_.name);
                set_cursor(luil::ui_cursor::hand);
                set_action(luil::ui_trigger::left_click, luil::make_message_action(provider_intent { chip_.provider }));
            }

            // 칸의 맞춤 폭이다 (논리 픽셀).
            [[nodiscard]] float fitted_width(const luil::measure_context& context) const
            {
                const float text { std::ceil(context.text_width(chip_.text, context.style_metrics().body_font_size)) };
                return filter_chip_padding * 2.0f + text + (chip_.provider.empty() ? 0.0f : filter_mark_size + filter_mark_gap);
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const float scale { context.scale > 0.0f ? context.scale : 1.0f };
                const luil::rect_f box { bounds() };
                if (box.width <= 0.0f || box.height <= 0.0f)
                    return;
                const float radius { context.metrics.row_corner_radius * scale };
                const SkRect shape { SkRect::MakeXYWH(box.x, box.y, box.width, box.height) };
                if (selected_)
                {
                    SkPaint thumb { luil::solid_paint(context.palette.thumb_background) };
                    thumb.setAntiAlias(true);
                    context.canvas.drawRRect(SkRRect::MakeRectXY(shape, radius, radius), thumb);
                    // 라이트의 흰 판이 밝은 트랙에서 번지지 않게 1px 선을 몸 안쪽에 긋는다.
                    SkPaint edge { luil::solid_paint(context.palette.divider) };
                    edge.setStyle(SkPaint::kStroke_Style);
                    edge.setStrokeWidth(scale);
                    edge.setAntiAlias(true);
                    const SkRect inner { shape.makeInset(scale / 2.0f, scale / 2.0f) };
                    context.canvas.drawRRect(SkRRect::MakeRectXY(inner, radius - scale / 2.0f, radius - scale / 2.0f), edge);
                }
                else
                    luil::draw_hover_fill(context, box, id(), interaction, true, context.metrics.row_corner_radius);

                const luil::ui_color text_color { selected_ ? context.palette.selection_foreground : context.palette.secondary_foreground };
                SkFont font { sk_ref_sp(context.ui_typeface), luil::scaled_font_size(context.metrics.body_font_size, scale) };
                font.setEmbolden(selected_);
                const float mark { chip_.provider.empty() ? 0.0f : (filter_mark_size + filter_mark_gap) * scale };
                const float text_width { luil::measure_text(chip_.text, font) };
                float x { box.x + std::max(0.0f, (box.width - mark - text_width) / 2.0f) };
                if (!chip_.provider.empty())
                {
                    const float side { filter_mark_size * scale };
                    // 고르지 않은 칸의 단색 표식은 글과 같이 흐리게 둔다.
                    draw_provider_mark(context, chip_.provider, { x, box.y + (box.height - side) / 2.0f, side, side }, text_color);
                    x += mark;
                }
                const SkPaint foreground { luil::solid_paint(text_color) };
                static_cast<void>(luil::draw_text_within(context.canvas, chip_.text, x, box.y + luil::centered_text_baseline(font, box.height), box.x + box.width - x, font, foreground));
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::radio_button, .name = chip_.name, .selected = selected_ };
            }

        private:
            filter_chip chip_ {};
            bool selected_ { false };
        };

        // provider 필터다. 칸은 맞춤 폭으로 왼쪽부터 늘어서고, 트랙은 칸들만큼만 그린다.
        class provider_filter_element final : public luil::ui_element
        {
        public:
            provider_filter_element(std::vector<filter_chip> chips, const std::u8string& selected)
                : ui_element { luil::ui_element_id { luil::ui_element_kind::choice_group, u8"provider-filter" } }
            {
                set_focus_group(luil::focus_axis::horizontal);
                set_focus_entry(luil::ui_element_id { luil::ui_element_kind::choice, selected });
                for (auto& chip : chips)
                {
                    const bool chosen { chip.value == selected };
                    add_child(std::make_unique<filter_chip_element>(std::move(chip), chosen));
                }
            }

            void arrange(const luil::arrange_context& context) override
            {
                const float scale { context.scale > 0.0f ? context.scale : 1.0f };
                const float padding { luil::choice_track_padding * scale };
                const float gap { luil::choice_segment_gap * scale };
                const float height { luil::choice_toggle_height * scale };
                float x { context.slot.x + padding };
                for (const auto& child : children())
                {
                    const float width { static_cast<const filter_chip_element&>(*child).fitted_width(context.measure()) * scale };
                    child->arrange(context.for_child({ x, context.slot.y + padding, width, std::max(0.0f, height - padding * 2.0f) }));
                    x += width + gap;
                }
                const float track { children().empty() ? 0.0f : x - gap + padding - context.slot.x };
                set_bounds({ context.slot.x, context.slot.y, std::min(track, context.slot.width), height });
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const float scale { context.scale > 0.0f ? context.scale : 1.0f };
                const luil::rect_f box { bounds() };
                const float radius { (context.metrics.row_corner_radius + luil::choice_track_padding) * scale };
                SkPaint track { luil::solid_paint(context.palette.input_background) };
                track.setAntiAlias(true);
                context.canvas.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), radius, radius), track);
                draw_children(context, interaction);
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::radio_group, .name = u8"AI 종류" };
            }
        };

        // 표식으로 시작하는 한 줄이다. provider 이름은 표식 뒤에도 글로 남겨 읽기·검색·툴팁이 그대로다.
        //  - `fit`이면 글 길이만큼만 차지한다 (상태 표시줄처럼 맞춤으로 쌓는 자리).
        //  - `tooltip`이 있으면 줄인 글 대신 그 글을 툴팁으로 보인다.
        std::unique_ptr<luil::stack_element> provider_line(const std::u8string& id, const std::string_view provider, std::u8string text, const float size,
            const luil::label_color_role color = luil::label_color_role::dim, const bool fit = false, std::u8string tooltip = {})
        {
            const luil::stack_config config { .direction = luil::stack_direction::row, .spacing = 4.0f, .cross_alignment = luil::stack_alignment::center };
            auto line { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-line" }, config) };
            const float mark { size + 1.0f };
            line->add(std::make_unique<provider_mark>(id + u8"-mark", provider), { .length = mark, .cross_length = mark });
            auto body { label(id, std::move(text), size, color) };
            if (!tooltip.empty())
                body->set_tooltip(std::move(tooltip));
            if (fit)
                line->add_fit(std::move(body));
            else
                line->add_flexible(std::move(body));
            return line;
        }

        // 표식이 provider를 말하므로 글 앞의 이름을 뗀다.
        std::u8string without_provider(const std::string_view provider, std::u8string text)
        {
            if (const auto* info { find_provider(provider) }; info != nullptr)
            {
                const std::u8string prefix { std::u8string { info->display } + u8" · " };
                if (text.starts_with(prefix))
                    text.erase(0, prefix.size());
            }
            return text;
        }

        // 위젯 바닥 줄은 provider 셋이 폭을 나눠 쓴다. 이름과 `자동 수집`을 뗀다.
        std::u8string short_signal(const std::string_view provider, std::u8string text)
        {
            text = without_provider(provider, std::move(text));
            constexpr std::u8string_view collected { u8"자동 수집 " };
            if (text.starts_with(collected))
                text.erase(0, collected.size());
            return text;
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

        // 흘리는 목록의 위·아래 끝을 투명하게 지운다. 반투명 위젯이라 그림자 대신 내용 자체가 옅어지며 사라지게 한다.
        //  - 흘린 쪽에만 걸고, 끝에서 떨어진 거리를 따라 `scroll_edge_strength`로 짙어진다 (흘리기 시작할 때 툭 켜지지 않게).
        //  - 막대 자리는 지우지 않는다 (`viewport` 안쪽만).
        class scroll_fade final : public luil::ui_element
        {
        public:
            scroll_fade(const std::u8string& id, std::unique_ptr<luil::scroll_area_element> content)
                : ui_element { luil::ui_element_id { kind_layout, id } }
                , area_ { content.get() }
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

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const luil::scroll_metrics& metrics { area_->metrics() };
                const float top { luil::scroll_edge_strength(metrics.scroll_offset) };
                const float bottom { luil::scroll_edge_strength(metrics.maximum_scroll - metrics.scroll_offset) };
                if (top <= 0.0f && bottom <= 0.0f)
                {
                    draw_children(context, interaction);
                    return;
                }
                const luil::rect_f box { bounds() };
                const SkRect layer { SkRect::MakeXYWH(box.x, box.y, box.width, box.height) };
                context.canvas.saveLayer(&layer, nullptr);
                draw_children(context, interaction);
                const luil::rect_f view { area_->viewport() };
                const float band { std::min(fade_distance * context.scale, view.height / 2.0f) };
                // 끝에서 `1 - strength`만 남기고 띠 안쪽으로 갈수록 온전히 남긴다 (DstIn은 그린 알파만큼 아래를 남긴다).
                const auto erase = [&](const float edge, const float inner, const float strength) {
                    if (strength <= 0.0f || band <= 0.0f)
                        return;
                    const SkPoint points[2] { { view.x, edge }, { view.x, inner } };
                    const SkColor4f colors[2] { { 0.0f, 0.0f, 0.0f, 1.0f - strength }, { 0.0f, 0.0f, 0.0f, 1.0f } };
                    SkPaint paint {};
                    paint.setShader(SkShaders::LinearGradient(points, SkGradient { SkGradient::Colors { colors, SkTileMode::kClamp }, {} }));
                    paint.setBlendMode(SkBlendMode::kDstIn);
                    context.canvas.drawRect(SkRect::MakeLTRB(view.x, std::min(edge, inner), view.x + view.width, std::max(edge, inner)), paint);
                };
                erase(view.y, view.y + band, top);
                erase(view.y + view.height, view.y + view.height - band, bottom);
                context.canvas.restore();
            }

        private:
            static constexpr float fade_distance { 20.0f };
            luil::scroll_area_element* area_ { nullptr };
        };

        // OS 앱 모드가 어두운가다. 시스템 테마를 고른 동안 상세 창의 밝기 단추가 지금 보이는 반대쪽을 고르는 데 쓴다.
        bool system_dark()
        {
            DWORD value { 1 };
            DWORD size { sizeof(value) };
            const auto result { RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) };
            return result == ERROR_SUCCESS && value == 0;
        }

        // 외양 파일에 적는 배경 이름이다. 예전 파일의 "blur"는 Acrylic이었으므로 색 없는 흐림은 "blur-plain"이다.
        const char* backdrop_name(const widget_backdrop backdrop)
        {
            switch (backdrop)
            {
            case widget_backdrop::transparent:
                return "transparent";
            case widget_backdrop::blur:
                return "blur-plain";
            case widget_backdrop::acrylic:
                break;
            }
            return "acrylic";
        }

        widget_backdrop backdrop_of_command(const std::uint32_t command)
        {
            if (command == backdrop_transparent_command)
                return widget_backdrop::transparent;
            if (command == backdrop_blur_command)
                return widget_backdrop::blur;
            return widget_backdrop::acrylic;
        }

        // 쓰다 끊겨도 옛 파일이 남도록 임시 파일을 다 쓴 뒤 바꿔 끼운다. 실패는 삼킨다.
        void write_json_file(const std::filesystem::path& file, const nlohmann::json& value) noexcept
        {
            try
            {
                std::filesystem::create_directories(file.parent_path());
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
                // 응답이 묻고 끝났으면 사용자의 답을 기다리는 것이고, 보고로 끝났으면 맡긴 일을 마친 것이다.
                if (session.turn_closed)
                    return session.awaiting_reply ? u8"응답 대기" : u8"응답 완료";
                return u8"대기 중";
            case session_status::running:
                // 오래 도는 도구는 확인할 일이 아니라 얼마나 돌았는지가 궁금한 일이다. 마지막 소식이 그 도구의 시작이다.
                if (long_running_tool(session, now))
                {
                    const auto minutes { std::max<std::int64_t>(0, (now - session.updated_ms) / 60'000) };
                    return u8"작업 중 · " + (minutes < 60 ? u8(std::to_string(minutes)) + u8"분째" : u8(std::to_string(minutes / 60)) + u8"시간째");
                }
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
            const auto found { std::ranges::find(providers, session.observation_source, &provider_info::local_source) };
            const std::u8string source { found != std::end(providers) ? found->local_text : std::u8string_view { u8"로컬 기록" } };
            return source + (session.runtime_loaded ? u8" · 열린 세션" : u8" · 최근 활동");
        }

        std::u8string own_activity_text(const session_snapshot& session, const std::int64_t now)
        {
            if (!session.activity.empty())
                return u8(session.activity);
            if (session.status == session_status::failed && !session.failure.empty())
                return u8"오류 · " + u8(failure_label(session.failure));
            if (session.status == session_status::failed)
                return u8"오류로 멈춤";
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
            // 끝난 turn의 상태는 상태 칸이 이미 말한다. 활동 줄은 세션이 지금 무엇을 기다리는지만 말한다.
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
            const auto* provider { find_provider(session.provider) };
            return provider != nullptr ? std::u8string { provider->display } : u8(session.provider);
        }

        // 위젯의 진행 중 묶음에 둘 세션이다. 응답이 끝났어도 하위가 돌면 진행 중이다.
        bool widget_active(const session_snapshot& session)
        {
            return is_working(session.status) || working_children(session) > 0;
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

        // 작업 중 표시의 크기다. 회전 글리프는 점보다 획이 가늘어 조금 크게 둔다.
        constexpr float running_mark_size { 11.0f };

        // 상태 점이다. 작업 중이면 도는 표시로 진행 중임을 보이고, 끝난 일은 속이 빈 점이다.
        //  - 점의 모양만으로도 진행·완료·종료가 갈린다. 움직임 줄이기를 켜면 회전 표시는 멈춘 글리프로 선다.
        std::unique_ptr<luil::ui_element> status_mark(const std::u8string& id, const bool running, const bool closed, const float size)
        {
            if (running)
            {
                auto spinner { std::make_unique<luil::spinner_element>(luil::ui_element_id { kind_layout, id + u8"-dot" }, luil::spinner_config { .name = u8"작업 중", .size = running_mark_size }) };
                spinner->set_hit_opaque(false);
                return spinner;
            }
            luil::panel_config dot_config {};
            dot_config.background = [closed](const luil::ui_color_palette& palette) { return closed ? luil::with_alpha(palette.surface_background, 0.0f) : palette.secondary_foreground; };
            dot_config.corner_radius = size / 2.0f;
            if (closed)
                dot_config.border = [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; };
            auto dot { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, id + u8"-dot" }, dot_config) };
            dot->set_hit_opaque(false);
            return dot;
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
            cell->add(status_mark(id, running, closed, 8.0f), { .length = running ? running_mark_size : 8.0f, .cross_length = running ? running_mark_size : 8.0f });
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

        // 한 줄 머리에 넣는 상태다. 상태 칸과 같은 말을 쓰되 글 길이만큼만 차지한다.
        //  - 확인할 일은 글리프가 붙는 배지, 작업 중은 도는 표시, 그 밖은 점과 흐린 글이다.
        std::unique_ptr<luil::ui_element> status_inline(const std::u8string& id, const session_snapshot& session, const std::int64_t now)
        {
            const luil::stack_config config { .direction = luil::stack_direction::row, .spacing = 6.0f, .cross_alignment = luil::stack_alignment::center };
            auto cell { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-cell" }, config) };
            const bool child_attention { !own_attention(session, now) && children_need_attention(session, now) };
            if (own_attention(session, now) || child_attention)
            {
                std::u8string text { state_text(session, now) };
                bool error { session.status == session_status::failed };
                if (child_attention)
                {
                    // 부모 자신은 멀쩡하므로 배지는 하위의 사정을 말한다.
                    const auto children { ordered_children(session, now) };
                    text = u8"하위 " + child_state_text(*children.front(), now);
                    error = children.front()->status == session_status::failed;
                }
                auto badge { std::make_unique<luil::badge_element>(luil::ui_element_id { kind_badge, id }, luil::badge_config { .text = text, .tone = error ? luil::badge_tone::error : luil::badge_tone::warning }) };
                badge->set_hit_opaque(false);
                cell->add_fit(std::move(badge));
                return cell;
            }
            const bool running { session.status == session_status::running };
            const bool closed { session.status == session_status::ended || session.status == session_status::interrupted };
            const float mark { running ? running_mark_size : 8.0f };
            cell->add(status_mark(id, running, closed, 8.0f), { .length = mark, .cross_length = mark });
            cell->add_fit(label(id, state_text(session, now), 11.0f, running ? luil::label_color_role::primary : luil::label_color_role::dim));
            return cell;
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
            text->add(provider_line(id + u8"-meta", session.provider, project_text(session) + u8" · " + activity_text(session, now), 11.0f), 16.0f);
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
            strip_tooltips(*result);
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
            strip_tooltips(*result);
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
            header->add_flexible(provider_line(u8"detail-source", session.provider, project_text(session), 11.0f));
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
            header->add_flexible(provider_line(u8"detail-source", session.provider, project_text(session) + u8" · 하위", 11.0f));
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
        constexpr float widget_group_header { 24.0f };
        constexpr float widget_group_gap { 6.0f };
        // 접힌 위젯의 알림 팝업 id다. 그 안 카드의 세부 정보 팝업이 이 표면에 붙는다.
        constexpr std::u8string_view update_surface { u8"session-updates" };
        // 머릿줄 상태 팝업 id다. 그 안 세션 줄의 세부 정보 팝업이 이 표면에 붙는다.
        constexpr std::u8string_view status_surface { u8"widget-status" };
        constexpr float widget_footer_height { 30.0f };
        constexpr float widget_setting_row { 26.0f };
        // 진하기의 자석이다. 0%는 바탕 없이 글만 뜨고, 100%는 DWM 합성을 끈 불투명 창이다.
        constexpr std::array<float, 6> widget_opacity_magnets { 0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f };
        constexpr float widget_row_inset { 12.0f };
        constexpr float widget_card_radius { 10.0f };
        // 위젯 설정 줄의 테마 카드다. luil 기본(112×70)은 위젯 폭에 세 장이 들지 않아 줄인다.
        constexpr float widget_theme_card_width { 88.0f };
        constexpr float widget_theme_preview_height { 46.0f };
        constexpr float widget_theme_card_gap { 8.0f };
        // 머릿줄의 상태 표식이다. 칸 폭은 재지 않고 표식·자릿수로 어림한다 (11px 숫자 한 자는 7px 안에 든다).
        constexpr float header_mark_size { 12.0f };
        constexpr float header_mark_spacing { 2.0f };
        constexpr float header_digit_width { 7.0f };
        constexpr float header_count_gap { 6.0f };
        // 표식 넷이 한 자리 수로 다 선 폭이다. 단추를 모을지 이 폭으로 정해, 수가 바뀌어도 단추가 들고 나지 않게 한다.
        constexpr float header_counts_reserve { 4.0f * (header_mark_size + header_mark_spacing + header_digit_width) + 3.0f * header_count_gap };
        // 단추를 모을지 정할 때 제목에 남겨 둘 폭이다. 이보다 좁으면 제목이 말줄임으로 줄어든다.
        constexpr float header_title_minimum { 84.0f };
        constexpr float header_spacing { 2.0f };
        // 판 팝업과 위젯 사이다.
        constexpr float widget_panel_gap { 6.0f };
        constexpr float widget_actions_gap { 2.0f };
        constexpr float widget_actions_inset { 6.0f };

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

        // 판 팝업을 닫는 계기다. 위젯을 끌어 옮기거나 크기를 바꾸는 동안은 판이 따라오게 둔다.
        std::function<luil::input_action(luil::popup_dismiss_reason)> close_panel(const widget_panel panel)
        {
            return [panel](const luil::popup_dismiss_reason reason) {
                if (reason == luil::popup_dismiss_reason::pointer_press_outside || reason == luil::popup_dismiss_reason::escape_key
                    || reason == luil::popup_dismiss_reason::activation_changed || reason == luil::popup_dismiss_reason::anchor_closed)
                    return luil::make_app_action(widget_panel_intent { .panel = panel, .open = false, .dismissed = true });
                return luil::input_action {};
            };
        }

        // 판을 여닫는 단추는 마우스로 누르는 순간 뒤집는다.
        //  - 판 밖을 누르면 그 누름에서 판이 닫히고, 뗄 때는 이미 닫힌 판의 단추가 다시 그려져 있다. 떼는 순간의 클릭으로
        //    뒤집으면 닫힌 판을 도로 연다. 누르는 순간이면 닫힘과 단추가 같은 판(열림)을 보고 둘 다 닫는다.
        //  - 누름 역할이 선 단추는 떼도 클릭이 나가지 않는다. 클릭 액션은 키보드·보조 기술의 실행으로만 쓰인다.
        void press_toggles(luil::ui_element& button)
        {
            const luil::ui_action* const click { button.action(luil::ui_trigger::left_click) };
            if (click == nullptr)
                return;
            button.set_pointer_drag_target(luil::pointer_drag_target { .on_press = *click });
        }

        // 머릿줄 상태 표식 하나다.
        struct header_count
        {
            std::u8string id {};
            std::u8string name {};
            // 0이면 작업 중 회전 표시다.
            char32_t glyph { 0 };
            std::size_t count { 0 };
        };

        float header_count_width(const header_count& item)
        {
            return header_mark_size + header_mark_spacing + header_digit_width * static_cast<float>(std::to_string(item.count).size());
        }

        float header_counts_width(const std::vector<header_count>& counts)
        {
            float width { 0.0f };
            for (const auto& item : counts)
                width += (width > 0.0f ? header_count_gap : 0.0f) + header_count_width(item);
            return width;
        }

        // 0인 표식은 흐리게 둔다. 자리는 지키되 눈은 수가 있는 표식으로 간다.
        void quiet_tint(luil::ui_color_palette& palette)
        {
            palette.primary_foreground = luil::with_alpha(palette.primary_foreground, 0.45f);
        }

        // 머릿줄의 상태 표식 줄이다. 보조 기술에는 표식을 하나씩 읽히지 않고 네 수를 한 문장으로 알린다.
        //  - 누르면 상태 팝업을 여닫으므로 단추로 읽힌다.
        class header_status final : public luil::ui_element
        {
        public:
            header_status(const std::vector<header_count>& counts)
                : ui_element { luil::ui_element_id { kind_label, u8"widget-count" } }
            {
                const luil::stack_config row_config { .direction = luil::stack_direction::row, .spacing = header_count_gap, .cross_alignment = luil::stack_alignment::center };
                auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-count-row" }, row_config) };
                for (const auto& item : counts)
                {
                    name_ += (name_.empty() ? std::u8string {} : std::u8string { u8" · " }) + item.name + u8" " + u8(std::to_string(item.count));
                    const luil::stack_config chip_config { .direction = luil::stack_direction::row, .spacing = header_mark_spacing, .cross_alignment = luil::stack_alignment::center };
                    auto chip { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, item.id }, chip_config) };
                    // 도는 표시는 일하는 세션이 있을 때만이다. 없으면 같은 모양의 멈춘 글리프다.
                    std::unique_ptr<luil::ui_element> mark {};
                    if (item.glyph == 0 && item.count > 0)
                        mark = std::make_unique<luil::spinner_element>(luil::ui_element_id { kind_layout, item.id + u8"-mark" }, luil::spinner_config { .size = header_mark_size });
                    else
                    {
                        const char32_t glyph { item.glyph == 0 ? luil::codicons::icon_loading : item.glyph };
                        mark = std::make_unique<luil::glyph_element>(luil::ui_element_id { kind_layout, item.id + u8"-mark" }, luil::glyph_config { .glyph = glyph, .font_size = header_mark_size });
                    }
                    mark->set_hit_opaque(false);
                    chip->add(std::move(mark), { .length = header_mark_size, .cross_length = header_mark_size });
                    chip->add_flexible(label(item.id + u8"-value", u8(std::to_string(item.count)), 11.0f));
                    if (item.count == 0)
                        row->add(std::make_unique<palette_scope>(item.id + u8"-quiet", quiet_tint, std::move(chip)), header_count_width(item));
                    else
                        row->add(std::move(chip), header_count_width(item));
                }
                add_child(std::move(row));
                set_hit_opaque(false);
                // 표식 줄 전체가 누르고 머무르는 자리다. 안의 수 라벨이 누름을 가로채지 않게 한다.
                strip_tooltips(*this);
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                for (const auto& child : children())
                    child->arrange(context.for_child(context.slot));
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                draw_children(context, interaction);
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::button, .name = name_ };
            }

        private:
            std::u8string name_ {};
        };

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
            // 도는 하위는 회전 표시, 끝난 하위는 속이 빈 점이다. 부모 카드의 상태 점과 같은 말을 쓴다.
            if (child.status == session_status::running)
                line->add(status_mark(id, true, false, 7.0f), { .length = running_mark_size, .cross_length = running_mark_size });
            else
            {
                luil::panel_config dot_config {};
                dot_config.background = [working](const luil::ui_color_palette& palette) { return working ? palette.accent : luil::with_alpha(palette.surface_background, 0.0f); };
                dot_config.corner_radius = 3.5f;
                if (!working)
                    dot_config.border = [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; };
                auto dot { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, id + u8"-dot" }, dot_config) };
                dot->set_hit_opaque(false);
                line->add(std::move(dot), { .length = 7.0f, .cross_length = 7.0f });
            }
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

        // 카드에 머무르면 옆에 세부 정보를 띄우고 떠나면 닫는다. 카드 id로 늦게 온 떠남을 가른다.
        void set_preview(luil::ui_element& card, const std::u8string& owner, const std::u8string& key, const std::u8string& surface)
        {
            strip_tooltips(card);
            card.set_hover_dwell(luil::hover_dwell_target {
                .delay = std::chrono::milliseconds { 450 },
                .on_dwell = luil::make_message_action(preview_intent { .owner = owner, .key = key, .surface = surface, .open = true }),
                .on_end = luil::make_message_action(preview_intent { .owner = owner, .key = key, .surface = surface, .open = false }),
            });
        }

        // 작업 설명 아래에 지금 하는 일을 덧붙일 만한가다. 끝난 세션의 활동 줄은 상태 칸을 되풀이할 뿐이다.
        bool activity_adds(const session_snapshot& session, const std::int64_t now)
        {
            return is_working(session.status) || session.status == session_status::failed || needs_attention(session, now) || working_children(session) > 0;
        }

        // 카드의 바탕이다. 펼친 카드는 선택 바탕, 세부 정보를 띄운 카드는 키 컬러 테두리로 가른다.
        //  - `alpha`가 1보다 작으면 카드 뒤로 창의 흐림 효과가 비친다 (아크릴 카드).
        luil::panel_config widget_card_surface(const bool expanded, const bool previewing, const float alpha = 1.0f)
        {
            luil::panel_config surface {};
            surface.background = [expanded, alpha](const luil::ui_color_palette& palette) {
                const luil::ui_color color { expanded ? palette.row_selection_background : palette.surface_background };
                return alpha < 1.0f ? luil::with_alpha(color, static_cast<float>(color >> 24U) / 255.0f * alpha) : color;
            };
            surface.corner_radius = widget_card_radius;
            // 옅은 카드는 바탕만으로는 가장자리가 흐림에 묻힌다. 글자색으로 얇은 테를 둘러 카드 꼴을 지킨다. 옅을수록 진하다.
            surface.border = [previewing, alpha](const luil::ui_color_palette& palette) {
                if (previewing)
                    return luil::with_alpha(palette.accent, 0.7f);
                return alpha < 1.0f ? luil::with_alpha(palette.primary_foreground, 0.12f + 0.2f * (1.0f - alpha)) : palette.divider;
            };
            return surface;
        }

        // 옅은 카드 위의 글·아이콘에 바탕색 후광을 두른다. 뒤가 밝은 창이거나 글자색과 비슷해도 글꼴의 윤곽이 남는다.
        //  - 카드가 옅을수록(`strength`가 클수록) 후광이 진하다. 0이면 그대로 그린다.
        //  - 후광색은 테마의 창 바탕색이라 다크에서는 어두운, 라이트에서는 밝은 테가 선다.
        //  - 카드 바탕이 아니라 내용에만 건다. 바탕까지 감싸면 카드 둘레에 그림자가 진다.
        class legible_scope final : public luil::ui_element
        {
        public:
            legible_scope(const std::u8string& id, const float strength, std::unique_ptr<luil::ui_element> content)
                : ui_element { luil::ui_element_id { kind_layout, id } }
                , strength_ { std::clamp(strength, 0.0f, 1.0f) }
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

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                if (strength_ <= 0.0f)
                {
                    draw_children(context, interaction);
                    return;
                }
                const float sigma { halo_sigma * context.scale };
                SkPaint layer {};
                layer.setImageFilter(SkImageFilters::DropShadow(0.0f, 0.0f, sigma, sigma, luil::with_alpha(context.palette.window_background, strength_), nullptr));
                const luil::rect_f box { bounds() };
                const SkRect area { SkRect::MakeXYWH(box.x, box.y, box.width, box.height).makeOutset(3.0f * sigma, 3.0f * sigma) };
                context.canvas.saveLayer(&area, &layer);
                draw_children(context, interaction);
                context.canvas.restore();
            }

        private:
            static constexpr float halo_sigma { 1.4f };
            float strength_ { 0.0f };
        };

        // 상태 팝업·세부 정보의 색이다. 글리프와 라벨은 전경색으로 그리므로 전경색만 그 뜻의 색으로 바꾼다.
        //  - 글자에는 채움용 키 컬러(`accent`)가 아니라 어두운 바탕에서도 읽히는 강조 글자색을 쓴다.
        void warning_tint(luil::ui_color_palette& palette)
        {
            palette.primary_foreground = palette.warning_accent;
        }

        void error_tint(luil::ui_color_palette& palette)
        {
            palette.primary_foreground = palette.error_accent;
        }

        void accent_tint(luil::ui_color_palette& palette)
        {
            palette.primary_foreground = palette.accent;
        }

        void accent_text_tint(luil::ui_color_palette& palette)
        {
            palette.primary_foreground = palette.accent_emphasis_foreground;
        }

        // 세션이 지금 무엇을 말하는가다. 상태 팝업의 묶음·결과 줄 색과 세부 정보의 이유 상자가 이 하나로 갈린다.
        enum class session_tone
        {
            attention,
            failed,
            running,
            done,
        };

        session_tone tone_of(const session_snapshot& session, const std::int64_t now)
        {
            if (session.status == session_status::failed)
                return session_tone::failed;
            if (group_of(session, now) == session_group::attention)
                return session_tone::attention;
            if (widget_active(session))
                return session_tone::running;
            return session_tone::done;
        }

        using palette_tint = void (*)(luil::ui_color_palette&);

        // 결과 줄·이유 상자의 글자색이다. 끝난 일은 흐린 글자 그대로라 물들이지 않는다.
        palette_tint text_tint_of(const session_tone tone)
        {
            switch (tone)
            {
            case session_tone::attention:
                return warning_tint;
            case session_tone::failed:
                return error_tint;
            case session_tone::running:
                return accent_text_tint;
            case session_tone::done:
                return nullptr;
            }
            return nullptr;
        }

        // 그 뜻의 바탕색이다. 이유 상자와 상태 팝업 카드의 테가 옅게 깐다.
        luil::ui_color tone_color(const luil::ui_color_palette& palette, const session_tone tone)
        {
            switch (tone)
            {
            case session_tone::attention:
                return palette.warning_accent;
            case session_tone::failed:
                return palette.error_accent;
            case session_tone::running:
                return palette.accent;
            case session_tone::done:
                return palette.primary_foreground;
            }
            return palette.primary_foreground;
        }

        // 글자 하나를 그 뜻의 색으로 물들인다. 물들일 색이 없으면 그대로 돌려준다.
        std::unique_ptr<luil::ui_element> tinted(const std::u8string& id, const palette_tint tint, std::unique_ptr<luil::ui_element> content)
        {
            if (tint == nullptr)
                return content;
            return std::make_unique<palette_scope>(id, tint, std::move(content));
        }

        // 상태 팝업의 세션 카드다. 옅은 반투명 판에 얇은 테를 두르고, 포인터가 오르면 한 단 밝아진다.
        //  - 세부 정보를 띄운 카드는 키 컬러 테로 어느 카드의 세부인지 잇는다.
        //  - 액션이 없어도 hit를 받아 머무름·hover가 선다.
        class status_card final : public luil::ui_element
        {
        public:
            status_card(const std::u8string& id, const bool previewing, std::unique_ptr<luil::ui_element> content)
                : ui_element { luil::ui_element_id { kind_layout, id } }
                , previewing_ { previewing }
            {
                set_hit_opaque(true);
                add_child(std::move(content));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                for (const auto& child : children())
                    child->arrange(context.for_child(context.slot));
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const luil::rect_f box { bounds() };
                const float radius { corner_radius * context.scale };
                const SkRect shape { SkRect::MakeXYWH(box.x, box.y, box.width, box.height) };
                SkPaint fill { luil::solid_paint(luil::with_alpha(context.palette.primary_foreground, previewing_ ? 0.11f : 0.055f)) };
                fill.setAntiAlias(true);
                context.canvas.drawRRect(SkRRect::MakeRectXY(shape, radius, radius), fill);
                if (!previewing_)
                    luil::draw_hover_fill(context, box, id(), interaction, true, corner_radius);
                SkPaint edge { luil::solid_paint(previewing_ ? luil::with_alpha(context.palette.accent, 0.7f) : luil::with_alpha(context.palette.primary_foreground, 0.08f)) };
                edge.setStyle(SkPaint::kStroke_Style);
                edge.setStrokeWidth(context.scale);
                edge.setAntiAlias(true);
                const SkRect inner { shape.makeInset(context.scale / 2.0f, context.scale / 2.0f) };
                context.canvas.drawRRect(SkRRect::MakeRectXY(inner, radius - context.scale / 2.0f, radius - context.scale / 2.0f), edge);
                draw_children(context, interaction);
            }

        private:
            static constexpr float corner_radius { 6.0f };
            bool previewing_ { false };
        };

        // 옅은 판 하나다. 세부 정보의 요청 상자·이유 상자가 쓴다. `border`가 0이면 테가 없다.
        std::unique_ptr<luil::panel_element> soft_box(const std::u8string& id, const session_tone tone, const float fill, const float border, std::unique_ptr<luil::ui_element> content)
        {
            luil::panel_config config {};
            config.background = [tone, fill](const luil::ui_color_palette& palette) { return luil::with_alpha(tone_color(palette, tone), fill); };
            if (border > 0.0f)
                config.border = [tone, border](const luil::ui_color_palette& palette) { return luil::with_alpha(tone_color(palette, tone), border); };
            config.corner_radius = 6.0f;
            auto result { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, id }, config) };
            result->set_content(std::move(content));
            result->set_hit_opaque(false);
            return result;
        }

        // 카드 진하기에 맞춘 후광 세기다. 기본 진하기(55%) 근처는 거의 없고 0%에서 가장 진하다.
        float halo_strength(const float alpha)
        {
            return alpha >= 1.0f ? 0.0f : std::clamp((0.7f - alpha) / 0.7f, 0.0f, 1.0f) * 0.95f;
        }

        // 위젯 카드 한 장이다: 상태·폴더·시각과 이동 단추, 작업 설명 두 줄, 진행 중이면 지금 하는 일.
        //  - 누르면 펼쳐 전체 설명과 모든 하위를 보이고, 머무르면 옆에 세부 정보를 띄운다.
        //  - 제품은 표식으로만 말한다. 이름은 읽기용 이름에 남긴다.
        sized_element widget_row(const session_snapshot& session, const std::int64_t now, const float width, const bool expanded, const bool previewing, const float alpha)
        {
            const std::u8string owner { widget_owner(session) };
            const std::u8string title { task_text(session) };
            const luil::stack_config content_config { .padding = { widget_row_inset, 8.0f, 6.0f, 10.0f } };
            auto content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, owner + u8"-body" }, content_config) };
            const luil::stack_config top_config { .direction = luil::stack_direction::row, .spacing = 8.0f, .cross_alignment = luil::stack_alignment::center };
            auto top { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, owner + u8"-top" }, top_config) };
            top->add(status_cell(owner + u8"-status", session, now), 112.0f);
            top->add_flexible(provider_line(owner + u8"-source", session.provider, project_text(session), 11.0f));
            top->add_fit(label(owner + u8"-age", age_text(latest_update(session), now), 11.0f, luil::label_color_role::dim));
            const std::u8string focus_name { focus_tooltip(session, provider_text(session) + u8" 세션 창으로 이동") };
            auto focus { widget_button(owner + u8"-focus", focus_name, luil::codicons::icon_link_external, luil::make_message_action(focus_session_intent { session_key(session) })) };
            focus->set_enabled(can_focus(session));
            top->add(std::move(focus), { .length = widget_icon_button, .cross_length = widget_icon_button });
            content->add(std::move(top), widget_icon_button);
            content->add_gap(4.0f);
            const float inner_width { width - widget_row_inset - 6.0f };
            float height { 18.0f + widget_icon_button + 4.0f };
            height += add_wrapped(*content, owner + u8"-request", title, inner_width, 12.0f, expanded ? 16 : 2);
            if (expanded || activity_adds(session, now))
            {
                content->add(label(owner + u8"-activity", activity_text(session, now), 11.0f, luil::label_color_role::dim), 16.0f);
                height += 16.0f;
            }
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
            auto row { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, owner }, widget_card_surface(expanded, previewing, alpha)) };
            row->set_content(std::make_unique<legible_scope>(owner + u8"-legible", halo_strength(alpha), std::move(content)));
            row->set_hit_opaque(true);
            row->set_cursor(luil::ui_cursor::hand);
            row->set_access_name(provider_text(session) + u8" · " + state_text(session, now) + u8" · " + title);
            set_preview(*row, owner, session_key(session), {});
            row->set_action(luil::ui_trigger::left_click, luil::make_message_action(expand_session_intent { owner }));
            return { std::move(row), height };
        }

        // 묶음 머리글이다: 흐린 이름과 개수, 오른쪽에 묶음 동작. 카드보다 앞에 서지 않게 바탕을 깔지 않는다.
        std::unique_ptr<luil::ui_element> widget_group_heading(
            const std::u8string& id, const std::u8string& name, const std::size_t count, const std::u8string& hint, std::unique_ptr<luil::ui_element> action = {})
        {
            const luil::stack_config config { .direction = luil::stack_direction::row, .spacing = 6.0f, .padding = { 4.0f, 0.0f, 2.0f, 0.0f }, .cross_alignment = luil::stack_alignment::center };
            auto content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-content" }, config) };
            content->add_fit(label(id, name, 11.0f, luil::label_color_role::dim));
            content->add_fit(std::make_unique<palette_scope>(id + u8"-tint", heading_tint, label(id + u8"-count", u8(std::to_string(count)), 11.0f)));
            content->add_flexible(label(id + u8"-hint", hint, 11.0f, luil::label_color_role::dim, luil::text_align::end));
            if (action != nullptr)
                content->add(std::move(action), { .length = widget_icon_button, .cross_length = widget_icon_button });
            return content;
        }

        // 실행 파일 resource에 실린 제3자 고지(luil 고지 생성기의 원문)를 나눈다. 못 찾으면 빈 목록이다.
        std::vector<notice_component> embedded_notices()
        {
            const HMODULE module { GetModuleHandleW(nullptr) };
            const HRSRC resource { FindResourceW(module, MAKEINTRESOURCEW(IDR_THIRD_PARTY_NOTICES), RT_RCDATA) };
            if (resource == nullptr)
                return {};
            const HGLOBAL loaded { LoadResource(module, resource) };
            const void* data { loaded == nullptr ? nullptr : LockResource(loaded) };
            if (data == nullptr)
                return {};
            // RCDATA는 끝에 NUL이 없으므로 크기로 자른다.
            return parse_notices({ static_cast<const char*>(data), SizeofResource(module, resource) });
        }

        // 라이선스 목록 행의 키는 이 앞붙이에 고지의 절 순서를 붙인 것이다. UIA 검증도 이 이름으로 행을 찾는다.
        constexpr std::u8string_view license_prefix { u8"license-" };

        std::u8string license_key(const std::size_t index)
        {
            return std::u8string { license_prefix } + u8(std::to_string(index));
        }

        std::size_t license_index(const std::u8string_view key)
        {
            const std::u8string_view digits { key.substr(std::min(key.size(), license_prefix.size())) };
            std::size_t index { 0 };
            std::from_chars(reinterpret_cast<const char*>(digits.data()), reinterpret_cast<const char*>(digits.data() + digits.size()), index);
            return index;
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

    luil::caption_config detail_caption(const bool dark, const bool licenses)
    {
        auto caption { board_caption() };
        caption.title = u8"Session Board · 상세";
        caption.close_tooltip = u8"상세 화면 닫기";
        caption.app_buttons.push_back({ .key = u8"theme", .glyph = luil::codicons::icon_color_mode, .tooltip = dark ? u8"밝게" : u8"어둡게", .action = luil::make_message_action(theme_intent {}) });
        caption.app_buttons.push_back({ .key = u8"refresh", .glyph = luil::codicons::icon_refresh, .tooltip = u8"새로 고침", .action = luil::make_message_action(refresh_intent {}) });
        const char32_t license_glyph { licenses ? luil::codicons::icon_arrow_left : luil::codicons::icon_law };
        const std::u8string license_tooltip { licenses ? u8"세션 목록으로" : u8"오픈 소스 라이선스" };
        caption.app_buttons.push_back({ .key = u8"licenses", .glyph = license_glyph, .tooltip = license_tooltip, .action = luil::make_message_action(licenses_intent {}) });
        return caption;
    }

    luil::caption_config widget_caption()
    {
        auto caption { board_caption() };
        caption.metrics.height = widget_caption_height;
        caption.metrics.application_icon_slot_width = 0;
        caption.buttons = { false, false, false };
        return caption;
    }

    board_driver::board_driver(std::filesystem::path directory, const bool open_detail, std::filesystem::path codex_directory, std::filesystem::path claude_directory,
        std::optional<claude_hook_result> claude_hooks, std::optional<codex_hook_result> codex_hooks, std::filesystem::path detail_placement_file, widget_appearance appearance,
        std::filesystem::path appearance_file, std::filesystem::path antigravity_directory)
        : directory_ { std::move(directory) }
        , codex_directory_ { std::move(codex_directory) }
        , claude_directory_ { std::move(claude_directory) }
        , antigravity_directory_ { std::move(antigravity_directory) }
        , claude_hooks_ { std::move(claude_hooks) }
        , codex_hooks_ { std::move(codex_hooks) }
        , theme_ { appearance.theme }
        , detail_open_ { open_detail }
        , accent_id_ { appearance.accent_id }
        , backdrop_ { appearance.backdrop }
        , opacity_ { appearance.opacity }
        , card_opacity_ { appearance.card_opacity }
        , detail_placement_file_ { std::move(detail_placement_file) }
        , appearance_file_ { std::move(appearance_file) }
        , saved_appearance_ { std::move(appearance) }
    {
        if (!detail_placement_file_.empty())
            detail_placement_ = load_window_placement(detail_placement_file_);
        // 프레임을 지우는 창 바탕을 투명하게 둔다. 색은 남겨 두어 상세 창이 알파만 되돌려 불투명 바탕을 깐다.
        auto style { std::make_shared<luil::ui_style>(luil::default_ui_style()) };
        style->dark.window_background = luil::with_alpha(style->dark.window_background, 0.0f);
        style->light.window_background = luil::with_alpha(style->light.window_background, 0.0f);
        // 위젯 밖에 뜨는 판·알림·세부 정보는 카드처럼 크게 둥글린다. OS 기본은 메뉴 크기의 작은 둥글기다.
        style->popup.corner = luil::popup_corner::round;
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
        if (!antigravity_directory_.empty())
            antigravity_ = std::make_unique<antigravity_monitor>(antigravity_directory_);
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
        if (antigravity_ != nullptr)
        {
            antigravity_data_ = antigravity_->snapshot();
            merge_antigravity_sessions(data_.sessions, antigravity_data_.sessions);
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
        // 목록에서 사라진 세션의 정리 기록은 더 쓸 데가 없다.
        std::erase_if(cleared_, [&](const auto& item) { return std::ranges::none_of(data_.sessions, [&](const auto& session) { return session_key(session) == item.first; }); });
        const auto previous_response { !updates_.pending().empty() && updates_.pending().front().response_required ? updates_.pending().front().sequence : 0 };
        updates_.observe(data_.sessions, collapsed_);
        if (!updates_.pending().empty() && updates_.pending().front().response_required && updates_.pending().front().sequence != previous_response)
            update_scroll_ = 0.0f;
    }

    void board_driver::handle(luil::app_message message)
    {
        if (message.get<close_intent>() != nullptr)
        {
            save_appearance();
            closed_.store(true);
        }
        else if (const auto* metrics { message.get<metrics_intent>() }; metrics != nullptr)
        {
            if (metrics->collapsed != collapsed_)
            {
                if (metrics->collapsed)
                    update_scroll_ = 0.0f;
                collapsed_ = metrics->collapsed;
                if (!collapsed_)
                    updates_.clear();
                else
                    updates_.observe(data_.sessions, true);
            }
            metrics_ = *metrics;
        }
        else if (message.get<refresh_intent>() != nullptr)
            reload();
        else if (const auto* update { message.get<acknowledge_update_intent>() }; update != nullptr)
            updates_.acknowledge(update->provider, update->session_id, update->sequence);
        else if (message.get<acknowledge_all_intent>() != nullptr)
        {
            const auto pending { updates_.pending() };
            for (const auto& item : pending)
                if (!item.response_required)
                    updates_.acknowledge(item.session.provider, item.session.session_id, item.sequence);
        }
        else if (message.get<clear_completed_intent>() != nullptr)
        {
            // 지금 보이는 끝난 세션의 마지막 소식을 적어 둔다. 그보다 새 소식이 오면 다시 보인다.
            const auto now { now_ms() };
            for (const auto& session : data_.sessions)
                if (is_widget_visible(session, now) && !widget_active(session))
                    cleared_[session_key(session)] = latest_update(session);
            scroll_ = 0.0f;
        }
        else if (const auto* preview { message.get<preview_intent>() }; preview != nullptr)
        {
            if (preview->open)
            {
                preview_ = *preview;
                preview_close_at_.reset();
                preview_popup_hover_ = false;
            }
            // 늦게 온 떠남이 다른 카드의 팝업을 닫지 않게 한다. 팝업 안 단추를 누르러 옮겨 가는 사이는 기다린다.
            else if (preview_.owner == preview->owner && !preview_popup_hover_)
                preview_close_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds { 300 };
        }
        else if (const auto* preview_hover { message.get<preview_hover_intent>() }; preview_hover != nullptr)
        {
            preview_popup_hover_ = preview_hover->inside;
            if (preview_hover->inside)
                preview_close_at_.reset();
            else if (!preview_.owner.empty())
                preview_close_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds { 300 };
            // 상태 팝업 카드의 세부 정보에 머무는 동안은 상태 팝업도 남긴다. 상태 팝업에서 나온 포인터가 세부 정보로 갔다.
            if (preview_.surface == status_surface && panel_ == widget_panel::status)
            {
                hover_popup_ = preview_hover->inside;
                if (preview_hover->inside)
                    hover_close_at_.reset();
                else if (!hover_trigger_)
                    hover_close_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds { 300 };
            }
        }
        else if (const auto* clear { message.get<clear_session_intent>() }; clear != nullptr)
        {
            if (const auto found { std::ranges::find_if(data_.sessions, [&](const auto& session) { return session_key(session) == clear->key; }) }; found != data_.sessions.end())
                cleared_[clear->key] = latest_update(*found);
            if (preview_.key == clear->key)
            {
                preview_ = {};
                preview_close_at_.reset();
                preview_popup_hover_ = false;
            }
        }
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
            theme_ = dark() ? luil::theme_preference::light : luil::theme_preference::dark;
        else if (const auto* mode { message.get<theme_mode_intent>() }; mode != nullptr)
            theme_ = mode->theme;
        else if (const auto* accent { message.get<accent_intent>() }; accent != nullptr)
            accent_id_ = accent->id;
        else if (message.get<widget_failed_intent>() != nullptr)
            error_ = "The widget window could not be prepared.";
        else if (const auto* detail { message.get<open_detail_intent>() }; detail != nullptr)
        {
            detail_open_ = !detail_open_ || !detail->key.empty() || detail->keep_open;
            // 세션을 보러 연 것이므로 라이선스 화면에서 세션 목록으로 돌아온다.
            licenses_open_ = false;
            if (!detail->key.empty())
                selected_session_ = detail->key;
        }
        else if (message.get<close_detail_intent>() != nullptr)
        {
            detail_open_ = false;
            licenses_open_ = false;
        }
        else if (const auto* detail_metrics { message.get<detail_metrics_intent>() }; detail_metrics != nullptr)
            detail_metrics_ = *detail_metrics;
        else if (const auto* backdrop { message.get<backdrop_intent>() }; backdrop != nullptr)
            backdrop_ = backdrop->backdrop;
        else if (const auto* opacity { message.get<opacity_intent>() }; opacity != nullptr)
            opacity_ = std::clamp(opacity->value, 0.0f, 1.0f);
        else if (const auto* card { message.get<card_opacity_intent>() }; card != nullptr)
            card_opacity_ = std::clamp(card->relative ? card_opacity_ + card->value : card->value, 0.0f, 1.0f);
        // 판은 한 번에 하나만 뜬다. 닫힘은 지금 그 판이 떠 있을 때만 듣는다 — 늦게 온 닫힘이 새로 연 판을 닫지 않는다.
        else if (const auto* panel { message.get<widget_panel_intent>() }; panel != nullptr)
        {
            const auto now { std::chrono::steady_clock::now() };
            if (panel->open)
            {
                // 판을 연 단추를 눌러 닫은 누름이 뒤늦게 다시 열려는 것이면 듣지 않는다. 닫힘과 그 누름은 한 몸짓이라 사이가 짧다.
                if (panel->panel != dismissed_panel_ || now - dismissed_at_ > std::chrono::milliseconds { 400 })
                    panel_ = panel->panel;
            }
            else if (panel_ == panel->panel)
            {
                panel_ = widget_panel::none;
                if (panel->dismissed)
                {
                    dismissed_panel_ = panel->panel;
                    dismissed_at_ = now;
                }
            }
            if (panel->open || !panel->dismissed)
                dismissed_panel_ = widget_panel::none;
            // 누름으로 연 판은 포인터가 떠나도 닫지 않는다. 머무름 표시는 새로 연 판의 것부터 다시 센다.
            hover_trigger_ = false;
            hover_popup_ = false;
            hover_close_at_.reset();
        }
        else if (const auto* hover { message.get<panel_hover_intent>() }; hover != nullptr)
        {
            // 머무름으로는 다른 판을 밀어내지 않는다. 설정을 고르다 지나친 포인터에 판이 바뀌면 안 된다.
            if (hover->inside && !hover->popup && panel_ == widget_panel::none)
                panel_ = hover->panel;
            // 다른 판의 늦은 들고 남은 지금 판의 닫힘을 정하지 않는다.
            if (panel_ == hover->panel)
            {
                (hover->popup ? hover_popup_ : hover_trigger_) = hover->inside;
                if (hover->inside)
                    hover_close_at_.reset();
                else if (!hover_trigger_ && !hover_popup_)
                    hover_close_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds { 300 };
            }
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
            pinned_ = widget->pinned;
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
            float& offset { scroll->status ? status_scroll_ : scroll->updates ? update_scroll_ : scroll->detail ? detail_scroll_ : scroll_ };
            offset = std::max(0.0f, scroll->absolute ? scroll->value : offset + scroll->value);
        }
        else if (const auto* licenses { message.get<licenses_intent>() }; licenses != nullptr)
        {
            licenses_open_ = !licenses_open_ || licenses->keep_open;
            detail_open_ = detail_open_ || licenses->keep_open;
        }
        else if (const auto* license { message.get<select_license_intent>() }; license != nullptr)
        {
            if (license->index != license_)
                license_text_scroll_ = 0.0f;
            license_ = license->index;
        }
        else if (const auto* license_scroll { message.get<license_scroll_intent>() }; license_scroll != nullptr)
        {
            float& offset { license_scroll->text ? license_text_scroll_ : license_list_scroll_ };
            offset = std::max(0.0f, license_scroll->absolute ? license_scroll->value : offset + license_scroll->value);
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
        auto next { next_tick_ };
        for (const auto& close : { hover_close_at_, preview_close_at_ })
            if (close.has_value())
                next = std::min(next, *close);
        return next;
    }

    void board_driver::tick(const std::chrono::steady_clock::time_point now)
    {
        if (hover_close_at_.has_value() && now >= *hover_close_at_)
        {
            hover_close_at_.reset();
            if (!hover_trigger_ && !hover_popup_)
                panel_ = widget_panel::none;
        }
        if (preview_close_at_.has_value() && now >= *preview_close_at_)
        {
            preview_close_at_.reset();
            if (!preview_popup_hover_)
                preview_ = {};
        }
        // 머무른 판을 닫으려고 일찍 깬 것이면 세션은 다시 읽지 않는다.
        if (now < next_tick_)
            return;
        if (changes_ == INVALID_HANDLE_VALUE || codex_ != nullptr || claude_ != nullptr || antigravity_ != nullptr)
            reload();
        else if (WaitForSingleObject(changes_, 0) == WAIT_OBJECT_0)
        {
            FindNextChangeNotification(changes_);
            reload();
        }
        save_appearance();
        next_tick_ = now + std::chrono::seconds { 1 };
    }

    bool board_driver::dark() const
    {
        return theme_ == luil::theme_preference::system ? system_dark() : theme_ == luil::theme_preference::dark;
    }

    void board_driver::save_appearance()
    {
        if (appearance_file_.empty())
            return;
        const widget_appearance current { .theme = theme_, .accent_id = accent_id_, .backdrop = backdrop_, .opacity = opacity_, .card_opacity = card_opacity_ };
        if (current == saved_appearance_)
            return;
        save_widget_appearance(appearance_file_, current);
        saved_appearance_ = current;
    }

    std::shared_ptr<const luil::ui_frame> board_driver::make_frame()
    {
        const auto now { now_ms() };
        updates_.expire(std::chrono::steady_clock::now());
        if (!notice_.empty() && std::chrono::steady_clock::now() >= notice_until_)
            notice_.clear();
        auto frame { std::make_shared<luil::ui_frame>() };
        frame->appearance.theme = theme_;
        frame->appearance.accent_id = accent_id_;
        frame->style = style_;
        // 세부 정보 팝업의 자리는 카드를 그린 tree가 배치 뒤에 채운다.
        preview_top_.reset();
        actions_right_.reset();
        status_left_.reset();
        frame->tree = std::make_shared<const luil::ui_tree>(make_widget_tree(now));
        // 알림 판과 머릿줄 판은 같은 자리에 선다. 사용자가 연 판이 앞선다.
        if (collapsed_ && !updates_.pending().empty() && panel_ == widget_panel::none)
            make_update_popup(*frame, now);
        make_settings_popup(*frame);
        make_actions_popup(*frame);
        make_status_popup(*frame, now);
        // 세부 정보는 카드를 그린 표면(위젯·알림·상태 팝업)이 자리를 채운 뒤에 만든다.
        make_preview_popup(*frame, now);
        if (detail_open_)
        {
            // 위젯으로 지켜보다가 자세히 볼 때 여는 독립 창이다. 작업 표시줄에 따로 선다.
            luil::ui_window detail {};
            detail.id = u8"detail";
            detail.caption = detail_caption(dark(), licenses_open_);
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
        const auto caption_config { detail_caption(dark(), licenses_open_) };
        const float caption_height { luil::caption_element::height_for(caption_config) * scale };
        auto caption { std::make_unique<luil::caption_element>(caption_config) };
        caption->arrange({ { 0.0f, 0.0f, width, caption_height }, scale });
        root->add(std::move(caption));
        if (licenses_open_)
        {
            luil::virtual_list_element* list_view { nullptr };
            luil::virtual_list_element* text_view { nullptr };
            auto licenses { make_license_body(width / scale, (height - caption_height) / scale, list_view, text_view) };
            licenses->arrange({ { 0.0f, caption_height, width, height - caption_height }, scale });
            // 흘린 양은 목록이 다듬은 값으로 되돌린다. 끝을 넘겨 굴린 휠이 쌓이지 않는다.
            if (list_view != nullptr)
                license_list_scroll_ = list_view->metrics().scroll_offset;
            if (text_view != nullptr)
                license_text_scroll_ = text_view->metrics().scroll_offset;
            root->add(std::move(licenses));
            return root;
        }

        // 맨 앞은 전체, 그 뒤는 provider 표의 순서다.
        std::array<std::size_t, std::size(providers) + 1> provider_counts {};
        std::size_t attention { 0 };
        std::size_t running { 0 };
        for (const auto& session : data_.sessions)
        {
            ++provider_counts[0];
            if (const auto* provider { find_provider(session.provider) }; provider != nullptr)
                ++provider_counts[static_cast<std::size_t>(provider - providers) + 1];
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
        //  - provider가 셋 이상이라 이름 대신 표식과 세션 수를 보인다. 이름은 툴팁과 보조 기술이 말한다.
        std::vector<filter_chip> chips {};
        const auto all_text { u8"전체 " + u8(std::to_string(provider_counts[0])) };
        chips.push_back({ .value = u8"filter-all", .text = all_text, .name = all_text });
        for (std::size_t index { 0 }; index < std::size(providers); ++index)
        {
            const auto count { u8(std::to_string(provider_counts[index + 1])) };
            chips.push_back(
                { .value = u8"filter-" + u8(providers[index].id), .provider = std::string { providers[index].id }, .text = count, .name = std::u8string { providers[index].display } + u8" " + count });
        }
        toolbar->add(std::make_unique<provider_filter_element>(std::move(chips), provider_.empty() ? u8"filter-all" : u8"filter-" + u8(provider_)),
            { .length = filter_width, .cross_length = luil::choice_toggle_height });
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
            for (const auto& [provider, text] : signal_lines(now))
                status->add_fit(provider_line(u8"signal-" + u8(provider), provider, without_provider(provider, text), 11.0f, luil::label_color_role::dim, true, text));
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

    std::unique_ptr<luil::ui_element> board_driver::make_license_body(const float width, const float height, luil::virtual_list_element*& list_view, luil::virtual_list_element*& text_view)
    {
        if (!licenses_.has_value())
            licenses_ = embedded_notices();
        const auto& components { *licenses_ };
        license_ = components.empty() ? 0 : std::min(license_, components.size() - 1);

        auto body { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"license-body" }, luil::stack_config {}) };
        const luil::stack_config toolbar_config {
            .direction = luil::stack_direction::row, .spacing = 12.0f, .padding = { row_inset, 0.0f, row_inset, 0.0f }, .cross_alignment = luil::stack_alignment::center
        };
        auto toolbar { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"license-toolbar" }, toolbar_config) };
        toolbar->add_fit(label(u8"license-title", u8"오픈 소스 라이선스", 14.0f));
        toolbar->add_flexible(
            label(u8"license-summary", u8"이 실행 파일에 들어 있는 제3자 구성 요소 " + u8(std::to_string(components.size())) + u8"개의 라이선스 원문입니다.", 11.0f, luil::label_color_role::dim));
        const luil::text_button_config back_config { .text = u8"세션 목록으로", .glyph = luil::codicons::icon_arrow_left };
        auto back { std::make_unique<luil::text_button_element>(luil::ui_element_id { kind_button, u8"license-back" }, back_config) };
        back->set_action(luil::ui_trigger::left_click, luil::make_message_action(licenses_intent {}));
        toolbar->add_fit(std::move(back));
        body->add(std::move(toolbar), toolbar_height);
        body->add(divider(u8"license-toolbar-rule"), 1.0f);

        if (components.empty())
        {
            const luil::stack_config config { .padding = luil::edge_insets::all(24.0f), .main_alignment = luil::stack_main_alignment::center };
            auto empty { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"license-empty-body" }, config) };
            empty->add(label(u8"license-empty", u8"실행 파일에서 라이선스 고지를 찾지 못했습니다.", 14.0f, luil::label_color_role::primary, luil::text_align::center), 24.0f);
            body->add_flexible(panel(u8"license-empty-surface", std::move(empty)));
            return body;
        }

        const auto pick = [](const std::u8string& key) { return luil::make_app_action(select_license_intent { license_index(key) }); };
        luil::virtual_list_config list {};
        list.owner = u8"license-list";
        for (std::size_t index { 0 }; index < components.size(); ++index)
            list.items.push_back({ .key = license_key(index), .label = u8(components[index].name) });
        list.selected = license_key(license_);
        list.row_height = license_row_height;
        list.scroll_offset = license_list_scroll_;
        list.select = pick;
        list.move = pick;
        list.activate = pick;
        list.scroll = [](const float delta) { return luil::make_app_action(license_scroll_intent { delta, false, false }); };
        list.scroll_to = [](const float offset) { return luil::make_app_action(license_scroll_intent { offset, true, false }); };
        auto list_element { std::make_unique<luil::virtual_list_element>(std::move(list)) };
        list_view = list_element.get();

        // 원문은 고른 구성 요소 하나만 줄로 나눠 둔다. Skia 고지까지 합친 전체는 수천 줄이다.
        const float text_width { width - license_list_width - 1.0f };
        const auto columns { static_cast<std::size_t>(std::max(0.0f, text_width - 2.0f * license_row_padding - luil::list_scrollbar_width) / (style_->metrics.body_font_size * license_glyph_width)) };
        if (license_lines_ == nullptr || license_lines_index_ != license_ || license_lines_columns_ != columns)
        {
            auto lines { std::make_shared<std::vector<luil::virtual_list_item>>() };
            const auto wrapped { wrap_notice_lines(components[license_].text, columns) };
            lines->reserve(wrapped.size());
            for (std::size_t index { 0 }; index < wrapped.size(); ++index)
                lines->push_back({ .key = u8"line-" + u8(std::to_string(index)), .label = u8(wrapped[index]) });
            license_lines_ = std::move(lines);
            license_lines_index_ = license_;
            license_lines_columns_ = columns;
        }
        luil::virtual_list_config text {};
        text.owner = u8"license-text";
        text.shared_items = license_lines_;
        text.row_height = license_line_height;
        text.scroll_offset = license_text_scroll_;
        text.scroll = [](const float delta) { return luil::make_app_action(license_scroll_intent { delta, false, true }); };
        text.scroll_to = [](const float offset) { return luil::make_app_action(license_scroll_intent { offset, true, true }); };
        auto text_element { std::make_unique<luil::virtual_list_element>(std::move(text)) };
        text_view = text_element.get();

        const luil::stack_config heading_config { .direction = luil::stack_direction::row, .padding = { row_inset, 0.0f, row_inset, 0.0f }, .cross_alignment = luil::stack_alignment::center };
        auto heading { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"license-heading" }, heading_config) };
        heading->add_flexible(label(u8"license-name", u8(components[license_].name), 13.0f));
        auto pane { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"license-pane" }, luil::stack_config {}) };
        pane->add(std::move(heading), field_height + 8.0f);
        pane->add(divider(u8"license-heading-rule"), 1.0f);
        pane->add_flexible(std::move(text_element));

        auto content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"license-content" }, luil::stack_config { .direction = luil::stack_direction::row }) };
        auto surface { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, u8"license-list-surface" }, luil::panel_config {}) };
        surface->set_content(std::move(list_element));
        content->add(std::move(surface), license_list_width);
        content->add(divider(u8"license-divider"), 1.0f);
        content->add_flexible(std::move(pane));
        body->add(std::move(content), std::max(0.0f, height - toolbar_height - 1.0f));
        return body;
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

        auto visible { widget_visible(now) };
        const auto active { [](const session_snapshot* session) { return widget_active(*session); } };
        std::ranges::stable_sort(visible, [&](const auto* left, const auto* right) { return active(left) > active(right); });
        const auto attention { std::ranges::count_if(visible, [now](const auto* session) { return group_of(*session, now) == session_group::attention; }) };
        const auto running { std::ranges::count_if(visible, [now](const auto* session) { return group_of(*session, now) == session_group::running; }) };

        // 머릿줄 내용은 캡션 위에 겹친다. luil이 이 캡션의 실제 배치에서 끌기 띠와 시스템 메뉴 자리를 읽는다.
        auto window_caption { std::make_unique<luil::caption_element>(widget_caption()) };
        window_caption->arrange({ { 0.0f, metrics_.header_bottom ? height - caption_height * scale : 0.0f, width, caption_height * scale }, scale });
        root->add(std::move(window_caption));

        // 머릿줄과 바닥 줄이 창 너비 전체를 덮고 그 사이를 목록 영역이 채운다. 창 모서리는 DWM이 둥글게 자른다.
        auto content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-content" }, luil::stack_config {}) };

        // 머릿줄: 키 컬러로 채워 목록과 가른다. 빈 자리의 끌기 판정은 luil의 캡션 배치를 따른다.
        const luil::stack_config caption_config { .direction = luil::stack_direction::row, .spacing = 2.0f, .padding = { 12.0f, 0.0f, 8.0f, 0.0f }, .cross_alignment = luil::stack_alignment::center };
        auto caption { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-header" }, caption_config) };
        caption->add(std::make_unique<luil::glyph_element>(luil::ui_element_id { kind_label, u8"widget-grip" }, luil::glyph_config { .glyph = luil::codicons::icon_gripper, .font_size = 14.0f }),
            { .length = 16.0f, .cross_length = 16.0f });
        caption->add_gap(4.0f);
        caption->add_flexible(label(u8"widget-title", u8"Session Board", 14.0f));
        // 확인·진행·완료·새 알림을 아이콘과 수로 줄여 보인다. 새 알림은 접힌 동안 쌓인 알림이다.
        //  - 넷은 0이어도 늘 같은 자리에 흐리게 서서, 수가 바뀌어도 머릿줄이 흔들리지 않는다.
        const std::vector<header_count> counts {
            { u8"widget-count-attention", u8"확인", luil::codicons::icon_warning, static_cast<std::size_t>(attention) },
            { u8"widget-count-running", u8"진행", 0, static_cast<std::size_t>(running) },
            { u8"widget-count-done", u8"응답", luil::codicons::icon_comment, static_cast<std::size_t>(std::ranges::count_if(visible, std::not_fn(active))) },
            { u8"widget-count-alerts", u8"새 알림", luil::codicons::icon_bell_dot, updates_.pending().size() },
        };
        // 단추를 모을지는 표식 넷이 한 자리 수로 다 선 폭으로 정한다. 그보다 길어진 수는 제목을 줄여 들인다.
        const float counts_width { std::max(header_counts_reserve, header_counts_width(counts)) };
        auto status { std::make_unique<header_status>(counts) };
        // 머무르거나 누르면 표식마다 센 세션을 보인다. 끌기 띠에서 빼야 머무름과 누름을 받는다.
        status->set_hit_opaque(true);
        status->set_caption_client(true);
        status->set_action(luil::ui_trigger::left_click, luil::make_message_action(widget_panel_intent { widget_panel::status, panel_ != widget_panel::status }));
        press_toggles(*status);
        status->set_hover_dwell(luil::hover_dwell_target {
            .delay = std::chrono::milliseconds { 250 },
            .on_dwell = luil::make_message_action(panel_hover_intent { .panel = widget_panel::status, .popup = false, .inside = true }),
            .on_end = luil::make_message_action(panel_hover_intent { .panel = widget_panel::status, .popup = false, .inside = false }),
        });
        const luil::ui_element* status_mark { status.get() };
        caption->add(std::move(status), counts_width);
        caption->add_gap(4.0f);
        const auto add_button = [&](std::unique_ptr<luil::button_element> button) {
            button->set_caption_client(true);
            caption->add(std::move(button), { .length = widget_icon_button, .cross_length = widget_icon_button });
        };
        // 단추가 제목과 상태 표식을 밀어낼 만큼 좁으면 접기·닫기만 남기고 나머지를 하나로 모은다. 모은 단추는 머무르거나 누르면 펼친다.
        auto actions { widget_actions() };
        const float fixed_width { 12.0f + 8.0f + 16.0f + 4.0f + header_title_minimum + header_counts_reserve + 4.0f };
        const auto header_width = [&](const std::size_t buttons) { return fixed_width + widget_icon_button * static_cast<float>(buttons) + header_spacing * static_cast<float>(buttons + 4); };
        const luil::ui_element* actions_trigger { nullptr };
        if (inner_width >= header_width(actions.size() + 2))
            for (auto& button : actions)
                add_button(std::move(button));
        else
        {
            const bool open { panel_ == widget_panel::actions };
            auto more { widget_button(u8"widget-more", u8"단추 더 보기", luil::codicons::icon_ellipsis, luil::make_message_action(widget_panel_intent { widget_panel::actions, !open }), open) };
            press_toggles(*more);
            more->set_hover_dwell(luil::hover_dwell_target {
                .delay = std::chrono::milliseconds { 150 },
                .on_dwell = luil::make_message_action(panel_hover_intent { .panel = widget_panel::actions, .popup = false, .inside = true }),
                .on_end = luil::make_message_action(panel_hover_intent { .panel = widget_panel::actions, .popup = false, .inside = false }),
            });
            actions_trigger = more.get();
            add_button(std::move(more));
        }
        // 접기는 위젯의 첫 단추라 모으지 않는다.
        const bool points_down { collapsed_ != metrics_.header_bottom };
        add_button(widget_button(u8"widget-collapse", collapsed_ ? u8"목록 펼치기" : u8"목록 접기", points_down ? luil::codicons::icon_chevron_down : luil::codicons::icon_chevron_up,
            command_action(toggle_collapse_command)));
        add_button(widget_button(u8"compact-close", u8"위젯 닫기", luil::codicons::icon_chrome_close, command_action(0)));
        // 모은 단추를 펼친 채 머릿줄이 넓어지면 단추가 제자리로 돌아온다. 보이지 않는 팝업이 알림 판을 막지 않게 닫는다.
        if (panel_ == widget_panel::actions && actions_trigger == nullptr)
            panel_ = widget_panel::none;
        luil::panel_config caption_surface {};
        caption_surface.background = [](const luil::ui_color_palette& palette) { return palette.accent; };
        auto caption_panel { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, u8"widget-caption" }, caption_surface) };
        caption_panel->set_content(std::make_unique<palette_scope>(u8"widget-caption-tint", caption_tint, std::move(caption)));
        if (!metrics_.header_bottom)
            content->add(std::move(caption_panel), caption_height);

        luil::scroll_area_element* scroll_metrics { nullptr };
        // 세부 정보를 띄운 카드다. 배치 뒤에 그 자리로 팝업을 맞춘다.
        const luil::ui_element* preview_card { nullptr };
        if (!collapsed_)
        {
            const luil::stack_config region_config { .spacing = widget_gap, .padding = luil::edge_insets::all(widget_gap) };
            auto region { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-region-body" }, region_config) };
            const float used { caption_height + 1.0f + widget_footer_height + 2.0f * widget_gap };

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
            // 카드도 창의 흐림을 비춘다. 투명 배경은 흐리지 않아 뒤 글이 그대로 비치므로, 불투명은 비칠 것이 없으므로 그대로 칠한다.
            const float card_alpha { cards_translucent() ? card_opacity_ : 1.0f };
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
                // 묶음 사이는 카드 사이보다 넓게 띄워 묶음이 갈리게 한다.
                if (content_height > 0.0f)
                {
                    rows->add_gap(widget_group_gap);
                    content_height += widget_group_gap;
                }
                // 끝난 묶음은 정리 단추로 한꺼번에 치운다. 새 소식이 오면 그 세션은 다시 나타난다.
                std::unique_ptr<luil::ui_element> clear {};
                if (!current)
                    clear = widget_button(u8"widget-clear-completed", u8"최근 응답 · 종료 목록 정리", luil::codicons::icon_clear_all, luil::make_message_action(clear_completed_intent {}));
                add_block(widget_group_heading(group_id, current ? u8"진행 중 · 대기" : u8"최근 응답 · 종료", members.size(), current ? u8"" : u8"15분간 유지", std::move(clear)), widget_group_header);
                for (const auto* session : members)
                {
                    const auto owner { widget_owner(*session) };
                    const bool previewing { preview_.surface.empty() && preview_.owner == owner };
                    auto row { widget_row(*session, now, row_width, expanded_session_ == owner, previewing, card_alpha) };
                    if (previewing)
                        preview_card = row.element.get();
                    add_block(std::move(row.element), row.height);
                }
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
                region->add_flexible(std::make_unique<scroll_fade>(u8"widget-scroll-fade", std::move(scroll)));
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
                // 첫 줄의 이름은 provider를 붙이지 않는다. UIA 검증이 이 이름으로 바닥 줄을 찾는다.
                bool first { true };
                for (const auto& [provider, text] : signal_lines(now))
                {
                    const std::u8string id { first ? std::u8string { u8"widget-signal" } : u8"widget-signal-" + u8(provider) };
                    footer->add_flexible(provider_line(id, provider, short_signal(provider, text), 11.0f, luil::label_color_role::dim, false, text));
                    first = false;
                }
            }
            luil::panel_config footer_surface {};
            footer_surface.background = [](const luil::ui_color_palette& palette) { return palette.surface_background; };
            auto footer_panel { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, u8"widget-footer" }, footer_surface) };
            footer_panel->set_content(std::move(footer));
            if (metrics_.header_bottom)
            {
                content->add(std::move(footer_panel), widget_footer_height);
                content->add(divider(u8"widget-footer-rule"), 1.0f);
                content->add_flexible(std::move(region_panel));
            }
            else
            {
                content->add_flexible(std::move(region_panel));
                content->add(divider(u8"widget-footer-rule"), 1.0f);
                content->add(std::move(footer_panel), widget_footer_height);
            }
        }
        if (metrics_.header_bottom)
            content->add(std::move(caption_panel), caption_height);
        content->arrange({ { 0.0f, 0.0f, width, height }, scale });
        if (scroll_metrics != nullptr)
            scroll_ = scroll_metrics->metrics().scroll_offset;
        if (preview_card != nullptr)
            preview_top_ = preview_card->bounds().y / scale;
        if (actions_trigger != nullptr)
            actions_right_ = (actions_trigger->bounds().x + actions_trigger->bounds().width) / scale;
        status_left_ = status_mark->bounds().x / scale;
        root->add(std::move(content));
        if (!collapsed_)
        {
            // 머릿줄 반대쪽의 오른쪽 모서리에서 크기를 조절한다. 이 표시는 자리를 알릴 뿐이다.
            const luil::glyph_config grip { .glyph = luil::codicons::icon_grabber, .font_size = 12.0f, .description = u8"크기 조절" };
            auto resize { std::make_unique<luil::glyph_element>(luil::ui_element_id { kind_label, u8"widget-resize" }, grip) };
            resize->set_tooltip(u8"크기 조절");
            resize->arrange({ { width - 14.0f * scale, metrics_.header_bottom ? 0.0f : height - 14.0f * scale, 14.0f * scale, 14.0f * scale }, scale });
            root->add(std::move(resize));
        }
        return root;
    }

    void board_driver::make_update_popup(luil::ui_frame& frame, const std::int64_t now)
    {
        constexpr float heading_height { 32.0f };
        constexpr float gap { 6.0f };
        constexpr float card_padding { 8.0f };
        constexpr float line_spacing { 4.0f };
        constexpr float text_height { 18.0f };
        constexpr float note_height { 16.0f };
        const float scale { metrics_.scale > 0.0f ? metrics_.scale : 1.0f };
        const float width { metrics_.width / scale };
        const auto count { updates_.pending().size() };
        const luil::stack_config rows_config { .spacing = gap, .padding = { gap, 0.0f, gap, gap } };
        auto rows { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"update-rows" }, rows_config) };
        const luil::ui_element* preview_card { nullptr };
        // 카드 높이는 덧붙일 줄이 있는지에 따라 다르다. 앞의 세 장까지 펼쳐 보이고 그 넘게는 흘린다.
        float content_height { 0.0f };
        float shown_height { 0.0f };
        std::size_t index { 0 };
        for (const auto& update : updates_.pending())
        {
            const auto& session { update.session };
            const auto id { u8"update-" + u8(std::to_string(update.sequence)) };
            const acknowledge_update_intent acknowledgement { session.provider, session.session_id, update.sequence };
            auto body { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-body" }, luil::stack_config { .spacing = line_spacing, .padding = { 12.0f, card_padding, 8.0f, card_padding } }) };
            // 머리 줄: 상태가 맨 앞이고 그 옆에 제품 표식과 프로젝트, 오른쪽에 시각과 단추다.
            //  - 응답이 처리된 알림은 세션의 지금 상태보다 처리됐다는 사실이 소식이다.
            const luil::stack_config heading_config { .direction = luil::stack_direction::row, .spacing = 8.0f, .cross_alignment = luil::stack_alignment::center };
            auto heading { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-heading" }, heading_config) };
            if (update.response_required || update.completed)
                heading->add_fit(status_inline(id + u8"-status", session, now));
            else
                heading->add_fit(label(id + u8"-status", u8"응답 처리됨", 11.0f, luil::label_color_role::dim));
            heading->add_flexible(provider_line(id + u8"-project", session.provider, project_text(session), 11.0f));
            heading->add_fit(label(id + u8"-age", age_text(latest_update(session), now), 11.0f, luil::label_color_role::dim));
            // 목록을 접어 둔 채로도 알린 세션으로 바로 갈 수 있어야 한다. 갈 수 있는지는 지금 목록의 세션으로 판단한다(창 정보는 Hook이 나중에 채운다).
            const auto current { std::ranges::find_if(data_.sessions, [&](const auto& item) { return session_key(item) == session_key(session); }) };
            const auto& target { current != data_.sessions.end() ? *current : session };
            const std::u8string focus_name { focus_tooltip(target, provider_text(session) + u8" 세션 창으로 이동") };
            auto focus { widget_button(id + u8"-focus", focus_name, luil::codicons::icon_link_external, luil::make_message_action(focus_session_intent { session_key(session) })) };
            focus->set_enabled(can_focus(target));
            heading->add(std::move(focus), { .length = widget_icon_button, .cross_length = widget_icon_button });
            // 확인 단추는 체크 표식 하나다. 응답을 기다리는 알림은 세션에서 답해야 사라지므로 누를 수 없다.
            //  - 묻고 끝난 응답은 답하지 않기로 할 수 있어 숨길 수 있다. 숨긴 그 응답은 새 요청이 오기 전까지 다시 띄우지 않는다.
            const bool reply { update.response_required && awaits_reply(session) };
            const std::u8string confirm_tip { reply           ? u8"답하지 않고 알림을 숨깁니다 — 새 요청이 오기 전까지 다시 띄우지 않습니다"
                    : update.response_required ? u8"세션에서 승인하거나 응답하면 잠시 뒤 자동으로 숨깁니다"
                    : update.completed         ? u8"끝난 작업 알림을 숨깁니다"
                                               : u8"응답이 처리된 알림을 숨깁니다" };
            auto confirm { widget_button(id + u8"-confirm", confirm_tip, luil::codicons::icon_check, luil::make_message_action(acknowledgement)) };
            confirm->set_access_name(update.response_required && !reply ? u8"응답 대기" : u8"닫기");
            confirm->set_enabled(!update.response_required || reply);
            heading->add(std::move(confirm), { .length = widget_icon_button, .cross_length = widget_icon_button });
            body->add(std::move(heading), widget_icon_button);
            // 끝난 작업은 무엇을 요청했는지가 먼저다. 요청 발췌문이 있으면 그것을 본문에, 결과 문장은 아랫줄에 둔다.
            const bool request { update.completed && !session.title.empty() };
            const std::u8string text { request ? u8(session.title) : task_text(session) };
            body->add(label(id + u8"-text", text, 12.0f), text_height);
            // 덧붙일 줄은 머리의 상태와 본문이 말하지 않은 것만이다: 기다리는 까닭, 오류의 원인, 요청 아래의 결과.
            std::u8string note {};
            //  - 묻고 끝난 응답은 본문이 그 물음이라 덧붙일 까닭이 없다.
            if ((update.response_required && !reply) || session.status == session_status::failed)
                note = activity_text(session, now);
            else if (request && !session.summary.empty())
                note = u8(session.summary);
            const bool has_note { !note.empty() && note != text && note != state_text(session, now) };
            if (has_note)
                body->add(label(id + u8"-activity", note, 11.0f, luil::label_color_role::dim), note_height);
            const float card_height { 2.0f * card_padding + widget_icon_button + line_spacing + text_height + (has_note ? line_spacing + note_height : 0.0f) };
            const bool previewing { preview_.surface == update_surface && preview_.owner == id };
            auto card { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, id }, widget_card_surface(false, previewing)) };
            card->set_content(std::move(body));
            card->set_hit_opaque(true);
            card->set_access_name(provider_text(session) + u8" · " + text);
            set_preview(*card, id, session_key(session), std::u8string { update_surface });
            if (previewing)
                preview_card = card.get();
            rows->add(std::move(card), card_height);
            content_height += card_height + gap;
            if (index++ < 3)
                shown_height += card_height + gap;
        }
        const float height { std::min(heading_height + shown_height, metrics_.notification_room / scale - gap) };
        if (width <= 0.0f || height < heading_height + 24.0f)
            return;
        luil::scroll_area_element* scroll_metrics { nullptr };
        auto content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"update-content" }, luil::stack_config {}) };
        const luil::stack_config heading_config {
            .direction = luil::stack_direction::row, .spacing = 6.0f, .padding = { 12.0f, 0.0f, gap + 4.0f, 0.0f }, .cross_alignment = luil::stack_alignment::center
        };
        auto heading { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"update-heading" }, heading_config) };
        const auto waiting { static_cast<std::size_t>(std::ranges::count_if(updates_.pending(), [](const auto& update) { return update.response_required; })) };
        const auto completed { static_cast<std::size_t>(std::ranges::count_if(updates_.pending(), [](const auto& update) { return update.completed; })) };
        const auto resolved { count - waiting - completed };
        std::u8string heading_text {};
        for (const auto& [number, name] : { std::pair { waiting, u8"확인 필요 " }, std::pair { completed, u8"작업 끝남 " }, std::pair { resolved, u8"응답 처리됨 " } })
            if (number > 0)
                heading_text += (heading_text.empty() ? std::u8string {} : std::u8string { u8" · " }) + name + u8(std::to_string(number)) + u8"개";
        // 접힌 위젯은 바닥 줄이 없다. 세션 창으로 가지 못한 이유는 알림 머리에서 말한다.
        if (!notice_.empty())
            heading->add_flexible(label(u8"update-notice", notice_, 11.0f, luil::label_color_role::error));
        else
            heading->add_flexible(label(u8"update-count", heading_text, 11.0f, luil::label_color_role::dim));
        // 응답을 기다리지 않는 알림은 한꺼번에 숨긴다.
        if (waiting < count)
            heading->add(widget_button(u8"update-clear", u8"알림 모두 숨기기", luil::codicons::icon_check_all, luil::make_message_action(acknowledge_all_intent {})),
                { .length = widget_icon_button, .cross_length = widget_icon_button });
        content->add(std::move(heading), heading_height);
        if (content_height > height - heading_height)
        {
            luil::scroll_area_config config { .owner = u8"update-scroll", .content_height = content_height, .scroll_offset = update_scroll_ };
            config.scroll = [](const float delta) { return luil::make_app_action(scroll_intent { .value = delta, .updates = true }); };
            config.scroll_to = [](const float value) { return luil::make_app_action(scroll_intent { .value = value, .absolute = true, .updates = true }); };
            auto scroll { std::make_unique<luil::scroll_area_element>(std::move(config)) };
            scroll_metrics = scroll.get();
            scroll->set_content(std::move(rows));
            content->add_flexible(std::move(scroll));
        }
        else
        {
            update_scroll_ = 0.0f;
            content->add(std::move(rows), content_height);
        }
        auto background { std::make_unique<popup_backing>(u8"update-background", std::move(content)) };
        auto root { std::make_unique<luil::root_element>() };
        root->arrange({ { 0.0f, 0.0f, width * scale, height * scale }, scale });
        background->arrange({ { 0.0f, 0.0f, width * scale, height * scale }, scale });
        if (scroll_metrics != nullptr)
            update_scroll_ = scroll_metrics->metrics().scroll_offset;
        if (preview_card != nullptr)
            preview_top_ = preview_card->bounds().y / scale;
        root->add(std::move(background));
        luil::ui_popup popup {};
        popup.id = std::u8string { update_surface };
        popup.y = metrics_.header_bottom ? -height - gap : static_cast<float>(widget_caption_height) + gap;
        popup.width = width;
        popup.height = height;
        popup.backdrop = luil::window_backdrop::acrylic;
        popup.tree = std::make_shared<const luil::ui_tree>(std::move(root));
        frame.popups.push_back(std::move(popup));
    }

    void board_driver::make_preview_popup(luil::ui_frame& frame, const std::int64_t now)
    {
        constexpr float preview_width { 380.0f };
        constexpr float gap { 8.0f };
        constexpr float inset { 16.0f };
        constexpr float spacing { 12.0f };
        constexpr float key_width { 44.0f };
        constexpr float box_padding { 10.0f };
        if (preview_.owner.empty() || !preview_top_.has_value())
            return;
        // 알림 카드의 세션은 목록에서 빠졌을 수 있다. 그때는 알림이 든 마지막 모습을 보인다.
        const session_snapshot* session { nullptr };
        if (const auto found { std::ranges::find_if(data_.sessions, [&](const auto& item) { return session_key(item) == preview_.key; }) }; found != data_.sessions.end())
            session = &*found;
        else if (const auto pending { std::ranges::find_if(updates_.pending(), [&](const auto& item) { return session_key(item.session) == preview_.key; }) }; pending != updates_.pending().end())
            session = &pending->session;
        if (session == nullptr)
            return;
        // 카드가 놓인 표면을 덮지 않는 쪽에 둔다. 덮으면 카드에서 떠난 것이 되어 팝업이 닫혔다 열리기를 되풀이한다.
        //  - 위젯과 알림 판의 카드는 위젯 밖 왼쪽(없으면 오른쪽)이다.
        //  - 상태 팝업은 위젯 밖으로 비켜 서 있으므로 그 팝업 옆에 붙인다. 위젯 옆에 두면 상태 팝업과 겹친다.
        const float scale { metrics_.scale > 0.0f ? metrics_.scale : 1.0f };
        const float need { (preview_width + gap) * scale };
        const float surface_width { metrics_.width / scale };
        float x { 0.0f };
        if (preview_.surface == status_surface)
        {
            const float right { metrics_.room_right + (surface_width - status_popup_x_ - status_popup_width_) * scale };
            const float left { metrics_.room_left + status_popup_x_ * scale };
            if (right >= need)
                x = status_popup_width_ + gap;
            else if (left >= need)
                x = -(preview_width + gap);
            else
                return;
        }
        else if (metrics_.room_left >= need)
            x = -(preview_width + gap);
        else if (metrics_.room_right >= need)
            x = surface_width + gap;
        else
            return;

        const session_tone tone { tone_of(*session, now) };
        const float text_width { preview_width - 2.0f * inset };
        const luil::stack_config content_config { .spacing = spacing, .padding = luil::edge_insets::all(inset) };
        auto content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"preview-content" }, content_config) };
        float height { 2.0f * inset };
        const auto add = [&](std::unique_ptr<luil::ui_element> element, const float length) {
            if (height > 2.0f * inset)
                height += spacing;
            content->add(std::move(element), length);
            height += length;
        };
        // 이름 붙은 한 덩이다: 흐린 이름 한 줄 아래 내용. 이름과 내용 사이는 덩이 사이보다 좁다.
        const auto section = [&](const std::u8string& id, const std::u8string& name, std::unique_ptr<luil::ui_element> body, const float body_height) {
            auto block { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-block" }, luil::stack_config { .spacing = 4.0f }) };
            block->add(label(id + u8"-name", name, 11.0f, luil::label_color_role::dim), 16.0f);
            block->add(std::move(body), body_height);
            add(std::move(block), 16.0f + 4.0f + body_height);
        };
        const auto wrapped = [&](const std::u8string& id, const std::u8string& text, const float width, const std::size_t lines, std::unique_ptr<luil::stack_element>& out) {
            out = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-lines" }, luil::stack_config {});
            return add_wrapped(*out, id, text, width, 12.0f, lines);
        };

        // 머리: 제품 표식 칸, 폴더 이름과 경로, 시각.
        const luil::stack_config header_config { .direction = luil::stack_direction::row, .spacing = 10.0f, .cross_alignment = luil::stack_alignment::center };
        auto header { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"preview-header" }, header_config) };
        const luil::stack_config mark_config { .padding = luil::edge_insets::all(8.0f) };
        auto mark_body { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"preview-mark-body" }, mark_config) };
        mark_body->add(std::make_unique<provider_mark>(u8"preview-mark", session->provider), 16.0f);
        luil::panel_config mark_surface {};
        mark_surface.background = [](const luil::ui_color_palette& palette) { return luil::with_alpha(palette.primary_foreground, 0.07f); };
        mark_surface.corner_radius = 8.0f;
        auto mark { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_layout, u8"preview-mark-surface" }, mark_surface) };
        mark->set_content(std::move(mark_body));
        mark->set_hit_opaque(false);
        header->add(std::move(mark), { .length = 32.0f, .cross_length = 32.0f });
        auto names { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"preview-names" }, luil::stack_config { .spacing = 2.0f }) };
        names->add(label(u8"preview-source", project_text(*session), 13.0f), 18.0f);
        names->add(label(u8"preview-path", session->cwd.empty() ? u8"폴더 정보 없음" : u8(session->cwd), 11.0f, luil::label_color_role::dim), 16.0f);
        header->add_flexible(std::move(names));
        header->add_fit(label(u8"preview-age", age_text(latest_update(*session), now), 11.0f, luil::label_color_role::dim));
        add(std::move(header), 36.0f);

        // 상태: 상태 칸(경고·오류는 배지, 작업 중은 도는 표시)과 대화 이름.
        const luil::stack_config state_config { .direction = luil::stack_direction::row, .spacing = 10.0f, .cross_alignment = luil::stack_alignment::center };
        auto state { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"preview-state" }, state_config) };
        state->add(status_cell(u8"preview-status", *session, now), 132.0f);
        if (!session->name.empty())
            state->add_flexible(label(u8"preview-conversation", u8"대화 · " + u8(session->name), 11.0f, luil::label_color_role::dim));
        else
            state->add_flexible_gap();
        add(std::move(state), 22.0f);

        // 확인할 일은 왜 멈췄는지가 먼저다. 그 뜻의 색으로 옅게 칠한 상자에 담는다.
        if (tone == session_tone::attention || tone == session_tone::failed)
        {
            std::u8string title { u8"세션 상태를 확인해야 합니다" };
            if (session->status == session_status::waiting_approval)
                title = u8"세션에서 승인해야 합니다";
            else if (session->status == session_status::waiting_input)
                title = u8"세션에서 답해야 합니다";
            else if (session->status == session_status::failed)
                title = u8"오류로 멈췄습니다";
            else if (!own_attention(*session, now))
                title = u8"하위 작업이 기다립니다";
            else if (needs_attention(*session, now))
                title = u8"오래 소식이 없습니다";
            const luil::stack_config reason_config { .spacing = 3.0f, .padding = { box_padding, 8.0f, box_padding, 8.0f } };
            auto reason { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"preview-reason" }, reason_config) };
            reason->add(tinted(u8"preview-reason-tint", text_tint_of(tone), label(u8"preview-reason-title", title, 11.0f)), 16.0f);
            float reason_height { 16.0f + 16.0f + 3.0f };
            reason_height += add_wrapped(*reason, u8"preview-reason-text", activity_text(*session, now), text_width - 2.0f * box_padding, 12.0f, 3);
            add(soft_box(u8"preview-reason-box", tone, 0.12f, 0.35f, std::move(reason)), reason_height);
        }

        // 요청: 무엇을 시켰는지다. 옅은 상자에 인용처럼 담는다.
        if (!session->title.empty())
        {
            const luil::stack_config request_config { .padding = { box_padding, 8.0f, box_padding, 8.0f } };
            auto request { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"preview-request-box-body" }, request_config) };
            const float request_height { 16.0f + add_wrapped(*request, u8"preview-request", u8(session->title), text_width - 2.0f * box_padding, 12.0f, 4) };
            section(u8"preview-request", u8"요청", soft_box(u8"preview-request-box", session_tone::done, 0.05f, 0.0f, std::move(request)), request_height);
        }

        // 결과 또는 지금 하는 일이다. 일하는 중이면 지금 무엇을 하는지가, 끝났으면 무엇을 했는지가 궁금하다.
        {
            const bool working { tone == session_tone::running };
            const std::u8string text { working ? activity_text(*session, now) : u8(session->summary) };
            if (!text.empty())
            {
                std::unique_ptr<luil::stack_element> body {};
                const float body_height { wrapped(u8"preview-summary", text, text_width, working ? 3 : 5, body) };
                section(u8"preview-summary", working ? u8"지금" : u8"결과", std::move(body), body_height);
            }
            if (working && !session->summary.empty())
            {
                std::unique_ptr<luil::stack_element> body {};
                const float body_height { wrapped(u8"preview-progress", u8(session->summary), text_width, 3, body) };
                section(u8"preview-progress", u8"진행", std::move(body), body_height);
            }
        }

        // 하위 작업: 위젯 카드를 펼친 것과 같은 줄이다.
        if (!session->children.empty())
        {
            auto children { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"preview-children-body" }, luil::stack_config {}) };
            const float children_height { add_widget_children(*children, *session, u8"preview", now, true) };
            add(std::move(children), children_height);
        }

        add(divider(u8"preview-rule"), 1.0f);

        // 어디서 무엇으로 도는 세션인지다. 이름이 왼쪽, 값이 오른쪽에 줄을 맞춘다. 폴더는 머리에 있다.
        {
            std::vector<std::pair<std::u8string, std::u8string>> facts {};
            if (!session->model.empty())
                facts.emplace_back(u8"모델", u8(session->model));
            if (!session->observation_source.empty())
                facts.emplace_back(u8"수집", source_text(*session));
            facts.emplace_back(u8"세션", u8(session->session_id));
            const luil::stack_config facts_config { .spacing = 2.0f };
            auto rows { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"preview-facts" }, facts_config) };
            for (std::size_t index { 0 }; index < facts.size(); ++index)
            {
                const auto& [name, value] { facts[index] };
                const std::u8string id { u8"preview-fact-" + u8(std::to_string(index)) };
                const luil::stack_config row_config { .direction = luil::stack_direction::row, .spacing = 8.0f };
                auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, id + u8"-row" }, row_config) };
                row->add(label(id + u8"-name", name, 11.0f, luil::label_color_role::dim), key_width);
                row->add_flexible(label(id, value, 11.0f));
                rows->add(std::move(row), 16.0f);
            }
            add(std::move(rows), 16.0f * static_cast<float>(facts.size()) + 2.0f * static_cast<float>(facts.size() - 1));
        }

        // 동작: 세션 창으로 가기가 주 동작이다. 답해야 하는 세션이면 그 말로 부른다. 끝난 세션은 목록에서 치울 수 있다.
        {
            const luil::stack_config actions_config { .direction = luil::stack_direction::row, .spacing = 6.0f, .cross_alignment = luil::stack_alignment::center };
            auto actions { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"preview-actions" }, actions_config) };
            const bool answer { session->status == session_status::waiting_approval || session->status == session_status::waiting_input };
            const luil::text_button_config focus_config {
                .text = answer ? (session->status == session_status::waiting_approval ? u8"승인하러 가기" : u8"답하러 가기") : u8"세션 창으로 이동",
                .visual = luil::text_button_visual::accent,
                .glyph = luil::codicons::icon_link_external,
            };
            auto focus { std::make_unique<luil::text_button_element>(luil::ui_element_id { kind_button, u8"preview-focus" }, focus_config) };
            focus->set_cursor(luil::ui_cursor::hand);
            focus->set_tooltip(focus_tooltip(*session, provider_text(*session) + u8" 세션 창으로 이동"));
            focus->set_enabled(can_focus(*session));
            focus->set_action(luil::ui_trigger::left_click, luil::make_message_action(focus_session_intent { session_key(*session) }));
            actions->add_fit(std::move(focus));
            if (!session->cwd.empty())
            {
                const luil::text_button_config open_config { .text = u8"폴더 열기", .glyph = luil::codicons::icon_folder_opened };
                auto open { std::make_unique<luil::text_button_element>(luil::ui_element_id { kind_button, u8"preview-open-folder" }, open_config) };
                open->set_cursor(luil::ui_cursor::hand);
                open->set_tooltip(u8"탐색기에서 작업 폴더를 엽니다");
                open->set_action(luil::ui_trigger::left_click, luil::make_message_action(open_folder_intent { u8(session->cwd) }));
                actions->add_fit(std::move(open));
            }
            actions->add_flexible_gap();
            if (tone == session_tone::done)
                actions->add(widget_button(u8"preview-clear", u8"목록에서 치우기 — 새 소식이 오면 다시 보입니다", luil::codicons::icon_check,
                                 luil::make_message_action(clear_session_intent { session_key(*session) })),
                    { .length = 28.0f, .cross_length = 28.0f });
            add(std::move(actions), 28.0f);
        }

        auto background { std::make_unique<popup_backing>(u8"preview-background", std::move(content)) };
        // 단추를 누르러 카드에서 이 팝업으로 옮겨 오는 동안 닫지 않는다.
        background->set_hover_region(luil::hover_region_target {
            .on_enter = [] { return luil::make_app_action(preview_hover_intent { true }); },
            .on_leave = [] { return luil::make_app_action(preview_hover_intent { false }); },
        });
        auto root { std::make_unique<luil::root_element>() };
        root->arrange({ { 0.0f, 0.0f, preview_width * scale, height * scale }, scale });
        background->arrange({ { 0.0f, 0.0f, preview_width * scale, height * scale }, scale });
        root->add(std::move(background));
        luil::ui_popup popup {};
        popup.id = u8"session-preview";
        popup.anchor = preview_.surface;
        popup.x = x;
        popup.y = std::max(0.0f, *preview_top_);
        popup.width = preview_width;
        popup.height = height;
        popup.backdrop = luil::window_backdrop::acrylic;
        popup.tree = std::make_shared<const luil::ui_tree>(std::move(root));
        frame.popups.push_back(std::move(popup));
    }

    bool board_driver::cards_translucent() const noexcept
    {
        return !fully_opaque(opacity_) && backdrop_ != widget_backdrop::transparent;
    }

    std::vector<const session_snapshot*> board_driver::widget_visible(const std::int64_t now) const
    {
        // 정리한 끝난 세션은 그 뒤에 새 소식이 올 때까지 뺀다.
        const auto cleared = [this](const session_snapshot& session) {
            const auto found { cleared_.find(session_key(session)) };
            return found != cleared_.end() && !widget_active(session) && latest_update(session) <= found->second;
        };
        std::vector<const session_snapshot*> visible {};
        for (const auto& session : data_.sessions)
            if (is_widget_visible(session, now) && !cleared(session))
                visible.push_back(&session);
        return visible;
    }

    void board_driver::make_status_popup(luil::ui_frame& frame, const std::int64_t now)
    {
        constexpr float popup_width { 360.0f };
        constexpr float inset { 10.0f };
        constexpr float spacing { 6.0f };
        constexpr float heading_height { 24.0f };
        constexpr float card_height { 74.0f };
        constexpr float more_height { 26.0f };
        constexpr float empty_height { 48.0f };
        constexpr float maximum_height { 560.0f };
        if (panel_ != widget_panel::status || !status_left_.has_value())
            return;
        const auto visible { widget_visible(now) };
        std::vector<const session_snapshot*> attention {};
        std::vector<const session_snapshot*> running {};
        std::vector<const session_snapshot*> done {};
        // 머릿줄 표식과 같은 규칙으로 센다. 오류로 끝난 세션은 확인과 완료 양쪽에 든다.
        for (const auto* session : visible)
        {
            if (const auto group { group_of(*session, now) }; group == session_group::attention)
                attention.push_back(session);
            else if (group == session_group::running)
                running.push_back(session);
            if (!widget_active(*session))
                done.push_back(session);
        }
        // 새 알림 가운데 이미 확인 필요에 든 세션은 빼서 같은 카드가 두 번 서지 않게 한다.
        std::vector<const session_snapshot*> alerts {};
        for (const auto& update : updates_.pending())
            if (std::ranges::none_of(attention, [&](const auto* session) { return session_key(*session) == session_key(update.session); }))
                alerts.push_back(&update.session);

        // 묶음 하나다. 확인할 일이 먼저고, 새 알림은 접힌 동안만 쌓이므로 그때만 보인다.
        struct status_section
        {
            std::u8string id {};
            std::u8string name {};
            // 0이면 작업 중 회전 표시다.
            char32_t glyph { 0 };
            palette_tint tint { nullptr };
            const std::vector<const session_snapshot*>* sessions { nullptr };
            // 이만큼만 카드로 보이고 나머지는 상세 화면으로 넘긴다.
            std::size_t shown { 3 };
            bool clear { false };
        };
        const std::array<status_section, 4> sections { {
            { u8"status-attention", u8"확인 필요", luil::codicons::icon_warning, warning_tint, &attention, 3, false },
            { u8"status-alerts", u8"새 알림", luil::codicons::icon_bell_dot, accent_tint, &alerts, 2, false },
            { u8"status-running", u8"진행 중", 0, accent_tint, &running, 3, false },
            { u8"status-done", u8"최근 응답 · 종료", luil::codicons::icon_comment, nullptr, &done, 2, true },
        } };

        auto list { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"status-list" }, luil::stack_config { .spacing = spacing }) };
        float content_height { 0.0f };
        const auto add = [&](std::unique_ptr<luil::ui_element> element, const float length) {
            if (content_height > 0.0f)
                content_height += spacing;
            list->add(std::move(element), length);
            content_height += length;
        };
        // 세부 정보를 띄운 카드다. 배치 뒤에 그 자리로 팝업을 맞춘다.
        const luil::ui_element* preview_card { nullptr };
        for (const auto& section : sections)
        {
            const auto& sessions { *section.sessions };
            if (sessions.empty() || (section.id == u8"status-alerts" && !collapsed_))
                continue;
            // 묶음 머리: 그 뜻의 색으로 물든 표식, 이름, 수. 끝난 묶음은 한꺼번에 치운다.
            const luil::stack_config heading_config { .direction = luil::stack_direction::row, .spacing = 6.0f, .padding = { 4.0f, 0.0f, 0.0f, 0.0f }, .cross_alignment = luil::stack_alignment::center };
            auto heading { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, section.id + u8"-heading" }, heading_config) };
            std::unique_ptr<luil::ui_element> mark {};
            if (section.glyph == 0)
                mark = std::make_unique<luil::spinner_element>(luil::ui_element_id { kind_layout, section.id + u8"-mark" }, luil::spinner_config { .size = 13.0f });
            else
                mark = std::make_unique<luil::glyph_element>(luil::ui_element_id { kind_layout, section.id + u8"-mark" }, luil::glyph_config { .glyph = section.glyph, .font_size = 13.0f });
            mark->set_hit_opaque(false);
            heading->add(tinted(section.id + u8"-mark-tint", section.tint, std::move(mark)), { .length = 13.0f, .cross_length = 13.0f });
            heading->add_fit(label(section.id + u8"-name", section.name, 12.0f));
            heading->add_flexible(label(section.id + u8"-count", u8(std::to_string(sessions.size())), 11.0f, luil::label_color_role::dim));
            if (section.clear)
                heading->add(widget_button(section.id + u8"-clear", u8"최근 응답 · 종료 목록 정리 — 새 소식이 오면 다시 보입니다", luil::codicons::icon_clear_all, luil::make_message_action(clear_completed_intent {})),
                    { .length = widget_icon_button, .cross_length = widget_icon_button });
            add(std::move(heading), heading_height);

            // 카드: 위는 표식·폴더·시각, 가운데는 요청, 아래는 결과나 기다리는 까닭을 그 뜻의 색으로 한 줄.
            for (std::size_t index { 0 }; index < std::min(section.shown, sessions.size()); ++index)
            {
                const auto& session { *sessions[index] };
                const std::u8string card_id { section.id + u8"-" + u8(std::to_string(index)) };
                const bool previewing { preview_.surface == status_surface && preview_.owner == card_id };
                const session_tone tone { tone_of(session, now) };
                const luil::stack_config body_config { .spacing = 3.0f, .padding = { 12.0f, 8.0f, 12.0f, 10.0f } };
                auto body { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, card_id + u8"-body" }, body_config) };
                const luil::stack_config top_config { .direction = luil::stack_direction::row, .spacing = 8.0f, .cross_alignment = luil::stack_alignment::center };
                auto top { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, card_id + u8"-top" }, top_config) };
                top->add_flexible(provider_line(card_id + u8"-source", session.provider, project_text(session), 11.0f));
                top->add_fit(label(card_id + u8"-age", age_text(latest_update(session), now), 11.0f, luil::label_color_role::dim));
                body->add(std::move(top), 16.0f);
                const std::u8string request { session.title.empty() ? task_text(session) : u8(session.title) };
                body->add(label(card_id + u8"-request", request, 12.0f), 18.0f);
                // 기다리거나 일하는 세션은 무엇을 기다리는지·하는지가 상태를 말한다. 끝난 세션은 상태 뒤에 결과를 붙인다.
                std::u8string result { tone == session_tone::done ? state_text(session, now) : activity_text(session, now) };
                if (tone == session_tone::done && !session.summary.empty())
                    result += u8" — " + u8(session.summary);
                const auto tint { text_tint_of(tone) };
                body->add(tinted(card_id + u8"-result-tint", tint, label(card_id + u8"-result", result, 11.0f, tint == nullptr ? luil::label_color_role::dim : luil::label_color_role::primary)), 16.0f);
                auto card { std::make_unique<status_card>(card_id, previewing, std::move(body)) };
                card->set_access_name(provider_text(session) + u8" · " + project_text(session) + u8" · " + request);
                card->set_cursor(luil::ui_cursor::hand);
                // 누르면 상세 화면에서 그 세션을 고른다. 세션 창 이동은 세부 정보의 주 동작이다.
                card->set_action(luil::ui_trigger::left_click, luil::make_message_action(open_detail_intent { .key = session_key(session), .keep_open = true }));
                set_preview(*card, card_id, session_key(session), std::u8string { status_surface });
                if (previewing)
                    preview_card = card.get();
                add(std::move(card), card_height);
            }
            if (sessions.size() > section.shown)
            {
                const luil::stack_config more_config { .direction = luil::stack_direction::row, .spacing = 4.0f, .cross_alignment = luil::stack_alignment::center, .main_alignment = luil::stack_main_alignment::center };
                auto more_line { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, section.id + u8"-more-line" }, more_config) };
                more_line->add_fit(tinted(section.id + u8"-more-tint", accent_text_tint,
                    label(section.id + u8"-more-text", u8(std::to_string(sessions.size() - section.shown)) + u8"개 더 — 상세 화면에서 보기", 11.0f)));
                more_line->add(tinted(section.id + u8"-more-mark-tint", accent_text_tint,
                                   std::make_unique<luil::glyph_element>(luil::ui_element_id { kind_layout, section.id + u8"-more-mark" }, luil::glyph_config { .glyph = luil::codicons::icon_chevron_right, .font_size = 12.0f })),
                    { .length = 12.0f, .cross_length = 12.0f });
                luil::panel_config more_surface {};
                more_surface.background = [](const luil::ui_color_palette& palette) { return luil::with_alpha(palette.primary_foreground, 0.04f); };
                more_surface.corner_radius = 4.0f;
                auto more { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_button, section.id + u8"-more" }, more_surface) };
                more->set_content(std::move(more_line));
                more->set_hit_opaque(true);
                more->set_cursor(luil::ui_cursor::hand);
                more->set_access_name(u8"상세 화면에서 " + section.name + u8" 모두 보기");
                more->set_action(luil::ui_trigger::left_click, luil::make_message_action(open_detail_intent { .keep_open = true }));
                strip_tooltips(*more);
                add(std::move(more), more_height);
            }
        }
        if (content_height <= 0.0f)
            add(label(u8"status-empty", u8"지금 지켜볼 세션이 없습니다", 12.0f, luil::label_color_role::dim, luil::text_align::center), empty_height);

        // 접혀 있으면 화면 끝까지 남은 자리에, 펼쳐 있으면 정해 둔 높이에 맞춰 넘치는 카드는 흘린다.
        const float scale { metrics_.scale > 0.0f ? metrics_.scale : 1.0f };
        float limit { maximum_height };
        if (collapsed_ && metrics_.notification_room > 0.0f)
            limit = std::clamp(metrics_.notification_room / scale - widget_actions_gap, 200.0f, maximum_height);
        const float height { std::min(content_height + 2.0f * inset, limit) };
        auto frame_content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"status-content" }, luil::stack_config { .padding = luil::edge_insets::all(inset) }) };
        luil::scroll_area_element* scroll_metrics { nullptr };
        if (content_height + 2.0f * inset > limit)
        {
            luil::scroll_area_config config { .owner = u8"status-scroll", .content_height = content_height, .scroll_offset = status_scroll_ };
            config.scroll = [](const float delta) { return luil::make_app_action(scroll_intent { .value = delta, .status = true }); };
            config.scroll_to = [](const float value) { return luil::make_app_action(scroll_intent { .value = value, .absolute = true, .status = true }); };
            auto scroll { std::make_unique<luil::scroll_area_element>(std::move(config)) };
            scroll->set_content(std::move(list));
            scroll_metrics = scroll.get();
            frame_content->add_flexible(std::move(scroll));
        }
        else
        {
            status_scroll_ = 0.0f;
            frame_content->add_flexible(std::move(list));
        }

        auto background { std::make_unique<popup_backing>(u8"status-background", std::move(frame_content)) };
        background->set_hover_region(luil::hover_region_target {
            .on_enter = [] { return luil::make_app_action(panel_hover_intent { .panel = widget_panel::status, .popup = true, .inside = true }); },
            .on_leave = [] { return luil::make_app_action(panel_hover_intent { .panel = widget_panel::status, .popup = true, .inside = false }); },
        });
        auto root { std::make_unique<luil::root_element>() };
        root->arrange({ { 0.0f, 0.0f, popup_width * scale, height * scale }, scale });
        background->arrange({ { 0.0f, 0.0f, popup_width * scale, height * scale }, scale });
        if (scroll_metrics != nullptr)
            status_scroll_ = scroll_metrics->metrics().scroll_offset;
        if (preview_card != nullptr)
            preview_top_ = preview_card->bounds().y / scale;
        root->add(std::move(background));
        // 표식 왼쪽 끝에 맞춰 바로 아래(머릿줄이 바닥이면 바로 위)에 띄운다. 위젯 오른쪽을 넘으면 안으로 당긴다.
        const float window_height { metrics_.height / scale };
        constexpr float caption_height { static_cast<float>(widget_caption_height) };
        status_popup_x_ = std::max(0.0f, std::min(*status_left_, metrics_.width / scale - popup_width));
        status_popup_width_ = popup_width;
        luil::ui_popup popup {};
        popup.id = std::u8string { status_surface };
        popup.x = status_popup_x_;
        popup.y = metrics_.header_bottom ? window_height - caption_height - widget_actions_gap - height : caption_height + widget_actions_gap;
        popup.width = popup_width;
        popup.height = height;
        popup.backdrop = luil::window_backdrop::acrylic;
        popup.dismiss = close_panel(widget_panel::status);
        popup.tree = std::make_shared<const luil::ui_tree>(std::move(root));
        frame.popups.push_back(std::move(popup));
    }

    std::vector<std::unique_ptr<luil::button_element>> board_driver::widget_actions() const
    {
        std::vector<std::unique_ptr<luil::button_element>> result {};
        result.push_back(widget_button(
            u8"widget-detail", detail_open_ ? u8"상세 화면 닫기" : u8"상세 화면 열기", luil::codicons::icon_editor_layout, luil::make_message_action(open_detail_intent {}), detail_open_));
        const auto panel_button = [&](const std::u8string& id, const std::u8string& name, const char32_t glyph, const widget_panel panel) {
            const bool open { panel_ == panel };
            auto button { widget_button(id, name, glyph, luil::make_message_action(widget_panel_intent { panel, !open }), open) };
            press_toggles(*button);
            result.push_back(std::move(button));
        };
        panel_button(u8"widget-theme", u8"테마 · 키 컬러", luil::codicons::icon_color_mode, widget_panel::theme);
        panel_button(u8"widget-backdrop", u8"배경 효과 · 진하기", luil::codicons::icon_symbol_color, widget_panel::backdrop);
        result.push_back(widget_button(u8"widget-pin", pinned_ ? u8"항상 위에 표시 해제" : u8"항상 위에 표시", luil::codicons::icon_pin, command_action(toggle_pin_command), pinned_));
        return result;
    }

    void board_driver::make_settings_popup(luil::ui_frame& frame)
    {
        if (panel_ != widget_panel::theme && panel_ != widget_panel::backdrop)
            return;
        const float scale { metrics_.scale > 0.0f ? metrics_.scale : 1.0f };
        const float width { metrics_.width / scale };
        if (width <= 0.0f)
            return;
        constexpr float inset { 10.0f };
        const luil::stack_config settings_config { .spacing = 4.0f, .padding = luil::edge_insets::all(inset) };
        auto settings { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-settings" }, settings_config) };
        float height { 2.0f * inset };
        if (panel_ == widget_panel::theme)
        {
            // 밝기와 키 컬러다. 키 컬러는 앱 전체 외양이라 상세 창도 함께 바뀐다.
            // 카드·견본 줄 수는 폭으로 정해지므로 실제로 받을 폭(판 여백과 팝업 테두리를 뺀 값)으로 높이를 잰다.
            const float picker_width { width - 2.0f * inset - 2.0f };
            settings->add(label(u8"widget-theme-name", u8"테마", 11.0f, luil::label_color_role::dim), 16.0f);
            luil::theme_picker_config themes {
                .owner = u8"theme-mode",
                .selected = theme_,
                .name = u8"테마",
                .accent_id = accent_id_,
                .card_width = widget_theme_card_width,
                .preview_height = widget_theme_preview_height,
                .gap = widget_theme_card_gap,
            };
            themes.select = [](const luil::theme_preference value) { return luil::make_app_action(theme_mode_intent { value }); };
            const float themes_height { luil::theme_picker_element::height_for(themes, picker_width) };
            settings->add(std::make_unique<luil::theme_picker_element>(std::move(themes)), themes_height);
            settings->add_gap(4.0f);
            settings->add(label(u8"widget-accent-name", u8"키 컬러", 11.0f, luil::label_color_role::dim), 16.0f);
            luil::accent_picker_config accents { .owner = u8"widget-accent", .selected = accent_id_, .name = u8"키 컬러" };
            accents.select = [](const std::u8string& id) { return luil::make_app_action(accent_intent { id }); };
            const float picker_height { luil::accent_picker_element::height_for(accents, picker_width) };
            settings->add(std::make_unique<luil::accent_picker_element>(std::move(accents)), picker_height);
            height += 16.0f + 4.0f + themes_height + 4.0f + 4.0f + 4.0f + 16.0f + 4.0f + picker_height;
        }
        else
        {
            // 흐림과 진하기다.
            const luil::stack_config row_config { .direction = luil::stack_direction::row, .spacing = 8.0f, .cross_alignment = luil::stack_alignment::center };
            // 투명·흐림을 한 판 위에서 고르는 선택 묶음이다.
            //  - DWM 합성은 창을 가진 UI thread에서 바꿔야 하므로 선택도 앱 UI 명령으로 답한다.
            //  - 100%에서는 비칠 자리가 없어 고를 일이 없다. 값은 남겨 두고 진하기를 내리면 다시 따른다.
            const bool opaque { fully_opaque(opacity_) };
            auto effect_row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-backdrop-row" }, row_config) };
            effect_row->add(label(u8"widget-backdrop-name", u8"배경", 11.0f, luil::label_color_role::dim), 40.0f);
            luil::choice_group_config mode { .owner = u8"widget-backdrop-mode", .style = luil::choice_style::toggle, .name = u8"배경" };
            mode.items.push_back({ u8"backdrop-transparent", u8"투명" });
            mode.items.push_back({ u8"backdrop-blur", u8"흐림" });
            mode.items.push_back({ u8"backdrop-acrylic", u8"Acrylic" });
            mode.selected = backdrop_ == widget_backdrop::transparent ? u8"backdrop-transparent" : backdrop_ == widget_backdrop::blur ? u8"backdrop-blur" : u8"backdrop-acrylic";
            mode.select = [](const std::u8string& value) {
                const std::uint32_t command { value == u8"backdrop-transparent" ? backdrop_transparent_command : value == u8"backdrop-blur" ? backdrop_blur_command : backdrop_acrylic_command };
                return luil::input_action { luil::app_ui_command { command } };
            };
            auto mode_element { std::make_unique<luil::choice_group_element>(std::move(mode)) };
            mode_element->set_enabled(!opaque);
            mode_element->set_tooltip(
                opaque ? u8"진하기가 100%라 뒤가 비치지 않습니다" : u8"투명은 뒤가 그대로 비치고, 흐림은 뒤를 흐리기만 하며, Acrylic은 흐린 뒤에 명암에 맞춘 색 막을 덧씌웁니다");
            effect_row->add_fit(std::move(mode_element));
            effect_row->add_flexible_gap();
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
            // 카드 바탕의 진하기다. 낮을수록 창의 흐림이 카드 뒤로 비치고, 글에는 후광·테를 짙게 둘러 읽히게 한다.
            //  - 창 효과와 달리 그리기만 바뀌므로 위젯 창을 거치지 않고 앱이 바로 받는다.
            //  - 흐림이 없거나(투명) 뒤가 비치지 않으면(100%) 카드는 불투명이라 고를 일이 없다. 값은 남겨 둔다.
            const bool translucent { cards_translucent() };
            luil::slider_config card_level { .minimum = 0.0f, .maximum = 1.0f, .value = card_opacity_ };
            card_level.change = [](const float delta) { return luil::make_app_action(card_opacity_intent { .value = delta, .relative = true }); };
            card_level.change_to = [](const float value) { return luil::make_app_action(card_opacity_intent { .value = value }); };
            card_level.magnets.assign(widget_opacity_magnets.begin(), widget_opacity_magnets.end());
            auto card_slider { std::make_unique<luil::slider_element>(luil::ui_element_id { kind_button, u8"widget-card-opacity" }, std::move(card_level)) };
            card_slider->set_enabled(translucent);
            card_slider->set_tooltip(translucent ? u8"카드 진하기" : u8"카드 진하기 · 흐림이나 Acrylic 배경에서 진하기가 100% 미만일 때 고릅니다");
            auto card_row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-card-opacity-row" }, row_config) };
            card_row->add(label(u8"widget-card-opacity-name", u8"카드", 11.0f, luil::label_color_role::dim), 40.0f);
            card_row->add_flexible(std::move(card_slider));
            card_row->add(label(u8"widget-card-opacity-value", u8(std::to_string(std::lround(card_opacity_ * 100.0f))) + u8"%", 11.0f, luil::label_color_role::dim, luil::text_align::end), 36.0f);
            settings->add(std::move(card_row), widget_setting_row);
            height += 3.0f * widget_setting_row + 2.0f * 4.0f;
        }

        auto background { std::make_unique<popup_backing>(u8"settings-background", std::move(settings)) };
        auto root { std::make_unique<luil::root_element>() };
        root->arrange({ { 0.0f, 0.0f, width * scale, height * scale }, scale });
        background->arrange({ { 0.0f, 0.0f, width * scale, height * scale }, scale });
        root->add(std::move(background));
        // 머릿줄 바깥쪽이 아니라 목록 쪽으로 붙인다. 접혀 있으면 창 밖으로 나가고, 화면 끝에 닿으면 luil이 작업 영역 안으로 들인다.
        const float window_height { metrics_.height / scale };
        constexpr float caption_height { static_cast<float>(widget_caption_height) };
        luil::ui_popup popup {};
        popup.id = u8"widget-settings-panel";
        popup.y = metrics_.header_bottom ? window_height - caption_height - widget_panel_gap - height : caption_height + widget_panel_gap;
        popup.width = width;
        popup.height = height;
        popup.backdrop = luil::window_backdrop::acrylic;
        popup.dismiss = close_panel(panel_);
        popup.tree = std::make_shared<const luil::ui_tree>(std::move(root));
        frame.popups.push_back(std::move(popup));
    }

    void board_driver::make_actions_popup(luil::ui_frame& frame)
    {
        if (panel_ != widget_panel::actions || !actions_right_.has_value())
            return;
        const float scale { metrics_.scale > 0.0f ? metrics_.scale : 1.0f };
        auto actions { widget_actions() };
        const float count { static_cast<float>(actions.size()) };
        const float width { 2.0f * widget_actions_inset + widget_icon_button * count + header_spacing * (count - 1.0f) };
        const float height { 2.0f * widget_actions_inset + widget_icon_button };
        const luil::stack_config row_config { .direction = luil::stack_direction::row, .spacing = header_spacing, .padding = luil::edge_insets::all(widget_actions_inset) };
        auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"widget-actions-row" }, row_config) };
        for (auto& button : actions)
            row->add(std::move(button), { .length = widget_icon_button, .cross_length = widget_icon_button });
        auto background { std::make_unique<popup_backing>(u8"actions-background", std::move(row)) };
        // 표지 단추에서 이 팝업으로 옮겨 오는 동안 닫히지 않게 한다. 떠나면 잠시 뒤 닫는다.
        background->set_hover_region(luil::hover_region_target {
            .on_enter = [] { return luil::make_app_action(panel_hover_intent { .panel = widget_panel::actions, .popup = true, .inside = true }); },
            .on_leave = [] { return luil::make_app_action(panel_hover_intent { .panel = widget_panel::actions, .popup = true, .inside = false }); },
        });
        auto root { std::make_unique<luil::root_element>() };
        root->arrange({ { 0.0f, 0.0f, width * scale, height * scale }, scale });
        background->arrange({ { 0.0f, 0.0f, width * scale, height * scale }, scale });
        root->add(std::move(background));
        // 표지 단추와 오른쪽 끝을 맞춰 바로 아래(머릿줄이 바닥이면 바로 위)에 띄운다. 단추를 덮으면 머무름이 끝나 닫힌다.
        const float window_height { metrics_.height / scale };
        constexpr float caption_height { static_cast<float>(widget_caption_height) };
        luil::ui_popup popup {};
        popup.id = u8"widget-actions";
        popup.x = std::max(0.0f, *actions_right_ - width);
        popup.y = metrics_.header_bottom ? window_height - caption_height - widget_actions_gap - height : caption_height + widget_actions_gap;
        popup.width = width;
        popup.height = height;
        popup.backdrop = luil::window_backdrop::acrylic;
        popup.dismiss = close_panel(widget_panel::actions);
        popup.tree = std::make_shared<const luil::ui_tree>(std::move(root));
        frame.popups.push_back(std::move(popup));
    }

    std::vector<std::pair<std::string, std::u8string>> board_driver::signal_lines(const std::int64_t now) const
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
        std::vector<std::pair<std::string, std::u8string>> result { { "codex", std::move(codex) }, { "claude", std::move(claude) } };
        // Antigravity는 수집을 켰거나 받은 세션이 있을 때만 한 줄을 차지한다. 쓰지 않는 사용자의 바닥 줄을 좁히지 않는다.
        if (antigravity_ != nullptr)
            result.emplace_back("antigravity",
                !antigravity_data_.error.empty() ? u8(antigravity_data_.error)
                    : antigravity_data_.ready    ? u8"Antigravity · 자동 수집 " + u8(std::to_string(antigravity_data_.sessions.size())) + u8"개"
                                                 : std::u8string { u8"Antigravity · 로컬 세션 탐색 중" });
        else if (std::ranges::any_of(data_.sessions, [](const auto& session) { return session.provider == "antigravity"; }))
            result.emplace_back("antigravity", u8"Antigravity · " + received("antigravity"));
        return result;
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

    void save_window_placement(const std::filesystem::path& file, const luil::win32::window_placement& placement, const bool header_bottom, const bool collapsed) noexcept
    {
        try
        {
            nlohmann::json value { { "x", placement.x }, { "y", placement.y }, { "width", placement.width }, { "height", placement.height } };
            if (placement.maximized)
                value["maximized"] = true;
            if (header_bottom)
                value["header_bottom"] = true;
            if (collapsed)
                value["collapsed"] = true;
            write_json_file(file, value);
        }
        catch (...)
        {}
    }

    widget_appearance load_widget_appearance(const std::filesystem::path& file)
    {
        widget_appearance result {};
        try
        {
            std::ifstream input { file, std::ios::binary };
            if (!input)
                return result;
            const auto value { nlohmann::json::parse(input) };
            if (const auto theme { value.find("theme") }; theme != value.end() && theme->is_string())
                static_cast<void>(luil::parse_theme_preference(u8(theme->get<std::string>()), result.theme));
            // 목록에 없는 id로 두면 표시만 기본색으로 물러서고 견본은 아무것도 고르지 않은 채가 된다.
            if (const auto accent { value.find("accent") }; accent != value.end() && accent->is_string())
                if (const auto id { u8(accent->get<std::string>()) }; luil::accent_exists(id))
                    result.accent_id = id;
            // 모르는 이름(예전의 "blur" 포함)은 Acrylic이다.
            if (const auto backdrop { value.find("backdrop") }; backdrop != value.end() && backdrop->is_string())
            {
                const auto name { backdrop->get<std::string>() };
                result.backdrop = name == "transparent" ? widget_backdrop::transparent : name == "blur-plain" ? widget_backdrop::blur : widget_backdrop::acrylic;
            }
            if (const auto opacity { value.find("opacity") }; opacity != value.end() && opacity->is_number())
                if (const auto level { opacity->get<float>() }; std::isfinite(level))
                    result.opacity = std::clamp(level, 0.0f, 1.0f);
            if (const auto card { value.find("card_opacity") }; card != value.end() && card->is_number())
                if (const auto level { card->get<float>() }; std::isfinite(level))
                    result.card_opacity = std::clamp(level, 0.0f, 1.0f);
            return result;
        }
        catch (...)
        {
            return {};
        }
    }

    void save_widget_appearance(const std::filesystem::path& file, const widget_appearance& appearance) noexcept
    {
        try
        {
            const std::u8string_view theme { luil::theme_preference_name(appearance.theme) };
            const nlohmann::json value {
                { "theme", std::string { theme.begin(), theme.end() } },
                { "accent", std::string { appearance.accent_id.begin(), appearance.accent_id.end() } },
                { "backdrop", backdrop_name(appearance.backdrop) },
                { "opacity", appearance.opacity },
                { "card_opacity", appearance.card_opacity },
            };
            write_json_file(file, value);
        }
        catch (...)
        {}
    }

    bool load_widget_header_bottom(const std::filesystem::path& file) noexcept
    {
        try
        {
            std::ifstream input { file, std::ios::binary };
            return input && nlohmann::json::parse(input).value("header_bottom", false);
        }
        catch (...)
        {
            return false;
        }
    }

    bool load_widget_collapsed(const std::filesystem::path& file) noexcept
    {
        try
        {
            std::ifstream input { file, std::ios::binary };
            return input && nlohmann::json::parse(input).value("collapsed", false);
        }
        catch (...)
        {
            return false;
        }
    }

    board_delegate::board_delegate(
        std::filesystem::path placement_file, const bool restored, single_instance* instance, widget_appearance appearance, const bool header_bottom, const bool collapsed)
        : placement_file_ { std::move(placement_file) }
        , restored_ { restored }
        , header_bottom_ { header_bottom }
        , collapsed_ { collapsed }
        , appearance_ { std::move(appearance) }
        , instance_ { instance }
    {}

    void board_delegate::on_started(luil::app_host& host)
    {
        // 창을 붙이기 전에 값만 둔다. 붙일 때 이 값으로 합성을 처음 건다.
        window_.set_backdrop(appearance_.backdrop);
        window_.set_opacity(appearance_.opacity);
        if (!window_.attach(
                restored_, header_bottom_,
                [this, &host] {
                    const RECT box { window_.bounds() };
                    const float scale { static_cast<float>(GetDpiForWindow(window_.handle())) / 96.0f };
                    host.post_app_message(make_window_metrics_message(static_cast<float>(box.right - box.left), static_cast<float>(box.bottom - box.top), scale));
                },
                [&host] {
                    const auto frame { host.acquire_frame() };
                    const auto theme { frame != nullptr ? frame->appearance.theme : luil::theme_preference::system };
                    return theme == luil::theme_preference::system ? system_dark() : theme == luil::theme_preference::dark;
                }))
        {
            host.post_app_message(luil::app_message { widget_failed_intent {} });
            return;
        }
        // 지난번에 목록을 접어 둔 채 닫았으면 접은 채로 연다. 저장한 크기는 펼친 크기라 펼치면 그 크기로 돌아온다.
        //  - 시작할 때 luil이 알린 자리는 접기 전의 것이라 접힘이 빠진 채 저장됐다. 접은 뒤 다시 쓴다.
        if (collapsed_)
        {
            window_.toggle_collapse();
            if (!placement_file_.empty() && placement_.has_value())
                save_window_placement(placement_file_, *placement_, window_.header_bottom(), window_.collapsed());
        }
        if (instance_ != nullptr)
            instance_->bind_window(window_.handle());
        // 위젯은 작업 표시줄에 서지 않으므로 숨기거나 끝낼 자리를 알림 영역에 둔다.
        tray_.attach(
            window_.handle(), [&host] { host.post_app_message(luil::app_message { open_detail_intent { .keep_open = true } }); },
            [&host] { host.post_app_message(luil::app_message { licenses_intent { .keep_open = true } }); });
    }

    luil::app_message board_delegate::make_window_placement_message(const luil::win32::window_placement& placement)
    {
        if (placement_file_.empty() || placement.maximized || placement.fullscreen)
            return {};
        auto saved { placement };
        if (window_.collapsed())
        {
            saved.height = window_.expanded_height();
            if (window_.header_bottom())
                saved.y -= saved.height - placement.height;
        }
        placement_ = saved;
        save_window_placement(placement_file_, saved, window_.header_bottom(), window_.collapsed());
        return {};
    }

    void board_delegate::execute_app_ui_command(luil::app_host& host, const luil::app_ui_command& command)
    {
        // 접힘 상태는 창 크기를 바꾸며 luil이 알리는 metrics에 실려 간다.
        //  - luil은 접기로 바뀐 크기를 끝날 때에야 알린다. 강제로 끝나도 남도록 접힘은 바꾼 자리에서 바로 저장한다.
        if (command.command == toggle_collapse_command)
        {
            window_.toggle_collapse();
            if (!placement_file_.empty() && placement_.has_value())
                save_window_placement(placement_file_, *placement_, window_.header_bottom(), window_.collapsed());
        }
        else if (command.command == toggle_pin_command)
        {
            window_.toggle_pin();
            host.post_app_message(luil::app_message { widget_state_intent { window_.pinned() } });
        }
        else if (command.command == backdrop_acrylic_command || command.command == backdrop_transparent_command || command.command == backdrop_blur_command)
        {
            // DWM 합성은 창을 가진 UI thread에서 바꾸고, 그릴 바탕의 진하기는 앱 상태로 알린다.
            const auto backdrop { backdrop_of_command(command.command) };
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
        // 레이어드 표시에서 luil은 미룬 배치의 크기로 부른다. 머릿줄 방향·알림 자리도 그 배치의 것이다.
        metrics_intent metrics {};
        metrics.width = width;
        metrics.height = height;
        metrics.scale = scale;
        metrics.header_bottom = window_.header_bottom();
        metrics.notification_room = static_cast<float>(window_.notification_room());
        metrics.room_left = static_cast<float>(window_.side_room(true));
        metrics.room_right = static_cast<float>(window_.side_room(false));
        metrics.collapsed = window_.collapsed();
        return luil::app_message { metrics };
    }

    std::vector<luil::input_action> board_policy::on_wheel(const luil::ui_tree& tree, const luil::mouse_wheel_event& event, const float delta)
    {
        return luil::route_wheel(tree, event, delta);
    }
} // namespace session_board
