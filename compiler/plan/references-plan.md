# Ссылки в Lumen (`ref`) — план реализации

> Явная семантика ссылок: модификатор `ref` слева от типа (`fn inc(ref x: i32)`),
> механика — **write-back** (паттерн `self`), без настоящих Reference-ячеек на этом этапе.
> Документ описывает **как будет** — спецификация для реализации, не отчёт о сделанном.

---

## 1. Проблема (диагностика текущей модели)

Сейчас непрозрачно, когда копия, а когда «ссылка»:

- `Value` = `std::any`; присваивание, передача в функцию и чтение поля **копируют** значение
  (`runtime/Values.cpp:11-33`). Массив — `std::vector<Value>` (`Values.cpp:69-73`),
  структура — `std::map<String,Value>` (`Values.cpp:80-84`) → копия **глубокая**.
- `self` в методах: копия + **write-back** после вызова (`DeepAnalizer.cpp:3502-3515`) —
  поэтому мутации «прилипают», хотя это не ссылка.
- Параметры обычных функций: **копия**, мутации теряются (`DeepAnalizer.cpp:3465-3488`)
  — главный источник непонимания.
- Настоящие ссылки есть только через `&`/`*`/`ptr` (`Pointer` + `field_slots`/`VirtualHeap`,
  `runtime/Context.cpp:142-149`) и `new` — отдельный (указательный) механизм.

Цель: дать явный маркер `ref`, при котором мутации владельца видны, а правила
читаются прямо из кода.

## 2. Решения (утверждены пользователем)

1. `ref` — **модификатор слева от типа**: `fn inc(ref x: i32)`.
2. Механика — **только write-back** (копия внутрь + запись обратно по lvalue-пути
   аргумента), как у `self`. Никаких shared-ссылок-ячеек на этом этапе.
3. `&`/`*`/`ptr`/`new` **не трогаем**; код без `ref` работает ровно как раньше.
4. На будущий транспилятор ложится 1:1 (там уже запланирован `set_path`+write-back,
   `transpilation-cpp.md:341,557`).

## 3. Синтаксис

| Форма | Пример | Смысл |
|---|---|---|
| ref-параметр | `fn inc(ref x: i32)` | тело видит копию, после вызова мутации пишутся в аргумент вызывающего |
| ref-псевдоним | `ref let r = y;` | `r` — алиас на `y`; запись/чтение идут в цель |
| ref-возврат | `fn get() -> ref Point` | возвращает lvalue (`ref`), а не копию |
| (опционально) const ref | `fn peek(const ref t: T)` | read-only ссылка (мутаторы по ней запрещены) |

Порядок слов:
- параметры: модификаторы стоят перед именем (`parseFunctionParameter`,
  `ASTGenerator.cpp:959-991`) → `ref x: i32` ложится естественно.
- объявления переменных: модификаторы стоят **перед** `let` (`strict let first: FirstT`,
  `final strict let a`, `ASTGenerator.cpp:1647-1652`) → `ref let r = y;`.
  (`let ref r` парсер не поймёт — `ref` станет именем переменной.)
- возврат: `-> ref T` — разобрать `ref` сразу после `->`.

## 4. Лексер и константы

- Добавить `"ref"` в `Literals::MODIFIERS` (`core/Constants.cpp`) — `isModifierKeyword`
  (`ASTGenerator.cpp:1105-1109`) подхватит `ref` сразу в `parseFunctionParameter`
  и в цепочке statement-модификаторов.
- Убедиться, что `ref` токенизируется как `KEYWORD` (дописать в список ключевых слов,
  если его там нет).

## 5. Парсер

- `parseFunctionParameter` (`ASTGenerator.cpp:959`): `ref` автоматически попадёт в
  `FuncArg.modifiers` (вектор строк, `NodeFunctionDeclaration.cpp:6-15`) — код не меняется;
  достаточно константы из §4.
