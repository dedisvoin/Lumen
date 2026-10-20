# Транспиляция семантики `all_semantic.lm` → C++

> Эталон семантики — **файл `all_semantic.lm`** (корень репозитория): каждый раздел —
> это рабочий код + комментарий с ожидаемым выводом. Транспилятор считается готовым,
> когда весь файл транслируется в `out.cpp`, собирается и при запуске выдаёт ровно те
> значения, что указаны в комментариях.
>
> Документ — уточнённая под **текущий синтаксис и полный набор конструкций** версия
> `compiler/plan/transpilation-cpp.md` (база: boxed-модель, эмиссия как отражение
> `execute()` из `DeepAnalizer`). Здесь добавлено всё, чего не было в старом плане:
> `ref`, nullable `?T` / union `A | B`, tuple `!{…}`, указатели/`new`/`ptr`, операторные
> дюндеры, актуальный синтаксис struct-полей (`strict let x: T;`), enum, дженерики.

---

## 0. Что транспилируем и как проверяем

`all_semantic.lm` разбит на разделы; каждый даёт **критерий приёмки**:

| Раздел файла | Критерий приёмки |
|---|---|
| декларации переменных | исполняемый вывод соответствует `// комментариям` |
| пространства имён | `std::a`, анонимный namespace |
| функции | defaults, void, generic, рекурсия, Func, lambda-присваивание |
| условия / циклы | if-выражения, while/for/break/continue |
| массивы / кортежи / строки | `+=`, `del host, idx`, `!{…}`, `s[i]` |
| указатели и память | `new`, `*`, `&`, `ptr()`, const-ячейка |
| ссылки `ref` | алиас, ref-параметр, `-> ref` + запись в результат |
| структуры | поля, `self`, static/const fn, дженерики, литерал, `new S {}` |
| enum | `Color::Red` → 0 … |
| перегрузка операторов | `__add__`, `__eq__`, `__sizeof__` |
| типы | `typeof`, `sizeof`, `as`, Type-алиасы, `?T`, `A \| B` |
| препроцессор | сущности `#…` до AST не доходят (см. §4.14) |
| видимость | `private` в namespace, `final` |

Сверка: каждый запуск транспилированной программы сравнивается с комментариями раздела
(автотест: запуск `out.exe` + diff по эталонным строкам вывода).

---

## 1. Общая стратегия: boxed фаза 1

Семантика языка **уже реализована один раз** в рантайме `C++`
(`compiler/runtime/Value*.cpp`, `Functions.cpp`, `Context.cpp`, `TypeSystem.cpp`,
`Strings.cpp`) и в `DeepAnalizer::execute()` (единственный источник «что делает нода»).
Первая фаза транспилятора **не пишет семантику заново**, а эмитит C++, который
вызывает те же хелперы.

- переменные — `Value` (тег реального типа сохраняется ⇒ `typeof`, `is`, union-Null строго совпадают с интерпретатором);
- каждое выражение → C++-выражение, возвращающее `Value`;
- каждый statement → C++-выражение `(void)(...)` со скобками по AST (приоритет не пересчитывается);
- вложенные LHS (`a`, `a.i`, `a[i]`, `a.b[i].c`, `NS::x`) → единый `set_path(root, path, v)` / чтение `get_path` — зеркало `writePathValue`/`setIndexRec`;
- дюндеры — те же имена и тот же порядок диспатча, что `tryDunder`.

Пайплайн (маяк — `lumenc.cpp`):

```
all_semantic.lm
  -> Lumenc (lex + preprocessor)         # #define/#macro уже раскрыты здесь
  -> ASTGenerator                        # 66 типов нод (NodeBase.cpp)
  -> SemanticAnalizer + DeepAnalizer     # проверки, execute()
  -> CodeGenerator:new                   # НОВОЕ (сейчас заглушка)
  -> out.cpp -> clang++ -std=c++17 -> out.exe
```

---

## 2. Маппинг типов (полный)

| Lumen | C++ boxed | Примечание |
|---|---|---|
| `i8`…`i64` | `Value` (целочисленный) | i8..i32 — те же 64-битные целые с тегом типа |
| `f16`/`f32`/`f64` | `Value` (float/double) | |
| `str` | `Value` → `RuntimeString` (`Strings.cpp`) | ключ. слово — `str`, не string |
| `char` | `Value` (char-тег) | арифметика и `as i64` — как в рантайме |
| `bool` | `Value` (bool) | правдивость — `isTrue()` |
| `null` | `Value(Ty::Null, nullptr)` | |
| `Array[T]` | `Value` → `RuntimeArray` (vector<Value>) | |
| `Tuple[...]` | `Value` → `RuntimeArray` фикс. длины | литерал `!{a,b}` = унарный `!` от массива (см. §4.6) |
| `Func`/`Lambda` | `Value` callable | лямбда-значение — открытый вопрос №1 |
| `Type` | `Value` (Ty::Type_, reified `Type`) | `strict let X: Type = i8 \| i16` |
| `?T` (nullable) | `Value` (union-тип включает `Null`) | runtime-локаль в `Type`, поведение уже в рантайме |
| `A \| B` (union) | `Value` (union-тип `A\|B`) | конструкции те же |
| `*T` (указатель) | `Value` (Pointer/`VirtualHeap`) | см. §4.7 |
| struct | `Value` → `RuntimeStruct` + реестр типов | generics — метаданные для `typeof` |
| enum | `Value` (индекс-константа) + реестр имён | |

