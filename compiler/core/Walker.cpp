#include "Tokens.cpp"
#include "vector"
#include "Constants.cpp"
#pragma once

struct Walker {
    std:: vector<Token> tokens;
    size_t index = 0;

    explicit Walker(std:: vector<Token> tokens) noexcept
                : tokens(tokens) {}

    Walker() = default;

    // special methods
    inline void Next() noexcept { index++; }
    inline void before() noexcept { index--; }
    inline bool isOuted() const noexcept { return index >= tokens.size(); }
    inline bool isEnd() const noexcept { return tokens.empty() || index >= tokens.size() - 1; }
    inline bool isPreEnd() const noexcept { return tokens.size() < 2 || index == tokens.size() - 2; }

    inline bool isEof() const noexcept { return safeGet().type == TokenTypes::END_OF_FILE;}

    unsafe
    inline Token Get(const int offset = 0) const noexcept  {
        return tokens[index + offset];
    }

    inline Token safeGet(const int offset = 0) const noexcept {
        if (tokens.empty()) return Token(TokenTypes::END_OF_FILE, String("EOF"), PositionInFile());
        size_t idx = static_cast<size_t>(static_cast<int>(index) + offset);
        if (idx >= tokens.size())
            return Token(TokenTypes::END_OF_FILE, String("EOF"), PositionInFile());
        return Get(offset);
    }

    inline bool CheckType(const TokenTypes type, const int offset = 0) const noexcept {
        size_t idx = static_cast<size_t>(static_cast<int>(index) + offset);
        if (idx >= tokens.size()) return false;
        return tokens[idx].type == type;
    }

    inline bool CheckValue(const String& value, const int offset = 0) const noexcept {
        size_t idx = static_cast<size_t>(static_cast<int>(index) + offset);
        if (idx >= tokens.size()) return false;
        return tokens[idx].value == value;
    }

    inline bool Check(const TokenTypes type, const String& value, const int offset = 0) const noexcept {
        size_t idx = static_cast<size_t>(static_cast<int>(index) + offset);
        if (idx >= tokens.size()) return false;
        return tokens[idx].type == type && tokens[idx].value == value;
    }

    inline void passStatement() {
        while (!Check(TokenTypes::OPERATOR, ";") && !isEof()) {
            Next();
        }
    }
};
