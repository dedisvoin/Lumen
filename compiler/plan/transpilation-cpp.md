# Транспиляция Lumen → C++

Целевой язык — **C++ (C++17, clang)**. Транспилятор — новая стадия после `DeepAnalizer`
(файл `compiler/CodeGenerator.cpp`, сегодня это заглушка).

Документ описывает, **как каждая конструкция Lumen переводится в C++** `<как будет>` —
это спецификация для реализации эмиттера, а не отчёт о сделанном.

---

## 1. Стратегия: «boxed» фаза 1

Ядро интерпретатора уже написано на C++ и уже реализует всю семантику языка
(`runtime/Values.cpp`, `runtime/Context.cpp`, `runtime/Functions.cpp`,
`core/TypeSystem.cpp`, `runtime/Strings.cpp`). Логичный первый шаг — **не писать
семантику заново, а эмитить C++, который вызывает те же хелперы**.

Два режима:

| Режим | Что эмитим | Плюсы | Минусы |
|---|---|---|---|
| **Boxed (фаза 1)** | переменные = `Value`, операции → вызовы хелперов рантайма, структуры → поля `Value` | точное совпадение с интерпретатором, 1:1 всю семантику уже написали | медленно, много накладных расходов |
| **Unboxed (фаза 2)** | вывод собственных типов, переменные = `int64_t`/`double`/`String`, прямые операторы | быстрый нативный код | нужен полноценный type-inferrer, дюндеры и динамика ломаются |

Фаза 1 должна давать **семантически эквивалентную** программу. Фаза 2 — оптимизация
позже, если понадобится.

### Общая схема
```
main.lm
  -> Lumenc    (lex+pre+parse)      AST  (66 node types, NodeBase.cpp)
  -> SemanticAnalizer  typeRefs     (M7, ещё не закончен: ASTDeepCheckerRealization.md)
  -> DeepAnalizer       проверка    execute(): та же семантика, что и у интерпретатора
  -> CodeGenerator:new               -> out.cpp  (НОВОЕ)
     -> clang++ out.cpp -o out.exe
```

### Формат выходного файла
Один самодостаточный `out.cpp`:
- `#include "..."` runtime-исходников Lumen прямо из исходников компилятора
  (в проекте принято подключать `.cpp` по `#pragma once`, см. `lumen-lsp.cpp`), **или**
  эмитится копия рантайма — решить при реализации. Рекомендация: include по абсолютному
  пути от `LUMEN_HOME` (переменная окружения уже используется в проекте), чтобы `out.cpp`
  не был гигантским и ссылался на актуальный рантайм.
- `int main()` → вызывает Lumen-функцию `main`, возвращает 0.

Не требуется, чтобы транспилированный код был «красивым C++» — требуется эквивалентность.

---

## 2. Маппинг типов

Таблица Lumen-тип → C++ (фаза 1, boxed):

| Lumen | C++ (boxed/фаза 1) | Примечание |
|---|---|---|
| `i8,i16,i32,i64` | `Value` с тегом целого (в рантайме все целые — 64-битные, `i8`..`i32` это те же целые с type tag) | `Ty::Int`, или tag-тип из `TypeSystem.cpp` |
| `f32,f64,fl,f` | `Value` с тегом float/double | |
| `str` | `Value` → `runtime/Strings.cpp::String` (`str` — нижний регистр!) | ключевое слово именно `str` |
| `char`,`Char` | `Value` с тегом char | |
| `bool` | `Value` → bool | правдивость — через `isTrue()`/харпyness рантайма |
| `void` | `void` / пустое `Value` | |
| `Array[T]`, `Array[T,N]` | `Value` → `RuntimeArray` (вектор `Value`) | count не обязан быть константным размером массива в фазу 1 |
| `Tuple[a,b,c]` | `Value` → vector `Value` фикс. длины | `Tuple[]` пустой — запрещён грамматикой |
| `Func[...]` | `Value` → callable (функция/`std::function`) | |
| `Type`, имена структур | `Value`-обёртка структуры + глобальная таблица типов | |
| enum | `enum class` ИЛИ `Value`-перечисления | в фазу 1: как в рантайме — int-значения с тегом |

Ключевой принцип: **в фазу 1 объявления не «сжимаются» до нативных типов**;
`Value` хранит тег реального типа, поэтому `typeof()` и проверки типов совпадают
с интерпретатором.

---

## 3. Прелюд выходной программы

Каждый `out.cpp` начинается с включений и разворачивания окружения:

```cpp
#include "runtime/Values.cpp"        // Value, фабрики, операторы
#include "runtime/Context.cpp"       // ContextObject, глобалы
#include "runtime/Functions.cpp"     // вызовы, операторы, formatValue
#include "core/TypeSystem.cpp"       // типы, регистрация struct/enum
#include "runtime/Strings.cpp"       // String

// корневой Scope, куда складываются глобальные имена — как в DeepAnalizer:
// RuntimeContext с активным контекстом (execN... активный scope).

int main() {
    vartree::... init;              // как DeepAnalizer::execute с "root" scope
    executeGlobalDeclarations();    // топ-уровень: decl'ы в порядке следования
    callUserMain();                 // fn main()
    return 0;
}
```