---

## 3. Прелюдия `out.cpp`

Один самодостаточный файл. **Решение M0 (отклонение от ранней версии плана):**
вместо `#include` исходников компилятора в `out.cpp` эмиттер печатает **встроенный
boxed-рантайм** (`namespace rt`: `Box` с тегом типа + `bin/neg/fmt/print/println/Ctx`)
— несколько сотен строк, без зависимостей от компилятора. Поведение хелперов
**зеркалирует** `DeepAnalizer` (`binaryMath`, `resultTypeOf`, `negationNumber`,
`formatValue`) 1:1. Это даёт мгновенный цикл валидации (`g++ out.cpp; out.exe`) без
unity-цепочки всего компилятора.

---

## 4. Разделы → эмиссия

### 4.1 Декларации переменных и модификаторы

`let x = e;` / `strict let x: T = e;` / `const let x = e;` / `final strict let x: Type = …`
(ноды: `NodeModifier` цепочка + `NodeVariableDeclaration`).

```cpp
// модификаторы разворачиваются в аттрибуты рантайма (как active_mods в DeepAnalizer):
ctx.define("x", e, Mods{ .Strict=true, .Const=true, .Final=false });
```

- `strict`/`const`/`final` → флаги `ContextObject::Modifiers` (запреты повторного
  assign/redeclare уже в рантайм-`define`).
- `strict let x: T` — тип в `NodeVariableDeclaration.type_node`; в boxed только
  метаданные (не сжимаем до нативного типа).
- `global` → в boxed не меняет раскладку (все глобальные и так в корневом скоупе).
- `public` → флаг видимости; рантайм-проверок нет (проверил анализер).

### 4.2 Пространства имён

`namespace std { … }` / анонимный `let x = namespace { … }` (`NodeNamespace`).

```cpp
ContextObject ns = makeNamespace("std");   // вложенный объект в root
/* statements тела исполняются ВНУТРИ ns */
```
Внешний доступ `std::a`, `my_str::testificate` — `resolve_scope(root,"std")->"a"`, как
`NodeBinaryScopeResolution` в анализе (цепочка имён до переменной/типа).

### 4.3 Функции

`fn add(a: i64, b: i64) -> i64 : { … }` (`NodeFunctionDeclaration`).

```cpp
registerFunction("add", pack(params), [](Args a)->Value { /* тело */ });
```

| Фича (`all_semantic.lm`) | Эмиссия |
|---|---|
| параметры по умолчанию `greet(name: str = "world")` | `FuncArg.default_value` — в сигнатуре, подстановка в рантайме (совпадает) |
| void-функция `log_if(show: bool)` (без `->`) | возвращает `Value(Ty::Null)` |
| generic `fn ident[T: Type](v: T) -> T` | мономорфизация **не нужна** — `T` это `Value` |
| рекурсия `fact` | обычная функция, self-reference в теле |
| const fn — только метод struct | см. §4.10 |
| overload (несколько `fn f`) | регистрируем все сигнатуры; диспатч по сигнатуре — из `Functions.cpp` |
| функция как значение `let fadd = add; echo fadd(1,2);` | `Value`-callable: имя функции кладётся как значение; вызов `call_callable` |
| `lambda(v)->T : { v*2 }` | присваивание эмитим, но вызов лямбды — **ошибка** до реализации лямбда-значений в рантайме (открытый вопрос №1) |

> В `all_semantic.lm` лямбда **только присваивается** (`let double = lambda…`), т.к.
> вызов сохранённой лямбды в интерпретаторе даёт `Null`. Эмиттер не обязан её похоронить,
> но и не может «оживить» — ставим ошибку `LAMBDA_CALL` до поддержки closure.

### 4.4 Условия

`NodeIf` — и statement, и **выражение** (значение ветки — последний statement ветки):

```cpp
if (is_true(c)) { /* then → .value */ } else { /* else → .value */ }
```

Логические `&&`, `||`, `!` — `rt_and/or/not` (НЕ short-circuit — язык так и делает:
оба операнда вычисляются, это **поведение** из `DeepAnalizer`, сохраняемое в boxed).

