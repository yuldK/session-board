# Claude Code 세션 자동 수집 구현 요청

작업 디렉터리는 이 저장소의 루트입니다. 아래 요청에 따라 조사에서 끝내지 말고 구현·빌드·연동 설정·실제 화면 검증까지 진행하세요.
커밋과 push는 요청하지 않습니다. 기존 미커밋 변경을 보존하고, 사용자가 명시적으로 요청하기 전에는 push하지 마세요.

## 원하는 결과

Session Board는 C++20/luil로 만든 Windows 앱입니다. 현재 사용 중인 Codex 대화는 이미 자동으로 발견하고 공개 진행 메시지와 상태를 표시합니다.
Claude Code도 같은 수준으로 연결하세요.

1. Session Board를 켜면 이 PC에서 이미 사용 중인 Claude Code 세션이 자동으로 나타나야 합니다. 다음 `SessionStart`나 새 검증용 세션이 생길 때까지 기다리는 구현으로 끝내지 마세요.
2. **이 프롬프트를 읽고 작업하는 Claude 대화 자체**가 수집되는지 원래 세션 ID와 UI를 대조하세요.
3. 카드 본문에는 현재 작업에 대한 공개 진행 메시지나 최근 답변의 짧은 설명을, 그 아래에는 실제 도구 동작과 상태를 표시하세요. 사용자 요청문의 앞부분을 작업 설명으로 대신 사용하지 마세요.
4. 진행 메시지·도구 동작·상태가 바뀌면 같은 카드가 갱신되어야 합니다. 열린 세션은 응답 완료 후에도 유지하고, 종료한 세션은 최근 기록으로 15분간 남기세요.
5. Codex와 Claude를 한 화면에서 함께 볼 수 있어야 합니다. 현재의 투명 목록, 캡션 이동, 접기·펼치기, 고정과 상세 화면 복귀는 보존하세요.

## 먼저 읽을 코드와 규칙

- `AGENTS.md`, `README.md`, `docs/event-protocol.md`
- `src/core/session.h`, `src/core/session.cpp`, `src/core/store.h`, `src/core/store.cpp`
- `src/core/codex_monitor.h`, `src/core/codex_monitor.cpp`
- `src/hook/main.cpp`, `src/app/board_app.h`, `src/app/board_app.cpp`, `src/app/main.cpp`
- `scripts/install-codex-hooks.ps1`, `scripts/verify-current-session.ps1`, `tests/session_tests.cpp`, `tests/ui-smoke.ps1`

Codex는 기존 로컬 기록의 추가분과 실제 작성 잠금, 공개 진행 메시지를 읽고 동일 ID의 Hook 기록과 메모리에서 병합합니다.
이 구조를 참고하되 Claude에도 같은 내부 기록 형식이나 잠금 방식이 있다고 가정하지 마세요.

C++20, luil, Allman 중괄호, snake_case, 한글 주석, UTF-8 무 BOM, CRLF를 지키세요. 사용 가능한 luil UI·UIA·코드 스타일 스킬도 적용하세요.

## 실제 설치 환경과 지원 기능부터 확인

- 설치된 Claude Code 버전, 실행 형태(CLI·IDE·로컬 데스크톱), 설정 루트와 실제 transcript 위치를 확인하세요. `CLAUDE_CONFIG_DIR`를 존중하고 다른 사용자의 경로를 하드코딩하지 마세요.
- 최신 공식 문서와 설치 버전의 실제 payload를 함께 확인하세요. 공식 문서에만 있고 설치 버전에 없는 기능을 전제로 구현하지 마세요.
- `SessionStart`, `UserPromptSubmit`, `PreToolUse`, `PostToolUse`, `PostToolUseFailure`, `PermissionRequest`, `Notification`, `Stop`, `SessionEnd` 등을 확인하고 지원되는 이벤트만 등록하세요.
- 공개 메시지 수집에는 설치 버전의 `MessageDisplay`, Hook의 `transcript_path`, `last_assistant_message`, 로컬 transcript 관측 중 적절한 경로를 선택하세요. 스트리밍 delta를 사용하면 message ID·순번·중복·부분 메시지를 처리하세요.
- `idle_prompt` 같은 알림을 모든 사용자 대기의 확정 신호로 취급하지 마세요. 실제 발생 조건, 승인 대기와 단순 응답 완료를 구별하세요.

공식 참고 문서:

- https://code.claude.com/docs/en/hooks
- https://code.claude.com/docs/en/claude-directory
- https://code.claude.com/docs/en/settings
- https://code.claude.com/docs/en/sessions

## 기존 세션 자동 발견과 로컬 관측

