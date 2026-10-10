#include "../utils/TerminalStyle.cpp"
#include "../utils/Strings.cpp"
#include "../utils/File.cpp"
#include "Tokens.cpp"
#include <memory>

#include "Constants.cpp"



#pragma once


enum struct MessageType {
    SyntaxError,
    PreprocessorError,
    InternalError,
    ExecutableError,
    Warning,
};

const std::vector<String> MessageTypeNames = {
    "SyntaxError",
    "PreprocessorError",
    "InternalError",
    "ExecutableError",
    "Warning"
};

const String messageNameFromType(MessageType type) {
    return MessageTypeNames[static_cast<int>(type)];
}


struct BaseError {
    String message;
    String special_message;
    String note;
    String filepath;

    MessageType type = MessageType::SyntaxError;
    bool is_warning = false;
    bool view_lines = true;
    bool chain_call_site = false;

    int line;
    int column;
    int end_line;
    int end_column;
    int size;

    int error_code = 0;

    std::vector<String> hints;
    std::shared_ptr<BaseError> sub_error = nullptr;


    BaseError() = default;

    BaseError(const String message, Token token) noexcept
        : message(message), line(token.position.line), column(token.position.index),
         end_line(token.position.line), end_column(token.position.index + token.position.size),
         size(token.position.size), filepath(token.position.file_path) {}

    BaseError(const String message, Token token_start, Token token_end) noexcept
        : message(message), line(token_start.position.line), column(token_start.position.index),
         end_line(token_end.position.line), end_column(token_end.position.index + token_end.position.size),
         size(token_end.position.index - token_start.position.index + token_end.position.size), filepath(token_start.position.file_path) {}

    // Builder-pattern mthods for convenient error configuration

    BaseError& withHint(const String& hint) noexcept {
        hints.push_back(hint);
        return *this;
    }

    BaseError& withNote(const String& note_text) noexcept {
        note = note_text;
        return *this;
    }

    BaseError& withType(MessageType t) noexcept {
        type = t;
        return *this;
    }

    BaseError& withCode(int code) noexcept {
        error_code = code;
        return *this;
    }

    BaseError& asWarning() noexcept {
        is_warning = true;
        return *this;
    }

    // Диагностика привязана к месту вызова (assert с current_call_site): даже если
    // это warning, по цепочке вызовов нужно дойти до места в коде пользователя,
    // иначе span указывает на оператор в теле функции-источника, а не туда,
    // где человек реально написал вызов.
    BaseError& withCallSiteChain() noexcept {
        chain_call_site = true;
        return *this;
    }

    BaseError& hideLines() noexcept {
        view_lines = false;
        return *this;
    }

    BaseError& withSubError(std::shared_ptr<BaseError> sub) noexcept {
        sub_error = sub;
        return *this;
    }

    BaseError& withSubError(const String& sub_message, Token token) noexcept {
        sub_error = std::make_shared<BaseError>(sub_message, token);
        sub_error->type = type;
        sub_error->error_code = error_code;
        sub_error->is_warning = is_warning;
        sub_error->chain_call_site = chain_call_site;
        return *this;
    }

    BaseError& withSubError(const String& sub_message, Token token_start, Token token_end) noexcept {
        sub_error = std::make_shared<BaseError>(sub_message, token_start, token_end);
        sub_error->type = type;
        sub_error->error_code = error_code;
        sub_error->is_warning = is_warning;
        sub_error->chain_call_site = chain_call_site;
        return *this;
    }


    String toString() const noexcept {

        // Read original file for accurate line display
        std::vector<String> lines;

        try {
            String file_data = openFile(filepath);
            lines = splitString(file_data, '\n');
        } catch (...) {
            // File could not be read — show error without source context
        }

        bool has_lines = !lines.empty();

        String result;

        int display_line = line + 1;
        int line_number_size = std:: to_string(display_line).size() + 1;
        bool is_multiline = (line != end_line);

        result += Style("error[" + std::to_string(error_code) + "] (" + messageNameFromType(type) + ")" + (is_warning ? Style("[warning]", TerminalStyle::BRIGHT_YELLOW) : "") + ": ", TerminalStyle::BRIGHT_RED) + special_message + "\n";
        result += Style(String::WithCapacity(line_number_size," ", false) + "--> ", TerminalStyle::BRIGHT_CYAN) + filepath + ":" + std::to_string(display_line) + ":" + std::to_string(column) + "\n";
        result += Style(String::WithCapacity(line_number_size," ", false) + " | ", TerminalStyle::BRIGHT_CYAN) + "\n";

        if (has_lines) {
            // Show context line before the error start
            if (line >= 1 && view_lines && (line - 1) < static_cast<int>(lines.size())) {
                result += String::WithCapacity(line_number_size, display_line - 1, false) + Style(" | ", TerminalStyle::BRIGHT_CYAN) +
                    Style(lines[line - 1], TerminalStyle::DIM_WHITE) + "\n";
            }

            // Show each error line with underlining
            for (int ln = line; ln <= end_line && ln < static_cast<int>(lines.size()); ln++) {
                int disp = ln + 1;
                result += String::WithCapacity(line_number_size, disp, false) + Style(" | ", TerminalStyle::BRIGHT_CYAN) +
                    Style(lines[ln], TerminalStyle::DIM_WHITE) + "\n";

                if (view_lines) {
                    int ul_start = (ln == line) ? column : 0;
                    int ln_size = static_cast<int>(lines[ln].size());
                    int ul_end = (ln == end_line)
                        ? (end_column < ln_size ? end_column : ln_size)
                        : ln_size;
                    int ul_size = ul_end - ul_start;

                    if (ul_size > 0) {
                        result += Style(String::WithCapacity(line_number_size," ", false) + " | ", TerminalStyle::BRIGHT_CYAN) +
                            String::RepeatFrom(' ', ul_start) +
                            Style("^" + String::RepeatFrom('~', ul_size - 1), TerminalStyle::BRIGHT_RED) + "\n";
                    }
                }
            }

            // Error message (at column for single-line, at start for multi-line)
            result += Style(String::WithCapacity(line_number_size," ", false) + " | ", TerminalStyle::BRIGHT_CYAN) +
                String::RepeatFrom(' ', is_multiline ? 0 : column) + Style(message, TerminalStyle::BRIGHT_RED) + "\n";

            // Show context line after the error end
            if (end_line + 1 < static_cast<int>(lines.size()) && view_lines) {
                result += String::WithCapacity(line_number_size, end_line + 2, false) + Style(" | ", TerminalStyle::BRIGHT_CYAN) +
                    Style(lines[end_line + 1], TerminalStyle::DIM_WHITE) + "\n";
            }
        }

        result += Style(String::WithCapacity(line_number_size," ", false) + " | ", TerminalStyle::BRIGHT_CYAN) + "\n";
        if (!note.empty()) {
            result += Style(String::WithCapacity(line_number_size," ", false) + " = note: ", TerminalStyle::BRIGHT_CYAN) +
                Style(note, TerminalStyle::DIM_WHITE) + "\n";
        }
        for (const auto& hint : hints) {
            result += Style(String::WithCapacity(line_number_size," ", false) + " = hint: ", TerminalStyle::BRIGHT_GREEN) +
                Style(hint, TerminalStyle::DIM_WHITE) + "\n";
        }
        if (sub_error) {
            result += sub_error->toString();
        }
        return result;
    }

};



/////////////////////////////////////////////////////////////////
// Breaks the error and exits the program.
/////////////////////////////////////////////////////////////////
inline void breakError(const BaseError& err) {
    std::cout << err.toString() << std::endl;
    exit(-1);
}

inline void viewError(const BaseError& err) {
    std:: cout << err.toString() << std:: endl;
}
/////////////////////////////////////////////////////////////////

namespace PREPROCESSOR_ERRORS {
    BaseError WAITING(const String message, const Token token_start, const Token token_end, const String waited, const String found, const String note = "") noexcept {
        auto err = BaseError(message, token_start, token_end)
            .withType(MessageType::PreprocessorError)
            .withCode(0)
            .withHint("Expected '" + waited + "' at this position.")
            .withNote(note);
        err.special_message = String("expected ") + waited + ", found " + found;
        return err;
    }

    BaseError WAITING(const String message, const Token token, const String waited, const String note = "") noexcept {
        auto err = BaseError(message, token)
            .withType(MessageType::PreprocessorError)
            .withCode(0)
            .withHint("Expected '" + waited + "' at this position.")
            .withNote(note);
        err.special_message = String("expected ") + waited + ", found " + token.value;
        return err;
    }

