# Session Board 이벤트 규격 v1

수집 경로: AI lifecycle hook → `session-board.exe --hook` → 세션별 원자적 JSON 스냅숏 → luil 앱.

Codex 보완 경로: 기존 로컬 세션의 JSONL 기록·작성 잠금 → C++ 백그라운드 수집기 → 동일 ID의 Hook 스냅숏과 메모리에서 병합 → luil 앱.
Codex 원본 파일과 잠금 파일의 내용은 수정하지 않는다. 별도의 AI 요청이나 검증 세션을 생성하지 않는다.

Claude 보완 경로: 실행 중 세션 등록 정보(`sessions/<pid>.json`, 프로세스 생성 시각으로 검증)와 transcript 추가분 → C++ 백그라운드 수집기 → 동일 ID의 Hook 스냅숏과 메모리에서 병합 → luil 앱.
Claude 원본 Hook은 `--provider claude`로 전달한다. Claude의 `prompt_id`를 `turn_id`로 보존하며 규격 필드는 바꾸지 않는다. 경로·이벤트 대응·한계는 [Claude 연동](claude-integration.md)에 둔다.

Antigravity 경로: 제품 폴더(`antigravity`, `antigravity-ide`, `antigravity-cli`)의 `presence/<대화 ID>.lock` 잠금과 `brain/<대화 ID>/.system_generated/logs/transcript.jsonl` 추가분 → C++ 백그라운드 수집기 → luil 앱. 아직 Hook을 등록하지 않으며, 정규화 이벤트(`--normalized`)로 받은 Antigravity 스냅숏은 같은 ID의 로컬 기록과 합친다. 근거와 한계는 [Antigravity 연동](antigravity-integration.md)에 둔다.

UI가 꺼져 있어도 Hook 실행 파일은 마지막 상태를 저장한다. 서버, API 키, OpenAI/Anthropic API 호출은 필요 없다.
이 규격은 로컬 Windows 환경을 대상으로 한다. WSL·원격 호스트·클라우드 세션은 별도 전달 어댑터가 필요하다.

일반 앱 시작 시 Claude `settings.json`과 Codex `hooks.json`에 provider별 Hook을 자동 등록한다. 앱 자신을 공용 위치(`%LOCALAPPDATA%/SessionBoard/bin`)에 먼저 복사하고 기존 설정·다른 Hook을 보존하며, 변경 전 백업과 동시 수정 감지를 적용한다. 같은 정의는 다시 쓰지 않는다. 등록 실패는 하단에 표시하고 로컬 관측을 계속한다.
Codex는 최초 등록·정의 변경 후 `/hooks`에서 사용자 신뢰 검토가 필요하다. 자동 등록은 신뢰 승인을 대신하지 않는다. `--data-dir`·provider별 custom home은 실제 설정을 바꾸지 않는 격리 실행이다.

## 전달

앱 실행 파일의 첫 인자를 `--hook`으로 주고 UTF-8 JSON 객체 하나를 stdin으로 전달한 뒤 stdin을 닫는다. 이 실행은 창을 만들지 않고 이벤트 하나만 저장한 뒤 끝난다. stdout의 `{}`는 AI에게 문맥을 추가하거나 작업을 중단하지 않는 응답이다.

```powershell
$event | ConvertTo-Json -Compress | & './session-board.exe' --hook --normalized
```

- 기본 저장 위치: `%LOCALAPPDATA%/SessionBoard/sessions`.
- 테스트/데모: `--data-dir "절대 경로"`. UI에도 같은 인자를 준다.
- 기본 오류 정책: stderr에 일반 진단 한 줄, stdout `{}`, 종료 코드 0. 원래 AI 작업을 막지 않는다.
- 테스트용 `--strict`: 실패 시 종료 코드 1. 실제 Hook 설정에서는 사용하지 않는다.
- 입력 상한 4 MiB. 저장 파일 상한 64 KiB. 긴 문자열은 UTF-8 경계를 보존하며 줄인다.
- 원본 transcript, 명령 인자, 도구 출력, 전체 프롬프트는 저장하지 않는다. 제목에는 최신 요청의 앞부분만 사용한다.

