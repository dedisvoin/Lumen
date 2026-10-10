#include "Strings.cpp"
#include "iostream"
#include <Windows.h>
#pragma once

namespace TerminalStyle {
    const String RESET   = "\033[0m";
    const String BOLD    = "\033[1m";
    const String DIM     = "\033[2m";
    const String UNDERLINE = "\033[4m";

    const String BLACK   = "\033[30m";
    const String RED     = "\033[31m";
    const String GREEN   = "\033[32m";
    const String YELLOW  = "\033[33m";
    const String BLUE    = "\033[34m";
    const String MAGENTA = "\033[35m";
    const String CYAN    = "\033[36m";
    const String WHITE   = "\033[37m";

    // Яркие (bright) версии
    const String BRIGHT_RED    = "\033[91m";
    const String BRIGHT_GREEN  = "\033[92m";
    const String BRIGHT_YELLOW = "\033[93m";
    const String BRIGHT_BLUE   = "\033[94m";
    const String BRIGHT_CYAN   = "\033[96m";

    // Темные (dim) версии
    const String DIM_BLACK   = "\033[2;30m";
    const String DIM_RED     = "\033[2;31m";
    const String DIM_GREEN   = "\033[2;32m";
    const String DIM_YELLOW  = "\033[2;33m";
    const String DIM_BLUE    = "\033[2;34m";
    const String DIM_MAGENTA = "\033[2;35m";
    const String DIM_CYAN    = "\033[2;36m";
    const String DIM_WHITE   = "\033[2;37m";

    const String ITALIC = "\033[3m";
}
typedef const String TerminalStyleType;

void ResetStyle() noexcept {
    std:: cout << TerminalStyle:: RESET;
}

String Style(String str, TerminalStyleType style) noexcept {
    return style + str + TerminalStyle:: RESET;
}
