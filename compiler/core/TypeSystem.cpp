#include "vector"
#include <string>
#include <cassert>
#include <type_traits>
#include "../utils/Strings.cpp"

#pragma once

static int id_counter = 0;
#define GET_UNIQUE_ID()      id_counter++

enum TypeType {
    BASE, POINTER, UNION, CALLABLE, ARRAY, TUPLE, STRUCT, ENUM
};


struct Type {
    TypeType type;
    int id;

    String name;

    // container for:
    // function arguments, generic type parameters, unions
    std::vector<Type> container_1;
    // container for:
    // function generic type parameters
    std::vector<Type> container_2;
    // container for:
    // function return type
    std::vector<Type> container_3;

    // для STRUCT: value-generic аргументы (Size = 3) в порядке позиций
    std::vector<long long> int_args;
    // для STRUCT: mask — бит i говорит, что i-й параметр — тип (1) или значение (0)
    uint32_t arg_mask = 0;

    // flags for: auto, any
    bool is_auto = false;
    bool is_any = false;


    // size for: array elements, generic type parameters
    int size;

    Type(TypeType type, String name = "") : type(type), id(GET_UNIQUE_ID()), name(name), size(-1) {}
    Type(TypeType type, int id) : type(type), id(id), size(-1) {}
    Type(TypeType type, String name, bool is_auto, bool is_any = false) : type(type), id(GET_UNIQUE_ID()), name(name), is_auto(is_auto), is_any(is_any), size(-1) {}

    Type(Type&& _T) : type(_T.type), id(_T.id), name(_T.name), is_auto(_T.is_auto), is_any(_T.is_any), container_1(std::move(_T.container_1)), container_2(std::move(_T.container_2)), container_3(std::move(_T.container_3)), size(_T.size), int_args(std::move(_T.int_args)), arg_mask(_T.arg_mask) {}
    Type(const Type& _T) : type(_T.type), id(_T.id), name(_T.name), is_auto(_T.is_auto), is_any(_T.is_any), container_1(_T.container_1), container_2(_T.container_2), container_3(_T.container_3), size(_T.size), int_args(_T.int_args), arg_mask(_T.arg_mask) {}

    Type& operator=(const Type&) = default;
    Type& operator=(Type&&)   = default;   // опционально


    inline bool isAny() const { return is_any; }
    inline bool isAuto() const { return is_auto; }

    static Type ArrayType(Type _T, int size = -1) {
        Type _U = Type(TypeType::ARRAY);
        _U.container_1.push_back(_T);
        _U.size = size;
        return _U;
    }

    static Type TupleType(std::vector<Type> _Ts) {
        Type _U = Type(TypeType::TUPLE);
        _U.container_1 = std::move(_Ts);
        return _U;
    }

    static Type FunctionType(std::vector<Type> _Ps, Type _R, std::vector<Type> _Ts = {}) {
        Type _U = Type(TypeType::CALLABLE);
        _U.container_1 = std::move(_Ps);
        _U.container_2 = std::move(_Ts);
        _U.container_3.push_back(_R);
        return _U;
    }

    // Структура: имя + generic-аргументы. mask: бит i = 1 => i-й параметр тип,
    // 0 => значение (берётся из int_args по порядку).
    static Type StructType(const String& name,
                           std::vector<Type> targs = {},
                           std::vector<long long> vargs = {},
                           uint32_t mask = 0) {
        Type _U = Type(TypeType::STRUCT, name);
        _U.container_1 = std::move(targs);
        _U.int_args = std::move(vargs);
        _U.arg_mask = mask;
        return _U;
    }

    // C++-enum: тип по имени, значения — последовательные константы.
    static Type EnumType(const String& name) {
        Type _U = Type(TypeType::ENUM, name);
        return _U;
    }

    inline bool isBase() const      { return type == BASE;      }
    inline bool isPointer() const   { return type == POINTER;   }
    inline bool isUnion() const     { return type == UNION;     }
    inline bool isCallable() const  { return type == CALLABLE;  }
    inline bool isArray() const     { return type == ARRAY;     }
    inline bool isTuple() const     { return type == TUPLE;     }
    inline bool isStruct() const    { return type == STRUCT;    }
    inline bool isEnum() const      { return type == ENUM;      }