### 4.5 Циклы

`NodeWhile`, `NodeFor`, `NodeBreak`, `NodeContinue`:

```cpp
while (is_true(c)) { … }
{ init; while (is_true(check)) { body; update; } }   // for (…;…;;) — уже Си-структура в AST
```

`break`/`continue` — нативные `break;`/`continue;` (+ `Value`-возврат цикла = Null).

### 4.6 Массивы, строки, кортежи

| Конструкция | Эмиссия |
|---|---|
| `{10,20,30}` (`NodeArray`) | `Value(RuntimeArray({…}))` |
| `arr += {40}` | де-сахар парсера → `arr = arr + {40}` → `set_path(arr, rt_add(...))`; `+` массив+массив конкатенирует (рантайм) |
| `del nums, 2` (`NodeDelete`) | `rt_delete_elem(host, index)`; `del host` без индекса → `DELETE_WITHOUT_INDEX` |
| `s[i]`, `s + "!"`, `sizeof(s)` | `get_index` / `rt_add` (строковая конкатенация) / `dunder_sizeof` |
| `!{1,"a",true}` (tuple-литерал) | **унарный `!` от массива** → рантайм превращает `Array`-значение в `Tuple` (ветка `NodeUnaryNegation`, `value.T.isArray()`) |
| доступ `tp[1]`, `sizeof(tp)` | индексация/размер как у массива (RuntimeArray фикс. длины) |
| `echo typeof(tp)` | `formatValue(typeof)` → `[i64, char, bool]` (статически из анализатора) |

### 4.7 Указатели и память

| Конструкция | Эмиссия |
|---|---|
| `new 7` (`NodeNew`) | `makeNew({}, Value(7))` → указатель на heap-ячейку (`VirtualHeap`) |
| `new <strict(i32)> 5` | `makeNew({ NewModifier("strict", type_i32) }, Value(5))` |
| `new <const> 3.14` | `makeNew({ NewModifier("const") }, …)`; запись `*p=…` → `CONST_REASSIGNED` |
| `*p` разыменование | `rt_deref(p)` (read) / через `set_path` (write) |
| `&x` (`NodeUnaryAddressOf`) | `rt_addr(x)` → Pointer |
| `ptr(0x1000, i64)` (`NodePtr`) | `rt_ptr(addr, "i64")` |
| `null`, `empty_ptr = null` | `Value(Ty::Null)` |

Разыменование нулевого указателя → `NULL_DEREFERENCE` (рантайм). Объекты в `new`-ячейках
живут в `VirtualHeap` и освобождаются вместе с контекстом (`del` на указателях **не
вызывается** — `del` работает только с массивами/кортежами, что подтверждено
`DELETE_…` ошибками).

### 4.8 Ссылки `ref` (ключевой новый раздел)

Три формы (семантика из `references-plan.md` и `all_semantic.lm`):

**а) `ref let alias = target;` — живой алиас**
```cpp
// эмитится «маршрут»: root (Value*) + путь из индексов, как у LHS writePathValue
RPath alias = capture_path(target);        // {root=&target, path={}}
// чтение/запись через alias:
alias_read(alias)  == get_path(alias)
alias = v          == set_path(alias, v)   // пишет В ЦЕЛЬ, mirror DeepAnalizer
```

**б) `fn bump(ref x: i64) { x = x+1; }` — ref-параметр**
Вместо копии аргументу приписывается `RPath` **вызывающего**; тело читает/пишет по
пути напрямую (никакой копии, никакого write-back не нужно — это уже настоящая ссылка):
```cpp
void fn_bump(RPath x) { x = rt_add(alias_read(x), Value(1)); }   // set_path(х)
// вызов: fn_bump(capture_path(counter))
```
Требование **lvalue-аргумента** (REF_RVALUE) — проверка на этапе анализа, до эмиссии.

**в) `fn slot_get() -> ref i64 { ret slot; }` — ref-возврат**
Функция возвращает `RPath` на глобал/поле/ячейку:
```cpp
RPath fn_slot_get() { return capture_path(slot); }
// запись в результат: slot_get() = 99  ==  set_path(fn_slot_get(), Value(99))
// ref let от ref-возврата: RPath slot_alias = fn_slot_get();
```
Не-поддерживаемые цели (`REF_BAD_TARGET`) продолжают отклоняться анализом до кода.

> Один общий примитив на все три формы: `RPath { Value* root; std::vector<int> path; }`
> + `get_path`/`set_path`, **уже существующие** как `writePathValue`/`setIndexRec`.
> Эмиттер только раскладывает ноды на «взять путь аргумента» и «записать по пути».

### 4.9 Структуры

