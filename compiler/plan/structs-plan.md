# План реализации структур и дженерик-структур

Целевой синтаксис (финальный, согласованный):

```lumen
struct[T, Size: i64 = -1] Vec {
    strict let _data: Array[T, Size] = {};
    private let len: i64 = 0;

    static fn filled(size: i64) -> Vec[T, Size] : { ... }   // ::static
    fn push(self, x: T) : { self._data[self.len] = x; self.len += 1; }
    const fn get(self, i: i64) -> T : { ret self._data[i]; }
}

let v = Vec[i64, 8] { _data: [1, 2, 3] };   // литерал; _data обязателен (Size>0)
let w = Vec[str] { };                        // Size=-1, дефолт {}
let p = new Vec[i64] { };                    // *(Vec[i64])
p.get(0); v._data; Vec[i64]::filled(4);      // .-авто-разыменование, :: статик
```

---

## M1 — Модель данных: тип STRUCT и значение-структура
**Файлы:** `core/TypeSystem.cpp`, `runtime/Values.cpp`, `runtime/Context.cpp`
- Новый тип `TypeType::STRUCT`: `container_1` = generic-args (типы и значения, как у ARRAY), плюс регистр имён `StructRegistry` (name → StructSymbol) в Context.
- `Type::StructType(name, generics)`, `isStruct()`, `toString()` → `Vec[i64, 8]`; equality и `operator>> / <<` — совпадение имени+аргументов (номинально: `Vec[i64,8] ≠ Vec[i64,16]`; варп по соглашению).
- `Value`-вариант «struct»: поля в `std::map<String,Value>` + `StructType`; фабрика `new_struct`, гет/сет поля.
- **Решение семантики копирования**: присваивание/аргументы — копия полей; мутация через `self` записывается обратно в lvalue (`ContextObject`).

**Acceptance:** `echo p.x` / `p.x = 5;` работают на value; `const fn` запрещает запись; `let q = p; q.x = …` не меняет `p`.

## M2 — Парсер
**Файлы:** `ASTGenerator.cpp`, `core/Nodes/NodeStructDeclaration.cpp`, +`NodeStructField`, +`NodeStructLiteral`
- Разрешить `[GenericParam, ...]` между `struct` и именем: генерики через существующий `parseGenericParameter`.
- Тело struct в struct-режиме: `[моды] let name : type = default;` → `NodeStructField{name_token, mods, type, default}`; `[моды] fn …` → метод (self-параметр).
- Литерал: постфикс `Name[args] { … }` / `Name { … }` → `NodeStructLiteral`; отличие от блока — context (записи `имя: expr`), хвостовая запятая, шорткат `{ x }`.
- `new Name[args] { … }` → `*STRUCT`.
- `NodeMemberAccess` (`a.b`) и `::` переиспользуются без изменений AST.

## M3 — Анализ: регистрация + инстансы (моно-морфная модель)
**Файлы:** `DeepAnalizer.cpp`, `runtime/Functions.cpp`
- `NodeStructDeclaration`: `StructSymbol{ name, generics[], fields[], методы (FunctionContainer) }`; регистрация имени в Context (`is_type_decl=true`).
- **Инстанс = материализация под аргументы**: на `Vec[i64, 8]` забиндить `generic_values` и вычислить типы полей через `evalTypeExpr` (даёт «зависимое поле» `Array[T, Size]`).
- Методы: инстанцировать сигнатуру; `v.method(a)` — резолв в StructSymbol, `self` связывается с lvalue; `Struct::method` — static/конструктор.
- Дефолты полей при создании; отсутствие обязательного поля — ошибка.
- `typeof(v)` → `Type::StructType(name, args)`.

---
**Статус:** M1 ✅ · M2 ✅ · M3 ✅ · M4 ✅ (private/final/strict/const-fn) · M5 ✅ (LSP hover/completion) · M6 — регрессии.

