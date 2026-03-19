    void TypeChecker::second_pass_check_program_(ast::StmtId program_stmt) {
        check_stmt_(program_stmt);

        // ----------------------------------------
        // Finalize unresolved deferred integers:
        // - If an inferred integer "{integer}" is never consumed in a way that fixes the type,
        //   we pick the smallest signed integer type that fits (i8..i128).
        // - This keeps DX friendly and avoids leaving IR in an unresolved state.
        // ----------------------------------------
        for (auto& kv : pending_int_sym_) {
            const uint32_t sym_id = kv.first;
            PendingInt& pi = kv.second;

            if (pi.resolved) continue;
            if (pi.has_value) {
                pi.resolved = true;
                pi.resolved_type = choose_smallest_signed_type_(pi.value);
                sym_.update_declared_type(sym_id, pi.resolved_type);
                continue;
            }

            const auto origin_it = pending_int_sym_origin_.find(sym_id);
            if (origin_it == pending_int_sym_origin_.end()) continue;

            ty::TypeId finalized = ty::kInvalidType;
            if (!finalize_infer_int_shape_(origin_it->second, sym_.symbol(sym_id).declared_type, finalized)) {
                continue;
            }

            pi.resolved = true;
            pi.resolved_type = finalized;
            sym_.update_declared_type(sym_id, finalized);
        }
    }

    // --------------------
    // stmt dispatch
    // --------------------

} // namespace parus::tyck