Актуальный синтаксис поля — `[моды] let имя : тип = default;` внутри тела (`NodeVariableDeclaration`
в struct-контексте), методы — `NodeFunctionDeclaration` с параметром `self`.

```cpp
registerStruct("Point", {
    fields:  { "x": Mods{.Strict=true}, "y": ... },
    methods: { "translate": fn_Point_translate, "is_origin": fn_Point_is_origin },
    statics: { "origin":  fn_Point_origin },          // через ::
});
```

| Конструкция | Эмиссия |
|---|---|
| `Point { x: 3.0, y: 4.0 }` (`NodeStructLiteral`) | `structLit("Point", {{"x",e},{"y",e}})`; дефолты полей из метаданных |
| `pt.translate(1.0, 1.0)` (метод, self-write-back) | `methods["translate"]`; передаём `self` **примитивом пути** → мутации пишутся в вызывающий слот (тот же `RPath`, что и `ref`) |
| `Point::origin()` (static) | статик-таблица, `self` нет |
| `const fn is_origin(self)` | const-ресивер: запись в поля внутри → `CONST_FN_MUTATION` (анализ) |
| `struct[FirstT, SecondT] Duo { … }` (дженерик) | структура регистрируется один раз; generic-аргументы — метаданные типа (для `typeof`/проверок), **мономорфизация не нужна** |
| `static fn of[A,B]` | обычная функция в статик-таблице; возвращает `structLit("Duo", …, generics={A,B})` |
| `new Duo[i64, str] {…}` | `makeNew` + struct-литерал; доступ `pduo.second` → auto-deref `get_member` |
| приватные поля `private let …` | проверка на уровне анализа; в рантайме writable, но наружу анализ не пустит |

`self`-параметр эмитится как `RPath` (см. 4.8, тот же примитив) — это **существующий**
write-back механизм интерпретатора, перенесённый 1:1.

### 4.10 Enum

`enum Color { Red, Green, Blue }` (`NodeEnumDeclaration`) — как в рантайме:
```cpp
enumEnum("Color");
defineEnumVariant("Color", "Red", Value(0));        // индексы по порядку перечисления
```
`Color::Red` (scope-resolution, где слева enum) → константное `Value`.
Ошибки (`ENUM_REDECLARED`, `ENUM_VARIANT_NOT_FOUND`) — анализ.

### 4.11 Перегрузка операторов (dunder-методы)

Операторные дюндеры диспатчатся в анализаторе, и эмиттер зеркалит **тот же порядок**
`tryDunder` (Grep: DeepAnalizer.cpp):
`__neg__`, `__pos__` (унарные) · `__add__`, `__sub__`, `__mul__`, `__truediv__`,
`__mod__` (арифметика) · `__invert__`, `__and__`, `__or__`, `__xor__`, `__lshift__`,
`__rshift__` (битовые) · `__eq__`, `__ne__`, `__lt__`, `__gt__`, `__le__`, `__ge__`
(сравнения) · `__bool__` · `__get_index__` · `__set_index__` · `__assign__` ·
`__typeof__` · `__sizeof__` · `__del__` · `__as__`.

```cpp
// a op b → (если есть __op__ у lhs) callDunder("__op__", a, {b}, writeback)
//           иначе встроенная ветка рантайма
rt_addWithDunder(a, b);   // хелперы уже в Functions.cpp / callDunderWithWriteback
```

`__assign__` — перегрузка **оператора присваивания**: `x = v` (а также `x.f = v`,
`*p = v`, `Ns::x = v`) вызывает `__assign__`, если *текущее* значение адресата —
struct с этим методом; иначе — обычная запись. Первое присваивание (слот `Null` или
другого типа) не диспатчится. Анализатор: `tryAssignDunder` в ветках
`NodeBinaryAssignment` и `writePathValue`; эмиттер: `rt::assign_dispatch` /
`putf_dispatch` / `setptr_dispatch` / `setref_dispatch`. Индексация `a[i] = v` — по-прежнему
`__set_index__`.

Составные `a op= b` де-сахарены парсером в `a = (a op b)` — перегрузка работает через
бинарный оператор (никакой отдельной эмиссии).

### 4.12 `typeof` / `sizeof` / `as`

- `typeof(x)` (`NodeTypeOf`) → дюндер `__typeof__`, иначе статически вычисленное
  анализатором имя типа; `DeepAnalizer` кладёт строку `formatValue(typeof)` в узел
  (`static_type`), и эмиттер отдаёт её литералом. Если узел исполнялся с разными типами
  (generic-функция) — fallback на рантайм `rt::typeof_dispatch` (имя по тегу `Value`);
- `sizeof(x)` (`NodeSizeOf`) → дюндер `__sizeof__`, иначе встроенный размер; для
  строк/массивов/кортежей — длина (как `sizeof("hello")` → 5);