    BaseError INCLUDE_ALREADY_INCLUDED(const String message, const Token token, const Token predeclared_line_token) noexcept {
        return BaseError(message, token)
            .withType(MessageType::PreprocessorError)
            .withCode(1)
            .asWarning()
            .withHint("The file has already been included. The include directive will be ignored.")
            .withSubError("Declared here", predeclared_line_token);
    }

    BaseError INCLUDE_NOT_FOUND(const Token token) noexcept {
        return BaseError("Include not found", token)
            .withType(MessageType::PreprocessorError)
            .withCode(2)
            .withHint("The file was not found. The include directive will be ignored.");
    }

    BaseError INCLUDE_EMPTY_PATH(const Token token) noexcept {
        return BaseError("Include empty path", token)
            .withType(MessageType::PreprocessorError)
            .withCode(3)
            .asWarning()
            .withHint("The include path is empty. The include directive will be ignored.");
    }

    BaseError INCLUDE_SELF(const Token token) noexcept {
        return BaseError("Include self", token)
            .withType(MessageType::PreprocessorError)
            .withCode(4)
            .asWarning()
            .withHint("The include path is the same as the file path. The include directive will be ignored.");
    }

    BaseError CPP_INCLUDE_EMPTY_PATH(const Token token) noexcept {
        return BaseError("Cppinclude empty path", token)
            .withType(MessageType::PreprocessorError)
            .withCode(143)
            .asWarning()
            .withHint("The C++ include path is empty. The #cppinclude directive will be ignored.");
    }

    BaseError CPP_INCLUDE_NOT_FOUND(const Token token) noexcept {
        return BaseError("Cppinclude not found", token)
            .withType(MessageType::PreprocessorError)
            .withCode(144)
            .withHint("The C++ file was not found. The #cppinclude directive will be ignored.");
    }

    BaseError DEFINE_WAITED_LITERAL(const Token token) noexcept {
        return BaseError("Define waited literal, but found " + token.value, token)
            .withType(MessageType::PreprocessorError)
            .withCode(5)
            .withHint(NOTE_DEFINE_SYNTAX);
    }

    BaseError DEFINE_WAITED_ONE_LEAST_TOKEN(const Token token) noexcept {
        return BaseError("Define at least one token", token)
            .withType(MessageType::PreprocessorError)
            .withCode(6)
            .withHint(NOTE_DEFINE_SYNTAX);

    }

    BaseError DEFINE_TOKENS_OUT_OF_LIMIT(const String message, const Token token_start, const Token token_end, const int count) noexcept {
        auto err = BaseError(message, token_start, token_end)
            .withType(MessageType::PreprocessorError)
            .withCode(7)
            .withHint("Reduce the number of tokens in the #define body.");
        err.special_message = String("Define tokens limit exceeded: ") + String(count);
        return err;
    }

    BaseError DEFINE_ALREADY_DEFINED(const String message, const Token token, const Token predeclared_line_token) noexcept {
        return BaseError(message, token)
            .withType(MessageType::PreprocessorError)
            .withCode(145)
            .asWarning()
            .withHint("The macro has already been defined. The new definition will overwrite the previous one.")
            .withSubError("Declared here", predeclared_line_token);
    }

    BaseError MACRO_ALREADY_DEFINED(const String message, const Token token, const Token predeclared_line_token) noexcept {
        return BaseError(message, token)
            .withType(MessageType::PreprocessorError)
            .withCode(146)
            .asWarning()
            .withHint("The macro has already been defined. The new definition will overwrite the previous one.")
            .withSubError("Declared here", predeclared_line_token);
    }

    BaseError DEFINE_RECURSION_LIMIT(const String message, const Token token, const int depth) noexcept {
        auto err = BaseError(message, token)
            .withType(MessageType::PreprocessorError)
            .withCode(147)
            .withHint("Avoid #define bodies that reference themselves too deeply.");
        err.special_message = String("Define recursion limit: ") + String(MAX_DEFINE_RECURSION_DEPTH);
        return err;
    }

    BaseError MACRO_WAITED_LITERAL(const Token token) noexcept {
        return BaseError("Macro definition waited literal, but found " + token.value, token)
            .withType(MessageType::PreprocessorError)
            .withCode(10)
            .withNote(NOTE_MACRO_SYNTAX);
    }

    BaseError MACRO_WAITED_ARGUMENT_IS_LITERAL(const Token token) noexcept {
        return BaseError("Macro definition waited literal argument, but found " + token.value, token)
            .withType(MessageType::PreprocessorError)
            .withCode(11)
            .withNote(NOTE_MACRO_SYNTAX);
    }

    BaseError MACRO_VARIADIC_ARGUMENT_INVALID_COUNT(const Token token) noexcept {
        return BaseError("Macro variadic argument waited integer number, but found " + token.value, token)
            .withType(MessageType::PreprocessorError)
            .withCode(12)
            .withNote(NOTE_MACRO_SYNTAX);
    }

    BaseError MACRO_VARIADIC_INFINITE_ARGUMENT_MUST_BE_LAST(const Token token) noexcept {
        return BaseError("Macro variadic argument must be the last one", token)
            .withType(MessageType::PreprocessorError)
            .withCode(13)
            .withNote(NOTE_MACRO_SYNTAX);
    }

    BaseError MACRO_TOKENS_OUT_OF_LIMIT(const String message, const Token token_start, const Token token_end, const int count) noexcept {
        auto err = BaseError(message, token_start, token_end)
            .withType(MessageType::PreprocessorError)
            .withCode(14)
            .withHint("Reduce the number of tokens in the macro body.");
        err.special_message = String("Macro tokens limit exceeded: ") + String(count);
        return err;
    }

    BaseError MACRO_ARGUMENT_COUNT_MISMATCH(const Token macro_token, const int expected, const int got) noexcept {
        auto err = BaseError("Macro '" + macro_token.value + "' expects " + String(expected) + " arguments, but got " + String(got), macro_token)
            .withType(MessageType::PreprocessorError)
            .withCode(15);
        err.special_message = String("expected ") + String(expected) + " arguments, got " + String(got);
        err.withNote(NOTE_MACRO_SYNTAX);
        return err;
    }

    BaseError MACRO_TOO_FEW_ARGUMENTS(const Token macro_token, const int expected, const int got) noexcept {
        auto err = BaseError("Macro '" + macro_token.value + "' expects at least " + String(expected) + " arguments, but got " + String(got), macro_token)
            .withType(MessageType::PreprocessorError)
            .withCode(16);
        err.special_message = String("expected at least ") + String(expected) + " arguments, got " + String(got);
        err.withNote(NOTE_MACRO_SYNTAX);
        return err;
    }

    BaseError MACRO_RECURSION_LIMIT(const String message, const Token token, const int depth) noexcept {
        auto err = BaseError(message, token)
            .withType(MessageType::PreprocessorError)
            .withCode(17)
            .withHint("Avoid macro bodies that reference themselves too deeply.");
        err.special_message = String("Macro recursion limit: ") + String(MAX_MACRO_RECURSION_DEPTH);
        return err;
    }

    BaseError MACRO_EMPTY_ARGUMENT(const Token token, const Token macro_token) noexcept {
        return BaseError("Empty argument in macro '" + macro_token.value + "' call", token)
            .withType(MessageType::PreprocessorError)
            .withCode(18)
            .withNote(NOTE_MACRO_SYNTAX);
    }

    BaseError MACRO_WAITING_BRACE_RIGHT(const Token token) noexcept {
        return BaseError("Expected ')' after macro arguments", token)
            .withType(MessageType::PreprocessorError)
            .withCode(19)
            .withNote(NOTE_MACRO_SYNTAX);
    }

    BaseError MACRO_VARIADIC_NOT_ENOUGH_TOKENS(const Token call_token, const Token param_token, const int expected, const int got) noexcept {
        auto err = BaseError("Variadic macro argument expected " + String(expected) + " groups, but got " + String(got), call_token)
            .withType(MessageType::PreprocessorError)
            .withCode(20)
            .withSubError("Argument declared here", param_token);
        err.special_message = String("expected ") + String(expected) + " argument groups, got " + String(got);
        err.withNote(NOTE_MACRO_SYNTAX);
        return err;
    }

    BaseError MACRO_TOO_MANY_ARGUMENTS(const Token macro_token, const int expected, const int got) noexcept {
        auto err = BaseError("Macro '" + macro_token.value + "' expects " + String(expected) + " arguments, but got " + String(got), macro_token)
            .withType(MessageType::PreprocessorError)
            .withCode(21);
        err.special_message = String("expected ") + String(expected) + " arguments, got " + String(got);
        err.withNote(NOTE_MACRO_SYNTAX);
        return err;
    }

    BaseError UNDEF_WAITED_LITERAL(const Token token) noexcept {
        return BaseError("Undef waited literal, but found " + token.value, token)
            .withType(MessageType::PreprocessorError)
            .withCode(22)
            .withNote(NOTE_UNDEF_SYNTAX);
    }

