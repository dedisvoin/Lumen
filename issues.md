# Известные проблемы Lumen

Срез на 2026-10-23. Каждый пункт воспроизведён в обоих режимах:

- `-r` — интерпретатор;
- native — `lumenc -f <file> -build -o <exe> -g <tmpdir>` + запуск exe.

Все пробники лежат в `%TEMP%\opencode\probes` и в репозиторий не входят.
Легенда: `[ ]` — открыто, `[x]` — исправлено.

---

## P0 — критично

### [x] P0-1. Замыкания не компилируются в native

Исправлено в `compiler/CodeGenerator.cpp` (`emitNestedFn`): захват строится через
`rt::scope_snapshot(env, {})`, снимок владеет копиями кадров цепочки (root `g_ctx`
заимствуется), а `ctx` инициализируется конструктором `Ctx(std::shared_ptr<Ctx>)`.
Проверено: `sum(10)(20)` → `30` в обоих движках; замыкания, переживающие выход из
объемлющей функции (`let add5 = makeAdder(5); echo add5(1);` → `6`), и вложенные
два уровня (`outer(100)(20)(3)` → `123`) работают в native. Регрессия: 72/0.

```lumen
fn sum(a: i64) -> Func[](i64) -> i64 : {
    fn inner(b: i64) -> i64 : { ret a + b; }
    ret inner;
}
echo sum(10)(20);
```

- `-r`: `30`
- native: `30` (до фикса — ошибка сборки `no viable overloaded '='`)

Исходная причина: `Ctx::parent` объявлен как `shared_ptr<Ctx>`, а кодогенератор
присваивал сырой указатель.
Файлы: `compiler/CodeGenerator.cpp` (генерация лямбд), `compiler/runtime/lumen_rt.h` (`Ctx`, `scope_snapshot`).

### [ ] P0-1b. Транзитивный захват (2+ уровня) в интерпретаторе

```lumen
fn outer(a: i64) -> Func[](i64) -> Func[](i64) -> i64 : {
    fn mid(b: i64) -> Func[](i64) -> i64 : {
        fn inner(c: i64) -> i64 : { ret a + b + c; }
        ret inner;
    }
    ret mid;
}
let m = outer(100)(20);
echo m(3);
```

- `-r`: `error[73] Undefined variable 'a'` (плюс `error[72]`/`error[98]`), `m(3)` не вычисляется
- native: `123` (после фикса P0-1)

Причина: интерпретатор хранит `Function.captured` как копию текущего `Context` с
сырым `parent` на кадр объявления; после выхода из объёмлющей функции указатель
висит. Нужен владеющий снимок цепочки (аналогично `scope_snapshot`) в
`Function.captured` / `DeepAnalizer.cpp` (объявление вложенной функции, ~3211-3219).

### [ ] P0-2. native молча печатает `null` вместо диагностики

```lumen
struct INNER { static fn op() -> i64 : ret 1; }
fn make() -> auto : ret INNER;
let p = make();
echo typeof(p);
echo p::op();
```

- `-r`: `Type` + `error[166] Left side of '::' is not a namespace (found type 'Type')`
- native: `Null` / `null`, exit 0

Общая проблема: ошибки, найденные только анализатором/интерпретатором,
в native превращаются в `null`. Нужен единый проход валидации до кодогена
(или проброс ошибок в рантайм с ненулевым exit-кодом).

### [ ] P0-3. Выход за границы массива и деление на ноль

```lumen
let a = {1, 2, 3};
echo a[10];
```

- `-r`: `error[158] Array index 10 is out of range (size 3)`
- native: `null`, exit 0

```lumen
echo 1 / 0;
```

- `-r`: `error[70] Division by zero`
- native: падение, `exit=-1073741676` (0xC0000094, SIGFPE)

Нужны проверки в рантайме (bounds-check и guard на ноль) с внятным
сообщением и управляемым exit-кодом.

### [ ] P0-4. `ref` на поле структуры молча копирует

```lumen
struct S { ref let x: i64; }
let a = 5;
let s = S{x: a};
s.x = 9;
echo a;
echo s.x;
```

- `-r`: `5` / `9`
- native: `5` / `9`

Ожидается либо алиас (`9` / `9`), либо диагностика `REF_BAD_TARGET` /
`MODIFIER_MISPLACED`. Сейчас нет ни того, ни другого (Gap A).
См. `compiler/plan/references-plan.md:115-117`.

