#include "../utils/Strings.cpp"
#include "vector"
#pragma once

const static String ALPHABET =      String("abcdefghijklmnopqrstuvwxyz") + String("abcdefghijklmnopqrstuvwxyz").to_upper() + String("абвгдеёжзийклмнопрстуфхцчшщъыьэюя") + String("абвгдеёжзийклмнопрстуфхцчшщъыьэюя").to_upper();
const static String NUMBERS =       "0123456789";
const static String OPERATORS =     "!~@$%&^*/|+-=.,:;<>?";
const static String SCOPES =        "{[()]}";

namespace Literals {
    const static std:: vector<String> KEYWORDS = {
        "let",
        "for",
        "while",
        "break",
        "continue",
        "ret",
        "true",
        "false",
        "null",
        "do",
        "if",
        "else",
        "struct",
        "enum",
        "namespace",
        "new",
        "out",
        "outln",
        "echo",
        "input",
        "del",
        "fn",
        "Func",
        "Tuple",
        "Array",
        "lambda",
        "assert",
        "as",
        "exit",
        "append",
        "reserve",
        "typeof",
        "sizeof",
        "ptr",
        "extern"
    } ;

    const static std:: vector<String> MODIFIERS = {
        "static",
        "const",
        "final",
        "strict",
        "global",
        "private",
        "public",
        "ref"
    };
}
