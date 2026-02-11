// compiler/src/sir/mut_analysis.cpp
#include <gaupel/sir/MutAnalysis.hpp>
#include <gaupel/diag/DiagCode.hpp>

namespace gaupel::sir {

    static bool is_local_place(const Value& v) {
        return v.place == PlaceClass::kLocal && v.sym != k_invalid_symbol;
    }

    static std::optional<SymbolId> root_written_symbol(const Module& m, ValueId lhs) {
        if (lhs == k_invalid_value) return std::nullopt;
        if (lhs >= m.values.size()) return std::nullopt;

        const Value& v = m.values[lhs];

        // direct local
        if (v.kind == ValueKind::kLocal && v.sym != k_invalid_symbol) {
            return v.sym;
        }

        // index write: a[i] = ...
        if (v.kind == ValueKind::kIndex) {
            // v.a = base, v.b = index
            if (v.a == k_invalid_value) return std::nullopt;
            const Value& base = m.values[v.a];
            if (base.kind == ValueKind::kLocal && base.sym != k_invalid_symbol) {
                return base.sym;
            }
        }

        // future: field/deref/etc.
        return std::nullopt;
    }

    MutAnalysisResult analyze_mut(const Module& m, diag::Bag& bag) {
        MutAnalysisResult r{};

        // 1) Collect declared mut info from var decl stmts
        for (const auto& st : m.stmts) {
            if (st.kind != StmtKind::kVarDecl) continue;
            if (st.sym == k_invalid_symbol) continue;

            MutInfo& info = r.by_symbol[st.sym];
            info.declared_mut = st.is_mut;
            info.is_set = st.is_set;
        }

        // helper: report illegal write once (but we can allow multiple if you want)
        auto report_illegal = [&](Span sp, SymbolId sym, const char* what) {
            (void)sym;
            // NOTE: diag code는 새로 추가 필요 (kWriteToImmutable 등)
            diag::Diagnostic d(diag::Severity::kError, diag::Code::kWriteToImmutable, sp);
            d.add_arg(what);
            bag.add(std::move(d));
        };

        // 2) Walk values: assign / postfix++ are writes
        for (uint32_t vid = 0; vid < (uint32_t)m.values.size(); ++vid) {
            const Value& v = m.values[vid];

            if (v.kind == ValueKind::kAssign) {
                // v.a = lhs, v.b = rhs
                auto sid = root_written_symbol(m, v.a);
                if (!sid) continue;

                MutInfo& info = r.by_symbol[*sid];
                info.ever_written = true;

                if (!info.declared_mut) {
                    info.illegal_write = true;
                    report_illegal(v.span, *sid, "assignment");
                }
                continue;
            }

            if (v.kind == ValueKind::kPostfixInc) {
                // v.a = place
                auto sid = root_written_symbol(m, v.a);
                if (!sid) continue;

                MutInfo& info = r.by_symbol[*sid];
                info.ever_written = true;

                if (!info.declared_mut) {
                    info.illegal_write = true;
                    report_illegal(v.span, *sid, "postfix++");
                }
                continue;
            }
        }

        return r;
    }

} // namespace gaupel::sir