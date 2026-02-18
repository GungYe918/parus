좋아. 지금 단계에서 “코드부터”가 아니라, SIR(SemanticIR) -> OIR(OptimizedIR) 파이프라인을 먼저 확정하고, 그 안에서 mut 가능성 판단 + mut 적용(= 실제로 mut이 필요/불필요를 결정하고 체크/진단까지) 을 “가장 먼저” 안정적으로 넣는 설계를 잡아보자.

아래는 너의 현재 구조(AST + SymbolTable + TypeChecker + expr_type_cache_) 를 최대한 살리면서, Rust의 HIR/MIR처럼 점진적으로 의미를 쌓을 수 있는 형태로 제안할게.

⸻

0) 큰 그림: “AST는 구문”, “SIR는 의미”, “OIR는 실행/최적화 친화”

AST (Syntax)
	•	파서가 만든 그대로. Span, 토큰/연산자, 원래 문법 형태를 최대한 유지.
	•	여기서 “mut 판단”을 하면, 분기/블록/대입/별칭(참조) 전파 때문에 코드가 빠르게 지저분해짐.

SIR (SemanticIR)
	•	“의미”가 들어간 IR.
	•	핵심: 식별자 = SymbolId로 고정, 모든 expr/stmt에 TypeId 부착, place vs value 같은 semantic category가 들어감.
	•	mut 판단도 여기에서 하는 게 가장 안정적임:
	•	AST는 “표현”이고, SIR은 “의도(의미)”라서,
	•	“이 대입이 무엇을 변경하는가?”를 정확히 추적 가능.

OIR (OptimizedIR)
	•	백엔드/JIT 직전에 들어가는 “낮은 레벨의 최적화 가능한 형태”.
	•	제어 흐름이 명시적(CFG), 임시값이 명시적, 가능하면 SSA 스타일로 변환 가능.
	•	mut 판단 결과가 “고정된 사실”로 들어가고, 이후 최적화(escape 제거, handle 최적화 등)에 활용됨.

⸻

1) 파이프라인(권장): name/tyck 다음에 SIR을 만든다

너의 현재 TypeChecker는 이미:
	•	SymbolTable로 name lookup
	•	expr_type_cache_로 expr 타입 확정
	•	place 체크(ident/index만 place)
	•	borrow/escape 정책(&, &&)도 타입으로 표현

이걸 기반으로 가장 깔끔한 순서는:
	1.	Parse -> AST
	2.	(선택) NameResolve 패스 (지금은 tyck에서 lookup을 하고 있으니, 당장은 생략 가능하지만 SIR 도입 시 강력 추천)
	3.	TypeCheck(tyck) -> TyckResult(expr_types) + SymbolTable(스코프 기반, declared_type 확정)
	4.	Lower AST -> SIR
	•	이때 모든 Ident를 SymbolId 참조로 바꿔버림
	•	모든 expr/stmt에 TypeId를 박아 넣음(tyck 결과 사용)
	5.	SIR Passes (mutability 포함)
	6.	Lower SIR -> OIR
	7.	OIR Passes (최적화)
	8.	Codegen / JIT ingest

⸻

2) SIR 설계에서 “mut 판단”에 필요한 최소 의미들

mut을 제대로 하려면 SIR에 최소 이 정보가 필요해.

(A) Place / Value를 SIR에서 1급으로

AST에서는 is_place_expr_(Ident/Index)로 “검사”만 했는데,
SIR에서는 아예 노드 자체에 붙이는 게 좋아:
	•	SIRPlace : “저장 위치” (lvalue)
	•	LocalVar(SymbolId)
	•	Index(basePlaceOrValue, indexValue)  (지금은 array만이지만, 확장 가능)
	•	(미래) Field(base, fieldId)
	•	(미래) Deref, etc
	•	SIRValue : 값 계산 결과

그리고 중요한 점:
	•	“대입의 lhs”는 무조건 SIRPlace로 lowering하도록 강제하면,
	•	mut 체크가 매우 단순해져.

(B) 모든 Ident는 SymbolId로 고정

지금 tyck는 sym_.lookup(e.text)를 expr마다 함.
SIR에서는:
	•	SIRValue::VarRef{sym: SymbolId} 같은 식으로 고정.
	•	이렇게 하면 이후 패스들이 “문자열 이름”을 다시 다룰 일이 없음.

(C) stmt/expr에 TypeId 부착

tyck의 expr_type_cache_를 그대로 사용해서
	•	SIR 노드 생성 시 TypeId를 박아두면 된다.

(D) “쓰기(write) 이벤트”를 SIR에서 명확히 표현

