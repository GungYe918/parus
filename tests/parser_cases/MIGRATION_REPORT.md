# parser_cases case* prefix migration

Renamed legacy `case*.pr` files to `ok_/warn_/err_` prefixed names.

## Rules

1. `//@expect-error` -> `err_`
2. `//@expect-warning` -> `warn_`
3. `//@expect-no-parser-error` -> `ok_`
4. fallback -> `ok_`

## Mapping

- `case10_field_ok.pr` -> `ok_case10_field_ok.pr` (fallback (legacy case* => ok_))
- `case11_acts_ok.pr` -> `ok_case11_acts_ok.pr` (fallback (legacy case* => ok_))
- `case12_cap_borrow_ok.pr` -> `ok_case12_cap_borrow_ok.pr` (fallback (legacy case* => ok_))
- `case13_cap_escape_ok.pr` -> `ok_case13_cap_escape_ok.pr` (fallback (legacy case* => ok_))
- `case14_cap_call_temp_mut_ok.pr` -> `ok_case14_cap_call_temp_mut_ok.pr` (fallback (legacy case* => ok_))
- `case15_array_unsized_decl_ok.pr` -> `ok_case15_array_unsized_decl_ok.pr` (fallback (legacy case* => ok_))
- `case16_array_sized_decl_ok.pr` -> `ok_case16_array_sized_decl_ok.pr` (fallback (legacy case* => ok_))
- `case17_slice_borrow_read_ok.pr` -> `ok_case17_slice_borrow_read_ok.pr` (fallback (legacy case* => ok_))
- `case18_slice_borrow_mut_ok.pr` -> `ok_case18_slice_borrow_mut_ok.pr` (fallback (legacy case* => ok_))
- `case19_namedgroup_slice_borrow_ok.pr` -> `ok_case19_namedgroup_slice_borrow_ok.pr` (fallback (legacy case* => ok_))
- `case1_var.pr` -> `ok_case1_var.pr` (fallback (legacy case* => ok_))
- `case20_static_escape_ok.pr` -> `ok_case20_static_escape_ok.pr` (fallback (legacy case* => ok_))
- `case21_namedgroup_array_cap_mix_ok.pr` -> `ok_case21_namedgroup_array_cap_mix_ok.pr` (fallback (legacy case* => ok_))
- `case22_escape_call_boundary_ok.pr` -> `ok_case22_escape_call_boundary_ok.pr` (fallback (legacy case* => ok_))
- `case23_do_scope_ok.pr` -> `ok_case23_do_scope_ok.pr` (fallback (legacy case* => ok_))
- `case24_do_while_ok.pr` -> `ok_case24_do_while_ok.pr` (fallback (legacy case* => ok_))
- `case25_borrow_rebind_owner_ok.pr` -> `ok_case25_borrow_rebind_owner_ok.pr` (fallback (legacy case* => ok_))
- `case26_manual_stmt_ok.pr` -> `ok_case26_manual_stmt_ok.pr` (fallback (legacy case* => ok_))
- `case27_nullable_lift_let_call_return_ok.pr` -> `ok_case27_nullable_lift_let_call_return_ok.pr` (fallback (legacy case* => ok_))
- `case28_field_optional_lift_ok.pr` -> `ok_case28_field_optional_lift_ok.pr` (fallback (legacy case* => ok_))
- `case29_acts_explicit_default_path_ok.pr` -> `ok_case29_acts_explicit_default_path_ok.pr` (fallback (legacy case* => ok_))
- `case2_fn_namedgroup.pr` -> `ok_case2_fn_namedgroup.pr` (fallback (legacy case* => ok_))
- `case30_acts_fq_owner_path_ok.pr` -> `ok_case30_acts_fq_owner_path_ok.pr` (fallback (legacy case* => ok_))
- `case31_nest_static_path_ok.pr` -> `ok_case31_nest_static_path_ok.pr` (fallback (legacy case* => ok_))
- `case3_loop.pr` -> `ok_case3_loop.pr` (fallback (legacy case* => ok_))
- `case4_if.pr` -> `ok_case4_if.pr` (fallback (legacy case* => ok_))
- `case5_while.pr` -> `ok_case5_while.pr` (fallback (legacy case* => ok_))
- `case6_cast_as.pr` -> `ok_case6_cast_as.pr` (fallback (legacy case* => ok_))
- `case7_cast_as_optional.pr` -> `ok_case7_cast_as_optional.pr` (fallback (legacy case* => ok_))
- `case8_cast_as_force.pr` -> `ok_case8_cast_as_force.pr` (fallback (legacy case* => ok_))
- `case9_null_coalesce_assign.pr` -> `ok_case9_null_coalesce_assign.pr` (fallback (legacy case* => ok_))
