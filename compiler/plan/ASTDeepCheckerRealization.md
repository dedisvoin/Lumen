# Реализация ASTDeepChecker (M1 — глобальная область видимости, 100/101)

Цель этапа M1: поверх «поверхностного» анализатора (`ASTAnalizer`, коды 50–62)
добавить «глубокий» проход, который строит таблицу имён и выдаёт:

- **100 `UNDECLARED_NAME`** — использование имени, которое нигде не объявлено в
  видимых областях (переменная, параметр, функция, структура, пространство имён).
- **101 `REDECLARATION`** — повторное объявление имени в одной и той же области.

Фаза НЕ делает: проверку совместимости типов, разрешение `::` и `.`,
вывод типов — это M3/M8. Здесь только **имена и области видимости**.

---

## 0. Точка входа (уже есть)

В `compiler/SemanticAnalizer.cpp` в самом конце файла уже объявлен каркас:

```cpp
struct ASTDeepChecker {
    AbstractTreeT& ast;
    ErrorContainer& error_container;

    ASTDeepChecker(AbstractTreeT& ast, ErrorContainer& error_container) :
        ast(ast), error_container(error_container) {}

    void operator()() {
        for (auto node : ast) {
            deepVisit(node);
        }
    }

    void deepVisit(Node* node) {

    }
};
```

Его нужно наполнить согласно разделам 2–4. В `lumenc.cpp` его надо вызвать
после поверхностного анализатора (см. раздел 6).

---

## 1. Фабрики ошибок (ErrorBase.cpp)

В `compiler/core/ErrorBase.cpp` внутри `namespace SEMANTIC_ERRORS { ... }`
(рядом с `SELF_ASSIGNMENT`, ~строка 683) добавить две фабрики. Шаблон взят из
существующих (`.withType(...).withCode(...).withHint(...)`), конструктор
`BaseError(message, token)` — однотокеновый (используется для позиции ошибки).

```cpp
BaseError UNDECLARED_NAME(const Token token, const String& name) noexcept {
    return BaseError("Use of undeclared name '" + name + "'", token)
        .withType(MessageType::SyntaxError)
        .withCode(100)
        .withHint("Declare '" + name + "' before use, or check the spelling.");
}

BaseError REDECLARATION(const Token token, const String& name) noexcept {
    return BaseError("Redeclaration of '" + name + "'", token)
        .withType(MessageType::SyntaxError)
        .withCode(101)
        .withHint("'" + name + "' is already declared in this scope; "
                  "use a different name or remove the duplicate.");
}
```

> Проверить: нигде нет `static_assert`/switch по диапазону кодов ошибок. Если есть
> реестр кодов — допиши 100/101 туда же. Методы `withCode`/`withType`/`withHint`
> уже используются другими фабриками, так что доступны.

---

## 2. Модель данных (Symbol / Scope)

Вставить **перед** `struct ASTDeepChecker` в `SemanticAnalizer.cpp`.
В файле уже есть `#include <map>` и `#include <vector>` (строки 41–42);
добавить `#include <set>` для `builtins`.

```cpp
#include <set>

enum class SymbolKind { Variable, Parameter, Function, Struct, Namespace };

struct Symbol {
    String      name;
    SymbolKind  kind;
    Token       decl;        // токен объявления (для будущих диагностик)
    bool        is_global = false;
};

// помощник: развернуть цепочку NodeModifier до реальной декларации
static Node* unwrapModifier(Node* n) {
    while (n && n->type == NodeTypes::NodeModifier)
        n = static_cast<struct NodeModifier*>(n)->node;
    return n;
}
```

---

## 3. Поля и методы ASTDeepChecker

Полностью заменить каркас из раздела 0 на следующий код. Он опирается на
реальные поля узлов (проверены по `compiler/core/Nodes/*.cpp`):

- `NodeVariable{ String name; Token token; }`
- `NodeVariableDeclaration{ String name; Token name_token; Node* value_node; Node* type_node; }`
- `NodeFunctionDeclaration{ String name; Token name_token; std::vector<FuncArg> params; Node* body; }`
  — `FuncArg{ String name; ... }` **не содержит токена** (см. примечание в 5).
- `NodeStructDeclaration{ String name; Token name_token; Node* body; }`
- `NodeNamespace{ String name; Token name_token; Node* body; }`
- `NodeStatementsScope{ std::vector<Node*> nodes; }` — блок `{ }`
- `NodeScope{ Node* node; }` — скобочное выражение `( )`
- `NodeFor{ Node* init; Node* check; Node* update; Node* body; Token token; }`
- `NodeLambda{ std::vector<FuncArg> params; Node* body; }`
- `NodeFunctionCall{ Node* callee; std::vector<Node*> args; std::vector<Node*> generics; }`
- `AbstractTreeT = std::vector<Node*>`

