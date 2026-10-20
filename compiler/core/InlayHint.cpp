#pragma once
#include "../utils/Strings.cpp"

// Инлайн-хинт редактора: позиция (строка/символ) и текст метки.
// Общий тип для двух движков:
//   • ASTDeepAnalyzer (интерпретатор) — типы `auto` и значения `echo`;
//   • TypeAnalyzer (статика)        — выведенные типы без исполнения.
struct InlayHintData {
    int line;
    int character;
    String label;
};
