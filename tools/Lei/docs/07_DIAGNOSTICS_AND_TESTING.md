# 07. Diagnostics And Testing

## 진단 계층

1. `C_*`: 파싱/문법 오류
2. `L_*`: 평가/템플릿/심볼 오류
3. `B_*`: 그래프/빌드 모델 오류

## Module-First 핵심 진단

1. `B_BUNDLE_MODULES_REQUIRED`
2. `B_MODULE_SCHEMA_INVALID`
3. `B_MODULE_HEAD_REMOVED`
4. `B_MODULE_AUTO_HEAD_CONFLICT`
5. `B_MODULE_IMPORT_INVALID`
6. `B_MODULE_TOP_HEAD_COLLISION`
7. `B_MODULE_HEAD_COLLISION`
8. `B_IMPORT_MODULE_NOT_DECLARED`
9. `B_BUNDLE_DEP_NOT_DECLARED`
10. `B_INLINE_BUNDLE_MULTI_FORBIDDEN`
11. `B_LEGACY_BUNDLE_SOURCES_REMOVED`

## Parus 연동 진단

1. `ImportDepNotDeclared` (Parus import gate)
2. `SymbolNotExportedFileScope`
3. `SymbolNotExportedBundleScope`
4. `ExportCollisionSameFolder`
5. `NestNotUsedForModuleResolution`

## 테스트 기준

1. LEI:
   1. `module`/`bundle.modules` 파싱/평가
   2. `module.head` 제거 검증
   3. `module.imports` canonicalization(`foo`, `foo::bar`, `::foo::bar`)
   4. `module.imports` malformed 입력 실패
   5. top-head 충돌 실패
   6. `bundle.deps` cycle 검증
   7. inline bundle 단일 허용 검증
2. Parus:
   1. 같은 module export auto-share 성공
   2. 같은 module non-export 참조 실패
   3. 다른 module import + module.imports 만족 시 성공
   4. `module.imports` 누락 import 실패
   5. `import ::foo::bar as x;` 파싱/해석 성공
3. E2E:
   1. `parus check`
   2. `parus build`
   3. `parus graph`