- C++ 백그라운드 수집기로 이미 존재하는 Claude 세션을 자동 발견하세요. Hook은 lifecycle·승인·입력 대기를 보완하고, 로컬 기록은 기존 세션 발견과 작업 설명을 보완하도록 구성하세요.
- 기본 후보인 설정 루트의 `projects` 아래 transcript와 실제 Hook 경로를 설치 환경에서 확인하세요. 디렉터리 이름을 임의로 디코딩해 cwd나 세션 ID를 만들어내지 말고 기록의 원래 식별자를 사용하세요.
- 파일이 존재하거나 최근 수정됐다는 사실만으로 '열린 세션'을 확정하지 마세요. 실제 버전이 제공하는 세션 메타데이터·프로세스와 세션의 연결·PID 생성 시각·등록 정보 등 검증 가능한 근거를 찾으세요. Codex의 잠금 방식을 그대로 가정하지 마세요.
- 실행 중임을 확인한 세션, 최근 기록, 상태 확인이 필요한 기록을 구별하세요. 근거가 없으면 '상태 확인 필요'나 최근 기록으로 표시하고 열린 세션이라고 단정하지 마세요.
- root 세션과 하위 에이전트를 구별하세요. 서브에이전트 기록이나 완료 이벤트가 부모 상태를 변경하면 안 됩니다.
- 매번 transcript 전체를 다시 읽지 말고 추가된 부분만 읽으세요. 초기 탐색 범위와 메모리 상한을 두고 큰 입력·부분 JSON 줄·손상·파일 교체·잘림·재개·compact·동시 쓰기에 대응하세요. 읽는 동안에는 마지막 정상 상태를 유지하세요.
- Claude 원본 기록과 실행 정보를 수정·삭제하지 마세요. 추가 Python/Node 런타임, 서버, 별도 AI API 호출 없이 구현하는 것을 기본으로 하세요.

## 작업 내용과 상태의 의미

- `summary`: Claude가 사용자에게 공개한 최근 작업 설명 또는 마지막 답변의 짧은 첫 문장. 원래 텍스트를 짧게 추려 표시하고 임의의 작업·성공 판정을 생성하지 마세요.
- `activity`: `파일 읽는 중 · Read`, `코드 수정 중 · Edit`, `명령 실행 중 · Bash`, `실행 승인 대기` 등 실제 관측한 도구·동작. 명령문·출력 전체는 넣지 마세요.
- 설명이 아직 없으면 대화 이름 또는 '작업 내용 수신 대기'를 표시하세요. `title`의 사용자 요청 발췌문은 펼친 상세에서만 사용할 수 있습니다.
- 비공개 thinking/reasoning, 시스템 프롬프트, tool result와 자격 증명은 작업 설명으로 사용하거나 별도 저장하지 마세요. 인증 파일을 수집 경로로 사용하지 마세요.
- 새 turn에는 이전 작업 설명과 도구 목록을 적절히 초기화하세요. 공개 메시지 갱신만으로 turn을 다시 시작하거나 승인 대기를 해제하면 안 됩니다.
- `Stop`은 응답 종료입니다. 세션 종료·요청 전체 성공으로 단정하지 마세요. 도구 하나의 실패도 세션 전체 실패와 구별하세요. 백그라운드 작업이 있으면 응답 종료와 별도로 그 상태를 표현하세요.
- 원래 세션 ID, tool-use ID, 실제로 제공되는 turn ID를 보존하세요. 없는 모델·turn ID·완료율·남은 시간은 만들지 마세요.
- `docs/event-protocol.md`를 공통 계약으로 유지하세요. 공개 설명 갱신을 기존 `turn_started`/`tool_started`로 가장하지 마세요. 중립적인 갱신이 필요하면 로컬 관측 경로 또는 이전 어댑터와 호환되는 최소 확장으로 처리하고 문서화하세요.
- Hook과 로컬 관측은 `(provider, session_id)`로 한 카드에 병합하세요. 서로 다른 turn의 늦은 종료·도구 이벤트가 새 작업 상태를 덮어쓰지 않게 하세요.

## 구현과 설정 설치