```cpp
struct ASTDeepChecker {
    AbstractTreeT& ast;
    ErrorContainer& error_container;

    // стек областей видимости; scopes[0] — глобальная область
    std::vector<std::map<String, Symbol>> scopes;
    // имена-заглушки, которые НЕ считаются «необъявленными» (примитивные типы)
    std::set<String> builtins;

    ASTDeepChecker(AbstractTreeT& ast, ErrorContainer& error_container) :
        ast(ast), error_container(error_container) {
        // сверь реальные имена примитивных типов с compiler/core/LangSpec.cpp
        for (const char* t : { "int", "float", "bool", "string", "char", "void" })
            builtins.insert(String(t));
    }

    // ---- поиск имени: от внутренней области к внешней, затем глобальная ----
    const Symbol* resolve(const String& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            if (it->count(name)) return &(*it)[name];
        static const Symbol builtin_sym{ "builtin", SymbolKind::Variable, Token(), true };
        if (builtins.count(name)) return &builtin_sym;   // имя примитивного типа
        return nullptr;
    }

    // ---- объявление локального имени с проверкой REDECLARATION ----
    void declareLocal(const String& name, SymbolKind kind, const Token& tok) {
        // глобальный let уже зарегистрирован в collectGlobal — не дублируем и не репортим
        if (scopes.size() == 1) {
            if (!scopes[0].count(name))
                scopes[0][name] = Symbol{ name, kind, tok, true };
            return;
        }
        auto& cur = scopes.back();
        if (cur.count(name)) {
            error_container.addError(SEMANTIC_ERRORS::REDECLARATION(tok, name));
            return;
        }
        cur[name] = Symbol{ name, kind, tok };
    }

    // ---- препроход: собрать ВСЕ глобальные объявления (для прямых ссылок вперёд) ----
    void collectGlobal() {
        scopes.clear();
        scopes.push_back({});                       // глобальная область
        for (auto node : ast) {
            Node* n = unwrapModifier(node);
            if (!n) continue;
            SymbolKind k;
            Token tok;
            switch (n->type) {
                case NodeTypes::NodeFunctionDeclaration:
                    k = SymbolKind::Function;  tok = static_cast<struct NodeFunctionDeclaration*>(n)->name_token; break;
                case NodeTypes::NodeVariableDeclaration:
                    k = SymbolKind::Variable;
                    tok = static_cast<struct NodeVariableDeclaration*>(n)->name_token; break;
                case NodeTypes::NodeStructDeclaration:
                    k = SymbolKind::Struct;    tok = static_cast<struct NodeStructDeclaration*>(n)->name_token; break;
                case NodeTypes::NodeNamespace:
                    k = SymbolKind::Namespace; tok = static_cast<struct NodeNamespace*>(n)->name_token; break;
                default: continue;                       // не декларация верхнего уровня
            }
            String nm = (n->type == NodeTypes::NodeVariableDeclaration)
                ? static_cast<struct NodeVariableDeclaration*>(n)->name
                : (n->type == NodeTypes::NodeFunctionDeclaration)
                  ? static_cast<struct NodeFunctionDeclaration*>(n)->name
                  : (n->type == NodeTypes::NodeStructDeclaration)
                    ? static_cast<struct NodeStructDeclaration*>(n)->name
                    : static_cast<struct NodeNamespace*>(n)->name;
            if (scopes[0].count(nm))
                error_container.addError(
                    SEMANTIC_ERRORS::REDECLARATION(tok, nm));
            else
                scopes[0][nm] = Symbol{ nm, k, tok, true };
        }
    }

    // ---- точка входа ----
    void operator()() {
        collectGlobal();
        for (auto node : ast)
            deepVisit(node);
    }

    // ---- рекурсивный scope-aware обход ----
    void deepVisit(Node* node) {
        if (!node) return;
        switch (node->type) {

            // ===== декларации =====
            case NodeTypes::NodeFunctionDeclaration: {
                auto n = static_cast<struct NodeFunctionDeclaration*>(node);
                scopes.push_back({});                       // область тела функции
                for (auto& p : n->params)                  // параметры — в область функции
                    declareLocal(p.name, SymbolKind::Parameter, n->args_open_token);
                deepVisit(n->body);
                scopes.pop_back();
                break;
            }
            case NodeTypes::NodeStructDeclaration: {
                auto n = static_cast<struct NodeStructDeclaration*>(node);
                scopes.push_back({});
                deepVisit(n->body);                        // поля — члены области структуры
                scopes.pop_back();
                break;
            }
            case NodeTypes::NodeNamespace: {
                auto n = static_cast<struct NodeNamespace*>(node);
                scopes.push_back({});
                deepVisit(n->body);
                scopes.pop_back();
                break;
            }
            case NodeTypes::NodeVariableDeclaration: {
                auto n = static_cast<struct NodeVariableDeclaration*>(node);
                declareLocal(n->name, SymbolKind::Variable, n->name_token);
                deepVisit(n->value_node);                 // использования в инициализаторе
                deepVisit(n->type_node);                   // имя типа (примитивы — в builtins)
                break;
            }

            // ===== блоки / циклы / лямбды =====
            case NodeTypes::NodeStatementsScope: {
                auto n = static_cast<struct NodeStatementsScope*>(node);
                scopes.push_back({});
                for (auto c : n->nodes) deepVisit(c);
                scopes.pop_back();
                break;
            }
            case NodeTypes::NodeFor: {
                auto n = static_cast<struct NodeFor*>(node);
                scopes.push_back({});
                deepVisit(n->init);                       // let в init виден в теле
                deepVisit(n->check);
                deepVisit(n->update);
                deepVisit(n->body);
                scopes.pop_back();
                break;
            }
            case NodeTypes::NodeLambda: {
                auto n = static_cast<struct NodeLambda*>(node);
                scopes.push_back({});
                for (auto& p : n->params)
                    declareLocal(p.name, SymbolKind::Parameter, Token());   // FuncArg не хранит токен
                deepVisit(n->body);
                scopes.pop_back();
                break;
            }
            case NodeTypes::NodeScope:                    // ( expr ) — не новая область
                deepVisit(static_cast<struct NodeScope*>(node)->node);
                break;

            // ===== использование имени =====
            case NodeTypes::NodeVariable: {
                auto n = static_cast<struct NodeVariable*>(node);
                if (!resolve(n->name))
                    error_container.addError(
                        SEMANTIC_ERRORS::UNDECLARED_NAME(n->token, n->name));
                return;                                   // листьев нет
            }

            // ===== отложено на M3: квалификация deref-имён =====
            case NodeTypes::NodeBinaryScopeResolution: {  // a :: b
                auto n = static_cast<struct NodeBinaryScopeResolution*>(node);
                deepVisit(n->l_node);
                deepVisit(n->r_node);
                break;                                    // НЕ трактуем b как переменную (ждём M3)
            }
            case NodeTypes::NodeMemberAccess: {           // a . b
                auto n = static_cast<struct NodeMemberAccess*>(node);
                deepVisit(n->l_node);
                deepVisit(n->r_node);
                break;                                    // .b — поле, не имя (ждём M3/M8)
            }

            // ===== вызов функции (callee-переменная проверится рекурсией) =====
            case NodeTypes::NodeFunctionCall: {
                auto n = static_cast<struct NodeFunctionCall*>(node);
                deepVisit(n->callee);
                for (auto a : n->args)      deepVisit(a);
                for (auto g : n->generics)  deepVisit(g);
                break;
            }

            // ===== всё остальное: рекурсия в детей (как в semanticVisit) =====
            case NodeTypes::NodeModifier:
                deepVisit(unwrapModifier(node));
                break;
            case NodeTypes::NodeIf: {
                auto n = static_cast<struct NodeIf*>(node);
                deepVisit(n->condition); deepVisit(n->then_body); deepVisit(n->else_body);
                break;
            }
            case NodeTypes::NodeWhile: {
                auto n = static_cast<struct NodeWhile*>(node);
                deepVisit(n->condition); deepVisit(n->body);
                break;
            }
            case NodeTypes::NodeReturn:
                deepVisit(static_cast<struct NodeReturn*>(node)->expr);
                break;
            case NodeTypes::NodeBinaryAssignment: {
                auto n = static_cast<struct NodeBinaryAssignment*>(node);
                deepVisit(n->l_node); deepVisit(n->r_node);
                break;
            }
            case NodeTypes::NodeCast: {
                auto n = static_cast<struct NodeCast*>(node);
                deepVisit(n->l_node); deepVisit(n->r_node);
                break;
            }
            case NodeTypes::NodeArrayAccess: {
                auto n = static_cast<struct NodeArrayAccess*>(node);
                deepVisit(n->base);
                for (auto i : n->indices) deepVisit(i);
                break;
            }
            case NodeTypes::NodeArray:
                for (auto e : static_cast<struct NodeArray*>(node)->elements) deepVisit(e);
                break;
            case NodeTypes::NodeOut:
                for (auto e : static_cast<struct NodeOut*>(node)->exprs) deepVisit(e);
                break;
            case NodeTypes::NodeOutln:
                for (auto e : static_cast<struct NodeOutln*>(node)->exprs) deepVisit(e);
                break;
            case NodeTypes::NodeDelete:
                deepVisit(static_cast<struct NodeDelete*>(node)->expr);
                break;
            case NodeTypes::NodeNew: {
                auto n = static_cast<struct NodeNew*>(node);
                deepVisit(n->value);
                break;
            }
            // унарные и бинарные операции — спускаемся в операнды
            case NodeTypes::NodeUnaryMinus:
            case NodeTypes::NodeUnaryPlus:
            case NodeTypes::NodeUnaryNegation:
            case NodeTypes::NodeUnaryDereference:
            case NodeTypes::NodeUnaryBitwiseNot:
            case NodeTypes::NodeUnaryAddressOf: {
                Node* c = nullptr;
                switch (node->type) {
                    case NodeTypes::NodeUnaryMinus:       c = static_cast<struct NodeUnaryMinus*>(node)->node; break;
                    case NodeTypes::NodeUnaryPlus:        c = static_cast<struct NodeUnaryPlus*>(node)->node; break;
                    case NodeTypes::NodeUnaryNegation:     c = static_cast<struct NodeUnaryNegation*>(node)->node; break;
                    case NodeTypes::NodeUnaryDereference:  c = static_cast<struct NodeUnaryDereference*>(node)->node; break;
                    case NodeTypes::NodeUnaryBitwiseNot:   c = static_cast<struct NodeUnaryBitwiseNot*>(node)->node; break;
                    case NodeTypes::NodeUnaryAddressOf:    c = static_cast<struct NodeUnaryAddressOf*>(node)->node; break;
                    default: break;
                }
                deepVisit(c);
                break;
            }
            case NodeTypes::NodeBinaryMultiplicative:
            case NodeTypes::NodeBinaryDivision:
            case NodeTypes::NodeBinaryRemainder:
            case NodeTypes::NodeBinaryAdditive:
            case NodeTypes::NodeBinarySubtraction:
            case NodeTypes::NodeBinaryShiftLeft:
            case NodeTypes::NodeBinaryShiftRight:
            case NodeTypes::NodeBinaryEqual:
            case NodeTypes::NodeBinaryNotEqual:
            case NodeTypes::NodeBinaryLess:
            case NodeTypes::NodeBinaryGreater:
            case NodeTypes::NodeBinaryLessEqual:
            case NodeTypes::NodeBinaryGreaterEqual:
            case NodeTypes::NodeBinaryBitwiseAnd:
            case NodeTypes::NodeBinaryBitwiseOr:
            case NodeTypes::NodeBinaryBitwiseXor:
            case NodeTypes::NodeBinaryLogicalAnd:
            case NodeTypes::NodeBinaryLogicalOr: {
                Node* l = nullptr; Node* r = nullptr;
                switch (node->type) {
                    case NodeTypes::NodeBinaryMultiplicative: l = static_cast<struct NodeBinaryMultiplicative*>(node)->l_node; r = static_cast<struct NodeBinaryMultiplicative*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryDivision:       l = static_cast<struct NodeBinaryDivision*>(node)->l_node;       r = static_cast<struct NodeBinaryDivision*>(node)->r_node;       break;
                    case NodeTypes::NodeBinaryRemainder:      l = static_cast<struct NodeBinaryRemainder*>(node)->l_node;      r = static_cast<struct NodeBinaryRemainder*>(node)->r_node;      break;
                    case NodeTypes::NodeBinaryAdditive:       l = static_cast<struct NodeBinaryAdditive*>(node)->l_node;       r = static_cast<struct NodeBinaryAdditive*>(node)->r_node;       break;
                    case NodeTypes::NodeBinarySubtraction:    l = static_cast<struct NodeBinarySubtraction*>(node)->l_node;    r = static_cast<struct NodeBinarySubtraction*>(node)->r_node;    break;
                    case NodeTypes::NodeBinaryShiftLeft:      l = static_cast<struct NodeBinaryShiftLeft*>(node)->l_node;      r = static_cast<struct NodeBinaryShiftLeft*>(node)->r_node;      break;
                    case NodeTypes::NodeBinaryShiftRight:     l = static_cast<struct NodeBinaryShiftRight*>(node)->l_node;     r = static_cast<struct NodeBinaryShiftRight*>(node)->r_node;     break;
                    case NodeTypes::NodeBinaryEqual:          l = static_cast<struct NodeBinaryEqual*>(node)->l_node;          r = static_cast<struct NodeBinaryEqual*>(node)->r_node;          break;
                    case NodeTypes::NodeBinaryNotEqual:       l = static_cast<struct NodeBinaryNotEqual*>(node)->l_node;       r = static_cast<struct NodeBinaryNotEqual*>(node)->r_node;       break;
                    case NodeTypes::NodeBinaryLess:           l = static_cast<struct NodeBinaryLess*>(node)->l_node;           r = static_cast<struct NodeBinaryLess*>(node)->r_node;           break;
                    case NodeTypes::NodeBinaryGreater:        l = static_cast<struct NodeBinaryGreater*>(node)->l_node;        r = static_cast<struct NodeBinaryGreater*>(node)->r_node;        break;
                    case NodeTypes::NodeBinaryLessEqual:      l = static_cast<struct NodeBinaryLessEqual*>(node)->l_node;      r = static_cast<struct NodeBinaryLessEqual*>(node)->r_node;      break;
                    case NodeTypes::NodeBinaryGreaterEqual:   l = static_cast<struct NodeBinaryGreaterEqual*>(node)->l_node;   r = static_cast<struct NodeBinaryGreaterEqual*>(node)->r_node;   break;
                    case NodeTypes::NodeBinaryBitwiseAnd:     l = static_cast<struct NodeBinaryBitwiseAnd*>(node)->l_node;     r = static_cast<struct NodeBinaryBitwiseAnd*>(node)->r_node;     break;
                    case NodeTypes::NodeBinaryBitwiseOr:      l = static_cast<struct NodeBinaryBitwiseOr*>(node)->l_node;      r = static_cast<struct NodeBinaryBitwiseOr*>(node)->r_node;      break;
                    case NodeTypes::NodeBinaryBitwiseXor:     l = static_cast<struct NodeBinaryBitwiseXor*>(node)->l_node;     r = static_cast<struct NodeBinaryBitwiseXor*>(node)->r_node;     break;
                    case NodeTypes::NodeBinaryLogicalAnd:     l = static_cast<struct NodeBinaryLogicalAnd*>(node)->l_node;     r = static_cast<struct NodeBinaryLogicalAnd*>(node)->r_node;     break;
                    case NodeTypes::NodeBinaryLogicalOr:      l = static_cast<struct NodeBinaryLogicalOr*>(node)->l_node;      r = static_cast<struct NodeBinaryLogicalOr*>(node)->r_node;      break;
                    default: break;
                }
                deepVisit(l); deepVisit(r);
                break;
            }

            // листья без детей
            case NodeTypes::NodeNumber:
            case NodeTypes::NodeString:
            case NodeTypes::NodeBool:
            case NodeTypes::NodeNull:
            case NodeTypes::NodeBreak:
            case NodeTypes::NodeContinue:
                break;

            default:
                break;
        }
    }
};
```

