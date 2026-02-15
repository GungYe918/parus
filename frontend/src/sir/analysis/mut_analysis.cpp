// frontend/src/sir/mut_analysis.cpp
#include <parus/sir/MutAnalysis.hpp>
#include <parus/diag/DiagCode.hpp>


namespace parus::sir {

    namespace {
        /// @brief 타입이 `&mut T`인지 판정한다.
        bool is_mut_borrow_type_(const ty::TypePool& types, TypeId t) {
            if (t == k_invalid_type) return false;
            if (t >= types.count()) return false;
            const auto& tt = types.get(t);
            return tt.kind == ty::Kind::kBorrow && tt.borrow_is_mut;
        }

        /// @brief 대입/증감 lhs가 `&mut` write-through 형태인지 판정한다.
        bool is_write_through_mut_borrow_lhs_(
            const Module& m,
            const ty::TypePool& types,
            ValueId lhs
        ) {
            if (lhs == k_invalid_value || lhs >= m.values.size()) return false;
            const Value& v = m.values[lhs];

            if (v.kind == ValueKind::kLocal) {
                return is_mut_borrow_type_(types, v.type);
            }

            if (v.kind == ValueKind::kIndex) {
                if (v.a == k_invalid_value || v.a >= m.values.size()) return false;
                const Value& base = m.values[v.a];
                return is_mut_borrow_type_(types, base.type);
            }

            return false;
        }
    } // namespace

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

    MutAnalysisResult analyze_mut(const Module& m, const ty::TypePool& types, diag::Bag& bag) {
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
            for (const auto& d : bag.diags()) {
                if (d.code() != diag::Code::kWriteToImmutable) continue;
                const auto ds = d.span();
                if (ds.file_id == sp.file_id && ds.lo == sp.lo && ds.hi == sp.hi) {
                    return; // 동일 위치 중복 진단 억제
                }
            }
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
                const bool write_through_borrow = is_write_through_mut_borrow_lhs_(m, types, v.a);

                MutInfo& info = r.by_symbol[*sid];
                info.ever_written = true;

                if (!info.declared_mut && !write_through_borrow) {
                    info.illegal_write = true;
                    report_illegal(v.span, *sid, "assignment");
                }
                continue;
            }

            if (v.kind == ValueKind::kPostfixInc) {
                // v.a = place
                auto sid = root_written_symbol(m, v.a);
                if (!sid) continue;
                const bool write_through_borrow = is_write_through_mut_borrow_lhs_(m, types, v.a);

                MutInfo& info = r.by_symbol[*sid];
                info.ever_written = true;

                if (!info.declared_mut && !write_through_borrow) {
                    info.illegal_write = true;
                    report_illegal(v.span, *sid, "postfix++");
                }
                continue;
            }
        }

        return r;
    }

} // namespace parus::sir
