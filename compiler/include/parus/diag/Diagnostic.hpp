// compiler/include/parus/diag/Diagnostic.hpp
#pragma once
#include <parus/text/Span.hpp>
#include <parus/diag/DiagCode.hpp>

#include <string>
#include <string_view>
#include <vector>


namespace parus::diag {

    class Diagnostic {
    public:
        Diagnostic(Severity severity, Code code, Span span)
            : severity_(severity), code_(code), span_(span) {}

        void add_arg(std::string_view s) {  args_.emplace_back(s);  }
        void add_arg_int(int v)          {  args_.emplace_back(std::to_string(v));  }

        Severity severity() const   {  return severity_;    }
        Code code() const           {  return code_;        }
        Span span() const           {  return span_;        }
        const std::vector<std::string>& args() const {  return args_;  }  
    
    private:
        Severity severity_{Severity::kError};
        Code code_{Code::kUnexpectedToken};
        Span span_{};
        std::vector<std::string> args_;
    };

    class Bag {
    public:
        void add(Diagnostic d) {
            if (d.severity() == Severity::kError) ++error_count_;
            if (d.severity() == Severity::kFatal) ++fatal_count_;
            diags_.push_back(std::move(d));
        }

        bool has_error() const {
            for (const auto& d : diags_) {
                if (d.severity() == Severity::kError || d.severity() == Severity::kFatal) return true;
            }
            return false;
        }

        bool has_fatal() const {
            return fatal_count_ != 0;
        }

        bool has_code(Code c) const {
            for (const auto& d : diags_) {
                if (d.code() == c) return true;
            }

            return false;
        }

        const std::vector<Diagnostic>& diags() const {  return diags_;  }

        uint32_t error_count() const {  return error_count_;  }
        uint32_t fatal_count() const {  return fatal_count_;  }

        uint32_t issue_count() const {  return error_count_ + fatal_count_;  }

    private:
        std::vector<Diagnostic> diags_;
        uint32_t error_count_ = 0;
        uint32_t fatal_count_ = 0;
    };

} // namespace parus::diag