### [ ] P0-5. Нет write-back через `-> ref Array`

```lumen
struct S { let a: Array[i64] = {1, 2}; }
fn get(ref self: S) -> ref Array[i64] : ret self.a;
let s = S{};
get(s)[0] = 9;
echo s.a[0];
```

- `-r`: `error[163] Cannot assign to 'expr'[]: the target is not a variable`
- native: `1` — присваивание молча потеряно

Нужно поддержать `ref`-возврат массива как lvalue в обоих движках.

### [ ] P0-6. `ref let` от rvalue (возврат по значению)

```lumen
fn val() -> i64 : ret 5;
ref let r = val();
r = 7;
echo r;
```

- `-r`: `error[163]` на `r = 7` (неверная позиция и сообщение), затем `5`
- native: `7` (запись ушла во временный объект)

Ожидается `REF_RVALUE` уже на объявлении. Проверки для других форм уже есть:
`ref let r = a + b;` → `error[186]`, `inc(5)` → `error[190]`.

---

## P1 — заметно

### [ ] P1-7. `out` / `outln` в интерпретаторе

```lumen
outln 1;
out 2;
echo 3;
```

- `-r`: `3` (обе строки потеряны)
- native: `1` + `23`

Также `outln(1, 2);` — синтаксическая ошибка: `error[152]`, `error[38]`, `error[149]`.
Нужно выровнять поведение (`out`/`outln` — без перевода строки/с переводом)
и добавить поддержку нескольких аргументов либо явно запретить её в документации.

### [ ] P1-8. Присваивание статическому методу — no-op

```lumen
struct X { static fn op() -> i64 : ret 999; }
fn sum(a: i64, b: i64) -> i64 : ret a + b;
X::op = sum;
echo X::op(10, 20);
```

- `-r`: `error[93] No matching overload of 'op' for arguments (i64, i64)`
- native: `null`

Если сигнатуры совпадают:

```lumen
struct X { static fn op(a: i64, b: i64) -> i64 : ret 999; }
fn sum(a: i64, b: i64) -> i64 : ret a + b;
X::op = sum;
echo X::op(10, 20);
```

оба движка печатают `999`, то есть `X::op = sum;` игнорируется. Ожидается
либо реальная замена, либо диагностика «нельзя переприсваивать статический метод».

### [ ] P1-9. Вызов `Func`-поля как метода

```lumen
struct S { let op: Func[](i64, i64) -> i64; }
fn sum(a: i64, b: i64) -> i64 : ret a + b;
let s = S{op: sum};
echo s.op(10, 20);
```

- `-r`: `error[112] Struct 'S' has no method 'op'`
- native: `null`

Обходной путь работает в обоих движках:

```lumen
let g = s.op;
echo g(10, 20); // 30
```

### [ ] P1-10. Namespace-литерал как вызываемый объект

```lumen
fn MyStruct(f: Func[](i64, i64) -> i64) -> Namespace : {
    ret namespace { let op = f; };
}
fn sum(a: i64, b: i64) -> i64 : ret a + b;
let p = MyStruct(sum);
eval echo typeof(p);
echo p::op(10, 20);
```

- `-r`: `Namespace` / `30`
- native: `namespace` / `null`

### [ ] P1-11. `del` по полю-массиву

```lumen
struct S { let a: Array[i64] = {1, 2, 3}; }
let s = S{};
del s.a[1];
echo s.a;
```

- `-r`: `error[124] 'del / delete' requires an index for type 'i64'`, затем `{1, 2, 3}`
- native: `{1, 2, 3}` — молча без эффекта

`del arr[i]` для локальной переменной работает. Нужно поддержать путь через поле.

### [ ] P1-12. `std/Dict.lm` и `std/String.lm` сломаны

```lumen
#include "std/Dict.lm"
let d = Dict[str, i64]{};
d["a"] = 1;
echo d["a"];
```

- `-r`: `error[112] Struct 'Pair' has no method 'create'` (`std/Dict.lm:11`) →
  `error[76]` → `call error in '__set_index__'` →
  `Assertion failed: 'Dict not containe this key.'`
- native: `assertion failed (Dict not containe this key.)`, crash `exit=-1073740791`

