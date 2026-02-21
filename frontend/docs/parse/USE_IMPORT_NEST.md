# Parse: use/import/nest

## 목적

`use`, `import`, `nest` 파싱 규칙과 AST 저장 방식, 복구 규칙을 정리한다.

## 현재 구현 (코드 근거)

1. `use`/`import`: `/Users/gungye/workspace/Lang/gaupel/frontend/src/parse/stmt/parse_stmt_use.cpp`
2. `nest`: `/Users/gungye/workspace/Lang/gaupel/frontend/src/parse/decl/parse_decl_nest.cpp`
3. path parser: `Parser::parse_path_segments()`

## 구현 요약

1. `import`는 `UseKind::kImport`로 저장한다.
2. `use`는 body 형태에 따라 `kTypeAlias`, `kPathAlias`, `kTextSubst`, `kActsEnable`, `kNestAlias`로 저장한다.
3. `use nest`는 parser에서 문법만 확정하고, namespace 여부는 name-resolve/tyck 단계에서 검증한다.

## `import` 파싱 규칙

1. `import foo;`
2. `import foo::bar as fb;`
3. alias 생략 시 마지막 segment를 alias로 사용한다.

## `use` 파싱 규칙

1. 타입 alias: `use NewT = OldType;`
2. 경로 alias: `use foo::bar as fb;`, `use foo::bar = fb;`
3. namespace alias: `use nest game::math as gm;`, `use nest game::math;`
4. text substitution: `use PI 3.14;` (literal-only)
5. acts 선택: `use Vec2 with acts(A);`, `use Vec2 with acts(default);`
6. 일반 acts alias: `use acts(Foo::Bar) as fb;`

## `use nest` 상세

1. 문법: `use nest Path ("as" Ident)? ";"`.
2. `=`는 금지한다. 입력되면 `kUseNestAliasAsOnly`를 보고하고 복구한다.
3. alias 생략 시 마지막 path segment를 alias로 저장한다.
4. `use nest world::add as w;` 같은 non-namespace 경로는 semantic 단계에서 에러다.

## `nest` 파싱 규칙

1. file directive: `nest a::b;`
2. block form: `nest a::b { ... }`
3. `export nest ...`는 파서에서 허용한다.

## EBNF (구현 반영)

```ebnf
ImportStmt     := "import" Path ("as" Ident)? ";" ;
UseStmt        := "use" UseBody ";" ;
UseBody        := "acts" "(" Path ")" ("as" | "=") Ident
               | "nest" Path ("as" Ident)?
               | Path ("as" | "=") Ident
               | Ident Literal
               | Path "with" "acts" "(" ("default" | Path) ")" ;
NestDecl       := ("export")? "nest" Path (";" | Block) ;
Path           := Ident ("::" Ident)* ;
```

## 진단/복구 규칙

1. `use nest`에서 path 누락 시 `UnexpectedToken(identifier path segment)`.
2. `use nest`에서 `=` 사용 시 `UseNestAliasAsOnly`.
3. `use` 본문 파싱 실패 시 `recover_to_delim()`으로 `;`까지 동기화한다.
4. legacy `use acts A for T;`는 즉시 폐기 진단 후 stmt 경계로 복구한다.

## 제약/비범위 (v0)

1. `import`는 include semantics가 아니다.
2. `import` file-scope 강제는 parser가 아니라 tyck에서 수행한다.
3. `use nest`의 namespace 판정은 parser가 아니라 semantic 단계에서 수행한다.
4. `with`는 현재 `Ident("with")` 토큰으로 처리한다.
