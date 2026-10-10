#include <cmath>
#include <cstdlib>
#include "C:\Users\Ivan\Desktop\Lumen\compiler\runtime\lumen_rt.h"

// Модуль математических функций (std/Math.lm).
// Экстерны регистрируются компилятором как lf_<имя> по образцу std/_Time.cpp.

// true, если в аргументе лежит целочисленный тип (i8..i64)
static inline bool lf_is_int(const rt::Box& v) {
    return v.t >= rt::Box::T_i8 && v.t <= rt::Box::T_i64;
}

// CompileTime{ value, type } wrapper: math externs return the box with its
// runtime type (mirror of the .lm fallback bodies CompileTime::from[...]).
static inline rt::Box lf_ct(rt::Box val, rt::Box type_val) {
    return rt::Box::strct("CompileTime", std::map<std::string, rt::Box>{
        { "value", val }, { "type", type_val } });
}

// math.abs(x)
// Принимает:  x (i8..i64|f32|f64) — произвольное число.
// Возвращает: модуль x; тип результата повторяет тип аргумента
//             (целое -> i64, дробное -> f64);
//             null, если аргумент отсутствует или не является числом.
rt::Box lf_abs(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    if (!rt::is_num(a)) return rt::Box::null();
    if (lf_is_int(a)) return lf_ct(rt::Box::i64(std::llabs(a.i)), rt::Box::typ(rt::tyname(a)));
    return lf_ct(rt::Box::f64(std::fabs(a.f)), rt::Box::typ(rt::tyname(a)));
}

// math.clamp(x, lo, hi)
// Принимает:  x, lo, hi (i8..i64|f32|f64) — число и границы диапазона.
// Возвращает: x, «зажатое» в [lo, hi]: x < lo -> lo, x > hi -> hi,
//             иначе x. Тип результата совпадает с типом x.
//             null, если аргументов меньше трёх или любой из них не число.
rt::Box lf_clamp(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.size() < 3) return rt::Box::null();
    rt::Box x  = rt::deref(rt::derefval(args_[0]));
    rt::Box lo = rt::deref(rt::derefval(args_[1]));
    rt::Box hi = rt::deref(rt::derefval(args_[2]));
    if (!rt::is_num(x) || !rt::is_num(lo) || !rt::is_num(hi)) return rt::Box::null();
    double low  = std::min(rt::num(lo), rt::num(hi));
    double high = std::max(rt::num(lo), rt::num(hi));
    double v = std::max(low, std::min(rt::num(x), high));
    if (lf_is_int(x)) return lf_ct(rt::Box::i64((long long)v), rt::Box::typ("f64"));
    return lf_ct(rt::Box::f64(v), rt::Box::typ("f64"));
}

// math.sign(x)
// Принимает:  x (i8..i64|f32|f64) — произвольное число.
// Возвращает: i64 — -1, если x < 0; 1, если x > 0; 0, если x == 0;
//             null, если аргумент отсутствует или не является числом.
rt::Box lf_sign(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    if (!rt::is_num(a)) return rt::Box::null();
    double v = rt::num(a);
    return lf_ct(rt::Box::i64((v > 0) ? 1 : (v < 0) ? -1 : 0), rt::Box::typ(rt::tyname(a)));
}

// math.sqrt(x)
// Принимает:  x (i8..i64|f32|f64) — неотрицательное число.
// Возвращает: f64 — квадратный корень из x (для отрицательных x — NaN);
//             null, если аргумент отсутствует или не является числом.
rt::Box lf_sqrt(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    if (!rt::is_num(a)) return rt::Box::null();
    return lf_ct(rt::Box::f64(std::sqrt(rt::num(a))), rt::Box::typ(rt::tyname(a)));
}

// math.pow(base, exp)
// Принимает:  base, exp (i8..i64|f32|f64) — основание и показатель степени.
// Возвращает: f64 — base, возведённое в степень exp;
//             null, если аргументов меньше двух или любой из них не число.
rt::Box lf_pow(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.size() < 2) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    rt::Box b = rt::deref(rt::derefval(args_[1]));
    if (!rt::is_num(a) || !rt::is_num(b)) return rt::Box::null();
    return lf_ct(rt::Box::f64(std::pow(rt::num(a), rt::num(b))), rt::Box::typ(rt::tyname(a)));
}

// math.ln(x)
rt::Box lf_ln(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    if (!rt::is_num(a)) return rt::Box::null();
    return rt::Box::f64(std::log(rt::num(a)));
}

// math.log10(x)
rt::Box lf_log10(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    if (!rt::is_num(a)) return rt::Box::null();
    return rt::Box::f64(std::log10(rt::num(a)));
}

// math.log2(x)
rt::Box lf_log2(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    if (!rt::is_num(a)) return rt::Box::null();
    return rt::Box::f64(std::log2(rt::num(a)));
}

// math.exp(x)
rt::Box lf_exp(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    if (!rt::is_num(a)) return rt::Box::null();
    return rt::Box::f64(std::exp(rt::num(a)));
}

// math.floor(x)
rt::Box lf_floor(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    if (!rt::is_num(a)) return rt::Box::null();
    return rt::Box::f64(std::floor(rt::num(a)));
}

// math.ceil(x)
rt::Box lf_ceil(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    if (!rt::is_num(a)) return rt::Box::null();
    return rt::Box::f64(std::ceil(rt::num(a)));
}

// math.round(x)
rt::Box lf_round(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    if (!rt::is_num(a)) return rt::Box::null();
    return rt::Box::f64(std::round(rt::num(a)));
}

// math.trunc(x)
rt::Box lf_trunc(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    if (!rt::is_num(a)) return rt::Box::null();
    return rt::Box::f64(std::trunc(rt::num(a)));
}

// math.fmod(a, b)
// Принимает:  a, b (i8..i64|f32|f64) — делимое и делитель.
// Возвращает: f64 — остаток от деления a на b (со знаком делимого);
//             null, если аргументов меньше двух или любой из них не число.
rt::Box lf_fmod(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.size() < 2) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    rt::Box b = rt::deref(rt::derefval(args_[1]));
    if (!rt::is_num(a) || !rt::is_num(b)) return rt::Box::null();
    return rt::Box::f64(std::fmod(rt::num(a), rt::num(b)));
}