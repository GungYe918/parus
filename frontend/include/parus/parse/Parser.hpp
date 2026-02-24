// frontend/include/parus/parse/Parser.hpp
#pragma once
#include <parus/parse/Cursor.hpp>
#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/ty/TypePool.hpp>

#include <vector>
#include <utility>
#include <unordered_set>
#include <string_view>


namespace parus {

    struct ParserFeatureFlags {
        bool macro_with_token = false;
    };

    class Parser {
    public:
        Parser(const std::vector<Token>& tokens,
               ast::AstArena& ast,
               ty::TypePool& types,
               diag::Bag* diags = nullptr,
               uint32_t max_errors = 64,
               ParserFeatureFlags feature_flags = {})
            : cursor_(tokens), ast_(ast), types_(types), diags_(diags), max_errors_(max_errors), parser_features_(feature_flags) {

            // Lexer 단계에서 UTF-8 fatal이 발생한 경우, 파싱은 즉시 중단 상태로 취급
            if (diags_ && diags_->has_code(diag::Code::kInvalidUtf8)) {
                lexer_fatal_ = true;
                aborted_ = true;
            }
        }

        // 최상위 표현식 1개를 파싱
        ast::ExprId parse_expr();
        // 표현식 1개를 파싱하고 입력의 끝까지 소비되지 않으면 error expr를 반환
        ast::ExprId parse_expr_full();
        // macro/type expander 전용: 타입 1개를 파싱하고 입력 끝까지 확인한다.
        ast::TypeNodeId parse_type_full_for_macro(ty::TypeId* out_type = nullptr);

        // stmt/decl 혼용 문장 1개를 파싱
        ast::StmtId parse_stmt();

        // EOF까지 stmt/decl을 반복 파싱하여 프로그램(Block) 노드 생성
        ast::StmtId parse_program();

        const ParserFeatureFlags& feature_flags() const { return parser_features_; }

    private:

        enum class BlockTailPolicy : uint8_t {
            kAllowEmptyTail,   // 일반 block expr: tail 없어도 됨
            kRequireValueTail, // if-expr branch 등: tail 값 필요
        };

        // --------------------
        // diag & small helpers
        // --------------------

        //  진단을 기록 (중복 스팸 방지, max-errors 처리 포함)
        void diag_report(diag::Code code, Span span, std::string_view a0 = {});
        /// @brief 경고 진단을 기록한다(파싱 오류 카운트에는 반영하지 않음).
        void diag_report_warn(diag::Code code, Span span, std::string_view a0 = {});

        //  정수 인자를 포함하는 진단을 기록
        void diag_report_int(diag::Code code, Span span, int v0);

        //  토큰 1개를 기대하고 소비, 실패 시 진단
        bool diag_expect(syntax::TokenKind k);

        //  중단 상태인지 확인
        bool is_aborted() const { return aborted_; }

        //  현재 토큰이 "decl 시작"인지 판정 (v0: @attr, export/extern, def, field, acts)
        bool is_decl_start(syntax::TokenKind k) const;
        bool is_context_keyword(const Token& t, std::string_view kw) const;
        bool is_macro_decl_start() const;

        bool is_unambiguous_stmt_start(syntax::TokenKind k) const;

        static bool is_expr_with_block_kind(ast::ExprKind k);

        std::pair<uint32_t, uint32_t> parse_path_segments(bool allow_leading_coloncolon = false); // returns (begin,count) in arena.path_segs
        
        // --------------------
        // expr
        // --------------------

        //  Pratt 파서 본체
        ast::ExprId parse_expr_pratt(int min_prec, int ternary_depth);

        //  prefix(unary 포함) 파싱
        ast::ExprId parse_expr_prefix(int ternary_depth);

        //  primary(리터럴/식별자/괄호 등) 파싱
        ast::ExprId parse_expr_primary(int ternary_depth);

        /// @brief 배열 리터럴(`[e0, e1, ...]`)을 파싱한다.
        ast::ExprId parse_expr_array_lit(int ternary_depth);

        //  postfix(call/index/++) 연속 파싱
        ast::ExprId parse_expr_postfix(ast::ExprId base, int ternary_depth);

        //  call 파싱
        ast::ExprId parse_expr_call(ast::ExprId callee, const Token& lparen_tok, int ternary_depth);

        //  index 파싱
        ast::ExprId parse_expr_index(ast::ExprId base, const Token& lbracket_tok, int ternary_depth);

        ast::ExprId parse_expr_if(int ternary_depth);

        // ast::ExprId parse_expr_block(int ternary_depth);

        ast::ExprId parse_expr_block(int ternary_depth, BlockTailPolicy policy);

        // loop expr 파싱
        ast::ExprId parse_expr_loop(int ternary_depth);

        ast::ExprId parse_use_literal_expr_or_error();

        // --------------------
        // type
        // --------------------

