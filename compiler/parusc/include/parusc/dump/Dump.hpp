// compiler/parusc/include/parusc/dump/Dump.hpp
#pragma once

#include <parus/ast/Nodes.hpp>
#include <parus/lex/Token.hpp>
#include <parus/oir/OIR.hpp>
#include <parus/sir/SIR.hpp>
#include <parus/ty/TypePool.hpp>

#include <vector>

namespace parusc::dump {

    /// @brief 토큰 목록을 콘솔에 출력한다.
    void dump_tokens(const std::vector<parus::Token>& tokens);

    /// @brief AST expression 트리를 출력한다.
    void dump_expr(const parus::ast::AstArena& ast, parus::ast::ExprId id, int indent);

    /// @brief AST statement 트리를 출력한다.
    void dump_stmt(const parus::ast::AstArena& ast, const parus::ty::TypePool& types, parus::ast::StmtId id, int indent);

    /// @brief SIR 모듈 전체를 사람이 읽기 쉬운 형태로 출력한다.
    void dump_sir_module(const parus::sir::Module& m, const parus::ty::TypePool& types);

    /// @brief OIR 모듈 전체를 사람이 읽기 쉬운 형태로 출력한다.
    void dump_oir_module(const parus::oir::Module& m, const parus::ty::TypePool& types);

} // namespace parusc::dump
