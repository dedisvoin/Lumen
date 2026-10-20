# Type
---

Значение, описывающее тип. Возвращается из `typeof`/`typeis`. Сравнимо с обычными типами.
```lumen
let t = typeof(x);   // Type, например для i64
if (t == i64) { ... }
```

## Составные типы

Тип-выражения можно комбинировать:

- **union**: `A | B` — значение одного из перечисленных типов;
- **nullable**: `?T` — синтаксический сахар над union `T | Null`.

`?T` — не отдельная сущность, а именно union с `Null`. Это видно через `typeis`:

```lumen
let a: ?i64 = null;
echo typeis(a);                   // i64 | Null
echo typeis(a) == (i64 | Null);   // true

let b: ?str = null;
echo typeis(b);                   // str | Null
```

Эквивалентность работает и в обе стороны — `?i64` и `i64 | Null` неотличимы:

```lumen
let x: ?i64 = 5;
let y: i64 | Null = 5;
echo typeis(x) == typeis(y);      // true
```
