# 하위 세션·백그라운드 작업 설계 (초안)

상태: 1~3단계 구현됨(2026-10-25). 확정 규칙은 `event-protocol.md`와 `claude-integration.md`가 기준이며, 이 문서는 설계 배경과 남은 단계를 담는다.

지금은 루트 세션만 보인다. 하위 에이전트 이벤트는 버리고, 백그라운드 명령은 시작 직후 `PostToolUse`가 와서 부모가 응답 완료처럼 보인다.
이 설계는 둘을 **부모 카드 안의 하위 행**으로 보인다.

## 대상

| 종류 | 출처 | 표시 |
| --- | --- | --- |
| 하위 에이전트 | Claude `Agent` 도구, Codex `thread_spawn` 하위 스레드 | 부모 아래 하위 행 |
| 백그라운드 에이전트 | Claude `Agent` + `requestShape: "background"` | 하위 행 + `백그라운드` 표시 |
| 백그라운드 명령 | Claude `Bash`/`PowerShell`의 `run_in_background` | 하위 행 + `백그라운드` 표시 |

백그라운드 명령도 개수만 세지 않고 하위 행으로 보인다. "무엇이 아직 돌고 있는가"가 이번 요구의 핵심이기 때문이다.

제외:

- **Claude 데스크톱의 연결 세션**(다른 세션에서 시작한 세션): `sessions/<pid>.json`에 `hostSessionId`는 있으나 부모를 가리키는 값이 없다. 각자 루트 카드로 남는다.
- **Codex `source.subagent.other`**(예: `guardian` 승인 검토)·`review`·`compact`: `parent_thread_id`가 없고 사용자가 띄운 작업이 아니다. 지금처럼 버린다.
- **Codex 백그라운드 터미널**: 확인한 신호가 없다. 별도 조사 후 추가한다.

## 확인한 신호 (2026-10-25, Claude Code 2.1.308)

### Claude 로컬 기록

`<설정 루트>/projects/<프로젝트>/<sessionId>/subagents/`에 하위 에이전트마다 세 파일이 있다.

| 파일 | 내용 |
| --- | --- |
| `agent-<id>.jsonl` | 하위 transcript. 모든 레코드가 `isSidechain: true`, `agentId`, 부모 `sessionId`·`promptId`를 갖는다 |
| `agent-<id>.meta.json` | `agentType`, `description`, `toolUseId`, `spawnDepth`, `requestShape`, `model`, 끝나면 `finished: true` |
| `agent-<id>.prefix.json` | 시스템 문맥 사본. 읽지 않는다 |

- `meta.json`은 실행 중 transcript와 함께 다시 쓰이고, 끝날 때 `finished: true`가 붙는다(실행 중 파일에는 키가 없음을 직접 확인).
- `toolUseId`는 부모 transcript의 `Agent` `tool_use` ID다. 이것으로 부모 turn에 연결한다.
- `description`은 모델이 쓴 3~5단어 작업 이름이다. 하위 행 제목으로 쓴다. 하위 에이전트에게 준 프롬프트는 저장하지 않는다.

부모 transcript:

- 백그라운드 명령: `tool_use.input.run_in_background: true`, `input.description`이 제목. 결과 레코드의 `toolUseResult.backgroundTaskId`가 작업 ID다.
- 완료 알림: `user` 레코드의 `<task-notification>` 블록. `<task-id>`, `<tool-use-id>`, `<status>`(`completed`·`failed`·`stopped` 확인), `<summary>`.
  - 같은 task-id가 여러 번 올 수 있다(재개된 에이전트). 마지막 것을 쓴다.
  - 세션 재시작 뒤 `__orphan_summary__:shell` 알림은 이전 세션의 끝나지 않은 명령 목록이다. 그 ID들을 `stopped`(확인 불가)로 닫는다.

### Claude Hook

- 하위 에이전트 안의 모든 Hook에는 `agent_id`(와 `agent_type`)가 붙는다. 지금은 `session.cpp`에서 버린다.
  이 Hook들은 **이미 실행되고 있으므로** 받아들여도 프로세스가 늘지 않고 파일 쓰기만 는다. `MessageDisplay`를 뺀 이유(프로세스 비용)는 여기 해당하지 않는다.
- `SubagentStart`·`SubagentStop`은 지금 등록하지 않는다. 등록하면 시작·끝을 바로 안다.
  **구현 전 확인 필요**: 실제 payload를 한 번 캡처해 `agent_type`·`last_assistant_message` 필드 존재를 확인한다.
- `Stop`의 `background_tasks`: 응답이 끝날 때 아직 도는 작업 목록. **형식 확인 필요.** 확인되면 Hook 경로만으로도 "응답 완료 · 백그라운드 N 진행"을 안다.
- 백그라운드 명령의 **끝**은 Hook으로 오지 않는다. 로컬 기록의 `<task-notification>`이 주 경로다.

### Codex

