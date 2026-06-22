#pragma once
#include <string>
#include <vector>
#include <variant>

namespace pyc::lexer {

enum class TokenType {
    NAME, INT_LITERAL, FLOAT_LITERAL, STR_LITERAL,
    IADD, ISUB, IMUL, IDIV, POW, PERCENT, LT, LE, GT, GE, EQ, NE,
    ASSIGN, COLON, COMMA, LPAREN, RPAREN, LBRACKET, RBRACKET, LBRACE, RBRACE, DOT,
    MINUS, PLUS,
    NEWLINE, DECREMENT, IN,
    DEF, CLASS, PASS, BREAK, CONTINUE,
    EOF_
};

struct Token {
    TokenType kind;
    std::string value;
    long int_val = 0;
};

std::vector<Token> tokenize(const std::string& source);

} // namespace pyc::lexer
