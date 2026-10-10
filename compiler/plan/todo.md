# Lumen — Semantic Analyzer: план и идеи

> Динамическая система типов: значения несут тип во время выполнения (reified `Type`).
> Статически ловим только то, что не зависит от типов: имена, области видимости,
> структуру, модификаторы, поток управления. Несовместимость типов уходит в рантайм.

## Ключевая идея (обсуждено с пользователем)
Два этапа анализа по дереву:

### Этап 1 — Поверхностная (shallow) семантическая проверка
Цель: быстрая, дешёвая проверка **формы** узлов, не зависящая от таблицы символов.
- Модификаторы присваиваются корректным нодам и в корректных комбинациях
  (`const`/`static`/`final`/`strict`/`global`/`public`/`private` применимы к ожидаемым
  видам нод: let/const, fn, struct, namespace, блок).
- Каждая нода имеет ожидаемых детей (арность/форма), базовая связность.
- Проверки вида «оператор/конструкция уместна в данном контексте».
- Не требует SymbolTable — работает даже на частично «битом» AST, даёт быстрый фидбек.
- Все ошибки показываются полностью: ничего не скрывается и не подавляется
  (язык учебный — важно видеть каждую проблему, каскад диагностик приветствуется).

### Этап 2 — Глубокая (deep) диагностика
Цель: полноценная семантика поверх заведомо «формально корректного» дерева.
- Таблица символов / ScopeStack, резолюция имён (`UNDECLARED_NAME`).
- Переобъявления (`REDECLARATION`) в рамках области видимости.
- Вызовы: `CALL_ARG_COUNT`, `GENERIC_ARG_COUNT`.
- Struct/namespace: `NO_SUCH_MEMBER`, `ns::x`.
- Модификаторы на уровне семантики: `CONST_ASSIGN`, видимость `private/public`.
- Поток управления: `MISSING_RETURN`, `UNREACHABLE_CODE`.
- (опционально) аннотация каждого узла `typeRef` для будущего codegen.

## Почему так — аргументация сеньора
1. Shallow-проход отсекает грубые ошибки до того, как deep-проход начнёт
   искать имена/типы → меньше ложных срабатываний и каскадных ошибок.
2. Shallow не зависит от SymbolTable → его можно запускать раньше и дешевле,
   он устойчив к «битому» дереву.
3. Deep-проход может предполагать, что форма узлов корректна, и сосредоточиться
   на смысле (имена/поток/модификаторы), а не на структуре.
4. Инкрементальная поставка: сначала shallow (модификаторы + форма), потом deep.

## Архитектура
- Оркестратор `SemanticAnalizer` (файл `compiler/SemanticAnalizer.cpp`), принимает
  готовый `AbstractTreeT ast`.
- `analyze()` вызывает сначала shallow-проход, затем deep-проход.
- `ScopeStack` — вектор областей `map<name, Symbol>`; `push/pop`, `resolve(name)`
  ищет от верха вниз (теневание бесплатно).
- `Symbol { name, kind, typeRef, is_const, decl_token, member_scope* }`.
  `kind ∈ {Variable, Parameter, Function, Struct, Namespace, GenericParam}`.
- `visit(Node*)` со `switch (node->type)` — норм для языка такого размера;
  при разрастании выносить обработчики в `visitXxx`.

### Token-инфраструктура (уже готова в нодах)
Каждая нода хранит свои исходные токены для диагностик:
- `NodeVariable.token` (имя)
- `NodeFunctionDeclaration.{name_token, gen_open/close_token, args_open/close_token}`
- `NodeVariableDeclaration.name_token`, `NodeStructDeclaration.name_token`,
  `NodeNamespace.name_token`
- `NodeFunctionCall.{open_token, close_token, gen_open_token, gen_close_token}`
- `NodeMemberAccess.member_token`, `NodeBinaryScopeResolution.member_token`
Ошибки строим из этих токенов, не из позиции парсера.

### Диагностика
- Переиспользуем `ErrorContainer` + `BaseError` (`.withCode(100+).withHint().withNote()`).
- Коды семантики: 100 UNDECLARED_NAME, 101 REDECLARATION, 110 CALL_ARG_COUNT,
  111 NO_SUCH_MEMBER, 120 CONST_ASSIGN, 121 MISSING_RETURN, 122 UNREACHABLE_CODE,
  130 GENERIC_ARG_COUNT.
- `sub_error` («Declared here») отправлять отдельной диагностикой (редактор не
  рендерит relatedInformation) — уже реализовано в `lumen-lsp.cpp`.

## Интеграция
- `lumenc.cpp` и `lumen-lsp.cpp`: после `ast_generator();` создать
  `SemanticAnalizer(ast_generator.nodes)`, вызвать `analyze()`. Ошибки попадут в
  тот же `error_container` → автоматически улетят в редактор.
- Нюанс сборки: `SemanticAnalizer.cpp` НЕ должен дублировать включение всего
  `ASTGenerator.cpp` вместе с `lumenc.cpp` (дубли определений). Либо держать
  стиль «всё включает .cpp» и включать анализатор вместо повторного ASTGenerator,
  либо компилировать анализатор отдельным TU, получая `ast` извне.

## Милестоуны
- M1 — каркас `SemanticAnalizer` + ScopeStack + Collect + Resolve-обход (база).
- M2 — Shallow: модификаторы на корректных нодах + базовая форма узлов.
- M3 — Deep: имена (`UNDECLARED_NAME`), `REDECLARATION`, стек областей.
- M4 — Deep: вызовы (`CALL_ARG_COUNT`, `GENERIC_ARG_COUNT`), резолюция callee.
- M5 — Deep: struct/namespace (`NO_SUCH_MEMBER`, `ns::x`).
- M6 — Deep: модификаторы/поток (`CONST_ASSIGN`, `MISSING_RETURN`, `UNREACHABLE`).
- M7 — (опц.) аннотация узлов `typeRef` для codegen; полировка диагностик.

## Заметки / открытые вопросы
- Модификаторы: часть уже проверяется на синтаксисе (`MODIFIER_MISPLACED` 52).
  Shallow-этап семантики должен проверять уже *семантику* модификаторов
  (напр. `const` на non-assignable, `private` вне struct/namespace), а не только
  формальную применимость.
- Не ловить типы статически — это против динамической модели.
- Не раздувать `Symbol` полной `TypeDesc` сейчас: достаточно имени типа + `is_const`;
  reified-`Type`/`TypeTable` вводим на этапе codegen.
