# 02. Syntax EBNF

## 개요

LEI v0는 들여쓰기 의미를 사용하지 않는다. 문장은 `;`로 끝난다.

## Lexical

```ebnf
Ident     := Letter (Letter | Digit | "_")* ;
IntLit    := Digit (Digit | "_")* ;
FloatLit  := Digit (Digit | "_" )* "." Digit (Digit | "_")* ;
StringLit := '"' { Char | Escape } '"' ;
BoolLit   := "true" | "false" ;
Comment   := "//" ... EOL | "/*" ... "*/" ;
```

## Program

```ebnf
Program      := { Item } EOF ;

Item         := ImportStmt
             | LetStmt
             | ConstStmt
             | DefStmt
             | AssertStmt
             | ExportStmt
             | ";"
             ;

ImportStmt   := "import" ImportSpec ";" ;
ImportSpec   := "intrinsic" "{" IdentList "}"
             | "{" IdentList "}" "from" StringLit
             ;

LetStmt      := "let" Ident [ ":" PrimType ] "=" Expr ";" ;
ConstStmt    := "const" Ident [ ":" PrimType ] "=" Expr ";" ;
DefStmt      := [ "export" ] "def" Ident "(" [ParamList] ")" "=>" Expr ";" ;
AssertStmt   := "assert" Expr ";" ;
ExportStmt   := "export" "build" Expr ";" ;

PrimType     := "int" | "float" | "string" | "bool" ;

Expr         := IfExpr | MatchExpr | AssignExpr ;
IfExpr       := "if" Expr "then" Expr "else" Expr ;
MatchExpr    := "match" Expr "{" MatchArm { "," MatchArm } [","] "}" ;
MatchArm     := (Literal | "_") "=>" Expr ;

AssignExpr   := MergeExpr ["?=" MergeExpr] ;
MergeExpr    := OrExpr { "&" OrExpr } ;
OrExpr       := AndExpr { "||" AndExpr } ;
AndExpr      := EqExpr { "&&" EqExpr } ;
EqExpr       := AddExpr { ("==" | "!=") AddExpr } ;
AddExpr      := MulExpr { ("+" | "-") MulExpr } ;
MulExpr      := UnaryExpr { ("*" | "/") UnaryExpr } ;
UnaryExpr    := ("-" | "!") UnaryExpr | PostfixExpr ;

PostfixExpr  := Primary { CallSuffix | MemberSuffix } ;
CallSuffix   := "(" [ArgList] ")" ;
MemberSuffix := "." Ident ;

Primary      := Literal
             | Ident
             | ObjectLit
             | ArrayLit
             | "(" Expr ")"
             ;

ObjectLit    := "{" [ ObjItem { "," ObjItem } [","] ] "}" ;
ObjItem      := (Ident | StringLit) ":" Expr ;

ArrayLit     := "[" [ ArrItem { "," ArrItem } [","] ] "]" ;
ArrItem      := Expr | "..." Expr ;

Literal      := IntLit | FloatLit | StringLit | BoolLit ;

IdentList    := Ident { "," Ident } [","] ;
ParamList    := Ident { "," Ident } [","] ;
ArgList      := Expr { "," Expr } [","] ;
```

## trailing comma

1. 객체/배열/match arm 리스트에서 trailing comma를 허용한다.
2. 인자 목록과 import 이름 목록에서 trailing comma를 허용한다.