- 하위 스레드는 별도 rollout 파일이다. `session_meta.source.subagent.thread_spawn = { parent_thread_id, depth, agent_nickname?, agent_role? }`(app-server 스키마로 확인).
- 이 PC에는 `thread_spawn` 실제 기록이 없다. 스키마 기준으로 구현하고 fixture로 검증한 뒤, 실제 기록이 생기면 다시 확인한다.

## 데이터 모델

부모 스냅숏 안에 하위 목록을 둔다. Claude 하위 에이전트는 부모와 `session_id`가 같아서, 따로 두면 지금의 `(provider, session_id)` 열쇠·병합·파일 이름이 모두 충돌한다.

```cpp
enum class child_kind
{
    agent,
    background_command,
};

struct session_child
{
    child_kind kind { child_kind::agent };
    std::string child_id {};       // Claude agentId 또는 backgroundTaskId, Codex 하위 thread ID
    std::string parent_tool_id {}; // 부모의 tool_use ID
    std::string agent_type {};     // Explore, general-purpose, Codex agent_role
    std::string title {};          // description, 최대 180 bytes
    std::string activity {};       // 하위 transcript의 공개 진행 문장 또는 현재 도구
    std::string tool_name {};
    session_status status { session_status::running };
    bool background { false };
    std::uint32_t depth { 1 };
    std::int64_t started_ms { 0 };
    std::int64_t updated_ms { 0 };
};

// session_snapshot에 추가
std::vector<session_child> children {};
```

- 상한: 부모마다 최근 24개. 넘치면 끝난 것부터 버린다. 스냅숏 64 KiB 상한 안에 든다.
- 깊이 2 이상(하위의 하위)은 루트 바로 아래로 평평하게 둔다. `depth`만 남겨 상세에서 `깊이 2`로 보인다.
- UI 열쇠: `<부모 열쇠>-child-<stable_key(child_id)>`. UIA 이름도 이 규칙을 따른다.
- Codex 하위 스레드는 자기 rollout을 지금의 reader로 읽어 스냅숏을 만든 뒤 `session_child`로 바꿔 `parent_thread_id`의 부모에 붙인다. 부모가 아직 안 보이면 붙일 때까지 보관하고 따로 보이지 않는다.

## 이벤트 규격 변경 (v1에 선택 필드 추가)

`schema_version`은 1로 둔다. 필드는 모두 선택이고, 새 이벤트 이름은 helper와 앱이 함께 배포되므로 따로 협상하지 않는다.

| 필드 | 규칙 |
| --- | --- |
| `child_id` | 있으면 이 이벤트는 그 하위 행에 적용한다. 부모 상태·도구 목록을 바꾸지 않는다 |
| `child_type` | `agent` 또는 `background_command` |
| `parent_tool_id` | 하위를 만든 부모 tool_use ID |
| `agent_type` | 최대 128 bytes |
| `background` | 불리언 |

| 새 이벤트 | 의미 |
| --- | --- |
| `child_started` | 하위 시작. 같은 `child_id`가 있으면 갱신만 한다 |
| `child_finished` | 하위 끝. `outcome`에 `completed`·`failed`·`stopped` |

`child_id`가 붙은 `tool_started`·`tool_finished`·`approval_requested`·`input_requested`는 하위 행의 상태만 바꾼다.

Claude Hook 대응:

| Claude Hook | 공통 이벤트 |
| --- | --- |
| `SubagentStart` | `child_started` (`child_id = agent_id`) |
| `SubagentStop` | `child_finished` (`completed`), 마지막 답변 첫 문장을 하위 `activity`로 |
| `agent_id`가 있는 `PreToolUse`·`PostToolUse`·`PermissionRequest` 등 | 같은 이벤트 + `child_id` |
| `PostToolUse`의 `tool_response.backgroundTaskId` | `child_started` (`background_command`, 제목은 `tool_input.description`) |

## 상태 규칙

- **부모 상태는 부모 것만 말한다.** 하위가 돌아도 부모 `status`를 `running`으로 바꾸지 않는다.
- **묶음 배치는 합친 상태를 쓴다.**
  - 부모나 하위 중 하나라도 승인·입력 대기·오류 → `확인 필요` 묶음.
  - 그 밖에 하위 하나라도 진행 중 → `진행 중 · 대기` 묶음(위젯), `진행 중` 묶음(상세).
- 부모 turn이 끝났는데 백그라운드가 돌면 부모 활동 줄은 `응답 완료 · 백그라운드 2 진행 중`.
- 새 turn(`turn_started`)은 부모 도구 목록만 비운다. 백그라운드 하위는 turn을 넘어 유지한다.
  끝나지 않은 **포그라운드** 하위는 부모 turn이 `interrupted`면 `중단됨`, `turn_finished`면 `확인 불가`로 닫는다.
