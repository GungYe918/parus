# Parlib Specification v1

문서 버전: `v1.0.0`  
상태: `Normative (Single Source of Truth)`

이 문서는 Parus `parlib` 구현의 단일 신뢰 기준이다.  
`docs/internal/compiler/backend/Backend.md` 또는 기존 구현 주석과 충돌 시, **parlib 관련 사항은 본 문서를 우선**한다.

---

## 1. 목적

Parlib는 Parus의 번들 산출물을 하나의 포맷으로 묶어 다음 목표를 동시에 만족한다.

1. AOT/JIT 공용 입력 포맷 제공
2. 빠른 랜덤 액세스(TOC 기반) 제공
3. 스트리밍 쓰기/읽기 경로 제공
4. C ABI 연계(외부 네이티브 라이브러리 링크 포함) 자동화 기반 제공

---

## 2. 배포 포지션 (고정)

Parus는 다음 배포 정책을 채택한다.

1. 기본 배포: **소스 배포 우선**
2. 내부 캐시/사내 재사용: `parlib` 사용
3. 외부 언어(C/C++/Rust/Zig 등) 연동: `export "C"` 기반 `.a/.so/.dylib` 병행 배포

즉, Parlib는 C ABI를 대체하지 않는다. Parlib는 Parus 내부 재사용/링크 최적화를 위한 포맷이다.

---

## 3. 호환성 정책

1. 본 문서의 parlib는 **`v1`로 고정**한다.
2. 기존 저장 포맷/스캐폴드 구현은 **레거시로 간주하며 호환하지 않는다**.
3. 리더는 레거시 포맷을 자동 업그레이드하지 않고 즉시 오류를 반환해야 한다.

알파 이전 단계이므로 포맷 단절은 허용한다.

---

## 4. 단위 개념

1. `lane`: `global`, `pcore`, `prt`, `pstd` (필수)
2. `chunk`: lane에 속한 데이터 블록
3. `bundle`: 하나의 Parus 컴파일/배포 단위
4. `target`: target triple + abi + feature 집합

---

## 5. 파일 구조

Parlib v1은 아래 순서를 사용한다.

```text
[Header][Chunk Stream][TOC][Footer]
```

의도:

1. `Chunk Stream`을 먼저 써서 스트리밍 빌드를 지원
2. 마지막 `TOC`/`Footer`로 랜덤 액세스를 가능하게 함

### 5.1 Header (고정 필드)

Header는 최소 아래 정보를 포함해야 한다.

1. magic
2. format major/minor
3. flags
4. compiler id/hash
5. bundle id
6. target summary
7. chunk_count
8. toc_offset
9. file_size

### 5.2 TOC (Table of Contents)

각 엔트리는 아래 정보를 포함한다.

1. `kind`
2. `lane`
3. `target_id` (global 청크는 0)
4. `offset`
5. `size`
6. `alignment`
7. `compression`
8. `checksum/hash`

### 5.3 Footer

Footer는 최소 아래를 포함한다.

1. footer magic
2. toc_offset
3. toc_size
4. footer checksum

리더는 Footer -> TOC 순으로 읽는 경로를 기본으로 사용한다.

---

## 6. 필수 청크

### 6.1 Global lane

1. `Manifest`
2. `StringTable`
3. `ExportCIndex`
4. `NativeDeps`

### 6.2 각 코드 lane (`pcore/prt/pstd`)

1. `SymbolIndex`
2. `TypeMeta`
3. `OIRArchive`
4. `ObjectArchive`

### 6.3 선택 청크

1. `Debug`
2. `SourceMap` (옵션)
3. `NativeArchivePayload` (embed 모드일 때)

---

## 7. 네이티브 의존성 모델 (FFI 연계 핵심)

`NativeDeps`는 외부 C/C++ 라이브러리 링크 정보를 담는다.

각 의존 항목은 다음 필드를 갖는다.

1. `name`
2. `kind` (`static`, `shared`, `framework`, `system`)
3. `mode` (`embed`, `reference`)
4. `target_filter`
5. `link_order`
6. `required` (bool)
7. `hash` (reference 검증용)

정책:

1. `embed`: 라이브러리 payload를 parlib 내부 chunk로 포함
2. `reference`: 경로/식별자 + 해시만 기록

---

## 8. FFI 연계 방식