mut 판단은 결국 “이 심볼이 쓰였는가?” 를 기반으로 시작한다.

SIR에 다음 형태들이 “write”로 들어오면 충분히 강해짐:
	•	Assign(place, value)
	•	PostfixInc(place)
	•	CompoundAssign(place, value) (미래)
	•	(미래) Call에서 &mut 요구하는 인자 전달도 “쓰기 요구”로 취급 가능

⸻

3) mutability의 의미를 “2단계”로 나누자

너가 말한 “mut가능성 판단 및 실제로 mut키워드 적용”을 안정적으로 하려면, mut을 두 층으로 분리하는 게 깔끔해:

3-1) Declared Mutability (문법상의 mut)
	•	사용자가 let mut x: T = ... 처럼 썼는지
	•	또는 set mut x = ... 같은 문법을 넣을지(아직 미정이지만)
	•	즉 “사용자 선언” 정보

3-2) Required Mutability (의미상 필요 mut)
	•	프로그램 의미상 x가 변경되거나 변경될 가능성이 있는가
	•	이게 “mut 가능성 판단”의 결과물

그리고 규칙은 이렇게:
	•	RequiredMut = true인데 DeclaredMut = false -> 진단(“mut 필요”)
	•	RequiredMut = false인데 DeclaredMut = true -> 경고/옵션(“mut 불필요”)
	•	둘 다 true -> OK
	•	둘 다 false -> OK

즉 “mut inference”는 사용자 코드를 자동으로 바꾸는 기능이 아니라,
“필요/불필요를 결정해서 진단 및(선택적으로) 자동 수정 제안”으로 가는 게 안전해.

⸻

4) SIR 패스 구성(최초 목표: mut inference 고정)

SIR로 들어오면, mut은 아래 패스 세트로 끝낼 수 있어.

Pass S1: BuildDefUseIndex (정적 인덱스 생성)

입력: SIR
출력: DefUseIndex
	•	함수별로:
	•	각 SymbolId가 “정의된 지점(Decl)” 1개
	•	“사용(Use)” 목록
	•	“쓰기(Write)” 목록
	•	“주소/별칭 생성(AddrOf/Borrow/Escape)” 목록

이건 최적화/진단/나중 borrow-check까지 다 쓰이므로 가장 먼저 만드는 게 좋음.

Pass S2: MutabilitySeed

입력: SIR + (AST decl flags or stored decl info)
출력: declared_mut[sym]
	•	문법으로부터 “사용자가 mut을 썼는지” 기록

Pass S3: MutabilityRequirementCollect (핵심)

입력: SIR + DefUseIndex
출력: required_mut[sym]
규칙 v0는 단순하게 시작할 수 있어:

(1) Direct write
	•	Assign(place=LocalVar(sym), ...)가 있으면 required_mut[sym]=true
	•	PostfixInc(LocalVar(sym))도 true

(2) Write-through place (부분 변경)
	•	Assign(place=Index(Var(sym), ...), ...)이면
	•	“sym의 값(배열/구조체)이 변한다”로 보고 required_mut[sym]=true
(이건 Rust의 “부분 대입도 mut 필요”와 동일한 감각)

(3) “mutable borrow 요구”가 생기면 true
	•	미래에 &mut x / 혹은 함수 호출 시 param이 &mut T 요구하는 경우
	•	그 인자로 들어간 base sym은 required_mut=true

v0에서는 (1)(2)만으로도 “실제로 mut키워드가 적용되도록”의 큰 줄기는 완성돼.

Pass S4: MutabilityCheckAndDiag

입력: declared_mut + required_mut + spans
출력: diagnostics / fix-it candidates
	•	required_mut && !declared_mut -> 에러(또는 warning->error)
	•	!required_mut && declared_mut -> warning(옵션)

중요: “스팬(span)”을 위해
	•	SIR decl 노드가 decl_span을 들고 있거나
	•	SymbolTable.symbol(sym).decl_span을 참조하면 됨

Pass S5: MutabilityFinalize (SIR annotation)

입력: required_mut
출력: SIR에 “mutability 결정”을 저장
	•	예: SIRLocalDecl{sym, mut: MutState} 처럼
	•	이후 OIR로 내려갈 때 이 정보가 계속 전달됨

⸻

5) SIR -> OIR로 내려갈 때 mut 정보는 “제약”이 된다

OIR에서는 “mut”을 문법 키워드로 보지 않고, 제약/속성으로 본다.
	•	OIR의 로컬 슬롯/레지스터/SSA value에 대해:
	•	“재할당이 발생하는 변수” -> SSA로 가면 phi/새 버전이 생김
	•	“절대 재할당이 없는 변수” -> 상수 전파/공유/로드 제거가 쉬움

