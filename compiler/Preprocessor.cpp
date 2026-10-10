#include "Lexer.cpp"
#include "core/Tokens.cpp"
#include "core/Walker.cpp"
#include "core/ErrorBase.cpp"
#include "utils/Debugs.cpp"

#include <memory>
#include <unordered_set>
#include <Windows.h>

#include "core/Constants.cpp"

////////////////////////////////////////////////////////////
/// Представление аргумента макроса.
////////////////////////////////////////////////////////////
struct MacroArg final {
    Token token = Token();
    int variadic_count = 0;

    MacroArg() = default;
    MacroArg(Token token) : token(token) {};
    MacroArg(Token token, int variadic_count) : token(token), variadic_count(variadic_count) {};

    inline bool is_variadic() const noexcept { return variadic_count != 0; }
    inline int is_infinite_variadic() const noexcept { return variadic_count == -1; }
};

struct Macro final {
    Token name_token;
    std::vector<MacroArg> args_tokens;
    std::vector<Token> body_tokens;
};

struct Define {
    Token name_token;
    std::vector<Token> tokens;
};

struct Include {
    Token file_path_token;
};

// Версия компилятора Lumen (для предопределённого макроса __LUMEN__)
static const String LUMEN_VERSION = "0.1";

// Преобразует предопределённые макросы (__FILE__, __LINE__, __LUMEN__)
// в соответствующие токены на основе позиции исходного токена.
static inline Token builtinMacroToken(const Token& tok) noexcept {
    Token out = tok;
    if (tok.value == "__FILE__") {
        out.type = TypeOfToken::STRING;
        out.value = tok.position.file_path;
        out.position.size = (int)out.value.size() + 2;
    } else if (tok.value == "__LINE__") {
        out.type = TypeOfToken::NUMBER;
        // position.line внутри 0-based, как и в сообщениях об ошибках (+1 при выводе),
        // поэтому показываем привычный пользователю 1-based номер строки.
        out.value = std::to_string(tok.position.line + 1);
        out.position.size = (int)out.value.size();
    } else if (tok.value == "__LUMEN__") {
        out.type = TypeOfToken::STRING;
        out.value = LUMEN_VERSION;
        out.position.size = (int)LUMEN_VERSION.size() + 2;
    }
    return out;
}

////////////////////////////////////////////////////////////
/// Преобразователь (препроцессор)
////////////////////////////////////////////////////////////
struct Preprocessor {
    std::vector<Define> defines;
    std::vector<Include> includes;
    std::vector<Macro> macros;
    // Абсолютные пути C++-файлов из #cppinclude (эмитятся в сгенерированный .cpp как #include)
    std::vector<String> cpp_includes;
    // Поток токенов ДО filterComments: с комментариями главного файла и всех
    // #include-файлов. docCommentAbove() (док-комментарии fn/struct/enum) ищет по нему
    // объявление и комментарий над ним, в т.ч. для функций из включённых файлов.
    std::vector<Token> source_tokens;
    bool debug_mode;
    ErrorContainer& error_container;

    // счётчики для debug-статистики (-d)
    int includes_pulled = 0;
    int defines_seen = 0;
    int macros_seen = 0;

    // Реестр файлов с #pragma once на всю компиляцию: общий для корневого
    // препроцессора и всех вложенных (#include), поэтому файл, включённый
    // по нескольким путям, обрабатывается ровно один раз.
    std::shared_ptr<std::unordered_set<std::string>> once_files;

    Preprocessor(bool debug_mode, ErrorContainer& error_container)
        : debug_mode(debug_mode), error_container(error_container),
          once_files(std::make_shared<std::unordered_set<std::string>>()) {}

    Preprocessor(bool debug_mode, ErrorContainer& error_container,
                 std::shared_ptr<std::unordered_set<std::string>> shared_once_files)
        : debug_mode(debug_mode), error_container(error_container),
          once_files(std::move(shared_once_files)) {}

