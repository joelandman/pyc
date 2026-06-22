#include "frontend/lexer.h"
#include <cctype>
#include <algorithm>
#include <stdexcept>
#include <iostream>

namespace pyc::lexer {

const char* token_type_name(TokenType t) {
    switch (t) {
        case TokenType::NAME: return "NAME";
        case TokenType::INT_LITERAL: return "INT";
        case TokenType::FLOAT_LITERAL: return "FLOAT";
        case TokenType::STR_LITERAL: return "STR";
        case TokenType::IADD: return "IADD";
        case TokenType::ISUB: return "ISUB";
        case TokenType::IMUL: return "IMUL";
        case TokenType::IDIV: return "IDIV";
        case TokenType::POW: return "POW";
        case TokenType::PERCENT: return "PERCENT";
        case TokenType::LT: return "LT";
        case TokenType::LE: return "LE";
        case TokenType::GT: return "GT";
        case TokenType::GE: return "GE";
        case TokenType::EQ: return "EQ";
        case TokenType::NE: return "NE";
        case TokenType::ASSIGN: return "ASSIGN";
        case TokenType::COLON: return "COLON";
        case TokenType::COMMA: return "COMMA";
        case TokenType::LPAREN: return "LPAREN";
        case TokenType::RPAREN: return "RPAREN";
        case TokenType::LBRACKET: return "LBRACKET";
        case TokenType::RBRACKET: return "RBRACKET";
        case TokenType::LBRACE: return "LBRACE";
        case TokenType::RBRACE: return "RBRACE";
        case TokenType::DOT: return "DOT";
        case TokenType::MINUS: return "MINUS";
        case TokenType::PLUS: return "PLUS";
        case TokenType::NEWLINE: return "NEWLINE";
        case TokenType::DECREMENT: return "DECREMENT";
        case TokenType::DEF: return "DEF";
        case TokenType::CLASS: return "CLASS";
        case TokenType::PASS: return "PASS";
        case TokenType::BREAK: return "BREAK";
        case TokenType::CONTINUE: return "CONTINUE";
        default: return "UNKNOWN";
    }
}

static bool is_id_start(char c) { return std::isalpha(c) || c == '_'; }
static bool is_id_continue(char c) { return std::isalnum(c) || c == '_'; }

std::vector<Token> tokenize(const std::string& source) {
    std::vector<Token> tokens;
    size_t pos = 0;
    size_t len = source.size();
    bool previous_is_dedent = false;

    while (pos < len) {
        // Newline -> emit NEWLINE and track indentation
        if (source[pos] == '\n') {
            // Count indentation of next non-blank line
            size_t line_start = pos + 1;
            while (line_start < len && (source[line_start] == ' ' || source[line_start] == '\t'))
                ++line_start;
            size_t indent = line_start - (pos + 1);

             // Count current indentation
            size_t current_line_start = pos;
            // Find beginning of current line
            size_t start_of_line = pos;
            size_t j = pos;
            while (j > 0) {
                if (source[j - 1] == '\n') { start_of_line = j; break; }
                --j;
            }
            if (j == 0) start_of_line = 0;
            // start_of_line is now the newline character, so move to next char
            if (start_of_line < len && source[start_of_line] == '\n')
                start_of_line++;

      // Count spaces after last newline
            size_t line_indent_start = start_of_line;
            size_t line_indent_end = start_of_line;
            while (line_indent_end < len && (source[line_indent_end] == ' ' || source[line_indent_end] == '\t'))
                ++line_indent_end;
            size_t line_indent = line_indent_end - line_indent_start;  // number of spaces

            if (line_start >= len || source[line_start] == '\n') {
                // Blank line
                ++pos;
                continue;
            }

            if (indent > line_indent) {
                // Indent increased - no token needed (simplified)
                tokens.push_back({TokenType::NEWLINE, "\n"});
                ++pos;
                continue;
            }
            if (indent < line_indent) {
                // Dedent - emit one DECREMENT per level
                while ((int)(line_indent - indent) > 0) { // simplified
                    tokens.push_back({TokenType::DECREMENT, ""});
                    indent += 4; // rough
                }
                tokens.push_back({TokenType::NEWLINE, "\n"});
                ++pos;
                continue;
            }
            ++pos;
            continue;
        }

        // Whitespace (skip)
        if (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\r') {
            ++pos;
            continue;
        }

        // Comments
        if (source[pos] == '#') {
            while (pos < len && source[pos] != '\n') ++pos;
            continue;
        }

        // String literals
        if (source[pos] == '"' || source[pos] == '\'') {
            char delim = source[pos];
            size_t start = pos;
            ++pos;
            while (pos < len && source[pos] != delim) {
                if (source[pos] == '\\') ++pos;
                ++pos;
            }
            if (pos < len) ++pos; // closing quote
            std::string str_val = source.substr(start, pos - start);
            tokens.push_back({TokenType::STR_LITERAL, str_val});
            continue;
        }

        // Numbers
        if (std::isdigit(source[pos])) {
            size_t start = pos;
            while (pos < len && std::isdigit(source[pos])) ++pos;
            bool is_float = false;
            if (pos < len && source[pos] == '.') {
                is_float = true;
                ++pos;
                while (pos < len && std::isdigit(source[pos])) ++pos;
            }
            std::string num_str = source.substr(start, pos - start);
            Token tok;
            if (is_float) {
                tok.kind = TokenType::FLOAT_LITERAL;
                tok.value = num_str;
            } else {
                tok.kind = TokenType::INT_LITERAL;
                tok.value = num_str;
                try { tok.int_val = std::stol(num_str); } catch (...) {
                    tokens.push_back({TokenType::NAME, num_str});
                    continue;
                }
            }
            tokens.push_back(tok);
            continue;
        }

        // Names / keywords
        if (is_id_start(source[pos])) {
            size_t start = pos;
            while (pos < len && is_id_continue(source[pos])) ++pos;
            std::string kw = source.substr(start, pos - start);
            if (kw == "def") tokens.push_back({TokenType::DEF, kw});
            else if (kw == "class") tokens.push_back({TokenType::CLASS, kw});
            else if (kw == "pass") tokens.push_back({TokenType::PASS, kw});
            else if (kw == "break") tokens.push_back({TokenType::BREAK, kw});
            else if (kw == "continue") tokens.push_back({TokenType::CONTINUE, kw});
            else if (kw == "if") tokens.push_back({TokenType::NAME, kw});
            else if (kw == "else") tokens.push_back({TokenType::NAME, kw});
            else if (kw == "for") tokens.push_back({TokenType::NAME, kw});
            else if (kw == "while") tokens.push_back({TokenType::NAME, kw});
            else if (kw == "return") tokens.push_back({TokenType::NAME, kw});
            else if (kw == "or") tokens.push_back({TokenType::NAME, kw});
            else if (kw == "and") tokens.push_back({TokenType::NAME, kw});
            else if (kw == "not") tokens.push_back({TokenType::NAME, kw});
            else if (kw == "True") tokens.push_back({TokenType::NAME, kw});
            else if (kw == "False") tokens.push_back({TokenType::NAME, kw});
            else if (kw == "None") tokens.push_back({TokenType::NAME, kw});
            else if (kw == "in") tokens.push_back({TokenType::NAME, kw});
            else {
                tokens.push_back({TokenType::NAME, kw});
            }
            continue;
        }

        // Operators
        if (source[pos] == '=' && pos + 1 < len && source[pos + 1] != '=') {
            tokens.push_back({TokenType::ASSIGN, "="}); ++pos; continue;
        }
        if (source[pos] == '+' && pos + 1 < len && source[pos + 1] == '+') {
            // skip for now
            ++pos; ++pos; continue;
        }
        if (source[pos] == '+') { tokens.push_back({TokenType::IADD, "+"}); ++pos; continue; }
        if (source[pos] == '-') {
            if (pos + 1 < len && source[pos + 1] == '>') {
                tokens.push_back({TokenType::NAME, "->"}); ++pos; ++pos; continue;
            }
            // Could be start of negative number or minus
            if (pos > 0 && source[pos - 1] != ' ') {
                tokens.push_back({TokenType::MINUS, "-"});
            } else {
                tokens.push_back({TokenType::IADD, "-"});
            }
            ++pos; continue;
        }
        if (source[pos] == '*') {
            tokens.push_back({TokenType::IMUL, "*"}); ++pos; continue;
        }
        if (source[pos] == '/') {
            tokens.push_back({TokenType::IDIV, "/"}); ++pos; continue;
        }
        if (source[pos] == '%') {
            tokens.push_back({TokenType::PERCENT, "%"}); ++pos; continue;
        }
        if (source[pos] == '^') {
            tokens.push_back({TokenType::POW, "^"}); ++pos; continue;
        }
        if (source[pos] == '<') {
            if (pos + 1 < len && source[pos + 1] == '=') {
                tokens.push_back({TokenType::LE, "<="}); ++pos; ++pos; continue;
            }
            tokens.push_back({TokenType::LT, "<"}); ++pos; continue;
        }
        if (source[pos] == '>') {
            if (pos + 1 < len && source[pos + 1] == '=') {
                tokens.push_back({TokenType::GE, ">="}); ++pos; ++pos; continue;
            }
            tokens.push_back({TokenType::GT, ">"}); ++pos; continue;
        }
        if (source[pos] == '=') {
            if (pos + 1 < len && source[pos + 1] == '=') {
                tokens.push_back({TokenType::EQ, "=="}); ++pos; ++pos; continue;
            }
            tokens.push_back({TokenType::ASSIGN, "="}); ++pos; continue;
        }
        if (source[pos] == '!' && pos + 1 < len && source[pos + 1] == '=') {
            tokens.push_back({TokenType::NE, "!="}); ++pos; ++pos; continue;
        }

        // Punctuation
        switch (source[pos]) {
            case ':': tokens.push_back({TokenType::COLON, ":"}); break;
            case ',': tokens.push_back({TokenType::COMMA, ","}); break;
            case '(': tokens.push_back({TokenType::LPAREN, "("}); break;
            case ')': tokens.push_back({TokenType::RPAREN, ")"}); break;
            case '[': tokens.push_back({TokenType::LBRACKET, "["}); break;
            case ']': tokens.push_back({TokenType::RBRACKET, "]"}); break;
            case '{': tokens.push_back({TokenType::LBRACE, "{"}); break;
            case '}': tokens.push_back({TokenType::RBRACE, "}"}); break;
            case '.': tokens.push_back({TokenType::DOT, "."}); break;
            default:
                throw std::runtime_error(std::string("Unexpected char: ") + source[pos]);
        }
        ++pos;
    }

    tokens.push_back({TokenType::EOF_, ""});
    return tokens;
}

} // namespace pyc::lexer