- `parseModifier`/`parseStatement` (`ASTGenerator.cpp:1164-1174, 1647-1649`):
  `ref let r = y` → `NodeModifier("ref", NodeVariableDeclaration)` — тоже без правок парсера.
- `-> ref T` (`parseFunction`, ~`ASTGenerator.cpp:1065-1102`): после `->` распознать
  ключевое слово `ref` и обернуть тип в маркер (см. §6).

## 6. AST

Без изменений существующих нод:
- `FuncArg.modifiers` уже `std::vector<String>` — наличие `"ref"` в векторе = ref-параметр.
- `NodeVariableDeclaration` без правок — модификаторы живут в `NodeModifier`-цепочке.

Новое (по аналогии с `NodeModifier`, `core/Nodes/NodeModifier.cpp`):
- `NodeRefType { Node* type; Token token; }` — маркер для `-> ref T`.
- `printNodes` (`ASTGenerator.cpp` c-e 2387/2017/2441): выводить `ref` в сигнатурах,
  объявлениях и `-> ref`, чтобы `-tree` показывал референсы.

## 7. DeepAnalizer (ядро)

### 7.1 ref-параметры — write-back как у `self`
В `executeFunctionCall` (`DeepAnalizer.cpp:3148`):
1. До биндинга параметров (`DeepAnalizer.cpp:3465`) для каждого `FuncArg`,
   чей `modifiers` содержит `"ref"`, сохранить **lvalue-путь аргумента**:
   - переменная → `ContextObject*` (как `recv_owner`, `DeepAnalizer.cpp:3290`),
   - цепочка `a.b`/`a[i]`/`a.b[i].c`/`NS::x` → node-путь (как `recv_member_node`, 3291).
2. После `execute(fn.body, call_ctx)` (`DeepAnalizer.cpp:3499`) — для каждого
   ref-параметра `writePathValue(путь, мутированное_значение)` (тот же вызов, что
   для self на `DeepAnalizer.cpp:3512-3514`).
3. Обобщить существующий блок «write-back self» (3507-3515) на список
   «(путь, значение)» из ref-параметров + self.

### 7.2 ref-псевдоним `ref let r = y`
В `case NodeTypes::NodeModifier` (`DeepAnalizer.cpp:1271`):
- модификатор `"ref"` над `NodeVariableDeclaration`: резолвить цель `y` в текущем
  контексте → сохранить алиас `r: (owner | node-путь)`.
- чтение `r` (`case NodeVariable` в `execute`): если есть алиас → вернуть значение по пути,
  иначе обычный `lookup`.
- запись `r = expr` (assign-пути `writePathValue`/`NodeBinaryAssignment`): путь берётся из алиаса.
- алиасы живут в текущем контексте и очищаются при выходе из него; резолв цели
  происходит **каждый доступ** → висячих ссылок по своей природе нет.

### 7.3 `-> ref T` (возврат ссылки)
- `return` в функции с `NodeRefType`: возвращать lvalue-путь.
- Допустимые цели (иначе диагностика): глобальные переменные, `ref`-параметры,
  ячейки `VirtualHeap`/поля объектов из `new`, поля этих целей.
- Присваивание в результат `get().field = x` → write-back по пути.
- Приём в переменную `let v = get();` — если нет `ref`, берётся **копия** (явная
  семантика: хочешь ссылку — пиши `ref let v = get();`).

## 8. SemanticAnalizer (диагностика)

- `"ref"` допустим только на: параметрах функций, `let`-декларациях, `-> ret`. На
  struct/enum/namespace/функции-целиком/generic-параметрах → ошибка (стиль
  `MODIFIER_MISPLACED`, новый код **REF_BAD_TARGET**).
- Аргумент в ref-параметр обязан быть **lvalue**: `NodeVariable`, `NodeMemberAccess`,
  `NodeArrayAccess`, `NodeUnaryDereference`, `NodeBinaryScopeResolution` → иначе
  **REF_RVALUE** (литералы, арифметика, вызовы, `new` — не пускать).