Всё глобальное (переменные, функции, структуры, enum, namespace) живёт в единой
имени-table `ContextObject` — ровно как в `DeepAnalizer` (контексты, где каждая
нода имеет `ctx`/`co`). Эмиттер повторяет эту структуру: каждой глобальной сущности —
слот в `ContextObject`.

---

## 4. Принципы эмиссии выражений

- Каждое **выражение** превращается в C++-выражение, возвращающее `Value`.
- Каждый **statement** — в C++-выражение со `static_cast<void>` (рантайм так и делает:
  все конструкции EX/ST — единый `execute()`).
- Пути присваивания (`writePathValue`, DeepAnalizer.cpp:862) для вложенных левых частей:
  `a`, `a.i`, `a[i]`, `a.b[i].c`, `NS::x` — эмитится как один вызов
  `set_path(root, {idx1, idx2, ...}, newValue)` в boxed-режиме. Это зеркалит
  `setIndexRec`/`writePathValue`, чтобы не дублировать логику.
- **Дюндеры** — те же имена рантайма: `__get_index__`, `__set_index__`, `__del__`,
  `__typeof__`, `__sizeof__`. Если у типа есть метод-дюндер — эмитим вызов, иначе
  встроенную ветку (см. п.7, п.9).

---

## 5. Азбука: литералы и переменные

### NodeNumber
`42`, `3.14`, шестнадцатеричные (если есть в лексере).
```cpp
Value(42.0)            // целое
Value(3.14)            // double
```
Ровно те же конструкции, что делает парсер чисел в `parsePrimaryExpression`.

### NodeString
```cpp
Value(RuntimeString("..."))   // String из runtime/Strings.cpp
```
Экранирование: переносим содержимое строкового литерала в C++-литерал как есть
(двойные кавычки внутри — экранировать). Одна и та же `String`-структура в фазу 1
(не `std::string`!), чтобы `.length`, конкатенация и операторы совпадали.

### NodeBool
```cpp
Value(true) / Value(false)
```

### NodeNull
```cpp
Value(Ty::Null, nullptr)     // как в рантайме
```

### NodeVariable
```cpp
ctx.lookup("name")           // из активного ContextObject
```
Без типа — просто поиск в рантайм-таблице. `Scope-resolution` см. п.6.

### NodeVariableDeclaration  — `let x;` / `let x: T = expr;` / `let x = expr;`
```cpp
ctx.define("x", expr, /*type hint*/);
```
Модификаторы (`const`/`final`/`strict`) → проверки в рантайм-`define` (уже есть в
`ContextObject`), эмиттим флагом.

---

## 6. Скоупы, модификаторы, namespace

### NodeScope / NodeStatementsScope
Тело в `{ }` → отдельный C++ scope вокруг одноимённого блока, чтобы сохранить
видимость/затенение:
```cpp
{ /* каждый дочерний statement */ }
```
Поведение совпадает с интерпретатором (новый контекст входит/выходит).

### NodeModifier  — `strict`, `const`, `final`, `static`, `private`, `public`, `(...)`
Модификатор — обёртка над узлом. Эмиссия: развернуть модификаторы в параметры рантайма
(как `active_mods` в `DeepAnalizer`), сам узел эмитится его case'ом.
- `const` → запрет повторного `writePathValue` (проверка в рантайме).
- `final` → запрет повторной декларации (в т.ч. имя struct/enum — см. п.9).
- `static` → член привязан к типу, не к экземпляру (кладём в тип-таблицу).
- `strict`/`private`/`public` → в фазу 1: не влияют на кодоген (только разрешение имён),
  но сохраняем атрибуты в именах, чтобы `NS::x` по-прежнему работал.

### NodeNamespace  — `namespace Name { ... }`
```cpp
ContextObject nameScope = makeNamespace("Name");   // вложенный объект в root
/* все statements тела выполняются ВНУТРИ nameScope, как ctx в DeepAnalizer */
```
Доступ снаружи — через `NodeBinaryScopeResolution` (`Name :: member`), см. ниже.

### NodeBinaryScopeResolution  — `A :: B`
```cpp
resolve_scope(ctx_child, "A")  ->  "B"
```
Эмитим как поиск по цепочке контекстов (базируется на
`DeepAnalizer.cpp:476-491`: разбор цепочки имён до переменной/типа).

---

## 7. Структуры, enum-ы и литералы

