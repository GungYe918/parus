# Parse: struct and acts Declarations

## 목적

`struct`, `acts`, `acts for` 선언 파싱 규칙과 self 문법을 명시한다.

## 현재 구현 (코드 근거)

1. struct/acts 파싱: `frontend/src/parse/decl/parse_decl_data.cpp`
2. 함수/receiver 파싱: `frontend/src/parse/decl/parse_decl_fn.cpp`
3. AST 구조: `frontend/include/parus/ast/Nodes.hpp`

## `struct` 선언

1. 형태: `struct layout(c)? align(n)? Name { member: Type; ... }`
2. `layout(c)`만 허용
3. `align(n)`은 정수 literal 파싱
4. `mut` struct member 선언은 파서에서 즉시 진단
5. legacy `Type name;` member도 recovery 차원에서 수용

## `acts` 선언

1. 일반 acts: `acts Name { ... }`
2. default acts: `acts for T { ... }`
3. named acts: `acts Name for T { ... }`
4. member는 `def` 선언만 허용
5. `operator(...)`는 `acts for` 계열에서만 파싱 허용

## self receiver 문법

1. 첫 파라미터에서만 허용
2. 허용 표기
   - `self` (`&Self`)
   - `self mut` (`&mut Self`)
   - `self move` (`Self`)
3. legacy `self a: T`는 파서 진단 후 소비

## EBNF (구현 반영)

```ebnf
FieldDecl      := "struct" FieldQual* Ident "{" FieldMember* "}" [";"] ;
FieldQual      := "layout" "(" "c" ")"
               | "align" "(" IntLit ")" ;
FieldMember    := Ident ":" Type ";" ;

ActsDecl       := ("export")? "acts" ActsHead "{" ActsMember* "}" [";"] ;
ActsHead       := "for" Type
               | Ident
               | Ident "for" Type ;
ActsMember     := FnDecl
               | OperatorDecl
               | EmptyItem ;

SelfRecv       := "self" ["mut" | "move"] ;
```

## 진단/오류 복구

1. acts block에서 비함수 token은 `UnexpectedToken` 후 `;`/`}`까지 recovery
2. operator 선언 파싱 실패 시 block 경계까지 skip
3. struct qualifier 중복/잘못된 인자는 즉시 진단

## 제약/비범위 (v0)

1. struct/proto/class 완전 통합 문법은 진행 중
2. struct member default value 문법 없음
3. acts inheritance/polymorphism 없음

## 미래 설계 (v1+)

1. struct member attribute 확장
2. acts declaration metadata 분리(AST slim)