    const String toString() const {
        switch (type) {
            case BASE:    {
                if (name.empty()) return std::to_string(id);
                return name;
            }
            case POINTER: return "*(" + container_1[0].toString() + ")";
            case UNION: {
                String s;
                for (size_t i = 0; i < container_1.size(); i++) {
                    s += container_1[i].toString();
                    if (i + 1 < container_1.size()) s += " | ";
                }
                return s;
            }
            case ARRAY: {
                String s = "{" + container_1[0].toString();
                if (size != -1) s += String(", ") + std::to_string(size);
                else s += ", ...";
                return s + "}";
            }
            case TUPLE: {
                String s = "[";
                for (size_t i = 0; i < container_1.size(); i++) {
                    s += container_1[i].toString();
                    if (i + 1 < container_1.size()) s += ", ";
                }
                return s + "]";
            }
            case CALLABLE: {
                String s = "(Function[";
                for (size_t i = 0; i < container_2.size(); i++) {
                    s += container_2[i].toString();
                    if (i + 1 < container_2.size()) s += ", ";
                }
                s += "](";
                for (size_t i = 0; i < container_1.size(); i++) {
                    s += container_1[i].toString();
                    if (i + 1 < container_1.size()) s += ", ";
                }
                if (container_3.empty()) return s + ") -> <unknown>)";
                return s + ") -> " + container_3[0].toString() + ")";
            }
            case STRUCT: {
                size_t total = container_1.size() + int_args.size();
                if (total == 0) return name;
                String s = name + "[";
                size_t ti = 0, vi = 0;
                for (size_t i = 0; i < total; i++) {
                    if (arg_mask & (1u << i)) {
                        s += container_1[ti].toString(); ti++;
                    } else {
                        s += String(std::to_string(int_args[vi])); vi++;
                    }
                    if (i + 1 < total) s += ", ";
                }
                return s + "]";
            }
            case ENUM:
                return name;

            default: return "Unknown";
        }
        return "Unknown";
    }

    inline Type operator*() const {
        // Построение типа-указателя на *this для ЛЮБОГО типа:
        // базы, указатели (pointer-to-pointer), массивы/кортежи/union'ы/functions.
        Type _T = Type(TypeType::POINTER, id);
        _T.container_1.push_back(*this);
        return _T;
    }

    inline Type operator|(const Type _T) const {
        if (isAny() || _T.isAny()) return Type(TypeType::BASE, "any", false, true);
        std::vector<Type> members;
        if (isUnion()) {
            for (const auto& e : container_1) members.push_back(e);
        } else {
            members.push_back(*this);
        }
        if (_T.isUnion()) {
            for (const auto& e : _T.container_1) members.push_back(e);
        } else {
            members.push_back(_T);
        }

        std::vector<Type> unique;
        for (const auto& e : members) {
            bool dup = false;
            for (const auto& u : unique) {
                if (u == e) { dup = true; break; }
            }
            if (!dup) unique.push_back(e);
        }

        if (unique.size() == 1) return unique[0];

        Type _U = Type(TypeType::UNION);
        _U.container_1 = std::move(unique);
        return _U;
    }

    inline bool operator==(const Type& _T) const {
        if (isAny() || _T.isAny()) return true;
        if (type != _T.type) return false;
        switch (type) {
            case CALLABLE: {
                if (container_1.size() != _T.container_1.size()) return false;
                if (container_3.empty() || _T.container_3.empty())
                    return container_3.empty() == _T.container_3.empty();
                for (size_t i = 0; i < container_1.size(); i++)
                    if (!(container_1[i] == _T.container_1[i])) return false;
                if (!(container_3[0] == _T.container_3[0])) return false;
                return container_2 == _T.container_2;
            }
            case ARRAY: {
                if (size != _T.size) return false;
                if (container_1.empty() || _T.container_1.empty()) return container_1.empty() == _T.container_1.empty();
                return container_1[0] == _T.container_1[0];
            }
            case TUPLE: {
                if (container_1.size() != _T.container_1.size()) return false;
                for (size_t i = 0; i < container_1.size(); i++)
                    if (!(container_1[i] == _T.container_1[i])) return false;
                return true;
            }
            case POINTER: {
                if (container_1.empty() || _T.container_1.empty()) return container_1.empty() == _T.container_1.empty();
                return container_1[0] == _T.container_1[0];
            }
            case UNION: {
                if (container_1.size() != _T.container_1.size()) return false;
                for (const auto& e : container_1) {
                    bool found = false;
                    for (const auto& f : _T.container_1) {
                        if (e == f) { found = true; break; }
                    }
                    if (!found) return false;
                }
                return true;
            }
            case STRUCT: {
                if (name != _T.name) return false;
                if (arg_mask != _T.arg_mask) return false;
                if (container_1.size() != _T.container_1.size()) return false;
                for (size_t i = 0; i < container_1.size(); i++)
                    if (!(container_1[i] == _T.container_1[i])) return false;
                return int_args == _T.int_args;
            }
            case ENUM:
                return name == _T.name;
            default:
                return id == _T.id;
        }
    }

    inline bool operator!=(const Type& _T) const {
        return !operator==(_T);
    }