    BaseError UNDEF_NOT_DEFINED(const Token token) noexcept {
        return BaseError("Undef identifier not defined: " + token.value, token)
            .withType(MessageType::PreprocessorError)
            .withCode(23)
            .asWarning()
            .withHint("The identifier was not defined. The undef directive will be ignored.");
    }

    BaseError IFDEF_WAITED_LITERAL(const Token token) noexcept {
        return BaseError("Ifdef waited literal, but found " + token.value, token)
            .withType(MessageType::PreprocessorError)
            .withCode(24)
            .withNote("#ifdef syntax: #ifdef <name> { ... }");
    }

    BaseError IFDEF_WAITED_BRACE_LEFT(const Token token) noexcept {
        return BaseError("Expected '{' after #ifdef condition", token)
            .withType(MessageType::PreprocessorError)
            .withCode(25)
            .withNote("#ifdef syntax: #ifdef <name> { ... }");
    }

    BaseError IFDEF_WAITED_BRACE_RIGHT(const Token token) noexcept {
        return BaseError("Expected '}' to close #ifdef block", token)
            .withType(MessageType::PreprocessorError)
            .withCode(26)
            .withNote("#ifdef syntax: #ifdef <name> { ... }");
    }

    BaseError ELSE_WITHOUT_IFDEF(const Token token) noexcept {
        return BaseError("#else without matching #ifdef", token)
            .withType(MessageType::PreprocessorError)
            .withCode(27)
            .withNote("#else must follow #ifdef <name> { ... }");
    }

    BaseError ELIFDEF_WAITED_LITERAL(const Token token) noexcept {
        return BaseError("Elifdef waited literal, but found " + token.value, token)
            .withType(MessageType::PreprocessorError)
            .withCode(28)
            .withNote("#elifdef syntax: #elifdef <name> { ... }");
    }

    BaseError ELIFDEF_WAITED_BRACE_LEFT(const Token token) noexcept {
        return BaseError("Expected '{' after #elifdef condition", token)
            .withType(MessageType::PreprocessorError)
            .withCode(29)
            .withNote("#elifdef syntax: #elifdef <name> { ... }");
    }

    BaseError ELIFDEF_WAITED_BRACE_RIGHT(const Token token) noexcept {
        return BaseError("Expected '}' to close #elifdef block", token)
            .withType(MessageType::PreprocessorError)
            .withCode(30)
            .withNote("#elifdef syntax: #elifdef <name> { ... }");
    }

    BaseError ELIFDEF_WITHOUT_IFDEF(const Token token) noexcept {
        return BaseError("#elifdef without matching #ifdef", token)
            .withType(MessageType::PreprocessorError)
            .withCode(31)
            .withNote("#elifdef must follow #ifdef or another #elifdef");
    }

    BaseError PASTE_NO_LEFT_OPERAND(const Token paste_token) noexcept {
        return BaseError("Token pasting '$$' has no left operand", paste_token)
            .withType(MessageType::PreprocessorError)
            .withCode(32)
            .withNote(NOTE_TOKEN_PASTING_SYNTAX);
    }

    BaseError PASTE_NO_RIGHT_OPERAND(const Token paste_token) noexcept {
        return BaseError("Token pasting '$$' has no right operand", paste_token)
            .withType(MessageType::PreprocessorError)
            .withCode(33)
            .withNote(NOTE_TOKEN_PASTING_SYNTAX);
    }

    BaseError PASTE_INVALID_LEFT_OPERAND(const Token token) noexcept {
        return BaseError("Token pasting '$$' left operand must be an identifier, but found " + token.value, token)
            .withType(MessageType::PreprocessorError)
            .withCode(34)
            .withNote(NOTE_TOKEN_PASTING_SYNTAX);
    }

    BaseError PASTE_INVALID_RIGHT_OPERAND(const Token token) noexcept {
        return BaseError("Token pasting '$$' right operand must be an identifier or an integer, but found " + token.value, token)
            .withType(MessageType::PreprocessorError)
            .withCode(35)
            .withNote(NOTE_TOKEN_PASTING_SYNTAX);
    }

    BaseError STRINGIFY_EMPTY_GROUP(const Token token) noexcept {
        return BaseError("Stringify group '@()' is empty", token)
            .withType(MessageType::PreprocessorError)
            .withCode(36)
            .withHint("Provide at least one token inside '@(' and ')'.");
    }
}

struct ErrorContainer {
    std::vector<BaseError> errors;

    void addError(const BaseError err) noexcept {
        errors.push_back(err);
    }

    void viewError() {
        for (auto& err: errors) {
            ::viewError(err);
        }
    }
};


namespace SYNTAX_ERRORS {
    BaseError INVALID_NUMBER_FORMAT(const Token token) noexcept {
        return BaseError("Invalid number format", token)
            .withType(MessageType::SyntaxError)
            .withCode(37)
            .withHint("Use a valid numeric literal, e.g. 10, 10.5, .5, 0xFF.")
            .withHint("Hexadecimal literals use the 0x prefix; binary/octal literals are not supported yet.");
    }

    BaseError WAITING(const Token token, String wait) {
        return BaseError("Expected '" + wait + "', but found '" + token.value + "'", token)
            .withType(MessageType::SyntaxError)
            .withCode(38)
            .withHint("Insert '" + wait + "' at this position.");
    }

    BaseError WAITING_TYPE_EXPRESSION(const Token token) {
        return BaseError("Expected a type expression, but found '" + token.value + "'", token)
            .withType(MessageType::SyntaxError)
            .withCode(39)
            .withHint("A type is expected here, e.g. i32, str, bool, or a generic parameter.")
            .withHint("Built-in types: i8, i16, i32, i64, f16, f32, f64, str, char, bool; generic parameters are declared as 'Name: Type'.");
    }

    BaseError TUPLE_REQUIRES_TYPE(const Token token) {
        return BaseError("Tuple type requires at least one element type", token)
            .withType(MessageType::SyntaxError)
            .withCode(150)
            .withNote("Tuple[] is empty, but a tuple must contain at least one element type.")
            .withHint("Provide one or more type expressions, e.g. Tuple[i32], Tuple[i32, str], Tuple[auto].");
    }

    BaseError ARRAY_REQUIRES_TYPE(const Token token) {
        return BaseError("Array type requires an element type", token)
            .withType(MessageType::SyntaxError)
            .withCode(151)
            .withNote("Array[] is empty, but an array must specify its element type.")
            .withHint("Provide the element type, optionally with a length: Array[i32], Array[str, 10], Array[auto].");
    }

    BaseError EXPECTED_EXPRESSION(const Token token) {
        return BaseError("Expected an expression, but found '" + token.value + "'", token)
            .withType(MessageType::SyntaxError)
            .withCode(40)
            .withHint("Provide a value, variable, or function call.");
    }

    BaseError EXPECTED_STATEMENT(const Token token) {
        return BaseError("Expected a statement, but found '" + token.value + "'", token)
            .withType(MessageType::SyntaxError)
            .withCode(149)
            .withNote("A statement was expected here (e.g. 'let', 'fn', 'if', 'while', '{ ... }').")
            .withHint("A statement starts with 'let', 'fn', 'if', 'while', 'for', 'ret', 'outln', 'namespace', 'struct', or a '{ ... }' block.");
    }

    BaseError EXPECTED_STATEMENT_DECLARATION(const Token token) {
        return BaseError("Expected a statement or declaration, but found '" + token.value + "'", token)
            .withType(MessageType::SyntaxError)
            .withCode(41)
            .withHint("Place a declaration or statement after the modifier (e.g. 'let x = ...;' or '{ ... }').")
            .withHint("Valid modifiers: const, static, final, strict, global, public, private.");
    }

    BaseError RETURN_OUTSIDE_FUNCTION(const Token token) noexcept {
        return BaseError("'ret' is not allowed outside of a function", token)
            .withType(MessageType::SyntaxError)
            .withCode(42)
            .withHint("Move the 'ret' statement inside a function body.")
            .withHint("Lambdas return a value via a 'namespace { ... }::member' expression, not via 'ret'.");
    }

    BaseError BREAK_OUTSIDE_LOOP(const Token token) noexcept {
        return BaseError("'break' is not allowed outside of a loop", token)
            .withType(MessageType::SyntaxError)
            .withCode(43)
            .withHint("Use 'break' only inside a 'while' or 'for' loop.")
            .withHint("A 'break' placed here has no loop to exit.");
    }