Причины: `Dict` вызывает `Pair::create`/`.first`/`.second`, которых в текущем
`Pair` нет; плюс опечатка `get_index`.

```lumen
#include "std/String.lm"
let s = String("abc");
echo sizeof(s);
echo s as str;
```

- `-r`: `error[109] Struct 'String' has no field '_size'` (`std/String.lm:36`) →
  `call error in '__sizeof__'`, затем `abc`
- native: `null` / `abc`

Нужно `self._str._size` вместо `self._size`.

---

## P2 — хвосты

### [ ] P2-13. 13 заглушек `TBD` в кодогенераторе

`compiler/CodeGenerator.cpp`:

| Строка | Что не поддержано |
| --- | --- |
| 617 | call |
| 854 | cast target |
| 938 | scope-res expr |
| 1117 | expr fallback |
| 1248 | for-step |
| 1460 | scope-res lhs |
| 1470 | lhs |
| 1591 | call statement |
| 1629 | append lhs |
| 1637 | reserve lhs |
| 1652, 1662 | del host (erase / write-back) |
| 1710 | statement |

Воспроизводимые no-op:

```lumen
append {1, 2}, 3;      // -r: error[179]   native: 0, без эффекта
reserve {1, 2}, 5;     // -r: error[138]   native: 0
fn f() -> i64 : ret 1;
f() = 5;               // -r: error[163]   native: exit 0
Nope::x = 5;           // -r: error[73]    native: exit 0
del s.a[1];            // -r: error[124]   native: без эффекта (см. P1-11)
```

`cast`, `for-step`, `scope-res expr` достижимы реже — закрывать вместе с тестами.

### [ ] P2-14. `typeof` в native

```lumen
fn sum(a: i64, b: i64) -> i64 : ret a + b;
eval echo typeof(sum);
let ns = namespace {};
eval echo typeof(ns);
```

- `-r`: `(Function[](i64, i64) -> i64)` / `Namespace`
- native: `fn` / `namespace`

Для `Type`: `Type` vs `Null` (см. P0-2). Для имени структуры (`S`) совпадает.

### [ ] P2-15. `keywords/Func.md` устарел

```lumen
let f: Func = add;
```

`error[38] Expected '[', but found '='`
Рабочая форма: `let f: Func[](i32, i32) -> i32 = add;` — проверено, `30` в обоих.

### [ ] P2-16. `Type::operator==` без гарда (внутренний риск)

`compiler/core/TypeSystem.cpp:214-215`:

```cpp
inline bool operator==(const Type& _T) const {
    if (isAny() || _T.isAny()) return true;
```

`any` считается равным любому типу. Внешнего repro сейчас нет
(`typeis(v: any) == str` → `false` в обоих движках), но гард
`!is_any && !is_auto` нужен, иначе новые проверки через `==` будут ложно-положительными.

### [ ] P2-17. `Numeric`-индексация в native без `std/Typing.lm`

```lumen
struct W {
    let b: Tuple[i64, i64] = {1, 2};
    fn __get_index__(ref self, index: Numeric) -> i64 : {
        ret self.b[index];
    }
}
let w = W{};
echo w[1];
```

- `-r`: `2`
- native: `null`

С `#include "std/Typing.lm"` оба движка работают корректно. Надёжнее объявлять индекс `i64`.

### [ ] P2-18. Мусор и незакоммиченные изменения

- untracked: `foreach_test.cpp`, `foreach_test.exe` в корне.
- изменён `editor/sessions/session.json` (шум сессии).
- в `std/String.lm` висит whitespace-only диф (удалены пустые строки в конце).
- `compiler/core/Nodes/NodeForEach.cpp` и `keywords/of.md` уже в git (не untracked).

---

## Полезные ссылки

- `compiler/CodeGenerator.cpp` — кодогенерация, TBD-заглушки, ref-логика.
- `compiler/runtime/lumen_rt.h` — `Box`/`T_REF`/`T_REFI`, `refi_cell`, `refpath`,
  `refindex_cell`, `refindex_path`, `setref_dispatch`.
- `compiler/plan/references-plan.md` — правила `ref`.
- `compiler/plan/transpilation-cpp.md` — §14 `out`, §19 `NodeLambda`.
- `compiler/core/TypeSystem.cpp:214` — `Type::operator==`.
- `std/Dict.lm`, `std/String.lm`, `std/Pair.lm`, `keywords/Func.md`.