> ⚠️ Важно: для **бинарных** и **унарных** узлов в `deepVisit` явно
> заинлайнен `switch` по типам (как в `semanticVisit`, строки 740–806 и 783–803):
> достаём `l_node`/`r_node` (или `node` унарного) и вызываем `deepVisit` на каждом.
> Главное — чтобы **каждый дочерний `Node*`** был передан в `deepVisit`, иначе
> вложенные `NodeVariable` не проверятся на 100.

---

## 4. Замечания по полям (почему именно так)

- **`FuncArg` не имеет токена.** Для параметров позиция ошибки — `best-effort`:
  используется `n->args_open_token` функции (или `Token()` для лямбды).
  Дубликат имени параметра (`fn f(a, a)`) всё равно ловится по имени.
  Точный токен параметра — задача M3 (добавить `Token` в `FuncArg`).
- **Глобальный `let` не дублируется.** `collectGlobal` кладёт все верхнеуровневые
  декларации в `scopes[0]`; `declareLocal` при `scopes.size()==1` не репортит
  повторно. Поэтому две глобальные `let x` дадут ровно ОДНУ ошибку 101 (из
  `collectGlobal`), а не две.
- **`::` и `.` не разрешаются в M1.** `NodeBinaryScopeResolution`/`NodeMemberAccess`
  рекурсивно обходятся, но правый операнд НЕ трактуется как переменная — иначе
  будут ложные 100 на полях структур и квалификаторах пространств имён.
  Полноценное разрешение — в M3.