## 정규화된 이벤트

```json
{
    "schema_version": 1,
    "provider": "claude",
    "session_id": "native-session-id",
    "event": "tool_started",
    "timestamp_ms": 1792840000000,
    "turn_id": "native-turn-id-if-available",
    "cwd": "C:/projects/example",
    "model": "actual-model-if-available",
    "title": "최신 사용자 요청의 짧은 제목",
    "tool_id": "native-tool-use-id",
    "tool_name": "Bash",
    "summary": "선택적인 짧은 활동 설명",
    "source": "startup"
}
```

| 필드 | 필수 | 규칙 |
| --- | --- | --- |
| `schema_version` | 예 | 정수 `1` |
| `provider` | 예 | `codex`, `claude`, `antigravity` 중 하나 |
| `session_id` | 예 | 원래 세션 ID, 비어 있지 않은 UTF-8 문자열, 최대 512 bytes |
| `event` | 예 | 아래의 이벤트 중 하나 |
| `timestamp_ms` | 예 | UTC Unix epoch milliseconds, 양의 정수. 현재보다 5분 이상 미래는 거부 |
| `turn_id` | 아니요 | 원래 turn ID. 없으면 생략. 임의의 UUID를 매 Hook마다 만들지 않는다 |
| `cwd` | 아니요 | 원래 작업 폴더, 최대 4096 bytes |
| `model` | 아니요 | payload에 있는 모델명, 최대 128 bytes. 추정하지 않는다 |
| `title` | 아니요 | 최대 180 bytes. `turn_started`에서 최신 요청으로 갱신하며, 그 밖에는 저장된 제목이 없을 때만 채운다 |
| `tool_id` | 아니요 | 원래 tool-use ID. 병렬 도구를 구별하므로 가능하면 반드시 전달 |
| `tool_name` | 아니요 | 도구 이름, 최대 128 bytes |
| `summary` | 아니요 | 활동 설명, 최대 360 bytes |
| `source` | 아니요 | 시작/재개 원인, 최대 32 bytes |
| `host_window` | 아니요 | 세션이 열린 최상위 창의 핸들(양의 정수). 없으면 생략. 숫자가 아니면 무시한다 |
| `host_process_id` | 아니요 | `host_window`를 가진 프로세스 ID. 창 핸들이 재사용돼도 다른 창으로 옮기지 않도록 함께 저장한다 |
| `child_id` | 아니요 | 있으면 이 이벤트는 부모가 아닌 그 하위 에이전트·백그라운드 작업에 적용한다. 최대 512 bytes |
| `child_type` | 아니요 | `agent`(기본) 또는 `background_command` |
| `parent_tool_id` | 아니요 | 하위를 띄운 부모의 tool_use ID |
| `agent_type` | 아니요 | 하위 에이전트 종류(`Explore` 등), 최대 128 bytes |
| `background` | 아니요 | 불리언. 부모 turn이 끝나도 계속 도는 하위다. `background_command`는 늘 참으로 본다 |
| `outcome` | 아니요 | `child_finished`의 끝난 모양: `completed`·`failed`·`stopped` |
| `failure` | 아니요 | `failed`의 원인. Claude `StopFailure`의 `error` 값(`rate_limit`·`overloaded`·`max_output_tokens` 등)을 그대로 싣는다. 영문 소문자·숫자·`_`만 받고, 다른 꼴이거나 `failed`가 아닌 이벤트에 붙으면 버린다. 화면은 `오류 · 사용 한도 도달`처럼 말로 바꿔 보인다 |

`child_id`가 있으면 `title`은 하위의 짧은 설명(모델이 쓴 작업 이름), `summary`는 하위의 현재 동작이다. 하위에게 준 지시문은 싣지 않는다.

