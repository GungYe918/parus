#include <parus/lex/Lexer.hpp>
#include <parus/parse/Parser.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" const char* __asan_default_options() {
    return "detect_container_overflow=0:detect_leaks=0";
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr) return 0;

    std::string src(reinterpret_cast<const char*>(data), size);

    parus::ast::AstArena ast;
    parus::ty::TypePool types;
    parus::diag::Bag bag;

    parus::Lexer lx(src, /*file_id=*/1, &bag);
    const std::vector<parus::Token> toks = lx.lex_all();

    parus::Parser parser(toks, ast, types, &bag, /*max_errors=*/256);
    (void)parser.parse_program();
    return 0;
}
