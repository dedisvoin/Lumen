# typeis
---

Возвращает **статический (объявленный) тип** выражения как значение типа `Type`.
Симметрично `typeof`, но не зависит от текущего значения: `typeof` смотрит на
рантайм-тип, а `typeis` — на объявленный тип.

```lumen
let a: i64|f64 = 5;
echo typeof(a);            // i64        (рантайм-тип значения)
echo typeis(a);           // i64 | f64  (объявленный тип)

let b = 2.5;
echo typeis(b);           // f64        (аннотации нет — выведенный тип)
```

Для поля структуры берётся объявленный тип поля с учётом генериков инстанса:
```lumen
struct[T] Box {
    strict let _inner: ?T = null;
    fn inner_type(ref self) -> Type : ret typeis(self._inner);
}
let bi = Box[i64|f64](5);
echo bi.inner_type();     // i64 | f64 | Null
```

Результат — compile-time константа, поэтому годится для сравнений:
```lumen
if (typeis(a) == (i64|f64)) { echo "объявлен как i64|f64"; }
```

См. также `typeof`, `is`.