- 기존 native helper에 `session-board-hook.exe --provider claude`를 추가하세요. `--provider codex`, `--normalized`, `--strict`, `--data-dir` 동작을 보존하세요.
- Claude 전용 parser·monitor 파일을 추가하고 앱의 시작·갱신·종료에 연결하세요. provider별 설정과 수집기만 분리하고 필요한 작은 공통화는 허용하되 Codex 구현과 UI를 대규모로 다시 작성하지 마세요.
- 앱의 수집 출처 문구·하단 수신 상태·초기 탐색·오류 표시가 현재 Codex 전용으로 되어 있는 부분도 Claude에 맞게 연결하세요. `runtime_loaded`, `observation_source`, `name`, `summary`, `activity`의 현재 의미를 확인하고 사용하세요.
- `--claude-home`, `--no-claude-discovery`처럼 테스트와 사용자 선택에 필요한 옵션을 추가하세요. `--data-dir`를 쓰는 테스트는 실제 Claude 홈을 자동으로 읽지 않도록 기존 Codex의 격리 규칙과 일관되게 만드세요.
- `scripts/install-claude-hooks.ps1`에 Preview·Remove·설정 파일/설치 경로 지정·백업·반복 실행 시 중복 방지를 구현하세요. 기존 permissions, MCP, 다른 Hooks와 알 수 없는 설정 키를 보존하세요.
- 실제 실행 파일은 기존 `%LOCALAPPDATA%/SessionBoard/bin/session-board-hook.exe`를 사용하세요. 공유 helper를 교체할 때 Codex 기능도 포함되어야 하며 기존 Codex Hook 명령과 신뢰 상태를 불필요하게 변경하지 마세요.
- 이 요청에는 필요한 Claude Hook 등록까지 포함합니다. 스크립트와 실제 적용될 Preview를 먼저 완성·검토한 뒤 백업하고 적용하세요. 샌드박스·권한·신뢰 검토를 우회하거나 자동 승인하지 마세요. 사람이 직접 해야 하는 제품의 검토 단계가 있으면 정확한 항목과 이유만 안내하세요.
- Hook 오류·파일 잠금·설정 누락·수집 지연은 원래 Claude 작업을 중단시키면 안 됩니다. 기본 실패 정책은 안전한 빈 응답과 종료 코드 0이며 `--strict` 실패만 테스트에서 사용하세요. 프롬프트·Stop을 막는 decision이나 추가 문맥은 반환하지 마세요.
- 이미 실행 중인 세션에 Hook 변경이 언제 적용되는지도 확인하세요. 강제 재시작으로 사용 중인 작업을 끊지 말고 기존 세션의 로컬 관측으로 보완하세요.

## 검증과 완료 조건

1. parser·상태·병합 테스트: 실제 형식 fixture, 동일 ID의 provider 분리, 병렬 도구, 새 turn, 늦은 이벤트, 승인·입력 대기, compact·재개, 부분 줄·손상 파일, 하위 에이전트 제외, 비공개 thinking 제외를 확인하세요.
2. 자동 발견 테스트: 앱을 켜기 전부터 존재한 세션, 새로 열린 세션, 이름/cwd, 작업 설명 변경, 응답 완료 후 유지와 종료 후 15분 보관을 확인하세요. 설치 후 새 Hook만 발생시키는 테스트로 대신하지 마세요.
3. 기존 Codex 테스트를 유지하고 아래 명령을 실행하세요. UI는 luil UIA로 확인하세요.

```powershell
./scripts/format.ps1
./scripts/format.ps1 -check
./scripts/check_source_style.ps1
cmake --build --preset vs2026-release --parallel
ctest --preset vs2026-release
./tests/ui-smoke.ps1
```

4. **지금 이 작업을 수행하는 실제 Claude 대화**의 native session ID를 확인하고 Session Board 카드의 ID·cwd·수집 출처와 대조하세요. 공개 진행 메시지를 바꿔 같은 카드의 본문이 자동으로 바뀌는 것을 확인하세요.
5. 앱에 현재 사용 중인 Codex 대화와 Claude 대화가 함께 표시되는지도 확인하세요. 사용자 세션을 임의로 종료하거나 다른 대화에 메시지를 보내지 마세요. 없는 상태를 만들기 위해 승인을 자동 처리하지 마세요.
6. 완료 증거를 `build/current-claude-session-verification.json`과 `build/current-claude-session.png`에 남기세요. JSON에는 관측 시각·원래 세션 ID·UI 카드 ID·수집 출처·표시 상태·짧은 공개 작업 설명을 담고 전체 대화나 자격 증명은 넣지 마세요.
7. 별도 검증 세션이나 데모 데이터가 보인 것을 실제 사용 세션 수집 성공으로 보고하지 마세요. fixture 검사와 실제 대화 검증을 구별하고, 재현하지 못한 승인·입력 대기 등은 명시하세요. 현재 대화를 검증하지 못했으면 완료라고 선언하지 말고 원인과 남은 작업을 구체적으로 보고하세요.
8. 마지막에는 실제 세션이 표시된 앱을 최소화하지 않고 남기세요. UIA 도우미가 포커스 정리 중 창을 최소화하는 경우도 확인하세요.

`docs/claude-integration.md`와 README·공통 규격에 실제 버전, 수집 경로, 지원 이벤트, 활성 판별 근거, 설정 설치·제거 방법과 한계를 기록하세요.
최종 답변은 변경 내용, 실제로 확인한 대화와 상태 변화, 테스트 결과, 직접 수행이 필요한 신뢰 단계만 간결하게 정리하고 실제 앱 캡처를 보여 주세요.
