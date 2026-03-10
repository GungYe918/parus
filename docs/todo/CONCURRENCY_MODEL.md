# Parus Concurrency Model (Design Freeze)

문서 버전: `draft-0.1`  
상태: `Design Freeze (TODO Track)`  
목적: `actor`, 병렬 실행, `async/await`의 경계와 표면 문법을 구현 전 기준으로 고정한다.

## 1. 목적과 범위

이 문서는 Parus의 전체 병렬성 정책을 설계 동결 문서로 고정한다.

1. `actor`를 스레드 primitive가 아니라 격리된 상태머신으로 정의한다.
2. 언어와 런타임의 책임 경계를 분리한다.
3. 미래 canonical actor 생성식과 비동기 표면 문법을 고정한다.
4. 구현자가 parser/tyck/runtime 작업 범위를 추가 결정 없이 파악할 수 있도록 예제와 진단 기준을 함께 제시한다.

비범위:

1. `lazy` 평가 모델
2. `async { ... }` 블록 표현식의 정식 도입
3. 실제 executor/runtime API 이름의 정본 확정
4. actor 스케줄링 구현(thread/task/mailbox)의 구체 알고리즘

## 2. 비가역 결정 사항 (Non-negotiable)

1. `actor`는 격리된 상태머신이며, 스레드 primitive가 아니다.
2. actor 실행 배치(thread/task/single-thread loop/kernel mailbox)는 런타임 정책이다.
3. 언어는 `draft/pub/sub/commit/recast` 불변식만 고정한다.
4. 미래 canonical actor 생성식은 `A(...)`다.
5. actor 생성식의 결과는 값이 아니라 runtime-managed actor handle이다.
6. 언어 키워드 `spawn`은 제거되며, 일반 식별자 공간으로 복귀한다.
7. 병렬 실행 시작은 언어 키워드가 아니라 런타임 라이브러리 `foo::spawn` 계열 책임이다.
8. 비동기 canonical 표기는 `async def`다.
9. qualifier canonical 순서는 `export async def ...`, `extern async def ...`다.
10. `await expr`는 정식 범위에 포함한다.
11. actor `pub/sub` 메서드에는 v1에서 `async`를 허용하지 않는다.
12. `async { ... }`와 `lazy`는 이번 문서 정식 범위에서 제외한다.

## 3. 현재 구현 기준 스냅샷

현재 코드베이스 사실 기준:

1. actor 생성은 `A(...)` 생성식을 사용한다.
2. actor 생성식의 결과는 runtime-managed actor handle이다.
3. `commit/recast`는 parser/tyck/SIR/OIR/LLVM까지 연결되어 있고, LLVM 하향에서는 actor runtime ABI call로 표현된다.
4. `async/await`는 아직 lexer/parser/tyck/IR에 도입되어 있지 않다.
5. `spawn`은 더 이상 키워드가 아니며, 이 문서에서는 thread/task 라이브러리 예시 이름으로만 등장할 수 있다.

이 문서는 위 구현 스냅샷과 미래 canonical 모델을 함께 기록한다. 구현은 아직 snapshot 상태를 따르더라도, 이후 설계 기준은 본 문서를 따른다.

## 4. 핵심 용어와 모델

## 4.1 actor

1. `actor`는 큰 공유 상태를 격리된 단일 소유 컨텍스트로 관리하는 상태머신이다.
2. actor의 핵심 불변식은 `draft/pub/sub/commit/recast`다.
3. actor는 상태 격리 모델이지 실행 단위(thread/task)를 직접 지정하지 않는다.
4. actor는 scheduler-agnostic하다.

## 4.2 thread

1. `thread`는 실제 병렬 실행 단위다.
2. thread 생성/조인/detach는 언어가 아니라 라이브러리/runtime 계층 책임이다.
3. thread API는 OS-hosted와 freestanding 환경 모두를 고려해 런타임이 선택한다.

## 4.3 task

1. `task`는 executor가 스케줄하는 비동기 작업 단위다.
2. task는 반드시 OS thread와 1:1 대응하지 않는다.
3. cooperative scheduling과 thread-pool scheduling 모두 task 런타임 정책이다.

## 4.4 runtime / executor