- **Примитивные типы в аннотациях.** `let x: int = 5;` → `int` в `type_node`
  встречается как `NodeVariable`. Чтобы не было ложного 100, имена из `builtins`
  считаются «объявленными». Сверь список `builtins` с реальными типами в
  `compiler/core/LangSpec.cpp`.

---

## 5. Встраивание в lumenc.cpp

В `lumenc.cpp` после поверхностного анализатора добавить вызов глубокого:

```cpp
auto semantic_analizer = ASTAnalizer(nodes, error_container);
semantic_analizer();

ASTDeepChecker deep_checker(nodes, error_container);   // <-- добавить
deep_checker();                                        // <-- добавить

error_container.viewError();
```

`ASTDeepChecker` объявлен в том же `SemanticAnalizer.cpp`, который уже
включён в `lumenc.cpp`, так что дополнительных include не нужно.

---

## 6. Тесты (прогнать вручную)

Собрать: `g++ -std=c++17 -I. lumenc.cpp -o lumenc_test.exe`

| Код (файл) | Ожидание |
|---|---|
| `let x = y;` | **100** на `y` (не объявлено) |
| `let x = 5; let x = 6;` (один блок) | **101** на второй `x` |
| `fn a() : { b(); } fn b() : {}` | без ошибок (прямая ссылка вперёд через `collectGlobal`) |
| `fn main() : { let x = 1; let x = 2; }` | **101** на второй `x` |
| `fn f(a, a) : {}` | **101** на второй параметр `a` (best-effort позиция) |
| `let x = 5; outln x;` | без ошибок |
| `fn main() : { let x = 5; outln(y); }` | **100** на `y` |
| `let x: int = 5;` | без ошибок (`int` в `builtins`) |
| `struct S {} fn main() : { S v; }` | без ошибок (`S` в глобальной области) |
| `fn main() : { { let x = 1; } { let x = 2; } }` | без ошибок (тень в разных блоках) |

