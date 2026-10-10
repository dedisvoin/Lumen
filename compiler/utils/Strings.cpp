#include <string>
#include <Windows.h>
#include "vector"
#include <iostream>
#pragma once


static inline void set_console_utf8() {
    static bool initialized = false;
    if (!initialized) {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        initialized = true;
    }
}

// Преобразование UTF-32 в UTF-8
std::string u32_to_u8(const std::u32string& utf32) {
    std::string result;
    result.reserve(utf32.size() * 4);

    for (char32_t cp : utf32) {
        if (cp < 0x80) {
            result.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0x10FFFF) {
            result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            // Некорректный кодовая точка, пропускаем
            continue;
        }
    }

    return result;
}

// Преобразование UTF-8 в UTF-32
std::u32string u8_to_u32(const std::string& utf8) {
    std::u32string result;
    result.reserve(utf8.size());

    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        char32_t cp;
        size_t len;

        if ((c & 0x80) == 0) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            // Некорректный UTF-8, пропускаем байт
            ++i;
            continue;
        }

        // Проверка, что достаточно байт в строке
        if (i + len > utf8.size()) {
            break; // Обрезаем некорректную последовательность
        }

        bool valid = true;
        for (size_t j = 1; j < len; ++j) {
            unsigned char next = static_cast<unsigned char>(utf8[i + j]);
            if ((next & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (next & 0x3F);
        }

        if (valid) {
            // Проверка на минимальные и максимальные значения
            if ((len == 2 && cp < 0x80) ||
                (len == 3 && cp < 0x800) ||
                (len == 4 && cp < 0x10000) ||
                cp > 0x10FFFF) {
                // Некорректная последовательность (overlong или вне диапазона)
                i += len;
                continue;
            }
            result.push_back(cp);
        }
        i += len;
    }

    return result;
}

// Функции для преобразования регистра с использованием таблиц
char32_t toupper32(char32_t ch) noexcept {
    // Базовые латинские буквы
    if (ch >= 'a' && ch <= 'z') {
        return ch - 32;
    }

    // Кириллица
    if (ch >= 0x0430 && ch <= 0x044F) { // а-я
        return ch - 32;
    }
    if (ch == 0x0451) { // ё
        return 0x0401; // Ё
    }

    // Латинские буквы с диакритиками
    if (ch >= 0x00E0 && ch <= 0x00F6) {
        return ch - 32;
    }
    if (ch >= 0x00F8 && ch <= 0x00FE) {
        return ch - 32;
    }

    // Греческие буквы
    if (ch >= 0x03B1 && ch <= 0x03C9) { // α-ω
        return ch - 32;
    }

    return ch;
}

char32_t tolower32(char32_t ch) noexcept {
    // Базовые латинские буквы
    if (ch >= 'A' && ch <= 'Z') {
        return ch + 32;
    }

    // Кириллица
    if (ch >= 0x0410 && ch <= 0x042F) { // А-Я
        return ch + 32;
    }
    if (ch == 0x0401) { // Ё
        return 0x0451; // ё
    }

    // Латинские буквы с диакритиками
    if (ch >= 0x00C0 && ch <= 0x00D6) {
        return ch + 32;
    }
    if (ch >= 0x00D8 && ch <= 0x00DE) {
        return ch + 32;
    }

    // Греческие буквы
    if (ch >= 0x0391 && ch <= 0x03A9) { // Α-Ω
        return ch + 32;
    }

    return ch;
}

std::u32string toupper(const std::u32string& str) {
    std::u32string result;
    result.reserve(str.size());
    for (char32_t ch : str) {
        result.push_back(toupper32(ch));
    }
    return result;
}

std::u32string tolower(const std::u32string& str) {
    std::u32string result;
    result.reserve(str.size());
    for (char32_t ch : str) {
        result.push_back(tolower32(ch));
    }
    return result;
}

struct String {
    std::u32string data;

    // Конструкторы
    String() noexcept { set_console_utf8(); }

    String(const String& other) noexcept : data(other.data) { set_console_utf8(); }

    String(String&& other) noexcept : data(std::move(other.data)) { set_console_utf8(); }

    // разрешаем неявное преобразование
    String(const std::string& utf8) {
        set_console_utf8();
        data = u8_to_u32(utf8);
    }

    // разрешаем неявное преобразование
    String(const char* utf8) {
        set_console_utf8();
        if (utf8) {
            data = u8_to_u32(std::string(utf8));
        }
    }

    String(const int value) : String(std:: to_string(value)) {}

