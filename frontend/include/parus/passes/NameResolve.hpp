// frontend/include/parus/passes/NameResolve.hpp
#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_set>

#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/sema/SymbolTable.hpp>


namespace parus::passes {

    // -----------------------------------------------------------------------------
    // shadowing 처리 정책
    // -----------------------------------------------------------------------------
    enum class ShadowingMode : uint8_t {
        kAllow,   // 기본: 허용, 진단 없음
        kWarn,    // 경고
        kError,   // 에러(하지만 “금지”와 동일한 의미로 쓰진 않음: 선언 자체는 계속 진행 가능)
    };

    struct NameResolveOptions {
        ShadowingMode shadowing = ShadowingMode::kAllow;
        uint32_t current_file_id = 0;
        std::string current_bundle_name{};
        std::string current_module_head{};
        std::string current_source_dir_norm{};
        std::unordered_set<std::string> allowed_import_heads{};

        struct ExternalExport {
            sema::SymbolKind kind = sema::SymbolKind::kVar;
            std::string path{};
            std::string link_name{};
            ty::TypeId declared_type = ty::kInvalidType;
            std::string declared_type_repr{};
            Span decl_span{};
            std::string decl_bundle_name{};
            std::string module_head{};
            std::string decl_source_dir_norm{};
            bool is_export = true;
        };
        std::vector<ExternalExport> external_exports{};
    };

    // -----------------------------------------------------------------------------
    // BindingKind (v0 fixed + future slots)
    //
    // - "이 심볼이 어떤 바인딩으로 쓰였는가"를 해석하기 위한 분류.
    // - SymbolId 하나만으로는 부족한 케이스(오버로드 셋/모듈 경로/특수 바인딩 등)를
    //   future에 ResolvedSymbol payload로 확장하기 위해 별도 kind를 둔다.
    // -----------------------------------------------------------------------------
    enum class BindingKind : uint8_t {
        kNone = 0,

        // v0
        kLocalVar,   // let/set로 선언된 local var
        kParam,      // 함수 파라미터
        kFn,         // 함수 선언 심볼

        // future
        kType,
        kModule,
        kUseAlias,
        kOverloadSet,
        kBuiltin,
    };

    // -----------------------------------------------------------------------------
    // ResolvedSymbol (table entry)
    // -----------------------------------------------------------------------------
    struct ResolvedSymbol {
        using SymbolId = uint32_t;
        static constexpr SymbolId k_invalid_symbol = 0xFFFF'FFFFu;

        BindingKind bind = BindingKind::kNone;
        SymbolId sym = k_invalid_symbol;

        // "이 바인딩이 일어난" 지점 span (use-site / decl-site 모두 가능)
        Span span{};
    };

    // -----------------------------------------------------------------------------
    // NameResolveResult (fixed shape)
    //
    // - expr_to_resolved[eid] : ExprId(kIdent 등) -> ResolvedSymbolId
    // - stmt_to_resolved[sid] : decl 성격 stmt(kVar/kFnDecl 등) -> ResolvedSymbolId
    // - param_to_resolved[pid]: Param index(ast.params() index) -> ResolvedSymbolId
    //
    // NOTE:
    // - SIR builder가 스코프 정보를 다시 만들지 않도록, “해결된 결과”를 보존한다.
    // - 미해결은 k_invalid_resolved로 유지된다.
    // -----------------------------------------------------------------------------
    struct NameResolveResult {
        using ResolvedId = uint32_t;
        static constexpr ResolvedId k_invalid_resolved = 0xFFFF'FFFFu;

        std::vector<ResolvedSymbol> resolved;

        std::vector<ResolvedId> expr_to_resolved;  // size = ast.exprs().size()
        std::vector<ResolvedId> stmt_to_resolved;  // size = ast.stmts().size()
        std::vector<ResolvedId> param_to_resolved; // size = ast.params().size()

        void reset_sizes(uint32_t expr_count, uint32_t stmt_count, uint32_t param_count) {
            resolved.clear();
            resolved.reserve(expr_count / 2);

            expr_to_resolved.assign(expr_count, k_invalid_resolved);
            stmt_to_resolved.assign(stmt_count, k_invalid_resolved);
            param_to_resolved.assign(param_count, k_invalid_resolved);
        }
    };

    // Public API: out_result는 “참조”로 강제한다. (포인터/optional 금지)
    void name_resolve_stmt_tree(
        const ast::AstArena& ast,
        ast::StmtId root,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt,
        NameResolveResult& out_result
    );

} // namespace parus::passes
