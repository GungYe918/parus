// compiler/include/gaupel/parse/Cursor.hpp
#pragma once
#include <gaupel/lex/Token.hpp>

#include <vector>


namespace gaupel {

    class Cursor {
    public:
        explicit Cursor(const std::vector<Token>& tokens) :tokens_(tokens) {}

        const Token& peek(size_t k = 0) const {
            size_t i = pos_ + k;
            if (i >= tokens_.size()) return tokens_.back();
            return tokens_[i];
        }

        bool at(syntax::TokenKind k) const {
            return peek().kind == k;
        }

        bool eat(syntax::TokenKind k) {
            if (at(k)) { ++pos_; return true; }
            return false;
        }

        const Token& bump() {  return tokens_[pos_++];  }

        size_t pos() const      {  return pos_;  }
        void rewind(size_t p)   {  pos_ = p;     }

    private:
        const std::vector<Token>& tokens_;
        size_t pos_ = 0;
    };  

} // namespace gaupel