    BaseError CONTINUE_OUTSIDE_LOOP(const Token token) noexcept {
        return BaseError("'continue' is not allowed outside of a loop", token)
            .withType(MessageType::SyntaxError)
            .withCode(148)
            .withHint("Use 'continue' only inside a 'while' or 'for' loop.")
            .withHint("A 'continue' placed here has no loop to skip to the next iteration.");
    }

    BaseError MISSING_OPERAND(const Token token, const String& op) {
        return BaseError("Expected an operand after '" + op + "', but found '" + token.value + "'", token)
            .withType(MessageType::SyntaxError)
            .withCode(45)
            .withHint("Provide a value or expression after the '" + op + "' operator.");
    }

    BaseError MISSING_CLOSING(const Token token, const String& open, const String& close) {
        return BaseError("Expected closing '" + close + "'", token)
            .withType(MessageType::SyntaxError)
            .withCode(152)
            .withNote("Expected '" + close + "' to close '" + open + "', but found '" + token.value + "'.")
            .withHint("Brackets must be balanced: parentheses (), braces {}, square brackets [].");
    }

    BaseError UNEXPECTED_TOKEN(const Token token) {
        return BaseError("Unexpected token '" + token.value + "'", token)
            .withType(MessageType::SyntaxError)
            .withCode(153)
            .withNote("The token '" + token.value + "' cannot appear in this position.")
            .withHint("Remove this token or move it to a valid position.");
    }

    BaseError UNSUPPORTED_OPERATOR(const Token token, const String& op) {
        return BaseError("Unsupported operator '" + op + "'", token)
            .withType(MessageType::SyntaxError)
            .withCode(52)
            .withHint("This operator is not supported yet.")
            .withHint("Check the language reference for the list of supported operators.");
    }

    BaseError MISSING_VALUE(const Token token) {
        return BaseError("Expected a value after '=', but found '" + token.value + "'", token)
            .withType(MessageType::SyntaxError)
            .withCode(49)
            .withHint("Provide a value or expression after the '=' sign.");
    }

    BaseError POSITIONAL_AFTER_NAMED_ARG(const Token token) {
        return BaseError("Positional argument after a named argument: '" + token.value + "'", token)
            .withType(MessageType::SyntaxError)
            .withCode(155)
            .withHint("Once a named argument ':name = value' appears, all later arguments must also be "
                      "named — a positional argument can no longer be placed reliably.");
    }

    BaseError EXTERN_NOT_GLOBAL(const Token token) {
        return BaseError("External function must be declared in the global scope", token)
            .withType(MessageType::SyntaxError)
            .withCode(154)
            .withNote("'extern fn' is bound to a native C++ symbol by its declaration name, and the "
                      "definition in the #cppinclude'd file is global — a nested declaration would "
                      "mangle to a different symbol than the one the C++ file defines.")
            .withHint("Move the 'extern fn' declaration to the top level of the file, outside any "
                      "namespace, function or struct. Call it through the namespace from the call "
                      "site instead of declaring it inside one.");
    }
}

namespace SEMANTIC_ERRORS {
    BaseError MODIFIER_MISPLACED(const Token token, const String& modifier) noexcept {
        return BaseError("Modifier '" + modifier + "' is misplaced", token)
            .withType(MessageType::SyntaxError)
            .withCode(50)
            .withHint("Place the modifier before a 'let', 'fn', 'struct', 'namespace', or a '{' block.")
            .withHint("Valid modifiers: const, static, final, strict, global, public, private.");
    }

    BaseError DUPLICATE_MODIFIER(const Token token, const String& modifier) noexcept {
        return BaseError("Duplicate modifier '" + modifier + "'", token)
            .withType(MessageType::SyntaxError)
            .withCode(51)
            .withHint("Remove the duplicate '" + modifier + "' modifier.");
    }

    BaseError INFINITE_LOOP(const Token token) noexcept {
        return BaseError("This 'while' loop may run forever", token)
            .withType(MessageType::Warning)
            .withCode(156)
            .asWarning()
            .withHint("The condition is always true and the body contains no 'break' or 'ret'.")
            .withHint("Add an exit (break/ret) inside the loop, or make the condition depend on a value that changes.");
    }

    BaseError STRICT_REQUIRES_TYPE(const Token token) noexcept {
        return BaseError("'strict' variable declaration requires an explicit type", token)
            .withType(MessageType::SyntaxError)
            .withCode(53)
            .withHint("Add a type annotation, e.g. 'strict let x: Type = ...;'.");
    }

    BaseError CONST_REQUIRES_VALUE(const Token token) noexcept {
        return BaseError("'const' variable declaration requires an initial value", token)
            .withType(MessageType::SyntaxError)
            .withCode(54)
            .withHint("Provide an initializer, e.g. 'const let x = 5;'.");
    }

    BaseError SELF_ASSIGNMENT(const Token token_start, const Token token_end) noexcept {
        return BaseError("Self-assignment has no effect", token_start, token_end)
            .withType(MessageType::Warning)
            .withCode(55)
            .asWarning()
            .withHint("The left and right sides refer to the same variable; this assignment does nothing.");
    }

    BaseError EMPTY_OUTPUT(const Token token) noexcept {
        return BaseError("Empty 'out'/'outln' call has no effect", token)
            .withType(MessageType::Warning)
            .withCode(56)
            .asWarning()
            .withHint("Provide at least one argument to print, or remove the call.");
    }

    BaseError NO_ENTRY_POINT(Token token = Token()) noexcept {
        return BaseError("No entry point found", token)
            .withType(MessageType::SyntaxError)
            .withCode(57)
            .withHint("Define a 'fn main()' function as the program entry point.");
    }

    BaseError CONSTANT_CONDITION(const Token token) noexcept {
        return BaseError("Condition is always constant", token)
            .withType(MessageType::Warning)
            .withCode(58)
            .asWarning()
            .withHint("The condition is a compile-time constant; one of the branches is unreachable.");
    }

    BaseError EMPTY_BODY(const Token token) noexcept {
        return BaseError("Empty loop/branch body has no effect", token)
            .withType(MessageType::Warning)
            .withCode(59)
            .asWarning()
            .withHint("The body block is empty; the statement does nothing.");
    }

    BaseError IDENTICAL_BRANCHES(const Token token) noexcept {
        return BaseError("'if' branches are identical", token)
            .withType(MessageType::Warning)
            .withCode(60)
            .asWarning()
            .withHint("The 'then' and 'else' branches contain the same code; the condition is pointless.");
    }

    BaseError DOUBLE_NEGATION(const Token token) noexcept {
        return BaseError("Redundant double negation '!!x'", token)
            .withType(MessageType::Warning)
            .withCode(61)
            .asWarning()
            .withHint("'!!x' is equivalent to 'x' (or to bool(x)); remove one '!'.");
    }

    BaseError MODIFIER_SEMANTICS(const Token token, const String& modifier, const String& hint) noexcept {
        return BaseError("Modifier '" + modifier + "' is not meaningful in this context", token)
            .withType(MessageType::Warning)
            .withCode(62)
            .asWarning()
            .withHint(hint);
    }
}

namespace DEEP_ERRORS {
    // Исполняемые ошибки глубокого анализа (runtime-проверки).
    BaseError DIVISION_BY_ZERO(const Token& token_start, const Token& token_end) noexcept {
        return BaseError("Division by zero", token_start, token_end)
            .withType(MessageType::ExecutableError)
            .withCode(70)
                .withHint("The right-hand operand of the '/' operator must not be zero.");
    }

    BaseError INPUT_RUNTIME(const Token& token_start, const Token& token_end) noexcept {
        return BaseError("input() reads a line from the standard input; statically it evaluates to ''", token_start, token_end)
            .asWarning()
            .withHint("The value is only known at runtime.");
    }

    BaseError INPUT_PROMPT_TYPE(const Token& token, const String& found) noexcept {
        String msg = "Prompt of 'input' must be a string or a char, found type '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(173)
            .withHint("Pass a string literal or a char: 'input(\"Enter name: \")' or 'input('?')'.");
    }

    BaseError EXIT_CODE_NOT_INTEGER(const Token& token, const String& found) noexcept {
        String msg = "Exit code of 'exit' must be an integer, found type '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(175)
            .withHint("Pass an integer expression: 'exit 0;' / 'exit code;' (like the C exit(3)).");
    }

    BaseError APPEND_TARGET_NOT_ARRAY(const Token& token, const String& found) noexcept {
        String msg = "Target of 'append' must be an array, found type '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(177)
            .withHint("The first operand of 'append' must be a mutable array: 'append arr, value;'.");
    }

    BaseError APPEND_TARGET_NOT_LVALUE(const Token& token, const String& found) noexcept {
        String msg = "Target of 'append' must be a mutable lvalue, found '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(179)
            .withHint("Pass a variable, field or indexed element: 'append arr, value;' / 'append p.items, x;'.");
    }

