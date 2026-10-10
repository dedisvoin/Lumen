#include <random>
#include <algorithm>
#include <chrono>
#include "C:\Users\Ivan\Desktop\Lumen\compiler\runtime\lumen_rt.h"

// один RNG на процесс; создаётся лениво при первом вызове
static std::mt19937_64& lf_rng() {
    static std::mt19937_64 eng([]() {
        std::random_device rd;
        auto t = (unsigned long long)std::chrono::high_resolution_clock::now().time_since_epoch().count();
        return (unsigned long long)rd() ^ t;
    }());
    return eng;
}

// random.seed(n)
// Принимает:  n (i8|i16|i32|i64) — значение seed для генератора.
// Возвращает: bool — true, если seed установлен; false, если args_[0]
//             отсутствует или не является целым числом.
rt::Box lf_random_seed(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::boo(false);
    rt::Box n = rt::deref(rt::derefval(args_[0]));
    if (!(n.t >= rt::Box::T_i8 && n.t <= rt::Box::T_i64)) return rt::Box::boo(false);
    lf_rng().seed((unsigned long long)n.i);
    return rt::Box::boo(true);
}

// random.float()
// Принимает:  ничего (args_ игнорируется).
// Возвращает: f64 — случайное число в диапазоне [0.0, 1.0).
rt::Box lf_random(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return rt::Box::f64(dist(lf_rng()));
}

// random.int(min, max)
// Принимает:  min (i8|i16|i32|i64), max (i8|i16|i32|i64) — границы диапазона;
//             порядок не важен, если min > max, они меняются местами.
// Возвращает: i64 — случайное целое в [min, max] включительно;
//             null, если аргументов меньше двух или один из них не целое число.
rt::Box lf_random_int(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.size() < 2) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    rt::Box b = rt::deref(rt::derefval(args_[1]));
    if (!(a.t >= rt::Box::T_i8 && a.t <= rt::Box::T_i64)) return rt::Box::null();
    if (!(b.t >= rt::Box::T_i8 && b.t <= rt::Box::T_i64)) return rt::Box::null();

    long long lo = a.i, hi = b.i;
    if (lo > hi) std::swap(lo, hi);
    std::uniform_int_distribution<long long> dist(lo, hi);
    return rt::Box::i64(dist(lf_rng()));
}

// random.range(min, max)
// Принимает:  min (i8..i64|f32|f64), max (i8..i64|f32|f64) — границы диапазона,
//             целые и дробные можно смешивать; порядок не важен.
// Возвращает: f64 — случайное дробное число в [min, max);
//             null, если аргументов меньше двух или один из них не число.
rt::Box lf_random_float(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.size() < 2) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    rt::Box b = rt::deref(rt::derefval(args_[1]));
    bool a_num = (a.t >= rt::Box::T_i8 && a.t <= rt::Box::T_i64) || a.t == rt::Box::T_f32 || a.t == rt::Box::T_f64;
    bool b_num = (b.t >= rt::Box::T_i8 && b.t <= rt::Box::T_i64) || b.t == rt::Box::T_f32 || b.t == rt::Box::T_f64;
    if (!a_num || !b_num) return rt::Box::null();

    double lo = (a.t == rt::Box::T_f32 || a.t == rt::Box::T_f64) ? a.f : (double)a.i;
    double hi = (b.t == rt::Box::T_f32 || b.t == rt::Box::T_f64) ? b.f : (double)b.i;
    if (lo > hi) std::swap(lo, hi);
    std::uniform_real_distribution<double> dist(lo, hi);
    return rt::Box::f64(dist(lf_rng()));
}

// random.bool([p])
// Принимает:  p (i8..i64|f32|f64, опционально) — вероятность true, по умолчанию 0.5;
//             если передан нечисловой аргумент, он игнорируется (p остаётся 0.5).
// Возвращает: bool — true с вероятностью p, иначе false.
rt::Box lf_random_bool(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    double p = 0.5;
    if (!args_.empty()) {
        rt::Box a = rt::deref(rt::derefval(args_[0]));
        if (a.t == rt::Box::T_f32 || a.t == rt::Box::T_f64) p = a.f;
        else if (a.t >= rt::Box::T_i8 && a.t <= rt::Box::T_i64) p = (double)a.i;
    }
    std::bernoulli_distribution dist(p);
    return rt::Box::boo(dist(lf_rng()));
}

// random.choice(arr)
// Принимает:  arr (Array[T]|Tuple[...]) — непустой массив или кортеж.
// Возвращает: T — случайный элемент arr (копия, тип совпадает с типом элемента);
//             null, если arr отсутствует, не является массивом/кортежем, или пуст.
rt::Box lf_random_choice(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    if ((a.t != rt::Box::T_ARR && a.t != rt::Box::T_TUP) || a.items.empty()) return rt::Box::null();

    std::uniform_int_distribution<size_t> dist(0, a.items.size() - 1);
    return a.items[dist(lf_rng())];
}

// random.shuffle(arr)
// Принимает:  arr (Array[T]|Tuple[...]) — массив или кортеж (не мутируется).
// Возвращает: Array[T]|Tuple[...] — новый массив/кортеж того же вида (ARR/TUP)
//             с теми же элементами в случайном порядке;
//             null, если arr отсутствует или не является массивом/кортежем.
rt::Box lf_random_shuffle(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::null();
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    if (a.t != rt::Box::T_ARR && a.t != rt::Box::T_TUP) return rt::Box::null();

    std::vector<rt::Box> copy = a.items;
    std::shuffle(copy.begin(), copy.end(), lf_rng());
    rt::Box out;
    out.t = a.t;
    out.items = std::move(copy);
    return out;
}