    // Reports an error by collecting it into the shared error container
    // (mirrors ASTGenerator::reportError) instead of printing and exiting.
    inline void reportError(const BaseError& err) noexcept {
        error_container.addError(err);
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////
    /// Удаляет токены комментариев (// и /* */) из потока.
    /// Каждый токен несёт абсолютную позицию, поэтому выкидывание комментариев
    /// не сдвигает координаты остальных токенов — пересчёт не нужен.
    //////////////////////////////////////////////////////////////////////////////////////////////////
    static inline std::vector<Token> filterComments(std::vector<Token> tokens) noexcept {
        std::vector<Token> filtered;
        filtered.reserve(tokens.size());
        for (const auto& t : tokens) {
            if (t.type == TypeOfToken::COMMENT || t.type == TypeOfToken::MULTI_COMMENT)
                continue;
            filtered.push_back(t);
        }
        return filtered;
    }

    static inline bool inDefines(const Token& token, const std::vector<Define>& defines) noexcept {
        for (const auto& def : defines) {
            if (def.name_token.value == token.value && token.type == TypeOfToken::LITERAL) {
                return true;
            }
        }
        return false;
    }

    static inline bool inMacro(const Token& token, const std::vector<Macro>& macros) noexcept {
        for (const auto& macro : macros) {
            if (macro.name_token.value == token.value) {
                return true;
            }
        }
        return false;
    }

    static inline std::vector<Token> getDefineTokens(const Token& token, const std::vector<Define>& defines) noexcept {
        for (const auto& def : defines) {
            if (def.name_token.value == token.value) {
                return def.tokens;
            }
        }
        return {};
    }

    static inline Macro getMacro(const Token& token, const std::vector<Macro>& macros) noexcept {
        for (const auto& mac: macros) {
            if (mac.name_token.value == token.value) {
                return mac;
            }
        }
        return {};
    }

    inline bool checkAlreadyDefinition(const Token& token) noexcept {
        for (const auto& def : defines) {
            if (def.name_token.value == token.value) {
                reportError(PREPROCESSOR_ERRORS::DEFINE_ALREADY_DEFINED(String("token is already defined: ") + token.value, token, def.name_token));
                return true;
            }
        }
        return false;
    }

    inline bool checkAlreadyMacro(const Token& token) noexcept {
        for (const auto& mac : macros) {
            if (mac.name_token.value == token.value) {
                reportError(PREPROCESSOR_ERRORS::MACRO_ALREADY_DEFINED(String("macro is already defined: ") + token.value, token, mac.name_token));
                return true;
            }
        }
        return false;
    }

    // Нормализованный ключ для реестров файлов: '\\' -> '/' и срезание ведущего
    // './', чтобы 'std/Types.lm', 'std\Types.lm' и './std/Types.lm' были одним файлом.
    static std::string normalizeIncludePath(const String& path) {
        std::string p = path.to_utf8();
        for (auto& c : p) if (c == '\\') c = '/';
        while (p.rfind("./", 0) == 0) p.erase(0, 2);
        return p;
    }

    inline bool checkAlreadyInclude(const Token& token) noexcept {
        for (const auto& include : includes) {
            if (normalizeIncludePath(include.file_path_token.value) == normalizeIncludePath(token.value)) {
                reportError(PREPROCESSOR_ERRORS::INCLUDE_ALREADY_INCLUDED(String("file is already included: ") + token.value, token, include.file_path_token));
                return true;
            }
        }
        return false;
    }

    inline bool checkSelfInclude(const Token& token) noexcept {
        if (normalizeIncludePath(token.value) == normalizeIncludePath(token.position.file_path)) {
            reportError(PREPROCESSOR_ERRORS::INCLUDE_SELF(token));
            return true;
        }
        return false;
    }

    inline bool checkFileExists(const Token& token) noexcept {
        if (!isFileFound(token.value)) {
            reportError(PREPROCESSOR_ERRORS::INCLUDE_NOT_FOUND(token));
            return false;
        }
        return true;
    }

    // Путь C++-файла для #cppinclude: нормализуем обратную косую в '/'.
    // Относительный путь трактуем от рабочей директории — так же, как сам #include
    // (существование уже проверено isFileFound). В эмитится АБСОЛЮТНЫМ: g++
    // резолвит #include относительно includer'а, а сгенерированный .cpp может
    // лежать в подкаталоге (`-o build/app`), и относительный путь его не нашёл бы
    // (`fatal error: 'std/_Math.cpp' file not found`). Абсолютный оставляем как есть.
    static String cppIncludePath(const Token& path_token) {
        std::string p = path_token.value.to_utf8();
        for (auto& c : p) if (c == '\\') c = '/';
        if (p.size() < 2 || p[1] != ':') {   // не абсолютный Windows-путь
            char cwd[MAX_PATH] = {0};
            DWORD n = GetCurrentDirectoryA(MAX_PATH, cwd);
            if (n > 0 && n < MAX_PATH) {
                std::string root(cwd, n);
                for (auto& c : root) if (c == '\\') c = '/';
                if (!root.empty() && root.back() != '/') root += '/';
                p = root + p;
            }
        }
        return String(p);
    }

    std::vector<Token> collectIncludes(const std::vector<Token>& tokens) noexcept {
        auto walker = Walker(tokens);
        std::vector<Token> includes_result;

        while (!walker.isOuted()) {
            auto token = walker.safeGet();

            if (token.type == TypeOfToken::DIRECTIVE && token.value == "pragma") {
                walker.Next();
                auto pragma_name = walker.safeGet();
                if (pragma_name.type == TypeOfToken::LITERAL && pragma_name.value == "once")
                    once_files->insert(normalizeIncludePath(token.position.file_path));
                // остальные #pragma (<имя> …) игнорируются
                walker.Next();
                continue;
            }

            if (token.type == TypeOfToken::DIRECTIVE && token.value == "cppinclude") {
                walker.Next();

                if (walker.safeGet().type != TypeOfToken::STRING) {
                    reportError(PREPROCESSOR_ERRORS::WAITING("Waiting for a string after #cppinclude directive",
                             walker.safeGet(), "string", NOTE_INCLUDE_SYNTAX));
                    walker.Next();
                    continue;
                }

                auto file_path_token = walker.safeGet();
                walker.Next();
                if (file_path_token.value.empty()) {
                    reportError(PREPROCESSOR_ERRORS::CPP_INCLUDE_EMPTY_PATH(file_path_token));
                    continue;
                }
                if (!isFileFound(file_path_token.value)) {
                    reportError(PREPROCESSOR_ERRORS::CPP_INCLUDE_NOT_FOUND(file_path_token));
                    continue;
                }
                String cpp_path = cppIncludePath(file_path_token);
                bool cpp_dup = false;
                for (const auto& p : cpp_includes)
                    if (p == cpp_path) { cpp_dup = true; break; }
                if (!cpp_dup) {
                    cpp_includes.push_back(cpp_path);
                    DEBUG DEBUG_CPPINCLUDE_FOUND(cpp_path);
                }
                continue;
            }

            if (token.type == TypeOfToken::DIRECTIVE && token.value == "include") {
                walker.Next();

                if (walker.safeGet().type != TypeOfToken::STRING) {
                    reportError(PREPROCESSOR_ERRORS::WAITING("Waiting for a string after #include directive",
                             walker.safeGet(), "string", NOTE_INCLUDE_SYNTAX));
                    walker.Next();
                    continue;
                }

                auto file_path_token = walker.safeGet();
                walker.Next();
                if (file_path_token.value.empty()) {
                    reportError(PREPROCESSOR_ERRORS::INCLUDE_EMPTY_PATH(file_path_token));
                    continue;
                }

                if (once_files->count(normalizeIncludePath(file_path_token.value)) != 0)
                    continue; // файл с #pragma once — тихо пропускаем повторное включение

                auto self_include = checkSelfInclude(file_path_token);
                auto already_included = checkAlreadyInclude(file_path_token);
                if (!checkFileExists(file_path_token))
                    continue;

                if (!already_included && !self_include) {
                    auto file_data = openFile(file_path_token.value);
                    auto file_tokens = fastLex(file_data, file_path_token.value, true);
                    auto sub = Preprocessor(debug_mode, error_container, once_files);
                    sub.includes = this->includes;
                    auto included_tokens = sub.collectIncludes(file_tokens);
                    // вложенные #cppinclude из включённого файла всплывают наверх:
                    // их C++-определения должны попасть в сгенерированный .cpp
                    cpp_includes.insert(cpp_includes.end(), sub.cpp_includes.begin(), sub.cpp_includes.end());
                    includes_result.insert(includes_result.end(), included_tokens.begin(), included_tokens.end());
                    includes.push_back({file_path_token});
                    includes_pulled += sub.includes_pulled + 1;

                    DEBUG DEBUG_INCLUDES_SUCCESS_COLLECTED(file_path_token.value);
                }
                continue;
            } else
                includes_result.push_back(token);

            walker.Next();
        }

        return includes_result;
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////
    /// Склейка токенов через $$ (два токена '$' подряд).
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    std::vector<Token> concatTokens(std::vector<Token> tokens) noexcept {
        bool changed = true;
        while (changed) {
            changed = false;
            std::vector<Token> result;
            for (size_t i = 0; i < tokens.size(); ) {
                bool is_paste = tokens[i].type == TypeOfToken::OPERATOR && tokens[i].value == "$";
                bool is_paste_pair = is_paste && i + 1 < tokens.size()
                    && tokens[i + 1].type == TypeOfToken::OPERATOR && tokens[i + 1].value == "$";

                if (is_paste_pair) {
                    if (i == 0) {
                        reportError(PREPROCESSOR_ERRORS::PASTE_NO_LEFT_OPERAND(tokens[i]));
                        return tokens;
                    }

                    auto& left = tokens[i - 1];
                    if (left.type != TypeOfToken::LITERAL && left.type != TypeOfToken::KEYWORD) {
                        reportError(PREPROCESSOR_ERRORS::PASTE_INVALID_LEFT_OPERAND(left));
                        return tokens;
                    }

                    if (i + 2 >= tokens.size() || tokens[i + 2].type == TypeOfToken::END_OF_FILE) {
                        reportError(PREPROCESSOR_ERRORS::PASTE_NO_RIGHT_OPERAND(tokens[i]));
                        return tokens;
                    }

                    auto& right = tokens[i + 2];
                    bool right_ok = right.type == TypeOfToken::LITERAL
                                 || right.type == TypeOfToken::KEYWORD
                                 || (right.type == TypeOfToken::NUMBER && isIntegerToken(right));
                    if (!right_ok) {
                        reportError(PREPROCESSOR_ERRORS::PASTE_INVALID_RIGHT_OPERAND(right));
                        return tokens;
                    }

                    Token merged;
                    merged.type = TypeOfToken::LITERAL;
                    merged.value = left.value + right.value;
                    merged.position = left.position;
                    if (left.position.line == right.position.line) {
                        merged.position.size = (right.position.index + right.position.size) - left.position.index;
                    } else {
                        merged.position.size = left.position.size
                                             + tokens[i].position.size
                                             + tokens[i + 1].position.size
                                             + right.position.size;
                    }

                    result.pop_back();      // убрать левый операнд из результата
                    result.push_back(merged);
                    i += 3;                 // пропустить $$ и правый операнд
                    changed = true;
                } else {
                    result.push_back(tokens[i]);
                    i++;
                }
            }
            tokens = std::move(result);
        }
        return tokens;
    }

    std::vector<Token> expandDirectives(const std::vector<Token>& tokens, int depth) {
        std::vector<Token> open_tokens;
        auto walker = Walker(tokens);

        auto isDefined = [&](const Token& t) -> bool {
            for (const auto& d : defines) {
                if (d.name_token.value == t.value) return true;
            }
            for (const auto& m : macros) {
                if (m.name_token.value == t.value) return true;
            }
            return false;
        };

        while (!walker.isOuted()) {
            auto token = walker.safeGet();

            // Предопределённые макросы: подставляем как токены в точке использования.
            if (token.type == TypeOfToken::LITERAL &&
                (token.value == "__FILE__" || token.value == "__LINE__" || token.value == "__LUMEN__")) {
                open_tokens.push_back(builtinMacroToken(token));
                walker.Next();
                continue;
            }

            if (token.type == TypeOfToken::DIRECTIVE && token.value == "else") {
                reportError(PREPROCESSOR_ERRORS::ELSE_WITHOUT_IFDEF(token));
                walker.Next();
                continue;
            }
            if (token.type == TypeOfToken::DIRECTIVE && token.value == "elifdef") {
                reportError(PREPROCESSOR_ERRORS::ELIFDEF_WITHOUT_IFDEF(token));
                walker.Next();
                continue;
            }

            if (token.type == TypeOfToken::DIRECTIVE && (token.value == "ifdef" || token.value == "ifndef")) {
                bool is_ifndef = (token.value == "ifndef");
                walker.Next();

                if (walker.isOuted() || walker.safeGet().type != TypeOfToken::LITERAL) {
                    reportError(PREPROCESSOR_ERRORS::IFDEF_WAITED_LITERAL(walker.safeGet()));
                    walker.Next();
                    continue;
                }
                auto name_token = walker.safeGet();
                walker.Next();

                if (!walker.CheckType(TypeOfToken::BRACE_LEFT_CURVE)) {
                            reportError(PREPROCESSOR_ERRORS::IFDEF_WAITED_BRACE_LEFT(walker.safeGet()));
                            walker.Next();
                            continue;
                }
                walker.Next();

                std::vector<Token> if_body;
                int brace_depth = 1;
                while (!walker.CheckType(TypeOfToken::END_OF_FILE) && brace_depth > 0) {
                    if (walker.CheckType(TypeOfToken::BRACE_LEFT_CURVE)) {
                        brace_depth++;
                    } else if (walker.CheckType(TypeOfToken::BRACE_RIGHT_CURVE)) {
                        brace_depth--;
                        if (brace_depth == 0) break;
                    }
                    if_body.push_back(walker.safeGet());
                    walker.Next();
                }

                if (!walker.CheckType(TypeOfToken::BRACE_RIGHT_CURVE)) {
                    reportError(PREPROCESSOR_ERRORS::IFDEF_WAITED_BRACE_RIGHT(name_token));
                    walker.Next();
                    continue;
                }
                walker.Next();

                std::vector<std::pair<Token, std::vector<Token>>> elif_blocks;
                std::vector<Token> else_body;
                bool has_else = false;

                while (!walker.isOuted() && walker.safeGet().type == TypeOfToken::DIRECTIVE) {
                    auto dir = walker.safeGet();

                    if (dir.value == "elifdef") {
                        walker.Next();

                        if (walker.isOuted() || walker.safeGet().type != TypeOfToken::LITERAL) {
                            reportError(PREPROCESSOR_ERRORS::ELIFDEF_WAITED_LITERAL(walker.safeGet()));
                            walker.Next();
                            continue;
                        }
                        auto elif_name = walker.safeGet();
                        walker.Next();

                        if (!walker.CheckType(TypeOfToken::BRACE_LEFT_CURVE)) {
                            reportError(PREPROCESSOR_ERRORS::ELIFDEF_WAITED_BRACE_LEFT(walker.safeGet()));
                            walker.Next();
                            continue;
                        }
                        walker.Next();

                        std::vector<Token> elif_body;
                        brace_depth = 1;
                        while (!walker.CheckType(TypeOfToken::END_OF_FILE) && brace_depth > 0) {
                            if (walker.CheckType(TypeOfToken::BRACE_LEFT_CURVE)) brace_depth++;
                            else if (walker.CheckType(TypeOfToken::BRACE_RIGHT_CURVE)) {
                                brace_depth--;
                                if (brace_depth == 0) break;
                            }
                            elif_body.push_back(walker.safeGet());
                            walker.Next();
                        }

                        if (!walker.CheckType(TypeOfToken::BRACE_RIGHT_CURVE)) {
                            reportError(PREPROCESSOR_ERRORS::ELIFDEF_WAITED_BRACE_RIGHT(elif_name));
                            walker.Next();
                            continue;
                        }
                        walker.Next();

                        elif_blocks.push_back({elif_name, std::move(elif_body)});
                    }
                    else if (dir.value == "else") {
                        walker.Next();

                        if (!walker.CheckType(TypeOfToken::BRACE_LEFT_CURVE)) {
                    reportError(PREPROCESSOR_ERRORS::IFDEF_WAITED_BRACE_LEFT(walker.safeGet()));
                    walker.Next();
                    continue;
                        }
                        walker.Next();

                        brace_depth = 1;
                        while (!walker.CheckType(TypeOfToken::END_OF_FILE) && brace_depth > 0) {
                            if (walker.CheckType(TypeOfToken::BRACE_LEFT_CURVE)) brace_depth++;
                            else if (walker.CheckType(TypeOfToken::BRACE_RIGHT_CURVE)) {
                                brace_depth--;
                                if (brace_depth == 0) break;
                            }
                            else_body.push_back(walker.safeGet());
                            walker.Next();
                        }

                        if (!walker.CheckType(TypeOfToken::BRACE_RIGHT_CURVE)) {
                            reportError(PREPROCESSOR_ERRORS::IFDEF_WAITED_BRACE_RIGHT(dir));
                            walker.Next();
                            continue;
                        }
                        walker.Next();

                        has_else = true;
                        break;
                    }
                    else {
                        break;
                    }
                }

                bool condition = isDefined(name_token);
                if (is_ifndef) condition = !condition;

                if (condition) {
                    auto expanded = expandDirectives(if_body, depth);
                    open_tokens.insert(open_tokens.end(), expanded.begin(), expanded.end());
                } else {
                    bool executed = false;
                    for (auto& [elif_name, elif_body] : elif_blocks) {
                        if (isDefined(elif_name)) {
                            auto expanded = expandDirectives(elif_body, depth);
                            open_tokens.insert(open_tokens.end(), expanded.begin(), expanded.end());
                            executed = true;
                            break;
                        }
                    }
                    if (!executed && has_else) {
                        auto expanded = expandDirectives(else_body, depth);
                        open_tokens.insert(open_tokens.end(), expanded.begin(), expanded.end());
                    }
                }
                continue;
            }

            if (token.type == TypeOfToken::DIRECTIVE && token.value == "define") {
                walker.Next();
                if (walker.safeGet().type != TypeOfToken::LITERAL) {
                    reportError(PREPROCESSOR_ERRORS::DEFINE_WAITED_LITERAL(walker.safeGet()));
                    // съедаем остаток директивы, чтобы токены не утекли в вывод
                    while (!walker.Check(TypeOfToken::OPERATOR, ";") && !walker.CheckType(TypeOfToken::END_OF_FILE))
                        walker.Next();
                    if (walker.Check(TypeOfToken::OPERATOR, ";")) walker.Next();
                    continue;
                }
                auto define_identifier = walker.safeGet();
                walker.Next();

                if (!walker.Check(TypeOfToken::OPERATOR, "=")) {
                    reportError(PREPROCESSOR_ERRORS::WAITING("expected '='", walker.Get(), "=", NOTE_DEFINE_SYNTAX));
                    walker.Next();
                    continue;
                }
                walker.Next();

                std::vector<Token> define_tokens;
                MAX_DEFINE_TOKENS_COUNTER_INIT
                while (!walker.Check(TypeOfToken::OPERATOR, ";") && !walker.CheckType(TypeOfToken::END_OF_FILE)) {
                    define_tokens.push_back(walker.Get());
                    defined_tokens_count++;
                    walker.Next();
                }
                IF_DEFINE_TOKEN_COUNT_IS_ZERO(
                    reportError(PREPROCESSOR_ERRORS::DEFINE_WAITED_ONE_LEAST_TOKEN(walker.Get()));
                )
                if (!walker.Check(TypeOfToken::OPERATOR, ";")) {
                    reportError(PREPROCESSOR_ERRORS::WAITING("expected ';'", walker.Get(), ";", NOTE_DEFINE_SYNTAX));
                    walker.Next();
                    continue;
                }
                walker.Next();

                IF_DEFINE_TOKEN_COUNT_MORE_MAX_TOKENS(
                    reportError(PREPROCESSOR_ERRORS::DEFINE_TOKENS_OUT_OF_LIMIT("Define tokens limit exceeded", define_tokens.front(), define_tokens.back(), defined_tokens_count));
                )

                if (checkAlreadyDefinition(define_identifier)) {
                    for (auto it = defines.begin(); it != defines.end(); ) {
                        if (it->name_token.value == define_identifier.value) {
                            it = defines.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }

                defines.push_back({define_identifier, expandDirectives(define_tokens, 0)});
                defines_seen++;
                continue;
            }

            if (token.type == TypeOfToken::DIRECTIVE && token.value == "macro") {
                walker.Next();
                if (walker.safeGet().type != TypeOfToken::LITERAL) {
                    reportError(PREPROCESSOR_ERRORS::MACRO_WAITED_LITERAL(walker.safeGet()));
                    walker.Next();
                    continue;
                }
                auto macro_identifier = walker.safeGet();
                walker.Next();

                if (!walker.CheckType(TypeOfToken::BRACE_LEFT)) {
                    reportError(PREPROCESSOR_ERRORS::WAITING("expected '('", walker.safeGet(), "(", NOTE_MACRO_SYNTAX));
                    walker.Next();
                    continue;
                }
                walker.Next();

                std::vector<MacroArg> args_tokens;
                // EOF-гард: при недописанном `#macro max(` (нет ')') токены
                // заканчиваются, и без него цикл крутится вечно (walker.Next()
                // растёт бесконечно, ошибки сыплются в контейнер).
                while (!walker.CheckType(TypeOfToken::BRACE_RIGHT)
                        && !walker.isOuted()
                        && !walker.CheckType(TypeOfToken::END_OF_FILE)) {
                    if (walker.CheckType(TypeOfToken::LITERAL)) {
                        auto arg_name_token = walker.safeGet();
                        args_tokens.push_back(MacroArg(arg_name_token));
                        walker.Next();

                        if (walker.CheckType(TypeOfToken::VA_ARGS)) {
                            args_tokens.back().variadic_count = -1;
                            walker.Next();

                            if (walker.CheckType(TypeOfToken::NUMBER)) {
                                if (isIntegerToken(walker.safeGet())) {
                                    args_tokens.back().variadic_count = std::atoi(walker.safeGet().value.to_utf8().c_str());
                                    walker.Next();
                                } else {
                                    reportError(PREPROCESSOR_ERRORS::MACRO_VARIADIC_ARGUMENT_INVALID_COUNT(walker.safeGet()));
                                    walker.Next();
                                    continue;
                                }
                            }
                        }

                        if (!walker.CheckType(TypeOfToken::BRACE_RIGHT)) {
                            if (walker.Check(TypeOfToken::OPERATOR, ",")) {
                                walker.Next();
                                if (!walker.CheckType(TypeOfToken::LITERAL) && !walker.CheckType(TypeOfToken::VA_ARGS)) {
                                    reportError(PREPROCESSOR_ERRORS::MACRO_WAITED_ARGUMENT_IS_LITERAL(walker.safeGet()));
                                    walker.Next();
                                    continue;
                                }
                                continue;
                            } else {
                                    reportError(PREPROCESSOR_ERRORS::WAITING("expected ',' or ')'", walker.safeGet(), "',' or ')'", NOTE_MACRO_SYNTAX));
                                    walker.Next();
                                    continue;
                            }
                        } else {
                            break;
                        }
                    } else {
                        reportError(PREPROCESSOR_ERRORS::MACRO_WAITED_ARGUMENT_IS_LITERAL(walker.safeGet()));
                        walker.Next();
                        continue;
                    }
                }

                if (!walker.CheckType(TypeOfToken::BRACE_RIGHT)) {
                    reportError(PREPROCESSOR_ERRORS::WAITING("expected ')'", walker.safeGet(), ")", NOTE_DEFINE_SYNTAX));
                    walker.Next();
                    continue;
                }
                walker.Next();

                for (int i = 0; i < args_tokens.size(); i++) {
                    if (args_tokens[i].is_variadic() && i != args_tokens.size() - 1 && args_tokens[i].is_infinite_variadic()) {
                        reportError(PREPROCESSOR_ERRORS::MACRO_VARIADIC_INFINITE_ARGUMENT_MUST_BE_LAST(args_tokens[i].token));
                        walker.Next();
                        continue;
                    }
                }

                if (!walker.CheckType(TypeOfToken::BRACE_LEFT_CURVE)) {
                    reportError(PREPROCESSOR_ERRORS::WAITING("expected '{'", walker.safeGet(), "{", NOTE_DEFINE_SYNTAX));
                    walker.Next();
                    continue;
                }
                walker.Next();

                std::vector<Token> define_tokens;
                int defined_tokens_count = 0;
                int brace_depth = 1;

                while (!walker.CheckType(TypeOfToken::END_OF_FILE) && brace_depth > 0) {
                    if (walker.CheckType(TypeOfToken::BRACE_LEFT_CURVE)) {
                        brace_depth++;
                    } else if (walker.CheckType(TypeOfToken::BRACE_RIGHT_CURVE)) {
                        brace_depth--;
                        if (brace_depth == 0) break;
                    }
                    define_tokens.push_back(walker.safeGet());
                    defined_tokens_count++;
                    walker.Next();
                }

                if (!walker.CheckType(TypeOfToken::BRACE_RIGHT_CURVE)) {
                    reportError(PREPROCESSOR_ERRORS::WAITING("expected '}'", walker.safeGet(), "}", NOTE_DEFINE_SYNTAX));
                    walker.Next();
                    continue;
                }
                walker.Next();

                if (defined_tokens_count > MAX_MACRO_TOKENS) {
                    reportError(PREPROCESSOR_ERRORS::MACRO_TOKENS_OUT_OF_LIMIT("Macro tokens limit exceeded", define_tokens.front(), define_tokens.back(), defined_tokens_count));
                    walker.Next();
                    continue;
                }

                if (checkAlreadyMacro(macro_identifier)) {
                    for (auto it = macros.begin(); it != macros.end(); ) {
                        if (it->name_token.value == macro_identifier.value) {
                            it = macros.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }

                macros.push_back({macro_identifier, args_tokens, define_tokens});
                macros_seen++;
                continue;
            }

            if (token.type == TypeOfToken::DIRECTIVE && token.value == "undef") {
                walker.Next();

                if (walker.isOuted() || walker.safeGet().type != TypeOfToken::LITERAL) {
                    reportError(PREPROCESSOR_ERRORS::UNDEF_WAITED_LITERAL(walker.safeGet()));
                    walker.Next();
                    continue;
                }
                auto undef_name_token = walker.safeGet();
                walker.Next();

                bool removed = false;

                for (auto it = defines.begin(); it != defines.end(); ) {
                    if (it->name_token.value == undef_name_token.value) {
                        it = defines.erase(it);
                        removed = true;
                    } else {
                        ++it;
                    }
                }
                for (auto it = macros.begin(); it != macros.end(); ) {
                    if (it->name_token.value == undef_name_token.value) {
                        it = macros.erase(it);
                        removed = true;
                    } else {
                        ++it;
                    }
                }

                if (!removed) {
                    reportError(PREPROCESSOR_ERRORS::UNDEF_NOT_DEFINED(undef_name_token));
                }
                continue;
            }

            // ---------- @ (stringify) ----------
            if (token.type == TypeOfToken::OPERATOR && token.value == "@") {
                walker.Next();
                if (walker.isOuted() || walker.CheckType(TypeOfToken::END_OF_FILE)) {
                    reportError(PREPROCESSOR_ERRORS::WAITING("expected token after '@'", token, "token", "@<token> or @(tokens)"));
                    walker.Next();
                    continue;
                }

                // @(<группа>) — раскрыть, склеить $$, затем stringify
                if (walker.CheckType(TypeOfToken::BRACE_LEFT)) {
                    walker.Next();
                    std::vector<Token> group;
                    int paren_depth = 1;

                    while (!walker.CheckType(TypeOfToken::END_OF_FILE) && paren_depth > 0) {
                        auto t = walker.safeGet();
                        if (t.type == TypeOfToken::BRACE_LEFT) {
                            paren_depth++;
                        } else if (t.type == TypeOfToken::BRACE_RIGHT) {
                            paren_depth--;
                            if (paren_depth == 0) break;
                        }
                        group.push_back(t);
                        walker.Next();
                    }

                    if (!walker.CheckType(TypeOfToken::BRACE_RIGHT)) {
                        reportError(PREPROCESSOR_ERRORS::WAITING("expected ')' to close '@('", walker.safeGet(), ")", "@(tokens)"));
                        walker.Next();
                        continue;
                    }
                    walker.Next();

                    if (group.empty()) {
                        reportError(PREPROCESSOR_ERRORS::STRINGIFY_EMPTY_GROUP(token));
                        walker.Next();
                        continue;
                    }

                    // Раскрываем содержимое
                    auto expanded_group = expandDirectives(group, depth);
                    // Склеиваем $$ внутри
                    auto concatted_group = concatTokens(expanded_group);

                    // Собираем строку из токенов через пробел
                    String str_value;
                    for (size_t i = 0; i < concatted_group.size(); i++) {
                        if (i > 0 && concatted_group[i].value != ",") str_value += " ";
                        str_value += concatted_group[i].value;
                    }

                    Token str_token;
                    str_token.type = TypeOfToken::STRING;
                    str_token.value = str_value;
                    str_token.position = token.position;
                    str_token.position.size = str_value.size() + 2;
                    open_tokens.push_back(str_token);
                    continue;
                }

                // @<одиночный токен> — раскрыть, затем stringify (как @(token))
                auto next_tok = walker.safeGet();
                walker.Next();

                auto expanded = expandDirectives({next_tok}, depth);

                String str_value;
                for (size_t i = 0; i < expanded.size(); i++) {
                    if (i > 0 && expanded[i].value != ",") str_value += " ";
                    str_value += expanded[i].value;
                }

                Token str_token;
                str_token.type = TypeOfToken::STRING;
                str_token.value = str_value;
                str_token.position = token.position;
                str_token.position.size = str_value.size() + 2;
                open_tokens.push_back(str_token);
                continue;
            }

            if (inDefines(token, defines)) {
                if (depth > MAX_DEFINE_RECURSION_DEPTH) {
                    reportError(PREPROCESSOR_ERRORS::DEFINE_RECURSION_LIMIT("define recursion limit exceeded", token, depth));
                    walker.Next();
                    continue;
                }
                auto opened_define_tokens = expandDirectives(getDefineTokens(token, defines), depth + 1);
                open_tokens.insert(open_tokens.end(), opened_define_tokens.begin(), opened_define_tokens.end());
                walker.Next();
                continue;
            }

            if (inMacro(token, macros)) {
                auto macro_name_token = token;

                if (depth > MAX_DEFINE_RECURSION_DEPTH) {
                    reportError(PREPROCESSOR_ERRORS::MACRO_RECURSION_LIMIT("macro recursion limit exceeded", token, depth));
                    walker.Next();
                    continue;
                }

                walker.Next();

                if (!walker.CheckType(TypeOfToken::BRACE_LEFT)) {
                    reportError(PREPROCESSOR_ERRORS::WAITING("expected '('", walker.safeGet(), "(", "<name>(args, ...)"));
                    walker.Next();
                    continue;
                }
                walker.Next();

                auto macro = getMacro(macro_name_token, macros);

                std::vector<std::vector<Token>> call_args;
                std::vector<Token> current_arg;
                int paren_depth = 0;
                int curve_depth = 0;
                bool found_closing_paren = false;

                while (!walker.CheckType(TypeOfToken::END_OF_FILE)) {
                    auto t = walker.safeGet();

                    if (t.type == TypeOfToken::BRACE_LEFT) {
                        paren_depth++;
                    } else if (t.type == TypeOfToken::BRACE_RIGHT) {
                        if (paren_depth == 0 && curve_depth == 0) {
                            if (!current_arg.empty()) {
                                call_args.push_back(current_arg);
                            }
                            walker.Next();
                            found_closing_paren = true;
                            break;
                        }
                        paren_depth--;
                    } else if (t.type == TypeOfToken::BRACE_LEFT_CURVE) {
                        curve_depth++;
                    } else if (t.type == TypeOfToken::BRACE_RIGHT_CURVE) {
                        curve_depth--;
                    }

                    if (t.type == TypeOfToken::OPERATOR && t.value == "," && paren_depth == 0 && curve_depth == 0) {
                        call_args.push_back(current_arg);
                        current_arg.clear();
                        walker.Next();
                        continue;
                    }

                    current_arg.push_back(t);
                    walker.Next();
                }

                if (!found_closing_paren) {
                    reportError(PREPROCESSOR_ERRORS::MACRO_WAITING_BRACE_RIGHT(macro_name_token));
                    walker.Next();
                    continue;
                }

                int min_args = 0;
                bool has_infinite = false;
                for (auto& arg : macro.args_tokens) {
                    if (!arg.is_variadic()) {
                        min_args++;
                    } else if (arg.is_infinite_variadic()) {
                        has_infinite = true;
                    } else {
                        min_args += arg.variadic_count;
                    }
                }

                std::vector<std::vector<Token>> resolved_args;
                int call_idx = 0;
                bool args_ok = true;

                for (int i = 0; i < macro.args_tokens.size(); i++) {
                    auto& macro_arg = macro.args_tokens[i];

                    if (!macro_arg.is_variadic()) {
                        if (call_idx >= call_args.size()) {
                            reportError(PREPROCESSOR_ERRORS::MACRO_TOO_FEW_ARGUMENTS(macro_name_token, min_args, call_args.size()));
                            args_ok = false;
                            break;
                        }
                        resolved_args.push_back(call_args[call_idx]);
                        call_idx++;
                    }
                    else if (macro_arg.is_infinite_variadic()) {
                        std::vector<Token> var_tokens;
                        bool first = true;
                        for (int j = call_idx; j < call_args.size(); j++) {
                            if (!first) {
                                Token comma;
                                comma.type = TypeOfToken::OPERATOR;
                                comma.value = ",";
                                var_tokens.push_back(comma);
                            }
                            first = false;
                            var_tokens.insert(var_tokens.end(), call_args[j].begin(), call_args[j].end());
                        }
                        resolved_args.push_back(var_tokens);
                        call_idx = call_args.size();
                    }
                    else {
                        int need = macro_arg.variadic_count;
                        if (call_idx + need > call_args.size()) {
                            reportError(PREPROCESSOR_ERRORS::MACRO_VARIADIC_NOT_ENOUGH_TOKENS(macro_name_token, macro_arg.token, need, call_args.size() - call_idx));
                            args_ok = false;
                            break;
                        }
                        std::vector<Token> var_tokens;
                        for (int j = 0; j < need; j++) {
                            if (j > 0) {
                                Token comma;
                                comma.type = TypeOfToken::OPERATOR;
                                comma.value = ",";
                                var_tokens.push_back(comma);
                            }
                            var_tokens.insert(var_tokens.end(), call_args[call_idx].begin(), call_args[call_idx].end());
                            call_idx++;
                        }
                        resolved_args.push_back(var_tokens);
                    }
                }

                if (!args_ok) {
                    continue;
                }

                if (call_idx < call_args.size()) {
                    int expected = has_infinite ? min_args : macro.args_tokens.size();
                    reportError(PREPROCESSOR_ERRORS::MACRO_TOO_MANY_ARGUMENTS(macro_name_token, expected, call_args.size()));
                    continue;
                }

                std::vector<Token> substituted_body;
                for (auto& body_tok : macro.body_tokens) {
                    bool replaced = false;
                    for (int i = 0; i < macro.args_tokens.size(); i++) {
                        if (body_tok.type == TypeOfToken::LITERAL && body_tok.value == macro.args_tokens[i].token.value) {
                            // НЕ раскрываем аргумент повторно: иначе, если аргумент сам
                            // является define (напр. a -> 0), он исказит тело до подстановки
                            // (было: #define a=0; -> #define 0=0;). Тело всё равно заново
                            // сканируется в expandDirectives(pasted_body), поэтому макросы
                            // внутри тела раскроются и без предварительного раскрытия аргумента.
                            auto rt = resolved_args[i];
                            substituted_body.insert(substituted_body.end(), rt.begin(), rt.end());
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced) {
                        substituted_body.push_back(body_tok);
                    }
                }

                auto pasted_body = concatTokens(substituted_body);
                auto expanded_body = expandDirectives(pasted_body, depth + 1);
                open_tokens.insert(open_tokens.end(), expanded_body.begin(), expanded_body.end());
                continue;
            }

            open_tokens.push_back(token);
            walker.Next();
        }
        return open_tokens;
    }

    /*
     * Главная точка входа:
     *   1. collectIncludes
     *   2. expandDirectives (внутри: #define, #macro, #undef, #ifdef, @, раскрытие)
     *   3. concatTokens ($$)
     */
    std::vector<Token> operator()(const std::vector<Token>& tokens) {
        auto includes_result = collectIncludes(tokens);
        auto expanded_result = expandDirectives(includes_result, 0);
        auto concat_result = concatTokens(expanded_result);
        source_tokens = concat_result;
        auto cleaned_result = filterComments(concat_result);

        return cleaned_result;
    }
};