- 부모 `session_ended`: 진행 중 하위를 모두 `확인 불가`로 닫는다.
- 10분 무소식 규칙(`상태 확인 필요`)은 하위 에이전트에만 적용한다. 백그라운드 명령은 원래 소식이 없으므로 제외하고 경과 시간(`12분째`)을 보인다.
- 하위의 승인 요청은 하위 행에 배지를 달고, 부모 머리 줄에도 `하위 승인 대기`를 보인다.

## 화면

### 위젯 카드

```
┌──────────────────────────────────────────────────┐
│ ● 작업 중   Claude · session-borad     방금   ↗  │
│ 하위 세션 지원 설계를 정리하고 있습니다            │
│ 명령 실행 중 · PowerShell                         │
│ ┊ 하위 2 · 백그라운드 1                           │
│ ├ ● Explore  Claude hooks 신호 조사       40초    │
│ ├ ● 백그라운드  Release 빌드              3분째   │
│ └ ○ Plan  화면 구조 검토 · 완료           2분 전  │
└──────────────────────────────────────────────────┘
```

- 하위 행은 부모 카드 표면 **안**에 그린다. 별도 카드나 묶음 최상위 행이 되지 않는다.
- 들여쓰기 16px, 왼쪽에 1px 연결선(부모 키 컬러를 옅게). 한 줄 높이 20px, 글자 11px.
- 한 줄 구성: 상태 점 · 종류(`agent_type` 또는 `백그라운드`) · 제목 · (오른쪽) 경과 또는 나이. provider·폴더는 부모와 같으므로 반복하지 않는다.
- 접힌 카드: 진행 중·확인 필요 하위와 끝난 지 2분 안의 하위만 보인다. 나머지는 `완료 3개 더` 한 줄로 줄인다.
- 펼친 카드: 하위 전부(최대 24), 각 행 아래에 활동 한 줄을 더 보인다.
- 하위 행을 누르면 부모 카드가 펼쳐진다. 하위 행만 따로 펼치지 않는다(위젯 높이 예측을 단순하게).
- "세션 창으로 이동"은 부모의 `host_window`를 쓴다. 하위 행에는 단추를 두지 않는다.

### 상세 창 목록

- 부모 행 바로 아래에 하위 행을 들여 넣는다. 부모가 어느 묶음이든 하위는 부모를 따라간다.
- 하위 행도 고를 수 있다. 상세 패널은 `종류`, `에이전트 종류`, `상태`, `현재 동작`, `시작`·`경과`, `부모 세션`, `하위 ID`, `깊이`를 보인다. 요청 칸 대신 `description`을 보인다.

### 위젯 머릿줄 수

`확인 N · 진행 M`의 M은 루트 카드 수로 센다(하위는 세지 않는다). 하위로 인한 확인 필요는 부모 카드 하나로 센다.

## 바뀌는 곳

| 위치 | 변경 |
| --- | --- |
| `docs/event-protocol.md` | 필드·이벤트·상태 규칙 추가, "v1 UI는 루트 세션만 표시" 문장 교체 |
| `docs/claude-integration.md` | `agent_id` 버림 → 수용, `SubagentStart/Stop` 등록, 133행 한계 해소 |
| `src/core/session.*` | `session_child`, `children`, `child_id` 이벤트 적용, 합친 상태 함수 |
| `src/core/claude_hooks.cpp`, `src/hook/main.cpp` | Subagent Hook 등록·변환 |
| `src/core/claude_monitor.cpp` | `subagents/` 읽기(240행의 제외 해제), `<task-notification>`·`backgroundTaskId` 해석 |
| `src/core/codex_monitor.cpp` | `thread_spawn` 하위를 부모에 붙임(463행 `root_session` 분기) |
| `src/app/board_app.cpp` | 하위 행, `group_of`·`active`가 합친 상태 사용, 상세 패널의 하위 표시 |

뒤집히는 테스트(지금은 제외를 확인함): `session_tests.cpp` 79·241행, `claude_tests.cpp` 97·99·290·342행. 새 테스트는 하위 시작·끝, 백그라운드가 turn을 넘는 경우, 하위 승인 대기의 부모 끌어올림, 24개 상한, orphan 알림, `thread_spawn` fixture를 다룬다.

## 단계

1. (완료) 모델·규격·상태 규칙과 테스트 (`session.*`, `event-protocol.md`).
2. (완료) Claude 로컬 기록: `subagents/`·`<task-notification>`·`backgroundTaskId`·`TaskStop` 결과.
3. (완료) 위젯·상세 화면의 하위 행.
4. Claude Hook: payload 캡처로 필드 확인 후 `SubagentStart/Stop`·`agent_id` 수용.
5. Codex `thread_spawn`.

## 정할 것

- "서브 세션"에 Claude 데스크톱의 연결 세션(다른 세션이 시작한 세션)도 들어가는가. 들어간다면 지금 부모를 가리키는 신호가 없어 따로 찾아야 한다.
- 접힌 카드에서 끝난 하위를 보이는 시간(초안 2분).