        struct ParsedType {
            ast::TypeNodeId node = ast::k_invalid_type_node;
            ty::TypeId id = ty::kInvalidType;
            Span span{};
        };

        //  타입 파싱(v0: NamedType).
        ParsedType parse_type();

        // --------------------
        // stmt
        // --------------------

        //  stmt/decl 혼용의 내부 엔트리
        ast::StmtId parse_stmt_any();

        //  expr ';' 문장 파싱
        ast::StmtId parse_stmt_expr();

        //  '{ ... }' 블록 파싱
        ast::StmtId parse_stmt_block(bool allow_macro_decl = false);

        //  let/set 변수 선언 파싱
        ast::StmtId parse_stmt_var();

        //  if/elif/else 파싱
        ast::StmtId parse_stmt_if();

        //  while 파싱
        ast::StmtId parse_stmt_while();
        //  do / do-while 파싱
        ast::StmtId parse_stmt_do();
        // manual[perm,...] { ... } 파싱
        ast::StmtId parse_stmt_manual();

        //  return 파싱
        ast::StmtId parse_stmt_return();

        //  break 파싱
        ast::StmtId parse_stmt_break();

        //  continue 파싱
        ast::StmtId parse_stmt_continue();

        // switch 파싱
        ast::StmtId parse_stmt_switch();

        // use stmt 파싱
        ast::StmtId parse_stmt_use();
        ast::StmtId parse_stmt_import();

        //  if/while/def에서 블록이 필수일 때
        ast::StmtId parse_stmt_required_block(std::string_view ctx);

        // --------------------
        // decl
        // --------------------

        //  decl 엔트리(현재는 def decl만 지원)
        ast::StmtId parse_decl_any();

        //  함수 선언(스펙 6.1)을 파싱
        ast::StmtId parse_decl_fn();
        ast::StmtId parse_decl_extern_var();

        //  field 선언 파싱
        ast::StmtId parse_decl_field();

        //  acts 선언 파싱 (v0: acts A, acts for T, acts Name for T)
        ast::StmtId parse_decl_acts();
        ast::StmtId parse_decl_acts_operator(ast::TypeId owner_type, bool allow_operator);

        // use구문 파싱
        ast::StmtId parse_decl_use();
        ast::StmtId parse_decl_import();
        ast::StmtId parse_decl_nest();
        bool parse_decl_macro();

        //  '@attr' 리스트를 파싱하여 arena에 저장
        std::pair<uint32_t, uint32_t> parse_decl_fn_attr_list();

        //  함수 파라미터 목록을 파싱 (positional + optional named-group)
        void parse_decl_fn_params(uint32_t& out_param_begin,
                                  uint32_t& out_param_count,
                                  uint32_t& out_positional_count,
                                  bool& out_has_named_group);

        //  파라미터 1개(Ident ':' Type ['=' Expr])를 파싱
        //  반환값: 성공 여부(이름/타입까지 정상 파싱되었는지)
        bool parse_decl_fn_one_param(bool is_named_group, std::string_view* out_name, bool* out_is_self = nullptr);

        // --------------------
        // call args
        // --------------------

        //  일반 인자(positional or labeled)를 파싱
        ast::Arg parse_call_arg(int ternary_depth);
        ast::ExprId parse_macro_call_expr();
        bool parse_macro_call_path(uint32_t& out_path_begin, uint32_t& out_path_count, Span& out_span);
        std::pair<uint32_t, uint32_t> parse_macro_call_arg_tokens();

        // --------------------
        // recovery & misc
        // --------------------

        //  Span 두 개를 결합
        Span span_join(Span a, Span b) const;

        //  ';'를 요구하되 없으면 stmt 경계까지 recovery
        Span stmt_consume_semicolon_or_recover(Span fallback_end);

        //  현재 위치에서 stmt 경계(';', '}', EOF)까지 스킵
        void stmt_sync_to_boundary();

        //  구분자(, ) } 등)까지 중첩을 고려해 스킵
        void recover_to_delim(syntax::TokenKind stop0,
                              syntax::TokenKind stop1 = syntax::TokenKind::kError,
                              syntax::TokenKind stop2 = syntax::TokenKind::kError);

        Cursor cursor_;
        ast::AstArena& ast_;
        ty::TypePool& types_; // <-- add

        diag::Bag* diags_ = nullptr;

        uint32_t last_diag_lo_ = 0xFFFFFFFFu;
        diag::Code last_diag_code_ = diag::Code::kUnexpectedToken;
        std::unordered_set<uint64_t> seen_diag_keys_{};
        uint32_t parse_error_count_ = 0;
        static constexpr uint32_t kMaxParseErrors = 1024;

        uint32_t max_errors_ = 64;
        bool lexer_fatal_ = false;
        bool aborted_ = false;
        bool too_many_errors_emitted_ = false;
        bool seen_file_nest_directive_ = false;
        uint32_t macro_scope_depth_ = 0;
        ParserFeatureFlags parser_features_{};
    };

} // namespace parus
