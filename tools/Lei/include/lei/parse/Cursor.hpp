#pragma once

#include <lei/syntax/TokenKind.hpp>

#include <cstddef>
#include <vector>

namespace lei::parse {

class Cursor {
public:
    explicit Cursor(const std::vector<syntax::Token>& tokens) : tokens_(tokens) {}

    const syntax::Token& peek(std::size_t k = 0) const {
        const std::size_t i = pos_ + k;
        if (i >= tokens_.size()) return tokens_.back();
        return tokens_[i];
    }

    bool at(syntax::TokenKind k) const {
        return peek().kind == k;
    }

    bool eat(syntax::TokenKind k) {
        if (!at(k)) return false;
        ++pos_;
        return true;
    }

    const syntax::Token& prev() const {
        if (pos_ == 0) return peek();
        const std::size_t i = pos_ - 1;
        if (i >= tokens_.size()) return tokens_.back();
        return tokens_[i];
    }

    const syntax::Token& bump() {
        if (pos_ >= tokens_.size()) return tokens_.back();
        return tokens_[pos_++];
    }

    std::size_t pos() const { return pos_; }
    void rewind(std::size_t p) { pos_ = p; }

private:
    const std::vector<syntax::Token>& tokens_;
    std::size_t pos_ = 0;
};

} // namespace lei::parse