    BaseError RESERVE_TARGET_NOT_ARRAY(const Token& token, const String& found) noexcept {
        String msg = "Target of 'reserve' must be an array, found type '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(137)
            .withHint("The first operand of 'reserve' must be an array: 'reserve arr, 1024;'.");
    }

    BaseError RESERVE_TARGET_NOT_LVALUE(const Token& token, const String& found) noexcept {
        String msg = "Target of 'reserve' must be an array lvalue, found '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(138)
            .withHint("Pass a variable, field or indexed element: 'reserve arr, 1024;' / 'reserve s.items, 64;'.");
    }

    BaseError RESERVE_COUNT_NOT_INTEGER(const Token& token, const String& found) noexcept {
        String msg = "Capacity of 'reserve' must be an integer, found type '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(139)
            .withHint("Pass an integer capacity: 'reserve arr, 1024;'.");
    }

    BaseError RESERVE_COUNT_NEGATIVE(const Token& token, long long count) noexcept {
        String msg = "Capacity of 'reserve' must not be negative, found ";
        msg += String(std::to_string(count));
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(141)
            .withHint("Capacity counts elements in total (it never shrinks): 'reserve arr, 1024;'.");
    }


    BaseError MODULO_BY_ZERO(const Token& token_start, const Token& token_end) noexcept {
        return BaseError("Modulo by zero", token_start, token_end)
            .withType(MessageType::ExecutableError)
            .withCode(140)
            .withHint("The right-hand operand of the '%' operator must not be zero.");
    }

    BaseError UNSUPPORTED_OPERATOR(const Token& token_start, const Token& token_end,
                                  const String& op, const String& lhs, const String& rhs) noexcept {
        String msg = "Unsupported operator '";
        msg += op; msg += "' for "; msg += lhs; msg += " and "; msg += rhs;
        return BaseError(msg, token_start, token_end)
            .withType(MessageType::ExecutableError)
            .withCode(72)
            .withHint("Arithmetic operators require numeric operands.");
    }

    // Неявная арифметика с char ('c + 1', '1 + c', 'c - 1', 'c - c') запрещена:
    // char не участвует в Numeric, а неявное приведение молча скрывало опечатки
    // ('a' + 1 -> 'b'). Строковые операции с char ('"a" + c', 'c * 3') не затронуты.
    BaseError CHAR_ARITHMETIC(const Token& token_start, const Token& token_end,
                              const String& op, const String& lhs, const String& rhs) noexcept {
        String msg = "Implicit arithmetic on 'char' is not allowed: operator '";
        msg += op; msg += "' for "; msg += lhs; msg += " and "; msg += rhs;
        return BaseError(msg, token_start, token_end)
            .withType(MessageType::ExecutableError)
            .withCode(181)
            .withHint("Convert explicitly, e.g. '((c as i64) + 1) as char'. String operations "
                      "with char ('\"a\" + c', 'c * 3') stay available.");
    }

    // Доступ к полю через указатель (`p.name`, где p: *(person)) запрещён:
      // разыменование должно быть явным. `self` — значение, а не указатель,
      // поэтому методы (self.field) продолжают работать.
      BaseError IMPLICIT_POINTER_MEMBER(const Token& token, const String& ptr_type,
                                        const String& field) noexcept {
          String msg = "Cannot access member '";
          msg += field;
          msg += "' of pointer type '";
          msg += ptr_type;
          msg += "': implicit dereference is not allowed";
          return BaseError(msg, token)
              .withType(MessageType::ExecutableError)
              .withCode(182)
              .withHint("Dereference explicitly, e.g. '(*p)." + field + "'.");
      }

      BaseError UNDEFINED_VARIABLE(const Token& token, const String& name) noexcept {
        String msg = "Undefined variable '";
        msg += name; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(73)
            .withHint("Declare the variable before use, e.g. 'let " + name + " = ...;'");
    }

    BaseError FINAL_REDECLARED(const Token& token, const String& name) noexcept {
        String msg = "Cannot redeclare variable '";
        msg += name; msg += "' declared as 'final'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(74)
            .withHint("A 'final' variable can be declared only once.");
    }

    BaseError FINAL_REASSIGNED(const Token& token, const String& name) noexcept {
        String msg = "Cannot reassign variable '";
        msg += name; msg += "' declared as 'final'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(82)
            .withHint("A 'final' variable is immutable; assign it only once at declaration.");
    }



    BaseError ASSIGNMENT_TYPE_MISMATCH(const Token& token, const String& name, const String& expected, const String& found) noexcept {
        String msg = "Type mismatch assigning to '";
        msg += name; msg += "': expected '";
        msg += expected; msg += "', found '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(83)
            .withHint("The assigned value must be compatible with the variable's declared type.");
    }

    BaseError EXPECTED_TYPE_EXPRESSION(const Token& token, const String& found) noexcept {
        String msg = "Expected a type expression, but found an expression of type '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(75)
            .withHint("Use a type name (e.g. i32, str, bool) or a type expression.");
    }

    BaseError STRING_REPEAT_NEGATIVE(const Token& token_start, const Token& token_end) noexcept {
        return BaseError("String repetition count must not be negative", token_start, token_end)
            .withType(MessageType::ExecutableError)
            .withCode(101)
            .withHint("The right-hand operand of 'str * n' must be a non-fractional number >= 0.");
    }

    BaseError STRICT_TYPE_MISMATCH(const Token& token, const String& expected, const String& found) noexcept {
        String msg = "Type mismatch in 'strict' declaration: expected '";
        msg += expected; msg += "', found '"; msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(76)
            .withHint("A 'strict' variable must match its declared type exactly.");
    }

    BaseError STRICT_ANY_POINTLESS(const Token& token) noexcept {
        return BaseError("'strict' with type 'Any' is pointless", token)
            .withType(MessageType::Warning)
            .withCode(77)
            .asWarning()
            .withHint("Use a plain 'let' without 'strict' to get a dynamically-typed variable.");
    }

    BaseError STRICT_AUTO_REQUIRES_VALUE(const Token& token) noexcept {
        return BaseError("A 'strict' variable with type 'auto' requires an initializer", token)
            .withType(MessageType::ExecutableError)
            .withCode(78)
            .withHint("Write 'strict let x: auto = <value>;' so the type can be inferred.");
    }

    BaseError ARRAY_INDEX_NOT_INTEGER(const Token& token, const String& found) noexcept {
        String msg = "Array index must be an integer, found '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(157)
            .withHint("Use a whole number (e.g. i64) as an array/tuple index.");
    }

    BaseError ARRAY_INDEX_OUT_OF_RANGE(const Token& token, const long long index, const long long size) noexcept {
        String msg = "Array index ";
        msg += String(std::to_string(index)); msg += " is out of range (size ";
        msg += String(std::to_string(size)); msg += ")";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(158)
            .withHint("Valid indexes are 0 .. size-1.");
    }

    BaseError ARRAY_SIZE_INVALID(const Token& token, const String& found) noexcept {
        String msg = "Array size must be a positive integer or -1, found '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(103)
            .withHint("Use e.g. 'Array[i32, -1]' (dynamic) or 'Array[i32, 10]' (fixed length).");
    }

    BaseError ARRAY_CTOR_SIZE_MISMATCH(const Token& token, const String& elem_type,
                                       const long long declared, const long long got) noexcept {
        String msg = "Array constructor size mismatch: 'Array[";
        msg += elem_type;
        msg += ", ";
        msg += String(std::to_string(declared));
        msg += "]' expects ";
        msg += String(std::to_string(declared));
        msg += " element(s), got ";
        msg += String(std::to_string(got));
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(185)
            .withHint("List exactly N elements, or drop the size: 'Array[T]{…}' for a dynamic array.");
    }

    BaseError ARRAY_CTOR_ELEMENT_MISMATCH(const Token& token, const String& expected,
                                          const String& found) noexcept {
        String msg = "Array constructor element type mismatch: expected '";
        msg += expected; msg += "', found '"; msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(187)
            .withHint("Convert explicitly ('x as i64') or use a matching element type.");
    }

    BaseError STRUCT_REDECLARED(const Token& token, const String& name) noexcept {
        String msg = "Struct '"; msg += name; msg += "' is declared more than once";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(104)
            .withHint("Give the second declaration a unique name.");
    }

    BaseError STRUCT_NOT_FOUND(const Token& token, const String& name) noexcept {
        String msg = "Unknown struct '"; msg += name; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(105)
            .withHint("Declare the struct before using it.");
    }