`host_window`·`host_process_id`는 "세션 창으로 이동" 단추를 위한 선택 필드다.
`session-board.exe --hook`은 이 값이 없으면 스스로 찾는다: 콘솔에 붙어 있으면 그 콘솔을 그리는 창(Windows Terminal은
의사 콘솔 창의 소유자)을, 아니면 부모 프로세스를 거슬러 올라가며 보이는 최상위 창을 쓴다. 한 프로세스에 창이 여럿이면
제목에 작업 폴더 이름이 든 창을 고른다. 찾지 못해도 이벤트는 그대로 저장한다.
이 값을 담지 않은 이벤트는 저장된 창 정보를 지우지 않는다. 로컬 기록 수집기의 병합도 Hook이 남긴 창 정보를 유지한다.
Windows Terminal의 탭이나 데스크톱 앱 안의 특정 대화까지는 고르지 못하고, 그 창을 앞으로 가져오는 데서 그친다.

## 상태 전이

| 이벤트 | 표시 상태 | 의미 |
| --- | --- | --- |
| `session_started` | 대기 중 / 기존 작업 유지 | 시작·재개·compact. 실행 중인 부모 상태를 초기화하지 않는다. 쉬던 세션은 열기 전의 응답 완료·중단·오류를 남기지 않고 대기 중이 된다 |
| `turn_started` | 작업 중 | 사용자 요청 제출. 새 turn에서 이전 도구 목록을 비운다 |
| `tool_started` | 작업 중 | 도구 실행 시작. tool ID를 병렬 집합에 추가 |
| `tool_finished` | 작업 중 | 도구 실행 끝. 해당 tool ID만 제거. 전체 작업 완료를 뜻하지 않는다 |
| `approval_requested` | 승인 대기 | 실제 승인 요청 발생. 모든 도구 호출을 승인 대기로 취급하지 않는다 |
| `input_requested` | 입력 대기 | 사용자가 답해야 하는 요청 |
| `turn_finished` | 응답 완료 | 이번 응답 종료. 요청 전체가 성공했다는 뜻은 아니다 |
| `interrupted` | 중단됨 | 명시적 사용자 중단 |
| `session_ended` | 세션 종료 | 명시적 세션 종료. 기록은 유지 |
| `failed` | 오류 | 세션/turn 전체의 실패가 확실할 때만 사용. `failure`가 있으면 저장하고, 오류가 아닌 상태로 바뀌면 지운다 |
| `child_started` | 하위 작업 중 | 하위 시작. `child_id` 필수. 같은 하위가 끝나 있었으면 다시 연다(재개된 에이전트) |
| `child_finished` | 하위 완료·실패·중지 | 하위 끝. `child_id` 필수. `outcome`이 없으면 완료 |

같은 session ID라도 provider가 다르면 별개다. 파일 이름은 ID 해시를 사용하며 원래 ID를 경로로 해석하지 않는다.
각 세션의 쓰기는 named mutex로 직렬화하고 임시 파일을 교체한다. 더 오래된 timestamp는 무시한다.
종료된 turn의 늦은 tool 이벤트와 현재 turn ID와 다른 종료 이벤트는 상태를 되돌리지 않는다.

### Hook 도착 순서

Hook은 이벤트마다 따로 뜨는 프로세스라 저장 순서가 발생 순서와 어긋날 수 있다. 다른 모니터들이 겪은 경합을 기준으로 다음 규칙을 지킨다.

