#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include "C:\Users\Ivan\Desktop\Lumen\compiler\runtime\lumen_rt.h"

// Модуль преобразований строк и чисел (std/Convert.lm).
// Экстерны регистрируются компилятором как lf_<имя> по образцу std/_Math.cpp.
//
// Разбор чисел делегирован в libc: strtoll для целых, strtod для дробных.
// Оба сообщают позицию первого непрочитанного символа (endptr): мусор после
// числа означает, что строка не является числом. ERANGE — переполнение.


// to_int(s)
// Принимает:  s (str | char) — десятичная запись числа: [знак]цифры, окружающие
//                      пробелы допускаются.
// Возвращает: i64 — разобранное число; null, если строка не является целым
//             числом или переполняет i64.
rt::Box lf_to_raf_int(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    std::string s;
    if (args_[0].t == rt::Box::T_STR) {
        s = args_[0].s;
    }
    else if (args_[0].t == rt::Box::T_CHAR) {
        s = std::string(1, args_[0].c);   // символ -> строка из одного символа
    }
    else {
        return rt::Box::i64(0);
    }

    // Окружающие пробелы отбрасываем: strtoll пропускает только ведущие.
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    s = s.substr(b, e - b);
    if (s.empty()) return rt::Box::null();

    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return rt::Box::null();  // мусор после числа
    if (errno == ERANGE) return rt::Box::null();                  // переполнение i64
    return rt::Box::i64(v);
}

// to_float(s)
// Принимает:  s (str | char) — [знак]цифры[.цифры][e-экспонента], окружающие
//                      пробелы допускаются.
// Возвращает: f64 — разобранное число; null, если строка не является числом
//             или переполняется в бесконечность.
rt::Box lf_to_raf_float(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    std::string s;
    if (args_[0].t == rt::Box::T_STR) {
        s = args_[0].s;
    }
    else if (args_[0].t == rt::Box::T_CHAR) {
        s = std::string(1, args_[0].c);   // символ -> строка из одного символа
    }
    else {
        return rt::Box::f64(0.0);
    }

    // Окружающие пробелы отбрасываем: strtod пропускает только ведущие.
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    s = s.substr(b, e - b);
    if (s.empty()) return rt::Box::null();

    char* end = nullptr;
    errno = 0;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return rt::Box::null();  // мусор после числа
    if (errno == ERANGE && !std::isfinite(v)) return rt::Box::null();  // переполнение в бесконечность
    return rt::Box::f64(v);
}

// to_str(v)
// Принимает:  v (любой тип) — ссылочные ячейки и heap-указатели
//                      разыменовываются, дальше форматируется само значение.
// Возвращает: str — представление в том же формате, что печатает outln
//             (целые без дробной части, дробные — 6 знаков, str как есть,
//             bool как "true"/"false", null как "null", массив как {a, b},
//             кортеж как [a, b], структура как object(Имя), замыкание как "fn").
//
// Обёртки (CompileTime и подобные) НЕ разворачиваются: to_str(to_int("10"))
// даст "object(CompileTime)". Для значения используйте развёрнутую форму —
// to_str(to_int("10")?).
rt::Box lf_to_raf_str(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::str("");
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    return rt::Box::str(rt::fmt(a));
}
