# str
---

Строка Unicode (UTF-8). Поддерживает:

- конкатенацию через `+`;
- доступ к символу по индексу `s[i]` → `char` (присваивание `s[i] = x` запрещено — `STRING_INDEX_IMMUTABLE`);
- сравнение `==`, `!=`.
```lumen
let s: str = "hello";
let c = s[0];       // 'h'
let t = s + "!";
```