- `timestamp_ms`는 Hook 프로세스가 입력을 읽은 시각이다. 늦게 저장된 이벤트라도 더 이른 시각이면 버린다. 같은 세션의 Hook이 서로를 앞지르는 경우를 시각으로 정리한다.
- Claude는 `PreToolUse`를 마친 뒤에 권한을 묻는다. 그래서 같은 도구의 `PreToolUse`가 `PermissionRequest`보다 늦게 오는 일은 없다고 본다. 승인 대기를 작업 중으로 되돌리는 것은 그 뒤의 `PostToolUse`나 새 `PreToolUse`뿐이다.
- `Stop`·`StopFailure`·`Interrupt`·`SessionEnd`로 닫힌 turn에는 도구 이벤트를 받지 않는다. 백그라운드 명령의 `PostToolUse`가 `Stop`보다 늦게 오는 경합이 있어서다. 그 명령은 하위 행이 따로 맡는다.
- 사용자 요청 없이 시작되는 turn이 있다(작업 알림 뒤의 이어 하기, `Stop` Hook이 막은 뒤). 이때는 `UserPromptSubmit`이 오지 않아 Hook만으로는 닫힌 turn의 `PreToolUse`를 버리게 된다. 로컬 기록 수집기가 기록의 `tool_use`로 turn을 다시 열어 이 공백을 메운다. Hook만 쓰는 실행에서는 다음 `UserPromptSubmit`까지 마지막 상태가 남는다.
- Claude `Notification`의 `idle_prompt`는 응답 완료 약 60초 뒤의 타이머이고 `permission_prompt`는 `PermissionRequest`와 겹치며 늦게 오거나 오지 않기도 한다. 둘 다 상태 전이에 쓰지 않는다.

### 하위 에이전트·백그라운드 작업

- 하위는 부모 스냅숏의 `children`에 `(child_type, child_id)`로 하나씩 둔다. 부모 하나에 최대 24개이며, 넘치면 가장 오래전에 끝난 하위부터 버린다.
- `child_id`가 붙은 이벤트는 `child_started`·`child_finished`·`tool_started`·`tool_finished`·`approval_requested`·`input_requested`만 허용한다. 그 밖은 거부한다.
- 하위 이벤트는 부모의 상태·시각·turn을 바꾸지 않는다. 시각 비교는 그 하위의 마지막 시각과 한다. 백그라운드 하위는 부모 turn과 엇갈려 오기 때문이다.
- 끝난 하위의 늦은 도구 이벤트는 되살리지 않는다. `child_finished`는 끝을 확인하지 못한 하위의 상태를 고친다.
- 부모 `turn_finished`·`failed`: 도는 포그라운드 하위를 `확인 불가`로 닫는다. `interrupted`: `중지됨`으로 닫는다. 백그라운드 하위는 그대로 둔다.
- 부모 `session_ended`: 백그라운드를 포함한 모든 도는 하위를 `확인 불가`로 닫는다.
- 하위 상태 값은 부모와 같은 이름을 쓴다: `idle`=완료, `interrupted`=중지됨, `ended`=확인 불가, `failed`=실패.
- 서브에이전트 Hook(`agent_id`가 있는 Hook)은 아직 하위 이벤트로 바꾸지 않고 버린다. 하위는 로컬 기록에서 모은다([Claude 연동](claude-integration.md)).

## 표시와 한계

