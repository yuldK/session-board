# Claude Code 연동

Claude Code 세션은 두 경로로 모아 `(provider, session_id)` 기준으로 한 카드에 병합한다.

1. **로컬 관측**(기본): C++ 백그라운드 수집기 `claude_monitor`가 실행 중 세션 등록 정보와 transcript를 읽는다. 앱을 켜기 전부터 열려 있던 세션과 Hook 설치 전에 시작한 세션도 보인다.
2. **Hook**(보완): `session-board-hook.exe --provider claude`가 lifecycle·승인·입력 대기를 공통 규격 이벤트로 저장한다.

Claude 원본 기록과 등록 파일은 읽기만 한다. 별도 런타임·서버·AI API 호출은 없다.

## 확인한 설치 환경 (2026-10-25)

| 항목 | 값 |
| --- | --- |
| PATH의 `claude` | 2.1.293 |
| 실제 실행 중인 세션 | 2.1.308 (`claude-desktop`), 2.1.309 (`claude-vscode`) |
| 설정 루트 | `CLAUDE_CONFIG_DIR`, 없으면 `%USERPROFILE%/.claude` |
| transcript | `<설정 루트>/projects/<프로젝트 폴더>/<sessionId>.jsonl` |
| 하위 에이전트 기록 | `<설정 루트>/projects/<프로젝트 폴더>/<sessionId>/subagents/*.jsonl` |
| 실행 중 세션 등록 | `<설정 루트>/sessions/<pid>.json` |

PATH의 CLI와 desktop 앱이 내장한 실행 파일의 버전이 다르다. 지원 기능은 실제 세션 실행 파일(2.1.308) 기준으로 확인했다.
`MessageDisplay`, `prompt_id`, `last_assistant_message`, `PostToolUseFailure`가 이 실행 파일에 있다.

## 활성 세션 판별

`sessions/<pid>.json`은 Claude Code가 실행 중 세션마다 쓰는 등록 정보다. 공식 문서에 없는 내부 형식이므로 다음 근거가 모두 맞을 때만 `runtime_loaded`(열린 세션)로 본다.

- `pid`의 프로세스를 열 수 있고 아직 종료되지 않았다.
- 그 프로세스의 생성 시각(`GetProcessTimes`, FILETIME)이 `procStart`와 정확히 같다. PID 재사용을 거른다.
- `pidDomain`이 있으면 `win32:<소문자 컴퓨터 이름>`과 같다. WSL·다른 호스트의 등록은 제외한다.

파일만 남아 있거나 생성 시각이 다르면 열린 세션으로 단정하지 않는다. 이 경우 transcript가 최근 15분 안에 바뀌었으면 최근 기록으로만 보인다.
같은 폴더의 `<pid>.<hash>.key` 파일은 자격 증명일 수 있어 열지 않는다.

등록 정보의 `status`는 다음처럼 해석한다.

| `status` | 표시 |
| --- | --- |
| `busy` | 작업 중. 열린 turn을 유지한다 |
| `idle` | 응답 완료. 세션은 열린 상태로 유지한다 |
| `waiting` + `waitingFor` `input needed`·`dialog open`·`goal proposal` | 입력 대기 |
| `waiting` + 그 밖의 `waitingFor` | 승인 대기 |

열린 세션의 `cwd`와 `name`은 등록 정보를 우선한다. transcript의 `cwd`는 셸 이동을 따라가기 때문이다.

## transcript 해석

- `type`이 `user`, `assistant`, `custom-title`인 레코드만 본다. `api-request*` 레코드는 thinking·tool_result의 사본을 담고 있어 무시한다.
- `isSidechain`, `isMeta`, `isCompactSummary` 레코드와 다른 `sessionId`의 레코드는 루트 상태에 반영하지 않는다. 하위 에이전트 기록은 아래의 하위 행으로만 쓴다.
- 새 turn: `origin.kind == "human"`인 `user` 레코드. turn ID는 `promptId`다. 요청 원문은 저장하지 않는다.
- 공개 진행 메시지: `assistant` 레코드의 `text` 블록에서 첫 줄 또는 첫 문장(최대 240 bytes)을 쓴다. `thinking` 블록은 쓰지 않는다.
- 도구: `tool_use` 블록의 `id`로 시작, 같은 ID의 `tool_result`로 종료한다. 병렬 도구는 ID 집합으로 유지한다.
- `stop_reason: end_turn`은 응답 완료다. `[Request interrupted by user…]`는 중단이다. 등록 정보가 transcript보다 최신일 때 그 상태를 우선한다. 승인 후 최신 `busy`가 오면 작업 중으로 돌아간다.
- 추가분만 읽는다. 한 번에 파일당 최대 8 MiB, 한 줄 최대 512 KiB를 읽는다. 잘린 마지막 줄은 다음 쓰기를 기다린다. 파일 크기가 줄거나 파일 ID가 바뀌면 처음부터 다시 읽되 다 읽을 때까지 이전 결과를 유지한다.
- 탐색 대상은 열린 세션의 transcript와 최근 15분 안에 바뀐 transcript 최대 64개다. 새 등록이 생기면 즉시, 그 밖에는 5초마다 다시 탐색한다. 등록 정보와 추가분은 약 1초마다 읽는다.