---

## 7. Известные ограничения (переходят в M3/M8)

1. **Вложенные `fn`/`struct` не в глобальной области** не регистрируются в
   `collectGlobal`, поэтому прямая ссылка вперёд на вложенную декларацию даст
   ложный 100. Решается в M3 полноценным обходом всех областей.
2. **`a::b` и `a.b`** не разрешаются (поля/квалификаторы) — ждут M3/M8.
3. **Параметры без токена** — позиция ошибки приблизительная (см. раздел 4).
4. **Типы как имена** проверяются только по наличию имени; совместимость типов
   (коды 140+) — в M8.
5. Тень глобального имени локальным (`let x` в функции при глобальном `let x`)
   разрешается корректно и НЕ считается переопределением.

---

## 8. Чек-лист готовности

- [ ] Добавлены фабрики `UNDECLARED_NAME`(100) и `REDECLARATION`(101) в `ErrorBase.cpp`.
- [ ] Добавлены `Symbol`/`SymbolKind`/`unwrapModifier` и `#include <set>`.
- [ ] `ASTDeepChecker` реализует `collectGlobal` + `deepVisit` + `resolve` + `declareLocal`.
- [ ] Блоки/функции/структуры/пространства имён/for/lambda корректно push/pop область.
- [ ] `NodeVariable` даёт 100; локальный дубль имени даёт 101; глобальный дубль — одна 101.
- [ ] `::` и `.` не вызывают ложных 100; примитивы в `builtins`.
- [ ] Вызов `deep_checker()` добавлен в `lumenc.cpp` после `semantic_analizer();`.
- [ ] Все тесты из раздела 6 проходят; сборка чистая (`g++ ... 2>&1 | Select-String "error:"`).