    BaseError STRUCT_GENERIC_ARG_COUNT(const Token& token, const String& name, const long long got, const long long expected) noexcept {
        String msg = "Struct '"; msg += name; msg += "' expects ";
        msg += String(std::to_string(expected)); msg += " generic argument(s), found ";
        msg += String(std::to_string(got));
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(106)
            .withHint("Write e.g. 'Vec[i64, 8]' for a struct declared as 'struct[T, Size: i64 = -1] Vec'.");
    }

    BaseError STRUCT_VALUE_GENERIC_TYPE(const Token& token, const String& name, const String& found) noexcept {
        String msg = "Value generic argument '"; msg += name;
        msg += "' must be an integer, found '"; msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(107)
            .withHint("Size/value parameters take an integer literal or constant.");
    }

    BaseError MISSING_REQUIRED_FIELD(const Token& token, const String& field, const String& name) noexcept {
        String msg = "Struct literal for '"; msg += name;
        msg += "' is missing required field '"; msg += field; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(108)
            .withHint("Provide the field or give it a default value in the struct declaration.");
    }

    BaseError NO_SUCH_FIELD(const Token& token, const String& name, const String& field) noexcept {
        String msg = "Struct '"; msg += name; msg += "' has no field '"; msg += field; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(109)
            .withHint("Check the field name against the struct declaration.");
    }

    BaseError DUPLICATE_FIELD(const Token& token, const String& field) noexcept {
        String msg = "Duplicate field '"; msg += field; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(110)
            .withHint("Each field may appear only once.");
    }

    BaseError MEMBER_OF_NON_STRUCT(const Token& token, const String& found, const String& member) noexcept {
        String msg = "Cannot access member '"; msg += member;
        msg += "' of type '"; msg += found; msg += "' (expected a struct)";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(111)
            .withHint("Use '::' for namespaces and '.' for struct members.");
    }

    BaseError METHOD_NOT_FOUND(const Token& token, const String& method, const String& name) noexcept {
        String msg = "Struct '"; msg += name; msg += "' has no method '"; msg += method; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(112)
            .withHint("Declare methods inside the struct body with 'fn name(self, ...)'.");
    }

    BaseError STRUCT_METHOD_REDECLARED(const Token& token, const String& method, const String& name) noexcept {
        String msg = "Method '"; msg += method; msg += "' of struct '"; msg += name;
        msg += "' redefines an existing method";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(113)
            .withHint("Method names inside one struct must be unique.");
    }

    BaseError EXPECTED_STRUCT_TYPE(const Token& token, const String& found) noexcept {
        String msg = "Struct literal requires a struct type name, found '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(114)
            .withHint("Write an instance as 'Name { field: value, ... }' or 'Name[args] { ... }'.");
    }

    BaseError PRIVATE_FIELD(const Token& token, const String& name, const String& field) noexcept {
        String msg = "Field '"; msg += field;
        msg += "' of struct '"; msg += name;
        msg += "' is private and cannot be accessed outside the struct body";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(115)
            .withHint("Private fields are visible only inside methods of the same struct.");
    }

    BaseError CONST_FN_MUTATION(const Token& token, const String& member) noexcept {
        String msg = "Cannot assign to member '";
        msg += member; msg += "' from a const function";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(116)
            .withHint("A 'const fn' provides a read-only view of the receiver.");
    }

    BaseError INSTANCE_METHOD_SCOPE_CALL(const Token& token, const String& method,
                                         const String& name) noexcept {
        String msg = "Cannot call instance method '";
        msg += method; msg += "' of struct '"; msg += name;
        msg += "' through '::' (static access): it requires a receiver";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(118)
            .withHint("Call the method on an instance value instead: 'value."
                      + method + "(...)'.");
    }
    BaseError INSTANCE_FIELD_SCOPE_CALL(const Token& token, const String& field,
                                        const String& name) noexcept {
        String msg = "Instance field '";
        msg += field; msg += "' of struct '"; msg += name;
        msg += "' cannot be accessed through '::' (static access)";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(171)
            .withHint("Access the field on an instance value instead: 'value."
                      + field + "'.");
    }

    BaseError CONST_FN_INVOKES_MUTATOR(const Token& token, const String& method) noexcept {
        String msg = "Const function cannot call mutating method '";
        msg += method; msg += "' on the receiver";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(117)
            .withHint("A 'const fn' may only call other const methods of the same struct.");
    }

    BaseError CANNOT_INDEX(const Token& token, const String& found) noexcept {
        String msg = "Cannot index a value of type '";
        msg += found; msg += "' (expected an array or tuple)";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(161)
            .withHint("Indexing is supported for arrays ([]) and tuples.");
    }

    BaseError ASSIGN_TO_EXPRESSION(const Token& token, const String& found) noexcept {
        String msg = "Cannot assign to '";
        msg += found; msg += "'[]: the target is not a variable";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(163)
            .withHint("Use a variable, namespace member, or dereference as the index-assign target.");
    }

    BaseError STRING_INDEX_IMMUTABLE(const Token& token) noexcept {
        const String msg = "Strings are immutable: cannot assign to a character at an index";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(63)
            .withHint("Read a character with s[i]; build a modified string via concatenation.");
    }

    BaseError NEW_UNKNOWN_MODIFIER(const Token& token, const String& found) noexcept {
        String msg = "Unknown 'new' modifier '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(64)
            .withHint("Supported modifiers: const, strict, strict(type).");
    }

    BaseError NEW_EXPECTED_TYPE(const Token& token, const String& found) noexcept {
        String msg = "'new <strict(...)>' requires a type expression, found '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(65)
            .withHint("Use e.g. 'new <strict(i32)>', 'new <strict(Array[i32])>' or 'new <strict> someValue'.");
    }

    BaseError UNARY_NEGETATION_NOT_INVALID_TYPE(const Token& token, const String& found) noexcept {
        String msg = "Operator '!' cannot be applied to type '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(159)
            .withHint("Use '?' on type expressions to create nullable types (e.g. ?i64).");
    }

    BaseError UNARY_MINUS_INVALID_TYPE(const Token& token, const String& found) noexcept {
        String msg = "Unary '-' cannot be applied to type '";
        msg += found; msg += "' (expected a numeric type)";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(167)
            .withHint("Use unary '-' only on numeric values (i8, i16, i32, i64, f32, f64).");
    }

    BaseError UNARY_BITWISE_NOT_INVALID_TYPE(const Token& token, const String& found) noexcept {
        String msg = "Operator '~' cannot be applied to type '";
        msg += found; msg += "' (expected an integer type)";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(169)
            .withHint("Use '~' only on integer values (i8, i16, i32, i64).");
    }

    BaseError UNARY_DEREFERENCE_NOT_INVALID_TYPE(const Token& token, const String& found) noexcept {
        String msg = "Operator '*' cannot be applied to type '";
        msg += found; msg += "' (expected a pointer type)";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(160)
            .withHint("You can dereference a pointer type using '*' operator.")
            .withHint("And can create a pointer type using *<type>.");
    }

    BaseError BINARY_COMPARE_INVALID_TYPE(const Token& start, const Token& end, const String& left, const String& right, const String& op) noexcept {
        String msg;
        if (op == "==" || op == "!=") {
            msg = "Value of type '";
            msg += left; msg += "' does not support equality comparison.";
        } else {
            msg = "Values of type '";
            msg += left; msg += "' cannot be compared with '";
            msg += op; msg += "' against type '";
            msg += right; msg += "'";
        }
        String hint = "Only numeric and string values support comparison operators.";
        if (op == "==" || op == "!=")
            hint = "Only comparable types support equality (numeric, bool, char, string, null, type).";
        return BaseError(msg, start, end)
            .withType(MessageType::ExecutableError)
            .withCode(81)
            .withHint(hint);
    }

    // Правая сторона '??' несовместима с типом результата (левая без Null).
    BaseError NULL_COALESCE_TYPE_MISMATCH(const Token& start, const Token& end,
                                          const String& left, const String& right, const String& result) noexcept {
        String msg = "Right operand of '??' has type '";
        msg += right; msg += "', which is not compatible with the result type '";
        msg += result; msg += "' (type of '";
        msg += left; msg += "' without 'Null')";
        return BaseError(msg, start, end)
            .withType(MessageType::ExecutableError)
            .withCode(142)
            .withHint("'a ?? b' has the type of 'a' with 'Null' removed, so both sides must agree.")
            .withHint("Use an explicit cast on the right operand, e.g. 'a ?? b as <type>'.");
    }

    BaseError NULL_DEREFERENCE(const Token& token) noexcept {
        return BaseError("Cannot dereference a null pointer", token)
            .withType(MessageType::ExecutableError)
            .withCode(164)
            .withHint("Make sure the pointer was initialized (e.g. 'let p = &x;') before dereferencing.");
    }

