#include "../core/TypeSystem.cpp"
#include "../utils/Strings.cpp"
#include <any>
#include <map>
#include <cstring>
#include <cstdint>
#pragma once

using namespace std;

// Единый нетemplate-Value: тип хранится в std::any,
// а Type T — для интроспекции/codegen.
struct Value {
    Type T;
    std::any value;

    Value(const Type& t, std::any v) : T(t), value(std::move(v)) {}
    Value(const Value&) = default;

    // извлечь значение как конкретный C++-тип (тип указывает вызывающий)
    template <typename A>
    A get() const {
        return std::any_cast<A>(value);
    }

    // извлечь мутабельную ссылку в хранимый объект (для in-place модификации,
    // напр. присваивания члену неймспейса). НЕ копирует.
    template <typename A>
    A& get_ref() {
        return std::any_cast<A&>(value);
    }

    // сырой std::any (без приведения)
    const std::any& raw() const { return value; }
};


struct Null final {
    Null() = default;
};

// integers
Value new_i8(int8_t value)   { return Value(Ty::i8,   value); }
Value new_i16(int16_t value)  { return Value(Ty::i16,  value); }
Value new_i32(int32_t value)  { return Value(Ty::i32,  value); }
Value new_i64(int64_t value)  { return Value(Ty::i64,  value); }

// floats: IEEE-754 binary16 (half) хранится как 16-битные биты.
// double <-> half с округлением к ближайшему чётному (round-to-nearest-even).
static inline uint16_t doubleToHalf(double value) {
    float f = static_cast<float>(value);
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    uint16_t sign = (uint16_t)((bits >> 16) & 0x8000u);
    uint32_t e = (bits >> 23) & 0xFFu;
    uint32_t m = bits & 0x7FFFFFu;
    if (e == 0xFF)  // inf/nan -> half inf/nan (nan сохраняет quiet-бит)
        return (uint16_t)(sign | 0x7C00u | (m ? 0x200u : 0u));
    int32_t exp = (int32_t)e - 127 + 15;
    if (exp >= 0x1F) return (uint16_t)(sign | 0x7C00u);  // переполнение -> inf
    if (exp <= 0) {
        if (exp < -10) return sign;                       // недостаточность -> signed zero
        m |= 0x800000u;                                  // неявная ведущая 1
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t mant = m >> shift;
        uint32_t rem  = m & ((1u << shift) - 1u);
        uint32_t half = 1u << (shift - 1);
        if (rem > half || (rem == half && (mant & 1))) ++mant;
        if (mant > 0x3FFu) { mant = 0x400u; }
        return (uint16_t)(sign | (uint16_t)mant);
    }
    m >>= 13;
    uint32_t rem = bits & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (m & 1))) ++m;
    if (m > 0x3FFu) { m = 0; ++exp; }
    if (exp >= 0x1F) return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint16_t)exp << 10) | (uint16_t)m);
}