### NodeStructDeclaration — `struct Point {}` / `struct Vec[i64, N] {}`
Фаза 1, boxed:
```cpp
// структура = объект с полями (map name->Value) + таблица методов
struct RuntimeObject { RuntimeContext fields; RuntimeContext methods; };
registerStruct("Point", /*глобальная таблица типов*/);
```
Методы — обычные Lumen-функции, зарегистрированные под `Point.*` (см. п.8):
эмиттим C++-функции `Value fn_Point_area(Value self, Args...)`, которые кладутся
в метод-таблицу структуры. Поиск метода `obj.m()` → `methods.lookup("m")` → вызов.

- Имя структуры **конкурирует** с переменными в root (shadowing ±: experiment).
  Для `final struct Name {}` — регистрируем `Name` как `final`-имя, поэтому
  `let Name = 10;` даёт `FINAL_REDECLARED` (74) — **как уже сделано в
  DeepAnalizer.cpp (~1214): `co0.modifiers.Final = active_mods.Final`**.
- Генерик-параметры (`Vec[i64, 8]`) в фазу 1: генерик-аргументы не реифицируются —
  все имена типов **не** парметризуются, рантайм хранит `generics` в метаданных
  структуры (для `typeof`), компиляторные проверки совпадают с DeepAnalizer
  (поиск по таблице типов с args).

### NodeStructLiteral — `Point { x: 1, y: 2 }` / `Vec[i64, 8] { … }` / шорткат `{ x }`
Имя типа и список generics разбираются в `l_node`/`generics`, поля — `StructFieldInit{name, value}`.
```cpp
Value obj = structLit("Point", { {"x", exprX}, {"y", exprY} });
```
- шорткат `{ x }` (без `: value`): `value` уже превращён парсером в `NodeVariable(x)` —
  эмитим как `ctx.lookup("x")`.
- базой может быть выражение (`(*p){…}`, `create(){…}`, `Geo::Point{…}`) —
  эмитим в том же порядке, что и `parseStructLiteralBody` (ASTGenerator.cpp:319).

### NodeEnumDeclaration — `enum Color { Red, Green, Blue }`
Как в рантайме: каждый вариант — имя, связанное с целым `Value`-индексом.
```cpp
enumEnum("Color");
defineEnumVariant("Color", "Red", Value(0)) ... // индексы по порядку
```
Типы `Color::Red` (ScopeResolution где левая часть — enum) → константное `Value`.
Ошибки реифицируются: `ENUM_REDECLARED` (121), `ENUM_VARIANT_NOT_FOUND` (122).

---

## 8. Функции и вызовы

### NodeFunctionDeclaration — `fn f[T](a: T, b: str = "x") -> ret : { body }`
```cpp
Value sigArgs = pack(params, defaults, modifiers);   // FuncArg: name, modifiers, type, default_value
registerFunction("f", sigArgs, [](Args args) -> Value {
    /* тело, с bind параметров в функцию-контекст */
});
```
- **Overload**: Lumen позволяет overload по имени (несколько `fn f`). В фазу 1
  эмитим несколько регистраций с одной семантикой рантайма: последняя декларация
  «побеждает» или диспатч по числу аргументов — **взять из существующего
  рантайма** (`Functions.cpp` уже диспатчит по signature). Эмиттер просто дублирует
  регистрации.
- **default args** (`= expr`): передаются как есть, рантайм подставляет default — совпадает.
- **Генерики** `fn f[T]`: в boxed не нужна мономорфизация — `T` это `Value`.

### NodeFunctionCall — `f(...)`, `obj.m(...)`, `N :: m(...)`, `arr[idx](...)`
```cpp
call_callable(resolvedCallee, { args... })
```
- `obj.m(...)` — `NodeMemberAccess` обёрнут вызовом: сначала разрешаем
  `obj.methods["m"]`, затем вызываем с `obj` как `self` (как
  `executeFunctionCall`, DeepAnalizer.cpp:3131).
- вызов по индексу `arr[idx](...)` и `(*p)(...)` — тоже каллабл-значение.

### NodeMemberAccess — `a.b`
```cpp
get_member(a, "b")
```

### NodeFunctionType — `Func[...]`
Используется как аннотация типа; в фазу 1 это метаданные-аннотация, runtime-значение
не создаётся (проверка типов остаётся анализерской).

### NodeLambda — `(i: i32) -> i32 { expr }`
```cpp
Value lambda = makeLambda(std::vector<FuncArg> params, std::vector<Value> env,
                          [](Args args) -> Value { return /* тело = одно выражение */; });
```
**Открытый вопрос №1**: `NodeLambda` **не обработан** в текущем execute-свиче
DeepAnalizer — транспилятор должен принять решение отдельно: (а) сначала
«вручную» поддержать лямбду и в интерпретаторе (closure = capture-контекст),
(б) временно выдавать ошибку «lambda not supported yet» на этапе транспиляции.
Рекомендация: (а) — лямбда важна для `==`-сравнения в `Dict` и передачи колбэков.

---

## 9. Операторы