    BaseError ADDRESS_OF_INVALID(const Token& token, const String& found) noexcept {
        String msg = "Cannot take the address of an expression of type '";
        msg += found; msg += "' (not an lvalue)";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(85)
            .withHint("Use '&' on a variable, e.g. 'let p = &x;'.");
    }

    BaseError INVALID_CAST(const Token& token, const String& from, const String& to) noexcept {
        String msg = "Cannot cast value of type '";
        msg += from; msg += "' to '"; msg += to; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(165)
            .withHint("The 'as' operator supports numeric conversions, bool conversion and pointer reinterpretation."
                      " If the source type has a '__as__(T)' method, it is used as a custom conversion.");
    }

    BaseError SCOPE_RESOLUTION_NOT_NAMESPACE(const Token& token, const String& found) noexcept {
        String msg = "Left side of '::' is not a namespace (found type '";
        msg += found; msg += "')";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(166)
            .withHint("Use '::' only on a namespace, e.g. 'MyNamespace::member'.");
    }

    BaseError SCOPE_RESOLUTION_INVALID_MEMBER(const Token& token, const String& name) noexcept {
        String msg = "Namespace has no member '";
        msg += name; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(87)
            .withHint("Check the namespace body for the declared member name.");
    }

    BaseError PRIVATE_ACCESS(const Token& token, const String& name) noexcept {
        String msg = "Cannot access private member '";
        msg += name; msg += "' from outside the namespace";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(102)
            .withHint("Private members of a namespace are accessible only inside its body, not via '::'.");
    }

    BaseError SIZEOF_INVALID_TYPE(const Token& token, const String& found) noexcept {
        String msg = "'sizeof' expects Array, Tuple, String or Char, found type '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(172)
            .withHint("Use 'sizeof' on an Array, Tuple, String or Char to get its length.");
    }

    BaseError PTR_INVALID_ADDRESS(const Token& token, const String& found) noexcept {
        String msg = "first argument of 'ptr' must be a non-negative integer address, found '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(176)
            .withHint("Use 'ptr(addr, Type)' with an integer address (e.g. 'ptr(0x1000, i64)').");
    }

    BaseError DELETE_INVALID_TYPE(const Token& token, const String& found) noexcept {
        String msg = "'del / delete' expects an Array or Tuple host, found type '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(123)
            .withHint("Use 'del host, index;' where host is an Array or Tuple variable.");
    }

    BaseError DELETE_WITHOUT_INDEX(const Token& token, const String& found) noexcept {
        String msg = "'del / delete' requires an index for type '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(124)
            .withHint("Write 'del host, index;' to remove the element at index.");
    }

    BaseError ASSERT_CONDITION_NOT_BOOL(const Token& token, const String& found) noexcept {
        String msg = "Condition of 'assert' must be a boolean (or numeric), found type '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(125)
            .withHint("Use a boolean expression, e.g. 'assert x > 0, \"msg\";'.");
    }

    BaseError ASSERT_MESSAGE_TYPE(const Token& token, const String& found) noexcept {
        String msg = "Message of 'assert' must be a string or a char, found type '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(126)
            .withHint("Pass a string literal or a char after the comma: 'assert cond, \"msg\";'.");
    }

    // Диагноз привязывается ко ВСЕМУ выражению-вызову (`a / b` целиком), а не
    // к одному токену: у вызова есть начало (имя/оператор) и конец (`)` или
    // последний операнд), и подсветка должна покрывать их оба.
    BaseError ASSERT_FAILED(const Token& token_start, const Token& token_end, const String& message) noexcept {
        String msg = String("Assertion failed");
        if (message.size() > 0) {
            msg += String(": '");
            msg += message; msg += String("'");
        }
        return BaseError(msg, token_start, token_end)
            .withType(MessageType::ExecutableError)
            .withCode(127)
            .asWarning()
            .withCallSiteChain()
            .withHint("The asserted condition evaluated to false at runtime.");
    }

    BaseError ASSERT_FAILED(const Token& token, const String& message) noexcept {
        return ASSERT_FAILED(token, token, message);
    }

    BaseError ENUM_REDECLARED(const Token& token, const String& name) noexcept {
        String msg = "Enum '";
        msg += name; msg += "' is declared more than once";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(121)
            .withHint("Give the second declaration a unique name.");
    }

    BaseError ENUM_VARIANT_NOT_FOUND(const Token& token, const String& name,
                                     const String& enum_name) noexcept {
        String msg = "Enum '";
        msg += enum_name; msg += "' has no variant '"; msg += name; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(122)
            .withHint("Declare the variant inside the enum body, e.g. 'enum "
                      + enum_name + " { " + name + " }'.");
    }

    BaseError IF_CONDITION_NOT_BOOL(const Token& token, const String& found) noexcept {
        String msg = "Condition of 'if' must be a boolean (or numeric), found type '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(168)
            .withHint("Use a boolean expression, e.g. 'if (x > 0) { ... }'.");
    }

    BaseError LOOP_CONDITION_NOT_BOOL(const Token& token, const String& found) noexcept {
        String msg = "Loop condition must be a boolean (or numeric), found type '";
        msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(170)
            .withHint("Use a boolean expression, e.g. 'while (i < n) { ... }'.");
    }

    BaseError LOOP_NOT_TERMINATING(const Token& token) noexcept {
        String msg = "Loop exceeded the analysis iteration budget (possible non-termination)";
        return BaseError(msg, token)
            .withType(MessageType::Warning)
            .withCode(90)
            .withHint("The analyzer stops loops after a fixed number of iterations to avoid hanging.")
            .asWarning();
    }

    BaseError CONST_REASSIGNED(const Token& token, const String& name) noexcept {
        String msg = "Cannot reassign variable '";
        msg += name; msg += "' declared as 'const'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(91)
            .withHint("A 'const' variable is immutable; assign it only once at declaration.");
    }

    BaseError FUNCTION_NOT_FOUND(const Token& token, const String& name) noexcept {
        String msg = "Undefined function '";
        msg += name; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(92)
            .withHint("Declare the function before the call, or check the spelling.");
    }

    BaseError NO_MATCHING_OVERLOAD(const Token& start, const Token& end, const String& name, const String& args) noexcept {
        String msg = "No matching overload of '";
        msg += name; msg += "' for arguments (";
        msg += args; msg += ")";
        return BaseError(msg, start, end)
            .withType(MessageType::ExecutableError)
            .withCode(93)
            .withHint("Check the argument types; add an overload with an exact matching signature.");
    }

    BaseError AMBIGUOUS_CALL(const Token& start, const Token& end, const String& name) noexcept {
        String msg = "Call of '";
        msg += name; msg += "' is ambiguous";
        return BaseError(msg, start, end)
            .withType(MessageType::ExecutableError)
            .withCode(94)
            .withHint("Several overloads match equally well; add an exact overload or cast the arguments.");
    }

    BaseError CALL_WITHOUT_FUNCTION(const Token& token, const String& found) noexcept {
        String msg = "Call attempted on a value of type '";
        msg += found; msg += "', which is not a function";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(95)
            .withHint("Only functions can be called with '( ... )'.");
    }

    BaseError CALL_RECURSION_LIMIT(const Token& token, const String& name) noexcept {
        String msg = "Call of '";
        msg += name; msg += "' exceeded the recursion limit";
        return BaseError(msg, token)
            .withType(MessageType::Warning)
            .withCode(96)
            .withHint("Possible unbounded recursion; the analyzer stops after a fixed call depth.")
            .asWarning();
    }

    BaseError CALL_BUDGET_EXHAUSTED(const Token& token, const String& name) noexcept {
        String msg = "Analysis of '";
        msg += name; msg += "' exceeded the call budget";
        return BaseError(msg, token)
            .withType(MessageType::Warning)
            .withCode(120)
            .withHint("Deep analysis is best-effort: heavy recursion is truncated here and the "
                      "interpreted value is not the real runtime value. The runtime still computes "
                      "the exact result.")
            .asWarning();
    }

    BaseError CALL_DUPLICATE_SIGNATURE(const Token& token, const String& name, const String& signature) noexcept {
        String msg = "Function '";
        msg += name; msg += "' already has an overload with signature (";
        msg += signature; msg += ")";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(97)
            .withHint("Overloads must differ by arity or by at least one parameter type.");
    }

    BaseError RETURN_TYPE_MISMATCH(const Token& token, const String& expected, const String& found) noexcept {
        String msg = "Return type mismatch: expected '";
        msg += expected; msg += "', found '"; msg += found; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(98)
            .withHint("The 'ret' expression must match the declared '-> type' of the function.");
    }

    BaseError FUNCTION_STRICT_REASSIGNED(const Token& token, const String& name) noexcept {
        String msg = "Function '";
        msg += name; msg += "' is declared 'strict' and cannot be reassigned";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(99)
            .withHint("Remove the 'strict' modifier, or use a different variable name.");
    }