    inline bool operator>>(const Type _T) const {
        if (isAny() && _T.isAny()) return true;
        if (_T.isAny()) return true;
        if (isAny()) return false;
        if (_T.isAuto()) return true;   // целевой auto принимает всё (напр. Array[auto])
        if (*this == _T) return true;

        // StructType <: Type
        if (type == BASE && name == "StructType" && _T.type == BASE && _T.name == "Type") return true;
        if (_T.type == BASE && _T.name == "StructType") return (*this == _T);
        if (_T.type == UNION) {
            for (const auto& e : _T.container_1)
                if (*this >> e) return true;
        }

        switch (type) {
            case BASE:
                if (_T.type == UNION) {
                    for (const auto& e : _T.container_1)
                        if (*this >> e) return true;
                }
                if (_T.type == BASE && _T.name == "Type") {
                    if (name == "StructType") return true;
                    return false;
                }
                return false;

            case POINTER:
                if (_T.type == POINTER && !container_1.empty() && !_T.container_1.empty())
                    return container_1[0] >> _T.container_1[0];
                if (_T.type == UNION) {
                    for (const auto& e : _T.container_1)
                        if (*this >> e) return true;
                }
                return false;

            case ARRAY:
                if (_T.type == ARRAY && !container_1.empty() && !_T.container_1.empty()) {
                    if (size != -1 && _T.size != -1 && size != _T.size) return false;
                    return container_1[0] >> _T.container_1[0];
                }
                if (_T.type == UNION) {
                    for (const auto& e : _T.container_1)
                        if (*this >> e) return true;
                }
                return false;

            case TUPLE:
                if (_T.type == TUPLE && container_1.size() == _T.container_1.size()) {
                    for (size_t i = 0; i < container_1.size(); i++)
                        if (!(container_1[i] >> _T.container_1[i])) return false;
                    return true;
                }
                if (_T.type == UNION) {
                    for (const auto& e : _T.container_1)
                        if (*this >> e) return true;
                }
                return false;

            case CALLABLE:
                if (_T.type == CALLABLE &&
                    container_1.size() == _T.container_1.size() &&
                    container_2.size() == _T.container_2.size() &&
                    !container_3.empty() && !_T.container_3.empty()) {
                    for (size_t i = 0; i < container_2.size(); i++)
                        if (!(container_2[i] >> _T.container_2[i]) && !(container_2[i] == _T.container_2[i])) return false;
                    for (size_t i = 0; i < container_1.size(); i++)
                        if (!(container_1[i] >> _T.container_1[i])) return false;
                    return container_3[0] >> _T.container_3[0];
                }
                if (_T.type == UNION) {
                    for (const auto& e : _T.container_1)
                        if (*this >> e) return true;
                }
                return false;

            case UNION:
                for (const auto& e : container_1) {
                    bool ok = false;
                    if (_T.type == UNION) {
                        for (const auto& t : _T.container_1)
                            if (e >> t) { ok = true; break; }
                    } else {
                        ok = (e >> _T);
                    }
                    if (!ok) return false;
                }
                return true;

            case STRUCT:
                if (_T.type == UNION) {
                    for (const auto& e : _T.container_1)
                        if (*this >> e) return true;
                    return false;
                }
                if (_T.type != STRUCT) return false;
                if (name != _T.name) return false;
                if (arg_mask != _T.arg_mask) return false;
                if (container_1.size() != _T.container_1.size()) return false;
                for (size_t i = 0; i < container_1.size(); i++)
                    if (!(container_1[i] >> _T.container_1[i])) return false;
                return int_args == _T.int_args;

            default:
                return false;
        }
    }

    inline bool operator<<(const Type _T) const {
        return _T >> *this;
    }
};

namespace Ty {
    Type Auto =      Type(TypeType::BASE, "auto", true);

    Type Any =       Type(TypeType::BASE, "any", false, true);

    Type Null =      Type(TypeType::BASE, "Null");

    Type i8 =        Type(TypeType::BASE, "i8");
    Type i16 =       Type(TypeType::BASE, "i16");
    Type i32 =       Type(TypeType::BASE, "i32");
    Type i64 =       Type(TypeType::BASE, "i64");

    Type f16 =       Type(TypeType::BASE, "f16");
    Type f32 =       Type(TypeType::BASE, "f32");
    Type f64 =       Type(TypeType::BASE, "f64");

    Type Bool =      Type(TypeType::BASE, "bool");
    Type Char =      Type(TypeType::BASE, "char");
    Type Str =       Type(TypeType::BASE, "str");
    Type Type_ =     Type(TypeType::BASE, "Type");
    Type Namespace = Type(TypeType::BASE, "Namespace");

    Type Numeric = i8 | i16 | i32 | i64 | f16 | f32 | f64;

    Type Integer = i8 | i16 | i32 | i64;
    Type Float = f16 | f32 | f64;

    template <typename T = void>
    Type getType() {
        if constexpr (std::is_same_v<T, int8_t>) return i8;
        if constexpr (std::is_same_v<T, int16_t>) return i16;
        if constexpr (std::is_same_v<T, int32_t>) return i32;
        if constexpr (std::is_same_v<T, int64_t>) return i64;
        if constexpr (std::is_same_v<T, int>) return i32;
        if constexpr (std::is_same_v<T, float>) return f32;
        if constexpr (std::is_same_v<T, double>) return f64;
        if constexpr (std::is_same_v<T, bool>) return Bool;
        if constexpr (std::is_same_v<T, char>) return Char;
        if constexpr (std::is_same_v<T, ::String>) return Str;
        if constexpr (std::is_same_v<T, Type>) return Type_;
        if constexpr (std::is_same_v<T, void>) return Null;
    }
}