Все бинарные узлы — 18 штук (NodeBase.cpp:31-48) + `NodeBinaryAssignment` (+ `::`).
Эмиссия — вызовы операторов рантайма (они уже реализованы в `Functions.cpp`
и `binaryMath` в DeepAnalizer, включая ошибки типа деление на ноль, строковые + и т.д.).

| Lumen | Нода | C++ (boxed) |
|---|---|---|
| `a * b` | NodeBinaryMultiplicative | `rt_mul(a, b)` |
| `a / b` | NodeBinaryDivision | `rt_div(a, b)` |
| `a % b` | NodeBinaryRemainder | `rt_rem(a, b)` |
| `a + b` | NodeBinaryAdditive | `rt_add(a, b)` (числа и **конкатенация строк**, как в рантайме) |
| `a - b` | NodeBinarySubtraction | `rt_sub(a, b)` |
| `a << b` | NodeBinaryShiftLeft | `rt_shl(a, b)` |
| `a >> b` | NodeBinaryShiftRight | `rt_shr(a, b)` |
| `a == b` | NodeBinaryEqual | `rt_eq(a, b)` |
| `a != b` | NodeBinaryNotEqual | `rt_ne(a, b)` |
| `a < b` | NodeBinaryLess | `rt_lt(a, b)` |
| `a > b` | NodeBinaryGreater | `rt_gt(a, b)` |
| `a <= b` | NodeBinaryLessEqual | `rt_le(a, b)` |
| `a >= b` | NodeBinaryGreaterEqual | `rt_ge(a, b)` |
| `a & b` | NodeBinaryBitwiseAnd | `rt_bitand(a, b)` |
| `a ^ b` | NodeBinaryBitwiseXor | `rt_bitxor(a, b)` |
| `a \| b` | NodeBinaryBitwiseOr | `rt_bitor(a, b)` |
| `a && b` | NodeBinaryLogicalAnd | `rt_and(a, b)` — **НЕ short-circuit** (семантика языка: оба операнда вычисляются!) |
| `a \|\| b` | NodeBinaryLogicalOr | `rt_or(a, b)` — **НЕ short-circuit** |

Приоритеты — те же, что в синтаксическом анализаторе (ASTGenerator.cpp:540-780):
`* / %` → `+ -` → `<< >>` → `== !=` → `<= >=` → `< >` → `&` → `^` → `|` → `&&` → `||`.
Эмитится с скобками по AST, приоритет не пересчитывается.

### Унарные (NodeBase.cpp:24-30) и постфикс
| Синтаксис | Нода | C++ |
|---|---|---|
| `-a` | NodeUnaryMinus | `rt_neg(a)` |
| `+a` | NodeUnaryPlus | `rt_pos(a)` |
| `!a` | NodeUnaryNegation | `rt_not(a)` |
| `*a` | NodeUnaryDereference | `rt_deref(a)` |
| `~a` | NodeUnaryBitwiseNot | `rt_bitnot(a)` |
| `&a` | NodeUnaryAddressOf | `rt_addr(a)` |
| `a++` | NodeUnaryPostfixIncrement | `rt_postfix_inc(a)` — десугаринг как в accSemПроверке (DeepAnalizer.cpp:1881): `(a = a + 1)` читается, увеличивается, возвращает старое значение |
| `a--` | NodeUnaryPostfixDecrement | `rt_postfix_dec(a)` |

### NodeCast — `a as Type`
```cpp
rt_cast(a, "Type")
```
(парсер: ASTGenerator.cpp:519). В фазу 1 — по `Value` перестраиваем тег/тип;
ошибки несовместимости — как в DeepAnalizer.

### NodeBinaryAssignment — `=` и составные `+= -= *= /= %= &= |= ^= <<= >>=`
`a = rhs`:
```cpp
set_path(a, { /*индексы*/ }, rhs);     // для простой Var/Chain одинаково
```
Составные (`+=` и т.д.) **уже де-сахарены парсером** в
`NodeBinaryAssignment(lhs, NodeBinary* (lhs op rhs))` — см. `makeCompoundBinop`
(ASTGenerator.cpp:1549-1561). Эмиттер просто получает бинарный узел в `RHS`,
ничего не пересобирает — `a += b` → `set_path(a, a + b)`.

Ошибки же такие же, как в DeepAnalizer: `const`/`final`-перезапись — runtime-проверки.

---

## 10. Массивы, индексация, delete

### NodeArray — `{ e1, e2, ... }`
```cpp
Value(Array(elements...))    // RuntimeArray
```
`Array[]` пустой — грамматика запрещает (проверка в парсере типов).

### NodeTupleType — `[a, b]`
Тип-запись/кортеж (для `Tuple[]` вида). В фазу 1 — vector `Value` фикс. длины.

### NodeArrayType — `Array[T]`, `Array[T, N]`
Тип-аннотация; count `N` может быть целой константой — в фазу 1 не влияет на
раскладку (см. TypeSystem).