- `x as T` (`NodeCast`) → дюндер `__as__` (аргумент — `new_type(T)`), иначе перестройка
  тега/типа по `Value`; ошибки несовместимости — как в `DeepAnalizer`.

### 4.13 `?T`, `A | B`, Type-значения

В boxed-модели — это **типы `Value`**, отдельной эмиссии не требуется:
`let maybe_num: ?i64 = null;` → `ctx.define("maybe_num", null_value, Type(i64|null))`,
присваивание `42` проходит через рантайм-проверку соответствия union-типу (строгому).
`final strict let Whole: Type = i8 | i16 | i32 | i64;` → `define` с типом
`Ty::Type_` и значением `new_type(union)`. `typeof` такой переменной → `Type`.

### 4.14 Препроцессор

`#define`, `#macro`, `#include`, `#ifdef/#ifndef/#undef`, `@(…)`, `$$` раскрываются в
`Preprocessor` **до AST** → транспилятор их не видит. Эмиттеру нужен **лишь учёт**
того, что в `all_semantic.lm` макросы порождают обычные ноды (`let ncount = 10;`
после `var_pair(count,10)`), поэтому таблица в §8 не обязана содержать `#`-директив.

### 4.15 Вывод и `assert`

- `out`/`outln` — печать `formatValue(expr)` без/с переводом строки;
- `echo expr` — транспилируется как `rt::println(expr)`, т.е. идентично `outln`
  (см. `CodeGenerator.cpp`, case `NodeEcho`). Узел `NodeEcho` в AST сохраняется, поэтому
  анализатор по-прежнему строит inlay-хинты в редакторе;
- `assert(cond, msg)` (`NodeAssert`) → `rt_assert(cond, msg)` (при ложном условии —
  диагностика/abort, как `judgeAssert` в рантайме);
- `input`/`exit` — зарезервированные keyword'ы, в `all_semantic.lm` не используются;
  эмиссия появится с реализацией парсера (вне скоупа этого плана).

---

## 5. `RPath` — единый примитив «живой ссылки»

Обобщение для `ref`, `self`-методов и write-back:

```cpp
struct RPath {                     // маршрут в боксовом контексте
    Value* root;                   // слот верхнего уровня (глобал/локальный слот)
    std::vector<int> path;         // индексы вниз: {} | {i} | {i,j} ...
};
Value  get_path(const RPath& r);
void   set_path(RPath r, Value v); // в т.ч. const/final-проверки
RPath  capture_path(Node* lhs, Context& c);  // из LHS-ноды: var / a.b / a[i] / NS::x
```

Вся семантика уже есть: `writePathValue`/`setIndexRec`/`resolve_scope` в `DeepAnalizer`.
Эмиттер не дублирует логику, а раскладывает узлы на эти три функции.

---

## 6. Дюндеры и write-back

Где `self` может мутировать объект — после вызова метода **пишем объект обратно в
слот** (через `RPath`), иначе мутации потеряются. Эмиттер вызывает уже готовый
рантайм-паттерн `callDunderWithWriteback` для всех мутирующих дюндеров
(`__set_index__`, `__assign__`, `__del__`) и методов с `self`.

---

## 7. Таблица эмиссии `NodeTypes` (актуально, 66 нод — NodeBase.cpp:3-72)

