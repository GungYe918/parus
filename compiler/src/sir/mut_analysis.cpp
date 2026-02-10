// compiler/src/sir/mut_analysis.cpp
#include <gaupel/sir/MutAnalysis.hpp>
#include <gaupel/diag/DiagCode.hpp>

namespace gaupel::sir {

    static bool is_local_place(const Value& v) {
        return v.place == PlaceClass::kLocal && v.sym != k_invalid_symbol;
    }

    MutAnalysisResult analyze_mut(const Module& m, diag::Bag& bag) {
        MutAnalysisResult out{};

        // 1) decl 스캔: VarDecl로 declared_mut/is_set 채우기
        for (const auto& st : m.stmts) {
            if (st.kind != StmtKind::kVarDecl) continue;
            if (st.sym == k_invalid_symbol) continue;

            auto& mi = out.by_symbol[st.sym];
            mi.declared_mut = st.is_mut;
            mi.is_set = st.is_set;

            // v0 정책: set은 mutable binding으로 취급
            if (st.is_set) mi.declared_mut = true;
        }

        // 2) write 스캔: values에서 Assign/PostfixInc
        for (const auto& v : m.values) {
            if (v.kind == ValueKind::kAssign) {
                // v.a = place, v.b = value
                if (v.a == k_invalid_value) continue;
                const auto& place = m.values[v.a];

                if (is_local_place(place)) {
                    auto& mi = out.by_symbol[place.sym];
                    mi.ever_written = true;

                    if (!mi.declared_mut) {
                        mi.illegal_write = true;

                        // diag code는 프로젝트에 맞춰 추가/연결해줘야 함.
                        // 아래는 “자리만” 잡아둔 형태.
                        // diag::Diagnostic d(diag::Severity::kError, diag::Code::kMutWriteToImmutable, v.span);
                        // d.add_arg(place.text);
                        // bag.add(std::move(d));
                    }
                } else {
                    // index write 등: v0에서는 허용(추후 borrow/alias에서 다룸)
                }
            }

            if (v.kind == ValueKind::kPostfixInc) {
                if (v.a == k_invalid_value) continue;
                const auto& place = m.values[v.a];

                if (is_local_place(place)) {
                    auto& mi = out.by_symbol[place.sym];
                    mi.ever_written = true;

                    if (!mi.declared_mut) {
                        mi.illegal_write = true;
                        // 위와 동일하게 diag 연결
                    }
                }
            }
        }

        return out;
    }

} // namespace gaupel::sir