### NodeArrayAccess — `base[idx1][idx2]...`
```cpp
get_index(base, {idx1, idx2, ...})     // flatten, как в NodeBinaryAssignment LHS
```
Индексы — `std::vector<Node*>` — эмитим мульти-индекс одним вызовом.

### NodeDelete — `del host;` / `del host, index;`
```cpp
// host — массив/кортеж:
rt_delete_elem(host, index)            // erase из RuntimeArray
// host — структура с дюндером __del__:
callDunderWithWriteback("__del__", host, {index})   // см. п.7/п.11
// иначе: DELETE_INVALID_TYPE (123); без index: DELETE_WITHOUT_INDEX (124)
```
`callDunderWithWriteback` (DeepAnalizer.cpp:~2990) — уже готовая функция:
прячет self в `$<dname>_self_`, вызывает метод, пишет мутированную структуру обратно
в реальный слот (включая вложенные `.a.b` пути). Эмиттер вызывает её же.

---

## 11. Дюндеры (`__del__`, `__typeof__`, `__sizeof__`, `__get_index__`, `__set_index__`, `__assign__`)

Дюндеры — методы структуры, вызываемые **неявной** семантикой. Автодополнение
(27 шт.) даёт: `__get_index__`, `__set_index__`, `__assign__`, `__del__`, `__typeof__`, `__sizeof__`,
`__as__` (совпадает с диспатчем DeepAnalizer) + операторные (`__add__`, `__eq__`, …
— они диспатчатся через `tryDunder`, см. §19.2).

| Дюндер | Где | C++ |
|---|---|---|
| `__get_index__` | `arr[i]`, `obj[i]` | если структура имеет метод → `callDunder(..., "self_and_arg_i")`, иначе встроенный `Array` |
| `__set_index__` | `a[i] = v` | через `callDunderWithWriteback` (self по значению, write-back) |
| `__assign__` | `x = v`, `x.f = v`, `*p = v`, `Ns::x = v`, где адресат — struct с `__assign__` | через `rt::assign_dispatch` / `putf_dispatch` / `setptr_dispatch` / `setref_dispatch` (self по значению; мутации полей идут в общую `sm`-карту) |
| `__del__` | `del obj, i` | через `callDunderWithWriteback` |
| `__typeof__` | `typeof(x)` | возвращает строку-имя типа: `Value("Int")` — по таблице тегов `Value` |
| `__sizeof__` | `sizeof(x)` | `Value((int64) встроенный размер)` / перегрузка дюндера без аргумента |
| `__as__` | `expr as T` | `expr.__as__(reified T)` — аргумент `new_type(T)`, результат уходит как значение `as` |

Хранение мутаций: везде, где self может быть изменён методом, эмитируем
**write-back** (после вызова кладём изменённый self обратно в LHS-слот), иначе
изменения потеряются — это уже исправлено в рантайме (`callDunderWithWriteback`),
эмиттер повторяет тот же паттерн.

`__assign__` вызывает перегрузку **оператора присваивания**: `x = v` при условии,
что *текущее* значение `x` — struct, у которого есть метод `__assign__` (сигнатура
`fn __assign__(self, value: T) -> Null`). Первое присваивание (когда слот ещё `Null`
или другого типа) идёт обычной записью; диспетчер не срабатывает. В анализаторе это
ветки `NodeBinaryAssignment` + `writePathValue` (`tryAssignDunder`); при внутренней
пропагации мутированной копии наружу диспетчер глушится (`assign_dunder_disabled`),
иначе `self.field = ...` внутри самого `__assign__` снова вызвал бы `__assign__`
для `self`. Индексация `a[i] = v` по-прежнему идёт через `__set_index__` (если
владелец — struct). **Элемент массива** при замене диспатчится по тому же правилу
«текущего значения»: `x[0] = v` для `Array[A]` (элементы — struct с `__assign__`)
вызывает `__assign__` элемента, иначе — замена; это зеркально в рантайме
(`setindex_dispatch`) и в анализе (проверка `elementAt` перед `setIndexRec` в
`NodeArrayAccess`-путях `writePathValue`/`writePathValue`-фластена и в write-back
`callDunderWithWriteback`). Запись в поле элемента (`s.items[i].f = v`) пишет поле
`f` plainly — диспатч определяется типом *поля*, а не элемента.

**Рекурсивный guard `__assign__`.** Пока выполняется тело `__assign__`, новые
диспетчи `__assign__` подавляются: `self = value` внутри тела означает *почленное
вливание* `value` в карту self (`struct_import`: идентичность `sm`-карты цели
сохраняется — диспетч-вызов без write-back видит правки через общую карту) и
почленную перезапись, а не повторный самовызов. Без guard это бесконечная
рекурсия (stack overflow 0xC00000FD). Зеркально в анализаторе (`dunder_assign_active`
в `tryAssignDunder`/`callDunderWithWriteback`): внутри тела `__assign__` записи идут
plain, поэтому warning error[96] (recursion limit) для `self = value` не появляется.
Согласованность: анализ заменяет значение цели копией `value`, рантайм вливает
поля в существующую карту — наблюдаемые поля совпадают.

