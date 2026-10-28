# Antigravity 연동 설계 (초안)

상태: 1·2·5단계 구현됨(2026-10-28). 확정 규칙은 `event-protocol.md`와 `antigravity-integration.md`가 기준이며, 이 문서는 설계 배경과 남은 단계(3·4)를 담는다.

Google Antigravity(`provider: "antigravity"`)를 Codex·Claude와 같은 수준으로 보인다.
Session Board를 켜면 이미 열린 대화와 최근 기록이 바로 보이고, Hook은 새 대화부터 보완한다.
SQLite는 쓰지 않는다. `conversation_summaries.db`, `conversations/*.db`는 열지 않고 파일 읽기, Win32 잠금 확인, `nlohmann::json`만 쓴다.

## 대상

| 제품 | 앱 데이터 폴더 | 이 PC에서 본 것 |
| --- | --- | --- |
| Antigravity 2.0 데스크톱 | `<홈>/antigravity` | `presence/`, `annotations/`, `brain/`이 모두 있다 |
| Antigravity IDE | `<홈>/antigravity-ide` | `brain/`(비어 있음), `conversations/`, `knowledge/`뿐이다. `presence/`, `annotations/`는 없다 |
| Antigravity CLI | `<홈>/antigravity-cli` | 폴더가 없다 |

- `<홈>`은 `%USERPROFILE%/.gemini`이고, `--antigravity-home <경로>`로 바꿀 수 있다.
- 세 폴더를 모두 탐색하되, 있는 폴더만 읽는다.
- `presence/`가 없는 제품은 열림 여부를 알 수 없다. 그 제품의 기록은 최근 기록(15분 유지)으로만 보인다. "열린 세션"이라고 단정하지 않는다.

제외:

- SQLite DB와 `*.pb` 파일(`agyhub_summaries_proto.pb`): 이진 protobuf·SQLite라 읽지 않는다.
- `brain/<id>/.system_generated/steps/`: 단계별 원본이다. transcript가 잘린 경우(`truncated_fields`)에도 요약 한 줄이면 충분하므로 읽지 않는다.
- `tasks/task-N.log`: 백그라운드 명령의 출력이다. 도구 출력은 저장하지 않는 원칙대로 읽지 않는다.

## 확인한 신호 (2026-10-28, 이 PC)

### 열린 대화: `presence/<대화 ID>.lock`

- 크기 0인 파일이다. 열린 대화의 잠금 파일은 공유 모드 0, 읽기·쓰기로 열면 `ERROR_SHARING_VIOLATION`(0x80070020)이 난다. 닫힌 대화의 잠금 파일은 그대로 열린다. 두 파일로 직접 확인했다.
- 실행 중인 `Antigravity.exe` 6개와 `language_server.exe` 1개를 확인했다.
- 판별은 `codex_writer_active()`를 그대로 쓴다. 파일이 있다는 것만으로 열렸다고 보지 않는다.
- 확인하려고 여는 순간에는 우리가 그 파일을 독점한다. 그러니 열자마자 닫는다. Codex 잠금 확인과 같은 위험·같은 처리다.

### 대화 이름: `annotations/<대화 ID>.pbtxt`

```
title:"…"  last_user_view_time:{seconds:1791293767  nanos:56000000}
```

- 한 줄짜리 protobuf 텍스트 형식이다. **필드 사이가 공백 두 칸**이므로 공백의 개수와 관계없이 읽는다.
- `title:"…"`의 protobuf 이스케이프(`\"`, `\\`, `\n`, `\NNN` 8진수 UTF-8 바이트)를 풀어 `name`으로 쓴다. `title`이 없는 대화도 있다.
- `last_user_view_time`은 사용자가 그 대화를 마지막으로 본 시각이다. 활동 시각이 아니므로 `updated_ms`에 쓰지 않는다.

### 대화 기록: `brain/<대화 ID>/.system_generated/logs/transcript.jsonl`

이 PC의 열린 대화 기록 94줄을 집계했다.