---

## 9. Логика работы этапа (концептуально)

Этот раздел описывает **поведение** глубокого анализатора словами, без привязки
к конкретным строкам кода из разделов 2–4. Читай его как спецификацию алгоритма.

### 9.1. Место в конвейере

```
лексер → препроцессор → ASTGenerator → ASTAnalizer (Pass 1, коды 50–62)
                                                    ↓
                                          ASTDeepChecker (M1, коды 100/101)
                                                    ↓
                                              codegen / вывод ошибок
```

- Этап **только читает** AST (`AbstractTreeT = std::vector<Node*>`) и **дописывает**
  ошибки в общий `ErrorContainer`. AST он не мутирует.
- Запускается **один раз на весь документ** после поверхностного `ASTAnalizer`.
- Поверхностный и глубокий проходы **ортогональны**: Pass 1 ловит структурно-
  модификаторные проблемы (50–62), глубокий — имена/области (100/101). Оба пишут
  в один `error_container`, и `viewError()` напечатает всё вместе. Между ними нет
  жёсткой связи — порядок вызова важен только для стабильности нумерации ошибок.

### 9.2. Вход и выход

| | Что |
|---|---|
| **Вход** | `ast` — готовое дерево; `error_container` — накопитель ошибок |
| **Выход** | те же ошибки 50–62 (если были) **плюс** 100/101 |
| **Побочки** | нет (read-only pass) |