**Перегрузки дюндеров.** В одной структуре может быть несколько методов с одним
dunder-именем (например `__assign__(self, i64)` и `__assign__(self, A)`). Каждый
метод манглится с пер-структурным суффиксом (`mf_A___assign__`, `mf_A___assign___1`, …)
и регистрируется в `g_dunders` отдельной записью вместе со списком тегов типов
параметров *после* `self` (`rt::reg_dunder(ty, name, {tags...}, &fn)`). Рантайм
`dunder_select` выбирает перегрузку по фактическим типам аргументов (точное
совпадение тега — 4 очка, generic/unknown — wildcard); если подходящей нет —
fallback на встроенную семантику. Тег «generic» ставится и для типовых параметров
структуры (`Ref[T].__assign__(self, T)` → `T` = wildcard).

**ref-параметры.** Кодген оборачивает в `rref(refcell_ptr(...))` **только имя
переменной**; анализатор поэтому требует, чтобы аргументом `ref`-параметра было имя
переменной (не выражение/элемент/поле): не-адресуемый аргумент — `REF_RVALUE`
(200), адресуемый, но не переменная (`p.x`, `arr[i]`, `*ptr`, вызов-`-> ref`) —
`REF_ARG_NOT_VARIABLE` (129). Валидация выполняется ДО исполнения тела; невалидному
аргументу отводится невинный скрэтч-слот, чтобы тело не добавило вторичный
`NULL_DEREFERENCE`. Write-back мутировавшей копии параметра в lvalue-путь аргумента
по-прежнему выполняется после тела.

**Висячие указатели на локаль функции (P2).** Указатель из `&local` или из
`ref`-параметра, привязанного к локальной переменной, целится в ячейку `Ctx`
(фрейма) функции; на `return` этот `Ctx` умирает, и указатель повисает (UB: запись
в мёртвый узел карты может не фолтить и молча теряться). Рантайм не рефакторился на
owning-ячейки (сломал бы рабочие ref-фичи) — заведена статическая проверка в
анализаторе: `current_fn_scope` бумпается вокруг `execute(fn.body, call_ctx)`; адрес
каждой `let`-локали помечается владельцем-скоупом (`local_owner[declared.addr]`);
обход дерева возвращаемого значения (`escapedLocalPtr` — значения, поля struct,
элементы array/tuple) на `NodeReturn` и guard-проверка в `writePathValue` (NodeVariable)
ловят указатель, чей владелец == текущий скоуп, в значение, покидающее функцию —
диагностика `POINTER_ESCAPES_SCOPE` (131). Глобальные/внешние destination-слоты и
указатели на top-level переменные не триггерят (owner-меток у них нет).

**`&` (адрес в цель-указатель) поддерживает только переменную, ref-параметр и
`ref let`-алиас** (`rt::refptr(env.get(nm))` / `rt::refptr(*nm.sp)`). `&obj.field`
и `&Ns::member` — `ADDRESS_OF_MEMBER_UNSUPPORTED` (130), у анализатора это явная
диагностика, у рантайма — ячейка поля не является самостоятельным Box.

---

## 12. Управление потоком

### NodeIf — `if (c) { ... } else { ... }`
```cpp
if (is_true(c)) { /* then */ } else { /* else */ }
```
`if` — **выражение** (хвостовая `;` не обязательна, значение ветки попадает в
`.value` объявления следующим statement-ом) — в фазу 1 для определений эмитим
«последнее выражение ветки как return из блока», как у интерпретатора
(execute() возвращает Value последней ноды).

### NodeWhile — `while (c) { ... }`
```cpp
while (is_true(c)) { ... }
```

### NodeFor — `for (init; check; update) { ... }`
```cpp
{ init; while (is_true(check)) { body; update; } }
```
(структура NodeFor — уже классический Си-стиль: init/check/update/body).

### NodeBreak / NodeContinue / NodeReturn
```cpp
break; continue; return Value;   // return в значении функции
```
Вынос значения return из вложенных веток — через `Flag`/исключение в рантайме;
в фазу 1 эмитим как есть (рантайм-`execute` вернёт значение), return из функции
синтаксически как `return expr;`.

---

## 13. Новые/память

### NodeNew — `new T;` / `new T(args)` / `new;`
Язык имеет указатели (`&`, `*`) и `new`. В boxed-модели память «у гепа»: Value копируется
по значению; указатели (`&a`) в рантайме — хелперы для разыменования.
```cpp
Value newObj = makeNew({ NewModifier(name, type_arg)... }, valueOrNull);
```
`NewModifier` — аттрибуты `new` (`new ...;` с именем типа или пустой `new;`).
Нативный heap/C++ new/delete в фазу 1 не нужен — всё уже Value/умные обёртки.