### 8.1 `export "C"` 심볼

`ExportCIndex`는 최소 아래를 포함해야 한다.

1. C 심볼명
2. 선언 시그니처 요약
3. 정의 위치 lane/chunk id
4. visibility 정보

### 8.2 링크 단계

Parlib 기반 링크는 아래 순서를 따른다.

1. `ExportCIndex`/`NativeDeps` 로드
2. target에 맞는 `ObjectArchive` 선택
3. `NativeDeps`의 `embed/reference` 규칙 적용
4. 최종 링커 인자 조립 후 링크

---

## 9. 랜덤 액세스 API (정본)

Reader API는 최소 아래 형태를 제공해야 한다.

```cpp
ParlibReader open(path);
Header read_header();
std::vector<ChunkRecord> list_chunks();
std::optional<ChunkRecord> find_chunk(kind, lane, target_id);
Bytes read_chunk_slice(record, offset, size);
ChunkStream open_chunk_stream(record);
std::optional<ExportCEntry> lookup_export_c(symbol_name);
```

요구사항:

1. `find_chunk`는 TOC 인덱스 기반으로 동작
2. `read_chunk_slice`는 전체 파일 로드 없이 부분 읽기 가능해야 함
3. 대형 chunk(`ObjectArchive`, `OIRArchive`)도 부분 읽기를 지원해야 함

---

## 10. 스트리밍 지원 (정본)

### 10.1 Writer

Writer API는 최소 아래 순서를 보장한다.

```cpp
begin(output);
append_chunk(meta, input_stream);
append_chunk(meta, input_stream);
finalize();
```

규칙:

1. chunk payload는 순차 입력 스트림으로 받아야 한다.
2. 해시/체크섬은 스트리밍 계산해야 한다.
3. finalize 시 TOC/Footer를 기록하고 파일을 닫는다.

### 10.2 Reader

1. seek 가능한 입력: Footer -> TOC -> 임의 chunk 접근
2. seek 불가 입력: 순차 스캔 모드 제공 (기능 제한 허용)

---

## 11. JIT/AOT 연계 규칙

1. AOT는 `ObjectArchive`를 우선 소비한다.
2. JIT는 `OIRArchive`를 우선 소비한다.
3. 양쪽 모두 `NativeDeps`/`ExportCIndex`를 동일 규칙으로 해석해야 한다.
4. lane 해석 규칙은 JIT/AOT에서 동일해야 한다.

---

## 12. 소스 경로 정책

Parlib는 기본적으로 `.pr` 절대경로를 필수로 저장하지 않는다.

1. 기본: 경로 비저장 또는 bundle-relative 경로 저장
2. 디버그 필요 시 `SourceMap`에 상대 경로 + content hash 저장
3. 절대경로 의존 동작은 금지

---

## 13. parlib vs `.a` / `rlib` 관점

### 13.1 장점

1. OIR/Object/심볼/FFI 메타를 단일 파일로 관리 가능
2. JIT/AOT 공용 입력으로 사용 가능
3. 링크 자동화에 필요한 구조화된 정보 보유

### 13.2 단점

1. 표준 링커 직접 소비 불가
2. 포맷/도구 유지비용 증가
3. 버전/검증 정책이 약하면 호환 리스크 증가

### 13.3 보완 원칙

1. `parlib-inspect`, `parlib-verify`, `parlib-extract` 도구를 제공
2. 항상 `parlib -> .a/.so/.dylib` 산출 경로 제공
3. compiler hash/target hash 불일치 시 즉시 거부

---

## 14. 구현 체크리스트

다음을 통과해야 parlib v1 준수로 본다.

1. Writer가 스트리밍 입력으로 parlib를 생성한다.
2. Reader가 Footer/TOC 기반 랜덤 액세스를 지원한다.
3. `NativeDeps` embed/reference 규칙이 구현된다.
4. `ExportCIndex`를 통해 C ABI 심볼 조회가 가능하다.
5. AOT/JIT가 동일한 lane/chunk 규칙을 사용한다.
6. 레거시 parlib 포맷 입력을 명시적으로 거부한다.

---

## 15. 비범위 (v1)

1. 네트워크 레지스트리 프로토콜
2. 원격 캐시 프로토콜 표준화
3. CUE/기타 빌드 메타 언어 통합

위 항목은 별도 문서에서 다룬다.