### 9.3. Два под-прохода и зачем их два

Глубокая проверка внутри M1 состоит из двух шагов:

1. **`collectGlobal` (пред-проход).** Линейно сканирует *только верхнеуровневые*
   узлы дерева и заносит каждую глобальную декларацию (`fn`, `let`, `struct`,
   `namespace`) в `scopes[0]` — глобальную область.
   - **Зачем отдельный проход:** чтобы разрешить **прямые ссылки вперёд**.
      Пример: `fn a() : { b(); } fn b() : {}`. Когда `deepVisit` дойдёт до вызова `b()`
     внутри `a`, тело `b` ещё не обойдено. Без пред-прохода `b` была бы «необъявлена»
     (ложный 100). Пред-проход гарантирует, что все глобальные имена известны
     *до* начала рекурсивного обхода.
   - Глобальные дубликаты фиксируются здесь же (ровно одна 101 на пару).

2. **`deepVisit` (scope-aware обход).** Рекурсивно обходит **каждый** узел дерева,
   поддерживая стек областей видимости и проверяя использования имён.

> Инвариант: `collectGlobal` всегда выполняется **перед** `deepVisit`
> (см. `operator()`). Поэтому глобальные 101 выдаются раньше, чем 100/локальные 101.

### 9.4. Модель областей видимости (лексические scope)

- Области — это **стек** `scopes: vector<map<имя, Symbol>>`.
  `scopes[0]` — глобальная, всегда живая.
- **Вход** в область = `push_back({})` (пустая карта).
- **Выход** = `pop_back()`.
- Каждый `map` — это **одна** область; имя уникально внутри своего `map`.

**Правила затенения (shadowing):**
- Внутренняя область может объявить имя, уже существующее во внешней. Это
  **не** переопределение — имена лежат в разных `map`.
- При поиске имени смотрим от *вершины стека* (внутренняя) к *основанию*
  (глобальная); первое совпадение выигрывает → внутреннее перекрывает внешнее.
- `let x` внутри `fn` при глобальном `let x` — корректное затенение, **не** 101.

**Переопределение (101) — только внутри ОДНОЙ `map`:**
- Локально: `declareLocal` видит уже существующее имя в `scopes.back()` → 101.
- Глобально: `collectGlobal` видит дубль в `scopes[0]` → 101 (одна на пару, без
  повтора в `deepVisit`, т.к. `declareLocal` при `scopes.size()==1` не репортит).

### 9.5. Алгоритм `deepVisit` (пошагово)

Для каждого узла:

1. **Декларация функции** `fn`:
   - `push` новой области;
   - параметры (`FuncArg`) заносятся в эту область (`declareLocal(..., Parameter)`)
     *до* обхода тела, чтобы тело видело параметры;
   - `deepVisit(body)`;
   - `pop`.
2. **Декларация переменной** `let`:
   - `declareLocal(name, Variable, name_token)` — имя регистрируется в *текущей*
     области **до** обхода инициализатора;
   - затем `deepVisit(value_node)` и `deepVisit(type_node)`.
   - **Важный эффект порядка:** последовательные `let` в одном блоке регистрируются
     по мере встречи. Поэтому `let a = b; let b = 1;` даст **100** на `b` (на момент
     `a` `b` ещё не объявлена), а `let b = 1; let a = b;` — чисто. Внутри одной
     области прямых ссылок вперёд **нет** (в отличие от глобальной, где есть).
3. **Блок `{ }`, `for`, `struct`, `namespace`, `lambda`:** `push` → рекурсия в детей
   → `pop`. Для `lambda`/`fn` параметры регистрируются внутри свежей области.
   `NodeScope ( expr )` область **не** создаёт — просто спуск в `node`.
4. **Использование имени** `NodeVariable`:
   - `resolve(name)`; если `nullptr` и имя не в `builtins` → **100** `UNDECLARED_NAME`.
5. **`::` и `.`** (M1): обходятся рекурсивно, но правый операнд **не** считается
   переменной — иначе ложные 100 на полях/квалификаторах. Полное разрешение — в M3.
6. **Всё остальное** (вызовы, операции, `if`/`while`/`ret`, литералы): рекурсивный
   спуск в детей, чтобы ни один вложенный `NodeVariable` не остался непроверенным.