    // для явного преобразования из UTF-32
    explicit String(const char32_t* utf32) {
        set_console_utf8();
        if (utf32) {
            data = std::u32string(utf32);
        }
    }

    // для явного преобразования из UTF-32
    explicit String(const std::u32string& utf32) : data(utf32) {
        set_console_utf8();
    }

    // для явного преобразования из одного символа
    explicit String(char32_t ch) : data(1, ch) {
        set_console_utf8();
    }

    // Деструктор
    ~String() noexcept = default;

    static String RepeatFrom(char32_t ch, size_t count) {
        auto str = String();
        for (size_t i = 0; i < count; i++) {
            str += ch;
        }
        return str;
    }

    static String WithCapacity(size_t capacity, String str, bool align_left = true) noexcept {
        if (capacity <= str.size())
            return str;

        String result;

        if (align_left) {
            for (size_t i = 0; i < str.size(); i++) {
                result += str[i];
            }
            result += String::RepeatFrom(' ', capacity - str.size());
        } else {
            result += String::RepeatFrom(' ', capacity - str.size());
            for (size_t i = 0; i < str.size(); i++) {
                result += str[i];
            }
        }
        return result;
    }

    // Операторы присваивания
    String& operator=(const String& other) noexcept {
        if (this != &other) {
            data = other.data;
        }
        return *this;
    }

    String& operator=(String&& other) noexcept {
        if (this != &other) {
            data = std::move(other.data);
        }
        return *this;
    }

    String& operator=(const std::string& utf8) {
        data = u8_to_u32(utf8);
        return *this;
    }

    String& operator=(const char* utf8) {
        if (utf8) {
            data = u8_to_u32(std::string(utf8));
        } else {
            data.clear();
        }
        return *this;
    }

    String& operator=(const char32_t* utf32) {
        if (utf32) {
            data = std::u32string(utf32);
        } else {
            data.clear();
        }
        return *this;
    }



    inline operator std::string () {
        return to_utf8();
    }

    // Базовые методы
    size_t size() const noexcept { return data.size(); }
    bool empty() const noexcept { return data.empty(); }
    void clear() noexcept { data.clear(); }
    void reserve(size_t n) { data.reserve(n); }
    void shrink_to_fit() { data.shrink_to_fit(); }

    // Доступ к символам с проверкой
    char32_t at(size_t index) const {
        if (index >= data.size()) {
            throw std::out_of_range("String index out of range");
        }
        return data[index];
    }

    char32_t& at(size_t index) {
        if (index >= data.size()) {
            throw std::out_of_range("String index out of range");
        }
        return data[index];
    }

    char32_t get_char32(size_t index) const noexcept {
        return data[index];
    }

    std::string get_char8(size_t index) const {
        if (index >= data.size()) {
            return "";
        }
        std::u32string temp(1, data[index]);
        return u32_to_u8(temp);
    }

    // Оператор доступа по индексу (без проверки)
    char32_t operator[](size_t index) const noexcept {
        return data[index];
    }

    char32_t operator[](size_t index) noexcept {
        return data[index];
    }

    // Преобразование
    std::string to_utf8() const {
        return u32_to_u8(data);
    }

    const std::u32string& to_utf32() const noexcept {
        return data;
    }

    // Преобразование регистра
    String to_upper() const {
        return String(toupper(data));
    }

    String to_lower() const {
        return String(tolower(data));
    }

    // Конкатенация
    String operator+(const String& other) const {
        String result;
        result.data.reserve(data.size() + other.data.size());
        result.data = data;
        result.data += other.data;
        return result;
    }

    String& operator+=(const String& other) {
        data += other.data;
        return *this;
    }

    String& operator+=(char32_t ch) {
        data.push_back(ch);
        return *this;
    }

    String& operator+=(const char* utf8) {
        data += u8_to_u32(std::string(utf8));
        return *this;
    }

    String& operator+=(const std::string& utf8) {
        data += u8_to_u32(utf8);
        return *this;
    }

    String& operator+=(std::string& utf8) {
        data += u8_to_u32(utf8);
        return *this;
    }

    String& operator+=(std::string utf8) {
        data += u8_to_u32(utf8);
        return *this;
    }




    // Сравнение
    bool operator==(const String& other) const noexcept {
        return data == other.data;
    }

    bool operator!=(const String& other) const noexcept {
        return data != other.data;
    }

    bool operator<(const String& other) const noexcept {
        return data < other.data;
    }

    bool operator>(const String& other) const noexcept {
        return data > other.data;
    }

    bool operator<=(const String& other) const noexcept {
        return data <= other.data;
    }

