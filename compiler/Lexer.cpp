#include "core/Tokens.cpp"
#include "core/LangSpec.cpp"
#include "utils/Special.cpp"
#include "utils/Debugs.cpp"
#include "core/Constants.cpp"
#include "vector"
#include <winnt.h>
#pragma once

struct Lexer final {

    bool debug_mode;

    // Переменные для хранения текущего состояния лексера
    int pos;    // текущая позиция в строке (индекс символа)
    int index;  // текущий индекс в строке (сбрасывается при переносе строки)
    int line;   // текущий номер строки (начинается с 0)

    Lexer(bool debug_mode) noexcept : pos(0), index(0), line(0), debug_mode(debug_mode) {}

    // Методы для управления состоянием лексера
    inline void reset() noexcept    {  pos = 0; index = 0; line = 0; }
    inline void next() noexcept     {  pos++;   index++;   }  // переход к следующему символу
    inline void before() noexcept   {  pos--;   index--;   }  // возврат к предыдущему символу

    // Функция для получения символа по смещению от текущей позиции
    inline char32_t get(const String& file_data, const int offset = 0) const noexcept {
        return file_data[pos + offset];
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////
    /// Парсинг литерала (идентификатора или ключевого слова)
    /// Считывает последовательность букв, цифр или подчёркиваний.
    /// Если значение входит в список ключевых слов или модификаторов, возвращает тип KEYWORD,
    /// иначе возвращает тип LITERAL.
    //////////////////////////////////////////////////////////////////////////////////////////////////
    Token parseLiteral(const String& file_data, const String& file_path) noexcept {
        String value;
        int start_line = line;
        int start_index = index;

        // Считываем символы, пока они являются буквами, цифрами или подчёркиванием
        while (inString(ALPHABET, get(file_data)) || inString(NUMBERS, get(file_data)) || (get(file_data) == '_')) {
            value += get(file_data);
            next();
        }
        TypeOfToken type = TypeOfToken::LITERAL;
        if (inVector(Literals::KEYWORDS, value))
            type = TypeOfToken::KEYWORD;
        if (inVector(Literals::MODIFIERS, value))
            type = TypeOfToken::KEYWORD;

        return Token(
            type, value, {file_path, start_line, start_index, int(value.size())}
        );
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////
    /// Парсинг числа
    /// Считывает последовательность цифр и, возможно, десятичной точки.
    /// Не обрабатывает знак числа или суффиксы (например, 'f' для float).
    //////////////////////////////////////////////////////////////////////////////////////////////////
    Token parseNumber(const String& file_data, const String& file_path) noexcept {
        String value;
        int start_line = line;
        int start_index = index;

        // Считываем цифры и десятичную точку
        while (inString(NUMBERS, get(file_data)) || (get(file_data) == '.')) {
            value += get(file_data);
            next();
        }

        // Префиксные форматы: 0x/0X (шестнадцатеричный), 0b/0B (двоичный).
        bool allZeros = (value.count('.') == 0);
        if (allZeros) {
            for (char32_t ch : value) {
                if (ch != '0') { allZeros = false; break; }
            }
        }
        if (allZeros) {
            if (get(file_data) == 'x' || get(file_data) == 'X') {
                value += get(file_data);
                next();
                auto isHexDigit = [](char32_t ch) -> bool {
                    return inString(NUMBERS, ch)
                        || (ch >= U'a' && ch <= U'f') || (ch >= U'A' && ch <= U'F');
                };
                while (isHexDigit(get(file_data))) {
                    value += get(file_data);
                    next();
                }
            } else if (get(file_data) == 'b' || get(file_data) == 'B') {
                value += get(file_data);
                next();
                while (get(file_data) == U'0' || get(file_data) == U'1') {
                    value += get(file_data);
                    next();
                }
            }
        }
        TypeOfToken type = TypeOfToken::NUMBER;

        return Token(
            type, value, {file_path, start_line, start_index, int(value.size())}
        );
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////
    /// Парсинг строки
    /// Считывает строку, заключённую в кавычки (одинарные или двойные).
    /// Не обрабатывает экранирующие последовательности.
    //////////////////////////////////////////////////////////////////////////////////////////////////
    Token parseString(const String& file_data, const String& file_path) noexcept {
        String value;
        int start_line = line;
        int start_index = index;

        char32_t start = get(file_data);  // запоминаем символ кавычки
        next();
        // Считываем символы до закрывающей кавычки, декодируя escape-последовательности
        // (\n, \t, \r, \\, \", \'); незнакомые оставляем как есть (обратный слэш + символ).
        while (pos < file_data.size() && get(file_data) != start) {
            char32_t ch = get(file_data);
            if (ch == U'\\' && pos + 1 < file_data.size()) {
                char32_t esc = get(file_data, 1);
                char32_t dec = 0;
                if (esc == U'n') dec = U'\n';
                else if (esc == U't') dec = U'\t';
                else if (esc == U'r') dec = U'\r';
                else if (esc == U'\\') dec = U'\\';
                else if (esc == U'"') dec = U'"';
                else if (esc == U'\'') dec = U'\'';
                else { value += U'\\'; dec = esc; }
                value += dec;
                next();
                next();
            } else {
                value += ch;
                next();
            }
        }
        if (pos < file_data.size()) next();  // пропускаем закрывающую кавычку
        TypeOfToken type = TypeOfToken::STRING;

        return Token(
            type, value, {file_path, start_line, start_index, int(value.size()) + 2}
        );
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////
    /// Парсинг оператора
    /// Считывает один символ оператора и присваивает ему тип OPERATOR.
    /// Многосимвольные операторы (например, "&&") не обрабатываются на этом этапе.
    //////////////////////////////////////////////////////////////////////////////////////////////////
    Token parseOperator(const String& file_data, const String& file_path) noexcept {
        int start_line = line;
        int start_index = index;
        if (get(file_data) == '.' && get(file_data, 1) == '.' && get(file_data, 2) == '.') {
            next(); next(); next();
            return Token(
                TypeOfToken::VA_ARGS, String("..."), {file_path, start_line, start_index, 3}
            );
        }
        char32_t value = get(file_data);
        next();

        TypeOfToken type = TypeOfToken::OPERATOR;

        return Token(
            type, String(value), {file_path, start_line, start_index, 1}
        );
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////
    /// Парсинг скобки
    /// Определяет тип скобки (круглая, квадратная, фигурная, угловая) и присваивает
    /// соответствующий токен (BRACE_LEFT, BRACE_RIGHT и т.д.).
    //////////////////////////////////////////////////////////////////////////////////////////////////
    Token parseBrace(const String& file_data, const String& file_path) noexcept {
        int start_line = line;
        int start_index = index;
        char32_t value = get(file_data);
        next();

        TypeOfToken type;
        if (value == '(') type = TypeOfToken::BRACE_LEFT;
        if (value == ')') type = TypeOfToken::BRACE_RIGHT;
        if (value == '[') type = TypeOfToken::BRACE_LEFT_RECT;
        if (value == ']') type = TypeOfToken::BRACE_RIGHT_RECT;
        if (value == '{') type = TypeOfToken::BRACE_LEFT_CURVE;
        if (value == '}') type = TypeOfToken::BRACE_RIGHT_CURVE;
        if (value == '<') type = TypeOfToken::BRACE_LEFT_TRIANGLE;
        if (value == '>') type = TypeOfToken::BRACE_RIGHT_TRIANGLE;

        return Token(
            type, String(value), {file_path, start_line, start_index, 1}
        );
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////
    /// Парсинг однострочного комментария
    /// Считывает все символы от "//" до конца строки (символа '\n').
    //////////////////////////////////////////////////////////////////////////////////////////////////
    Token parseOneLineComment(const String& file_data, const String& file_path) noexcept {
        String value;
        int start_line = line;
        int start_index = index;

        // Пропускаем два символа "/"
        next();
        next();

        // Считываем символы до конца строки
        while (pos < file_data.size() && get(file_data) != '\n') {
            value += get(file_data);
            next();
        }
        TypeOfToken type = TypeOfToken::COMMENT;

        return Token(
            type, value, {file_path, start_line, start_index, int(value.size())}
        );
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////
    /// Парсинг многострочного комментария
    /// Считывает все символы от "*/" до соответствующей закрывающей последовательности "*/".
    /// Обновляет номер строки и индекс при обнаружении переноса строки.
    //////////////////////////////////////////////////////////////////////////////////////////////////
    Token parseMultiLineComment(const String& file_data, const String& file_path) noexcept {
        String value;
        int start_line = line;
        int start_index = index;

        // Пропускаем два символа "/*"
        next();
        next();

        // Считываем символы, пока не встретим закрывающую последовательность "*/"
        while (pos < file_data.size() && !(get(file_data) == '*' && pos + 1 < file_data.size() && get(file_data, 1) == '/')) {
            if (get(file_data) == '\n') {
                line++;
                index = 0;
            }
            value += get(file_data);
            next();
        }

        // Пропускаем закрывающую последовательность "*/"
        if (pos < file_data.size()) next();
        if (pos < file_data.size()) next();

        TypeOfToken type = TypeOfToken::MULTI_COMMENT;

        return Token(
            type, value, {file_path, start_line, start_index, int(value.size())}
        );
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////
    /// Парсинг директивы
    /// Считывает имя директивы, начинающееся с '#', за которым следуют буквы или подчёркивания.
    /// Не обрабатывает аргументы директивы.
    //////////////////////////////////////////////////////////////////////////////////////////////////
    Token parseDirective(const String& file_data, const String& file_path) noexcept {
        int start_line = line;
        int start_index = index;

        // Пропускаем символ '#'
        next();

        String value;
        // Считываем символы, пока они являются буквами или подчёркиванием
        while (pos < file_data.size() && (inString(ALPHABET, get(file_data)) || get(file_data) == '_')) {
            value += get(file_data);
            next();
        }

        TypeOfToken type = TypeOfToken::DIRECTIVE;

        return Token(
            type, value, {file_path, start_line, start_index, int(value.size()) + 1}
        );
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////
    /// Парсинг токенов (главный метод лексического анализа)
    /// Проходит по всем символам входной строки, классифицирует их и создаёт список токенов.
    /// Обрабатывает пробелы, переносы строки, литералы, числа, строки, комментарии,
    /// операторы, скобки и директивы.
    //////////////////////////////////////////////////////////////////////////////////////////////////
    std::vector<Token> operator()(const String& file_data, const String& file_path, const bool without_eof = false) {
        std::vector<Token> token_list;
        const size_t data_size = file_data.size();

        while (static_cast<size_t>(pos) < data_size) {
            auto c = get(file_data);

            // --- Обработка пробелов и табуляции ---
            if (c == ' ' || c == '\t') {
                next();
                continue;
            }

            // --- Обработка переноса строки ---
            if (c == '\n') {
                int start_line = line;
                int start_index = index;
                next();
                line++;
                index = 0;
                continue;
            }

            // --- Обработка символа возврата каретки ---
            if (c == '\r') {
                next();
                continue;
            }

            // --- Парсинг литерала (идентификатора или ключевого слова) ---
            if (inString(ALPHABET, c) || (c == '_')) {
                token_list.push_back(parseLiteral(file_data, file_path));
                continue;
            }

            // --- Парсинг числа (включая числа с десятичной точкой) ---
            if (inString(NUMBERS, c) || (c == '.' && inString(NUMBERS, get(file_data, 1)))) {
                token_list.push_back(parseNumber(file_data, file_path));
                continue;
            }

            // --- Парсинг директивы (начинается с '#') ---
            if (c == '#' && inString(ALPHABET, get(file_data, 1))) {
                token_list.push_back(parseDirective(file_data, file_path));
                continue;
            }

            // --- Парсинг однострочного комментария (начинается с "//") ---
            if (c == '/' && get(file_data, 1) == '/') {
                token_list.push_back(parseOneLineComment(file_data, file_path));
                continue;
            }

            // --- Парсинг многострочного комментария (начинается с "/*") ---
            if (c == '/' && get(file_data, 1) == '*') {
                token_list.push_back(parseMultiLineComment(file_data, file_path));
                continue;
            }

            // --- Парсинг оператора ---
            if (inString(OPERATORS, c)) {
                token_list.push_back(parseOperator(file_data, file_path));
                continue;
            }

            // --- Парсинг строки (заключённой в кавычки) ---
            if (c == '\"' || c == '\'') {
                token_list.push_back(parseString(file_data, file_path));
                continue;
            }

            // --- Парсинг скобки ---
            if (inString(SCOPES, c)) {
                token_list.push_back(parseBrace(file_data, file_path));
                continue;
            }

            // --- Неизвестный символ: пропускаем его ---
            next();
        }

        if (!without_eof) {
            if (token_list.size() != 0) {
                auto end_token_position = token_list.back().position;
                token_list.push_back(Token(TypeOfToken::END_OF_FILE, String("EOF"),
                    {file_path, end_token_position.line, end_token_position.index + end_token_position.size - 1, 1}));
            } else {
                token_list.push_back(Token(TypeOfToken::END_OF_FILE, String("EOF"),
                    {file_path, 1, 0, 1}));
            }
        }

        return token_list;
    }
};

// Функция для быстрого лексического анализа
std::vector<Token> fastLex(const std::string& file_data, const std::string& file_path, const bool without_eof = false, const bool debug_mode = false) noexcept {
    Lexer lexer(debug_mode);
    return lexer(file_data, file_path, without_eof);
}

const void addEndOfFile(std::vector<Token>& token_list, const String& file_path) noexcept {
    if (token_list.size() != 0) {
        auto end_token_position = token_list.back().position;
        token_list.push_back(Token(TypeOfToken::END_OF_FILE, String("EOF"),
            {file_path, end_token_position.line, end_token_position.index + end_token_position.size - 1, 1}));
    } else {
        token_list.push_back(Token(TypeOfToken::END_OF_FILE, String("EOF"),
            {file_path, 1, 0, 1}));
    }
}
