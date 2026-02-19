# Parse: use/import/nest

## 목적

`use`, `import`, `nest` 파싱과 alias 도입 정책을 정리한다.

## 현재 구현 (코드 근거)

1. `use`/`import`: `frontend/src/parse/stmt/parse_stmt_use.cpp`
2. `nest`: `frontend/src/parse/decl/parse_decl_nest.cpp`
3. path segment parser: `Parser::parse_path_segments()`

## `import` 규칙

1. `import foo;`, `import foo::bar as fb;`
2. alias 미지정 시 마지막 segment를 alias로 사용
3. AST는 `StmtKind::kUse`, `UseKind::kImport`로 저장

## `use` 규칙

1. path alias: `use Foo::Bar as B;`
2. type alias: `use NewT = OldType;`
3. text substitution: `use PI 3.14;` (literal-only)
4. acts selection: `use T with acts(NameOrDefault);`
5. acts namespace alias: `use acts(Foo::Bar) as fb;`
6. legacy `use acts A for T;`는 파서에서 폐기 진단

## `nest` 규칙

1. file directive: `nest a::b;` (파일당 1회)
2. block form: `nest a::b { ... }`
3. `export nest ...`는 파서에서 허용

## EBNF (구현 반영)

```ebnf
ImportStmt     := "import" Path ("as" Ident)? ";" ;
UseStmt        := "use" UseBody ";" ;
UseBody        := "acts" "(" Path ")" ("as" | "=") Ident
               | Path ("as" | "=") Ident
               | Ident Literal
               | Path "with" "acts" "(" ("default" | Path) ")" ;
NestDecl       := ("export")? "nest" Path (";" | Block) ;
Path           := Ident ("::" Ident)* ;
```

## 진단/오류 복구

1. `use`에서 핵심 토큰 누락 시 `recover_to_delim()` 사용
2. `import`는 block scope에서도 파싱되지만 tyck에서 file-scope 강제
3. `nest` file directive 중복은 parser에서 즉시 진단

## 제약/비범위 (v0)

1. `import`는 include semantics가 아님
2. `use` text substitution은 literal만 허용
3. `with`는 keyword 토큰이 아니라 `Ident("with")`로 인식

## 미래 설계 (v1+)

1. `with`를 reserved keyword로 승격
2. use/import alias resolver 공유 테이블을 parser 단계부터 분리
