#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace session_board {
    // Claude 데스크톱 앱(Code 탭)이 연 세션을 찾는다.
    //  - 데스크톱은 세션을 자기 ID(`local_…`)로 부르고, Hook·로컬 기록이 아는 것은 Claude Code의 세션 ID다.
    //    둘을 잇는 것은 데스크톱 세션 기록(`claude-code-sessions/<계정>/<조직>/local_*.json`)의 `cliSessionId`뿐이다.
    //  - 기록 형식은 공개 규격이 아니다. 읽지 못하면 그 세션을 모르는 것으로 두고 창 이동으로 물러선다.
    class claude_desktop_index final
    {
    public:
        explicit claude_desktop_index(std::vector<std::filesystem::path> roots);
        // 바뀐 기록만 다시 읽는다. 사라진 기록은 지운다.
        void refresh();
        // 보관하지 않은 데스크톱 세션이면 그 `local_…` ID다. 아니면 비어 있다.
        [[nodiscard]] std::string find(std::string_view cli_session_id) const;

    private:
        struct entry
        {
            std::filesystem::file_time_type time {};
            std::string session_id {};
            std::string cli_session_id {};
            bool archived { false };
        };
        std::vector<std::filesystem::path> roots_ {};
        std::map<std::filesystem::path, entry> entries_ {};
    };

    // 데스크톱 세션 기록이 놓이는 `claude-code-sessions` 폴더들이다. 있는 것만 돌려준다.
    //  - 스토어(MSIX) 설치는 데이터를 `%LOCALAPPDATA%/Claude-Data/<패키지>/`나 패키지 가상 폴더에 두고,
    //    일반 설치는 `%APPDATA%/Claude/`에 둔다.
    [[nodiscard]] std::vector<std::filesystem::path> default_claude_desktop_roots();
    // 데스크톱 앱에서 그 세션을 여는 `claude://` 주소다. ID가 데스크톱 형식이 아니면 비어 있다.
    //  - 앱은 `/continue?session=`에 `local_` ID만 받고, 보관하지 않은 세션이면 그 세션 화면을 연다.
    [[nodiscard]] std::wstring claude_desktop_link(std::string_view desktop_session_id);
} // namespace session_board