즉 required_mut=false인 심볼은:
	•	OIR에서 “readonly local”로 취급 가능
	•	이후 최적화에서 공격적으로 inline/const-prop 가능

required_mut=true인 심볼은:
	•	“stateful local”로 남기거나
	•	SSA 변환에서 여러 버전으로 분해됨

⸻

6) OIR 패스 구성(최소 뼈대만, mut과 연결되는 부분 중심)

지금 당장 최적화 풀세트를 다 넣을 필요는 없고,
mut 결과를 활용할 수 있는 “최소 OIR 패스”만 잡아두면 돼.

권장 OIR 흐름:

O1: LowerControlFlow
	•	if/while/loop를 “명시적 블록 + 분기”로
	•	CFG 생성의 기반

O2: PromoteReadonlyLocals
	•	required_mut=false인 locals에 대해:
	•	“store가 없으면” load를 값으로 치환 가능
	•	특히 decl init이 상수/순수식이면 상수 전파 출발점

O3: SSA (선택)
	•	JIT/최적화를 제대로 하려면 결국 SSA가 유리
	•	required_mut=true인 것들은 SSA 버전이 늘어남
	•	required_mut=false인 것들은 거의 “단일 정의”라 SSA 비용도 낮음

O4: DCE / ConstFold / SimplifyCFG
	•	mut이 false인 애들은 여기서 급격히 사라지기 쉬움(좋은 의미로)

⸻

7) 너의 현재 코드(tyck/symbol table)와의 접점: 어디까지 “지금” 바꿔야 하나?

지금 당장 바꾸지 않아도 되는 것
	•	TypeChecker의 전체 구조(패스1 함수 수집 + 패스2 체크)
	•	expr_type_cache_ 전략
	•	SymbolTable 구조

단, SIR 도입을 위해 “추가하면 좋은” 최소 확장
	1.	(추천) NameResolve 결과를 AST/SIR에 저장
	•	현재는 tyck에서 lookup을 반복.
	•	SIR lowering 때 최소한 Ident -> SymbolId만 확정 저장하면 충분.
	2.	Decl 노드에 “declared mut” 정보를 저장할 위치
	•	AST에 이미 let mut가 들어가면 좋고,
	•	없으면 SIR 생성할 때 별도 테이블로 보관해도 됨.
	3.	place lowering 규칙을 고정
	•	SIR에서는 “lhs는 반드시 Place”가 되도록 강제
	•	그 변환이 불가능하면 lowering 단계에서 이미 error node를 만들면 됨

⸻

8) v0 mut 규칙을 너무 똑똑하게 만들지 말자 (중요)

mut/alias/borrow가 들어가면 “진짜 분석”은 금방 borrow-check 영역으로 넘어가.
그래서 v0에서는:
	•	“직접 대입/증감” + “부분 대입(Index/Field)”만으로 required_mut을 결정
	•	추후 SIR에 의미를 더 추가하면서:
	•	&mut / “mut-required parameter passing” / 캡처/클로저 등으로 확장

이게 Rust도 실제로 밟은 길이랑 비슷해. (HIR/MIR에 의미를 점진적으로 쌓음)

⸻

9) 최종 결론: 이번 스프린트에서 확정할 “Pass 표준” (SSOT)

SIR 파이프라인(초기 고정안)
	1.	LowerToSIR
	•	Ident -> SymbolId
	•	expr TypeId 부착(tyck 결과)
	•	lhs는 Place로 정규화
	2.	BuildDefUseIndex
	3.	MutabilitySeed (declared_mut)
	4.	MutabilityRequirementCollect (required_mut)
	•	write/partial-write 기반
	5.	MutabilityCheckAndDiag
	•	mut 필요/불필요 진단
	6.	AnnotateSIRMutability (결과를 SIR에 저장)

OIR 파이프라인(초기 골격)
	1.	LowerSIRToOIR (CFG-friendly)
	2.	PromoteReadonlyLocals (required_mut=false 활용)
	3.	(선택) SSA
	4.	ConstFold / DCE / SimplifyCFG

⸻

원하면, 다음 단계로는 “SIR 데이터 구조(노드 종류, place/value 타입, span 보존 방식, 함수 단위 컨테이너)”를 코드 없이 문서처럼 더 정밀하게 스펙화해줄게. (예: sir::ExprKind, sir::PlaceKind, sir::StmtKind, sir::Body, sir::Fn 등)