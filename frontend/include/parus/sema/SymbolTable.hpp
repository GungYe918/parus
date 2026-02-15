// frontend/include/parus/sema/SymbolTable.hpp
#pragma once
#include <parus/text/Span.hpp>
#include <parus/ty/Type.hpp>

#include <cstdint>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional> 


namespace parus::sema {

    // 심볼 종류(확장 대비)
    enum class SymbolKind : uint8_t {
        kVar,     // 변수(let/set, 파라미터 포함)
        kFn,      // 함수
        kType,    // 타입 이름(class/struct/alias 등)
        kField,   // field 이름(향후)
        kAct,     // acts 이름(향후)
    };

    // 심볼 1개 엔트리
    struct Symbol {
        SymbolKind kind = SymbolKind::kVar;

        std::string_view name{};
        ty::TypeId declared_type = ty::kInvalidType; // 선언 타입(없으면 invalid)

        Span decl_span{}; // 선언 지점
        uint32_t owner_scope = 0; // 소속 스코프 id (디버그/정책용)
    };

    // shadowing 기록(경고/에러 정책을 옵션으로 나중에 결정)
    struct Shadowing {
        uint32_t old_symbol = 0;
        uint32_t new_symbol = 0;
        Span span{}; // 새 선언 span
    };

    // unordered_map for string_view
    struct SvHash {
        size_t operator()(std::string_view s) const noexcept {
            // FNV-1a (간단)
            size_t h = 1469598103934665603ull;
            for (unsigned char c : s) {
                h ^= (size_t)c;
                h *= 1099511628211ull;
            }
            return h;
        }
    };
    struct SvEq {
        bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
    };

    struct Scope {
        uint32_t parent = 0xFFFF'FFFFu;
        std::unordered_map<std::string_view, uint32_t, SvHash, SvEq> table;
    };

    // 심볼 테이블: 스코프 스택 + 심볼 저장소
    class SymbolTable {
    public:
        SymbolTable() {
            scopes_.reserve(64);
            symbols_.reserve(256);
            shadowings_.reserve(64);

            // [0] 글로벌 스코프
            Scope g{};
            g.parent = kNoScope;
            scopes_.push_back(std::move(g));
            scope_stack_.push_back(0);
        }

        static constexpr uint32_t kNoScope = 0xFFFF'FFFFu;

        // 현재 스코프 id
        uint32_t current_scope() const { return scope_stack_.empty() ? 0 : scope_stack_.back(); }

        // 스코프 push
        uint32_t push_scope() {
            Scope s{};
            s.parent = current_scope();
            scopes_.push_back(std::move(s));
            uint32_t id = (uint32_t)scopes_.size() - 1;
            scope_stack_.push_back(id);
            return id;
        }

        // 스코프 pop (글로벌은 pop 금지)
        void pop_scope() {
            if (scope_stack_.size() <= 1) return;
            scope_stack_.pop_back();
        }

        // 심볼 조회(현재 스코프 체인)
        // 찾으면 symbol id, 아니면 nullopt
        std::optional<uint32_t> lookup(std::string_view name) const {
            uint32_t s = current_scope();
            while (s != kNoScope) {
                const auto& m = scopes_[s].table;
                auto it = m.find(name);
                if (it != m.end()) return it->second;
                s = scopes_[s].parent;
            }
            return std::nullopt;
        }

        // 같은 스코프 내 중복 여부(duplicate 체크용)
        std::optional<uint32_t> lookup_in_current(std::string_view name) const {
            const auto& m = scopes_[current_scope()].table;
            auto it = m.find(name);
            if (it == m.end()) return std::nullopt;
            return it->second;
        }

        // 삽입:
        // - 같은 스코프에 있으면 duplicate
        // - 바깥 스코프에 있으면 shadowing 기록(허용)
        struct InsertResult {
            bool ok = false;
            bool is_duplicate = false;
            bool is_shadowing = false;
            uint32_t symbol_id = 0;
            uint32_t shadowed_symbol_id = 0;
        };

        InsertResult insert(SymbolKind kind, std::string_view name, ty::TypeId declared_type, Span decl_span) {
            InsertResult r{};

            // duplicate (same scope)
            if (auto dup = lookup_in_current(name)) {
                r.ok = false;
                r.is_duplicate = true;
                r.symbol_id = *dup;
                return r;
            }

            // shadowing (outer scopes)
            if (auto outer = lookup(name)) {
                r.is_shadowing = true;
                r.shadowed_symbol_id = *outer;
            }

            Symbol sym{};
            sym.kind = kind;
            sym.name = name;
            sym.declared_type = declared_type;
            sym.decl_span = decl_span;
            sym.owner_scope = current_scope();

            symbols_.push_back(sym);
            uint32_t sid = (uint32_t)symbols_.size() - 1;
            scopes_[current_scope()].table.emplace(name, sid);

            r.ok = true;
            r.symbol_id = sid;

            if (r.is_shadowing) {
                Shadowing sh{};
                sh.old_symbol = r.shadowed_symbol_id;
                sh.new_symbol = sid;
                sh.span = decl_span;
                shadowings_.push_back(sh);
            }
            return r;
        }

        // ----------------------------
        // for tyck / passes
        // ----------------------------

        const Symbol& symbol(uint32_t id) const { return symbols_[id]; }

        Symbol& symbol_mut(uint32_t id) { return symbols_[id]; }

        // 타입 갱신: SymbolId 기반으로 declared_type을 바꿀 수 있어야 한다.
        // (set 추론 확정, deferred integer 확정 등)
        bool update_declared_type(uint32_t id, ty::TypeId new_type) {
            if (id >= symbols_.size()) return false;
            symbols_[id].declared_type = new_type;
            return true;
        }

        const std::vector<Symbol>& symbols() const { return symbols_; }
        const std::vector<Shadowing>& shadowings() const { return shadowings_; }

    private:
        std::vector<Scope> scopes_;
        std::vector<uint32_t> scope_stack_;

        std::vector<Symbol> symbols_;
        std::vector<Shadowing> shadowings_;
    };

} // namespace parus::sema