## 하위 에이전트·백그라운드 작업

부모 카드 안의 하위 행으로 보인다. 규칙은 [이벤트 규격](event-protocol.md)의 하위 절을 따른다. 설계 배경은 [하위 세션 설계](child-sessions-design.md)에 있다.

하위 에이전트(`<세션>/subagents/`):

| 파일 | 쓰는 것 |
| --- | --- |
| `agent-<id>.meta.json` | `agentType`·`description`(제목)·`toolUseId`·`spawnDepth`·`requestShape`(`background`)·`finished`. 64 KiB 넘으면 읽지 않는다 |
| `agent-<id>.jsonl` | `isSidechain` 레코드만 같은 해석기로 읽어 현재 도구·공개 진행 문장을 얻는다. 첫 레코드 시각이 시작 시각이다 |
| `agent-<id>.prefix.json` | 읽지 않는다 |

- `meta.json`은 실행 중 기록과 함께 다시 쓰이고 끝나면 `finished: true`가 붙는다(2.1.308에서 확인).
- 폴더 시각이 바뀔 때만 다시 훑고, 최근에 바뀐 메타 24개만 본다. 메타는 시각이 바뀔 때만, 기록은 추가분만 읽는다. 15분 넘게 지난 끝난 하위의 기록은 읽지 않는다.
- 하위에게 준 지시문(첫 사용자 레코드, 메타·결과의 `prompt`)은 저장하지 않는다.
- 결과를 도구로 넘기고 끝나는 하위는 마지막 답변 문장이 없을 수 있다. 끝난 하위에는 진행 문구를 남기지 않는다.

백그라운드 명령(부모 transcript):

- `tool_use.input.run_in_background: true`인 호출의 `input.description`을 제목으로 기억했다가, 같은 ID 결과 레코드의 `toolUseResult.backgroundTaskId`로 하위 행을 만든다.
- 끝은 `<task-notification>`이다. `queue-operation`의 `content`, `attachment`(`queued_command`)의 `prompt`, `user` 메시지 어디에 와도 같게 적용한다. 이 알림은 새 turn이 아니다.
  - `<status>`: `completed` → 완료, `failed` → 실패, `stopped`·`killed` → 중지됨. 같은 ID가 여러 번 오면 마지막 것이 남는다.
  - 세션 재시작 뒤의 요약 알림은 `<task-id>`를 여럿 담는다. `__`로 시작하는 자기 ID를 빼고 모두 닫는다.
- `TaskStop`·`KillShell`로 직접 멈춘 작업은 알림을 남기지 않는다. 멈춤 도구의 오류 아닌 결과로 중지됨 처리한다.
- 같은 알림은 백그라운드 에이전트의 실패·중지도 알려 준다. 끝나지 않은 에이전트의 알림이 그 뒤 기록보다 오래됐으면 재개 전의 것이므로 무시한다.

부모 응답이 끝났는데(`turn_closed`) 끝 표시 없이 도는 포그라운드 하위는 `확인 불가`(부모가 중단됐으면 `중지됨`)로 닫는다. 하위 행은 매 주기 메타로 다시 만들어지므로 늦게 붙은 `finished`가 이긴다.
세션이 열려 있지 않으면(`runtime_loaded`가 아니면) 백그라운드를 포함한 도는 하위를 모두 `확인 불가`로 닫는다. 끝난 하위는 15분 뒤 카드에서 뺀다.

## Hook 이벤트