| # | Нода | Эмиссия (boxed) |
|---|------|------------------|
| 0 | NodeNumber | `Value(num)` |
| 1 | NodeString | `Value(RuntimeString("…"))` |
| 2 | NodeBool | `Value(true/false)` |
| 3 | NodeScope | `{ … }` блок + C++ scope |
| 4 | NodeStatementsScope | последовательность statements |
| 5 | NodeModifier | развернуть моды в аттрибуты, эмитить узел |
| 6 | NodeNamespace | `makeNamespace(name)` |
| 7 | NodeIf | `if(is_true(c)) … else …` |
| 8 | NodeWhile | `while(is_true(c)) …` |
| 9 | NodeFor | `{init; while(check){body; update;}}` |
| 10 | NodeBreak | `break` |
| 11 | NodeContinue | `continue` |
| 12 | NodeReturn | `return Value;` |
| 13 | NodeOut | `print(formatValue(…))` |
| 14 | NodeOutln | `println(formatValue(…))` |
| 15 | NodeEcho | `println(formatValue(…))` (см. §4.15) |
| 16 | NodeDelete | `rt_delete_elem(host, idx)` / дюндер `__del__` |
| 17 | NodeNull | `Value(Ty::Null)` |
| 18 | NodeVariable | `ctx.lookup(name)` / `get_path(RPath)` |
| 19 | NodeUnaryMinus | `rt_neg` |
| 20 | NodeUnaryPlus | `rt_pos` |
| 21 | NodeUnaryNegation | `rt_not`; при операнде-`Array` → tuple (`!{…}`) |
| 22 | NodeUnaryDereference | `rt_deref` |
| 23 | NodeUnaryBitwiseNot | `rt_bitnot` |
| 24 | NodeUnaryAddressOf | `rt_addr` |
| 25-42 | бинарные (18 шт.) | `rt_*` с диспатчем дюндеров (§4.11) |
| 43 | NodeBinaryScopeResolution | `resolve_scope` цепочкой |
| 44 | NodeBinaryAssignment | `set_path(lhs, rhs)` (составные уже RHS-бинор); диспатч `__assign__` (§4.11) |
| 45 | NodeCast | `rt_cast(a, T)` (дюндер `__as__`) |
| 46 | NodeMemberAccess | `get_member(a,"b")` (auto-deref для `*S`) |
| 47 | NodeFunctionCall | `call_callable(f, args)` |
| 48 | NodeFunctionType | аннотация-метаданные |
| 49 | NodeTupleType | аннотация (`Tuple[i64,bool,char]`) |
| 50 | NodeArrayType | аннотация (`Array[i64]`) |
| 51 | NodeArray | `Value(RuntimeArray(els))` |
| 52 | NodeArrayAccess | `get_index(a, {i…})` |
| 53 | NodeLambda | присваивание; вызов — ошибка до closure (№1) |
| 54 | NodeNew | `makeNew(modifiers, value)` |
| 55 | NodeTypeOf | статический `static_type` из анализатора / дюндер `__typeof__` / рантайм `typeof_dispatch` |
| 56 | NodeSizeOf | дюндер `__sizeof__` / размер/длина |
| 57 | NodePtr | `rt_ptr(addr, "T")` |
| 58 | NodeVariableDeclaration | `ctx.define(name, value, mods)` |
| 59 | NodeFunctionDeclaration | `registerFunction(name, sig, lambda)` |
| 60 | NodeStructDeclaration | `registerStruct(name, fields, methods, statics)` |
| 61 | NodeEnumDeclaration | `enumEnum` + варианты |
| 62 | NodeStructLiteral | `structLit(name, {field: expr}, generics)` |
| 63 | NodeUnaryPostfixIncrement | `rt_postfix_inc` (`a = a+1; вернуть старое`) |
| 64 | NodeUnaryPostfixDecrement | `rt_postfix_dec` |
| 65 | NodeAssert | `rt_assert(cond, msg)` |

---

## 8. Пример эмиссии (кусок из `all_semantic.lm`)

```lumen
struct Point {
    strict let x: f64;
    strict let y: f64;
    static fn origin() -> Point : { ret Point { x: 0.0, y: 0.0 }; }
}
let pt = Point { x: 3.0, y: 4.0 };
pt.translate(1.0, 1.0);

let target = 10;
ref let alias = target;
alias = 5;
```

```cpp
// ...прелюдия (§3)
void fn_Point_origin(A*) { return structLit("Point", {{"x",Value(0.0)},{"y",Value(0.0)}}); }
int main() {
    initRuntime();
    registerStruct("Point",
        {{"x",Mods{.Strict=true}},{"y",Mods{.Strict=true}}},
        {{"translate",fn_Point_translate}},
        {{"origin",fn_Point_origin}});
    Value pt = structLit("Point", {{"x",Value(3.0)},{"y",Value(4.0)}});
    MethodCall(capture_path(pt), "translate", {Value(1.0), Value(1.0)});   // RPath-метод
    Value target = ctx.define("target", Value(10));
    RPath alias = capture_path(&target);          // ref let alias = target
    set_path(alias, Value(5));                    // alias = 5 → target==5
    return 0;
}
```

---

## 9. Сборка

```bash
lumenc all_semantic.lm --cpp -o out.cpp     # эмиссия (новый флаг в lumenc.cpp)
clang++ out.cpp -std=c++17 -o out.exe
out.exe                                     # сверяем с комментариями all_semantic.lm
```

- `-o` в lumenc сейчас не задействован — привязать к эмиттеру;
- прелюдия и `CodeGenerator` — файл `compiler/CodeGenerator.cpp` (сегодня заглушка),
  каркас обхода уже есть (`CodeGenUnit::emitNode`).

---

## 10. План работ (M0–M6, по разделам `all_semantic.lm`)