| `source` / `type` / `status` | 개수 | 비고 |
| --- | --- | --- |
| `MODEL` / `GENERIC` / `DONE` | 63 | 도구 실행 결과 |
| `MODEL` / `PLANNER_RESPONSE` / `DONE`, `tool_calls` 있음 | 21 | 도구 호출 |
| `MODEL` / `PLANNER_RESPONSE` / `DONE`, `tool_calls` 없음 | 4 | 최종 답변. 사용자 입력 4개와 짝이 맞는다 |
| `USER_EXPLICIT` / `USER_INPUT` / `DONE` | 4 | 새 turn |
| `MODEL` / `GENERIC` / `RUNNING` | 1 | 백그라운드 명령 시작 |
| `SYSTEM` / `SYSTEM_MESSAGE` / `DONE` | 1 | 백그라운드 명령 완료 알림 |

- 모든 줄에 있는 키: `step_index`, `source`, `type`, `status`, `created_at`(UTC, `Z`로 끝남)
- 줄에 따라 있는 키: `content`, `tool_calls`, `truncated_fields`, `input_tokens`, `cache_read_tokens`, `output_tokens`
- 이번 기록에는 `thinking` 키가 없었다. 다른 기록에 나오더라도 읽지 않는다.
- `tool_calls[].args`는 객체이고, **값이 모두 문자열**이다. 예: `StartLine:"12"`, `WaitMsBeforeAsync:"…"`.
- 실제로 본 도구 이름: `read_file`, `run_command`, `search_web`, `write_file`
  - 브리프의 `edit_file`, `write_to_file`, `view_file`은 이번 기록에 없었다.
  - 활동 문구는 표로 대응하고, 표에 없는 이름은 `도구 실행 중 · <이름>`으로 보인다.
- `run_command`의 `args.toolSummary`는 모델이 쓴 짧은 명령 설명이다. 백그라운드 하위 행의 제목 후보다. `CommandLine`은 명령 인자라 표시하지 않는다.
- `USER_INPUT.content`에 들어 있는 태그: `<USER_REQUEST>`, `<ADDITIONAL_METADATA>`(현지 시각 안내뿐이고 경로는 없다), `<USER_SETTINGS_CHANGE>`
  - 모델 변경 문구는 ``The user changed setting `Model Selection` from None to <모델>.``이다.
- 최종 답변의 `content`는 마크다운(`**…**`, `###`)으로 시작한다. 요약을 만들 때 기호를 걷어 낸다.

### 백그라운드 명령

- 시작을 알리는 줄: `GENERIC` / `RUNNING`의 `content`에 다음 문구가 있다.
  - `Tool is running as a background task with task id: <대화 ID>/task-5`
  - `Task Description: …`
- **작업 ID는 `<대화 ID>/task-N` 꼴이다.** `task-N`만 쓰지 않는다. `child_id`에는 전체를 쓴다.
- 끝: `SYSTEM_MESSAGE`의 `content`에 `[Message] timestamp=… sender=<작업 ID> … content=Task id "<작업 ID>" finished with result: …`가 있다.
  - 이 줄은 `truncated_fields: ["content"]`로 잘려 있을 수 있다. 하지만 `sender=`와 `finished`는 앞부분에 있다.
- `content` 안의 `Created At:`은 현지 시각(`+09:00`)이고, 줄의 `created_at`은 UTC다. 시각은 늘 `created_at`에서 읽는다.
- `brain/<대화 ID>/.system_generated/messages/*.json`에는 두 가지 모양이 있다.
  - `sender`, `renderDetails`, `sourceMetadata` 등을 가진 평평한 객체
  - UUID를 키로 한 묶음
  - transcript의 `SYSTEM_MESSAGE`만으로 끝을 알 수 있으므로 1단계에서는 읽지 않는다.

### 작업 폴더: `config/projects/*.json`