## Состояние M1-M4 (прогресс)
- M1: `TypeType::STRUCT` + `int_args`/`arg_mask`, `Type::StructType(name,targs,vargs,mask)`, `isStruct()/==/>>/toString`; `RuntimeStruct` + `new_struct`; `formatValue` → `(Vec[i64, 8] { _data: {...}, len: 3 })`.
- M2: `parseStruct` принимает `[GenericParam, ...]`; `NodeStructLiteral` (type_expr, generics, inits, шорткат `{ x }`, хвостовая запятая); дизамбигуация `[` по `(`/`{`; подавление литералов вокруг `-> return_type` (флаг `suppress_struct_literals` в fn/lambda).
- M3 (DeepAnalizer): реестр `structs` (StructSymbol: fields + методы FunctionContainer + decl_context); `buildStructInstance` (связывание targs/vargs/mask); `materializeStruct` (дефолты в bind-контексте, литералы в контексте вызывающего, adopt `{}`, strict); литeral/инстанс-тип/`let w: Vec[i64,-1] = ...`; member get (auto-deref)/assign; индекс-запись поля `self._data[i]`; методы с self-write-back; static через `::`; вложенные структуры. Автоприрост динамических массивов по `arr[len]==size` убран: запись по индексу за пределами существующих элементов — всегда `ARRAY_INDEX_OUT_OF_RANGE` (в т.ч. в `SetIndexRec`).
- M4 (валидация) — сделано:
  - `PRIVATE_FIELD` (115): чтение/запись приватного поля вне тела структуры (трек через `active_struct`, выставляемый вокруг тела метода и статика, включая `::`-методы).
  - `final`-поля: assign после создания → FINAL_REASSIGNED (и `final`-владелец целиком).
  - `CONST_FN_MUTATION` (116): запись в члены ресивера внутри `const fn` (`const_receiver` = self-объект вызывающей константной функции).
  - `CONST_FN_INVOKES_MUTATOR` (117): вызов не-const метода с ресивером == текущий const-ресивер.
  - strict-поля: reified-тип поля при member-assign через `fieldTypeFor` (bindings из инстанс-типа) → STRICT_TYPE_MISMATCH.
  - `is_const` у `Function` (методы-`const fn` ловятся в collectMethods через моды NodeModifier).
- Проверено харнессами: lsp_struct_prog (чисто), lsp_struct_neg (4 негатива), lsp_struct_m4 (private/final/strict), lsp_struct_const (const-fn). Старые регрессии зелёные.
- **Fix: метод-дженерик с дефолтом на структурный generic** (`fn push[PT = T](self, x: PT)` — дефолт ссылается на generic-имя структуры). При вызове метода недостающие generic-дефолты вычисляем в контексте с привязкой generic-имён инстанса (`bindingsForType` из `recv_value.T`: T→i64, Size→-1) и передаём как **значения** в `generic_args` (пропуски — placeholder `new_null()`, позиции не сдвигаются). Live-контекст `resolve_function_call` не подменяется — первый вариант (child-Context как `caller`) давал краш LSP (v.size()/p.norm()/p.set()). Проверено: repro_push/repro_full (push/+=, `let pt = T`), edge-cases (Vec[bool].push(true), явный `push[i64](...)`, value-generic дефолт, вложенные методы), lsp_struct_m5 + все регрессии зелёные.

## Открытые пункты (опционально, не блокирует)
- static `let`-поля (константы) через `::` (сейчас только методы).
- `==` структур (тип-равенство есть; сравнение значений — нет).
- static/free `fn` возврат `-> Vec[T, Size]` для reified-типа (generic-имена внутри struct → BASE/Any при декларации).