- **M0 — каркас. ✓ СДЕЛАНО.** `-c`/`-o` в `lumenc.cpp` и `ArgParser.cpp`; эмиттер
  `compiler/CodeGenerator.cpp` (заменена заглушка): встроенный boxed-рантайм
  (`rt::Box` + `bin/neg/fmt/print/println/Ctx`), литералы, `let`/`strict let`/
  `const let`, присваивание, арифметика `+ - * / %`, унарный минус, char/str-конкатенация,
  `"ab"*3`, char-арифметика, `echo/out/outln`, `null`/bool/char-литералы, `NodeScope`.
  Валидировано: `probe_cg_m0.lm` → `out_m0.cpp` → компиляция → вывод = спецификация;
  регрессия: `all_semantic.lm` по-прежнему 0 ошибок, эмиссия полного файла не падает
  (неизвестные ноды → `/* TBD */`). Приёмка M0 — литералы + `let` + арифметика.
- **M1 — поток. ✓ СДЕЛАНО.** if/else-if/else (+ if-выражение `let x = if (c) {...} else {...}`),
  while/for/break/continue, `&&`/`||`/`!`, сравнения `== != < > <= >=` (строки — по длине),
  правдивость условия (bool | число), для `for` — нативный C++-for (continue падает в update).
  `echo` транслируется как `outln`. Валидировано `probe_cg_m1.lm`
  (вывод: positive/ok/neg/true/false/true/012/01234/135); полный `all_semantic.lm`
  по-прежнему 0 ошибок, эмиссия не падает.
- **M2 — данные. ✓ СДЕЛАНО.** массивы `{}`/`+=`/индексы, строки `s[i]`/`+`, tuple `!{…}`
  (`rt::not_` → `Box::tup`), `del host, idx` (write-back по переменной), `sizeof`,
  `as i64` (char→int код). В рантайме `Box` получил тег `T_ARR`/`T_TUP` + `std::vector<Box>
  items`, хелперы `rt::index/del/szof/cast`, конкатенация в `bin('+')`, формат
  `{10, 20, 30, 40}` / `[1, a, true]`. Валидировано `probe_cg_m2.lm` → `out_m2.exe`:
  вывод = 11 ожидаемых строк (транспилированная печать совпадает со спецификацией)
  (`§4.6`). Приёмка: «массивы/кортежи/строки».
- **M3 — функции. ✓ СДЕЛАНО.** регистрация/вызов/overload/default-args/дженерики,
  `Func`-значения, рекурсия (`§4.3`). Все функции компилируются в C++ единой сигнатуры
  `rt::Box(rt::Ctx, const std::vector<rt::Box>&)`; препасс строит реестр имён/перегрузок
  (`resolveForCall` по числу аргументов + статическим тегам `exprTag`/`paramTag`),
  дефолты заполняются на месте вызова, generic-функции — единые (Box-параметры), вызовы
  по имени — статические `lf_<name>`, `Func`-значения — `rt::Box::fn(&lf_...)` +
  `rt::call`, рекурсия — нативная C++. `Ctx` получил parent-цепочку и глобальный
  `rt::g_ctx` (локальный Ctx на каждый вызов). Лямбды — как анонимные функции
  (`collectLambdas`, тело-выражение → `return <expr>`). Валидировано `probe_cg_m3.lm`:
  30/Hello, world/Hello, Lumen/shown/42/text/120/3; регрессия `all_semantic.lm` — 0
  ошибок, эмиссия собирается (в т.ч. c лямбдой §4.3).
- **M4 — структуры/enum/namespace. ✓ СДЕЛАНО.** поля, `self-методы`, static/const-fn, дженерики,
  литералы, `new S {}`, private, enum, `::` (`§4.2, §4.9-4.10`). Box получил теги `T_STRUCT`/
  `T_PTR` + `shared_ptr<map<string,Box>>` для write-back поля, `shared_ptr<Box>` для `new`;
  препасс (`collectFrom`) регистрирует `instanceRegistry` (instance-методы `mf_<Struct>_<m>`),
  `enumMap` (варианты), `nsVars` (`Ns::var`); методы в `fnRegistry` под ключами `Name::method`
  (static) / `Ns::fn`. `buildCall` обрабатывает callee `NodeMemberAccess` (метод — recv первый,
  + аргументы + дефолты), `NodeBinaryScopeResolution` (static/ns — `resolveForCall`); в `emitExpr` —
  `NodeStructLiteral` → `Box::strct(map)`, `NodeMemberAccess` → `rt::field`, `NodeBinaryScopeResolution`
  → enum index / `fn` value, `NodeNew` → `Box::ptr`. `emitStmt` обрабатывает `NodeNamespace`
  (prefixed defines), `NodeBinaryAssignment` + lhs `NodeMemberAccess` → `rt::putf`. Лямбды, Func и
  рекурсия (M3) продолжают работать, т.к. instance-методы нативные C++ (`Ctx` по значению).
  Валидировано `probe_cg_m4.lm` → `out_m4.exe`: 10 строк (Point/translate/is_origin/origin,
  Duo::of/new, Color, Auth); регрессия `all_semantic.lm` — 0 ошибок, полная эмиссия собирается
  и работает. Приёмка: «структуры», «enum», «namespace».