- `folderUri`는 최상위가 아니라 `projectResources.resources[].folderUri`에 있다. 예: `file:///e%3A/...`
- `resources[]`에는 `gitFolder` 객체를 가진 항목도 있다. 이 모양은 따로 확인해야 한다.
- `outside-of-project.json`에는 `projectResources`가 없다.
- 로컬 기록만 있는 세션은 도구 인자의 `Path`·`AbsolutePath`·`TargetFile`·`Cwd`를 이 프로젝트 루트들과 대조한다. 가장 길게 일치하는 루트를 `cwd`로 쓰고, 없으면 비워 둔다. 폴더 이름을 추측하지 않는다.

### Hook (확인 못 함)

- `config/hooks.json`이 이 PC에 **없다**. 브리프의 다음 내용은 모두 실제로 확인하지 못했다.
  - 이름별 묶음 형식
  - 이벤트별 flat/grouped 차이
  - `Stop`의 `{"decision":"allow"}` 응답
  - `PreToolUse`의 `decision` 요구
- `Stop`에 잘못 답하면 대화가 멈추지 않거나 오류가 날 수 있다. 이는 "Hook이 AI 작업을 막지 않는다"는 원칙을 어긴다.
- 그래서 **자동 등록은 실제 Antigravity에서 다음을 확인한 다음에 켠다**.
  - 수동 등록한 Hook 하나가 불리는지
  - `{}`를 돌려줘도 `Stop`이 정상으로 끝나는지

## 데이터 모델

`session_snapshot`은 바꾸지 않는다. `provider`에 `"antigravity"`를 허용한다.

| 스냅숏 필드 | 로컬 기록에서 | Hook에서 |
| --- | --- | --- |
| `session_id` | 대화 ID(폴더 이름) | `conversationId` |
| `name` | `annotations`의 `title` | — |
| `title` | `<USER_REQUEST>`의 앞부분, 최대 180 bytes | — |
| `summary` | 공개 `content`의 첫 문장, 최대 240 bytes | — |
| `activity` | 도구 이름별 문구 | `tool_name` |
| `cwd` | 프로젝트 루트 대조 결과 또는 빈 값 | `workspacePaths[0]` |
| `model` | `USER_SETTINGS_CHANGE`의 모델 | `modelName`(`auto`·빈 값 제외) |
| `turn_id` | `step-<USER_INPUT의 step_index>` | 없으면 생략 |
| `runtime_loaded` | `presence` 잠금 | — |
| `children` | 백그라운드 명령, `invoke_subagent` | — |

- `tool_id`는 `<step_index>:<tool_calls 순번>`이다.
  - Hook의 `stepIdx`와 이름 공간이 달라서 같은 도구가 두 번 잡힐 수 있다.
  - 병합할 때는 로컬 기록이 도구 목록을 맡고, Hook은 `cwd`·창 정보·턴 경계만 맡는다(아래 병합 규칙).

## 이벤트 규격 변경

- `provider`에 `antigravity`를 추가한다. 필드는 그대로다.
- `session-board.exe --hook --provider antigravity`
  - payload에 이벤트 이름이 없을 수 있다. 그래서 `--event <이름>` 인자를 등록 명령에 함께 넣는다. 고유 필드로 추정하는 것은 인자가 없을 때만 쓰는 보조 수단이다.
  - 대응 규칙:

    | Antigravity Hook | 공통 이벤트 |
    | --- | --- |
    | `SessionStart` | `session_started` |
    | `PreInvocation` | `turn_started` |
    | `PreToolUse` | `ask_question`이면 `input_requested`, 아니면 `tool_started` |
    | `PostToolUse` | `tool_finished` |
    | `Stop` + `USER_CANCELED` | `interrupted` |
    | `Stop` + `ERROR` | `failed` |
    | `Stop` + 그 밖 | `turn_finished` |
  - `PreToolUse`는 파서에서만 처리한다. 자동 등록에는 넣지 않는다. 권한 결정(`allow`/`ask`)에 끼어들지 않기 위해서다.
