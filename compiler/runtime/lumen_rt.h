#pragma once

#include <string>
#include <map>
#include <iostream>
#include <cmath>
#include <vector>
#include <memory>
#include <functional>
#include <cstdlib>
#include <cstring>

namespace rt {

struct Ctx;
struct Box;
using FnPtr = Box (*)(Ctx, const std::vector<Box>&);
using ClosFn = std::function<Box(Ctx, const std::vector<Box>&)>;

// IEEE-754 binary16 (half): значение хранится как 16-битные биты.
// double <-> half с округлением к ближайшему чётному (round-to-nearest-even).
static inline unsigned short tohalf(double value) {
    float f = static_cast<float>(value);
    unsigned int bits;
    std::memcpy(&bits, &f, sizeof(bits));
    unsigned short sign = (unsigned short)((bits >> 16) & 0x8000u);
    unsigned int e = (bits >> 23) & 0xFFu;
    unsigned int m = bits & 0x7FFFFFu;
    if (e == 0xFF)  // inf/nan -> half inf/nan (nan сохраняет quiet-бит)
        return (unsigned short)(sign | 0x7C00u | (m ? 0x200u : 0u));
    int exp = (int)e - 127 + 15;
    if (exp >= 0x1F) return (unsigned short)(sign | 0x7C00u);  // переполнение -> inf
    if (exp <= 0) {
        if (exp < -10) return sign;                             // недостаточность -> signed zero
        m |= 0x800000u;                                        // неявная ведущая 1
        unsigned int shift = (unsigned int)(14 - exp);
        unsigned int mant = m >> shift;
        unsigned int rem  = m & ((1u << shift) - 1u);
        unsigned int half = 1u << (shift - 1);
        if (rem > half || (rem == half && (mant & 1))) ++mant;
        if (mant > 0x3FFu) mant = 0x400u;
        return (unsigned short)(sign | (unsigned short)mant);
    }
    m >>= 13;
    unsigned int rem = bits & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (m & 1))) ++m;
    if (m > 0x3FFu) { m = 0; ++exp; }
    if (exp >= 0x1F) return (unsigned short)(sign | 0x7C00u);
    return (unsigned short)(sign | ((unsigned short)exp << 10) | (unsigned short)m);
}