1. runtime은 actor/thread/task를 실제로 배치하고 구동하는 계층이다.
2. executor는 async task를 poll/resume하는 실행기다.
3. 언어는 runtime/executor의 존재와 최소 계약만 가정하고, 기본 구현 방식은 고정하지 않는다.

## 4.5 언어와 런타임의 책임 분리

언어가 고정하는 것:

1. actor의 격리 규칙
2. `commit/recast` 경계 의미
3. `async def` / `await` 표면 문법
4. async 함수의 suspend/resume 가능성

런타임이 결정하는 것:

1. actor가 어느 실행기 위에 올라가는지
2. thread/task 배치 정책
3. mailbox/event loop/waker 구현
4. block-on/join/detach의 실제 스케줄링 동작

## 5. actor 모델

## 5.1 actor 선언과 상태 경계

```parus
actor Counter {
  draft {
    value: i32;
  }

  init(seed: i32) {
    draft.value = seed;
  }

  def sub get() -> i32 {
    recast;
    return draft.value;
  }

  def pub add(delta: i32) -> i32 {
    draft.value = draft.value + delta;
    commit;
    return draft.value;
  }
}
```

고정 규칙:

1. `draft`는 actor의 내부 상태 저장소다.
2. `sub`는 관찰/조회 경로다.
3. `pub`는 수정/발행 경로다.
4. `commit;`은 수정 결과를 발행하는 경계다.
5. `recast;`는 관찰 스냅샷을 재동기화하는 경계다.

## 5.2 actor 생성의 미래 canonical 표기

미래 canonical 예시:

```parus
set c = Counter(seed: 1i32);
set n = c.get();
set m = c.add(delta: 2i32);
```

고정 규칙:

1. actor 생성은 class와 같은 표면 문법 `A(...)`를 사용한다.
2. class 생성식은 값/인스턴스를 만든다.
3. actor 생성식은 actor handle을 만든다.
4. actor handle은 runtime-managed entity reference다.
5. actor handle의 내부 배치(thread/task/loop/mailbox)는 사용자 문법에서 관찰하지 않는다.

## 5.3 왜 `spawn`을 제거하는가

`spawn` 제거 이유:

1. `spawn`은 실행기(thread/task)를 연상시키며 actor의 의미를 과도하게 runtime 중심으로 보이게 한다.
2. actor는 스레드 primitive가 아니라 상태 모델이므로, 생성 문법이 실행 정책을 암시하면 안 된다.
3. thread/task 확장을 위해 `spawn`을 유지하면 actor와 runtime API 경계가 흐려진다.
4. actor 생성은 타입 생성식으로 통일하는 편이 문법 계층이 더 일관된다.

현재 구현과의 차이:

1. actor 생성/handle 런타임은 이미 구현되었다.
2. 아직 미구현인 것은 `async/await`와 thread/task public API다.

## 6. 병렬 실행 API 경계

## 6.1 언어 키워드가 아닌 라이브러리 책임

병렬 실행 시작은 라이브러리 API로 제공한다.

예시:

```parus
set t = thread::spawn(worker(arg: 1i32));
set h = task::spawn(fetch_value());
set v = runtime::block_on(main_async());
```

고정 규칙:

1. `thread::spawn`, `task::spawn`, `runtime::block_on`은 예시 이름이다.
2. 실제 표준 라이브러리 네임스페이스는 후속 문서에서 고정한다.
3. 언어는 “병렬 실행 시작은 라이브러리 계층 책임”이라는 정책만 고정한다.
4. `spawn` 키워드의 재사용 계획은 없다.

## 6.2 core/std와의 관계

1. core 측에는 future/executor 계약이 들어갈 수 있다.
2. std 측에는 thread/task/runtime 편의 API가 들어갈 수 있다.
3. 이번 문서에서는 trait/proto/type 이름을 정식 확정하지 않는다.
4. 단, 언어 내장 키워드로 thread/task 실행을 직접 표현하지 않는다는 점은 확정한다.

## 7. 비동기 모델

## 7.1 표면 문법

정식 표기:

```parus
async def fetch() -> i32 {
  return 1i32;
}

export async def main_async() -> i32 {
  set v = await fetch();
  return v;
}
```

canonical 순서:

1. `async def ...`
2. `export async def ...`
3. `extern async def ...`

비채택:

