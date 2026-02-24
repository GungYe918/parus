# 02. Syntax EBNF

## 개요

LEI Core Syntax는 세미콜론 기반 문법을 사용한다. 모든 import 예시는 상대 경로(`./`, `../`)를 사용한다.

## Legacy 제거

다음 형식은 제거되었다.

1. 구형 빌드 엔트리 선언
2. 구형 화살표 본문 함수 선언
3. 구형 기본값 오버레이 연산
4. 구형 intrinsic import 선언

## EBNF

```ebnf
Program          := { Item } EOF ;

Item             := ImportStmt
                 | ProtoDecl
                 | PlanDecl
                 | ExportPlanDecl
                 | ExportPlanRef
                 | LetDecl
                 | VarDecl
                 | DefDecl
                 | AssertStmt
                 | ";"
                 ;

ImportStmt       := "import" Ident "from" StringLit ";" ;

ProtoDecl        := "proto" Ident "{" { ProtoField } "}" ";" ;
ProtoField       := Ident ":" ProtoType [ "=" Expr ] ";" ;
ProtoType        := ScalarType | "[" ProtoType "]" ;

PlanDecl         := "plan" Ident PlanBody ";"
                 | "plan" Ident "=" PlanExpr ";"
                 ;

ExportPlanDecl   := "export" "plan" Ident PlanBody ";"
                 | "export" "plan" Ident "=" PlanExpr ";"
                 ;

ExportPlanRef    := "export" "plan" Ident ";" ;

PlanExpr         := Expr ;

PlanBody         := "{" { PlanAssign } "}" ;
PlanAssign       := Path "=" Expr ";" ;
Path             := Ident { "." Ident | "[" Expr "]" } ;

LetDecl          := "let" Ident [ ":" Type ] "=" Expr ";" ;
VarDecl          := "var" Ident [ ":" Type ] "=" Expr ";" ;

DefDecl          := "def" Ident "(" [ParamList] ")" [ "->" Type ] Block ;
ParamList        := Param { "," Param } [","] ;
Param            := Ident [ ":" Type ] ;

Type             := ScalarType | "[" Type "]" ;
ScalarType       := "int" | "float" | "string" | "bool" ;

Block            := "{" { Stmt } "}" ;

Stmt             := LetDecl
                 | VarDecl
                 | AssignStmt
                 | ForStmt
                 | IfStmt
                 | ReturnStmt
                 | AssertStmt
                 | ExprStmt
                 | ";"
                 ;

AssignStmt       := Path "=" Expr ";" ;

ForStmt          := "for" Ident "in" Expr Block ;
IfStmt           := "if" Expr Block [ "else" Block ] ;
ReturnStmt       := "return" Expr ";" ;
AssertStmt       := "assert" Expr ";" ;
ExprStmt         := Expr ";" ;

Expr             := MergeExpr ;
MergeExpr        := OrExpr { "&" OrExpr } ;
OrExpr           := AndExpr { "||" AndExpr } ;
AndExpr          := EqExpr { "&&" EqExpr } ;
EqExpr           := AddExpr { ("==" | "!=") AddExpr } ;
AddExpr          := MulExpr { ("+" | "-") MulExpr } ;
MulExpr          := UnaryExpr { ("*" | "/") UnaryExpr } ;
UnaryExpr        := ("-" | "!") UnaryExpr | PostfixExpr ;

PostfixExpr      := Primary { MemberSuffix | IndexSuffix | CallSuffix } ;
MemberSuffix     := "." Ident ;
IndexSuffix      := "[" Expr "]" ;
CallSuffix       := "(" [ArgList] ")" ;

ArgList          := Expr { "," Expr } [","] ;

Primary          := NamespaceRef
                 | Ident
                 | Literal
                 | ObjectLit
                 | ArrayLit
                 | PlanPatchLit
                 | "(" Expr ")"
                 ;

NamespaceRef     := Ident "::" Ident { "::" Ident } ;

ObjectLit        := "{" [ ObjItem { "," ObjItem } [","] ] "}" ;
ObjItem          := (Ident | StringLit) ":" Expr ;

ArrayLit         := "[" [ Expr { "," Expr } [","] ] "]" ;
PlanPatchLit     := "{" { PlanAssign } "}" ;

Literal          := IntLit | FloatLit | StringLit | BoolLit ;

Ident            := Letter { Letter | Digit | "_" } ;
IntLit           := Digit { Digit | "_" } ;
FloatLit         := Digit { Digit | "_" } "." Digit { Digit | "_" } ;
StringLit        := '"' { Char | Escape } '"' ;
BoolLit          := "true" | "false" ;
```

## 표면 규칙

1. `proto`는 사용자 템플릿/스키마 선언이다.
2. 권장 선언 스타일은 `export plan foo = MyProto & { ... };` 또는 `export plan foo = bundle & { ... };`다.
3. `plan foo { ... }; export plan foo;`도 허용한다.
4. import된 심볼 접근은 `alias::symbol`만 허용한다.
5. 객체 접근은 `.`만 사용한다.
6. 배열 접근은 `[]`만 사용한다.

## 파서 판정 규칙

1. `{ ... }`에서 `path = expr;` 형태가 나오면 `PlanPatchLit`으로 판정한다.
2. `{ key: expr, ... }` 형태면 `ObjectLit`으로 판정한다.
3. 구현 기준에서 빈 `{}`는 patch 문맥으로 판정되므로, 객체 리터럴은 명시적으로 키-값을 포함하는 형태를 권장한다.

## 주석

1. `bundle`, `module`, `master`, `task`, `codegen`은 문법 키워드가 아니다.
2. 빌드 시스템 profile(예: parus)이 Build API를 통해 주입하는 일반 식별자다.