- 응답: 기본은 `{}`, 종료 코드 0이다.
  - `Stop`만 Antigravity가 비차단으로 받는 값을 쓴다. 앞의 Hook 확인에서 정한다.
  - 응답은 provider와 이벤트의 표 하나로 정한다. `hook_command.cpp`에 분기를 늘리지 않는다.

## 상태 규칙

| 로컬 기록 | 상태 | `activity` |
| --- | --- | --- |
| `USER_INPUT` | 작업 중, 도구 목록·`turn_closed` 초기화 | `작업 내용 수신 대기` (Claude·Codex와 같은 말) |
| `PLANNER_RESPONSE` + `tool_calls` | 작업 중, 도구 추가 | 도구별 문구 |
| 위 줄의 도구 중 `ask_question` | 입력 대기 | `사용자 답변 대기` |
| `GENERIC` `DONE`/`ERROR` | 도구 하나 끝. 남은 도구가 없으면 | `분석 또는 응답 작성 중` |
| `GENERIC` `RUNNING` + 백그라운드 문구 | 포그라운드 도구 끝, 하위 행 열기 | — |
| `PLANNER_RESPONSE` 도구 없음 `DONE` | 응답 완료(`idle`), `turn_closed` | `응답 완료` |
| `SYSTEM_MESSAGE` + `sender=`·`finished` | 하위 행 닫기(완료). 부모 상태는 그대로 | — |
| `CHECKPOINT` | 무시. `title`이 비었을 때만 `# USER Objective:` 다음 줄 | — |

- `GENERIC` `ERROR`는 도구 하나의 실패다. 세션 전체를 `failed`로 만들지 않는다.
- `SYSTEM_MESSAGE`는 사용자 turn이 아니다. `turn_id`를 바꾸지 않는다.
- 부모 turn이 끝날 때 하위를 닫는 규칙은 `child-sessions-design.md`와 같다. 백그라운드 명령은 부모 turn이 끝나도 남긴다.
- 대화가 닫히면(잠금 해제) 아직 도는 하위를 `확인 불가`로 닫는다.

### 정해야 하는 공백: 승인 대기

- 자동 등록에서 `PreToolUse`를 빼면, 기록만으로는 "도구 실행 중"과 "사용자 승인 대기"를 구별할 신호가 없다.
- 두 경우 모두 `PLANNER_RESPONSE`에 도구가 있고, 그 뒤에 `GENERIC`이 아직 없다.
- (다)로 정했다: `PreToolUse`를 등록하되 결정 없이 답한다. 4단계에서 붙이며, 그 전까지 승인 대기는 `작업 중`으로 보인다.

## 병합

- `(provider, session_id)`가 같으면 하나의 카드다.
- 로컬 기록을 상태·도구·요약의 기준으로 삼는다. Hook 스냅숏에서는 `cwd`, `host_window`, `host_process_id`, `model`만 가져온다.
  - Claude는 Hook과 기록이 서로 보완하는 구조다. Antigravity는 Hook 이벤트가 더 적고 도구 ID 이름 공간도 달라서, 기록 쪽을 기준으로 둔다.
- 병합은 provider마다 따로 둔다(`merge_antigravity_sessions`). Codex·Claude는 Hook이 더 새로우면 Hook 상태를 쓰지만 Antigravity는 늘 기록 쪽이 기준이라, 공통 함수에 정책을 넘기면 오히려 갈래가 늘어난다.

## 창 이동

- Hook의 프로세스 체인은 `cmd.exe`/`powershell.exe` → `language_server.exe` → `Antigravity.exe`라고 한다(브리프 기준, 확인 못 함). 이대로라면 `find_host_window()`가 그대로 찾는다.
- 로컬 기록만 있는 세션은 창 정보가 없다. 이동 단추를 꺼 둔다.
- Antigravity 데스크톱에 대화를 여는 주소(`antigravity://…`)가 있는지는 확인하지 못했다. 찾으면 `codex://threads/`처럼 붙인다.

## 화면