1. `def async ...`
2. `async { ... }`
3. actor `async def pub/sub ...`

## 7.2 EBNF 초안

```ebnf
AsyncFnDecl   := LinkageOpt "async" "def" Ident GenericParamClauseOpt
                 FuncParams ConstraintClauseOpt "->" Type Block ;

AwaitExpr     := "await" Expr ;

ActorCtorExpr := PathType "(" ArgListOpt ")" ;
```

보조 규칙:

1. `ActorCtorExpr`는 문법상 class 생성식과 동일한 표면을 가지며, 의미론에서 actor type일 때 handle 생성으로 판정한다.
2. `await`는 prefix keyword expression이다.
3. `await`의 피연산자 형식과 future lowering 규칙은 후속 구현 문서에서 구체화한다.

## 7.3 `await` 규칙

1. `await expr`는 async 함수 내부에서만 허용한다.
2. non-async 함수에서의 `await`는 컴파일 오류다.
3. `await`는 future/task state machine을 poll/resume하는 논리 경계다.
4. `await`가 실제 어떤 runtime hook을 호출하는지는 executor 구현 책임이다.

## 7.4 async 함수의 의미

1. 표면 반환형은 `T`로 적는다.
2. lowering 관점에서는 async 함수가 future/task state machine으로 변환된다.
3. 호출 지점은 직접 `T`를 받지 않고, `await` 또는 runtime API를 통해 결과를 관찰한다.
4. JIT/AOT 모두에서 async 함수의 소스 의미는 동일해야 한다.

## 7.5 actor와 async를 분리하는 이유

actor `async`를 v1에서 금지하는 이유:

1. `pub`는 수정 경계와 `commit;` 규칙을 가진다.
2. suspend 지점이 `commit` 이전/이후에 끼어들면 상태 발행 규칙이 복잡해진다.
3. `sub`도 `recast;`와 snapshot 의미를 가지므로 suspend 경계가 섞이면 관찰 모델이 불명확해진다.
4. 따라서 v1은 actor 상태머신과 async suspend 모델을 직접 결합하지 않는다.

## 8. 현재 미지원/보류 항목

## 8.1 `async { ... }` 블록 표현식

다음 문법은 이번 문서의 정식 범위에 포함하지 않는다.

```parus
set fut = async {
  return 42i32;
};
```

보류 이유:

1. 캡처 규칙
2. borrow/move 캡처 규칙
3. 지역 변수 수명과 future 수명 관계
4. 명시/암시 capture 정책

따라서 `async { ... }`는 후속 설계 과제로만 기록한다.

## 8.2 lazy 평가

1. Parus의 기본 평가 전략은 eager다.
2. `lazy`는 explicit model로만 검토한다.
3. 이번 문서에서는 `lazy`, `force`, memoized thunk를 다루지 않는다.

## 9. 진단 규칙 고정

future model 기준 필수 진단 범주:

1. `AwaitOutsideAsync`
2. `ActorAsyncNotSupportedV1`
3. `AsyncBlockNotEnabled`

진단 정책:

1. 현재 구현 snapshot과 미래 canonical model을 혼동하지 않도록 메시지에 문맥을 포함한다.
2. `await` outside async는 위치 지점과 enclosing function kind를 함께 보고한다.
3. actor `async` 금지 진단은 `commit/recast` 불변식 충돌 가능성을 설명한다.

## 10. 예제

## 10.1 정상 예제 (미래 canonical)

actor 생성/호출:

```parus
actor Counter {
  draft {
    value: i32;
  }

  init(seed: i32) {
    draft.value = seed;
  }

  def sub get() -> i32 {
    recast;
    return draft.value;
  }

  def pub add(delta: i32) -> i32 {
    draft.value = draft.value + delta;
    commit;
    return draft.value;
  }
}

def main() -> i32 {
  set c = Counter(seed: 1i32);
  c.add(delta: 2i32);
  return c.get();
}
```

class vs actor 생성 차이:

```parus
class Point {
  x: i32;
  y: i32;
};

actor Scene {
  draft {
    count: i32;
  }

  init() {
    draft.count = 0i32;
  }

  def sub value() -> i32 {
    recast;
    return draft.count;
  }
}

def main() -> i32 {
  set p = Point(x: 1i32, y: 2i32);
  set s = Scene();
  return p.x + s.value();
}
```

