# mod
---

Возвращает `true`, если у объявления, на которое указывает выражение, установлен
заданный **модификатор**. Свойство компиляционного времени; проверяются:
`static`, `final`, `const`, `strict`, `private`, `ref`, `public`
(`public` — это отсутствие `private`).

```lumen
struct S {
    strict let a: i64 = 1;
    let b: i64 = 2;
    final let c: i64 = 3;
    static let d: i64 = 4;
}

let s = S{};
echo mod(s.a, strict);   // true
echo mod(s.a, final);    // false
echo mod(s.c, final);    // true
echo mod(s.d, static);   // true
echo mod(s.a, public);   // true
echo mod(s.a, private);  // false
```

Работает и для переменных, и для полей generic-структур:
```lumen
fn f() -> i64 : {
    final let x = 5;
    echo mod(x, final);  // true
    ret x;
}

struct[T] Box {
    strict let _inner: ?T = null;
    fn is_strict(ref self) -> bool : ret mod(self._inner, strict);
}
```

Результат — compile-time константа, поэтому годится в условиях:
```lumen
if (mod(state, final)) { echo "нельзя менять"; }
```

Второй аргумент должен быть одним из перечисленных модификаторов; иначе —
синтаксическая ошибка.

См. также `typeof`, `typeis`.
