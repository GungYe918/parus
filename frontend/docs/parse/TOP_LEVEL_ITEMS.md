# Parse: Top-level Items

## 목적

파일 스코프에서 허용되는 item과 금지되는 statement를 파서/패스 관점에서 정리한다.

## 현재 구현 (코드 근거)

1. decl 시작 판정: `frontend/src/parse/decl/parse_decl_entry.cpp`
2. top-level parse 루프: `frontend/src/parse/stmt/parse_stmt_core.cpp` (`parse_program`)
3. top-level decl-only 검사: `frontend/src/passes/check_top_level_decl_only.cpp`

## 규칙

1. parse 단계에서는 `parse_stmt_any()`로 decl/stmt를 모두 생성할 수 있다.
2. pass 단계에서 top-level이 declaration/use/import/nest 외 문장을 포함하면 에러(`kTopLevelDeclOnly`).
3. `;` 단독, `;;`는 `StmtKind::kEmpty`로 허용한다.

## EBNF (구현 반영)

```ebnf
Program        := TopItem* EOF ;
TopItem        := DeclAny | EmptyItem | InvalidStmtRecovered ;
EmptyItem      := ";" ;

DeclAny        := UseDecl
               | ImportDecl
               | NestDecl
               | FieldDecl
               | ActsDecl
               | FnDecl
               | ExternVarDecl ;
```

## 진단/오류 복구

1. 파서 루프는 토큰 전진이 없으면 `UnexpectedToken` 후 1토큰 강제 소비
2. block 파싱도 동일한 진행 보장 가드 사용
3. `stmt_consume_semicolon_or_recover`로 `;`, `}`, EOF 경계 복구

## 제약/비범위 (v0)

1. parse 단계에서 strict top-item만 허용하지 않고, pass에서 정책 강제
2. `kEmpty`는 경고 없이 no-op

## 미래 설계 (v1+)

1. parser 단계 top-level strict mode 옵션
2. top-level item grammar를 parser-generated table로 전환