static inline double fromhalf(unsigned short h) {
    unsigned short sign = (unsigned short)(h & 0x8000u);
    unsigned short exp  = (unsigned short)((h >> 10) & 0x1Fu);
    unsigned short mant = (unsigned short)(h & 0x3FFu);
    unsigned int bits;
    if (exp == 0x1F) {
        bits = ((unsigned int)sign << 16) | 0x7F800000u | ((unsigned int)mant << 13);
    } else if (exp == 0) {
        if (mant == 0) {
            bits = (unsigned int)sign << 16;                   // signed zero
        } else {
            int e = -15;                                       // subnormal -> normalize
            unsigned int b = mant;
            while (!(b & 0x400u)) { b <<= 1; --e; }
            e += 127;
            bits = ((unsigned int)sign << 16) | ((unsigned int)(e & 0xFF) << 23) | ((b & 0x3FFu) << 13);
        }
    } else {
        bits = ((unsigned int)sign << 16) | ((unsigned int)(exp - 15 + 127) << 23) | ((unsigned int)mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return (double)f;
}

struct Box {
    enum Tag { T_NULL, T_i8, T_i16, T_i32, T_i64, T_f16, T_f32, T_f64, T_BOOL, T_CHAR, T_STR, T_ARR, T_TUP, T_FN, T_CLOSURE, T_TYPE, T_STRUCT, T_PTR, T_REF, T_REFI } t = T_NULL;
    long long    i = 0;
    double       f = 0;
    bool         b = false;
    char         c = 0;
    std::string  s;
    std::vector<Box> items;
    FnPtr        fp = nullptr;
    std::shared_ptr<ClosFn> cl;   // замыкание (вложенная функция): захваченный контекст внутри лямбды
    // структура: имя типа + поля (shared_map: копии Box разделяют состояние -> write-back)
    std::string  tname;
    std::shared_ptr<std::map<std::string, Box>> sm;
    // объявленные типы generic-полей структуры (для typeis): имя поля -> канонич. тип.
    // Заполняется только у инстансов, чьи поля зависят от generic-параметров структуры.
    std::shared_ptr<std::map<std::string, std::string>> fdecl;
    // указатель (new): одно значение-носитель
    std::shared_ptr<Box> heap;
    // ссылка-алиас (ref let / ref-параметр / ref-возврат): носитель = общая ячейка
    std::shared_ptr<Box> sp;
    // ссылка-индекс (ref let x = a[i]): ячейка-носитель контейнера + индекс.
    // Резолвится на каждый доступ через текущий контейнер (безопасно при переназначении).
    long long refi = 0;
    std::shared_ptr<Box> host;
    // виртуальный адрес в арене (new/ptr): последовательные адреса, как VirtualHeap
    // интерпретатора. -1 = указатель без адреса (&var), арифметика к нему не применима.
    long long addr = -1;
    // namespace-литерал: захваченная область видимости (параметры/локалы/глобалы).
    // Резерв для чтения ns::member после выхода из функции, как parent-цепочка
    // интерпретатора. У обычных структур == nullptr (поле неактивно).
    std::shared_ptr<Ctx> scope;

    static Box null()           { Box v; v.t = T_NULL; return v; }
    static Box i8 (long long x) { Box v; v.t = T_i8;  v.i = x; return v; }
    static Box i16(long long x) { Box v; v.t = T_i16; v.i = x; return v; }
    static Box i32(long long x) { Box v; v.t = T_i32; v.i = x; return v; }
    static Box i64(long long x) { Box v; v.t = T_i64; v.i = x; return v; }
    static Box f32(double x)    { Box v; v.t = T_f32; v.f = x; return v; }
    static Box f64(double x)    { Box v; v.t = T_f64; v.f = x; return v; }
    static Box f16(double x)    { Box v; v.t = T_f16; v.f = (double)fromhalf(tohalf(x)); return v; }
    static Box boo(bool x)      { Box v; v.t = T_BOOL; v.b = x; return v; }
    static Box chr(char x)      { Box v; v.t = T_CHAR; v.c = x; return v; }
    static Box str(std::string x){ Box v; v.t = T_STR; v.s = std::move(x); return v; }
    static Box arr(std::vector<Box> x){ Box v; v.t = T_ARR; v.items = std::move(x); return v; }
    static Box tup(std::vector<Box> x){ Box v; v.t = T_TUP; v.items = std::move(x); return v; }
    static Box fn(FnPtr x)      { Box v; v.t = T_FN;  v.fp = x; return v; }
    static Box clos(ClosFn f)   { Box v; v.t = T_CLOSURE; v.cl = std::make_shared<ClosFn>(std::move(f)); return v; }
    static Box typ(const std::string& tn) { Box v; v.t = T_TYPE; v.s = tn; return v; }
    static Box strct(const std::string& name, std::map<std::string, Box> f,
                     std::map<std::string, std::string> fd = {}) {
        Box v; v.t = T_STRUCT; v.tname = name;
        v.sm = std::make_shared<std::map<std::string, Box>>(std::move(f));
        if (!fd.empty()) v.fdecl = std::make_shared<std::map<std::string, std::string>>(std::move(fd));
        return v;
    }
    static Box ptr(Box x) { Box v; v.t = T_PTR; v.addr = (x.t >= T_i8 && x.t <= T_i64) ? x.i : -1; v.heap = std::make_shared<Box>(std::move(x)); return v; }
    static Box rref(std::shared_ptr<Box> p) { Box v; v.t = T_REF; v.sp = std::move(p); return v; }
    static Box refcell(Box x) { return Box::rref(std::make_shared<Box>(std::move(x))); }
};

// невладеющий shared_ptr на существующую Box-ячейку (стабильна: узлы std::map не переезжают)
static inline std::shared_ptr<Box> refcell_ptr(Box& b) { return std::shared_ptr<Box>(&b, [](Box*){}); }

// --- арена виртуальной памяти (зеркало VirtualHeap интерпретатора) -------------
// new-ячейки получают последовательные адреса 0,1,2,…; ptr ± n сдвигает адрес,
// ptr - ptr даёт разницу, разыменование идёт по адресу. Это делает осмысленной
// адресную арифметику (`*(p+1)` = следующая new-ячейка).
static inline std::vector<std::shared_ptr<Box>>& heap_arena() {
    static std::vector<std::shared_ptr<Box>> a;
    return a;
}
static inline long long heap_alloc(std::shared_ptr<Box> cell) {
    auto& a = heap_arena();
    if (a.empty()) a.push_back(nullptr); // резерв: индекс 0 не используется (как VirtualHeap, адреса 1,2,3,…)
    a.push_back(std::move(cell));
    return (long long)a.size() - 1;
}
// указатель по виртуальному адресу; вне диапазона или 0 -> null (как heap.contains(0) == false)
static inline Box ptr_at(long long address) {
    auto& a = heap_arena();
    if (address <= 0 || address >= (long long)a.size()) return Box::null();
    Box v; v.t = Box::T_PTR; v.addr = address; v.heap = a[(size_t)address];
    return v;
}
// new: свежая ячейка в арене -> указатель с последовательным адресом
static inline Box make_new(Box x) {
    auto cell = std::make_shared<Box>(std::move(x));
    long long address = heap_alloc(cell);
    Box v; v.t = Box::T_PTR; v.addr = address; v.heap = cell;
    return v;
}

// ячейка элемента для T_REFI (резолв через текущий контейнер-носитель).
// refi == -1: ячейка поля - host + путь в s (резолвится заново при каждом доступе).
static inline Box* fieldpath(Box& root, const std::vector<std::string>& path);
static inline Box* refi_cell(const Box& v) {
    if (v.t != Box::T_REFI || !v.host) return nullptr;
    Box* h = v.host.get();
    if (!h) return nullptr;
    if (v.refi < 0) {
        std::vector<std::string> path;
        std::string cur;
        for (char ch : v.s) {
            if (ch == '\x1f') { path.push_back(cur); cur.clear(); } else cur += ch;
        }
        if (!cur.empty()) path.push_back(cur);
        if (path.empty()) return nullptr;
        return fieldpath(*h, path);
    }
    if (h->t != Box::T_ARR && h->t != Box::T_TUP) return nullptr;
    if (v.refi >= (long long)h->items.size()) return nullptr;
    return &h->items[(size_t)v.refi];
}
// Полное разворачивание ссылок к ячейке-цели (цепочка ref->ref->…->цель, без копий).
// Нужно для ref-возвратов и рефов на переменные-рефы: `v = Vec::New()` держит реф
// на ячейку рефа на объект, методы получают double-ref и т.п.
static inline Box* derefcell(Box& v) {
    Box* c = &v;
    for (int g = 0; g < 64; ++g) {
        if (c->t == Box::T_REF && c->sp) { c = c->sp.get(); continue; }
        if (c->t == Box::T_REFI) { Box* n = refi_cell(*c); if (!n) return nullptr; c = n; continue; }
        break;
    }
    return c;
}
// ячейка-носитель любого ref-значения: T_REF -> цель, T_REFI -> элемент (цепочкой)
static inline Box* refaddr(Box& v) { return derefcell(v); }
// чтение ссылки: даёт значение цели (цепочкой)
static inline Box derefval(const Box& v) {
    Box* c = derefcell(const_cast<Box&>(v));
    return c ? *c : Box::null();
}
// запись через ссылку: пишет в цель (иначе — в саму ячейку)
static inline Box& setref(Box& slot, Box val) {
    if (slot.t == Box::T_REF && slot.sp) { *(slot.sp) = std::move(val); return *(slot.sp); }
    if (slot.t == Box::T_REFI) {
        Box* c = refi_cell(slot);
        if (c) { *c = std::move(val); return *c; }
    }
    slot = std::move(val);
    return slot;
}
// slot_get() = v: запись в живой алиас-результат вызова (T_REF или T_REFI)
static inline Box setref_call(Box alias, Box val) {
    if (alias.t == Box::T_REF && alias.sp) { *(alias.sp) = std::move(val); return *(alias.sp); }
    if (alias.t == Box::T_REFI) {
        Box* c = refi_cell(alias);
        if (c) { *c = std::move(val); return *c; }
    }
    return val;
}
// ref-of-ref: алиас-копия (тот же host+index для T_REFI, та же ячейка для T_REF)
static inline Box refalias(const Box& v) {
    if (v.t == Box::T_REFI) { Box r; r.t = Box::T_REFI; r.host = v.host; r.refi = v.refi; r.s = v.s; return r; }
    if (v.t == Box::T_REF) return Box::rref(v.sp);
    return v;
}

// &s.age: указатель на ПОЛЕ структуры. Носим владельца (heap) и имя поля (s),
// а не готовую ячейку std::map: ячейка резолвится при каждом обращении, поэтому
// переприсваивание владельца (`s = p{...}`) не оставляет указатель в отброшенной
// структуре - та же схема, что у T_REFI для a[i] (host + индекс, не готовая ячейка).
static inline bool is_fieldptr(const Box& v) { return v.t == Box::T_PTR && v.heap && !v.s.empty(); }
static inline Box fieldptr(Box& owner, const std::string& name) {
    static long long seq = 0;   // каждый & - свой адрес, как слоты VirtualHeap в интерпретаторе
    Box v; v.t = Box::T_PTR; v.heap = std::shared_ptr<Box>(&owner, [](Box*){});
    v.s = name; v.refi = ++seq; return v;
}
static inline Box* ptr_cell(const Box& v);  // определена ниже, после fieldpath()
static inline Box deref(const Box& v) { Box* c = ptr_cell(v); return c ? *c : v; }
// &переменная: указатель на существующую ячейку env (невладеющий shared_ptr — ячейка стабильна)
static inline Box refptr(Box& b) { Box v; v.t = Box::T_PTR; v.heap = std::shared_ptr<Box>(&b, [](Box*){}); return v; }
// *p = v: запись через указатель в ячейку-носитель
static inline Box& setptr(Box& ptr, Box val) {
    Box* c = (ptr.t == Box::T_PTR) ? ptr_cell(ptr) : nullptr;
    if (c) { *c = std::move(val); return *c; }
    ptr = std::move(val);
    return ptr;
}
static inline std::map<std::string, Box>* smap(const Box& v) {
    Box u = deref(v);
    for (int g = 0; g < 64 && (u.t == Box::T_REF || u.t == Box::T_REFI); ++g)
        u = derefval(u);
    return u.t == Box::T_STRUCT ? u.sm.get() : nullptr;
}
// поиск в захваченной области namespace-литерала (определена ниже, после struct Ctx)
static inline const Box* scope_lookup(const Box& v, const std::string& name);

// поле структуры (авто-разыменование T_PTR)
static inline Box field(const Box& v, const std::string& name) {
    std::map<std::string, Box>* m = smap(v);
    if (!m) return Box::null();
    auto it = m->find(name);
    if (it != m->end()) return it->second;
    // namespace-литерал: промах по членам -> ищем в захваченной области
    if (const Box* r = scope_lookup(v, name)) return *r;
    return Box::null();
}
// запись поля (авто-разыменование T_PTR, shared_map -> виден всем копиям)
static inline void putf(const Box& v, const std::string& name, Box val) {
    std::map<std::string, Box>* m = smap(v);
    if (m) (*m)[name] = std::move(val);
}

// Путь по полям структуры obj.a.b: возвращает указатель на ячейку самого вложенного поля.
// Узлы std::map стабильны (не перемещаются при вставке/удалении соседних ключей), поэтому
// Box* остаётся валидным, пока жив контейнер (нe-владеющие shared_ptr в refpath/refcell_ptr).
static inline Box* fieldpath(Box& root, const std::vector<std::string>& path) {
    Box* cur = &root;
    for (auto& key : path) {
        std::map<std::string, Box>* m = smap(*cur);
        if (!m) return nullptr;
        auto it = m->find(key);
        if (it == m->end()) return nullptr;
        cur = &it->second;
    }
    return cur;
}
// ref let x = obj.a.b — живой алиас на ячейку поля (нe-владеющий shared_ptr, как refcell_ptr).
static inline Box refpath(Box& root, const std::vector<std::string>& path) {
    if (path.empty()) return Box::null();
    // T_REFI с refi = -1: путь полей в s, разделённых '\x1f'. Ячейка резолвится
    // заново при каждом обращении, поэтому переприсваивание структуры не оставляет
    // алиас в отброшенной копии (та же идея, что у указателя на поле).
    std::string joined;
    for (auto& p : path) { if (!joined.empty()) joined += '\x1f'; joined += p; }
    Box v; v.t = Box::T_REFI;
    v.host = std::shared_ptr<Box>(&root, [](Box*){});
    v.s = joined; v.refi = -1;
    return v;
}
// живая ячейка указателя: для &obj.field имя поля резолвится заново при каждом
// обращении, поэтому переприсваивание владельца не «оставляет» указатель в старой
// структуре. nullptr - разрешить нечем (указатель без носителя или поле исчезло).
static inline Box* ptr_cell(const Box& v) {
    if (v.t == Box::T_PTR) {
        if (is_fieldptr(v)) return fieldpath(*(v.heap), {v.s});
        return v.heap ? v.heap.get() : nullptr;
    }
    return const_cast<Box*>(&v);
}

static inline bool is_int_t(const Box& v) { return v.t >= Box::T_i8 && v.t <= Box::T_i64; }
static inline bool is_f(const Box& v)     { return v.t == Box::T_f16 || v.t == Box::T_f32 || v.t == Box::T_f64; }
static inline bool is_num(const Box& v)   { return is_int_t(v) || is_f(v); }
static inline double  num(const Box& v)   { return is_f(v) ? v.f : (double)v.i; }
static inline long long inum(const Box& v){ return is_f(v) ? (long long)v.f : v.i; }

static inline Box make_i(Box::Tag t, long long x) {
    if (t == Box::T_i8)  return Box::i8 (x);
    if (t == Box::T_i16) return Box::i16((long long)(short)x);
    if (t == Box::T_i32) return Box::i32((long long)(int)x);
    return Box::i64(x);
}
static inline Box make_f(Box::Tag t, double x) {
    if (t == Box::T_f16) return Box::f16(x);
    if (t == Box::T_f32) return Box::f32((float)x);
    return Box::f64(x);
}

// --- append <контейнер>, <значение>; ----------------------------------------
// Прямая ячейка-носитель цели: T_PTR -> носитель, T_REF -> цель, T_REFI -> элемент.
static inline Box& append_target(Box& target) {
    Box* c = derefcell(target);
    if (!c) c = &target;
    if (c->t == Box::T_PTR) { if (Box* p = ptr_cell(*c)) c = p; }
    return *c;
}
// append: in-place push элемента (зеркало DeepAnalizer). Не-контейнер молча
// пропускается (интерпретатор даёт статическую APPEND_TARGET_NOT_ARRAY).
static inline Box& append_dispatch(Ctx& ctx, Box& target, Box val) {
    Box& t = append_target(target);
    if (t.t == Box::T_ARR || t.t == Box::T_TUP)
        t.items.push_back(std::move(val));
    return t;
}
// reserve <контейнер>, <ёмкость>; --------------------------------------------
// Предвыделение места под count элементов ВСЕГО (не «ещё count»). Длина не
// меняется, усадки нет: count меньше текущего размера — no-op. Не-контейнер и
// нецелое значение молча пропускаются (интерпретатор даёт статические
// RESERVE_TARGET_NOT_ARRAY / RESERVE_COUNT_NOT_INTEGER).
static inline Box& reserve_dispatch(Ctx& ctx, Box& target, Box count) {
    Box& t = append_target(target);
    if ((t.t == Box::T_ARR || t.t == Box::T_TUP) && is_int_t(count)) {
        long long n = inum(count);
        if (n > 0) t.items.reserve((size_t)n);
    }
    return t;
}
// ячейка элемента контейнера по индексу (Box* прямо в std::vector) — для целей
// вида `append a[i], x;` в транспиляции.
static inline Box* arr_cell(Box& v, Box idx) {
    Box& t = append_target(v);
    if (is_int_t(idx) && (t.t == Box::T_ARR || t.t == Box::T_TUP)) {
        long long i = inum(idx);
        if (i >= 0 && i < (long long)t.items.size()) return &t.items[(size_t)i];
    }
    return nullptr;
}
// адрес ячейки-носителя указателя/ссылки (для `append *p, x;`).
static inline Box* box_addr(Box v) {
    Box* c = derefcell(v);
    if (!c) return nullptr;
    if (c->t == Box::T_PTR) return ptr_cell(*c);
    return c;
}

static inline std::string fmt(const Box& v) {
    switch (v.t) {
        case Box::T_STR:  return v.s;
        case Box::T_BOOL: return v.b ? "true" : "false";
        case Box::T_CHAR: return std::string(1, v.c);
        case Box::T_NULL: return "null";
        case Box::T_ARR:
        case Box::T_TUP: {
            std::string s;
            s += (v.t == Box::T_ARR ? '{' : '[');
            for (size_t k = 0; k < v.items.size(); ++k) {
                if (k) s += ", ";
                s += fmt(v.items[k]);
            }
            s += (v.t == Box::T_ARR ? '}' : ']');
            return s;
        }
        case Box::T_STRUCT: {
            if (!v.tname.empty()) return "object(" + v.tname + ")";
            return "{}";
        }
        case Box::T_PTR:
            if (v.addr >= 0) return "0x" + std::to_string(v.addr);
            return v.heap ? "ptr" : "null-ptr";
        case Box::T_REF: return v.sp ? fmt(*v.sp) : "ref-null";
        case Box::T_REFI: { Box* c = refi_cell(v); return c ? fmt(*c) : "ref-null"; }
        case Box::T_TYPE: return v.s;
        case Box::T_CLOSURE: return "fn";
        default: {
            double d = num(v);
            if (std::floor(d) == d && std::fabs(d) < 1e15)
                return std::to_string((long long)d);
            return std::to_string(d);
        }
    }
}
static inline void print(const Box& v)     { std::cout << fmt(v); }
static inline void println(const Box& v)   { std::cout << fmt(v) << "\n"; }

static inline Box input(const Box& prompt) {
    if (prompt.t == Box::T_STR) std::cout << prompt.s;
    else if (prompt.t == Box::T_CHAR) std::cout << prompt.c;
    std::string line;
    if (!std::getline(std::cin, line)) line.clear();
    return Box::str(std::move(line));
}


static inline int rank_i(Box::Tag t) {
    switch (t) {
        case Box::T_i8: return 1;
        case Box::T_i16: return 2;
        case Box::T_i32: return 3;
        case Box::T_i64: return 4;
        default: return 0;
    }
}
static inline int rank_f(Box::Tag t) {
    switch (t) {
        case Box::T_f16: return 1;
        case Box::T_f32: return 2;
        case Box::T_f64: return 3;
        default: return 0;
    }
}
static inline Box::Tag result_t(Box l, Box r) {
    bool af = is_f(l), bf = is_f(r);
    if (af || bf) {
        if (af && bf) return rank_f(l.t) >= rank_f(r.t) ? l.t : r.t;
        return af ? l.t : r.t;
    }
    return rank_i(l.t) >= rank_i(r.t) ? l.t : r.t;
}

// Зеркало DeepAnalizer::binaryMath для подмножества M0.
static inline Box bin(char op, Box l, Box r) {
    if (op == '*' && (l.t == Box::T_STR || l.t == Box::T_CHAR)) {
        if (!is_int_t(r)) return Box::null();
        long long n = inum(r);
        if (n < 0) return Box::null();
        std::string s = (l.t == Box::T_STR) ? l.s : std::string(1, l.c);
        std::string out;
        out.reserve(s.size() * (size_t)n);
        for (long long k = 0; k < n; ++k) out += s;
        return Box::str(out);
    }
    // Неявная арифметика с char запрещена (зеркало DeepAnalizer::binaryMath):
    // требуется явный перевод '((c as i64) + 1) as char'. Диагностику выдаёт
    // deep-проход при компиляции; здесь — заглушка, чтобы `-no-deep-analyze`
    // не вернул молчаливый null. Строковые операции с char не затронуты.
    if ((op == '+' || op == '-') && (l.t == Box::T_CHAR || r.t == Box::T_CHAR)) {
        bool lc = (l.t == Box::T_CHAR), rc = (r.t == Box::T_CHAR);
        if ((op == '-' && lc && rc) || (lc && is_int_t(r)) || (rc && is_int_t(l) && op == '+'))
            return Box::null();
    }
    if (op == '+' && (l.t == Box::T_STR || l.t == Box::T_CHAR || r.t == Box::T_STR || r.t == Box::T_CHAR)) {
        auto toS = [](const Box& v) -> std::string {
            if (v.t == Box::T_STR)  return v.s;
            if (v.t == Box::T_CHAR) return std::string(1, v.c);
            return std::string();
        };
        if (l.t == Box::T_STR || l.t == Box::T_CHAR)
            return Box::str(toS(l) + toS(r));
        return Box::null();
    }
    // array + array / tuple + tuple → array (зеркало DeepAnalizer)
    if (op == '+' && (l.t == Box::T_ARR || l.t == Box::T_TUP) && (r.t == Box::T_ARR || r.t == Box::T_TUP)) {
        std::vector<Box> res;
        res.reserve(l.items.size() + r.items.size());
        for (auto& x : l.items) res.push_back(x);
        for (auto& x : r.items) res.push_back(x);
        return Box::arr(std::move(res));
    }
    // адресная арифметика указателей (зеркало DeepAnalizer): ptr ± n, ptr - ptr.
    // Адреса арены последовательны, поэтому `p+1` — следующая new-ячейка.
    if ((op == '+' || op == '-') && (l.t == Box::T_PTR || r.t == Box::T_PTR)) {
        bool lp = (l.t == Box::T_PTR), rp = (r.t == Box::T_PTR);
        if (lp && rp) {
            if (op == '-') return Box::i64(l.addr - r.addr);
            return Box::null();
        }
        Box p = lp ? l : r;
        Box n = lp ? r : l;
        if (!is_int_t(n) || p.addr < 0) return Box::null();
        long long step = inum(n);
        if (op == '-' && lp) step = -step;
        return ptr_at(p.addr + step);
    }
    if (!is_num(l) || !is_num(r)) return Box::null();
    Box::Tag rt_ = result_t(l, r);
    bool isf = (rt_ == Box::T_f16 || rt_ == Box::T_f32 || rt_ == Box::T_f64);
    if (isf) {
        double a = num(l), b = num(r), res = 0;
        switch (op) {
            case '+': res = a + b; break;
            case '-': res = a - b; break;
            case '*': res = a * b; break;
            case '/': res = a / b; break;
            case '%': res = std::fmod(a, b); break;
        }
        return make_f(rt_, res);
    }
    long long a = inum(l), b = inum(r), res = 0;
    switch (op) {
        case '+': res = a + b; break;
        case '-': res = a - b; break;
        case '*': res = a * b; break;
        case '/': res = a / b; break;
        case '%': res = a % b; break;
        case '|': res = a | b; break;
        case '&': res = a & b; break;
        case '^': res = a ^ b; break;
        case 'l': res = a << b; break;
        case 'r': res = a >> b; break;
    }
    return make_i(rt_, res);
}

// Зеркало DeepAnalizer::negationNumber.
static inline Box neg(const Box& v) {
    switch (v.t) {
        case Box::T_i8:  return Box::i8 (-v.i);
        case Box::T_i16: return Box::i16(-v.i);
        case Box::T_i32: return Box::i32(-v.i);
        case Box::T_i64: return Box::i64(-v.i);
        case Box::T_f16: return Box::f16(-(double)v.f);
        case Box::T_f32: return Box::f32(-(float)v.f);
        case Box::T_f64: return Box::f64(-v.f);
        default: return Box::null();
    }
}

// Правдивость условия — зеркало evalCondition (bool или число != 0).
static inline bool truthy(const Box& v) {
    if (v.t == Box::T_BOOL) return v.b;
    if (is_num(v)) return num(v) != 0;
    return false;
}

// Сравнение type-имён: юнионы ("i32 | f32") сравниваются как множества —
// зеркало структурного сравнения Type в интерпретаторе (порядок не важен).
static inline bool type_name_eq(const std::string& a, const std::string& b) {
    if (a == b) return true;
    if (a.find(" | ") == std::string::npos && b.find(" | ") == std::string::npos)
        return false;
    auto split = [](const std::string& s) {
        std::vector<std::string> r;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t sep = s.find(" | ", pos);
            if (sep == std::string::npos) { r.push_back(s.substr(pos)); break; }
            r.push_back(s.substr(pos, sep - pos));
            pos = sep + 3;
        }
        return r;
    };
    auto va = split(a), vb = split(b);
    if (va.size() != vb.size()) return false;
    for (auto& x : va) {
        bool found = false;
        for (auto& y : vb) if (x == y) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

// Сравнения — зеркало бинарных ==/!=/</>/<=/>=. op: 'e' ==, 'n' !=, '<', '>', 'L' <=, 'G' >=.
static inline Box cmp(char op, Box l, Box r) {
    if (op == 'e' || op == 'n') {
        bool eq = false;
        if (l.t == r.t) {
            switch (l.t) {
                case Box::T_NULL: eq = true; break;
                case Box::T_BOOL: eq = (l.b == r.b); break;
                case Box::T_CHAR: eq = (l.c == r.c); break;
                case Box::T_STR:  eq = (l.s == r.s); break;
                case Box::T_TYPE: eq = type_name_eq(l.s, r.s); break;
case Box::T_PTR:
                      // у &obj.field своя личность (refi), как у слота VirtualHeap:
                      // два независимых &s.age не равны, но копия одного указателя равна.
                      if (is_fieldptr(l) || is_fieldptr(r))
                          eq = is_fieldptr(l) && is_fieldptr(r) && l.refi == r.refi;
                      else
                          eq = (l.addr >= 0 || r.addr >= 0)
                             ? (l.addr == r.addr) : (l.heap.get() == r.heap.get());
                      break;
                default: eq = is_num(l) && (num(l) == num(r));
            }
        }
        return Box::boo(op == 'e' ? eq : !eq);
    }
    // порядковые (числа; строки — по длине, как в интерпретаторе)
    bool res = false;
    if (is_num(l) && is_num(r)) {
        double a = num(l), b = num(r);
        if (op == '<' || op == 'L') res = a < b;
        else if (op == '>' || op == 'G') res = a > b;
        if ((op == 'L' || op == 'G') && a == b) res = true;
    } else if (l.t == Box::T_STR && r.t == Box::T_STR) {
        size_t a = l.s.size(), b = r.s.size();
        if (op == '<' || op == 'L') res = a < b;
        else if (op == '>' || op == 'G') res = a > b;
        if ((op == 'L' || op == 'G') && a == b) res = true;
    }
    return Box::boo(res);
}

// Логические && || — только bool (как в интерпретаторе; оба операнда вычислены заранее).
static inline Box land(Box l, Box r) {
    return Box::boo((l.t == Box::T_BOOL ? l.b : false) && (r.t == Box::T_BOOL ? r.b : false));
}
static inline Box lor(Box l, Box r) {
    return Box::boo((l.t == Box::T_BOOL ? l.b : false) || (r.t == Box::T_BOOL ? r.b : false));
}
// ?? — null-coalescing: l ?? r возвращает r, только если l == null. Как и у && / ||,
// правый операнд вычисляется заранее; ссылки разыменовываются перед проверкой.
static inline Box coalesce(Box l, Box r) {
    l = derefval(l);
    if (l.t == Box::T_NULL) return derefval(r);
    return l;
}
static inline Box not_(Box v) {
    if (v.t == Box::T_BOOL) return Box::boo(!v.b);
    if (is_num(v)) return Box::boo(num(v) == 0);
    if (v.t == Box::T_ARR)  return Box::tup(v.items);   // !{ ... } → tuple
    return Box::null();
}

// Индексация — str→char, array/tuple→элемент. Эмиттер сворачивает несколько
// индексов последовательными вызовами rt::index.
static inline Box index(Box v, Box idx) {
    if (!is_int_t(idx)) return Box::null();
    long long i = inum(idx);
    if (v.t == Box::T_STR) {
        if (i < 0 || i >= (long long)v.s.size()) return Box::null();
        return Box::chr(v.s[(size_t)i]);
    }
    if (v.t == Box::T_ARR || v.t == Box::T_TUP) {
        if (i < 0 || i >= (long long)v.items.size()) return Box::null();
        return v.items[(size_t)i];
    }
    return Box::null();
}

// del expr, idx — удаление элемента, возвращает новый array/tuple
// (ляет по ссылке через emitStmt отдельно).
static inline Box del(Box host, Box idx) {
    if (!(is_int_t(idx))) return host;
    long long i = inum(idx);
    if (host.t != Box::T_ARR && host.t != Box::T_TUP) return host;
    if (i < 0 || i >= (long long)host.items.size()) return host;
    std::vector<Box> res;
    res.reserve(host.items.size() - 1);
    for (long long k = 0; k < (long long)host.items.size(); ++k)
        if (k != i) res.push_back(host.items[(size_t)k]);
    if (host.t == Box::T_ARR) return Box::arr(std::move(res));
    return Box::tup(std::move(res));
}

// sizeof — длина строки/массива/кортежа, 1 для char.
static inline Box szof(Box v) {
    if (v.t == Box::T_STR)              return Box::i64((long long)v.s.size());
    if (v.t == Box::T_CHAR)             return Box::i64(1);
    if (v.t == Box::T_ARR || v.t == Box::T_TUP)
                                         return Box::i64((long long)v.items.size());
    return Box::null();
}

// cast(expr, "i64") — простые приведения как в interpreter (NodeCast → as)
static inline Box cast(Box v, const std::string& tn) {
    if (v.t == Box::T_CHAR) {
        if (tn == "i8" || tn == "i16" || tn == "i32" || tn == "i64")
            return Box::i64((long long)(unsigned char)v.c);
        if (tn == "str") return Box::str(std::string(1, v.c));
        if (tn == "char") return v;
        return Box::null();
    }
    if (tn == "i64")  return Box::i64(inum(v));
    if (tn == "i32")  return Box::i32((int)inum(v));
    if (tn == "i16")  return Box::i16((short)inum(v));
    if (tn == "i8")   return Box::i8((char)inum(v));
    if (tn == "f64")  return Box::f64(num(v));
    if (tn == "f32")  return Box::f32((float)num(v));
    if (tn == "f16")  return Box::f16(num(v));
    if (tn == "char") {
        if (v.t == Box::T_STR && v.s.size() == 1) return Box::chr(v.s[0]);
        if (is_int_t(v) || is_f(v)) return Box::chr((char)inum(v));
        return Box::null();
    }
    if (tn == "str")  return Box::str(fmt(v));
    if (tn == "bool") return Box::boo(truthy(v));
    return Box::null();
}

struct Ctx {
    std::map<std::string, Box> m;
    // Родительская цепочка. Заимствованный указатель (на rt::g_ctx или временный
    // контекст фрейма) хранится как shared_ptr с no-op делитером; владеющая связь
    // используется только в снапшотах namespace-литералов (scope), удерживая всю
    // цепочку копий фреймов живой после выхода из функции.
    std::shared_ptr<Ctx> parent;
    Ctx() {}
    explicit Ctx(Ctx* p) : parent(std::shared_ptr<Ctx>(p, [](Ctx*){})) {}
    explicit Ctx(std::shared_ptr<Ctx> p) : parent(std::move(p)) {}
    void define(const std::string& n, Box v) { m[n] = std::move(v); }
    Box& get(const std::string& n) {
        auto it = m.find(n);
        if (it != m.end()) return it->second;
        return parent ? parent->get(n) : m[n];
    }
    // поиск БЕЗ вставки: get() при промахе создаёт T_NULL-запись, а резолв типа
    // тега дундера должен быть read-only (иначе каждый промах раздувает контекст).
    const Box* find(const std::string& n) const {
        for (const Ctx* c = this; c; c = c->parent.get()) {
            auto it = c->m.find(n);
            if (it != c->m.end()) return &it->second;
        }
        return nullptr;
    }
};

// --- namespace-литералы: захват видимой области ---------------------------------
// Интерпретатор даёт namespace-значению live-цепочку на enclosing scope через
// ns.context.parent. В транспилированном коде все фреймы функций создаются как
// rt::Ctx(&rt::g_ctx) (см. CodeGenerator), поэтому цепочка всегда заканчивается
// стабильным глобальным контекстом: его можно ЗАИМСТВОВАТЬ (живёт весь запуск),
// а фреймы функций (умрут после return) — скопировать на хип. Параметры функции
// живут как C++-локали и в ctx.m не входят — вызывающий передаёт их через captured.
static inline std::shared_ptr<Ctx> scope_snapshot(Ctx& ctx, std::map<std::string, Box>&& captured) {
    std::vector<Ctx*> chain;
    for (Ctx* c = &ctx; c; c = c->parent.get()) chain.push_back(c);
    std::shared_ptr<Ctx> cur = std::make_shared<Ctx>();
    cur->parent = std::shared_ptr<Ctx>(chain.back(), [](Ctx*){}); // заимствуем корень (g_ctx)
    for (size_t i = chain.size() - 1; i >= 1; --i) { // копируем фреймы, кроме корня (chain[0]..chain[n-2])
        auto fr = std::make_shared<Ctx>();
        fr->m = chain[i - 1]->m;  // копия локалов фрейма (путь: от родителя к текущему)
        fr->parent = cur;         // владеет предыдущим звеном — вся цепочка живёт
        cur = fr;
    }
    if (!captured.empty() && chain.size() > 1)
        for (auto& kv : captured) cur->m[kv.first] = std::move(kv.second); // параметры поверх локалов
    return cur;
}

static inline const Box* scope_lookup(const Box& v, const std::string& name) {
    Box u = deref(v);
    return u.scope ? u.scope->find(name) : nullptr;
}

// namespace-литерал: члены тела + захват видимой области.
static inline Box nslit(Ctx& ctx, const std::string& tname,
                        std::map<std::string, Box> fields,
                        std::map<std::string, Box> captured = {}) {
    Box b = Box::strct(tname, std::move(fields));
    b.scope = scope_snapshot(ctx, std::move(captured));
    return b;
}

// живой ref из переменной при выходе из фрейма: ЛОКАЛЬ функции (ячейка умрёт вместе с её
// фреймом) становится владеющей копией (Box::refcell); переменная родителя или РУТ-контекста
// (g_ctx, parent == nullptr) живёт дольше нас — остаётся невладеющим алиасом, иначе
// `slot_get() = 99` на глобал писал бы в копию, а не в слот.
static inline Box env_ref(Ctx& env, const std::string& name) {
    if (env.parent != nullptr && env.m.count(name)) return Box::refcell(env.get(name));
    return Box::rref(refcell_ptr(env.get(name)));
}

// return <не-алиас> (вызов/выражение) из `-> ref`-функции: результат обязан пережить фрейм,
// поэтому захватываем его во владеющую ячейку. Алиас-источники (переменная/поле/элемент)
// этим путём не идут — они сохраняют живую связь (refpromote не вызывается).
static inline Box retpromote(Box v) { return Box::refcell(derefval(v)); }

// вызов функции-значения (rt::Box::fn(...)): T_FN -> fp(ctx, args)
static inline bool dunder_try(Ctx& ctx, const std::string& ty, const std::string& d,
                              std::vector<Box> args, Box& out); // forward (определение ниже)
static inline Box call(Box f, Ctx& ctx, std::vector<Box> args) {
    while (f.t == Box::T_REF) { if (!f.sp) return Box::null(); f = *f.sp; }
    if (f.t == Box::T_FN && f.fp) return f.fp(ctx, std::move(args));
    if (f.t == Box::T_CLOSURE && f.cl) return (*f.cl)(ctx, std::move(args));
    // __call__: объект структуры вызывается как функция — self = объект
    if (f.t == Box::T_STRUCT && !f.tname.empty()) {
        args.insert(args.begin(), f);
        Box out;
        if (dunder_try(ctx, f.tname, "__call__", std::move(args), out)) return out;
        args.erase(args.begin());
    }
    return Box::null();
}

// операторные дюндеры: тип-структуры -> (dunder-имя -> [перегрузки {теги параметров, fn}])
// Теги — типы параметров ПОСЛЕ self; рантайм выбирает перегрузку по фактическим типам
// аргументов, поэтому перегрузки `__assign__(self, i64)` и `__assign__(self, A)`
// сосуществуют и вызываются каждая для своего типа значения.
using DunderEntry = std::pair<std::vector<std::string>, FnPtr>;
static std::map<std::string, std::map<std::string, std::vector<DunderEntry>>> g_dunders;
static inline void reg_dunder(const std::string& ty, const std::string& name,
                              const std::vector<std::string>& tags, FnPtr fp) {
    g_dunders[ty][name].push_back({tags, fp});
}
static inline std::string tyname(const Box& v);
static inline bool typcontains(const std::string& container, const std::string& member);

// Совместимость тега дундера с фактическим типом аргумента — по тем же правилам,
// что `Type::operator>>` (TypeSystem.cpp): значение входит в UNION, если входит
// хотя бы в один его член (случай BASE vs UNION). Состав union'а берётся из
// объявленного в программе значения типа: typunion склеивает его в "i8 | i16 | ...",
// и typcontains проверяет вхождение по этим же данным. Имён типов в рантайме
// не зашито — алиас любого пользовательского union'а работает так же, как Numeric.
static inline bool tag_accepts(Ctx& ctx, const std::string& pt, const std::string& at) {
    const Box* ty = ctx.find(pt);
    if (!ty || ty->t != Box::T_TYPE) return false;
    return typcontains(ty->s, at);
}

// выбор перегрузки по РАНТАЙМНЫМ типам аргументов; args[0] = self (тип = ключ таблицы).
static inline FnPtr dunder_select(Ctx& ctx, const std::vector<DunderEntry>& v,
                                  const std::vector<Box>& args) {
    FnPtr best = nullptr;
    int bestScore = -1;
    for (auto& e : v) {
        const std::vector<std::string>& pts = e.first;
        if (pts.size() + 1 != args.size()) continue;   // +1 за self
        int score = 0;
        bool ok = true;
        for (size_t i = 0; i < pts.size() && ok; ++i) {
            std::string at = tyname(args[i + 1]);
            const std::string& pt = pts[i];
            if (pt == "generic" || pt == "unknown" || pt == "any") {
                score += 1;   // wildcard: принимает любой тип
            } else if (at == pt) {
                score += 4;   // точное совпадение — наивысший приоритет
            } else if (tag_accepts(ctx, pt, at)) {
                score += 2;   // вхождение в объявленный union-тип параметра
            } else {
                ok = false;
            }
        }
        if (!ok) continue;
        if (score > bestScore) { bestScore = score; best = e.second; }
    }
    return best;
}
// рекурсивный guard для __assign__: пока выполняется __assign__ (внутри его тела),
// новые диспетчи __assign__ подавляются — вместо самовызова идёт почленное
// «вливание» (struct_import). Иначе `self = value` внутри __assign__ бесконечно
// вызывает сам себя (stack overflow, 0xC00000FD).
static int g_assign_guard = 0;
// попытка вызвать дюндер (с матчингом перегрузки); false => нет подходящей (fallback-семантика)
static inline bool dunder_try(Ctx& ctx, const std::string& ty, const std::string& d, std::vector<Box> args, Box& out) {
    auto it = g_dunders.find(ty);
    if (it == g_dunders.end()) return false;
    auto m = it->second.find(d);
    if (m == it->second.end()) return false;
    FnPtr f = dunder_select(ctx, m->second, args);
    if (!f) return false;
    // while выполняется __assign__ (его тело уже исполняется) вложенный диспетч
    // __assign__ подавляется: вместо самовызова идёт почленное «вливание» ниже в
    // assign_dispatch / setindex_dispatch. Иначе `self = value` внутри __assign__
    // бесконечно вызывает сам себя (stack overflow, 0xC00000FD).
    if (d == "__assign__") {
        if (g_assign_guard > 0) return false;
        ++g_assign_guard;
        out = f(ctx, std::move(args));
        --g_assign_guard;
        return true;
    }
    out = f(ctx, std::move(args));
    return true;
}
// __get__: автораспаковка значения при «чтении» (echo/out/outln). Если значение —
// объект структуры с зарегистрированным дюндером __get__, вызываем его
// (self = объект) и повторяем разворот (вложенные обёртки) с защитой от циклов.
    // Обычные (не-дюндерные) методы: тип-структуры -> (имя метода -> перегрузки).
    // Нужны для вызова `x.method(...)`, когда x — параметр типа-генерика
    // (`fn show[T](x: T) -> str { ret x.to_str(); }`): на этапе кодогенерации
    // конкретная структура неизвестна, поэтому метод ищется по типу значения
    // в рантайме — ровно так же, как op_dispatch ищет операторы.
    static std::map<std::string, std::map<std::string, std::vector<DunderEntry>>> g_methods;
    static inline void reg_method(const std::string& ty, const std::string& name,
                                  const std::vector<std::string>& tags, FnPtr fp) {
        g_methods[ty][name].push_back({tags, fp});
    }
    // Вызов метода по типу значения-ресивера. args[0] = self.
    // nparams — объявленное число параметров метода (без self): по нему выбирается
    // перегрузка, чтобы скрытые generic-аргументы метода (идущие после них) не ломали
    // сопоставление по арности. null — у типа нет такого метода.
    static inline Box method_dispatch(Ctx& ctx, const std::string& name,
                                      std::vector<Box> args, int nparams) {
        if (args.empty()) return Box::null();
        std::string ty = tyname(args[0]);
        auto it = g_methods.find(ty);
        if (it == g_methods.end()) return Box::null();
        auto m = it->second.find(name);
        if (m == it->second.end()) return Box::null();
        size_t want = (size_t)nparams + 1;
        if (want > args.size()) want = args.size();
        std::vector<Box> sel(args.begin(), args.begin() + want);
        FnPtr f = dunder_select(ctx, m->second, sel);
        if (!f) return Box::null();
        return f(ctx, std::move(args));
    }
    static inline Box auto_get(Ctx& ctx, Box v) {
    int guard = 0;
    while (v.t == Box::T_STRUCT && !v.tname.empty() && guard++ < 64) {
        Box out;
        if (!dunder_try(ctx, v.tname, "__get__", {v}, out)) break;
        v = std::move(out);
    }
    return v;
}
// цели (диспетч-вызов без write-back полагается на общую карту полей), т.е.
// `self = other` внутри __assign__ = почленное копирование, а не перенацеливание.
static inline void struct_import(Box& tgt, const Box& val) {
    if (tgt.t == Box::T_STRUCT && val.t == Box::T_STRUCT && tgt.sm && val.sm) {
        for (const auto& kv : *val.sm) (*tgt.sm)[kv.first] = kv.second;
    } else {
        tgt = val;
    }
}
// Полная таблица «символ оператора -> имя dunder».
// Символы берутся из CodeGenerator::binOf/cmpOf (CodeGenerator.cpp: binOf('+'),
// '-', '*', '/', '%', '&', '^', '|', 'l', 'r'; cmpOf('e', 'n', '<', '>', 'L', 'G')).
// Раньше здесь были только '+', 'e', 'n' — из-за чего остальные 13 dunder'ов
// (например __truediv__) никогда не вызывались: op_dispatch получал пустое имя,
// пропускал dunder_try и уходил в bin/cmp по двум структурам, отдавая null (или
// false для сравнений) без диагностики.
static inline std::string dname(char op) {
    switch (op) {
        case '+': return "__add__";
        case '-': return "__sub__";
        case '*': return "__mul__";
        case '/': return "__truediv__";
        case '%': return "__mod__";
        case 'e': return "__eq__";
        case 'n': return "__ne__";
        case '<': return "__lt__";
        case '>': return "__gt__";
        case 'L': return "__le__";
        case 'G': return "__ge__";
        case '&': return "__and__";
        case '^': return "__xor__";
        case '|': return "__or__";
        case 'l': return "__lshift__";
        case 'r': return "__rshift__";
        default:  return "";
    }
}
// бинарный оператор c диспетчеризацией дюндеров (l — ресивер), fallback — bin/cmp
static inline Box typunion(const Box& a, const Box& b);  // forward-decl (определение ниже)
static inline Box typnullable(const Box& t);
static inline bool typcontains(const std::string& container, const std::string& member);
static inline bool typassignable(const std::string& target, const std::string& source);
static inline Box op_dispatch(char op, Ctx& ctx, Box l, Box r) {
    // живые алиасы (T_REF/T_REFI) в выражениях читаются по значению цели —
    // `ref_arr[i] + 1`, `a_ref == 5`: операция видит значение, не ссылку.
    l = derefval(l);
    r = derefval(r);
    std::string d = dname(op);
    if (!d.empty() && l.t == Box::T_STRUCT) {
        Box out;
        if (dunder_try(ctx, l.tname, d, {l, r}, out)) return out;
    }
    if (op == '|' && l.t == Box::T_TYPE && r.t == Box::T_TYPE)
        return typunion(l, r);
    if (op == '|' && l.t == Box::T_TYPE && r.t == Box::T_NULL)
        return typunion(l, Box::typ("Null"));
    if (op == '|' && l.t == Box::T_NULL && r.t == Box::T_TYPE)
        return typunion(Box::typ("Null"), r);
    // << на типах: a << b — каждый элемент b содержится в a (b ⊆ a)
    if (op == 'l' && l.t == Box::T_TYPE && r.t == Box::T_TYPE)
        return Box::boo(typcontains(l.s, r.s));
    // >> на типах: a >> b — каждый элемент a содержится в b (a ⊆ b)
    if (op == 'r' && l.t == Box::T_TYPE && r.t == Box::T_TYPE)
        return Box::boo(typcontains(r.s, l.s));
    bool isCmp = (op == 'e' || op == 'n' || op == '<' || op == '>' || op == 'L' || op == 'G');
    return isCmp ? cmp(op, l, r) : bin(op, l, r);
}
// sizeof c диспетчеризацией __sizeof__
static inline Box szof_dispatch(Ctx& ctx, Box v) {
    v = derefval(v);
    if (v.t == Box::T_STRUCT) {
        Box out;
        if (dunder_try(ctx, v.tname, "__sizeof__", {v}, out)) return out;
    }
    return szof(v);
}

// имя типа по тегу Box — зеркало Type::toString для базовых/структур
static inline std::string tyname(const Box& v) {
    switch (v.t) {
        case Box::T_NULL:   return "Null";
        case Box::T_i8:     return "i8";
        case Box::T_i16:    return "i16";
        case Box::T_i32:    return "i32";
        case Box::T_i64:    return "i64";
        case Box::T_f16:    return "f16";
        case Box::T_f32:    return "f32";
        case Box::T_f64:    return "f64";
        case Box::T_BOOL:   return "bool";
        case Box::T_CHAR:   return "char";
        case Box::T_STR:    return "str";
        case Box::T_ARR: {
            if (v.items.empty()) return "{...}";
            return "{" + tyname(v.items[0]) + ", " + std::to_string(v.items.size()) + "}";
        }
        case Box::T_TUP: {
            std::string s = "[";
            for (size_t i = 0; i < v.items.size(); ++i) {
                if (i) s += ", ";
                s += tyname(v.items[i]);
            }
            return s + "]";
        }
        case Box::T_FN:     return "fn";
        case Box::T_CLOSURE: return "fn";
        case Box::T_TYPE:   return "Type";
        case Box::T_STRUCT: return v.tname.empty() ? "struct" : v.tname;
        case Box::T_PTR: {
            Box d = deref(v);
            if (d.t == Box::T_PTR) return "Null";
            return "*(" + tyname(d) + std::string(1, ')');
        }
        case Box::T_REF: return tyname(derefval(v));
        // T_REFI — живой алиас на элемент контейнера (`ref let x = arr[0]`),
        // derefval() его тоже разворачивает, имя типа — как у цели.
        case Box::T_REFI: return tyname(derefval(v));
    }
    return "Null";
}
// объявленный (статический) тип поля структуры для typeis(...): если при создании
// инстанса сохранён (generic-поля), возвращаем его; иначе — рантайм-тип значения поля.
static inline Box field_decltype(const Box& box, const std::string& name) {
    Box v = derefval(box);
    if (v.t == Box::T_STRUCT && v.fdecl) {
        auto it = v.fdecl->find(name);
        if (it != v.fdecl->end()) return Box::typ(it->second);
    }
    return Box::typ(tyname(field(v, name)));
}
// тип-значение (Box::typ / строка) -> имя типа
static inline std::string typename_of(const Box& t) {
    if (t.t == Box::T_TYPE) return t.s;
    if (t.t == Box::T_STR)  return t.s;
    return "";
}
// a | b на типах — объединение (union) с дедупликацией: i32 | f32 | i32 -> "i32 | f32"
static inline Box typunion(const Box& a, const Box& b) {
    std::string na = typename_of(a);
    std::string nb = typename_of(b);
    // собираем все имена из обоих юнионов
    std::vector<std::string> parts;
    auto split_union = [&](const std::string& s) {
        size_t pos = 0;
        while (pos < s.size()) {
            size_t sep = s.find(" | ", pos);
            if (sep == std::string::npos) { parts.push_back(s.substr(pos)); break; }
            parts.push_back(s.substr(pos, sep - pos));
            pos = sep + 3;
        }
    };
    if (!na.empty()) split_union(na);
    if (!nb.empty()) split_union(nb);
    // дедупликация (сохраняем порядок первого вхождения)
    std::vector<std::string> unique;
    for (auto& p : parts) {
        bool dup = false;
        for (auto& u : unique) if (u == p) { dup = true; break; }
        if (!dup) unique.push_back(p);
    }
    if (unique.empty()) return Box::typ("Null");
    if (unique.size() == 1) return Box::typ(unique[0]);
    std::string result = unique[0];
    for (size_t i = 1; i < unique.size(); ++i) result += " | " + unique[i];
    return Box::typ(result);
}
// ?T -> T | Null (nullable-тип): принимает тип-значение, возвращает объединённый тип
static inline Box typnullable(const Box& t) {
    return typunion(t, Box::typ("Null"));
}
// разбор юниона "i32 | f32" -> ["i32","f32"]; простой тип -> [name]
static inline std::vector<std::string> split_type(const std::string& s) {
    std::vector<std::string> r;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t sep = s.find(" | ", pos);
        if (sep == std::string::npos) { r.push_back(s.substr(pos)); break; }
        r.push_back(s.substr(pos, sep - pos));
        pos = sep + 3;
    }
    return r;
}
// << на типах: a << b — каждый элемент b содержится в a (b ⊆ a)
// super << (i32 | i64) -> true
static inline bool typcontains(const std::string& container, const std::string& member) {
    auto cv = split_type(container);
    auto mv = split_type(member);
    for (auto& m : mv) {
        bool found = false;
        for (auto& c : cv) { if (c == m) { found = true; break; } }
        if (!found) return false;
    }
    return true;
}
// примитивное имя для cast() (зеркало веток cast)
static inline bool cast_primitive_name(const std::string& tn) {
    return tn == "i8" || tn == "i16" || tn == "i32" || tn == "i64" ||
           tn == "f16" || tn == "f32" || tn == "f64" || tn == "bool" || tn == "char" || tn == "str";
}
// cast(expr, target) с диспетчеризацией __as__: `expr as T` -> `expr.__as__(T)`,
// если у структуры есть метод (зеркало DeepAnalizer NodeCast). Неизвестная
// динамическая цель (generic-параметр вроде "T"/"P") — тождество, как возврат lv
// интерпретатором после INVALID_CAST.
static inline Box cast_dispatch(Ctx& ctx, Box v, Box target) {
    v = derefval(v);
    std::string tn = typename_of(target);
    if (v.t == Box::T_STRUCT) {
        Box out;
        if (dunder_try(ctx, v.tname, "__as__", {v, Box::typ(tn)}, out)) return out;
        return v;
    }
    if (!tn.empty() && tyname(v) == tn) return v;
    if (cast_primitive_name(tn)) return cast(v, tn);
    return v;
}
// typeof c диспетчеризацией __typeof__
static inline Box typeof_dispatch(Ctx& ctx, Box v) {
    v = derefval(v);
    if (v.t == Box::T_STRUCT) {
        Box out;
        if (dunder_try(ctx, v.tname, "__typeof__", {v}, out)) return out;
    }
    // typeof отдаёт type-значение (T_TYPE, зеркало интерпретатора), чтобы
    // `typeof(x) == i64` сравнивалось с типовым литералом, а не со строкой.
    return Box::typ(tyname(v));
}
// ~x — битовое НЕ (только целые, как в DeepAnalizer)
static inline Box bnot(Box v) {
    if (v.t == Box::T_i8)  return Box::i8((char)~v.i);
    if (v.t == Box::T_i16) return Box::i16((short)~v.i);
    if (v.t == Box::T_i32) return Box::i32((long long)(int)~v.i);
    if (v.t == Box::T_i64) return Box::i64(~(long long)v.i);
    return Box::null();
}

// Унарные операторы с диспетчеризацией дюндеров. Раньше эмиттер писал напрямую
// rt::neg/rt::not_/rt::bnot, а unary plus просто отдавал операнд, — поэтому
// __neg__/__pos__/__bool__/__invert__ компилировались и регистрировались
// (reg_dunder), но никогда не вызывались: встроенные функции переключаются по
// типу бокса и для T_STRUCT уходили в `return Box::null()`. Метод был мёртвым кодом.
// Зеркалим op_dispatch: сначала дюндер, при его отсутствии — встроенное поведение.
static inline std::string uname(char op) {
    switch (op) {
        case '-': return "__neg__";
        case '+': return "__pos__";
        case '!': return "__bool__";
        case '~': return "__invert__";
        default:  return "";
    }
}
static inline Box unary_dispatch(char op, Ctx& ctx, Box v) {
    // живые алиасы (T_REF/T_REFI) читаются по значению цели, как в op_dispatch
    v = derefval(v);
    std::string d = uname(op);
    if (!d.empty() && v.t == Box::T_STRUCT) {
        Box out;
        if (dunder_try(ctx, v.tname, d, {v}, out)) {
            // __bool__ задаёт ИСТИННОСТЬ объекта (как bool(x) в Python) и потому
            // обслуживает и условие, и `!`. Поэтому `!x` — это отрицание результата,
            // а не сам результат: !a == not __bool__(a), !!a == __bool__(a).
            // Остальные дюндеры (__neg__/__pos__/__invert__) уже возвращают значение
            // выражения, инверсия им не нужна.
            if (op == '!') return not_(out);
            return out;
        }
    }
    switch (op) {
        case '-': return neg(v);
        case '+': return v;
        case '!': return not_(v);   // !{...} на массиве -> кортеж (не дюндер)
        case '~': return bnot(v);
        default:  return v;
    }
}

// Правдивость значения в УСЛОВИИ (if/while/for). Старая truthy(const Box&)
// проверяла только bool и число, поэтому структура с __bool__ попадала в
// always-false: `if (a)` компилировался в ветку, которая никогда не бралась.
// Зеркалим unary_dispatch('!'): сначала дюндер, затем встроенное поведение —
// и DeepAnalizer::truthyOf решает точно так же, поэтому статический анализ и
// рантайм не расходятся.
static inline bool truthy(Ctx& ctx, const Box& v0) {
    Box v = derefval(v0);
    if (v.t == Box::T_BOOL) return v.b;
    if (is_num(v)) return num(v) != 0;
    if (v.t == Box::T_STRUCT) {
        Box out;
        if (dunder_try(ctx, v.tname, "__bool__", {v}, out))
            return out.t == Box::T_BOOL ? out.b : false;
    }
    return false;
}

// Индексация c диспетчеризацией __get_index__ для структур; fallback — встроенный index().
static inline Box index_dispatch(Ctx& ctx, Box v, Box idx) {
    v = deref(v);
    if (v.t == Box::T_REF || v.t == Box::T_REFI) v = derefval(v);
    if (v.t == Box::T_STRUCT) {
        Box out;
        if (dunder_try(ctx, v.tname, "__get_index__", {v, idx}, out)) return out;
    }
    return index(v, idx);
}

// запись `a[i] = v` — диспетчеризация на элементы-алиасы требует assign_dispatch,
// определённую ниже; forward-объявление.
static inline Box assign_dispatch(Ctx& ctx, Box& v, Box val);

// Запись a[i] = v c диспетчеризацией __set_index__ (write-back). Принимает
// живую ячейку по ссылке: элементы массива/кортежа меняются в-месте, а не в копии.
static inline Box setindex_dispatch(Ctx& ctx, Box& v, Box idx, Box val) {
    Box* target = &v;
    if (v.t == Box::T_PTR) { if (Box* c = ptr_cell(v)) target = c; }
    if (target->t == Box::T_STRUCT) {
        Box out;
        if (dunder_try(ctx, target->tname, "__set_index__", {*target, idx, val}, out)) return out;
    }
    if (is_int_t(idx) && (target->t == Box::T_ARR || target->t == Box::T_TUP)) {
        long long i = inum(idx);
        if (i >= 0 && i < (long long)target->items.size()) {
            Box& cell = target->items[(size_t)i];
            // элемент присваивается «по текущему значению»: если текущий элемент — struct
            // с __assign__ (как и для поля/переменной), диспатчим его, иначе — замена.
            if (cell.t == Box::T_STRUCT) {
                Box out;
                if (dunder_try(ctx, cell.tname, "__assign__", {cell, val}, out)) return *target;
                if (g_assign_guard > 0 && val.t == Box::T_STRUCT) {
                    struct_import(cell, val);   // внутри __assign__: почленное вливание
                    return *target;
                }
            }
            // элемент — живой алиас (ref-массив: `{a_ref, b_ref}`): запись идёт
            // сквозь ссылку в общую ячейку, алиас в массиве сохраняется.
            if (cell.t == Box::T_REF && cell.sp) return assign_dispatch(ctx, *cell.sp, std::move(val));
            if (cell.t == Box::T_REFI) {
                Box* c = refi_cell(cell);
                if (c) return assign_dispatch(ctx, *c, std::move(val));
            }
            cell = std::move(val);
        }
    }
    return *target;
}
// rvalue-база (a[i] = v, где a — временная копия): мутация теряется, но компилируется.
static inline Box setindex_dispatch(Ctx& ctx, Box&& v, Box idx, Box val) {
    return setindex_dispatch(ctx, v, idx, std::move(val));
}

// Запись `x = v` c диспетчеризацией __assign__. Если текущее значение ячейки —
// struct с методом __assign__, вызываем его (self = текущее значение, val — новый);
// мутации полей self идут в общую sm-карту и видны в ячейке. Иначе — обычная замена.
// ВАЖНО: НЕ разворачиваем T_PTR — переменная-указатель при `p = v` должна
// ПЕРЕПРИВЯЗЫВАТЬСЯ, а не писать сквозь себя (разворачивание выполняют
// setptr_dispatch / setref_dispatch ДО вызова assign_dispatch).
static inline Box assign_dispatch(Ctx& ctx, Box& v, Box val) {
    Box* target = &v;
    bool guarded = false;
    if (target->t == Box::T_STRUCT) {
        Box out;
        if (dunder_try(ctx, target->tname, "__assign__", {*target, val}, out)) return val;
        guarded = (g_assign_guard > 0);
    }
    if (guarded && target->t == Box::T_STRUCT && val.t == Box::T_STRUCT)
        struct_import(*target, val);
    else
        *target = std::move(val);
    return *target;
}
// `a += b` / `a = a + b` in-place (эмитится CodeGenerator для канонической формы
// составного присваивания на обычной переменной). Два массива — доклеиваем элементы
// RHS в живую ячейку без копирования всей конкатенации (value-семантика Box:
// другие копии переменных не разделяют items). RHS передаётся значением — `a += a`
// безопасен. Не-массивы (числа/строки/кортежи) — прежняя семантика a = a + rhs.
static inline Box arr_plus_assign(Ctx& ctx, Box& cell, Box rhs) {
    if (cell.t == Box::T_ARR && rhs.t == Box::T_ARR) {
        cell.items.reserve(cell.items.size() + rhs.items.size());
        for (auto& e : rhs.items) cell.items.push_back(std::move(e));
        return cell;
    }
    return assign_dispatch(ctx, cell, op_dispatch('+', ctx, cell, std::move(rhs)));
}
// *p = v / ref-алиас = v: диспетчеризуем __assign__ на цели, а не на самой ссылке.
// ptr принимается по значению: цель — это ячейка heap (общий shared_ptr), а выражения
// вида `*field(...) = v` дают временный Box-указатель, который нельзя связать с Box&.
static inline Box setptr_dispatch(Ctx& ctx, Box ptr, Box val) {
    if (ptr.t == Box::T_PTR) { if (Box* c = ptr_cell(ptr)) return assign_dispatch(ctx, *c, std::move(val)); }
    return assign_dispatch(ctx, ptr, std::move(val));
}
static inline Box setref_dispatch(Ctx& ctx, Box& slot, Box val) {
    if (slot.t == Box::T_REF && slot.sp) return assign_dispatch(ctx, *slot.sp, std::move(val));
    if (slot.t == Box::T_REFI) {
        Box* c = refi_cell(slot);
        if (c) return assign_dispatch(ctx, *c, std::move(val));
    }
    return assign_dispatch(ctx, slot, std::move(val));
}
// obj.field = v: диспетчеризуем __assign__ на текущем значении поля (shared_map).
static inline void putf_dispatch(Ctx& ctx, const Box& v, const std::string& name, Box val) {
    std::map<std::string, Box>* m = smap(v);
    if (!m) return;
    auto it = m->find(name);
    if (it == m->end()) { (*m)[name] = std::move(val); return; }
    assign_dispatch(ctx, it->second, std::move(val));
}

// ref let x = <массив/кортеж>[i] — живой алиас-индекс (T_REFI) на элемент.
// host_cell — ячейка-носитель контейнера (refcell_ptr(env.get(a)) / C++-локаль / sp ref).
// Элемент резолвится на каждый доступ через текущий контейнер: алиасы безопасны при
// переназначении/пересоздании контейнера (решает проблему повисающих shared_ptr-ов).
static inline Box refindex_cell(const std::shared_ptr<Box>& host_cell, Box idx) {
    if (!host_cell || !is_int_t(idx)) return Box::null();
    long long i = inum(idx);
    Box& h = *host_cell;
    if (h.t != Box::T_ARR && h.t != Box::T_TUP) return Box::null();
    if (i < 0 || i >= (long long)h.items.size()) return Box::null();
    Box v; v.t = Box::T_REFI; v.host = host_cell; v.refi = i;
    return v;
}

} // namespace rt