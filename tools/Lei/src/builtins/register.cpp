#include <lei/builtins/Register.hpp>

namespace lei::builtins::detail {

void register_constant_values(eval::BuiltinRegistry& reg);
void register_core_functions(eval::BuiltinRegistry& reg);
void register_string_functions(eval::BuiltinRegistry& reg);
void register_array_object_functions(eval::BuiltinRegistry& reg);
void register_path_fs_functions(eval::BuiltinRegistry& reg);
void register_semver_functions(eval::BuiltinRegistry& reg);
void register_parus_helper_functions(eval::BuiltinRegistry& reg);

} // namespace lei::builtins::detail

namespace lei::builtins {

void register_builtin_constants(eval::BuiltinRegistry& reg) {
    detail::register_constant_values(reg);
}

void register_builtin_functions(eval::BuiltinRegistry& reg) {
    detail::register_core_functions(reg);
    detail::register_string_functions(reg);
    detail::register_array_object_functions(reg);
    detail::register_path_fs_functions(reg);
    detail::register_semver_functions(reg);
    detail::register_parus_helper_functions(reg);
}

} // namespace lei::builtins

