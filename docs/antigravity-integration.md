# Antigravity 연동

Google Antigravity(`provider: "antigravity"`)의 대화를 로컬 기록만으로 모은다. 설계 배경과 남은 단계는 [Antigravity 연동 설계](antigravity-design.md)에 있다.

- SQLite는 쓰지 않는다. `conversation_summaries.db`, `conversations/*.db`, `*.pb`는 열지 않는다.
- 아직 Hook을 등록하지 않는다. `~/.gemini/config/hooks.json`의 형식과 `Stop` 응답을 실제로 확인한 뒤 붙인다.
- 정규화 이벤트(`session-board.exe --hook --normalized`)의 `provider`로 `antigravity`를 받는다. 그 스냅숏은 같은 ID의 로컬 기록과 합친다.

## 읽는 곳

`<홈>`은 `%USERPROFILE%/.gemini`이고 `--antigravity-home <경로>`로 바꾼다. 그 아래 제품 폴더 `antigravity`, `antigravity-ide`, `antigravity-cli` 중 있는 것을 모두 읽는다.

| 파일 | 쓰는 것 |
| --- | --- |
| `<제품>/presence/<대화 ID>.lock` | 앱이 공유를 막고 열어 둔 잠금이면 열린 대화(`runtime_loaded`). 파일이 남아 있는 것만으로는 열렸다고 보지 않는다 |
| `<제품>/annotations/<대화 ID>.pbtxt` | `title:"…"`을 대화 이름(`name`)으로. protobuf 텍스트 이스케이프(8진수 UTF-8 바이트 포함)를 푼다 |
| `<제품>/brain/<대화 ID>/.system_generated/logs/transcript.jsonl` | 추가분만 읽는다. 열린 대화와 최근 48시간 안에 바뀐 기록 중 최대 64개 |
| `<홈>/config/projects/*.json` | `projectResources.resources[].folderUri` 또는 `gitFolder.folderUri`의 `file:///` 주소를 프로젝트 루트로 |

- 잠금 확인은 모든 공유를 허락하고 열어 본다. 우리가 확인하는 동안에도 Antigravity가 잠금을 다시 잡을 수 있다.
- `presence`가 없는 제품(이 PC의 IDE)은 열림을 알 수 없다. 그 기록은 최근 기록으로만 15분간 보인다.
- 열린 대화에 기록이 아직 없으면 잠금과 이름만으로 `다음 요청 대기` 카드를 보인다.

## 기록 해석

| `source` / `type` | 해석 |
| --- | --- |
| `USER_EXPLICIT` / `USER_INPUT` | 새 turn(`turn_id = step-<step_index>`). `<USER_REQUEST>` 첫 줄을 `title`로(최대 180 bytes), `<USER_SETTINGS_CHANGE>`의 `` `Model Selection` from … to <모델>. ``을 `model`로 |
| `MODEL` / `PLANNER_RESPONSE`, `tool_calls` 있음 | 도구마다 `<step_index>:<순번>` ID로 실행 중 도구를 더한다. 공개 `content` 첫 문장을 `summary`로 |
| 위 도구가 `ask_question` | 입력 대기 |
| 위 도구가 `invoke_subagent` | 하위 에이전트 행(`args.Role`, `args.TypeName`). 그 도구의 결과가 오면 닫는다 |
| `MODEL` / `GENERIC`, `DONE`·`ERROR` | 결과를 기다리던 도구 하나가 끝난다(호출 순서대로). `ERROR`는 도구 하나의 실패라 세션을 `failed`로 만들지 않는다 |
| `MODEL` / `GENERIC`, `RUNNING` + `running as a background task with task id: <작업 ID>` | 그 도구는 끝나고 백그라운드 명령 하위 행이 열린다. 작업 ID는 `<대화 ID>/task-N` 전체다 |
| `MODEL` / `PLANNER_RESPONSE`, 도구 없음 `DONE` | 응답 완료(묻고 끝나면 응답 대기). 포그라운드 하위는 `확인 불가`로 닫고 백그라운드 명령은 남긴다 |
| `SYSTEM` / `SYSTEM_MESSAGE` | 사용자 turn이 아니다. `sender=<작업 ID>`와 `Task id "…" finished`면 그 하위를 완료로 닫는다 |
| `SYSTEM` / `CHECKPOINT` | 상태는 바꾸지 않는다. `title`이 비었을 때만 `# USER Objective:` 다음 줄을 쓴다 |

- 같은 `step_index`는 끝난 모양(`DONE`·`ERROR`, 도구 호출, 백그라운드 시작)으로 한 번만 적용한다. 끝나지 않은 같은 단계는 다시 받는다.
- 공개 답변은 마크다운이다. 첫 줄이 제목(`### …`)이면 본문 첫 문장을 쓰고, 강조·목록 기호를 걷어 낸다.
- 시각은 줄의 `created_at`(UTC)이다. `content` 안의 `Created At:`은 현지 시각이라 쓰지 않는다.
- 쓰지 않는 것: 비공개 `thinking`, 도구 인자의 명령(`CommandLine`)·내용, 백그라운드 작업 설명(`Task Description`은 명령 원문), 도구 결과, `tasks/*.log`, `steps/`, `messages/`.
- 작업 폴더: Hook이 `cwd`를 주면 그것을 쓴다. 로컬 기록만 있으면 도구 인자의 경로(`Path`·`AbsolutePath`·`TargetFile`·`Cwd`·`DirectoryPath`·`SearchPath`)를 최근 것부터 프로젝트 루트와 맞춰 가장 긴 루트를 쓴다. 맞는 것이 없으면 비워 둔다.

## 열림과 닫힘

- 열린 대화는 응답이 끝나도 카드가 남는다. 닫힌 대화는 마지막 활동 뒤 15분간 최근 기록으로 남는다.
- 이번 실행에서 열린 것을 본 대화의 잠금이 풀렸는데 작업 중이었다면 `세션 종료`로 확정한다.

## 한계

- 승인 대기를 기록에서 구별하지 못한다. 승인을 기다리는 도구는 결과가 오기 전까지 `작업 중`(오래되면 `작업 중 · N분째`)으로 보인다. 설계에서는 결정 없이 답하는 `PreToolUse` Hook으로 메우기로 했고, Antigravity가 그 응답을 받는지 확인한 뒤 붙인다.
- 창 이동 단추는 Hook이 창을 기록해야 켜진다. 지금은 로컬 기록만 있어 꺼져 있다. Antigravity 대화를 여는 주소 형식은 확인하지 못했다.
- 기록 형식은 Antigravity 버전에 따라 바뀔 수 있다. 확인한 형식은 2026-10-28 이 PC의 Antigravity 2.0 데스크톱 기록이다.
