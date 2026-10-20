# Null
---

Тип нулевого значения. Соответствует `null`.
```lumen
let p: Null = null;
```

Nullable-тип `?T` — это синтаксический сахар над union `T | Null`:
```lumen
let a: ?i64 = null;      // то же, что i64 | Null
let b: i64 | Null = null;
echo typeis(a) == typeis(b);   // true
```

`!T` тоже **не** создаёт nullable — префиксный `!` к типу не применим
(`error[159]`); используйте `?T`. См. `Type`, `typeis`.
