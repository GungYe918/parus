# Incremental Parsing (Parser + parusd v1)

상태: `Implemented`  
성격: `internal 구현 문서`

이 문서는 Parus frontend의 1차 증분 파싱 구현 결과를 설명한다.  
언어 의미 정본은 `docs/reference/language/*`, ABI 정본은 `docs/reference/abi/*`가 우선이며, 이 문서는 구현 API와 파이프라인 구조 변경만 다룬다.

## 1. 목적

1. `parusd`의 `didChange` 편집 루프에서 parse 비용을 줄인다.
2. 정답성 우선 원칙으로 증분 실패 시 자동 full parse fallback을 수행한다.
3. 의미분석(`passes/tyck/cap`)은 기존 full-run 경로를 유지한다.

## 2. 구현 범위

1. **포함**: full lex + top-level item 단위 증분 parse + AST root splice.
2. **포함**: `parusd::DocumentState`에 parse session 캐시 연동.
3. **포함**: 공통 AST visitor 도입(`visit_stmt_tree`, `visit_expr_tree`)과 일부 pass 이관.
4. **제외**: incremental tyck/cap, incremental OIR, incremental lex.

## 3. 변경된 코드 경로

1. 신규 API 헤더: `frontend/include/parus/parse/IncrementalParse.hpp`
2. 신규 구현: `frontend/src/parse/common/incremental_parse.cpp`
3. parusd 연동: `tools/parusd/src/main.cpp`
4. 공통 visitor: `frontend/include/parus/ast/Visitor.hpp`
5. visitor 적용 pass:
   - `frontend/src/passes/check_pipe_hole.cpp`
   - `frontend/src/passes/check_place_expr.cpp`

## 4. 핵심 데이터 모델

| 타입 | 역할 | 핵심 필드 |
|---|---|---|
| `EditWindow` | 변경 구간(이전 텍스트 기준) | `lo`, `hi` |
| `TopItemMeta` | top-level child 메타 | `sid`, `lo`, `hi` |
| `ParseSnapshot` | 파싱 결과 스냅샷 | `ast`, `types`, `root`, `tokens`, `top_items`, `revision` |
| `IncrementalParserSession` | 증분 파싱 세션 | `initialize`, `reparse_with_edits`, `snapshot` |
| `ReparseMode` | parse 방식 추적 | `kFullRebuild`, `kIncrementalMerge`, `kFallbackFullRebuild` |

## 5. API 계약

```cpp
struct EditWindow {
    uint32_t lo = 0;
    uint32_t hi = 0;
};

struct TopItemMeta {
    ast::StmtId sid = ast::k_invalid_stmt;
    uint32_t lo = 0;
    uint32_t hi = 0;
};
```

```cpp
struct ParseSnapshot {
    ast::AstArena ast{};
    ty::TypePool types{};
    ast::StmtId root = ast::k_invalid_stmt;
    std::vector<Token> tokens{};
    std::vector<TopItemMeta> top_items{};
    uint64_t revision = 0;
};
```

```cpp
class IncrementalParserSession {
public:
    bool initialize(std::string_view source, uint32_t file_id, diag::Bag& bag);
    bool reparse_with_edits(std::string_view source,
                            uint32_t file_id,
                            std::span<const EditWindow> edits,
                            diag::Bag& bag);

    const ParseSnapshot& snapshot() const;
    ParseSnapshot& mutable_snapshot();
    bool ready() const;
    ReparseMode last_mode() const;
};
```

규칙:

1. `initialize`는 항상 full parse를 수행한다.
2. `reparse_with_edits`는 증분 merge 시도 후 실패하면 즉시 full rebuild로 복귀한다.
3. 증분 실패는 사용자 에러가 아니라 정상 fallback 경로다.

## 6. 증분 알고리즘 (현재 구현)

현재 구현은 “**영향 시작 item부터 EOF까지 재파싱**” 전략을 사용한다.

1. 입력 edit 목록에서 최소 `lo`를 계산한다.
2. 기존 `top_items`에서 첫 영향 item 인덱스 `first`를 찾는다.
3. `first == 0`이거나 불변식이 깨지면 곧바로 full fallback한다.
4. `first > 0`이면:
   - prefix(`0..first-1`) child는 기존 AST에서 재사용
   - suffix(`first..end`)는 새 token 슬라이스를 Parser로 재파싱
   - 새 root block child를 splice하여 snapshot 갱신
5. 성공 시 `ReparseMode::kIncrementalMerge`, 실패 시 `ReparseMode::kFallbackFullRebuild`.

```cpp
// simplified flow
if (!try_incremental_merge_(...)) {
    return full_rebuild_(..., ReparseMode::kFallbackFullRebuild);
}
```

## 7. parusd 연동 구조

`DocumentState`는 parse session을 보유한다.

```cpp
struct DocumentState {
    std::string text{};
    int64_t version = 0;
    uint64_t revision = 0;
    std::vector<parus::parse::EditWindow> pending_edits{};

    parus::parse::IncrementalParserSession parse_session{};
    bool parse_ready = false;
    AnalysisCache analysis{};
};
```

흐름:

1. `didOpen`: `initialize(...)`
2. `didChange`: `pending_edits` 누적 후 `reparse_with_edits(...)`
3. parse 후에도 `passes/tyck/cap`은 full-run 유지
4. 진단/semantic token은 최신 snapshot 기준으로 생성

디버그:

1. `PARUSD_TRACE_INCREMENTAL=1` 설정 시 `stderr`에 parse mode를 출력한다.

## 8. Visitor 계층 도입 상태

`frontend/include/parus/ast/Visitor.hpp`에 공통 walker를 도입했다.

```cpp
class TreeVisitor {
public:
    virtual void enter_stmt(StmtId, const Stmt&) {}
    virtual void leave_stmt(StmtId, const Stmt&) {}
    virtual void enter_expr(ExprId, const Expr&) {}
    virtual void leave_expr(ExprId, const Expr&) {}
};

void visit_stmt_tree(const AstArena& ast, StmtId root, TreeVisitor& visitor);
void visit_expr_tree(const AstArena& ast, ExprId root, TreeVisitor& visitor);
```

적용:

1. `check_pipe_hole`
2. `check_place_expr`

## 9. 수명/메모리 정책

1. 1차 구현은 `std::string_view` 수명 보장을 위해 parse session 내부에 source owner를 보관한다.
2. owner 누적이 커지면(`>16`) 증분을 중단하고 full rebuild로 전환해 세션을 압축한다.
3. STL 전면 제거는 하지 않는다. 현재 단계는 알고리즘 정합성과 fallback 안정성이 우선이다.

## 10. 한계와 다음 단계

1. 현재는 full lex 고정이다.
2. `first == 0` 편집은 full rebuild로 처리된다.
3. 의미분석 증분화는 아직 미구현이다.
4. v1+에서 검토:
   - incremental lex
   - name resolve / tyck invalidation 그래프
   - source owner 압축 및 interner 전략
