#include "../utils/Strings.cpp"
#include "../utils/File.cpp"


#pragma once

typedef enum TokenTypes {
    NUMBER,         // numbers like 3, 3.14, 0.343, .344
    STRING,         // strings or chars like 'test', 'f', "dddf", "df"
    LITERAL,        // literals
    OPERATOR,       // like += -- + - \ || * &&
    KEYWORD,        // like :
                    //      static, const, final, let, for, while
    DIRECTIVE,      // like :
                    //      #include, #macro, #define

    BRACE_LEFT,             // brackets
    BRACE_RIGHT,            //
    BRACE_LEFT_RECT,        //
    BRACE_RIGHT_RECT,       //
    BRACE_LEFT_CURVE,       //
    BRACE_RIGHT_CURVE,      //
    BRACE_LEFT_TRIANGLE,    //
    BRACE_RIGHT_TRIANGLE,   //

    COMMENT,                // comments
    MULTI_COMMENT,          //

    VA_ARGS,                // varargs

    END_OF_FILE,

    DUMMY
} TypeOfToken;

const std::vector<String> typeOfTokenStrings = {
    "NUMBER", "STRING", "LITERAL", "OPERATOR", "KEYWORD", "DIRECTIVE",
    "BRACE_LEFT", "BRACE_RIGHT", "BRACE_LEFT_RECT", "BRACE_RIGHT_RECT",
    "BRACE_LEFT_CURVE", "BRACE_RIGHT_CURVE", "BRACE_LEFT_TRIANGLE", "BRACE_RIGHT_TRIANGLE",
    "COMMENT", "MULTI_COMMENT", "VA_ARGS", "END_OF_FILE", "DUMMY",
};

String typeOfTokenToString(TypeOfToken type) {
    return typeOfTokenStrings[type];
}

struct PositionInFile {
    String      file_path;
    int         line, index, size;

    PositionInFile() = default;
    PositionInFile(const String& file_path, const int line, const int index, const int size) :
        file_path(file_path), line(line), index(index), size(size) {}

    inline String toString() const noexcept {
        return file_path.to_utf8() + ":" + std:: to_string(line) + ":" +
               std:: to_string(index) + ":" + std:: to_string(size);
    }
};

struct Token {
    TypeOfToken     type;
    String          value;
    PositionInFile  position;

    Token() {
        type = TypeOfToken::DUMMY;
        value = "";
        position = PositionInFile();
    };
    Token(const TypeOfToken type, const String& value, const PositionInFile& position) :
        type(type), value(value), position(position) {};

    static Token FileStartToken(const String& file_path) {
        return Token(TypeOfToken::DUMMY, "", PositionInFile(file_path, 0, 0, 0));
    }
};

void printTokens(const std::vector<Token>& tokens) noexcept {
    std:: cout << "START PRINT TOKENS" << std:: endl;
    for (auto& token: tokens) {
        std::cout << token.value << " " << token.position.toString() << " " << std::to_string(token.type) << "\n";
    }
    std:: cout << "FINISH PRINT TOKENS" << std:: endl;
}

void saveTokensVector(const std:: vector<Token>& tokens, String file_name) noexcept {
    String out;
    for (auto& token: tokens) {
        auto value = token.value;
        out += value + " " + token.position.toString() + " " + std:: to_string(token.type) + "\n";
    }
    saveFile(file_name, out);
}

// Собирает токены в строку
String stringFromTokens(const std::vector<Token>& tokens, const bool ignore_comments = false) noexcept {
    String result;
    for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i].type == TypeOfToken::END_OF_FILE) continue;

        if (i != 0 && tokens[i].position.line == tokens[i - 1].position.line && tokens[i].position.file_path == tokens[i - 1].position.file_path) {
            if (tokens[i].position.index != tokens[i - 1].position.index) {
                int offset = tokens[i].position.index - (tokens[i - 1].position.index + tokens[i - 1].position.size);
                if (offset < 0) offset = 0;
                result += String::RepeatFrom(' ', offset);
            } else {
                result += "\n";
            }
        }

        if ((tokens[i].position.line != tokens[i - 1].position.line || tokens[i].position.file_path != tokens[i - 1].position.file_path) && i != 0) result += "\n";
        if (tokens[i].type == TypeOfToken::COMMENT && !ignore_comments)  result += "//";
        if (tokens[i].type == TypeOfToken::STRING) result += "\"" + tokens[i].value + "\"";
        else if (tokens[i].type == TypeOfToken::MULTI_COMMENT && !ignore_comments) result += "/*" + tokens[i].value + "*/";
        else result += tokens[i].value;

    }
    return result;
}

std::vector<String> stringVectorFromTokens(const std::vector<Token>& tokens) noexcept {
    String res = stringFromTokens(tokens);
    return splitString(res, '\n');
}

bool isIntegerToken(const Token& token) noexcept {
    for (auto c: token.value) {
        if (c == '.') return false;
    }
    return token.type == TypeOfToken::NUMBER;
}