- 필터는 표식과 세션 수만 보이는 칩이다(`전체 N`만 글). provider 이름은 툴팁과 보조 기술 이름(`Codex 3`)이 말한다. UIA 값(`filter-<provider>`)은 그대로다.
- provider 표식은 Simple Icons(CC0)의 Google Gemini 표식과 그 색(#8E75B2)이다.
- 수집 출처 문구는 `Antigravity 로컬 기록` / `Antigravity Hook`이다.
- 하단 상태 줄: 지금 `signal_text`는 Codex·Claude 두 줄을 한 문자열로 이어 붙였다가 구분자로 다시 나눈다. provider 목록을 돌며 줄을 만드는 구조로 바꾼다.

## 바뀌는 곳

### provider 표로 일반화 (먼저 한다)

지금 코드는 provider가 둘이라고 가정한 곳이 많다. 셋째 분기를 곳곳에 넣지 않고 `core/providers.h`의 표 하나로 모은다.
표에는 이름, 표시 이름, 로컬 기록 출처와 그 설명만 두었다. 표식 경로는 화면 코드(Skia)에, 기본 홈·데스크톱 주소·Hook 응답은 각 수집기에 남겼다. 지금 쓰는 곳이 하나씩이라 표로 올리면 core가 화면과 Win32 주소를 알게 된다.

| 위치 | 지금 |
| --- | --- |
| `src/core/hook_command.cpp:31` | `--provider`는 `codex`·`claude`만 허용 |
| `src/core/hook_command.cpp:53` | provider별 변환 함수를 삼항 연산으로 고름 |
| `src/core/session.cpp:70` | `valid_provider`가 둘만 허용 |
| `src/core/claude_hooks.cpp:303` | 표시 이름을 삼항 연산으로 고름 |
| `src/app/board_app.cpp:169` | `provider_mark`가 `codex_` 불리언 하나로 표식을 고름 |
| `src/app/board_app.cpp:608` | `provider_text` 삼항 연산 |
| `src/app/board_app.cpp:1590` | `provider_counts[3]`, 인덱스 1·2를 Codex·Claude로 고정 |
| `src/app/board_app.cpp:1734`, `:2064` | 상태 줄을 Codex·Claude 두 줄로 고정 |
| `src/app/board_app.cpp:2196` | `signal_text` 두 provider 이어 붙이기 |
| `src/app/board_app.cpp:48` | 필터 폭 `264.0f` |

### 공통화

- `tracked_file`(추가분 읽기)과 `record_time`(ISO 8601 → ms)이 `codex_monitor.cpp`·`claude_monitor.cpp`에 따로 있다. `core/local_record.h/.cpp`로 옮겨 셋이 함께 쓴다.
- 잠금 확인은 `file_lock_held()`로 옮겼다. `codex_writer_active()`는 기존 호출을 위해 그것을 부르는 이름으로 남겼다.
- `ensure_hook_installation()`은 이미 provider 이름과 갱신 함수를 받는다. Antigravity는 `update_antigravity_hook_settings`만 새로 쓴다.

### 새 파일

| 파일 | 내용 |
| --- | --- |
| `src/core/antigravity_monitor.h/.cpp` | `antigravity_transcript`(줄 해석), `parse_antigravity_annotation`, 프로젝트 루트 대조, 수집 스레드 |
| `src/core/antigravity_hooks.h/.cpp` | `antigravity_event`, `update_antigravity_hook_settings`, `ensure_antigravity_hooks` |
| `scripts/install-antigravity-hooks.ps1` | `-Preview`, `-Remove`, `-SettingsFile`, `-InstallDirectory`, `-Executable`, `-Timeout` |
| `tests/antigravity_tests.cpp` | 아래 단계별 검사 |
| `docs/antigravity-integration.md` | 확정 규칙 |

### 바꾸는 파일

- `src/app/main.cpp`: `--antigravity-home`, `--no-antigravity-discovery`를 더했다. `--no-antigravity-hooks`는 4단계에서 더한다. `--data-dir`만 주면 실제 홈을 읽지도, 설정을 바꾸지도 않는다(기존 규칙과 같다).
- `src/app/board_app.h/.cpp`: 수집기 시작, 병합, 필터, 표식, 상태 줄
- `CMakeLists.txt`, `README.md`, `docs/event-protocol.md`

## 단계

1. **provider 표 일반화**: 기능 변화 없이 두 provider로 기존 테스트와 `ui-smoke.ps1`이 통과하게 한다.
2. **로컬 관측기**
   - 하는 일:
     - `presence` 잠금으로 열린 대화 판별
     - `annotations` 이름
     - `transcript.jsonl` 추가분 읽기(열린 대화 + 최근 48시간, 최대 64개)
     - 상태·요약·도구·백그라운드 하위
   - 이 단계만으로도 이미 열린 대화가 바로 보인다.
   - 검사 항목:
     - 제목 추출
     - `thinking` 제외
     - 마크다운 걷어 내기
     - `<대화 ID>/task-N` 백그라운드 시작·끝
     - 잘린 `SYSTEM_MESSAGE`
     - 부분 줄·손상 줄
     - 실제 잠금 파일
     - `pbtxt`의 두 칸 공백과 이스케이프
     - 프로젝트 루트 대조의 세 모양(`folderUri`, `gitFolder`, 없음)
3. **Hook 수동 확인**: `hooks.json` 형식과 `Stop` 응답을 실제 Antigravity에서 확인한다. 이것을 확인하기 전에는 4단계로 가지 않는다.
4. **Hook 자동 등록**
   - `SessionStart`, `PreInvocation`, `PostToolUse`, `Stop` 4개를 등록한다.
   - `session-board` 키만 관리하고, 다른 묶음과 순서는 보존한다.
   - 끄기 표시 파일은 `antigravity-hooks.disabled`다.
   - `hook-startup.ps1`에 Antigravity를 더한다.
5. **화면과 실제 검증**: 필터, 표식, 상태 줄을 추가한다. UIA로 세 provider 카드가 함께 보이는지 확인하고 화면을 캡처한다.

## 개선 후보와의 관계

- **토큰 사용량**: Antigravity 기록의 각 줄에 `input_tokens`·`cache_read_tokens`·`output_tokens`가 있다. Codex `token_count`와 함께 하면 세 provider에 같은 "토큰" 표시를 줄 수 있다.
- **알림·확인 안 한 항목·전역 단축키**: provider와 무관하게 공통 상태로 만든다. Antigravity는 위 "승인 대기 공백" 때문에 처음에는 입력 대기(`ask_question`)만 알린다.
- **긴 도구 오판 방지**: Antigravity는 결과가 아직 붙지 않은 도구 호출(`PLANNER_RESPONSE` 뒤 `GENERIC` 없음)과 `presence` 잠금이 바로 그 증거다. 공통 규칙으로 만들면 셋 다 혜택을 본다.
- **Gemini CLI**: provider 표 일반화를 먼저 하면 같은 `~/.gemini` 아래의 Gemini CLI도 넷째 provider로 붙이기 쉽다.

## 정한 것 (2026-10-28)

1. **승인 대기**: `PreToolUse`를 등록하되 결정 없이 답한다(다). Antigravity가 결정 없는 응답을 받는지는 3단계에서 확인한다. 받지 않으면 (가) 승인 대기 상태를 두지 않는 쪽으로 물러선다.
2. **provider 표식**: Google Gemini 표식
3. **`presence/`가 없는 제품**: 최근 기록으로만 보인다
4. **필터 모양**: 표식만 쓴 칩

## 남은 일

- 3단계: `hooks.json` 형식, `Stop` 응답, 결정 없는 `PreToolUse` 응답을 실제 Antigravity에서 확인한다.
- 4단계: Hook 자동 등록과 `install-antigravity-hooks.ps1`, `--no-antigravity-hooks`, 창 이동.