| Claude Hook | 공통 이벤트 | 비고 |
| --- | --- | --- |
| `SessionStart` | `session_started` | `source` 보존 |
| `UserPromptSubmit` | `turn_started` | `prompt_id` → `turn_id`, 요청 앞부분만 `title` |
| `PreToolUse` | `tool_started` | `AskUserQuestion`은 `input_requested` |
| `PostToolUse`, `PostToolUseFailure` | `tool_finished` | 도구 실패는 세션 실패가 아니다 |
| `PermissionRequest` | `approval_requested` | `tool_use_id` 없음 |
| `Notification` (`elicitation_dialog`, `elicitation_url_dialog`) | `input_requested` | |
| `Stop` | `turn_finished` | `last_assistant_message`의 첫 문장을 `summary`로 |
| `StopFailure` | `failed` | API 오류로 turn이 끝남 |
| `SessionEnd` | `session_ended` | |

등록하지 않거나 버리는 것:

- `agent_id`가 있는 모든 Hook: 하위 에이전트 안의 이벤트다. `SubagentStart`/`SubagentStop`도 등록하지 않는다.
- `Notification`의 `idle_prompt`: 응답이 끝나고 약 60초 동안 입력이 없으면 오는 알림일 뿐 대기 확정 신호가 아니다. `permission_prompt`는 `PermissionRequest`와 겹친다.
- `MessageDisplay`: 화면에 나오는 줄 묶음마다 실행되고 Hook 출력이 표시 텍스트를 대체한다. 프로세스 비용과 화면을 비울 위험 때문에 쓰지 않는다. 진행 메시지는 transcript로 충분하다.

Hook 실패 정책은 Codex와 같다. stdout `{}`, 종료 코드 0이며 decision이나 추가 문맥을 돌려주지 않는다. `--strict`는 테스트에서만 쓴다.

## 자동 등록

사용자가 스크립트를 실행하지 않아도 앱을 열면 Hook이 등록되도록 `ensure_claude_hooks`(`src/core/claude_hooks.*`)를 둔다. 앱은 시작할 때 이 함수를 한 번 호출한다.

1. `%LOCALAPPDATA%/SessionBoard/claude-hooks.disabled`가 있으면 아무것도 하지 않는다. 사용자가 제거를 선택했다는 표시다.
2. 앱 옆의 `session-board-hook.exe`를 `%LOCALAPPDATA%/SessionBoard/bin`으로 복사한다. 내용이 같으면 복사하지 않는다. Codex Hook도 같은 helper를 쓰며 앱의 helper는 두 provider를 모두 지원한다. 실행 중이라 교체하지 못하면 기존 helper·설정을 보존하고 다음 실행 때 다시 시도한다.
3. helper가 없거나 갱신에 실패하면 새 설정을 등록하지 않는다. 없는 실행 파일이나 Claude 미지원의 오래된 helper를 등록해 매 도구 호출마다 오류가 나는 일을 막는다.
4. `<설정 루트>/settings.json`을 순서 보존 JSON으로 읽어 Session Board 항목만 넣는다. 원하는 등록이 이미 하나씩 있으면 파일을 쓰지 않는다. 그래서 매번 실행해도 백업이 쌓이지 않는다.
5. 바꿀 것이 있으면 `settings.json.session-board-<시각>.bak` 백업을 만들고 임시 파일로 교체한다. 들여쓰기 2칸, 원래 줄 끝(LF/CRLF)을 따른다.
6. 해석할 수 없는 설정(주석·깨진 JSON 등)은 건드리지 않는다. 읽은 뒤 Claude Code나 사용자가 설정을 바꾸면 덮어쓰지 않고 다음 실행으로 미룬다. 앱 인스턴스끼리는 named mutex로 직렬화한다.
7. 어떤 실패도 예외로 앱을 멈추지 않는다. 결과의 `error`를 하단 상태에 표시한다.

`--data-dir`·`--claude-home`을 준 테스트 실행과 `--no-claude-discovery`·`--no-claude-hooks`를 준 실행에서는 호출하지 않는다.

Codex도 같은 설치 경로·mutex·백업·동시 수정 감지를 사용해 `hooks.json`에 자동 등록한다. 두 provider의 제거 표시는 각각 `claude-hooks.disabled`, `codex-hooks.disabled`다. Codex의 최초 신뢰 검토는 사용자가 `/hooks`에서 해야 한다.

Claude Code는 설정 파일 변경을 file watcher로 반영한다. `/hooks`에서 등록을 확인할 수 있다.
2.1.308 desktop 세션에서 앱이 등록한 직후 **이미 실행 중이던 세션**의 다음 도구 호출부터 Hook이 실행되는 것을 확인했다(재시작 불필요). 반영 전에도 로컬 관측으로 표시된다.
Hook의 `prompt_id`는 transcript의 `promptId`와 같은 값이어서 두 경로의 turn ID가 맞는다.