top-level async:

```parus
async def load_value() -> i32 {
  return 7i32;
}

export async def run() -> i32 {
  set v = await load_value();
  return v;
}
```

class method async:

```parus
class Client {
  async def fetch(self) -> i32 {
    return 1i32;
  }
};
```

acts method async:

```parus
acts for i32 {
  async def next(self) -> i32 {
    return self + 1i32;
  }
};
```

thread runtime 예시:

```parus
def worker(x: i32) -> i32 {
  return x + 1i32;
}

def main() -> i32 {
  set h = thread::spawn(worker(1i32));
  return thread::join(h);
}
```

task runtime 예시:

```parus
async def fetch() -> i32 {
  return 5i32;
}

def main() -> i32 {
  set h = task::spawn(fetch());
  return task::join(h);
}
```

block_on 예시:

```parus
async def main_async() -> i32 {
  return 42i32;
}

def main() -> i32 {
  return runtime::block_on(main_async());
}
```

## 10.2 실패 예제 (미래 canonical)

actor async pub:

```parus
actor Counter {
  draft { value: i32; }

  async def pub add(delta: i32) -> i32 {
    draft.value = draft.value + delta;
    commit;
    return draft.value;
  }
}
// error: ActorAsyncNotSupportedV1
```

actor async sub:

```parus
actor Counter {
  draft { value: i32; }

  async def sub get() -> i32 {
    recast;
    return draft.value;
  }
}
// error: ActorAsyncNotSupportedV1
```

await outside async:

```parus
def main() -> i32 {
  return await load_value();
}
// error: AwaitOutsideAsync
```

async block:

```parus
def main() -> i32 {
  set fut = async {
    return 1i32;
  };
  return 0i32;
}
// error: AsyncBlockNotEnabled
```

actor lifecycle direct call:

```parus
def main() -> i32 {
  set c = Counter(seed: 1i32);
  c.init(seed: 0i32);
  return 0i32;
}
// error: actor lifecycle direct call forbidden
```

runtime policy leakage:

```parus
def main() -> i32 {
  set c = Counter(seed: 1i32);
  // 금지: 언어 차원에서 actor를 특정 스레드에 고정하는 문법은 제공하지 않는다.
  return 0i32;
}
```

lazy 포함 시도:

```parus
def main() -> i32 {
  // error: lazy model is out of scope for this design
  return 0i32;
}
```

## 11. 구현 로드맵 (문서용)

1. 1차: `async`/`await` lexer-parser-tyck-SIR/OIR 설계
2. 2차: future/executor core 계약 설계
3. 3차: runtime thread/task API 설계
4. 4차: actor+async 결합 여부 재평가
5. 5차: `async { ... }`와 lazy 모델 별도 설계

## 12. 수용 기준

1. 문서만 읽고 구현자가 parser/tyck/runtime 경계를 결정할 수 있어야 한다.
2. actor/thread/task/async의 책임 경계에 미결정 항목이 남지 않아야 한다.
3. 현재 구현 snapshot과 미래 canonical model이 혼동되지 않아야 한다.
4. actor 생성 canonical 예제는 모두 `A(...)`로 일관돼야 한다.
5. thread/task 예제는 모두 라이브러리 호출로만 제시돼야 한다.
6. `async def` 예제는 전부 `async def` / `export async def` 순서를 따라야 한다.
7. actor `async` 금지와 `async {}` 보류가 문서 전반에서 일관돼야 한다.

## 13. 명시적 가정과 기본값

1. 이번 문서는 TODO 설계 동결 문서이며, 구현 후에만 normative spec 승격을 검토한다.
2. `spawn`은 미래 언어 키워드로 유지하지 않는다.
3. actor 생성은 class와 같은 표면 문법 `A(...)`로 통일한다.
4. actor 생성 결과는 값이 아니라 handle 의미로 고정한다.
5. `async def`가 canonical 표기이며 `def async`는 채택하지 않는다.
6. actor 메서드 `async`는 v1에서 금지한다.
7. `async { ... }`와 `lazy`는 본 문서 정식 범위에서 제외한다.
8. 병렬 실행 API 네이밍은 예시 수준으로만 두고, 실제 core/std 네임스페이스는 후속 문서에서 고정한다.
