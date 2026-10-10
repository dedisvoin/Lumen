#include "NodeBase.cpp"
#include "type_traits"
#include "cstdlib"
#include "cstdint"
#include "../../utils/Strings.cpp"

#pragma once


struct NodeNumber final: public Node  {
    bool is_integer;
    int32_t int_value;
    long double float_value;
    
    NodeNumber(int32_t value) : Node(NodeTypes::NodeNumber, EX) {
        is_integer = true;
        int_value = value;
    }
    
    NodeNumber(long double value) : Node(NodeTypes::NodeNumber, EX) {
        is_integer = false;
        float_value = value;
    }
    
    NodeNumber(String value) : Node(NodeTypes::NodeNumber, EX) {
        if (isInteger(value)) {
            is_integer = true;
            int_value = genInt32(value);
        } else {
            is_integer = false;
            float_value = genLongDouble(value);
        }
    }
    
    inline static bool isCorrectNumber(String value) {
        if (value.count('.') > 1) return false;
        return true;
    }
    
    inline static bool isInteger(String value) {
        if (value.count('.') == 0) return true;
        return false;
    }
    
    inline static int32_t genInt32(String value) {
        // Префиксные литералы: 0x/0X (hex), 0b/0B (bin); всё остальное — десятичное
        // через std::stoll (сохраняет прежнее поведение в т.ч. для мусора).
        int base = 0;
        String digits = value;
        if (value.size() > 2 && value[0] == U'0') {
            char32_t p = value[1];
            if (p == U'x' || p == U'X') { base = 16; digits = value.substr(2); }
            else if (p == U'b' || p == U'B') { base = 2;  digits = value.substr(2); }
        }
        if (base == 0) {
            try {
                return static_cast<int32_t>(std::stoll(value.to_utf8()));
            } catch (const std::exception& e) {
                return 0; // или выбросить исключение
            }
        }
        int64_t result = 0;
        for (char32_t ch : digits) {
            int d;
            if (ch >= U'0' && ch <= U'9') d = ch - U'0';
            else if (ch >= U'a' && ch <= U'f') d = ch - U'a' + 10;
            else if (ch >= U'A' && ch <= U'F') d = ch - U'A' + 10;
            else return 0; // невалидная цифра
            if (d >= base) return 0;
            result = result * base + d;
        }
        return static_cast<int32_t>(result);
    }
    
    inline static long double genLongDouble(String value) {
        return static_cast<long double>(std::stod(value.to_utf8()));
    }
};