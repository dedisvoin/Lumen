#include <chrono>
#include <thread>
#include "C:\Users\Ivan\Desktop\Lumen\compiler\runtime\lumen_rt.h"

// time.now()
// Возвращает: f64 — секунды с начала эпохи Unix (1970-01-01 00:00:00 UTC),
//             дробная часть — доли секунды; зависит от системных часов.
rt::Box lf_raf_time(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return rt::Box::f64((double)us / 1.0e6);
}

// time.clock()
// Возвращает: i64 — монотонное время в микросекундах с неопределённой,
//             но стабильной точки отсчёта (не зависит от системной даты).
rt::Box lf_raf_clock(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return rt::Box::i64(us);
}

// time.sleep(seconds)
// Принимает:  seconds (i8..i64|f32|f64) — длительность паузы в секундах.
// Возвращает: true, если ожидание выполнено; false, если аргумент
//             отсутствует, не является числом или отрицателен.
rt::Box lf_raf_sleep(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::boo(false);
    rt::Box a = rt::deref(rt::derefval(args_[0]));
    if (!rt::is_num(a)) return rt::Box::boo(false);
    double secs = rt::num(a);
    if (secs < 0.0) return rt::Box::boo(false);
    std::this_thread::sleep_for(std::chrono::duration<long double>(secs));
    return rt::Box::boo(true);
}