// compiler/include/gaupel/diag/Diagnostic.hpp
#pragma once
#include <gaupel/text/Span.hpp>
#include <gaupel/diag/DiagCode.hpp>

#include <string>
#include <string_view>
#include <vector>


namespace gaupel::diag {

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
        void add(Diagnostic d) {  diags_.push_back(std::move(d));  }

        bool has_error() const {
            for (const auto& d : diags_) {
                if (d.severity() == Severity::kError) return true;
            }

            return false;
        }

        const std::vector<Diagnostic>& diags() const {  return diags_;  }

    private:
        std::vector<Diagnostic> diags_;
    };

} // namespace gaupel::diag