static inline double halfToDouble(uint16_t h) {
    uint16_t sign = (uint16_t)(h & 0x8000u);
    uint16_t exp  = (uint16_t)((h >> 10) & 0x1Fu);
    uint16_t mant = (uint16_t)(h & 0x3FFu);
    uint32_t bits;
    if (exp == 0x1F) {
        bits = ((uint32_t)sign << 16) | 0x7F800000u | ((uint32_t)mant << 13);
    } else if (exp == 0) {
        if (mant == 0) {
            bits = (uint32_t)sign << 16;                 // signed zero
        } else {
            int e = -15;                                 // subnormal -> normalize
            uint32_t m = mant;
            while (!(m & 0x400u)) { m <<= 1; --e; }
            e += 127;
            bits = ((uint32_t)sign << 16) | ((uint32_t)(e & 0xFF) << 23) | ((m & 0x3FFu) << 13);
        }
    } else {
        bits = ((uint32_t)sign << 16) | ((uint32_t)(exp - 15 + 127) << 23) | ((uint32_t)mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return static_cast<double>(f);
}

Value new_f16(double value)   { return Value(Ty::f16,  doubleToHalf(value)); }
Value new_f32(float value)    { return Value(Ty::f32,  value); }
Value new_f64(double value)   { return Value(Ty::f64,  value); }

// bools
Value new_bool(bool value)    { return Value(Ty::Bool, value); }
Value new_bool(int value)     { return Value(Ty::Bool, static_cast<bool>(value)); }
Value new_bool(String value)  {
    if (value == "true") return Value(Ty::Bool, true);
    else if (value == "false") return Value(Ty::Bool, false);
    else return Value(Ty::Bool, false);
}

// char
Value new_char(char value)    { return Value(Ty::Char, value); }

// string (кастомный String)
Value new_string(const String& value) { return Value(Ty::Str, value); }

// types
Value new_type(const Type& type)     { return Value(Ty::Type_, type); }

// array / tuple (tuple хранит тот же контейнер — вектор значений)
// COW: данные в shared_ptr — копирование Value (а с ним RuntimeArray)
// O(1); мутация через mut() детачит общее владение.
struct Node;    // интерпретаторные типы для ref-алиасов (forward из core/Nodes)
struct Context;
struct RuntimeArray {
    std::shared_ptr<std::vector<Value>> data;
    // индекс -> живой алиас (ref-элемент литерала `{a_ref, b_ref}`): чтение и
    // запись такого элемента идут к цели, а не к скопированному значению.
    std::map<size_t, std::pair<Node*, Context*>> refs;
    RuntimeArray() : data(std::make_shared<std::vector<Value>>()) {}
    explicit RuntimeArray(std::vector<Value> it) : data(std::make_shared<std::vector<Value>>(std::move(it))) {}

    // читающий доступ к элементам (один владелец или нет — все видят одно состояние)
    const std::vector<Value>& view() const { return *data; }
    // мутирующий доступ с copy-on-write: при общем владении отщепляем копию
    std::vector<Value>& mut() {
        if (data.use_count() > 1) data = std::make_shared<std::vector<Value>>(*data);
        return *data;
    }
    size_t size() const { return data->size(); }
};

Value new_array(const Type& type, std::vector<Value> items) {
    return Value(type, RuntimeArray(std::move(items)));
}

// struct: поля — упорядоченная карта имя -> значение; тип — StructType
// COW: данные в shared_ptr — копирование Value (а с ним RuntimeStruct)
// O(1); мутация через mut() детачит общее владение.
struct RuntimeStruct {
    std::shared_ptr<std::map<String, Value>> data;
    RuntimeStruct() : data(std::make_shared<std::map<String, Value>>()) {}
    explicit RuntimeStruct(std::map<String, Value> f) : data(std::make_shared<std::map<String, Value>>(std::move(f))) {}

    // читающий доступ к полям
    const std::map<String, Value>& view() const { return *data; }
    // мутирующий доступ с copy-on-write: при общем владении отщепляем копию
    std::map<String, Value>& mut() {
        if (data.use_count() > 1) data = std::make_shared<std::map<String, Value>>(*data);
        return *data;
    }
};

Value new_struct(const Type& type, std::map<String, Value> fields) {
    return Value(type, RuntimeStruct(std::move(fields)));
}

// null
Value new_null() { return Value(Ty::Null, Null()); }


template <typename T = double>
T getNumberFromType(Value value) {
    if (value.T == Ty::i8) return static_cast<int8_t>(value.get<int8_t>());
    if (value.T == Ty::i16) return static_cast<int16_t>(value.get<int16_t>());
    if (value.T == Ty::i32) return static_cast<int32_t>(value.get<int32_t>());
    if (value.T == Ty::i64) return static_cast<int64_t>(value.get<int64_t>());
    if (value.T == Ty::f16) return static_cast<T>(halfToDouble(value.get<uint16_t>()));
    if (value.T == Ty::f32) return static_cast<float>(value.get<float>());
    if (value.T == Ty::f64) return static_cast<double>(value.get<double>());
    return 0;
}

bool isNumberType(const Type& type) {
    return type >> Ty::Numeric;
}

bool isIntegerType(const Type& type) {
    return type >> Ty::Integer;
}

bool isFloatType(const Type& type) {
    return type >> Ty::Float;
}

template <typename T = Value>
Value negationNumber(Value value) {
    if (value.T == Ty::i8) return new_i8(-getNumberFromType<>(value));
    if (value.T == Ty::i16) return new_i16(-getNumberFromType<>(value));
    if (value.T == Ty::i32) return new_i32(-getNumberFromType<>(value));
    if (value.T == Ty::i64) return new_i64(-getNumberFromType<>(value));
    if (value.T == Ty::f16) return new_f16(-getNumberFromType<>(value));
    if (value.T == Ty::f32) return new_f32(-getNumberFromType<>(value));
    if (value.T == Ty::f64) return new_f64(-getNumberFromType<>(value));
    return Value(Ty::Null, 0);
}
