# Parus File Item Model v0.0.1

문서 버전: `v0.0.1`
상태: `Normative (Top-level Item Rules)`

이 문서는 Parus의 파일 최상위 구성 규칙(`item` 모델)을 정의한다.
문법 관련 충돌이 발생하면 본 문서를 우선 적용한다.

---

## 1. 목적

Parus 파일 스코프를 `item` 중심으로 고정해 다음을 달성한다.

1. top-level 문법을 단순화한다.
2. 선언(declaration)과 실행(statement) 영역을 분리한다.
3. 진단 메시지를 일관되게 만든다.

---

## 2. 코어 규칙 (v0 고정)

1. 파일은 `item`과 `empty item`(`;`)의 반복을 허용한다.
2. `statement`는 블록 내부에서만 허용한다.
3. `item = declaration item + directive item`.
4. `empty item`(`;`)은 no-op으로 허용한다.
5. `declaration item (braced)` 자체는 종결 `;`를 요구하지 않는다.
6. braced declaration 뒤의 `;`는 별도 `empty item`으로 허용한다.
7. `simple item`은 끝 `;`가 필수다.

---

## 3. item 분류

### 3.1 declaration item

#### 3.1.1 braced declaration item (종결 `;` 불필요)

1. `def` 본문 선언
2. `field`
3. `acts`
4. (미래) `proto`
5. (미래) `tablet`
6. (미래) `class`

#### 3.1.2 simple declaration item (`;` 필수)

1. `extern "C" def ...;`
2. 전역 변수 선언

### 3.2 directive item (`;` 필수)

1. `import ...;`
2. `use ...;`
3. `nest path;` (파일 지시어)

`nest path { ... }`는 block을 갖는 declaration-style item으로 취급한다.

---

## 3.3 EBNF (v0)

```ebnf
File            := (Item | EmptyItem)* EOF ;
EmptyItem       := ";" ;

Item            := DeclItem | DirectiveItem ;

DeclItem        := BracedDeclItem | SimpleDeclItem ;
BracedDeclItem  := NormalFuncDecl
                 | CAbiFuncDef
                 | FieldDecl
                 | ProtoDecl
                 | TabletDecl
                 | ActsDecl
                 | ClassDecl ;
SimpleDeclItem  := CAbiFuncDecl | GlobalVarDecl ;

DirectiveItem   := ImportStmt | UseStmt | NamespaceDecl ;
```

---

## 4. 예제

### 4.1 올바른 예제

```parus
import game;
use game::math::Vec2 as V2;
nest app::core;
;
;;

field Vec2 {
  x: i32;
  y: i32;
}

acts A for Vec2 {
  def add(self, rhs: i32) -> i32 {
    return self.x + rhs;
  }
}

extern "C" def puts(s: ptr u8) -> i32;
let g_count: i32 = 0;

def main() -> i32 {
  let mut v: V2 = Vec2{ x: 1, y: 2 } with acts(A);
  return v.add(3);
}
```

### 4.2 잘못된 예제

```parus
if (true) { } // error: top-level statement 금지

foo(); // error: top-level expression statement 금지

extern "C" def puts(s: ptr u8) -> i32 // error: simple item ';' 누락
```

---

## 5. top-level에서 허용되지 않는 것

1. `if` / `while` / `switch` 같은 제어 statement
2. expression statement (`foo();`)
3. `return` / `break` / `continue`

---

## 6. 진단 권장 문구

1. `TopLevelDeclOnly`: `top-level allows items only`
2. `UnexpectedToken`: `top-level statement/expression is not allowed`
3. `ExpectedToken`: `simple item requires ';' terminator`

---

## 7. 구현 체크리스트

1. 파서가 top-level에서 `item` 외 구문을 거부한다.
2. `import` file-scope 규칙을 일관 검증한다.
3. `;`/`;;`를 empty item으로 허용하고 no-op 처리한다.
4. simple item에서 `;` 누락 시 복구 진단을 제공한다.

---

## 8. 관련 문서

1. `docs/reference/language/SPEC.md`
2. `docs/reference/language/terminology.md`
3. `docs/reference/abi/v0.0.1/ABI.md`
4. `docs/reference/abi/v0.0.1/OOP_MODEL.md`