    BaseError MISSING_GENERIC_ARGUMENT(const Token& token, const String& generic, const String& fn) noexcept {
        String msg = "Call '";
        msg += fn; msg += "' requires generic argument '"; msg += generic; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(100)
            .withHint("Value generics are not inferred from call arguments. Pass it explicitly: "
                      "fn[generic](...), or declare a default value: [generic: type = default].");
    }

    BaseError REF_RVALUE(const Token& token, const Token& ref_token, const String& name) noexcept {
        String msg = "ref-";
        String hint;
        if (name == "return") {
            msg += "function '" + name + "' must return an l-value (a variable, struct field, array element or dereference), not a temporary value";
            hint = "A ref-return creates a live alias: return an assignable path, e.g. 'ret self.field;', 'ret arr[i];', 'ret global;'.";
        } else {
            msg += "arg '";
            msg += name; msg += "' must be an l-value (variable, struct field, array element or dereference), not a temporary value";
            hint = "Pass an assignable path: 'inc(a)', 'inc(p.x)', 'inc(arr[0])', 'inc(*ptr)'.";
        }
        return BaseError(msg, ref_token.type == TypeOfToken::DUMMY ? token : ref_token)
            .withType(MessageType::ExecutableError)
            .withCode(190)
            .withHint(hint);
    }

    BaseError VALUE_RETURNS_REF(const Token& token) noexcept {
        String msg = "Value-returning function must not return a live ref (it would silently "
                     "dereference into a copy, losing the alias)";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(205)
            .withHint("A live ref comes from a '-> ref ...' callee or a 'ref' variable/parameter. "
                      "Declare '-> ref T' to return it as a live alias, or dereference explicitly "
                      "to return the value.");
    }

    BaseError REF_NEEDS_ARG(const Token& token, const String& name) noexcept {
        String msg = "Function cannot be called without argument for ref-parameter '";
        msg += name; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(202)
            .withHint("A ref-parameter has no meaningful default: pass a variable/field/element to mutate, "
                      "or remove 'ref' from that parameter.");
    }

    BaseError REF_NEEDS_INIT(const Token& token, const String& name) noexcept {
        String msg = "ref-declaration '";
        msg += name; msg += "' must be initialized: 'ref let ";
        msg += name; msg += " = <lvalue>;'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(191)
            .withHint("A reference must point somewhere from the very beginning; "
                      "Example: 'ref let r = y;' — from now on 'r' is an alias of 'y'.");
    }

    BaseError REF_SELF_ALIAS(const Token& token, const String& name) noexcept {
        String msg = "ref-declaration 'ref let ";
        msg += name; msg += " = ";
        msg += name; msg += ";' creates an alias that points to itself";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(204)
            .withHint("An alias cannot reference its own declaration; "
                      "point it at a different variable: 'ref let r = other;'.");
    }

    BaseError REF_CYCLE(const Token& token, const String& name) noexcept {
        String msg = "cyclic ref-alias detected while resolving '";
        msg += name; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(200)
            .withHint("ref-aliases form a cycle, e.g. 'ref let a = b; ref let b = a;'. "
                      "Break the cycle so every alias ultimately points to a real variable.");
    }

    BaseError REF_ARG_NOT_VARIABLE(const Token& token, const String& fname, const String& pname) noexcept {
        String msg = "reference argument of '";
        msg += fname; msg += "("; msg += pname;
        msg += ": ...)' must be a variable or an lvalue path (field/element)";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(178)
            .withHint("A 'ref' parameter is bound to the caller's live cell, so pass a plain "
                      "variable, a field path ('s.field'), or an element ('arr[i]'). Other "
                      "expressions (rvalues, '*ptr', 'Ns::x') are not addressable here.");
    }

BaseError ADDRESS_OF_MEMBER_UNSUPPORTED(const Token& token, const String& what) noexcept {
          String msg = "address of ";
          msg += what; msg += " is not supported (a target of '&' must be a variable, "
                 "a 'ref' parameter, a 'ref let' alias, or one field of a variable: "
                 "'&obj.field')";
          return BaseError(msg, token)
              .withType(MessageType::ExecutableError)
              .withCode(180)
              .withHint("A single field of a variable has a live cell and can be addressed, "
                        "but a nested field ('&o.a.b') or a temporary has no addressable "
                        "location: store it in a variable first ('let t = o.a; let p = &t.b;').");
      }

    BaseError POINTER_ESCAPES_SCOPE(const Token& token, const String& what) noexcept {
        String msg = "pointer to a function-local value ";
        msg += what; msg += " escapes its scope — the target cell dies when the function returns";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(131)
            .withHint("A pointer/ref captured from '&local' (or from a 'ref' parameter bound to a "
                      "local) is returned from or stored beyond the function. The runtime targets a "
                      "cell of the function's frame, which is destroyed on return. Move the value to "
                      "the heap first: 'let local = <value>; let box = new(local);' and keep the "
                      "pointer to the heap cell.");
    }

    BaseError NAMED_ARG_UNKNOWN(const Token& token, const String& fname, const String& arg) noexcept {
        String msg = "Function '";
        msg += fname; msg += "' has no parameter '"; msg += arg; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(132)
            .withHint("A named argument ':name = value' must match a declared parameter of the "
                      "chosen overload; overloads may name their parameters differently.");
    }

BaseError NAMED_ARG_DUPLICATE(const Token& token, const String& fname, const String& arg) noexcept {
        String msg = "Argument '";
        msg += arg; msg += "' was passed more than once to '"; msg += fname; msg += "'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(133)
            .withHint("Named arguments must be unique: pass each parameter at most once: 'f(:a = 1, :b = 2)'.");
    }

BaseError NAMED_ARG_ON_DYNAMIC_CALL(const Token& token, const String& fname) noexcept {
        String msg = "Named arguments require a statically known callee, but '";
        msg += fname; msg += "' is called dynamically (as a function value)";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(134)
            .withHint("Named arguments are resolved against the declaration at compile time. Call the "
                      "function by its declared name, or assign the value to a typed 'Func[..., T]' "
                      "parameter/variable before calling.");
    }

    BaseError EXTERN_EXECUTED(const Token& token, const String& fname) noexcept {
        String msg = "External C++ function '";
        msg += fname; msg += "' cannot be interpreted";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(135)
            .withHint("'-r' runs the Lumen interpreter and cannot invoke native C++ code "
                      "(#cppinclude). Compile and run the program with '-build' instead.");
    }

    BaseError EXTERN_REDECLARED(const Token& token, const String& fname) noexcept {
        String msg = "External C++ function '";
        msg += fname; msg += "' is declared more than once";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(136)
            .withHint("Declare each extern fn exactly once; the native definition comes from the "
                      "#cppinclude'd C++ file.");
    }

    BaseError PRIVATE_STATIC_FIELD(const Token& token, const String& fname) noexcept {
        String msg = "'private' has no effect on the static field '";
        msg += fname; msg += "'";
        return BaseError(msg, token)
            .asWarning()
            .withHint("A static field lives on the type, not on an instance, so it is reached "
                      "through 'Struct::" + fname + "' and is visible to any code that can name the "
                      "struct. Drop 'private' or move the value into a private instance field.");
    }
} // namespace DEEP_ERRORS

namespace SEMANTIC_ERRORS {
    // ——— Ссылки (`ref`) ———

    BaseError REF_RVALUE(const Token& token, const String& name) noexcept {
        String msg = "ref-argument '";
        msg += name; msg += "' must be an l-value (variable, field, array element or dereference), not a temporary";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(201)
            .withHint("Pass an assignable path: a variable ('fn inc(ref x: i32); inc(a)'), "
                      "a struct field ('inc(p.x)'), an array element ('inc(arr[0])'), "
                      "or a dereference ('inc(*p)').");
    }

    BaseError REF_NEEDS_INIT(const Token& token, const String& name) noexcept {
        String msg = "ref-variable '";
        msg += name; msg += "' must be initialized with an l-value: 'ref let ";
        msg += name; msg += " = <lvalue>;'";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(183)
            .withHint("A reference must point somewhere from the very beginning. "
                      "Example: 'ref let r = y;' — from now on 'r' is an alias of 'y'.");
    }

    BaseError REF_BAD_TARGET(const Token& token, const String& name) noexcept {
        String msg = "ref-variable '";
        msg += name; msg += "' must reference an l-value, not a rvalue call result or literal";
        return BaseError(msg, token)
            .withType(MessageType::ExecutableError)
            .withCode(186)
            .withHint("Only assignable paths can be referenced: variables, struct fields, "
                      "array elements and dereferences.");
    }
}
