// tools/gaupelc/src/dump/Dump.hpp
#pragma once

#include <gaupel/ast/Nodes.hpp>
#include <gaupel/lex/Token.hpp>
#include <gaupel/oir/OIR.hpp>
#include <gaupel/sir/SIR.hpp>
#include <gaupel/ty/TypePool.hpp>

#include <vector>

namespace gaupelc::dump {

    /// @brief 토큰 목록을 콘솔에 출력한다.
    void dump_tokens(const std::vector<gaupel::Token>& tokens);

    /// @brief AST expression 트리를 출력한다.
    void dump_expr(const gaupel::ast::AstArena& ast, gaupel::ast::ExprId id, int indent);

    /// @brief AST statement 트리를 출력한다.
    void dump_stmt(const gaupel::ast::AstArena& ast, const gaupel::ty::TypePool& types, gaupel::ast::StmtId id, int indent);

    /// @brief SIR 모듈 전체를 사람이 읽기 쉬운 형태로 출력한다.
    void dump_sir_module(const gaupel::sir::Module& m, const gaupel::ty::TypePool& types);

    /// @brief OIR 모듈 전체를 사람이 읽기 쉬운 형태로 출력한다.
    void dump_oir_module(const gaupel::oir::Module& m, const gaupel::ty::TypePool& types);

} // namespace gaupelc::dump
