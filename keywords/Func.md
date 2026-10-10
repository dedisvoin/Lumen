# Func
---

Тип функции-значения (callable). Функция может быть сохранена в переменную и вызвана.
```lumen
fn add(a: i32, b: i32) -> i32 : { ret a + b; }
let f: Func = add;
echo f(1, 2);
```
