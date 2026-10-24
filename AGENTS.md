# Git push 승인

- 사용자가 push를 명시적으로 요청하지 않으면 절대 push하지 않는다.
- 커밋 또는 rebase 요청을 push 승인으로 해석하지 않는다.
- "올려줘", "반영해줘" 같은 모호한 표현을 push 승인으로 해석하지 않는다.

# 구현

- C++20, luil, Allman 중괄호, snake_case, UTF-8 무 BOM, CRLF를 사용한다.
- `docs/event-protocol.md`를 Claude/Codex 연동의 공통 계약으로 유지한다.
- Hook 오류는 AI의 원래 작업을 막지 않아야 한다.