> Главное требование к `deepVisit`: **каждый дочерний `Node*` должен быть передан
> в `deepVisit`**. Если какой-то ребёнок пропущен — вложенное использование имени
> не проверится (тихая потеря 100). Поэтому для бинарных/унарных узлов в разделе 3
> заинлайнен явный `switch` по типам.

### 9.6. Семантика `resolve` (поиск имени)

```
для области от вершины стека до основания:
    если имя есть в этой области -> вернуть Symbol   // теневое перекрытие работает тут
если имя в builtins -> вернуть builtin_sym            // примитивные типы «всегда объявлены»
иначе -> nullptr                                      // => 100
```

- Пользовательское имя **перекрывает** builtin (builtins проверяются последними).
- `builtin_sym` — статическая-заглушка: настоящий `Symbol` не нужен, важен лишь
  факт «найдено», чтобы не выдать ложный 100 на `int`, `bool` и т.п. в аннотациях.

### 9.7. Когда 100, когда 101 — короткая матрица

| Ситуация | Код | Где фиксируется |
|---|---|---|
| Имя использовано, но нигде не видно (и не builtin) | **100** | `deepVisit` → `NodeVariable` → `resolve()==nullptr` |
| Два `let`/`fn`/параметра с одним именем в **одной** области | **101** | `declareLocal` / `collectGlobal` |
| Глобальный дубль (`let x` + `let x` на верхнем уровне) | **101** (ровно 1) | `collectGlobal` |
| Локальное имя совпадает с глобальным (затенение) | — (ок) | не 101, не 100 |
| `int`/`bool`/… в аннотации типа | — (ок) | `builtins` в `resolve` |

### 9.8. Инварианты и детерминизм

- Проход **идемпотентен** при однократном запуске (не мутирует AST).
- **Одна ошибка на проблемное имя**: глобальный дубль ловится один раз в
  `collectGlobal`; локальный — один раз в `declareLocal`.
- Порядок выдачи: сначала все глобальные 101 (из пред-прохода), затем 100 и
  локальные 101 (из `deepVisit`). Это даёт стабильный, предсказуемый вывод.

### 9.9. Для LSP / инкрементальности

- Этап оперирует **всем деревом документа за один прогон**. При инкрементальном
  редактировании (транспорт `incremental_changes` в `editor/lsp/protocol.py`)
  сервер **пересобирает AST целиком** и заново запускает `deep_checker()` — полный
  re-analysis дешевле, чем поддерживать инкрементальный кэш символов, и гарантирует
  корректность при любых правках. Результаты (AST, таблица имён, диагностики)
  кэшируются на стороне сервера для hover/go-to-def.

### 9.10. Почему этап именно такой (границы M1)

M1 намеренно **узкий**: только имена + области. Он НЕ делает:
- проверку совместимости типов (коды 140+, M8);
- разрешение `::` / `.` (квалификаторы, поля структур — M3);
- вывод типов (M8);
- анализ видимости `private`/`public` (M3).

Узость делает M1 тестируемым и даёт устойчивый фундамент: вся инфраструктура
(стек областей `scopes`, `collectGlobal`, `resolve`, `declareLocal`, обход всех
узлов) **переживёт** переход к M3/M8 — будут только углубляться семантика
`resolve` и обработка отдельных узлов.

### 9.11. Эволюция в M3 / M8 (как каркас превращается в полный анализ)

- **M3 (имена/разрешение):** `NodeBinaryScopeResolution` и `NodeMemberAccess`
  перестают «слепо» обходить правый операнд — вместо этого `resolve` начинает
  понимать квалификаторы (`A::b`, `obj.field`), появляется таблица членов
  структур/пространств, `private`/`public` начинают реально ограничивать доступ.
- **M8 (типы):** `Symbol` обогащается полем `Type*`; `resolve` возвращает
  типизированный символ; поверх `deepVisit` навешивается фаза проверки
  совместимости (коды 140+) и вывода типов. Сама структура обхода (стек областей,
  порядок регистрации имён) остаётся без изменений.

### 9.12. Краткая схема потока данных

```
                 ast (vector<Node*>) ──┐
                                        ├─► ASTDeepChecker::operator()
 error_container ◄──────────────────────┤     1) collectGlobal  -> scopes[0]
 (50–62 + 100/101)                      │     2) deepVisit(node) для каждого:
                                        │          push scope → declare имена
                                        │          → resolve(NodeVariable) → 100/101
                                        │          → pop scope
                                        ┘
```

Этот раздел — «что и зачем делает этап»; разделы 0–8 — «как именно реализовать».
Они должны оставаться согласованными: любое изменение логики здесь отражается в
коде разделов 2–4.