- `ref let r` без инициализатора → **REF_NEEDS_INIT**; `ref let r = литерал` → REF_RVALUE.
- Комбинации: `const ref` — разрешить (read-only); `ref` + `static`/`private`/`final` —
  запретить.
- (M3, опц.) предупреждение о «переживании» алиаса: если scope владельца заканчивается
  раньше scope алиаса — REF_ALIAS_STALE.

## 9. LSP

- hover: показывать сигнатуру как есть: `fn inc(ref x: i32)` — информация уже в
  `FuncArg.modifiers` / `NodeModifier("ref")`.
- автодополнение: ref-параметр в сигнатуре — пометка; подсветка при вызовах с rvalue.
- Исполнение ничего не трогаем (DeepAnalizer уже даёт семантику).

## 10. Совместимость с транспиляцией

`transpilation-cpp.md`:
- §8 `NodeFunctionDeclaration` — отметить ref-параметры: эмиссия = bind копии +
  `set_path(путь_аргумента, мутированный)` **после** вызова (как write-back self, §16 `writeback(self)`).
- §4 `NodeBinaryAssignment`/`NodeMemberAccess` — уже через `set_path`/`get_member` → ref-алиас эмитится
  как разрешённый путь цели, изменения — через `set_path`.
- Новых примитивов рантайма не требуется (фаза 1 boxed).

## 11. Милестоуны

- **M1 — синтаксис**: `"ref"` в модификаторы; `NodeRefType`; `printNodes`; проверка
  `parseFunctionParameter` + `ref let`. Валидация: `lumenc2.exe -f probe -tree`.
- **M2 — семантика**: ref-параметры (write-back, §7.1), ref-алиас (§7.2);
  SemanticAnalizer: REF_BAD_TARGET/REF_RVALUE/REF_NEEDS_INIT; probe-тесты.
- **M3 — возврат и расширения**: `-> ref T` (§7.3), REF_ALIAS_STALE (опц.), hover/
  автодополнение (§9); полная регрессия (все существующие `.lm` не должны сломаться).

## 12. Тесты (probe)

По образцу существующих регрессий (структура `lumenc2.exe -f <file>` + сравнение вывода):

- `probe_ref_param.lm` — `fn swap(ref a: i32, ref b: i32)`; мутации видны вызывающему.
- `probe_ref_struct.lm` — `fn fill(ref p: Point)`; запись полей через параметр.
- `probe_ref_alias.lm` — `ref let r = y;` чтение/запись через алиас, exit из scope.
- `probe_ref_return.lm` — `-> ref` на глобал/поле heap-объекта; запись через результат.
- `probe_ref_errors.lm` — REF_RVALUE (литерал/арифметика в ref-аргумент), REF_NEEDS_INIT,
  REF_BAD_TARGET.
- LSP: hover сигнатуры с `ref` (по образцу `lsp_*` тестов), автодополнение параметров.

## 13. Открытые вопросы (на этап реализации)

1. **`ref let r = y` vs `let ref r = y`**: рекомендация — `ref let` (модификатор перед
   `let`, как `strict let`). Если пользователь предпочитает `let ref` — потребуется
   спец-парсинг в `parseVariableDeclaration` (ветка `let ref <name>`).
2. **Передача алиаса в функцию** (`fn g(ref t: T)` от `r`): на M2 решить — (а) резолвить
   ref-псевдоним сразу к конечной цели (collapse), или (б) поддерживать цепочку
   «ссылка на ссылку» (дороже, но ближе к C++).
3. **`const ref`** — включить в M2 сразу или отдельным шагом.
4. **Глубина write-back**: для вложенных путей (`arr[i].field`) write-path уже готов
   (`writePathValue`); ограничений не вводим.
5. **ref на параметр-**POD (числа/строки)**: работает той же механикой (копия в тело,
   запись обратно) — разрешаем; производительность не цель этого этапа.