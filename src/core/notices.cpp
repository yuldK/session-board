#include "core/notices.h"

#include <algorithm>
#include <iterator>

namespace session_board {
    namespace {
        // 고지 생성기와 Skia 패키지의 NOTICE.md가 절 머리 위아래에 긋는 줄이다.
        constexpr std::string_view notice_rule { "----------------------------------------" };
        constexpr std::string_view component_prefix { "\nComponent: " };

        std::string_view trim_end(std::string_view text)
        {
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t'))
                text.remove_suffix(1);
            return text;
        }

        // UTF-8 글자 하나의 byte 수와 칸 수다. 세 byte 이상인 글자(한글·한자·기호)는 넓게 센다.
        struct glyph_extent
        {
            std::size_t bytes { 1 };
            std::size_t columns { 1 };
        };

        glyph_extent extent_at(const std::string_view text, const std::size_t index)
        {
            const auto lead { static_cast<unsigned char>(text[index]) };
            const std::size_t bytes { lead >= 0xf0u ? 4u : lead >= 0xe0u ? 3u : lead >= 0xc0u ? 2u : 1u };
            return { std::min(bytes, text.size() - index), lead >= 0xe0u ? 2u : 1u };
        }

        std::size_t columns_of(const std::string_view text)
        {
            std::size_t columns { 0 };
            for (std::size_t index { 0 }; index < text.size();)
            {
                const auto extent { extent_at(text, index) };
                columns += extent.columns;
                index += extent.bytes;
            }
            return columns;
        }
    } // namespace

    std::vector<notice_component> parse_notices(const std::string_view source)
    {
        // Windows의 CMake가 고지를 CRLF로 쓰고 원문마다 줄 끝도 섞여 있으므로 `\n` 하나로 맞춘 뒤 나눈다.
        std::string normalized {};
        normalized.reserve(source.size());
        std::ranges::copy_if(source, std::back_inserter(normalized), [](const char character) { return character != '\r'; });
        const std::string_view notices { normalized };
        const std::string marker { std::string { notice_rule } + std::string { component_prefix } };
        std::vector<notice_component> components {};
        auto position { notices.find(marker) };
        while (position != std::string_view::npos)
        {
            const auto name_start { position + marker.size() };
            const auto name_end { notices.find('\n', name_start) };
            if (name_end == std::string_view::npos)
                break;
            // 이름 아래의 줄 긋기를 건너뛰고 다음 절의 줄 긋기 앞까지가 원문이다.
            auto body { name_end + 1 };
            if (notices.substr(body).starts_with(notice_rule))
            {
                const auto rule_end { notices.find('\n', body) };
                body = rule_end == std::string_view::npos ? notices.size() : rule_end + 1;
            }
            const auto next { notices.find(marker, body) };
            const auto end { next == std::string_view::npos ? notices.size() : next };
            components.push_back({ std::string { trim_end(notices.substr(name_start, name_end - name_start)) }, std::string { trim_end(notices.substr(body, end - body)) } });
            position = next;
        }
        return components;
    }

    std::vector<std::string> wrap_notice_lines(const std::string_view text, std::size_t columns)
    {
        // 칸이 너무 좁으면 낱말마다 줄이 갈려 읽을 수 없다. 좁은 창에서도 한 줄은 이만큼 둔다.
        columns = std::max<std::size_t>(columns, 16);
        std::vector<std::string> lines {};
        std::size_t start { 0 };
        while (start <= text.size())
        {
            auto end { text.find('\n', start) };
            if (end == std::string_view::npos)
                end = text.size();
            std::string line {};
            for (const char character : text.substr(start, end - start))
                if (character == '\t')
                    line += "    ";
                else if (character != '\r')
                    line += character;
            while (columns_of(line) > columns)
            {
                std::size_t used { 0 };
                std::size_t cut { 0 };
                std::size_t space { std::string::npos };
                for (std::size_t index { 0 }; index < line.size();)
                {
                    const auto extent { extent_at(line, index) };
                    if (used + extent.columns > columns)
                        break;
                    if (line[index] == ' ')
                        space = index;
                    used += extent.columns;
                    index += extent.bytes;
                    cut = index;
                }
                // 칸에 꼭 찬 낱말 바로 뒤가 공백이면 거기서 자른다.
                if (cut < line.size() && line[cut] == ' ')
                    space = cut;
                if (space != std::string::npos && space > 0)
                {
                    lines.push_back(line.substr(0, space));
                    line.erase(0, space + 1);
                }
                else
                {
                    lines.push_back(line.substr(0, cut));
                    line.erase(0, cut);
                }
            }
            lines.push_back(std::move(line));
            start = end + 1;
        }
        while (!lines.empty() && trim_end(lines.back()).empty())
            lines.pop_back();
        return lines;
    }
} // namespace session_board