- 앱은 디렉터리 변경 신호를 최대 1초 간격으로 확인한다. 변경이 있을 때 스냅숏을 다시 읽고 최신 512개를 표시한다.
- 투명 목록은 진행 중인 세션을 우선 표시한다. 작성 잠금이 실제 보유 중인 Codex 세션은 응답 완료 후에도 유지하고, 닫힌 로컬 세션의 최근 활동은 15분간 유지한다. Hook 기록 파일은 지우지 않는다.
- 카드 본문은 `summary`의 작업 설명이다. 로컬 Codex 경로는 공개 assistant 진행 메시지 또는 마지막 답변의 첫 문장을 최대 240 bytes로 표시한다. 설명이 없으면 대화 이름·수신 대기를 표시한다. `title`의 사용자 요청 발췌문은 카드의 세부 정보 팝업·펼친 상세와 접힌 위젯의 `작업 끝남` 알림에서만 사용한다.
- 로컬 수집은 `session_meta`, `turn_context`, 공개 assistant 메시지, 명시적 turn 시작·종료·중단, 도구 시작·종료·승인·입력 대기를 해석한다. 비공개 analysis/reasoning과 도구 출력은 작업 설명으로 사용하지 않는다.
- 로컬 기록은 원본에 그대로 두고 UI용 짧은 상태만 메모리에 유지한다. `name`과 `activity`는 스냅숏의 선택적 표시 필드이며, 이전 v1 스냅숏도 읽는다. `runtime_loaded`와 수집 출처는 관측 시점의 메모리 정보다.
- 작성 잠금은 파일의 존재가 아닌 실제 보유 여부로 판별한다. 새 파일은 최대 5초 간격으로 발견하고 기존 기록의 추가분은 약 1초 간격으로 읽는다. 잘린 마지막 JSON 줄은 다음 쓰기를 기다리고, 큰 레코드·손상 파일·하위 에이전트는 루트 상태와 분리한다.
- Codex 내부 로컬 기록 형식은 버전에 따라 달라질 수 있다. 해석할 수 없는 기록은 수집 오류·수신 대기로 드러내며 Hook 수집을 계속한다. 로컬 기록에 남지 않는 승인·입력 대기는 신뢰 처리된 Hook 이벤트가 보완한다.
- 10분 이상 이벤트가 없는 실행/승인/입력 대기 세션은 `상태 확인 필요`로 표시하고 원래 상태를 보존한다.
  - 예외: 로컬 수집기가 열린 세션(`runtime_loaded`)으로 확인했고, 작업 중이며, 결과가 아직 없는 도구가 있으면 긴 명령으로 본다. `작업 중 · 12분째`처럼 마지막 소식 뒤의 시간을 보인다. 승인·입력 대기는 이 예외를 받지 않는다.
  - 이번 실행에서 열린 것을 본 세션이 닫혔는데(Claude 등록 정보가 사라짐, Codex 작성 잠금·Antigravity `presence` 잠금이 풀림) 마지막 상태가 작업 중이면 `세션 종료`로 확정한다. 쓰는 도중의 등록 파일을 닫힘으로 읽지 않게 2초 동안 닫혀 있을 때만 확정하고, 닫힌 것을 처음 본 시각을 마지막 소식으로 쓴다. 열린 것을 본 적이 없는 세션은 건드리지 않는다.
- 하위는 부모 카드 안에 들여 쓴 한 줄씩 보인다. 접힌 카드는 도는 하위·확인할 하위·끝난 지 2분 안의 하위만 보이고 나머지는 `끝난 하위 N개 더`로 줄인다. 상세 목록은 부모 행 바로 아래에 모두 보인다.
- 묶음은 부모와 하위를 합쳐 정한다. 하위가 승인·입력 대기이거나 15분 안에 실패했으면 부모 카드가 `확인 필요`로 올라가고, 하위가 돌면 부모 응답이 끝났어도 진행 중 묶음에 남는다. 부모 자신의 상태 글은 바꾸지 않고 `응답 완료 · 백그라운드 N개 진행 중`처럼 덧붙인다.
- 10분 무소식 규칙은 하위 에이전트에만 적용한다. 백그라운드 명령은 끝날 때까지 소식이 없는 것이 정상이므로 경과 시간(`3분째`)을 보인다.
- 끝난 하위는 15분이 지나면 카드에서 뺀다. 카드의 나이와 위젯 유지 기간은 부모와 하위 중 가장 최근 소식을 따른다.
- 긴 명령 실행, 프로세스 강제 종료, Hook 누락을 이 정보만으로 확정적으로 구별할 수 없다.
- Hooks만 사용하면 설치 전에 시작한 세션은 이후 이벤트가 있어야 나타난다. 기본 Codex 로컬 수집기는 이미 열린 세션과 최근 48시간 내 기록 중 최대 64개를 자동 탐색한다. 전체 과거 대화를 가져오지는 않는다.
- 정확한 완료율·남은 시간·모든 종류의 사용자 대기를 제공하지 않는다.
- 잘못된 파일은 건너뛰고 UI에 개수를 표시한다. 다른 정상 세션은 계속 보인다.
- 저장 스냅숏은 내부 구현이다. 어댑터는 파일을 직접 쓰지 말고 `session-board.exe --hook --normalized`에 정규화 이벤트를 보낸다.

Codex 원본 입력은 `--provider codex`로 전달한다. 설정 위치와 신뢰 절차는 [공식 Hooks 문서](https://learn.chatgpt.com/docs/hooks)를 따른다.