---

## 14. Вывод

### NodeOut / NodeOutln — `out x, y;` / `out;`
Печать через функцию-форматтер рантайма:
```cpp
print(formatValue(expr1)); print(formatValue(expr2)); // out — без переводa
printLn(...);                                          // outln — перевод строки
```
Аргументы разбираются только через запятую. Форма `out(x, y)` **не поддерживается**:
скобки после `out`/`outln` принадлежат выражению, иначе не парсится, например,
`outln (i32 | char) << i32;` (`<<` — проверка подтипа через `Type::operator<<`).
`out;` — без аргументов → просто печать перевода/ничего.

### NodeEcho — `echo expr;`
**Debug-инструмент анализатора** (inline hint'ы в редакторе). Решение принято:
эмитится как `outln` — `rt::println(expr)` (просто и ожидаемо, совпадает с поведением
LSP-hint в дебаге). Узел `NodeEcho` в AST сохраняется ради inlay-хинтов.

### node `typeof(x)` / `sizeof(x)` в выражениях
`typeof`: анализатор кладёт статически вычисленную строку типа в `NodeTypeOf`
(поле `static_type`), эмиттер отдаёт её литералом `rt::Box::str(...)`; при
неоднозначности (execution одного узла с разными типами) — рантайм
`rt::typeof_dispatch`. `sizeof`: compile-const — эмитим `Value(N)` напрямую, без
рантайм-вызова; но приоритет также дать дюндеру, если он есть (совпадает с DeepAnalizer).

---

## 15. Вся грамматика = таблица эмиссии

Полный перечень `NodeTypes` (NodeBase.cpp:3-71) и «куда» эмитится:

| # | NodeTypes | C++ (кратко) |
|---|---|---|
| 0 | NodeNumber | `Value(num)` |
| 1 | NodeString | `Value(String("..."))` |
| 2 | NodeBool | `Value(bool)` |
| 3 | NodeScope | своя область видимости (блок), рантайм-контекст |
| 4 | NodeStatementsScope | список statements |
| 5 | NodeModifier | развернуть модификаторы, эмитить узел |
| 6 | NodeNamespace | вложенный ContextObject |
| 7 | NodeIf | `if(is_true)` |
| 8 | NodeWhile | `while(is_true)` |
| 9 | NodeFor | `{init; while} ` |
| 10 | NodeBreak | `break` |
| 11 | NodeContinue | `continue` |
| 12 | NodeReturn | `return` |
| 13 | NodeOut | `print(...)` |
| 14 | NodeOutln | `println(...)` |
| 15 | NodeEcho | `println(...)` (как `outln`) |
| 16 | NodeDelete | `rt_delete_elem` / дюндер |
| 17 | NodeNull | `Value(Ty::Null)` |
| 18 | NodeVariable | `ctx.lookup(name)` |
| 19-24 | унарные (-\+!\*~&) | `rt_neg/pos/not/deref/bitnot/addr` |
| 25-42 | бинарные (18 шт.) | `rt_*` по таблице п.9 |
| 43 | NodeBinaryScopeResolution | `resolve_scope` цепочкой |
| 44 | NodeBinaryAssignment | `set_path` |
| 45 | NodeCast | `rt_cast(a, "T")` |
| 46 | NodeMemberAccess | `get_member(a, "b")` |
| 47 | NodeFunctionCall | `call_callable(f, args)` |
| 48 | NodeFunctionType | аннотация-метаданные |
| 49 | NodeTupleType | аннотация/кортеж |
| 50 | NodeArrayType | аннотация |
| 51 | NodeArray | `Value(RuntimeArray(els))` |
| 52 | NodeArrayAccess | `get_index(a, {i...})` |
| 53 | NodeLambda | `std::function` (**вопрос №1**) |
| 54 | NodeNew | `makeNew(...)` |
| 55 | NodeTypeOf | статический `static_type`/дюндер/`typeof_dispatch` |
| 56 | NodeSizeOf | дюндер/compile-const |
| 57 | NodeVariableDeclaration | `define` |
| 58 | NodeFunctionDeclaration | `registerFunction` |
| 59 | NodeStructDeclaration | `registerStruct` (+ g-таблица) |
| 60 | NodeEnumDeclaration | `enumEnum` |
| 61 | NodeStructLiteral | `structLit` |
| 62 | NodeUnaryPostfixIncrement | `rt_postfix_inc` |
| 63 | NodeUnaryPostfixDecrement | `rt_postfix_dec` |

---

## 16. Пример: полная транспиляция

Вход `demo.lm`:
```
struct Probe {
    hits: i32,
}
fn Probe.bump() : {
    self.hits = self.hits + 1;
}
fn main() : {
    let p = Probe { hits: 0 };
    p.bump();
    let arr = {10, 20, 30, 40};
    del arr, 1;
    outln(typeof(p), sizeof(arr), arr[2]);
}
```

Ожидаемый `out.cpp` (схема, упрощена):
```cpp
#include "runtime/Values.cpp"   // ... и т.д.

int main() {
    initRuntime();
    registerStruct("Probe", /*fields: hits*/);
    registerFunction("Probe.bump", 0, [](Args a)->Value {
        Value self = selfOf(a);
        set_member_path(self, {"hits"}, rt_add(get_index(self,{"hits"}), Value(1)));
        writeback(self);      // self по значению → пишем обратно (как callDunderWithWriteback)
        return Value(Ty::Null, nullptr);
    });
    registerFunction("main", 0, [](Args a)->Value {
        Value p = structLit("Probe", {{"hits", Value(0)}});
        call_callable(get_member(p, "bump"), {p});
        Value arr = Value(RuntimeArray({Value(10),Value(20),Value(30),Value(40)}));
        rt_delete_elem(arr, Value(1));
        print(formatValue(dunder_typeof(p))); print(" ");
        print(formatValue(dunder_sizeof(arr))); print(" ");
        println(formatValue(get_index(arr, {Value(2)})));
        return Value(Ty::Null, nullptr);
    });
    run_main("main");
    return 0;
}
```

---

## 17. Команды сборки

Транспиляция из lumenc:
```
lumenc demo.lm --cpp -o demo.cpp          # эмиссия C++ (флаг --cpp; -o значит файл КОДА, не экзешник)
clang++ demo.cpp -std=c++17 -o demo.exe  # нативная сборка
```
(В текущем lumenc `-o` не используется; привязать к эмиттеру. `--emit`/`--cpp` — новое.)

---

## 18. План работ (M1–M5)

- **M1** — каркас CodeGenerator: обход AST, `--cpp` в lumenc, шаблон `out.cpp` с прелюдом
  и `main()`, литералы, переменные, арифметика. Валидация: транспилировать probe-программы.
- **M2** — управление потоком (if/while/for/break/continue/return), объявления переменных,
  statement-scopes, mодификаторы.
- **M3** — функции (регистрация, вызов, overload, default args, генерики как Value),
  структуры (struct/StructLiteral/поля/self/методы), enum, namespace, `::`.
- **M4** — операторы (все, составные присваивания), массивы/индексы/delete, cast,
  postfix ++/--, `&`/`*`/`new`.
- **M5** — дюндеры (все 5 неявных), typeof/sizeof, лямбды (см. вопрос №1), echo/out/outln,
  **консолидированная регрессия**: каждый probe-скрипт транслируется и запускается,
  вывод сравнивается с интерпретатором (погрунт-тесты, как `def_probe.py` и др.).

Оценка объёма: **1500–2500 строк** эмиттера (включая шаблон прелюдии).

---

## 19. Открытые вопросы перед реализацией

1. **NodeLambda** — не поддержан в интерпретаторе: сначала реализовать лямбды в
   рантайме (closure = captured context), либо временно запретить на транспиляции.
2. **Операторные дюндеры** (`__add__`, `__eq__`, `__bool__`, `__neg__`, `__invert__`,
   `__as__` и т.д.) — **диспатчатся** в анализаторе через `tryDunder`
   (DeepAnalizer.cpp:~2955): бинарные/унарные операторы и `as` сначала пробуют
   метод структуры. Неподдерживаемыми остаются только операторные дюндеры для
   присваиваний (`a op= b`) — они не вызывают перегрузку, а просто де-сахарятся
   парсером в `a = (a op b)` (и здесь перегрузка работает через бинарный оператор).
3. **echo** — решено: эмитится как `outln` (`rt::println(expr)`).
4. **Unboxed-фаза 2** — целевая ли она вообще? Решение принять после M5 (профилирование).
5. **std/Pair.lm** — баг имён полей `first_`/`second_` (vs `first`/`second`) — удалить
   или переименовать до M4, иначе транспиляция std не заведётся.

---

## 20. Ресурсы

- `compiler/CodeGenerator.cpp` — место реализации (заглушка сегодня).
- `lumenc.cpp` — драйвер `--cpp` / `-o` wiring.
- `compiler/DeepAnalizer.cpp` — единственный источник семантики (execute(), 2700+ строк):
  все ветки в switch по NodeTypes — это и есть «что делает каждая нода».
- `runtime/Values.cpp`, `runtime/Functions.cpp`, `runtime/Context.cpp`,
  `core/TypeSystem.cpp`, `runtime/Strings.cpp` — хелперы для прелюдии.
- `compiler/core/NodeBase.cpp` — полный список NodeTypes (66).
- `compiler/ASTGenerator.cpp` — синтаксис (parse*).
- `syntax/*.lm` — грамматика в виде примеров (operators/declarations/preprocessor/etc).