    bool operator>=(const String& other) const noexcept {
        return data >= other.data;
    }

    // Поиск
    size_t find(const String& substr, size_t pos = 0) const noexcept {
        return data.find(substr.data, pos);
    }

    size_t find(char32_t ch, size_t pos = 0) const noexcept {
        return data.find(ch, pos);
    }

    size_t find(const char* utf8, size_t pos = 0) const {
        return data.find(u8_to_u32(std::string(utf8)), pos);
    }

    size_t rfind(const String& substr, size_t pos = std::u32string::npos) const noexcept {
        return data.rfind(substr.data, pos);
    }

    // Извлечение подстроки
    String substr(size_t pos, size_t count = std::u32string::npos) const {
        if (pos > data.size()) {
            throw std::out_of_range("String position out of range");
        }
        return String(data.substr(pos, count));
    }

    // Вставка и удаление
    String& insert(size_t pos, const String& str) {
        data.insert(pos, str.data);
        return *this;
    }

    String& erase(size_t pos, size_t count = std::u32string::npos) {
        data.erase(pos, count);
        return *this;
    }

    String& replace(size_t pos, size_t count, const String& str) {
        data.replace(pos, count, str.data);
        return *this;
    }

    // Итераторы
    auto begin() noexcept { return data.begin(); }
    auto end() noexcept { return data.end(); }
    auto begin() const noexcept { return data.begin(); }
    auto end() const noexcept { return data.end(); }
    auto cbegin() const noexcept { return data.cbegin(); }
    auto cend() const noexcept { return data.cend(); }

    // C-строка (только для отладки)
    const char32_t* c_str() const noexcept {
        return data.c_str();
    }

    // Количество вхождений символа char32_t
    int count(char32_t ch) const noexcept {
        int counter = 0;
        for (char32_t c : data) {
            if (c == ch) counter++;
        }
        return counter;
    }

    // Количество вхождений символа char (обычный ASCII)
    int count(char ch) const noexcept {
        char32_t target = static_cast<char32_t>(static_cast<unsigned char>(ch));
        int counter = 0;
        for (char32_t c : data) {
            if (c == target) counter++;
        }
        return counter;
    }

    // Количество вхождений подстроки из std::string
    int count(const std::string& ch) const noexcept {
        if (ch.empty()) return 0;
        std::u32string target = u8_to_u32(ch);
        if (target.empty()) return 0;
        return count(String(target));
    }

    // Количество вхождений подстроки из String
    int count(const String& ch) const noexcept {
        if (ch.empty()) return 0;

        // Если искомая строка - один символ, используем оптимизированный поиск
        if (ch.size() == 1) {
            return count(ch[0]);
        }

        // Поиск всех вхождений подстроки
        int counter = 0;
        size_t searchPos = 0;
        while (true) {
            size_t found = data.find(ch.data, searchPos);
            if (found == std::u32string::npos) break;
            counter++;
            searchPos = found + 1; // Разрешаем пересекающиеся вхождения
        }
        return counter;
    }

    // Оператор вывода
    friend std::ostream& operator<<(std::ostream& os, const String& str) {
        set_console_utf8();
        os << str.to_utf8();
        return os;
    }

    // Статические методы создания
    static String from_utf8(const std::string& utf8) {
        return String(utf8);
    }

    static String from_utf32(const std::u32string& utf32) {
        return String(utf32);
    }

    static String from_char(char32_t ch) {
        return String(ch);
    }
};

String operator+(const std::string& a, const String& b) {
    return String(a) + b;
}

std::vector<String> splitString(const String& str, char32_t delimiter) noexcept {
    std::vector<String> sub_strings;
    String current;
    for (auto c: str) {
        if (c == delimiter) {
            sub_strings.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    sub_strings.push_back(current);
    return sub_strings;
}

bool inString(const String& str, char32_t in) noexcept {
    for (auto s: str) {
        if (s == in) return true;
    }
    return false;
}

bool inString(const String& str, const String& in) noexcept {
    for (auto s: str) {
        if (String(s) == in) return true;
    }
    return false;
}

bool isSubString(const String& str, const String& sub_str, size_t index) noexcept {
    for (size_t i = index; i < index + sub_str.size(); i++)
        if (str[i] != sub_str[i - index]) return false;
    return true;
}

namespace std {
    template<>
    struct hash<String> {
        size_t operator()(const String& s) const noexcept {
            // Используем хэш от std::u32string или от UTF-8 строки
            return hash<std::u32string>{}(s.to_utf32());
        }
    };
}