## M5 — LSP (hover/completion/inlay)
**Файлы:** `lumen-lsp.cpp`
- hover: тип → `**struct[T, Size: i64 = -1] Vec**`; поле → `**strict let _data: Array[T, Size]**`; метод → `**fn push(self, x: T)**`.
- completion: `Vec { |` → поля; `v.`/`v::` → поля+методы (private скрыть).
- `typeof` в inlay/шаблоны — без правок.
- **Сделано:** реестр `g_structs[uri]` (`std::move(da.structs)` из analyzeAndPublish); хелперы `exprText/nodeTypeText/structFieldText/methodSignature/structDocBody/resolveStructOwner`; `contextHover`+`buildHover` принимают `structs`, ветка члена структуры (поля+методы, локальные `collectParamsInRange`/`collectLetVars`, detail из RHS-литерала `let v = Vec[...] {}`); member-completion через namespace/struct-цепочку, `scoped` (`::` → только методы), `memberAccessContext` с `isScope`. Член ищется по полному `word`, не по `prefix`. Guard `it==end()` в `contextHover` (fix краша hover `#define PI`). Проверено: lsp_struct_m5 (H1-H4, C1-C5) + все старые регрессии зелёные.
- **Сделано (доп.):** автодополнение после цепочек с вызовами методов — `b.at(0).` теперь даёт члены возвращаемой структуры (`['first','left','right','second']` для `Box[Pair[i64,str],-1]`). `memberAccessContext` пропускает балансом `(...)` аргументов вызова (сегмент помечается в parallel `callFlags`); в `buildCompletion` сегмент-вызов резолвится как метод владельца, возвращаемый тип реифицируется под инстансом цепочки (`reifyMethodReturn`: подстановка generic-имён `T`/`Size` из `inst.container_1` по `arg_mask`, копия структурных имён из `da.structs`), вдоль цепочки протаскивается `Type inst` (для глобальной `let b` — `obj.value.T` с полным набором типовых аргументов). Для локальных переменных (detail = только имя структуры, без аргументов) реификация generic-возврата недоступна (ограничение); ховер цепочек с вызовами остаётся как был. Регрессии (m4/neg/prog/const/parse, generic, m5 C4, repro_full/push, demo_examples) зелёные.
- **Сделано (доп., hover):** `buildHover` теперь вызывает `memberAccessContext` с `callFlags`; `resolveStructOwner` принимает `callFlags` и реифицирует сегменты-вызовы (`reifyMethodReturn`) так же, как completion. Ховер на `b.at(0).first` и на методах/полях цепочек с вызовами работает (`**const fn first(self) -> L**`). В hover членов структуры (полей/методов) добавлена строка `*provided by struct[L, R] Pair*` — по заголовку `structHeaderText` (вынесен из `structDocBody`). Регрессии зелёные (m5 H1-H4, generic/shadow/final/mhover/nnew, struct_prog/parse/const/neg/m4, std/ifexpr, demo_examples, repro_full, completion_repro).
- **Сделано (доп., док-комментарии M7):** комментарий `/* … */` (или стек `//`) непосредственно над объявлением функции/структуры привязывается к ноде на этапе AST-генерации и красиво рендерится в hover под сигнатурой (`\n\n---\n<текст>`). Механика: `Preprocessor::filterComments` выкидывает комментарии из parse-потока, поэтому конструктор `ASTGenerator` получил второй параметр — сырые токены лексера; `docCommentAbove(decl_pos)` ищет токен декларации (`fn`/`struct`/имени) в сыром потоке и идёт вверх, пропуская модификаторы (`static fn`), собирая «стоящие отдельно» комментарии (проверка: предыдущий токен на другой строке — иначе `} /* tail */` НЕ док) с допуском пустых строк. Поля `doc_comment` добавлены `NodeFunctionDeclaration`/`NodeStructDeclaration` (и задел — `NodeVariableDeclaration`). Hover: глобальные функции через `findFunctionDocInTree` + `contextHover(doc, structDoc)`, методы через `findStructMethodDoc` (по телу `NodeStructDeclaration`), сама структура — `findStructDocInTree`; форматирование `appendDocBlock` (семантический отступ, снятие `* ` и `//`). Ограничения: перегрузки (несколько сигнатур) — док не добавляется, пока `functions.size()==1`; макрос-сгенерированные fn без доков. Тест `lsp_doc.py` (H1–H7) + все M1–M6 регрессии зелёные.

## M6 — Регрессии
- Старые harness зелёные; новый `lsp_struct` харнесс; ограничения: без move-семантики и наследования (композиция).
- **������� (���., P8):** hover/completion ������� �� ��������� ���������� (`lb.at(0).first`) ������ ���������� �� ������� ���� DeepAnalyzer: `g_localTypes[uri] = std::move(da.local_types)` (���� `line<<32|index` ������ ����� ����������), `LocalCandidate.declKey`, hover-����� 2b ����������� ������ ��� (`let lb: Box[Pair[i64, str]]` ������ `Box`/`auto`). �������� �� ��, ��� � inlay-������: ���� ������� ����������� DeepAnalyzer, ������ ���� ������� ������� � �����, ������� � �� ���������� �������� ��������� ���� � ������� �� ������������� (��������� ����������� ������������� �����������, ����� ��� ��������� ������/P2). ����� ������� ���� ������������ generic-������� ���������� (`strict let first: FirstT`) � ��� � ���������� �������.