## 수동 Preview와 제거

```powershell
./scripts/install-claude-hooks.ps1 -Preview
./scripts/install-claude-hooks.ps1
./scripts/install-claude-hooks.ps1 -Remove
```

- 스크립트는 앱과 같은 명령·matcher·timeout으로 등록하므로 둘 중 어느 쪽이 먼저 등록해도 다른 쪽이 다시 쓰지 않는다.
- 기본 대상은 `<설정 루트>/settings.json`이다. `-SettingsFile`, `-InstallDirectory`, `-Executable`, `-Timeout`(초, 기본 5)으로 바꿀 수 있다.
- 쓰기 전에 백업을 만든다. `permissions`, 다른 Hook, 플러그인, 알 수 없는 키와 순서를 보존한다. `-Remove` 결과는 설치 전과 같다.
- `-Remove`는 `claude-hooks.disabled`를 만들어 앱이 다시 등록하지 않게 한다. 다시 설치하면 이 파일을 지운다.
- 실행 파일 경로에 공백이 없으면 따옴표 없이 등록해 Git Bash와 PowerShell Hook 셸 모두에서 실행된다. 공백이 있으면 Git Bash를 전제로 따옴표를 붙인다.

## 앱 연결

- `src/app/main.cpp`: `--claude-home <경로>`, `--no-claude-discovery`, `--no-claude-hooks`를 받는다. `--data-dir`만 주면 실제 Claude 홈을 읽지 않는다(Codex의 격리 규칙과 같다). 기본 실행에서만 창을 띄우기 전에 `ensure_claude_hooks`를 호출한다.
- `src/app/board_app.*`: `claude_monitor`를 시작하고, 갱신 때 Codex 병합 다음 `merge_claude_sessions`를 호출한다. Claude 수집기가 있으면 1초 주기로 다시 읽는다.
  - 펼친 카드와 상세 화면의 수집 출처는 `observation_source`에 따라 `Claude 로컬 기록`/`Codex 로컬 기록`으로 표시한다.
  - 하단은 provider마다 한 줄이다. `Claude · 자동 수집 N개 · Hooks 등록됨/병행/꺼짐` 또는 수집·등록 오류 문구를 보인다. 한 provider의 오류가 다른 줄을 가리지 않는다.
- 남은 정리(선택): `merge_codex_sessions`와 `merge_claude_sessions`, 두 수집기의 `tracked_file`·`record_time`은 provider만 다르므로 공통화할 수 있다.

## 실제 대화 검증

`scripts/verify-claude-session.ps1`은 실행 중인 앱에 UIA로 붙어 원래 세션 ID의 카드를 찾고, 본문·상태·동작·수집 출처·작업 폴더·세션 ID를 읽어 `build/current-claude-session-verification.json`과 `build/current-claude-session.png`를 남긴다.
전체 대화나 요청 원문은 남기지 않는다. 끝나면 앱을 닫지 않고, UIA 정리가 창을 최소화했으면 활성화 없이 다시 보인다.

```powershell
./scripts/verify-claude-session.ps1 -AppProcessId <앱 PID> -SessionId <Claude 세션 ID> -ExpectedProgress '<카드 본문 정규식>'
```

## 한계

- `sessions/<pid>.json`과 transcript 형식은 공개 규격이 아니다. 버전이 바뀌어 해석할 수 없으면 등록 정보가 없는 세션은 최근 기록으로만 보이고 Hook 수집은 계속된다.
- 승인 후 도구가 오래 실행되면 Hook 경로는 `PostToolUse`까지 승인 대기로 남는다. 열린 세션은 등록 정보의 `busy`가 더 최근이면 작업 중으로 보인다.
- 차단형 Stop Hook이 응답을 이어 가게 하면 Hook 경로는 이미 turn을 닫았으므로 같은 turn의 도구 이벤트를 버린다. 로컬 관측이 이를 보완한다.
- 하위 에이전트·백그라운드 작업은 로컬 기록으로만 모은다. `agent_id`가 있는 Hook·`SubagentStart`/`SubagentStop`·`Stop`의 `background_tasks`는 payload를 확인한 뒤 붙인다(설계 4단계). 그래서 하위의 승인 대기는 아직 보이지 않는다.
- 원격·WSL·클라우드 세션은 대상이 아니다.
