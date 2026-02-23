#pragma once

#include <lei/eval/Evaluator.hpp>

namespace lei::builtins {

void register_builtin_constants(eval::BuiltinRegistry& reg);
void register_builtin_functions(eval::BuiltinRegistry& reg);

} // namespace lei::builtins