- **M5 — ref и дюндеры. ✓ СДЕЛАНО.** `RPath`, ref-алиас/параметр/возврат,
  операторные дюндеры (`__add__`/`__eq__`/`__sizeof__` dispatch), `typeof/sizeof/as`,
  `?T`/union (`§4.8, §4.11-4.13`). Реализация: Box получил тег `T_REF` + `shared_ptr<Box> sp`
  (алиас на ячейку env-хранилища); `rt::rref/refcell_ptr/derefval/setref`; препроцесс
  `collectFrom` собирает `structDunders` (методы `__dunder__`), генератор main эмитит
  `rt::reg_dunder`; `rt::op_dispatch` (арифметика → `bin`, сравнение → `cmp`, direct) и
  `rt::szof_dispatch` (`__sizeof__`). `emitStmt` для `ref let x = var` → `rref(refcell_ptr(...))`,
  `ref let x = f()` → результат функции; `NodeReturn` + `returns_ref` возвращает живой алиас;
  ref-параметры на call-site оборачиваются в `rref(refcell_ptr(env.get(...)))`; запись через
  ref/вер из функции → `rt::setref`, `slot_get() = v` → прямое разыменование `.sp`.
  `Box::strct` теперь принимает имя типа (для `tname` в диспетчере).
  Приёмка: «ссылки», «перегрузка», «типы».
- **M6 — периферия + консолидация. ✓ СДЕЛАНО.** `new`/`&`/`*`/`ptr`,
  `echo/out/outln`, `assert`, константы (const-ячейки) (`§4.7, §4.15`). Реализация:
  `NodeUnaryDereference` → `rt::deref`, `NodeUnaryAddressOf` → `rt::refptr`
  (T_PTR с невладеющим shared_ptr на ячейку env), `NodePtr` → `rt::Box::ptr(addr)`,
  `*p = v` → `rt::setptr`, `del p` (без индекса) → сброс ячейки в null,
  `assert cond[, msg]` → `if (!truthy) abort`, `Ns::var = v` (write-back) → `g_ctx.get(key) = v`,
  чтение `Ns::var`/`Ns::fn` через scope-resolution. **Финальная регрессия**:
  новый `regress_transpiler.ps1` — для всех 8 целей (`probe_cg_m0..m6` + `all_semantic.lm`)
  анализ→эмиссия→g++→запуск→построчная сверка с эталоном; все пройдены.
  Приёмка: «периферия». **TBD-заглушки устранены**: `let T = Tuple[...]` → `Box::str`
  с описанием типа (NodeTupleType/NodeArrayType/NodeFunctionType в emitExpr),
  union-типы `A | B` → битовые операции в `bin` (`| & ^ << >>`), анонимный
  `namespace {...}` как значение → `Box::strct` из полей тела.

Оценка объёма эмиттера: 1500–2500 строк (как в старом плане) + прелюдия.

---

## 11. Открытые вопросы

1. **Лямбда-значения.** Интерпретатор хранит лямбду как `Null` (вызов невозможен).
   До реализации closure в рантайме — эмиттер выдаёт `LAMBDA_CALL`; в `all_semantic.lm`
   лямбда только присваивается, поэтому Milestone M3 это не блокирует.
2. **`echo`** — транспилируется как `rt::println(expr)`, т.е. идентично `outln`;
   анализатор по-прежнему строит inlay-хинты в редакторе.
3. **Состояние рантайма-исполнения в lumenc.** `DeepAnalizer` считает runtime-ошибки
   (`DIVISION_BY_ZERO`, `CALL_…`) при анализе; транспилированная программа использует
   **те же** хелперы ⇒ поведение идентично автоматически.
4. **Std-библиотека** (`std/Pair.lm` с полями `first_`/`second_` и т.п.) — унаследованный
   вопрос из `transpilation-cpp.md §19`; вне скоупа `all_semantic.lm`.

---

## 12. Ресурсы

- `all_semantic.lm` — эталонная семантика и критерий приёмки (этот план);
- `compiler/CodeGenerator.cpp` — место реализации (заглушка);
- `compiler/DeepAnalizer.cpp` — единственный источник: `execute()` + `tryDunder` +
  `writePathValue`/`resolve_scope`;
- `compiler/runtime/{Values,Context,Functions,Strings}.cpp`, `core/TypeSystem.cpp` — хелперы;
- `compiler/core/Nodes/NodeBase.cpp` — 68 типов нод;
- `compiler/ASTGenerator.cpp` — синтаксис (в т.ч. `new <mods>`, `!{…}`);
- `compiler/plan/references-plan.md` — семантика `ref` (write-back модель);
- `compiler/plan/transpilation-cpp.md` — база (boxed-стратегия, таблицы операторов).