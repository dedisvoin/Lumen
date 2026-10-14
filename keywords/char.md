# char
---

Один символ Unicode. Записывается в одинарных кавычках. Тип `char`.

Поддерживает арифметику через приведение `as i64` (код символа):
```lumen
let c: char = 'A';
let code = c as i64;       // 65
echo code + 1 as char;     // 'B'
```
Сравнение символов и доступ к элементу строки:
```lumen
let s = "hello";
let first = s[0];          // 'h'
echo first == 'h';         // true
```

Пустой код 0 и пробел:
```lumen
echo '\0' as i64;          // 0
echo ' ' as i64;           // 32
```

Литерал в одинарных кавычках обязан содержать ровно один символ. Если внутри `''`
не один символ (`''`, `'ab'`), выдаётся предупреждение `warning[206]`, а литерал
трактуется как строка (`str`):
```lumen
let a = 'ab';       // warning[206]: char literal length: 2 -> type str
let b = '';         // warning[206]: char literal length: 0 -> type str
let c = 'a';        // type char
```
