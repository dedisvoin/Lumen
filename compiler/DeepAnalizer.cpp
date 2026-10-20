#include "ASTGenerator.cpp"

#include "core/Tokens.cpp"
#include "core/ErrorBase.cpp"

#include "runtime/Functions.cpp"
#include "runtime/Context.cpp"
#include <cmath>
#include <chrono>
#include <memory>
#include <algorithm>
#include <iostream>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

// Маркер value-generic структуры без конкретного значения: символический
// инстанс в сигнатуре метода (`self: Box[T, N]`, `-> Vec[T, Size]`). Реальные
// значения таких инстансов всегда конкретны, поэтому маркер ни с чем не
// сравнивается — он лишь заполняет слот, чтобы сохранить выравнивание аргументов.
static const long long SYMBOLIC_VALUE_GENERIC = -9223372036854775807LL - 1;


#include "core/InlayHint.cpp"

// Печать адреса указателя: «0x» + hex (нижний регистр): ptr(0x1000,i64) -> 0x1000.
static String formatAddr(uint64_t a) {
    char buf[2 + 16 + 1];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(a));
    return String(buf);
}

// РџРµС‡Р°С‚СЊ Р·РЅР°С‡РµРЅРёСЏ (РёСЃРїРѕР»СЊР·СѓРµС‚СЃСЏ echo/outln Рё РІР»РѕР¶РµРЅРѕ РґР»СЏ РјР°СЃСЃРёРІРѕРІ/РєРѕСЂС‚РµР¶РµР№).
static String formatValue(const Value& v) {
    if (v.T == Ty::Str)        return v.get<String>();
    if (v.T == Ty::Bool)       return v.get<bool>() ? "true" : "false";
    if (isNumberType(v.T)) {
        double d = getNumberFromType<>(v);
        if (d == std::floor(d) && std::fabs(d) < 1e15)
            return std::to_string(static_cast<long long>(d));
        return std::to_string(d);
    }
    if (v.T == Ty::Char) { String s; s += v.get<char32_t>(); return s; }
    if (v.T == Ty::Type_) return v.get<Type>().toString();
    if (v.T == Ty::Null)  return "null";
    if (v.T == Ty::Namespace) return "object(namespace)";
    if (v.T.isCallable()) { String s = "fn "; s += v.get<FunctionContainer>().name; return s; }
    if (v.T.isPointer())  return formatAddr(v.get<Pointer>().addr);
    if (v.T.isStruct()) {
        return "object(" + v.T.name + ")";
    }
    if (v.T.isEnum()) {
        return std::to_string(v.get<long long>());
    }
    if (v.T.isArray() || v.T.isTuple()) {
        String s = v.T.isArray() ? "{" : "[";
        const RuntimeArray& arr = v.get<RuntimeArray>();
        for (size_t i = 0; i < arr.view().size(); ++i) {
            if (i) s += ", ";
            s += formatValue(arr.view()[i]);
        }
        s += v.T.isArray() ? "}" : "]";
        return s;
    }
    return "";
}

// =========================== структуры ====================================
// Метаданные объявленной структуры (заполняются при NodeStructDeclaration,
// используются при материализации литералов и вызове методов).

struct StructFieldDef {
    String name;
    Token token;
    Node* type_node = nullptr;      // тип поля (nullptr = auto)
    Node* default_node = nullptr;   // дефолт-значение (nullptr = обязательное поле)
    ContextObject::Modifiers mods;
};

struct StructSymbol {
    String name;
    Token name_token;
    std::vector<GenericParam> generics;
    std::vector<StructFieldDef> fields;
    std::map<String, FunctionContainer> methods;   // методы (перегрузки по имени)
    Context decl_context;                          // лекс-окружение декларации (для дефолтов)
    Token decl_token;
    // имена static-полей (регистрируются как переменные в decl_context)
    std::vector<String> static_fields;
    // узел-декларация: повторное исполнение того же узла (структура внутри функции,
    // вызванной многократно) не считается переопределением
    Node* decl_node = nullptr;
    // `final struct X {}` — передекларация такой структуры запрещена
    bool final = false;

    // голый 'T' и 'T: Type' — тип-параметр; 'N: i64' — value-параметр
    bool is_type_generic(const GenericParam& gp) const {
        if (gp.type && gp.type->type == NodeTypes::NodeVariable)
            return static_cast<struct NodeVariable*>(gp.type)->name == "Type";
        return true;
    }
    bool has_field(const String& name) const {
        for (const auto& f : fields)
            if (f.name == name) return true;
        return false;
    }
    const StructFieldDef* field_def(const String& name) const {
        for (const auto& f : fields)
            if (f.name == name) return &f;
        return nullptr;
    }
};

// C++-стиль enum: набор именованных констант (последовательные целые).
// Варианты живут как переменные в enum_context — доступ через `Name::var`.
struct EnumSymbol {
    String name;
    Token name_token;
    Token decl_token;
    Context enum_context;   // вариант -> ContextObject{ EnumType(name), index }
    Node* decl_node = nullptr;
};

// Р‚СЂР°РЅСЃРїР°СЂРµРЅС‚РЅР°СЏ РїСЂРѕРІРµСЂРєР° В«С‡РёСЃС‚РѕС‚С‹В» РІРµС‚РєРё РґР»СЏ dry-РїСЂРѕРіРѕРЅР°:
    // РІРµС‚РєР° РЅРµ РѕР±Р»Р°РґР°РµС‚ РїРѕР±РѕС‡РЅС‹РјРё СЌС„С„РµРєС‚Р°РјРё (РЅРµ РїРёС€РµС‚ РІ РїРµСЂРµРјРµРЅРЅС‹Рµ,
    // РЅРµ РѕР±СЉСЏРІР»СЏРµС‚ РёРґРµРЅС‚РёС„РёРєР°С‚РѕСЂС‹, РЅРµ РІС‹Р·С‹РІР°РµС‚ С„СѓРЅРєС†РёРё, РЅРµ РїРµС‡Р°С‚Р°РµС‚).
    // Р”Р»СЏ С‚Р°РєРёС… РІРµС‚РѕРє dry-РїСЂРѕРіРѕРЅ РЅРµ С‚СЂРµР±СѓРµС‚ РєРѕРїРёРё РєРѕРЅС‚РµРєСЃС‚Р°:
    // РІ РѕР±СЂР°С‚РЅРѕРј СЃР»СѓС‡Р°Рµ РІРЅСѓС‚СЂРё С†РёРєР»Р° (РїРѕРёСЃРє РІ Dict::contain) РіР»СѓР±РѕРєР°СЏ
    // РєРѕРїРёСЏ СЂР°СЃС‚СѓС‰РµРіРѕ РєРѕРЅС‚РµР№РЅРµСЂР° РґР°С‘С‚ O(N³) Рё Р°РЅР°Р»РёР·Р°С‚РѕСЂ В«Р·Р°РІРёСЃР°РµС‚В» РЅР° 100+ РёС‚РµСЂР°С†РёСЏС….
    static bool deadBranchIsPure(Node* n, int depth = 0) {
        if (!n) return true;
        if (depth > 64) return false;   // слишком глубокий граф — консервативно: копируем как раньше
        switch (n->type) {
            // литералы, переменные, лёгкие операции — читают контекст, но не меняют его
            case NodeTypes::NodeNumber:
            case NodeTypes::NodeString:
            case NodeTypes::NodeBool:
            case NodeTypes::NodeNull:
            case NodeTypes::NodeVariable:
                return true;
            case NodeTypes::NodeScope:
                return deadBranchIsPure(static_cast<struct NodeScope*>(n)->node, depth + 1);
            case NodeTypes::NodeStatementsScope: {
                for (auto* c : static_cast<struct NodeStatementsScope*>(n)->nodes)
                    if (!deadBranchIsPure(c, depth + 1)) return false;
                return true;
            }
            // поток: выставляет control_flow/return_value — они сохраняются/восстанавливаются
            case NodeTypes::NodeReturn:
                return deadBranchIsPure(static_cast<struct NodeReturn*>(n)->expr, depth + 1);
            case NodeTypes::NodeBreak:
            case NodeTypes::NodeContinue:
                return true;
            // вложенный if: для чистоты нужна чистота условия И обеих веток
            case NodeTypes::NodeIf: {
                auto* i = static_cast<struct NodeIf*>(n);
                return deadBranchIsPure(i->condition, depth + 1)
                    && deadBranchIsPure(i->then_body, depth + 1)
                    && deadBranchIsPure(i->else_body, depth + 1);
            }
            case NodeTypes::NodeUnaryMinus:
            case NodeTypes::NodeUnaryPlus:
            case NodeTypes::NodeUnaryNegation:
            case NodeTypes::NodeUnaryDereference:
            case NodeTypes::NodeUnaryBitwiseNot:
            case NodeTypes::NodeUnaryAddressOf:
                return deadBranchIsPure(static_cast<struct NodeUnaryMinus*>(n)->node, depth + 1);
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
              case NodeTypes::NodeBinaryLogicalOr:
              case NodeTypes::NodeBinaryNullCoalesce: {
                auto* b = static_cast<struct NodeBinaryAdditive*>(n);
                return deadBranchIsPure(b->l_node, depth + 1)
                    && deadBranchIsPure(b->r_node, depth + 1);
            }
            case NodeTypes::NodeBinaryScopeResolution: {
                auto* b = static_cast<struct NodeBinaryScopeResolution*>(n);
                return deadBranchIsPure(b->l_node, depth + 1)
                    && deadBranchIsPure(b->r_node, depth + 1);
            }
            case NodeTypes::NodeCast: {
                auto* c = static_cast<struct NodeCast*>(n);
                return deadBranchIsPure(c->l_node, depth + 1)
                    && deadBranchIsPure(c->r_node, depth + 1);
            }
            case NodeTypes::NodeMemberAccess: {
                auto* m = static_cast<struct NodeMemberAccess*>(n);
                return deadBranchIsPure(m->l_node, depth + 1)
                    && deadBranchIsPure(m->r_node, depth + 1);
            }
            case NodeTypes::NodeArrayAccess: {
                auto* a = static_cast<struct NodeArrayAccess*>(n);
                if (!deadBranchIsPure(a->base, depth + 1)) return false;
                for (auto* ix : a->indices)
                    if (!deadBranchIsPure(ix, depth + 1)) return false;
                return true;
            }
            case NodeTypes::NodeArray: {
                for (auto* e : static_cast<struct NodeArray*>(n)->elements)
                    if (!deadBranchIsPure(e, depth + 1)) return false;
                return true;
            }
            case NodeTypes::NodeTypeOf:
                return deadBranchIsPure(static_cast<struct NodeTypeOf*>(n)->expr, depth + 1);
            case NodeTypes::NodeMod:
                return deadBranchIsPure(static_cast<struct NodeMod*>(n)->expr, depth + 1);
            case NodeTypes::NodePostfixUnwrap:
                return deadBranchIsPure(static_cast<struct NodePostfixUnwrap*>(n)->node, depth + 1);
            case NodeTypes::NodeSizeOf:
                return deadBranchIsPure(static_cast<struct NodeSizeOf*>(n)->expr, depth + 1);
            case NodeTypes::NodePtr:
                return deadBranchIsPure(static_cast<struct NodePtr*>(n)->addr, depth + 1)
                    && deadBranchIsPure(static_cast<struct NodePtr*>(n)->type, depth + 1);
            case NodeTypes::NodeAssert:
                return deadBranchIsPure(static_cast<struct NodeAssert*>(n)->check, depth + 1);
            case NodeTypes::NodeExit:
                return deadBranchIsPure(static_cast<struct NodeExit*>(n)->expr, depth + 1);
            case NodeTypes::NodeAppend: {
                auto* an = static_cast<struct NodeAppend*>(n);
                return deadBranchIsPure(an->array_expr, depth + 1)
                    && deadBranchIsPure(an->value_expr, depth + 1);
            }
            case NodeTypes::NodeReserve: {
                auto* rn = static_cast<struct NodeReserve*>(n);
                return deadBranchIsPure(rn->array_expr, depth + 1)
                    && deadBranchIsPure(rn->count_expr, depth + 1);
            }
            default:
                return false;
        }
    }

    static String memberNameNode(Node* r) {
        if (r && r->type == NodeTypes::NodeVariable)
            return static_cast<struct NodeVariable*>(r)->name;
        return String();
    }

struct ASTDeepAnalyzer {
    AbstractTreeT ast;
    ErrorContainer& error_container;

    Context global_context;
    VirtualHeap heap;
    // СЏС‡РµР№РєРё, СЃРѕР·РґР°РЅРЅС‹Рµ РѕРїРµСЂР°С‚РѕСЂРѕРј `new`, Р¶РёРІСѓС‚ РІ РєСѓС‡Рµ РІРїР»РѕС‚СЊ РґРѕ РєРѕРЅС†Р° Р°РЅР°Р»РёР·Р°;
    // unique_ptr РґР°С‘С‚ РёРј СЃС‚Р°Р±РёР»СЊРЅС‹Рµ Р°РґСЂРµСЃР° (std::map/РІРµРєС‚РѕСЂ-realloc СЃРґРІРёРЅСѓР»Рё Р±С‹ РёС…).
    std::vector<std::unique_ptr<ContextObject>> new_cells;
    std::vector<std::unique_ptr<Context>> ctx_pool; // call_ctx/frame alive as long as analyzer (heap may point into them)
    bool aborted = false;

    // СЏРІРЅРѕРµ `exit <code>;` в интерпретаторе (-r): после прогона lumenc вернёт
    // этот код процесса. В deep-анализе (lint/LSP) exit не исполняется вовсе.
    bool exit_requested = false;
    int exit_code = 0;

    // СѓРїСЂР°РІР»РµРЅРёРµ РїРѕС‚РѕРєРѕРј (break / continue / return / exit)
    enum class ControlFlow { None, Break_, Continue_, Return_, Exit_ };
    ControlFlow control_flow = ControlFlow::None;
    Value return_value = new_null();
    int loop_depth = 0;
    // >0 — мы внутри ветки, статически недостижимой из-за охраны вида
    // `typeof(x) == T` / `typeis(x) == T` (T не входит в статический тип x).
    // В таких ветках `ret` не может нарушить контракт => RETURN_TYPE_MISMATCH
    // не выдаём (зеркало их недостижимости; без этого generic-методы с
    // рантайм-диспетчеризацией по typeof ложно падают).
    int supp_ret_mismatch = 0;
    int iter_budget = 10000;        // РѕР±С‰РёР№ Р±СЋРґР¶РµС‚ РёС‚РµСЂР°С†РёР№ РЅР° РЅР°Р±РѕСЂ РІР»РѕР¶РµРЅРЅС‹С… С†РёРєР»РѕРІ
    bool budget_exhausted = false;
    // Липкий флаг: срабатывал ли хоть раз бюджет итераций/вызовов за весь прогон.
    // budget_exhausted/call_budget_exhausted сбрасываются на каждом цикле/функции,
    // поэтому для внешних потребителей (напр. -bench, где важно знать, что
    // интерпретатор оборвал программу) нужен отдельный невосстанавливаемый признак.
    bool any_budget_hit = false;
    int call_depth = 0;             // РіР»СѓР±РёРЅР° РІС‹Р·РѕРІРѕРІ С„СѓРЅРєС†РёР№ (Р·Р°С‰РёС‚Р° РѕС‚ Р±РµСЃРєРѕРЅРµС‡РЅРѕР№ СЂРµРєСѓСЂСЃРёРё)
    // Бюджет вызовов функций за один top-level оператор: глубокий анализ —
    // best-effort, дорогие вычисления (напр. наивная рекурсия fib(30) ~2.7M
    // вызовов) обрезаются с возвратом нейтрального значения объявленного типа.
    // Типы при этом выводятся корректно, реальное значение считает рантайм.
    int call_budget = 0;
    bool call_budget_exhausted = false;
    bool call_budget_warned = false;   // предупреждение о сработавшем бюджете вызовов — один раз
    bool print_input_prompts = false;  // -r: печатать приглашение input() в stdout (без блокировки на stdin)

    // -r: исполняем программу интерпретатором и выводим результат. В этом режиме
    // внешние C++-функции (extern fn / #cppinclude) вызвать нельзя — даём ошибку.
    bool interpret_mode = false;

    // LSP: собрать только символьные таблицы (structs/enums/globals), НЕ
    // прокручивая тела циклов. Редактор не должен зависеть от времени работы
    // программы: типы/хинты считает TypeAnalyzer, а циклы нужны только для
    // побочных эффектов, которых анализ не требует.
    bool symbols_only = false;

    // имена уже заявленных extern-функций (дубликаты — ошибка)
    std::vector<String> extern_fns;
    static const int CALL_BUDGET_LIMIT = 50000;

    // M8: драйвер `eval { ... }`. eval_only — исполнить только собранные
    // top-level eval-блоки (+ объявления), не весь файл. in_eval/eval_budget_shared
    // включают семантику блока: запреты (input/extern) и общий per-block бюджет
    // итераций (сбрасывается один раз на блок, не на каждый цикл).
    std::vector<Node*> eval_blocks;
    bool eval_only = false;
    bool in_eval = false;
    bool eval_budget_shared = false;
    static const int EVAL_LOOP_BUDGET = 10000;
    // Граница dry-прогона мёртвой ветки `if` (кадр scratch). Записи в переменные,
    // которые разрешаются ВЫШЕ этого кадра (в живой цепочке parent), сохранять
    // нельзя: мёртвая ветка исполняется только для сбора ошибок, а её эффекты
    // должны отбрасываться. nullptr — не в dry-прогоне (или чистая ветка).
    Context* dry_frame = nullptr;
    bool dryAllowsWrite(Context& ctx, const String& name) const {
        if (!dry_exec) return true;
        for (Context* c = &ctx; c; c = c->parent) {
            if (c->variables.find(name) != c->variables.end()) return true;
            if (c == dry_frame) return false;
        }
        return false;
    }
    bool dry_exec = false;          // РІРЅСѓС‚СЂРё В«СЃСѓС…РѕРіРѕВ» РїСЂРѕРіРѕРЅР° РјС‘СЂС‚РІРѕР№ РІРµС‚РєРё (С‚РµР»Р° РІС‹Р·РѕРІРѕРІ РЅРµ РёСЃРїРѕР»РЅСЏРµРј)
    long long prof_exec_count = 0;  // временно: счётчик вызовов execute (профилирование)
    bool prof_enabled = false;
    long long prof_node_count[128] = {0};
    long long prof_node_time[128] = {0};
    long long prof_copy_count = 0;
    long long prof_pure_count = 0;  // Головой-счётчик: простые мёртвые ветки (fast-path, без копии)  // временно: счётчик контекстных копий в dry-ветке if
    // Токен вызова и его конец: assert внутри функции сообщает о нарушенном
    // контракте на месте вызова, а не в теле. Храним пару, чтобы подсветка
    // покрывала весь вызов (`a / b`, `foo(x)`), а не один токен.
    Token* current_call_site = nullptr;
    Token* current_call_site_end = nullptr;
    static const int CALL_DEPTH_LIMIT = 32;
    // ref-alias chain depth guard: `ref let a = b; ...` — infinite recursion on a cycle
    int ref_chain_depth = 0;
    bool ref_cycle_reported = false;
    static const int REF_CHAIN_LIMIT = 256;
    void refCycleError(const Token& tok, const String& name) {
        if (ref_cycle_reported) return;
        ref_cycle_reported = true;
        error_container.addError(DEEP_ERRORS::REF_CYCLE(tok, name));
        aborted = true;
    }
    Type current_ret_type = Ty::Any; // РґРµРєР»Р°СЂРёСЂРѕРІР°РЅРЅС‹Р№ '-> С‚РёРї' РІС‹РїРѕР»РЅСЏРµРјРѕР№ С„СѓРЅРєС†РёРё (Ty::Any = РЅРµ РїСЂРѕРІРµСЂСЏРµРј)

    // ref-функции ('-> ref T'): механизм живых ссылок.
    // current_returns_ref — тело текущей (внутренней) функции возвращает ссылку.
    // writePathValue(NodeFunctionCall) откладывает write-back: повторно исполняет
    // вызов, и его `return <lvalue>` в write-режиме пишет ref_write_val в цель.
    bool current_returns_ref = false;
    bool current_write_active = false;      // именно этот вызов должен записать значение
    bool ref_write_armed = false;           // write-back отложен
    Value ref_write_val = new_null();       // значение для write-back
    struct NodeFunctionCall* ref_write_rcall = nullptr;   // вызов-адресат
    bool ref_write_consumed = false;        // return реально выполнил запись
    // ref-параметры текущей функции ('ref self', 'ref x'): их имена в теле — это
    // живые ссылки, а не значения. Нужно, чтобы `ret` value-функции не пропускал
    // возврат реф-значения (VALUE_RETURNS_REF).
    std::set<String> current_ref_params;

    // РёРЅР»Р°Р№РЅ-С…РёРЅС‚С‹ (РЅР°РїСЂ. РІС‹РІРµРґРµРЅРЅС‹Р№ С‚РёРї РґР»СЏ auto)
    std::vector<InlayHintData> inlay_hints;
    // P8: декл-позиция локальной (line<<32|index) -> полный реифицированный тип.
    // LSP-цепочки (`lb.at(0).`) реифицируют generic-возврат метода по нему.
    std::map<unsigned long long, Type> local_types;

    // Решения резолва перегрузок: узел вызова (NodeFunctionCall) -> узел объявления
    // выбранной функции. Заполняется здесь, на настоящих Type (Type::operator>>),
    // и читается кодогеном: он берёт ту же перегрузку, а не подбирает свою по строковым
    // тегам. Мест, которые анализатор не обошёл (тело невызванной функции, -noanalize),
    // в карте нет — там кодоген откатывается на свой fallback.
    std::map<Node*, Node*> resolved_calls;

    // счётчик уникальных скрытых переменных для dunder-вызовов (перегрузка операторов)
    int dunder_temp = 0;

    // scope-анализ указателей (P2): адрес function-local ячейки -> скоуп функции,
    // в которой она объявлена. Указатель/Ref, захваченный через '&local' или из
    // ref-параметра, обязан НЕ покидать скоуп владельца (рантайм целится в ячейку
    // фрейма функции, которая умирает на return).
    int fn_scope_seq = 0;
    int current_fn_scope = 0;              // 0 = глобальный контекст (top-level)
    std::map<uint64_t, int> local_owner;   // addr локальной ячейки -> скоуп владельца

    // идёт ли в дереве Значения указатель на локаль, чей скоуп == scope
    bool escapedLocalPtr(Value v, int scope) {
        if (v.T.isPointer()) {
            auto it = local_owner.find(v.get<Pointer>().addr);
            return it != local_owner.end() && it->second == scope;
        }
        if (v.T.isStruct()) {
            const RuntimeStruct& rs = v.get_ref<RuntimeStruct>();
            for (const auto& kv : rs.view())
                if (escapedLocalPtr(kv.second, scope)) return true;
            return false;
        }
        if (v.T.isArray() || v.T.isTuple()) {
            const RuntimeArray& ar = v.get_ref<RuntimeArray>();
            for (const auto& e : ar.view())
                if (escapedLocalPtr(e, scope)) return true;
            return false;
        }
        return false;
    }

    // РЅР°РєРѕРїР»РµРЅРЅС‹Рµ РјРѕРґРёС„РёРєР°С‚РѕСЂС‹ РїСЂРё СЃРїСѓСЃРєРµ РїРѕ С†РµРїРѕС‡РєР°Рј/Р±Р»РѕРєР°Рј (const strict { ... })
    ContextObject::Modifiers active_mods{};
    bool active_global = false;
    // порядок написания модификаторов (`final strict`), сохраняется в
    // ContextObject::Modifiers.order для hover/автодополнения
    std::vector<String> modifier_order;

    // реестр объявленных структур: имя -> символ (ноды std::map стабильны,
    // указатели на structs[name].decl_context и методы не инвалидируются)
    std::map<String, StructSymbol> structs;

    // реестр объявленных enum'ов: имя -> символ (константы вариантов)
    std::map<String, EnumSymbol> enums;

    // структура, чей метод/статик сейчас выполняется (для private-доступа)
    const StructSymbol* active_struct = nullptr;
    // ресивер текущего const-метода: запись-в-него запрещена (CONST_FN_MUTATION)
    ContextObject* const_receiver = nullptr;

    // Слоты полей для `&obj.field`: виртуальный адрес -> (владелец-объект, имя
    // поля). Поля структур хранятся как Value внутри RuntimeStruct у владельца,
    // поэтому разыменование слота читает/пишет поле напрямую. Адреса слотов
    // выделяются из области, не пересекающейся с VirtualHeap (малые номера).
    std::unordered_map<uint64_t, std::pair<ContextObject*, String>> field_slots;
    std::map<std::pair<ContextObject*, String>, uint64_t> field_slot_rev;
    uint64_t field_slot_next = (uint64_t(1) << 60);

    // Символ структуры по инстанс-типу (nullptr для не-структур/неизвестных).
    const StructSymbol* structSymForType(const Type& t) const {
        if (!t.isStruct()) return nullptr;
        auto it = structs.find(t.name);
        if (it == structs.end()) return nullptr;
        return &it->second;
    }

    // Привязки generic-имён из инстанс-типа: тип-аргументы -> new_type, значения -> i64.
    bool bindingsForType(const StructSymbol& sym, const Type& inst,
                         std::vector<Value>& out) const {
        if (inst.name != sym.name) return false;
        size_t ti = 0, vi = 0;
        for (size_t i = 0; i < sym.generics.size(); ++i) {
            if (inst.arg_mask & (1u << static_cast<unsigned>(i))) {
                if (ti >= inst.container_1.size()) return false;
                out.push_back(new_type(inst.container_1[ti]));
                ti++;
            } else {
                if (vi >= inst.int_args.size()) return false;
                out.push_back(new_i64(inst.int_args[vi]));
                vi++;
            }
        }
        return true;
    }

    // Reified-тип поля под конкретным инстансом (T -> reified, Size -> константа).
    Type fieldTypeFor(const StructSymbol& sym, const Type& inst,
                      const String& field, const StructFieldDef** def_out = nullptr) {
        const StructFieldDef* fd = sym.field_def(field);
        if (!fd || !fd->type_node) return Ty::Any;
        std::vector<Value> bindings;
        if (!bindingsForType(sym, inst, bindings)) return Ty::Any;
        Context bind_ctx(ContextType::FunctionT);
        bind_ctx.parent = const_cast<Context*>(&sym.decl_context);
        for (size_t i = 0; i < sym.generics.size() && i < bindings.size(); ++i) {
            const GenericParam& gp = sym.generics[i];
            ContextObject co(gp.name, bindings[i], Ty::Null);
            co.is_type_decl = sym.is_type_generic(gp);
            bind_ctx.add_variable(gp.name, co);
        }
        if (def_out) *def_out = fd;
        return evalTypeExpr(fd->type_node, bind_ctx);
    }

    // Статический тип выражения для `typeis(...)` — симметрично `typeof`, но берётся
    // объявленный (reified) тип, а не рантайм-тип значения:
    //   * переменная/параметр -> ContextObject::wait_type (аннотация), иначе тип значения;
    //   * поле структуры      -> объявленный тип поля с учётом генериков инстанса;
    //   * иначе               -> тип значения (fallback, как у typeof).
    Type staticTypeOfExpr(Node* e, Context& context) {
        Node* n = e;
        while (n && n->type == NodeTypes::NodeScope)
            n = static_cast<struct NodeScope*>(n)->node;
        if (n && n->type == NodeTypes::NodeVariable) {
            auto* v = static_cast<struct NodeVariable*>(n);
            if (context.has_variable(v->name)) {
                ContextObject& co = context.get_variable(v->name);
                if (co.ref_alias_node)
                    return staticTypeOfExpr(co.ref_alias_node,
                        co.ref_alias_ctx ? *co.ref_alias_ctx : context);
                if (!(co.wait_type == Ty::Null) && !co.wait_type.is_auto)
                    return co.wait_type;
                return co.value.T;
            }
        }
        if (n && n->type == NodeTypes::NodeMemberAccess) {
            auto* ma = static_cast<struct NodeMemberAccess*>(n);
            String field = memberNameNode(ma->r_node);
            Value base = execute(ma->l_node, context);
            if (base.T.isStruct() && !field.empty()) {
                const StructSymbol* sym = structSymForType(base.T);
                if (sym) {
                    const StructFieldDef* fd = nullptr;
                    Type ft = fieldTypeFor(*sym, base.T, field, &fd);
                    if (fd && fd->type_node) return ft;
                }
            }
        }
        return execute(e, context).T;
    }

    // Значение модификатора по имени для набора флагов объявления.
    static bool modifierFlag(const ContextObject::Modifiers& m, const String& name) {
        if (name == "static")  return m.Static;
        if (name == "const")   return m.Const;
        if (name == "final")   return m.Final;
        if (name == "strict")  return m.Strict;
        if (name == "private") return m.Private;
        if (name == "ref")     return m.Ref;
        if (name == "public")  return !m.Private;
        return false;
    }

    // mod(expr, modifier) — проверяет, установлен ли модификатор у объявления,
    // на которое указывает expr (переменная или поле структуры). Результат
    // компиляционного времени: сохраняется в узле для кодогенератора.
    void resolveMod(struct NodeMod* nm, Context& context) {
        nm->resolved = false;
        nm->value = false;
        Node* e = nm->expr;
        while (e && e->type == NodeTypes::NodeScope)
            e = static_cast<struct NodeScope*>(e)->node;
        if (!e) return;
        if (e->type == NodeTypes::NodeVariable) {
            auto* v = static_cast<struct NodeVariable*>(e);
            if (context.has_variable(v->name)) {
                ContextObject& co = context.get_variable(v->name);
                nm->resolved = true;
                nm->value = modifierFlag(co.modifiers, nm->modifier);
                // ref-параметр: модификаторы принадлежат АРГУМЕНТУ в месте вызова,
                // а не объявлению параметра (binding кладёт их в modifiers, а
                // кодогенератор берёт их из mods переданного значения).
                if (nm->modifier == "ref" && current_ref_params.count(v->name))
                    nm->dynamic = true;
            }
            return;
        }
        if (e->type == NodeTypes::NodeMemberAccess) {
            auto* ma = static_cast<struct NodeMemberAccess*>(e);
            String field = memberNameNode(ma->r_node);
            Value base = execute(ma->l_node, context);
            if (base.T.isStruct() && !field.empty()) {
                const StructSymbol* sym = structSymForType(base.T);
                if (sym) {
                    for (const auto& f : sym->fields) {
                        if (f.name == field) {
                            nm->resolved = true;
                            nm->value = modifierFlag(f.mods, nm->modifier);
                            return;
                        }
                    }
                }
            }
        }
    }

    static bool typeMayBe(const Type& S, const Type& T) {
        if (S.is_any || S.is_auto || T.is_any || T.is_auto) return true;
        if (S.isUnion()) {
            for (const auto& m : S.container_1)
                if (m == T || (T << m)) return true;
            return false;
        }
        if (S == T) return true;
        if (T << S) return true;
        if (S << T) return true;
        return false;
    }

    // Разбор охраны `typeof(x) == T` / `typeis(x) == T` (и `!=`). Возвращает:
    //   0 — обе ветки достижимы; 1 — ветка then статически недостижима;
    //   2 — ветка else статически недостижима.
    int deadBranchByTypeGuard(Node* cond, Context& context) {
        auto unwrap = [](Node* n) {
            while (n && n->type == NodeTypes::NodeScope)
                n = static_cast<struct NodeScope*>(n)->node;
            return n;
        };
        Node* c = unwrap(cond);
        if (!c) return 0;
        bool neg = false;
        Node *l = nullptr, *r = nullptr;
        if (c->type == NodeTypes::NodeBinaryEqual) {
            l = static_cast<struct NodeBinaryEqual*>(c)->l_node;
            r = static_cast<struct NodeBinaryEqual*>(c)->r_node;
        } else if (c->type == NodeTypes::NodeBinaryNotEqual) {
            neg = true;
            l = static_cast<struct NodeBinaryNotEqual*>(c)->l_node;
            r = static_cast<struct NodeBinaryNotEqual*>(c)->r_node;
        } else {
            return 0;
        }
        l = unwrap(l); r = unwrap(r);
        Node* te = nullptr;   // typeof/typeis
        Node* ty = nullptr;   // тип-выражение
        if (l && l->type == NodeTypes::NodeTypeOf) { te = l; ty = r; }
        else if (r && r->type == NodeTypes::NodeTypeOf) { te = r; ty = l; }
        else return 0;
        if (!ty) return 0;
        auto* tn = static_cast<struct NodeTypeOf*>(te);
        Type S = staticTypeOfExpr(tn->expr, context);
        // Тип-выражение справа: сперва как значение (builtin-имена/генерики
        // резолвятся через execute), иначе — статический разбор типа.
        Type T = Ty::Any;
        {
            Value tv = execute(ty, context);
            if (tv.T == Ty::Type_) T = tv.get<Type>();
            else T = evalTypeExpr(ty, context);
        }
        if (S.is_any || S.is_auto || T.is_any || T.is_auto) return 0;
        bool possible = tn->is_static ? (S == T) : typeMayBe(S, T);
        bool strictEqual = !S.isUnion() && (S == T);
        bool thenDead, elseDead;
        if (!neg) { thenDead = !possible; elseDead = strictEqual; }
        else      { thenDead = strictEqual; elseDead = !possible; }
        if (thenDead) return 1;
        if (elseDead) return 2;
        return 0;
    }

    ASTDeepAnalyzer(AbstractTreeT ast, ErrorContainer& error_container) :
    ast(ast), error_container(error_container) {}

    // Р РµР·СѓР»СЊС‚РёСЂСѓСЋС‰РёР№ (Р±РѕР»РµРµ СѓР·РєРёР№ РёР· РґРІСѓС…) С‚РёРї СЃРѕРіР»Р°СЃРЅРѕ РїСЂР°РІРёР»Р°Рј РїРѕРІС‹С€РµРЅРёСЏ:
    //   int+int     -> С€РёСЂРµ РёР· РґРІСѓС…   (i8<i16<i32<i64)
    //   float+float -> С€РёСЂРµ float      (f32<f64)
    //   int+float   -> С‚РёРї float
    Type resultTypeOf(const Type& a, const Type& b) const {
        auto intRank = [](const Type& t) -> int {
            if (t == Ty::i8)  return 1;
            if (t == Ty::i16) return 2;
            if (t == Ty::i32) return 3;
            if (t == Ty::i64) return 4;
            return 0;
        };
        auto fltRank = [](const Type& t) -> int {
            if (t == Ty::f16) return 1;
            if (t == Ty::f32) return 2;
            if (t == Ty::f64) return 3;
            return 0;
        };
        bool af = (a == Ty::f16 || a == Ty::f32 || a == Ty::f64);
        bool bf = (b == Ty::f16 || b == Ty::f32 || b == Ty::f64);
        if (af || bf) {
            if (af && bf) return (fltRank(a) >= fltRank(b)) ? a : b;
            return af ? a : b;   // int + float -> С‚РёРї float
        }
        return (intRank(a) >= intRank(b)) ? a : b;
    }


    bool isZeroValue(const Value& v) const {
        if (v.T == Ty::i8  || v.T == Ty::i16 || v.T == Ty::i32 || v.T == Ty::i64 ||
            v.T == Ty::f16 || v.T == Ty::f32 || v.T == Ty::f64) {
            return getNumberFromType<double>(v) == 0.0;
        }
        return false;
    }

    // Р”РёР°РіРЅРѕСЃС‚РёРєР° РЅРµСЂР°Р·СЂРµС€С‘РЅРЅС‹С… РёРјС‘РЅ-С‚РёРїРѕРІ РІ return-С‚РёРїРµ С„СѓРЅРєС†РёРё РїСЂРё РґРµРєР»Р°СЂР°С†РёРё:
    // РёРјСЏ РґРѕР»Р¶РЅРѕ Р±С‹С‚СЊ РіРµРЅРµСЂРёРє-РїР°СЂР°РјРµС‚СЂРѕРј С„СѓРЅРєС†РёРё, "auto"/"any", Р»РёР±Рѕ СЃСѓС‰РµСЃС‚РІСѓСЋС‰РµР№
    // РїРµСЂРµРјРµРЅРЅРѕР№-С‚РёРїРѕРј (РІРєР»СЋС‡Р°СЏ РІСЃС‚СЂРѕРµРЅРЅС‹Рµ Р°Р»РёР°СЃС‹ Bool/Type/Str/...). РРЅР°С‡Рµ вЂ” РѕС€РёР±РєР°.
    void validateReturnTypeNames(Node* n, const Function& f, Context& ctx) {
        if (!n) return;
        switch (n->type) {
            case NodeTypes::NodeVariable: {
                auto* vn = static_cast<struct NodeVariable*>(n);
                const String& nm = vn->name;
                if (isGenericName(nm, f)) return;
                if (nm == "auto" || nm == "any") return;
                if (ctx.has_variable(nm)) {
                    ContextObject& co = ctx.get_variable(nm);
                    if (co.value.T == Ty::Type_) return;
                }
                error_container.addError(DEEP_ERRORS::UNDEFINED_VARIABLE(vn->token, nm));
                return;
            }
            case NodeTypes::NodeUnaryDereference:
                validateReturnTypeNames(static_cast<struct NodeUnaryDereference*>(n)->node, f, ctx);
                return;
              case NodeTypes::NodeBinaryBitwiseOr:
              case NodeTypes::NodeBinaryLogicalOr:
              case NodeTypes::NodeBinaryNullCoalesce: {
                  auto* bn = static_cast<struct NodeBinaryBitwiseOr*>(n);
                  validateReturnTypeNames(bn->l_node, f, ctx);
                  validateReturnTypeNames(bn->r_node, f, ctx);
                  return;
              }
            case NodeTypes::NodeArrayType: {
                auto* at = static_cast<struct NodeArrayType*>(n);
                validateReturnTypeNames(at->element_type, f, ctx);
                if (at->count && at->count->type == NodeTypes::NodeVariable)
                    validateReturnTypeNames(at->count, f, ctx);
                return;
            }
            case NodeTypes::NodeTupleType:
                for (auto* t : static_cast<struct NodeTupleType*>(n)->types)
                    validateReturnTypeNames(t, f, ctx);
                return;
            default:
                return;
        }
    }


    void initContext() {
        struct BV { const char* n; Type t; };
        const BV builtins[] = {
            {"i8", Ty::i8}, {"i16", Ty::i16}, {"i32", Ty::i32},
            {"i64", Ty::i64}, {"f16", Ty::f16}, {"f32", Ty::f32}, {"f64", Ty::f64},
            {"bool", Ty::Bool}, {"char", Ty::Char}, {"str", Ty::Str},
            {"any", Ty::Any}, {"auto", Ty::Auto}, {"Type", Ty::Type_},
            {"Namespace", Ty::Namespace}, {"Null", Ty::Null},
        };
        for (auto& b : builtins) {
            ContextObject co(String(b.n), new_type(b.t), Ty::Type_);
            co.is_type_decl = true;
            global_context.add_variable(String(b.n), co);
        }
    }

    // Общий тип элемента результата конкатенации массивов (lhs-тип + элементы).
    // Значим только container_1: декларированный тип левого операнда сохраняется,
    // если в него укладываются все элементы результата, иначе — объединение типов.
    // Используется и binaryMath (полная конкатенация, копия), и in-place `+=`
    // (тот же результат без копирования) — чтобы типы не расходились.
    Type arrayConcatElemType(const Type& ltype, const std::vector<Value>& all) {
        Type elem = Ty::Any;
        if (ltype.isArray() && !ltype.container_1.empty() && !ltype.container_1[0].isAny()) {
            bool all_fit = true;
            for (const auto& v : all) {
                if (!(v.T >> ltype.container_1[0])) { all_fit = false; break; }
            }
            if (all_fit && !all.empty()) elem = ltype.container_1[0];
        }
        for (const auto& v : all) {
            if (elem.isAny()) elem = v.T;
            else if (!(elem == v.T)) elem = elem | v.T;
        }
        return elem;
    }

    Value binaryMath(Value& l, Value& r, char op, const Token& start, const Token& end) {
        // 'str * n' / 'char * n' вЂ” РїРѕРІС‚РѕСЂ СЃС‚СЂРѕРєРё/СЃРёРјРІРѕР»Р° n СЂР°Р· (n вЂ” РЅРµРґСЂРѕР±РЅРѕРµ С‡РёСЃР»Рѕ)
        if (op == '*' && (l.T == Ty::Str || l.T == Ty::Char)) {
            if (!isIntegerType(r.T)) {
                error_container.addError(DEEP_ERRORS::UNSUPPORTED_OPERATOR(start, end, String("*"), l.T.toString(), r.T.toString()));
                return new_null();
            }
            int64_t n = getNumberFromType<int64_t>(r);
            if (n < 0) {
                error_container.addError(DEEP_ERRORS::STRING_REPEAT_NEGATIVE(start, end));
                return new_null();
            }
            String s = (l.T == Ty::Str) ? l.get<String>() : String::RepeatFrom(l.get<char32_t>(), 1);
            String out;
            out.reserve(s.size() * (size_t)n);
            for (int64_t i = 0; i < n; ++i) out += s;
            return new_string(out);
        }
        // Неявная арифметика с char запрещена: 'char +/- integer' и 'char - char' больше
        // не приводятся неявно (раньше 'c + 1' молча давало 'b'). Нужен явный перевод:
        // '((c as i64) + 1) as char'. Строковые операции ('str + char', 'char * n') не затронуты.
        if ((op == '+' || op == '-') && (l.T == Ty::Char || r.T == Ty::Char)) {
            bool lc = (l.T == Ty::Char), rc = (r.T == Ty::Char);
            if ((op == '-' && lc && rc) || (lc && isIntegerType(r.T)) || (rc && isIntegerType(l.T) && op == '+')) {
                error_container.addError(DEEP_ERRORS::CHAR_ARITHMETIC(
                    start, end, String(static_cast<char32_t>(op)), l.T.toString(), r.T.toString()));
                return new_null();
            }
            // иначе -> не наша комбинация (char + str, bool - char, ...), идём дальше
        }
        // 'str + str' / 'str + char' / 'char + str' / 'char + char' вЂ” РєРѕРЅРєР°С‚РµРЅР°С†РёСЏ.
        // РЎРўР РћР“Рћ: РѕР±Рµ СЃС‚РѕСЂРѕРЅС‹ РѕР±СЏР·Р°РЅС‹ Р±С‹С‚СЊ String/Char; 'str + С‡РёСЃР»Рѕ' вЂ” РѕС€РёР±РєР°.
        if (op == '+' && (l.T == Ty::Str || l.T == Ty::Char || r.T == Ty::Str || r.T == Ty::Char)) {
            auto toStr = [](const Value& v) -> String {
                if (v.T == Ty::Str) return v.get<String>();
                if (v.T == Ty::Char) return String::RepeatFrom(v.get<char32_t>(), 1);
                return String();
            };
            if (l.T == Ty::Str || l.T == Ty::Char) {
                if (r.T != Ty::Str && r.T != Ty::Char) {
                    error_container.addError(DEEP_ERRORS::UNSUPPORTED_OPERATOR(start, end, String("+"), l.T.toString(), r.T.toString()));
                    return new_null();
                }
                return new_string(toStr(l) + toStr(r));
            }
            // r вЂ” Str/Char, Р° l вЂ” С‡С‚Рѕ-С‚Рѕ РёРЅРѕРµ (С‡РёСЃР»Рѕ, bool, ...) в†’ РєРѕРЅРєР°С‚РµРЅР°С†РёСЏ РЅРµ РѕРїСЂРµРґРµР»РµРЅР°
            error_container.addError(DEEP_ERRORS::UNSUPPORTED_OPERATOR(start, end, String("+"), l.T.toString(), r.T.toString()));
            return new_null();
        }
        // array + array / tuple + tuple вЂ” РєРѕРЅРєР°С‚РµРЅР°С†РёСЏ
        if (op == '+' && (l.T.isArray() || l.T.isTuple()) && (r.T.isArray() || r.T.isTuple())) {
            const RuntimeArray& la = l.get<RuntimeArray>();
            const RuntimeArray& ra = r.get<RuntimeArray>();
            std::vector<Value> result;
            result.reserve(la.view().size() + ra.view().size());
            for (const auto& v : la.view()) result.push_back(v);
            for (const auto& v : ra.view()) result.push_back(v);
            // реф-элементы обеих частей остаются живыми в результате конкатенации
            std::map<size_t, std::pair<Node*, Context*>> cref;
            for (auto const& rp : la.refs) cref[rp.first] = rp.second;
            for (auto const& rp : ra.refs) cref[la.view().size() + rp.first] = rp.second;
            // РІС‹С‡РёСЃР»СЏРµРј РѕР±С‰РёР№ С‚РёРї СЌР»РµРјРµРЅС‚РѕРІ.
            // РЈРёСЂР·Р°Р№РЅС‹Р№ С‚РёРї Р»РµРІРѕРіРѕ РѕРїРµСЂРЅРґР° (`_container: Array[Pair[i64, str | char]]`)
            // СЃС‡РёС‚Р°РµРј РѕР±СЉСЏРІР»РµРЅРЅС‹Рј: РµСЃР»Рё РІСЃРµ СЃРѕР±РёСЂР°РµРјС‹Рµ СЌР»РµРјРµРЅС‚С‹ РІ РЅРµРіРѕ СѓРєР»Р°РґС‹РІР°СЋС‚СЃСЏ,
            // СЃРѕС…СЂР°РЅСЏРµРј РµРіРѕ (РёРЅР°С‡Рµ Р¶Рµ `+=` РЅРµ Р·РЅР°СЋС‰РµРіРѕ union РїСЂРµРІСЂР°С‰Р°РµС‚ element
            // РІ `Pair[i64, str] | Pair[i64, char]` Рё РµРіРѕ РЅРµР»СЊР·СЏ РёРЅРґРµРєСЃРёСЂРѕРІР°С‚СЊ/С‡РёС‚Р°С‚СЊ Ð¿Ð¾Ð»Ðµ).
            // РЁРёСЂРѕРєР°СЏ РіРµРЅРµСЂР°Р»РёР·Р°С†РёСЏ РїСЂРµР¶РЅРµРіРѕ РєРѕРґР° РЅРµ Р»РѕРјР°РµС‚СЃСЏ: union РІСЃРµ Р°РІРЅРѕ
            // СЂР°СЃС‚РµС‚, РєРѕРіРґР° Сѓ Р»РµРІРѕРіРѕ РЅРµС‚ С‚РёРїРёР·РёСЂРѕРІР°РЅРЅРѕРіРѕ РїСѓСЃС‚РѕРіРѕ `{}`/`[]`.
            Type elem = arrayConcatElemType(l.T, result);
            // Точность длины: если оба операнда имеют известный размер, сумма
            // тоже известна — иначе `typeof` печатал бы `{i64, ...}` там, где
            // `sizeof` уже даёт конкретное число (инвариант «число в typeof ==
            // sizeof»). Неизвестный (-1) у любого из операндов — остаёмся в -1.
            int concat_size = (l.T.size >= 0 && r.T.size >= 0)
                ? l.T.size + r.T.size : -1;
            Value cr = new_array(Type::ArrayType(elem, concat_size), std::move(result));
            if (!cref.empty()) cr.get_ref<RuntimeArray>().refs = std::move(cref);
            return cr;
        }
        // Адресная арифметика указателей: ptr + n, n + ptr, ptr - n, ptr - ptr.
        // Виртуальные адреса распределяются последовательно (++_address), поэтому шаг
        // на 1 = следующая ячейка. ptr ± n сдвигает адрес на n; ptr - ptr даёт разницу.
        bool lp = l.T.isPointer();
        bool rp = r.T.isPointer();
        if ((op == '+' || op == '-') && (lp || rp)) {
            if (lp && rp) {
                if (op == '-') {
                    long long diff = static_cast<long long>(l.get<Pointer>().addr) -
                                     static_cast<long long>(r.get<Pointer>().addr);
                    return new_i64(diff);
                }
                error_container.addError(DEEP_ERRORS::UNSUPPORTED_OPERATOR(start, end, String("+"), l.T.toString(), r.T.toString()));
                return new_null();
            }
            Value* ptr = lp ? &l : &r;
            Value* intv = lp ? &r : &l;
            if (!isIntegerType(intv->T)) {
                error_container.addError(DEEP_ERRORS::UNSUPPORTED_OPERATOR(start, end, String(static_cast<char32_t>(op)), l.T.toString(), r.T.toString()));
                return new_null();
            }
            long long step = getNumberFromType<long long>(*intv);
            if (op == '-' && lp) step = -step;
            unsigned long long new_addr = static_cast<unsigned long long>(
                static_cast<long long>(ptr->get<Pointer>().addr) + step);
            return new_pointer(ptr->get<Pointer>().pointee, new_addr);
        }
        if (!isNumberType(l.T) || !isNumberType(r.T)) {
            error_container.addError(DEEP_ERRORS::UNSUPPORTED_OPERATOR(start, end, String(static_cast<char32_t>(op)), l.T.toString(), r.T.toString()));
            return new_null();
        }
        Type resultT = resultTypeOf(l.T, r.T);
        bool isFloat = (resultT == Ty::f16 || resultT == Ty::f32 || resultT == Ty::f64);

        if (isFloat) {
            double a = getNumberFromType<double>(l);
            double b = getNumberFromType<double>(r);
            double res = 0;
            switch (op) {
                case '+': res = a + b; break;
                case '-': res = a - b; break;
                case '*': res = a * b; break;
                case '/':
                    res = a / b; break;
                case '%':
                    res = std::fmod(a, b); break;
                default: return new_null();
            }
            if (resultT == Ty::f16) return new_f16(res);
            if (resultT == Ty::f32) return new_f32(static_cast<float>(res));
            return new_f64(res);
        }

        int64_t a = getNumberFromType<int64_t>(l);
        int64_t b = getNumberFromType<int64_t>(r);
        int64_t res = 0;
        switch (op) {
            case '+': res = a + b; break;
            case '-': res = a - b; break;
            case '*': res = a * b; break;
            case '/':
                res = a / b; break;
            case '%':
                res = a % b; break;
            default: return new_null();
        }
        if (resultT == Ty::i8)  return new_i8(static_cast<int8_t>(res));
        if (resultT == Ty::i16) return new_i16(static_cast<int16_t>(res));
        if (resultT == Ty::i32) return new_i32(static_cast<int32_t>(res));
        return new_i64(res);
    }

    // СЂР°Р·СЂРµС€Р°РµС‚ lvalue РІРёРґР° [scope::]*member (РєРѕСЂРµРЅСЊ вЂ” РёРјСЏ РЅРµР№РјСЃРїРµР№СЃР° РёР»Рё
    // СЂР°Р·С‹РјРµРЅРѕРІР°РЅРёРµ СѓРєР°Р·Р°С‚РµР»СЏ; РґРѕРїСѓСЃРєР°РµС‚СЃСЏ РѕР±С‘СЂС‚РєР° РІ NodeScope) РІ С…СЂР°РЅРёРјС‹Р№
    // ContextObject*. Р’РѕР·РІСЂР°С‰Р°РµС‚ nullptr РїСЂРё РѕС€РёР±РєРµ (РѕС€РёР±РєР° СѓР¶Рµ РґРѕР±Р°РІР»РµРЅР° РІ error_container).
    // Разрешает член ТИПА: вариант enum'а либо (static) поле/метод структуры.
    // Используется и когда тип — корень пути (`Vec::filled`), и когда тип идёт
    // после namespace (`N::E::B`, `N::S::v`). Возвращает хранимый ContextObject*
    // либо nullptr (ошибка уже добавлена в error_container).
    ContextObject* resolveTypeMember(const Type& tt, const String& member_name, const Token& tok) {
        if (tt.isStruct()) {
            auto sit = structs.find(tt.name);
            if (sit == structs.end()) {
                error_container.addError(DEEP_ERRORS::STRUCT_NOT_FOUND(tok, tt.name));
                return nullptr;
            }
            auto mit = sit->second.methods.find(member_name);
            if (mit == sit->second.methods.end()) {
                bool is_static_field =
                    std::find(sit->second.static_fields.begin(),
                              sit->second.static_fields.end(),
                              member_name) != sit->second.static_fields.end();
                if (is_static_field && sit->second.decl_context.has_variable(member_name))
                    return &sit->second.decl_context.get_variable(member_name);
                auto fit = std::find_if(sit->second.fields.begin(),
                            sit->second.fields.end(),
                            [&](const StructFieldDef& f) { return f.name == member_name; });
                if (fit != sit->second.fields.end()) {
                    error_container.addError(DEEP_ERRORS::INSTANCE_FIELD_SCOPE_CALL(
                        tok, member_name, tt.name));
                    return nullptr;
                }
                error_container.addError(DEEP_ERRORS::METHOD_NOT_FOUND(tok, member_name, tt.name));
                return nullptr;
            }
            auto co_cell = std::make_unique<ContextObject>(
                member_name, Value(mit->second.callableType(), mit->second), Ty::Null);
            co_cell->addr = heap.allocate_high(co_cell.get());
            auto* ptr = co_cell.get();
            new_cells.push_back(std::move(co_cell));
            return ptr;
        }
        if (tt.isEnum()) {
            auto eit = enums.find(tt.name);
            if (eit == enums.end()) {
                error_container.addError(DEEP_ERRORS::ENUM_REDECLARED(tok, tt.name));
                return nullptr;
            }
            if (!eit->second.enum_context.has_variable_local(member_name)) {
                error_container.addError(DEEP_ERRORS::ENUM_VARIANT_NOT_FOUND(tok, member_name, tt.name));
                return nullptr;
            }
            return &eit->second.enum_context.get_variable_local(member_name);
        }
        error_container.addError(DEEP_ERRORS::SCOPE_RESOLUTION_NOT_NAMESPACE(tok, tt.toString()));
        return nullptr;
    }

    ContextObject* resolveScopeMember(Node* node, Context& ctx) {
        if (!node) return nullptr;
        while (node->type == NodeTypes::NodeScope) {
            node = static_cast<struct NodeScope*>(node)->node;
        }
        if (node->type != NodeTypes::NodeBinaryScopeResolution) {
            error_container.addError(DEEP_ERRORS::SCOPE_RESOLUTION_INVALID_MEMBER(Token(), "<expr>"));
            return nullptr;
        }
        auto* sr = static_cast<struct NodeBinaryScopeResolution*>(node);
        auto nameOf = [](Node* n) -> String {
            if (n && n->type == NodeTypes::NodeVariable)
                return static_cast<struct NodeVariable*>(n)->name;
            return String();
        };
        std::vector<String> names;
        String member_name = nameOf(sr->r_node);
        Node* cur = sr->l_node;
        while (cur->type == NodeTypes::NodeBinaryScopeResolution) {
            names.push_back(nameOf(static_cast<struct NodeBinaryScopeResolution*>(cur)->r_node));
            cur = static_cast<struct NodeBinaryScopeResolution*>(cur)->l_node;
        }
        // обход идёт от самого внутреннего имени к внешнему, а резолвинг ниже
        // идёт от корня вглубь — поэтому порядок «A::B::C::x» => [B, C], не [C, B]
        std::reverse(names.begin(), names.end());
        Node* root = cur;
        // разворачиваем скобки вокруг корня: (*a)::p -> root = *a (NodeUnaryDereference)
        while (root->type == NodeTypes::NodeScope) {
            root = static_cast<struct NodeScope*>(root)->node;
        }
        if (member_name.empty()) {
            error_container.addError(DEEP_ERRORS::SCOPE_RESOLUTION_INVALID_MEMBER(sr->member_token, "<expr>"));
            return nullptr;
        }
        Namespace* ns = nullptr;
        if (root->type == NodeTypes::NodeVariable) {
            String nm = static_cast<struct NodeVariable*>(root)->name;
            if (!ctx.has_variable(nm)) {
                error_container.addError(DEEP_ERRORS::UNDEFINED_VARIABLE(sr->member_token, nm));
                return nullptr;
            }
            ContextObject& obj = ctx.get_variable(nm);
            // тип как корень пути: `Vec::filled(1)`, `Types::a`, `Test::name`
            if (obj.value.T == Ty::Type_ && obj.is_type_decl) {
                Type tt = obj.value.get<Type>();
                if (tt.isStruct() || tt.isEnum())
                    return resolveTypeMember(tt, member_name, sr->member_token);
            }
            if (obj.value.T != Ty::Namespace) {
                error_container.addError(DEEP_ERRORS::SCOPE_RESOLUTION_NOT_NAMESPACE(sr->member_token, obj.value.T.toString()));
                return nullptr;
            }
            ns = &obj.value.get_ref<Namespace>();
        } else if (root->type == NodeTypes::NodeUnaryDereference) {
            auto* deref_node = static_cast<struct NodeUnaryDereference*>(root);
            Value ptr = execute(deref_node->node, ctx);
            if (!ptr.T.isPointer()) {
                error_container.addError(DEEP_ERRORS::UNARY_DEREFERENCE_NOT_INVALID_TYPE(deref_node->token, ptr.T.toString()));
                return nullptr;
            }
            Pointer pp = ptr.get<Pointer>();
            if (pp.addr == 0 || !heap.contains(pp.addr)) {
                error_container.addError(DEEP_ERRORS::NULL_DEREFERENCE(deref_node->token));
                return nullptr;
            }
            ContextObject* obj = heap.get(pp.addr);
            if (obj->value.T != Ty::Namespace) {
                error_container.addError(DEEP_ERRORS::SCOPE_RESOLUTION_NOT_NAMESPACE(deref_node->token, obj->value.T.toString()));
                return nullptr;
            }
            ns = &obj->value.get_ref<Namespace>();
        } else {
            // выражение слева от '::', например вызов, возвращающий тип структуры: `create()::a`
            Value lv = execute(root, ctx);
            if (lv.T == Ty::Type_) {
                Type lt = lv.get<Type>();
                if (lt.isStruct()) {
                    auto sit = structs.find(lt.name);
                    if (sit == structs.end()) {
                        error_container.addError(DEEP_ERRORS::STRUCT_NOT_FOUND(sr->member_token, lt.name));
                        return nullptr;
                    }
                    auto mit = sit->second.methods.find(member_name);
                    if (mit == sit->second.methods.end()) {
                        bool is_static_field =
                            std::find(sit->second.static_fields.begin(),
                                      sit->second.static_fields.end(),
                                      member_name) != sit->second.static_fields.end();
                        if (is_static_field && sit->second.decl_context.has_variable(member_name))
                            return &sit->second.decl_context.get_variable(member_name);
                        auto fit = std::find_if(sit->second.fields.begin(),
                                    sit->second.fields.end(),
                                    [&](const StructFieldDef& f) { return f.name == member_name; });
                        if (fit != sit->second.fields.end()) {
                            error_container.addError(DEEP_ERRORS::INSTANCE_FIELD_SCOPE_CALL(
                                sr->member_token, member_name, lt.name));
                            return nullptr;
                        }
                        error_container.addError(DEEP_ERRORS::METHOD_NOT_FOUND(
                            sr->member_token, member_name, lt.name));
                        return nullptr;
                    }
                    // значение-функция живёт в куче анализатора (весь запуск)
                    auto co_cell = std::make_unique<ContextObject>(
                        member_name, Value(mit->second.callableType(), mit->second), Ty::Null);
                    co_cell->addr = heap.allocate_high(co_cell.get());
                    auto* ptr = co_cell.get();
                    new_cells.push_back(std::move(co_cell));
                    return ptr;
                }
                // массив как type-значение: typeof(arr)::type / ::size
                if (lt.isArray() && !lt.container_1.empty()) {
                    if (member_name == "type") {
                        auto co_cell = std::make_unique<ContextObject>(
                            member_name, new_type(lt.container_1[0]), Ty::Null);
                        co_cell->addr = heap.allocate_high(co_cell.get());
                        auto* ptr = co_cell.get();
                        new_cells.push_back(std::move(co_cell));
                        return ptr;
                    }
                    if (member_name == "size") {
                        auto co_cell = std::make_unique<ContextObject>(
                            member_name, new_i64(lt.size), Ty::Null);
                        co_cell->addr = heap.allocate_high(co_cell.get());
                        auto* ptr = co_cell.get();
                        new_cells.push_back(std::move(co_cell));
                        return ptr;
                    }
                }
            }
            error_container.addError(DEEP_ERRORS::SCOPE_RESOLUTION_NOT_NAMESPACE(sr->member_token, lv.T.toString()));
            return nullptr;
        }
        if (!ns) return nullptr;
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i].empty() || !ns->context.has_variable(names[i])) {
                error_container.addError(DEEP_ERRORS::SCOPE_RESOLUTION_INVALID_MEMBER(sr->member_token, names[i]));
                return nullptr;
            }
            ContextObject& next = ns->context.get_variable(names[i]);
            if (next.modifiers.Private) {
                error_container.addError(DEEP_ERRORS::PRIVATE_ACCESS(sr->member_token, names[i]));
                return nullptr;
            }
            // тип (enum/struct) как звено пути: дальше идёт сам член
            // (`N::E::B`, `N::S::v`). Тип не может содержать namespace,
            // поэтому это всегда последнее промежуточное имя.
            if (next.value.T == Ty::Type_ && next.is_type_decl) {
                Type nt = next.value.get<Type>();
                if (nt.isStruct() || nt.isEnum()) {
                    if (i + 1 != names.size()) {
                        error_container.addError(DEEP_ERRORS::SCOPE_RESOLUTION_NOT_NAMESPACE(
                            sr->member_token, next.value.T.toString()));
                        return nullptr;
                    }
                    return resolveTypeMember(nt, member_name, sr->member_token);
                }
            }
            if (next.value.T != Ty::Namespace) {
                error_container.addError(DEEP_ERRORS::SCOPE_RESOLUTION_NOT_NAMESPACE(sr->member_token, next.value.T.toString()));
                return nullptr;
            }
            ns = &next.value.get_ref<Namespace>();
        }
        if (!ns->context.has_variable(member_name)) {
            error_container.addError(DEEP_ERRORS::SCOPE_RESOLUTION_INVALID_MEMBER(sr->member_token, member_name));
            return nullptr;
        }
        ContextObject& member = ns->context.get_variable(member_name);
        if (member.modifiers.Private) {
            error_container.addError(DEEP_ERRORS::PRIVATE_ACCESS(sr->member_token, member_name));
            return nullptr;
        }
        return &member;
    }

    // Истинность значения в УСЛОВИИ (if / while / for). Структура с дюндером
    // __bool__ проверяется его вызовом — раньше здесь принимались только bool и
    // число, поэтому `if (a)` для объекта с __bool__ давал error[88], хотя
    // оператор `!` тот же дюндер уже вызывал. Зеркалит rt::truthy(Ctx&, Box),
    // чтобы статический анализ и рантайм решали условие одинаково.
    // ok=false => значение непригодно как условие (вызывающий ставит ошибку).
    bool truthyOf(Value& v, const Token& tok, Context& ctx, bool& out) {
        if (v.T == Ty::Bool) { out = v.get<bool>(); return true; }
        if (isNumberType(v.T)) { out = getNumberFromType<>(v) != 0; return true; }
        Value dnd = new_null();
        if (tryDunder("__bool__", v, {}, ctx, tok, tok, dnd)) {
            if (dnd.T != Ty::Bool) return false;
            out = dnd.get<bool>();
            return true;
        }
        return false;
    }

    // РІС‹С‡РёСЃР»СЏРµС‚ СѓСЃР»РѕРІРёРµ С†РёРєР»Р°: bool -> get<bool>(); С‡РёСЃР»Рѕ -> != 0 (truthy);
    // иначе вЂ” ошибка LOOP_CONDITION_NOT_BOOL и false (цикл не исполнится)
    bool evalCondition(Node* cond_node, const Token& tok, Context& ctx) {
        Value v = execute(cond_node, ctx);
        bool out = false;
        if (truthyOf(v, tok, ctx, out)) return out;
        error_container.addError(DEEP_ERRORS::LOOP_CONDITION_NOT_BOOL(tok, v.T.toString()));
        return false;
    }

    // Р’С‹С‡РёСЃР»СЏРµС‚ С‚РёРї РІС‹СЂР°Р¶РµРЅРёСЏ-С‚РёРїР° (РІ С‚.С‡. Array[T, N] / Tuple[...]) РІ СЂР°РЅС‚Р°Р№Рј-РїРѕР·РёС†РёРё.
    // Р“РµРЅРµСЂРёРє-РёРјС‘РЅ РЅРµС‚ (СЌС‚Рѕ РЅРµ СЃРёРіРЅР°С‚СѓСЂР° С„СѓРЅРєС†РёРё) вЂ” С‚РѕР»СЊРєРѕ РєРѕРЅРєСЂРµС‚РЅС‹Рµ РёРјРµРЅР°/auto/any/union'С‹.
    Type evalTypeExpr(Node* n, Context& ctx) {
        if (!n) return Ty::Any;
        switch (n->type) {
            case NodeTypes::NodeVariable: {
                String nm = static_cast<struct NodeVariable*>(n)->name;
                if (nm == "auto") return Ty::Auto;
                if (nm == "any")  return Ty::Any;
                if (!ctx.has_variable(nm)) return Ty::Any;
                ContextObject& co = ctx.get_variable(nm);
                if (co.value.T == Ty::Type_) return co.value.get<Type>();
                return co.value.T;
            }
            case NodeTypes::NodeBinaryBitwiseOr:
            case NodeTypes::NodeBinaryLogicalOr: {
                auto* bn = static_cast<struct NodeBinaryBitwiseOr*>(n);
                return evalTypeExpr(bn->l_node, ctx) | evalTypeExpr(bn->r_node, ctx);
            }
            case NodeTypes::NodeUnaryDereference:
                return *(evalTypeExpr(static_cast<struct NodeUnaryDereference*>(n)->node, ctx));
            case NodeTypes::NodeArrayType: {
                auto* at = static_cast<struct NodeArrayType*>(n);
                Type elem = evalTypeExpr(at->element_type, ctx);
                int cnt = -1;
                if (at->count) {
                    Value cv = execute(at->count, ctx);
                    if (cv.T != Ty::Null) { // Null => execute уже добавил ошибку
                        if (isIntegerType(cv.T)) {
                            long long sz = getNumberFromType<long long>(cv);
                            if (sz == -1 || sz > 0) {
                                cnt = static_cast<int>(sz);
                            } else {
                                error_container.addError(DEEP_ERRORS::ARRAY_SIZE_INVALID(
                                    at->count_token, formatValue(cv)));
                            }
                        } else {
                            error_container.addError(DEEP_ERRORS::ARRAY_SIZE_INVALID(
                                at->count_token, cv.T.toString()));
                        }
                    }
                }
                return Type::ArrayType(elem, cnt);
            }
            case NodeTypes::NodeTupleType: {
                std::vector<Type> ts;
                for (auto* t : static_cast<struct NodeTupleType*>(n)->types)
                    ts.push_back(evalTypeExpr(t, ctx));
                return Type::TupleType(std::move(ts));
            }
            default: {
                Value v = execute(n, ctx);
                if (v.T == Ty::Type_) return v.get<Type>();
                return v.T;
            }
        }
    }

    // Связывает generic-аргументы структуры с параметрами декларации.
    // bindings — вычисленные значения (тип-параметр => Value(Ty::Type_),
    // value-параметр => число). при ошибке — false и уже добавленная диагностика.
    bool buildStructInstance(const StructSymbol& sym,
                             const std::vector<Node*>& arg_nodes,
                             const Token& tok, Context& ctx,
                             std::vector<Value>& bindings, Type& out) {
        size_t np = sym.generics.size();
        if (arg_nodes.size() > np) {
            error_container.addError(DEEP_ERRORS::STRUCT_GENERIC_ARG_COUNT(
                tok, sym.name,
                static_cast<long long>(arg_nodes.size()),
                static_cast<long long>(np)));
            return false;
        }
        std::vector<Type> targs;
        std::vector<long long> vargs;
        uint32_t mask = 0;
        // Дефолт generic-параметра — выражение, записанное в области видимости
        // объявления структуры. Явные аргументы, наоборот, считаем в вызывающем
        // контексте. Для локальной структуры decl_context — копия кадра объемлющей
        // функции, поэтому `struct[T: Type = type]` видит её параметры (к моменту
        // инстанцирования они уже не в контексте вызывающего).
        Context decl_defaults_ctx(ContextType::FunctionT);
        decl_defaults_ctx.parent = const_cast<Context*>(&sym.decl_context);
        for (size_t i = 0; i < np; ++i) {
            const GenericParam& gp = sym.generics[i];
            bool is_type = sym.is_type_generic(gp);
            Value bv = new_null();
            if (i < arg_nodes.size()) {
                bv = execute(arg_nodes[i], ctx);
                if (bv.T == Ty::Null) return false;   // execute уже добавил ошибку
            } else if (gp.default_value) {
                bv = execute(gp.default_value, decl_defaults_ctx);
                if (bv.T == Ty::Null) return false;
            } else {
                error_container.addError(DEEP_ERRORS::STRUCT_GENERIC_ARG_COUNT(
                    tok, sym.name,
                    static_cast<long long>(arg_nodes.size()),
                    static_cast<long long>(np)));
                return false;
            }
            if (is_type && bv.T != Ty::Type_) {
                error_container.addError(DEEP_ERRORS::EXPECTED_TYPE_EXPRESSION(
                    tok, bv.T.toString()));
                return false;
            }
            // Символический value-генералик структуры в сигнатуре метода
            // (`fn f(self: Box[T, N])`, `-> Vec[T, Size]`): на этапе декларации
            // конкретное значение ещё неизвестно и представлено как Type(BASE, имя)
            // (см. регистрацию generic-имён структуры в decl_context). Оно
            // конкретизируется на вызове по типу получателя (bindingsForType), а
            // реифицированный тип-шаблон нужен лишь для typeof — поэтому кодируем
            // значение маркером, а не ошибкой error[107].
            bool symbolic_value = false;
            if (!is_type && !isIntegerType(bv.T) && bv.T == Ty::Type_) {
                const Type& svt = bv.get<Type>();
                symbolic_value = svt.type == TypeType::BASE && svt.name == gp.name;
            }
            if (!is_type && !isIntegerType(bv.T) && !symbolic_value) {
                error_container.addError(DEEP_ERRORS::STRUCT_VALUE_GENERIC_TYPE(
                    tok, gp.name, bv.T.toString()));
                return false;
            }
            bindings.push_back(bv);
            if (symbolic_value) {
                vargs.push_back(SYMBOLIC_VALUE_GENERIC);
                continue;
            }
            if (is_type) {
                targs.push_back(bv.get<Type>());
                mask |= (1u << static_cast<unsigned>(i));
            } else {
                vargs.push_back(static_cast<long long>(getNumberFromType<>(bv)));
            }
        }
        out = Type::StructType(sym.name, targs, vargs, mask);
        return true;
    }

    // Обратная операция к сборке привязок в buildStructInstance: достаёт
    // generic-привязки из уже реифицированного типа структуры. Нужна, когда
    // тип выражения уже конкретизирован (например, вернулся из функции) и
    // пересобирать его по дефолтам нельзя. true — привязки восстановлены.
    bool bindingsFromReified(const StructSymbol& sym, const Type& inst,
                             std::vector<Value>& bindings) {
        bindings.clear();
        size_t ti = 0, vi = 0;
        for (size_t i = 0; i < sym.generics.size(); ++i) {
            if (sym.is_type_generic(sym.generics[i])) {
                if (ti >= inst.container_1.size()) return false;
                bindings.push_back(new_type(inst.container_1[ti++]));
            } else {
                if (vi >= inst.int_args.size()) return false;
                bindings.push_back(new_i64(static_cast<int64_t>(inst.int_args[vi++])));
            }
        }
        return true;
    }

    // Тип структуры уже содержит generic-аргументы (маска непуста или есть
    // аргументы в контейнерах), т.е. конкретизирован.
    static bool isReifiedStructType(const Type& t) {
        return t.arg_mask != 0 || !t.container_1.empty() || !t.int_args.empty();
    }

    // Объявленный тип массива связывает значение и БЕЗ strict. Параметры и
    // возврат уже проверяют `ft << v.T` независимо от модификатора
    // (error[93] / error[98]), поэтому локальное объявление, поле структуры,
    // `=` и `+=` обязаны решать так же — иначе `let a: Array[i64] = {1, "x"};`
    // принимается, а то же состояние, набранное через `a += {"x"}`, отклонялось
    // бы как error[83]. Проверяется и элемент, и длина (`size == -1` — «не
    // известно / неважно», как в operator>> TypeSystem.cpp:298). strict-путь
    // остаётся строже (полное `ft << v.T`) и выполняется отдельно.
    static bool arrayAnnotationOk(const Type& annotated, const Type& value) {
        if (!annotated.isArray() || !value.isArray()) return true;
        const Type we = annotated.container_1.empty() ? Ty::Any : annotated.container_1[0];
        if (we.isAny() || we.is_auto) return true;
        if (!value.container_1.empty()) {
            const Type ve = value.container_1[0];
            if (!ve.isAny() && !ve.is_auto && !(ve >> we)) return false;
        }
        if (annotated.size >= 0 && value.size >= 0 && annotated.size != value.size)
            return false;
        return true;
    }

    // Материализует поля инстанса: литеральные инициализаторы — в контексте
    // вызывающего, дефолты — в контексте привязок generic-имён структуры.
    bool materializeStruct(const StructSymbol& sym,
                           const std::vector<Value>& bindings,
                           const std::vector<StructFieldInit>& inits,
                           Context& caller, const Token& tok,
                           std::map<String, Value>& fields) {
        // контекст с привязками generic-имён: T -> reified Type, Size -> константа
        Context bind_ctx(ContextType::FunctionT);
        bind_ctx.parent = const_cast<Context*>(&sym.decl_context);
        for (size_t i = 0; i < sym.generics.size() && i < bindings.size(); ++i) {
            const GenericParam& gp = sym.generics[i];
            ContextObject co(gp.name, bindings[i], Ty::Null);
            co.is_type_decl = sym.is_type_generic(gp);
            bind_ctx.add_variable(gp.name, co);
        }

        std::map<String, const StructFieldInit*> provided;
        for (const auto& in : inits) {
            if (provided.count(in.name)) {
                error_container.addError(DEEP_ERRORS::DUPLICATE_FIELD(in.token, in.name));
                return false;
            }
            provided[in.name] = &in;
        }

        for (size_t i = 0; i < sym.fields.size(); ++i) {
            const StructFieldDef& fd = sym.fields[i];
            auto pit = provided.find(fd.name);
            Value fv = new_null();
            if (pit != provided.end()) {
                fv = execute(pit->second->value, caller);
            } else if (fd.default_node) {
                fv = execute(fd.default_node, bind_ctx);
            } else {
                error_container.addError(DEEP_ERRORS::MISSING_REQUIRED_FIELD(
                    fd.token, fd.name, sym.name));
                fv = new_null();
            }
            if (fd.type_node) {
                Type ft = evalTypeExpr(fd.type_node, bind_ctx);
                // пустой {} принимает элемент типа массива из объявленного типа;
                // динамический Array[T, -1] остаётся динамическим (size -1)
                if (ft.isArray() && fv.T.isArray()
                    && fv.T.container_1.size() == 1 && fv.T.container_1[0].isAny()
                    && fv.T.size == 0) {
                    int keep_size = (ft.size == -1) ? -1 : fv.T.size;
                    fv.T = Type::ArrayType(ft.container_1[0], keep_size);
                }
                if (fd.mods.Strict && !ft.is_auto && !ft.is_any && !(ft << fv.T)) {
                    error_container.addError(DEEP_ERRORS::STRICT_TYPE_MISMATCH(
                        fd.token, ft.toString(), fv.T.toString()));
                }
                else if (!arrayAnnotationOk(ft, fv.T)) {
                    error_container.addError(DEEP_ERRORS::ASSIGNMENT_TYPE_MISMATCH(
                        fd.token, fd.name, ft.toString(), fv.T.toString()));
                }
            }
            fields.emplace(fd.name, fv);
        }

        for (const auto& in : inits) {
            if (!sym.has_field(in.name)) {
                error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                    in.token, sym.name, in.name));
            }
        }
        return true;
    }

    // Контекст-объект, хранящий структуру, по левой части '.' (для write-back).
    ContextObject* structOwnerFor(Node* l_node, Context& ctx) {
        if (!l_node) return nullptr;
        if (l_node->type == NodeTypes::NodeVariable) {
            String nm = static_cast<struct NodeVariable*>(l_node)->name;
            if (!ctx.has_variable(nm)) return nullptr;
            ContextObject& oc = ctx.get_variable(nm);
            if (oc.ref_alias_node) {
                if (++ref_chain_depth > REF_CHAIN_LIMIT) {
                    --ref_chain_depth;
                    refCycleError(static_cast<struct NodeVariable*>(l_node)->token, oc.identifier);
                    return nullptr;
                }
                ContextObject* r = structOwnerFor(oc.ref_alias_node, ctx);
                --ref_chain_depth;
                return r;
            }
            return &oc;
        }
        if (l_node->type == NodeTypes::NodeUnaryDereference) {
            auto* dn = static_cast<struct NodeUnaryDereference*>(l_node);
            Value ptr = execute(dn->node, ctx);
            if (!ptr.T.isPointer()) return nullptr;
            Pointer p = ptr.get<Pointer>();
            if (p.addr == 0 || !heap.contains(p.addr)) return nullptr;
            return heap.get(p.addr);
        }
        if (l_node->type == NodeTypes::NodeBinaryScopeResolution)
            return resolveScopeMember(l_node, ctx);
        return nullptr;
    }

    // Является ли узел «адресатом» (l-value), в который можно писать/читать
    // как в ссылку: переменная, поле структуры, элемент массива, разыменование,
    // scope-резолв. ref-параметры и `ref let`-алиасы требуют именно l-value.
    bool isAssignablePath(Node* target, Context& ctx) {
        if (!target) return false;
        while (target->type == NodeTypes::NodeScope)
            target = static_cast<struct NodeScope*>(target)->node;
        switch (target->type) {
            case NodeTypes::NodeVariable: {
                auto* vn = static_cast<struct NodeVariable*>(target);
                if (!ctx.has_variable(vn->name)) return false;
                ContextObject& co = ctx.get_variable(vn->name);
                return !co.modifiers.Final && !co.modifiers.Const;
            }
            case NodeTypes::NodeUnaryDereference:
                return true;
            case NodeTypes::NodeMemberAccess: {
                auto* ma = static_cast<struct NodeMemberAccess*>(target);
                // цепочка `p.x` / `p.a.y`: адресуема, если адресуем её корень-переменная
                return isAssignablePath(ma->l_node, ctx) || ma->l_node->type == NodeTypes::NodeBinaryScopeResolution;
            }
            case NodeTypes::NodeArrayAccess: {
                auto* aa = static_cast<struct NodeArrayAccess*>(target);
                return isAssignablePath(aa->base, ctx);
            }
            case NodeTypes::NodeFunctionCall:
                // вызов, возвращающий ссылку ('-> ref T'): «адресат» — живая цель
                return true;
            case NodeTypes::NodeBinaryScopeResolution:
                return true;
            default:
                return false;
        }
    }

    // Структурная адресуемость пути БЕЗ учёта const/final (переменная, поле,
    // элемент, разыменование, вызов-вернувший-ref, Ns::member). Нужна для
    // const-корректности ссылок: `const ref let b = a;` на const-цель.
    bool isAddressablePath(Node* target, Context& ctx) {
        if (!target) return false;
        while (target->type == NodeTypes::NodeScope)
            target = static_cast<struct NodeScope*>(target)->node;
        switch (target->type) {
            case NodeTypes::NodeVariable:
                return ctx.has_variable(static_cast<struct NodeVariable*>(target)->name);
            case NodeTypes::NodeUnaryDereference:
                return true;
            case NodeTypes::NodeMemberAccess: {
                auto* ma = static_cast<struct NodeMemberAccess*>(target);
                return isAddressablePath(ma->l_node, ctx)
                    || ma->l_node->type == NodeTypes::NodeBinaryScopeResolution;
            }
            case NodeTypes::NodeArrayAccess:
                return isAddressablePath(static_cast<struct NodeArrayAccess*>(target)->base, ctx);
            case NodeTypes::NodeFunctionCall:
            case NodeTypes::NodeBinaryScopeResolution:
                return true;
            default:
                return false;
        }
    }

    // Корень пути — const-ячейка? (источник ссылки неизменяем).
    bool pathRootIsConst(Node* target, Context& ctx) {
        if (!target) return false;
        while (target->type == NodeTypes::NodeScope)
            target = static_cast<struct NodeScope*>(target)->node;
        switch (target->type) {
            case NodeTypes::NodeVariable: {
                auto* vn = static_cast<struct NodeVariable*>(target);
                if (!ctx.has_variable(vn->name)) return false;
                return ctx.get_variable(vn->name).modifiers.Const;
            }
            case NodeTypes::NodeMemberAccess:
                return pathRootIsConst(static_cast<struct NodeMemberAccess*>(target)->l_node, ctx);
            case NodeTypes::NodeArrayAccess:
                return pathRootIsConst(static_cast<struct NodeArrayAccess*>(target)->base, ctx);
            default:
                return false;
        }
    }

    // Является ли выражение реф-ЗНАЧЕНИЕМ (живой ссылкой), а не значением:
    //  - вызов функции, объявленной '-> ref T' (декларация берётся из resolved_calls,
    //    заполненного при реальном исполнении вызова Execute), или
    //  - переменная, объявленная как `ref let` либо являющаяся реф-параметром.
    // Возврат реф-значения из value-функции — VALUE_RETURNS_REF. Поля/элементы
    // (self.field, arr[i]) — это значения, а не рефы: они сюда не попадают.
    bool exprIsLiveRef(Node* e, Context& ctx) {
        if (!e) return false;
        while (e->type == NodeTypes::NodeScope)
            e = static_cast<struct NodeScope*>(e)->node;
        switch (e->type) {
            case NodeTypes::NodeFunctionCall: {
                auto rit = resolved_calls.find(e);
                if (rit == resolved_calls.end() || !rit->second) return false;
                if (rit->second->type != NodeTypes::NodeFunctionDeclaration) return false;
                return static_cast<struct NodeFunctionDeclaration*>(rit->second)->returns_ref;
            }
            case NodeTypes::NodeVariable: {
                auto* vn = static_cast<struct NodeVariable*>(e);
                if (current_ref_params.count(vn->name)) return true;
                if (!ctx.has_variable(vn->name)) return false;
                const ContextObject& co = ctx.get_variable(vn->name);
                return co.ref_alias_node != nullptr;
            }
            default:
                return false;
        }
    }

    // Есть ли у struct-типа метод-дюндер dname (напр. __assign__).
    bool structHasDunder(const Type& t, const String& dname) {
        if (!t.isStruct()) return false;
        auto sit = structs.find(t.name);
        return sit != structs.end() && sit->second.methods.find(dname) != sit->second.methods.end();
    }

    // __get__: автораспаковка значения при чтении переменной (NodeVariable).
    // Если значение — объект структуры с методом __get__, вызываем его
    // (self = объект) и повторяем разворот (вложенные обёртки) с защитой
    // от циклов. Разворачивание НЕ выполняется неявно — только по явному
    // запросу постфиксным оператором `a?` (см. NodePostfixUnwrap).
    Value maybeAutoGet(Value v, Context& context, const Token& tok) {
        int guard = 0;
        while (v.T.isStruct() && guard++ < 64) {
            auto sit = structs.find(v.T.name);
            if (sit == structs.end()) break;
            if (sit->second.methods.find(String(U"__get__")) == sit->second.methods.end()) break;
            Value unwrapped = new_null();
            if (!tryDunder(String(U"__get__"), v, {}, context, tok, tok, unwrapped)) break;
            v = unwrapped;
        }
        return v;
    }

    // Перегрузка присваивания: если текущее значение адресата — struct с __assign__,
    // вызываем метод (self = текущее значение, v — новое) с write-back мутаций self.
    // Во время внутренней пропагации мутированной копии наружу (writePathValue из
    // MemberAccess/ArrayAccess) диспетчер глушится, иначе `self.field = ...` внутри
    // самого __assign__ снова вызвал бы __assign__ для self.
    bool assign_dunder_disabled = false;
    // глубина выполнения __assign__: пока мы внутри тела __assign__, новые диспетчи
    // __assign__ подавляются (plain write) — иначе `self = value` рекурсирует
    // (аналог runtime g_assign_guard). Зеркало runtime-стороны.
    int dunder_assign_active = 0;
    bool tryAssignDunder(Node* target, Context& ctx, const Value& cur, const Value& v, const Token& tok) {
        if (assign_dunder_disabled || dunder_assign_active > 0) return false;
        if (!structHasDunder(cur.T, String(U"__assign__"))) return false;
        std::vector<Value> args;
        args.push_back(v);
        return callDunderWithWriteback(String(U"__assign__"), target, cur, args, tok, ctx);
    }

    // Путевая запись для выражений-адресатов: `c[0].first.value = x` / `p.arr[j] = y`.
    // Спускаемся по цепочке MemberAccess/ArrayAccess, мутируем глубокую копию и
    // на обратном ходу записываем значение обратно на каждом уровне (Value — это
    // глубокая копия, без записи мутация нигде не «прилипает»).
    bool writePathValue(Node* target, Context& ctx, const Value& v, const Token& tok) {
        if (!target) return false;
        while (target->type == NodeTypes::NodeScope)
            target = static_cast<struct NodeScope*>(target)->node;
        switch (target->type) {
            case NodeTypes::NodeVariable: {
                auto* vn = static_cast<struct NodeVariable*>(target);
                if (!ctx.has_variable(vn->name)) {
                    error_container.addError(DEEP_ERRORS::UNDEFINED_VARIABLE(vn->token, vn->name));
                    return false;
                }
                ContextObject& co = ctx.get_variable(vn->name);
                // const/final-линк: изменять сам алиас нельзя, даже если цель
                // изменяема (`const ref let b = a; b = 20;` — ошибка).
                if (co.modifiers.Final) {
                    error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(tok, co.identifier));
                    return false;
                }
                if (co.modifiers.Const) {
                    error_container.addError(DEEP_ERRORS::CONST_REASSIGNED(tok, co.identifier));
                    return false;
                }
                // ref-алиас: запись переадресуется цели («живая ссылка»)
                if (co.ref_alias_node) {
                    if (++ref_chain_depth > REF_CHAIN_LIMIT) {
                        --ref_chain_depth;
                        refCycleError(tok, co.identifier);
                        return false;
                    }
                    bool ok = writePathValue(co.ref_alias_node, ctx, v, tok);
                    --ref_chain_depth;
                    return ok;
                }
                if (const_receiver && (const_receiver == &co || const_receiver->identifier == co.identifier)) {
                    error_container.addError(DEEP_ERRORS::CONST_FN_MUTATION(tok, co.identifier));
                    return false;
                }
                // запись значения, содержащего указатель на локаль ЭТОЙ функции, в слот,
                // который переживёт return (глобальная переменная / локаль внешней
                // функции) — висячий указатель (P2)
                if (current_fn_scope != 0 && escapedLocalPtr(v, current_fn_scope)) {
                    auto lit = local_owner.find(co.addr);
                    bool dest_dies_with_fn = (lit != local_owner.end() && lit->second == current_fn_scope);
                    if (!dest_dies_with_fn) {
                        error_container.addError(DEEP_ERRORS::POINTER_ESCAPES_SCOPE(
                            tok, "stored beyond the owning function"));
                    }
                }
                if (tryAssignDunder(target, ctx, co.value, v, tok)) return true;
                // строгая типизация цели: та же проверка, что при прямой записи `a = v`
                {
                    Type wt = co.wait_type;
                    bool is_fn = wt.isCallable() || (wt.isUnion() && [&wt]() {
                        for (const auto& m : wt.container_1) if (m.isCallable()) return true;
                        return false;
                    }());
                    Value assignV = v;
                    if (wt.isArray() && v.T.isArray()
                            && v.T.container_1.size() == 1
                            && v.T.container_1[0].isAny()
                            && v.T.size == 0
                            && !wt.container_1.empty()) {
                        assignV.T = Type::ArrayType(wt.container_1[0], v.T.size);
                    }
                    bool constrained = co.modifiers.Strict && !is_fn
                        && !(wt == Ty::Null) && !wt.is_auto && !wt.is_any;
                    if (!arrayAnnotationOk(wt, assignV.T)
                            || (constrained && !(wt << assignV.T))) {
                        error_container.addError(DEEP_ERRORS::ASSIGNMENT_TYPE_MISMATCH(
                            tok, co.identifier, wt.toString(), assignV.T.toString()));
                        return false;
                    }
                    if (dryAllowsWrite(ctx, vn->name)) co.value = assignV;
                }
                return true;
            }
            case NodeTypes::NodeUnaryDereference: {
                auto* dn = static_cast<struct NodeUnaryDereference*>(target);
                Value ptr = execute(dn->node, ctx);
                if (!ptr.T.isPointer()) {
                    error_container.addError(DEEP_ERRORS::UNARY_DEREFERENCE_NOT_INVALID_TYPE(
                        dn->token, ptr.T.toString()));
                    return false;
                }
                Pointer p = ptr.get<Pointer>();
                if (p.addr == 0 || !heap.contains(p.addr)) {
                    error_container.addError(DEEP_ERRORS::NULL_DEREFERENCE(dn->token));
                    return false;
                }
                ContextObject* co = heap.get(p.addr);
                if (co->modifiers.Final) {
                    error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(tok, co->identifier));
                    return false;
                }
                if (co->modifiers.Const) {
                    error_container.addError(DEEP_ERRORS::CONST_REASSIGNED(tok, co->identifier));
                    return false;
                }
                if (const_receiver && (const_receiver == co || const_receiver->identifier == co->identifier)) {
                    error_container.addError(DEEP_ERRORS::CONST_FN_MUTATION(tok, co->identifier));
                    return false;
                }
                if (tryAssignDunder(target, ctx, co->value, v, tok)) return true;
                {
                    Type wt = co->wait_type;
                    bool constrained = co->modifiers.Strict
                        && !(wt == Ty::Null) && !wt.is_auto && !wt.is_any;
                    if (!arrayAnnotationOk(wt, v.T) || (constrained && !(wt << v.T))) {
                        error_container.addError(DEEP_ERRORS::ASSIGNMENT_TYPE_MISMATCH(
                            tok, co->identifier, wt.toString(), v.T.toString()));
                        return false;
                    }
                }
                co->value = v;
                return true;
            }
            case NodeTypes::NodeBinaryScopeResolution: {
                ContextObject* co = resolveScopeMember(target, ctx);
                if (!co) return false;
                if (co->modifiers.Final) {
                    error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(tok, co->identifier));
                    return false;
                }
                if (co->modifiers.Const) {
                    error_container.addError(DEEP_ERRORS::CONST_REASSIGNED(tok, co->identifier));
                    return false;
                }
                if (tryAssignDunder(target, ctx, co->value, v, tok)) return true;
                {
                    Type wt = co->wait_type;
                    bool constrained = co->modifiers.Strict
                        && !(wt == Ty::Null) && !wt.is_auto && !wt.is_any;
                    if (!arrayAnnotationOk(wt, v.T) || (constrained && !(wt << v.T))) {
                        error_container.addError(DEEP_ERRORS::ASSIGNMENT_TYPE_MISMATCH(
                            tok, co->identifier, wt.toString(), v.T.toString()));
                        return false;
                    }
                }
                if (!dry_exec) co->value = v;
                return true;
            }
            case NodeTypes::NodeMemberAccess: {
                auto* ma = static_cast<struct NodeMemberAccess*>(target);
                String f = memberNameNode(ma->r_node);
                Node* parentNode = ma->l_node;
                // ресивер доступа к полю — сам объект (без автораспаковки __get__)
                Value parent = execute(parentNode, ctx);
                if (parent.T.isPointer() && parentNode->type != NodeTypes::NodeUnaryDereference) {
                    error_container.addError(DEEP_ERRORS::IMPLICIT_POINTER_MEMBER(
                        ma->member_token, parent.T.toString(), f));
                    return false;
                }
                if (parent.T.isPointer()) {
                    // (как putf в рантайме транспиляции), а не в копию структуры.
                    Pointer ptd = parent.get<Pointer>();
                    ContextObject* p_owner = nullptr;
                    RuntimeStruct* p_rs = nullptr;
                    auto pfs = field_slots.find(ptd.addr);
                    if (pfs != field_slots.end()) {
                        p_owner = pfs->second.first;
                    } else {
                        if (ptd.addr == 0 || !heap.contains(ptd.addr)) {
                            error_container.addError(DEEP_ERRORS::NULL_DEREFERENCE(ma->member_token));
                            return false;
                        }
                        p_owner = heap.get(ptd.addr);
                    }
                    if (!p_owner || !p_owner->value.T.isStruct()) {
                        error_container.addError(DEEP_ERRORS::MEMBER_OF_NON_STRUCT(
                            ma->member_token, p_owner ? p_owner->value.T.toString() : String(), f));
                        return false;
                    }
                    p_rs = &p_owner->value.get_ref<RuntimeStruct>();
                    if (p_owner->modifiers.Final) {
                        error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(tok, p_owner->identifier));
                        return false;
                    }
                    if (p_owner->modifiers.Const) {
                        error_container.addError(DEEP_ERRORS::CONST_REASSIGNED(tok, p_owner->identifier));
                        return false;
                    }
                    if (const_receiver && p_owner == const_receiver) {
                        error_container.addError(DEEP_ERRORS::CONST_FN_MUTATION(ma->member_token, f));
                        return false;
                    }
                    auto& pf = p_rs->mut();
                    auto pit = pf.find(f);
                    if (pit == pf.end()) {
                        error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                            ma->member_token, p_owner->value.T.name, f));
                        return false;
                    }
                    const StructSymbol* psym = structSymForType(p_owner->value.T);
                    const StructFieldDef* pfd = psym ? psym->field_def(f) : nullptr;
                    if (pfd && pfd->mods.Private && active_struct != psym) {
                        error_container.addError(DEEP_ERRORS::PRIVATE_FIELD(
                            ma->member_token, p_owner->value.T.name, f));
                        return false;
                    }
                    if (pfd && pfd->mods.Final) {
                        error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(
                            ma->member_token, f));
                        return false;
                    }
                    if (tryAssignDunder(target, ctx, pit->second, v, tok)) return true;
                    Value p_assignV = v;
                    {
                        Type ft = (psym && pfd) ? fieldTypeFor(*psym, p_owner->value.T, f) : pit->second.T;
                        if (ft.isArray() && v.T.isArray() && v.T.container_1.size() == 1
                                && v.T.container_1[0].isAny() && v.T.size == 0
                                && !ft.container_1.empty()) {
                            p_assignV.T = Type::ArrayType(ft.container_1[0], v.T.size);
                        }
                    }
                    if (pfd) {
                        Type ft = fieldTypeFor(*psym, p_owner->value.T, f);
                        if (pfd->mods.Strict) {
                            if (!ft.is_auto && !ft.is_any && !(ft << p_assignV.T)) {
                                error_container.addError(DEEP_ERRORS::STRICT_TYPE_MISMATCH(
                                    ma->member_token, ft.toString(), p_assignV.T.toString()));
                                return false;
                            }
                        }
                        else if (!arrayAnnotationOk(ft, p_assignV.T)) {
                            error_container.addError(DEEP_ERRORS::ASSIGNMENT_TYPE_MISMATCH(
                                ma->member_token, f, ft.toString(), p_assignV.T.toString()));
                            return false;
                        }
                    }
                    pit->second = p_assignV;
                    return true;
                }
                if (!parent.T.isStruct()) {
                    error_container.addError(DEEP_ERRORS::MEMBER_OF_NON_STRUCT(
                        ma->member_token, parent.T.toString(), f));
                    return false;
                }
                RuntimeStruct& rs = parent.get_ref<RuntimeStruct>();
                auto& pw = rs.mut();
                auto it = pw.find(f);
                if (it == pw.end()) {
                    error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                        ma->member_token, parent.T.name, f));
                    return false;
                }
                const StructSymbol* sym = structSymForType(parent.T);
                const StructFieldDef* fd = sym ? sym->field_def(f) : nullptr;
                if (fd && fd->mods.Private && active_struct != sym) {
                    error_container.addError(DEEP_ERRORS::PRIVATE_FIELD(
                        ma->member_token, parent.T.name, f));
                    return false;
                }
                if (fd && fd->mods.Final) {
                    error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(
                        ma->member_token, f));
                    return false;
                }
                if (tryAssignDunder(target, ctx, it->second, v, tok)) return true;
                Value assignV = v;
                {
                    Type ft = (sym && fd) ? fieldTypeFor(*sym, parent.T, f) : it->second.T;
                    if (ft.isArray() && v.T.isArray()
                            && v.T.container_1.size() == 1
                            && v.T.container_1[0].isAny()
                            && v.T.size == 0
                            && !ft.container_1.empty()) {
                        assignV.T = Type::ArrayType(ft.container_1[0], v.T.size);
                    }
                }
                if (fd) {
                        Type ft = fieldTypeFor(*sym, parent.T, f);
                        if (fd->mods.Strict) {
                            if (!ft.is_auto && !ft.is_any && !(ft << assignV.T)) {
                                error_container.addError(DEEP_ERRORS::STRICT_TYPE_MISMATCH(
                                    ma->member_token, ft.toString(), assignV.T.toString()));
                                return false;
                            }
                        }
                        else if (!arrayAnnotationOk(ft, assignV.T)) {
                            error_container.addError(DEEP_ERRORS::ASSIGNMENT_TYPE_MISMATCH(
                                ma->member_token, f, ft.toString(), assignV.T.toString()));
                            return false;
                        }
                    }
                    it->second = assignV;
                {
                    bool saved = assign_dunder_disabled;
                    assign_dunder_disabled = true;
                    bool ok = writePathValue(parentNode, ctx, parent, ma->member_token);
                    assign_dunder_disabled = saved;
                    return ok;
                }
            }
            case NodeTypes::NodeArrayAccess: {
                auto* aa = static_cast<struct NodeArrayAccess*>(target);
                Value cur = execute(aa->base, ctx);
                // диспатч «по текущему значению элемента»: элемент — struct с __assign__
                // обрабатывается дюндером (зеркало рантайм setindex_dispatch), иначе — замена
                Value elt = new_null();
                if (elementAt(cur, aa->indices, 0, tok, ctx, elt)) {
                    if (tryAssignDunder(target, ctx, elt, v, tok)) return true;
                }
                if (!setIndexRec(cur, aa->indices, 0, tok, ctx, v)) return false;
                bool saved = assign_dunder_disabled;
                assign_dunder_disabled = true;
                bool ok = writePathValue(aa->base, ctx, cur, tok);
                assign_dunder_disabled = saved;
                return ok;
            }
            case NodeTypes::NodeFunctionCall: {
                // write-back ref-возврата: `get() = v` / `get().field = v`.
                // Повторно исполняем вызов: его `return <lvalue>` в write-режиме
                // запишет v в живую цель. Если функция НЕ возвращает ref — значение
                // некуда писать (ассайн во временный результат) — ошибка.
                auto* fcN = static_cast<struct NodeFunctionCall*>(target);
                ref_write_armed = true;
                ref_write_val = v;
                ref_write_rcall = fcN;
                ref_write_consumed = false;
                execute(fcN, ctx);
                ref_write_armed = false;
                ref_write_rcall = nullptr;
                if (!ref_write_consumed)
                    error_container.addError(DEEP_ERRORS::ASSIGN_TO_EXPRESSION(tok, "expr"));
                return ref_write_consumed;
            }
            default:
                error_container.addError(DEEP_ERRORS::ASSIGN_TO_EXPRESSION(tok, "expr"));
                return false;
        }
    }

    // Числовой литерал на месте элемента `Array[T, N]{…}`: `1` — это i64, но
    // контейнер объявлен `Array[i32, 3]`, и литерал должен занять место i32
    // (как в `let x: i32 = 5`). Без этой адаптации `Array[i32, 3]{1, 2, 3}`
    // давал error[187], а хранил i64 внутри `Array[i32, 3]`.
    static bool isNumericLiteralNode(Node* n) {
        while (n && n->type == NodeTypes::NodeScope)
            n = static_cast<struct NodeScope*>(n)->node;
        return n && n->type == NodeTypes::NodeNumber;
    }

    static Value adaptNumericLiteral(const Value& v, const Type& target) {
        if (v.T == target) return v;
        if (isIntegerType(v.T) && isIntegerType(target)) {
            long long x = getNumberFromType<long long>(v);
            if (target == Ty::i8)  return new_i8(static_cast<int8_t>(x));
            if (target == Ty::i16) return new_i16(static_cast<int16_t>(x));
            if (target == Ty::i32) return new_i32(static_cast<int32_t>(x));
            if (target == Ty::i64) return new_i64(x);
        }
        if (isNumberType(v.T) && isFloatType(target)) {
            double d = getNumberFromType<double>(v);
            if (target == Ty::f16) return new_f16(d);
            if (target == Ty::f32) return new_f32(static_cast<float>(d));
            if (target == Ty::f64) return new_f64(d);
        }
        return v;
    }

    Type commonElementType(const Type& a, const Type& b) {
        if (a == b) return a;
        if (b >> a) return a;
        if (a >> b) return b;
        return a | b;
    }

    // Р—Р°РїРёСЃС‹РІР°РµС‚ Р·РЅР°С‡РµРЅРёРµ v РІ СЌР»РµРјРµРЅС‚ РїРѕ С†РµРїРѕС‡РєРµ РёРЅРґРµРєСЃРѕРІ (РјСѓС‚РёСЂСѓСЏ РєРѕРїРёСЋ root).
    bool setIndexRec(Value& cur, const std::vector<Node*>& idxs, size_t k, const Token& tok, Context& ctx, const Value& v) {
        if (cur.T == Ty::Str) {
            error_container.addError(DEEP_ERRORS::STRING_INDEX_IMMUTABLE(tok));
            return false;
        }
        if (!(cur.T.isArray() || cur.T.isTuple())) {
            error_container.addError(DEEP_ERRORS::CANNOT_INDEX(tok, cur.T.toString()));
            return false;
        }
        Value iv = execute(idxs[k], ctx);
        if (!isIntegerType(iv.T)) {
            error_container.addError(DEEP_ERRORS::ARRAY_INDEX_NOT_INTEGER(tok, iv.T.toString()));
            return false;
        }
        long long idx = static_cast<long long>(getNumberFromType<>(iv));
        RuntimeArray& arr = cur.get_ref<RuntimeArray>();
        if (idx < 0 || idx >= static_cast<long long>(arr.size())) {
            error_container.addError(DEEP_ERRORS::ARRAY_INDEX_OUT_OF_RANGE(tok, idx, static_cast<long long>(arr.size())));
            return false;
        }
        if (k + 1 == idxs.size()) {
            // живой элемент-алиас: пишем сквозь ссылку в цель, алиас сохраняем
            auto ait = arr.refs.find(static_cast<size_t>(idx));
            if (ait != arr.refs.end()) {
                Node* an = ait->second.first;
                Context* actx = ait->second.second ? ait->second.second : &ctx;
                return writePathValue(an, *actx, v, tok);
            }
            arr.mut()[static_cast<size_t>(idx)] = v;
            return true;
        }
        return setIndexRec(arr.mut()[static_cast<size_t>(idx)], idxs, k + 1, tok, ctx, v);
    }

    // копия Значения элемента по цепочке индексов (для диспатча __assign__ в writePathValue)
    bool elementAt(const Value& root, const std::vector<Node*>& idxs, size_t k,
                   const Token& tok, Context& ctx, Value& out) {
        Value cur = root;
        for (size_t j = k; j < idxs.size(); ++j) {
            if (!(cur.T.isArray() || cur.T.isTuple())) return false;
            Value iv = execute(idxs[j], ctx);
            if (!isIntegerType(iv.T)) return false;
            long long idx = static_cast<long long>(getNumberFromType<>(iv));
            const RuntimeArray& arr = cur.get_ref<RuntimeArray>();
            if (idx < 0 || idx >= static_cast<long long>(arr.size())) return false;
            auto ait = arr.refs.find(static_cast<size_t>(idx));
            if (ait != arr.refs.end()) {
                // живой элемент-алиас: актуальное значение цели
                Node* an = ait->second.first;
                Context* actx = ait->second.second ? ait->second.second : &ctx;
                if (++ref_chain_depth > REF_CHAIN_LIMIT) {
                    --ref_chain_depth;
                    return false;
                }
                cur = execute(an, *actx);
                --ref_chain_depth;
            } else {
                cur = arr.view()[static_cast<size_t>(idx)];
            }
        }
        out = cur;
        return true;
    }

    // assignIndexElement replaced by flattened index handling in NodeBinaryAssignment LHS branch

    // ===== append <array>, <value>; — in-place добавление элемента =====
    // Общий тип элементов контейнера (единственный значимый элемент Type-union):
    // дообновляем container_1 без пересоздания массива.
    Type appendUnionType(const Type& cur, const Type& v) {
        // Только isAny(). Запись `cur == Ty::Any` здесь запрещена: operator==
        // считает any равным ЛЮБОМУ типу (TypeSystem.cpp:215), поэтому условие
        // было истинно всегда и union не строился никогда — `a += {"x"}` молча
        // подменял элемент i64 на char вместо `i64 | char`.
        if (cur.isAny()) return v;
        if (cur == v) return cur;
        if (v >> cur) return cur;      // новое значение уже покрывается текущим типом
        if (cur >> v) return v;
        return cur | v;
    }

    // Декларированный тип контейнера владельца (переменной/ячейки *p / Ns):
    // связывает и БЕЗ strict (инвариант «элемент приводим к T, длина равна N»,
    // см. arrayAnnotationOk и error[83] на объявлении/`=`). Если владелец без
    // аннотации или тип не контейнер — Ty::Any (проверок не требуется).
    Type declaredContainer(const ContextObject& co) const {
        Type wt = co.wait_type;
        if (wt == Ty::Null || wt.is_auto || wt.is_any) return Ty::Any;
        if (!wt.isArray()) return Ty::Any;
        return wt;
    }

    // Добавить значения v в конец VALUE-ячейки cell (только массив; кортежи фиксированной
    // арности — append к ним запрещён, error[129]). commit=false
    // (lint/LSP, мёртвые ветки) — только проверка типа без мутации.
    // declared — декларированный тип контейнера владельца (wait_type переменной
    // или тип поля структуры): значение после push_back обязано удовлетворять
    // arrayAnnotationOk (элемент — объявленной элемент, длина — объявленной N),
    // иначе error[83], как у `=`/`+=`.
    bool doAppendValue(Value& cell, const Value& v, const Token& tok, bool commit,
                       const Type& declared = Ty::Any,
                       const String& decl_name = String()) {
        if (!cell.T.isArray()) {
            error_container.addError(DEEP_ERRORS::APPEND_TARGET_NOT_ARRAY(tok, cell.T.toString()));
            return false;
        }
        // Кандидат после push_back: элемент расширяется union-ом (appendUnionType),
        // длина растёт на 1. Проверяем именно кандидата, а не только v: объявленный
        // `i64 | char` обязан принимать оба члена, а `i64 | char >> i64` = false.
        {
            const Type dc = declared;
            if (dc.isArray() && !dc.is_auto && !dc.is_any && !(dc == Ty::Null)
                    && !dc.container_1.empty()) {
                Type new_elem = cell.T.container_1.empty()
                                    ? v.T
                                    : appendUnionType(cell.T.container_1[0], v.T);
                int cand_size = (cell.T.size >= 0) ? cell.T.size + 1 : -1;
                Type cand = Type::ArrayType(new_elem, cand_size);
                if (!arrayAnnotationOk(dc, cand)) {
                    error_container.addError(DEEP_ERRORS::ASSIGNMENT_TYPE_MISMATCH(
                        tok, decl_name, dc.toString(), cand.toString()));
                    return false;
                }
            }
        }
        if (!commit) return true;
        RuntimeArray& arr = cell.get_ref<RuntimeArray>();
        std::vector<Value>& vec = arr.mut();
        vec.push_back(v);
        Type elemType = Ty::Any;
        if (!cell.T.container_1.empty())
            elemType = cell.T.container_1[0];
        elemType = appendUnionType(elemType, v.T);
        if (cell.T.container_1.empty())
            cell.T.container_1.push_back(elemType);
        else
            cell.T.container_1[0] = elemType;
        // Точность длины: после push_back статический размер обязан равняться
        // фактическому, иначе `sizeof` и `typeof` начинают врать по-разному.
        // Раньше size вообще не обновлялся — `{i64, 0}` при sizeof == 2.
        cell.T.size = static_cast<int>(vec.size());
        return true;
    }

    // Спуск по цепочке индексов `a[i][j]` и добавление в самый глубокий контейнер.
    bool appendAt(Value& cell, const std::vector<Node*>& idxs, size_t k,
                  const Token& tok, Context& ctx, const Value& v, bool commit) {
        if (cell.T == Ty::Str) {
            error_container.addError(DEEP_ERRORS::STRING_INDEX_IMMUTABLE(tok));
            return false;
        }
        if (!(cell.T.isArray() || cell.T.isTuple())) {
            error_container.addError(DEEP_ERRORS::CANNOT_INDEX(tok, cell.T.toString()));
            return false;
        }
        Value iv = execute(idxs[k], ctx);
        if (!isIntegerType(iv.T)) {
            error_container.addError(DEEP_ERRORS::ARRAY_INDEX_NOT_INTEGER(tok, iv.T.toString()));
            return false;
        }
        long long idx = static_cast<long long>(getNumberFromType<>(iv));
        RuntimeArray& arr = cell.get_ref<RuntimeArray>();
        if (idx < 0 || idx >= static_cast<long long>(arr.size())) {
            error_container.addError(DEEP_ERRORS::ARRAY_INDEX_OUT_OF_RANGE(
                tok, idx, static_cast<long long>(arr.size())));
            return false;
        }
        if (k + 1 == idxs.size()) {
            auto ait = arr.refs.find(static_cast<size_t>(idx));
            if (ait != arr.refs.end()) {
                Node* an = ait->second.first;
                Context* actx = ait->second.second ? ait->second.second : &ctx;
                return appendInPlace(an, *actx, v, tok, commit);
            }
            return doAppendValue(arr.mut()[static_cast<size_t>(idx)], v, tok, commit);
        }
        return appendAt(arr.mut()[static_cast<size_t>(idx)], idxs, k + 1, tok, ctx, v, commit);
    }

    // Поиск ячейки значения по lvalue-цепочке append и добавление в неё.
    bool appendInPlace(Node* target, Context& ctx, const Value& v, const Token& tok, bool commit) {
        while (target && target->type == NodeTypes::NodeScope)
            target = static_cast<struct NodeScope*>(target)->node;
        if (!target) {
            error_container.addError(DEEP_ERRORS::APPEND_TARGET_NOT_LVALUE(tok, "null"));
            return false;
        }
        switch (target->type) {
            case NodeTypes::NodeVariable: {
                auto* var_node = static_cast<struct NodeVariable*>(target);
                if (!ctx.has_variable(var_node->name)) {
                    error_container.addError(DEEP_ERRORS::UNDEFINED_VARIABLE(var_node->token, var_node->name));
                    aborted = true;
                    return false;
                }
                ContextObject& var = ctx.get_variable(var_node->name);
                // const/final-линк: нельзя менять сам алиас (см. writePathValue).
                if (var.modifiers.Const) {
                    error_container.addError(DEEP_ERRORS::CONST_REASSIGNED(var_node->token, var_node->name));
                    return false;
                }
                if (var.modifiers.Final) {
                    error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(var_node->token, var_node->name));
                    return false;
                }
                if (var.ref_alias_node)
                    return appendInPlace(var.ref_alias_node, ctx, v, tok, commit);
                if (const_receiver && (const_receiver == &var
                        || (const_receiver->identifier == var.identifier))) {
                    error_container.addError(DEEP_ERRORS::CONST_FN_MUTATION(tok, var.identifier));
                    return false;
                }
                return doAppendValue(var.value, v, tok, commit, declaredContainer(var), var_node->name);
            }
            case NodeTypes::NodeUnaryDereference: {
                auto* dn = static_cast<struct NodeUnaryDereference*>(target);
                Value ptr = execute(dn->node, ctx);
                if (!ptr.T.isPointer()) {
                    error_container.addError(DEEP_ERRORS::UNARY_DEREFERENCE_NOT_INVALID_TYPE(
                        dn->token, ptr.T.toString()));
                    return false;
                }
                Pointer p = ptr.get<Pointer>();
                if (p.addr == 0 || !heap.contains(p.addr)) {
                    error_container.addError(DEEP_ERRORS::NULL_DEREFERENCE(dn->token));
                    return false;
                }
                ContextObject* co = heap.get(p.addr);
                if (co->modifiers.Final) {
                    error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(tok, co->identifier));
                    return false;
                }
                if (co->modifiers.Const) {
                    error_container.addError(DEEP_ERRORS::CONST_REASSIGNED(tok, co->identifier));
                    return false;
                }
                if (const_receiver && (const_receiver == co
                        || (const_receiver->identifier == co->identifier))) {
                    error_container.addError(DEEP_ERRORS::CONST_FN_MUTATION(tok, co->identifier));
                    return false;
                }
                return doAppendValue(co->value, v, tok, commit,
                                     declaredContainer(*co), co->identifier);
            }
            case NodeTypes::NodeBinaryScopeResolution: {
                ContextObject* co = resolveScopeMember(target, ctx);
                if (!co) return false;
                if (co->modifiers.Final) {
                    error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(tok, co->identifier));
                    return false;
                }
                if (co->modifiers.Const) {
                    error_container.addError(DEEP_ERRORS::CONST_REASSIGNED(tok, co->identifier));
                    return false;
                }
                return doAppendValue(co->value, v, tok, commit,
                                     declaredContainer(*co), co->identifier);
            }
            case NodeTypes::NodeMemberAccess: {
                auto* ma = static_cast<struct NodeMemberAccess*>(target);
                String f = memberNameNode(ma->r_node);
                Node* parentNode = ma->l_node;
                Value parent = execute(parentNode, ctx);
                if (parent.T.isPointer() && parentNode->type != NodeTypes::NodeUnaryDereference) {
                    error_container.addError(DEEP_ERRORS::IMPLICIT_POINTER_MEMBER(
                        ma->member_token, parent.T.toString(), f));
                    return false;
                }
                if (parent.T.isPointer()) {
                    Pointer ptd = parent.get<Pointer>();
                    ContextObject* p_owner = nullptr;
                    auto pfs = field_slots.find(ptd.addr);
                    if (pfs != field_slots.end())
                        p_owner = pfs->second.first;
                    else if (ptd.addr == 0 || !heap.contains(ptd.addr)) {
                        error_container.addError(DEEP_ERRORS::NULL_DEREFERENCE(ma->member_token));
                        return false;
                    } else
                        p_owner = heap.get(ptd.addr);
                    if (!p_owner || !p_owner->value.T.isStruct()) {
                        error_container.addError(DEEP_ERRORS::MEMBER_OF_NON_STRUCT(
                            ma->member_token, p_owner ? p_owner->value.T.toString() : String(), f));
                        return false;
                    }
                    if (p_owner->modifiers.Final) {
                        error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(tok, p_owner->identifier));
                        return false;
                    }
                    if (p_owner->modifiers.Const) {
                        error_container.addError(DEEP_ERRORS::CONST_REASSIGNED(tok, p_owner->identifier));
                        return false;
                    }
                    if (const_receiver && p_owner == const_receiver) {
                        error_container.addError(DEEP_ERRORS::CONST_FN_MUTATION(ma->member_token, f));
                        return false;
                    }
                    RuntimeStruct& p_rs = p_owner->value.get_ref<RuntimeStruct>();
                    auto& pf = p_rs.mut();
                    auto pit = pf.find(f);
                    if (pit == pf.end()) {
                        error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                            ma->member_token, p_owner->value.T.name, f));
                        return false;
                    }
                    const StructSymbol* psym = structSymForType(p_owner->value.T);
                    const StructFieldDef* pfd = psym ? psym->field_def(f) : nullptr;
                    if (pfd && pfd->mods.Private && active_struct != psym) {
                        error_container.addError(DEEP_ERRORS::PRIVATE_FIELD(
                            ma->member_token, p_owner->value.T.name, f));
                        return false;
                    }
                    if (pfd && pfd->mods.Final) {
                        error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(ma->member_token, f));
                        return false;
                    }
                    return doAppendValue(pit->second, v, tok, commit,
                                         pfd ? fieldTypeFor(*psym, p_owner->value.T, f)
                                             : Ty::Any,
                                         f);
                }
                if (!parent.T.isStruct()) {
                    error_container.addError(DEEP_ERRORS::MEMBER_OF_NON_STRUCT(
                        ma->member_token, parent.T.toString(), f));
                    return false;
                }
                const StructSymbol* sym = structSymForType(parent.T);
                RuntimeStruct& rs = parent.get_ref<RuntimeStruct>();
                auto& pw = rs.mut();
                auto it = pw.find(f);
                if (it == pw.end()) {
                    error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                        ma->member_token, parent.T.name, f));
                    return false;
                }
                const StructFieldDef* fd = sym ? sym->field_def(f) : nullptr;
                if (fd && fd->mods.Private && active_struct != sym) {
                    error_container.addError(DEEP_ERRORS::PRIVATE_FIELD(
                        ma->member_token, parent.T.name, f));
                    return false;
                }
                if (fd && fd->mods.Final) {
                    error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(ma->member_token, f));
                    return false;
                }
                if (!doAppendValue(it->second, v, tok, commit,
                                     fd ? fieldTypeFor(*sym, parent.T, f)
                                        : Ty::Any,
                                     f))
                    return false;
                if (commit) {
                    bool saved = assign_dunder_disabled;
                    assign_dunder_disabled = true;
                    bool ok = writePathValue(parentNode, ctx, parent, ma->member_token);
                    assign_dunder_disabled = saved;
                    return ok;
                }
                return true;
            }
            case NodeTypes::NodeArrayAccess: {
                auto* aa = static_cast<struct NodeArrayAccess*>(target);
                if (aa->indices.empty())
                    return appendInPlace(aa->base, ctx, v, tok, commit);
                // lvalue-цепочка a[i].field / a[i][j]: разрешаем спуск по индексам
                // от текущей ячейки к самому глубокому массиву.
                Node* inner = aa->base;
                while (inner && inner->type == NodeTypes::NodeScope)
                    inner = static_cast<struct NodeScope*>(inner)->node;
                if (inner && inner->type == NodeTypes::NodeMemberAccess)
                    return appendMemberThenIndex(inner, ctx, v, tok, commit, aa->indices);
                std::vector<Node*> full = aa->indices;
                while (inner && inner->type == NodeTypes::NodeArrayAccess) {
                    auto* ia = static_cast<struct NodeArrayAccess*>(inner);
                    for (auto* i2 : ia->indices) full.insert(full.begin(), i2);
                    inner = ia->base;
                    while (inner && inner->type == NodeTypes::NodeScope)
                        inner = static_cast<struct NodeScope*>(inner)->node;
                }
                // спуск начинается с ячейки базового lvalue
                return appendDescendFrom(inner, ctx, v, tok, commit, full);
            }
            default:
                error_container.addError(DEEP_ERRORS::APPEND_TARGET_NOT_LVALUE(tok, "expr"));
                return false;
        }
    }

    // reserve <array lvalue>, <count>; — предвыделение ёмкости контейнера.
    //
    // В отличие от append ЗДЕСЬ НЕ нужен lvalue-спуск с write-back: ёмкость живёт
    // в разделяемом COW-буфере (RuntimeArray::data — shared_ptr<vector<Value>>),
    // поэтому копия Value цели уже смотрит в ТОТ ЖЕ буфер. Меняем только
    // capacity (не size и не элементы) — наблюдаемых значений это не меняет,
    // поэтому безопасно и в dry_exec, и для const-контейнера.
    // Цель читаем как rvalue-expression; допустимы только путевые узлы.
    bool reserveInPlace(Node* target, Context& ctx, const Token& tok, long long count) {
        while (target && target->type == NodeTypes::NodeScope)
            target = static_cast<struct NodeScope*>(target)->node;
        if (!target) {
            error_container.addError(DEEP_ERRORS::RESERVE_TARGET_NOT_LVALUE(tok, "null"));
            return false;
        }
        switch (target->type) {
            case NodeTypes::NodeVariable:
            case NodeTypes::NodeMemberAccess:
            case NodeTypes::NodeArrayAccess:
            case NodeTypes::NodeUnaryDereference:
            case NodeTypes::NodeBinaryScopeResolution:
                break;
            default:
                error_container.addError(DEEP_ERRORS::RESERVE_TARGET_NOT_LVALUE(tok, "expr"));
                return false;
        }
        if (count < 0) {
            error_container.addError(DEEP_ERRORS::RESERVE_COUNT_NEGATIVE(tok, count));
            return false;
        }
        Value cell = execute(target, ctx);
        if (!cell.T.isArray() && !cell.T.isTuple()) {
            return false;
        }
        if (count == 0) return true; // легальный no-op
        RuntimeArray& arr = cell.get_ref<RuntimeArray>();
        // Пишем прямо в разделяемый буфер (без mut()): mut() отщепил бы полную
        // копию массива — ровно тот O(n), который reserve обязан убрать.
        arr.data->reserve(static_cast<size_t>(count));
        return true;
    }

    // append в цепочку индексов, где база — член (`a[i].f`): спускаемся по полю,
    // затем по индексам.
    bool appendMemberThenIndex(Node* member_node, Context& ctx, const Value& v,
                               const Token& tok, bool commit, const std::vector<Node*>& idxs) {
        auto* ma = static_cast<struct NodeMemberAccess*>(member_node);
        String f = memberNameNode(ma->r_node);
                Node* parentNode = ma->l_node;
                Value parent = execute(parentNode, ctx);
                if (parent.T.isPointer() && parentNode->type != NodeTypes::NodeUnaryDereference) {
                    error_container.addError(DEEP_ERRORS::IMPLICIT_POINTER_MEMBER(
                        ma->member_token, parent.T.toString(), f));
                    return false;
                }
                if (parent.T.isPointer()) {
                    Pointer ptd = parent.get<Pointer>();
                    ContextObject* p_owner = nullptr;
                    auto pfs = field_slots.find(ptd.addr);
                    if (pfs != field_slots.end())
                        p_owner = pfs->second.first;
            else if (ptd.addr == 0 || !heap.contains(ptd.addr)) {
                error_container.addError(DEEP_ERRORS::NULL_DEREFERENCE(ma->member_token));
                return false;
            } else
                p_owner = heap.get(ptd.addr);
            if (!p_owner || !p_owner->value.T.isStruct()) {
                error_container.addError(DEEP_ERRORS::MEMBER_OF_NON_STRUCT(
                    ma->member_token, p_owner ? p_owner->value.T.toString() : String(), f));
                return false;
            }
            if (p_owner->modifiers.Final || p_owner->modifiers.Const) {
                error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(tok, p_owner->identifier));
                return false;
            }
            RuntimeStruct& p_rs = p_owner->value.get_ref<RuntimeStruct>();
            auto& pf = p_rs.mut();
            auto pit = pf.find(f);
            if (pit == pf.end()) {
                error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                    ma->member_token, p_owner->value.T.name, f));
                return false;
            }
            const StructSymbol* psym = structSymForType(p_owner->value.T);
            const StructFieldDef* pfd = psym ? psym->field_def(f) : nullptr;
            if (pfd && pfd->mods.Final) {
                error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(ma->member_token, f));
                return false;
            }
            return appendAt(pit->second, idxs, 0, tok, ctx, v, commit);
        }
        if (!parent.T.isStruct()) {
            error_container.addError(DEEP_ERRORS::MEMBER_OF_NON_STRUCT(
                ma->member_token, parent.T.toString(), f));
            return false;
        }
        RuntimeStruct& rs = parent.get_ref<RuntimeStruct>();
        auto& pw = rs.mut();
        auto it = pw.find(f);
        if (it == pw.end()) {
            error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                ma->member_token, parent.T.name, f));
            return false;
        }
        const StructSymbol* sym = structSymForType(parent.T);
        const StructFieldDef* fd = sym ? sym->field_def(f) : nullptr;
        if (fd && fd->mods.Final) {
            error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(ma->member_token, f));
            return false;
        }
        if (!appendAt(it->second, idxs, 0, tok, ctx, v, commit)) return false;
        if (commit) {
            bool saved = assign_dunder_disabled;
            assign_dunder_disabled = true;
            bool ok = writePathValue(parentNode, ctx, parent, ma->member_token);
            assign_dunder_disabled = saved;
            return ok;
        }
        return true;
    }

    // Разрешить базовую lvalue-ячейку цепочки индексов и спуститься по полным индексам.
    bool appendDescendFrom(Node* base, Context& ctx, const Value& v, const Token& tok,
                           bool commit, const std::vector<Node*>& idxs) {
        while (base && base->type == NodeTypes::NodeScope)
            base = static_cast<struct NodeScope*>(base)->node;
        if (!base) {
            error_container.addError(DEEP_ERRORS::APPEND_TARGET_NOT_LVALUE(tok, "null"));
            return false;
        }
        switch (base->type) {
            case NodeTypes::NodeVariable: {
                auto* var_node = static_cast<struct NodeVariable*>(base);
                if (!ctx.has_variable(var_node->name)) {
                    error_container.addError(DEEP_ERRORS::UNDEFINED_VARIABLE(var_node->token, var_node->name));
                    aborted = true;
                    return false;
                }
                ContextObject& var = ctx.get_variable(var_node->name);
                if (var.modifiers.Const) {
                    error_container.addError(DEEP_ERRORS::CONST_REASSIGNED(var_node->token, var_node->name));
                    return false;
                }
                if (var.modifiers.Final) {
                    error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(var_node->token, var_node->name));
                    return false;
                }
                if (var.ref_alias_node)
                    return appendDescendFrom(var.ref_alias_node, ctx, v, tok, commit, idxs);
                return appendAt(var.value, idxs, 0, tok, ctx, v, commit);
            }
            case NodeTypes::NodeUnaryDereference: {
                auto* dn = static_cast<struct NodeUnaryDereference*>(base);
                Value ptr = execute(dn->node, ctx);
                if (!ptr.T.isPointer()) {
                    error_container.addError(DEEP_ERRORS::UNARY_DEREFERENCE_NOT_INVALID_TYPE(
                        dn->token, ptr.T.toString()));
                    return false;
                }
                Pointer p = ptr.get<Pointer>();
                if (p.addr == 0 || !heap.contains(p.addr)) {
                    error_container.addError(DEEP_ERRORS::NULL_DEREFERENCE(dn->token));
                    return false;
                }
                ContextObject* co = heap.get(p.addr);
                if (co->modifiers.Final || co->modifiers.Const) {
                    error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(tok, co->identifier));
                    return false;
                }
                return appendAt(co->value, idxs, 0, tok, ctx, v, commit);
            }
            case NodeTypes::NodeBinaryScopeResolution: {
                ContextObject* co = resolveScopeMember(base, ctx);
                if (!co) return false;
                if (co->modifiers.Final || co->modifiers.Const) {
                    error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(tok, co->identifier));
                    return false;
                }
                return appendAt(co->value, idxs, 0, tok, ctx, v, commit);
            }
            case NodeTypes::NodeMemberAccess: {
                auto* ma = static_cast<struct NodeMemberAccess*>(base);
                String f = memberNameNode(ma->r_node);
                Node* parentNode = ma->l_node;
                Value parent = execute(parentNode, ctx);
                if (parent.T.isPointer() && parentNode->type != NodeTypes::NodeUnaryDereference) {
                    error_container.addError(DEEP_ERRORS::IMPLICIT_POINTER_MEMBER(
                        ma->member_token, parent.T.toString(), f));
                    return false;
                }
                if (parent.T.isPointer()) {
                    Pointer ptd = parent.get<Pointer>();
                    ContextObject* p_owner = nullptr;
                    auto pfs = field_slots.find(ptd.addr);
                    if (pfs != field_slots.end())
                        p_owner = pfs->second.first;
                    else if (ptd.addr == 0 || !heap.contains(ptd.addr)) {
                        error_container.addError(DEEP_ERRORS::NULL_DEREFERENCE(ma->member_token));
                        return false;
                    } else
                        p_owner = heap.get(ptd.addr);
                    if (!p_owner || !p_owner->value.T.isStruct()) {
                        error_container.addError(DEEP_ERRORS::MEMBER_OF_NON_STRUCT(
                            ma->member_token, p_owner ? p_owner->value.T.toString() : String(), f));
                        return false;
                    }
                    if (p_owner->modifiers.Final || p_owner->modifiers.Const) {
                        error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(tok, p_owner->identifier));
                        return false;
                    }
                    RuntimeStruct& p_rs = p_owner->value.get_ref<RuntimeStruct>();
                    auto& pf = p_rs.mut();
                    auto pit = pf.find(f);
                    if (pit == pf.end()) {
                        error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                            ma->member_token, p_owner->value.T.name, f));
                        return false;
                    }
                    const StructSymbol* psym = structSymForType(p_owner->value.T);
                    const StructFieldDef* pfd = psym ? psym->field_def(f) : nullptr;
                    if (pfd && pfd->mods.Final) {
                        error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(ma->member_token, f));
                        return false;
                    }
                    return appendAt(pit->second, idxs, 0, tok, ctx, v, commit);
                }
                if (!parent.T.isStruct()) {
                    error_container.addError(DEEP_ERRORS::MEMBER_OF_NON_STRUCT(
                        ma->member_token, parent.T.toString(), f));
                    return false;
                }
                RuntimeStruct& rs = parent.get_ref<RuntimeStruct>();
                auto& pw = rs.mut();
                auto it = pw.find(f);
                if (it == pw.end()) {
                    error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                        ma->member_token, parent.T.name, f));
                    return false;
                }
                const StructSymbol* sym = structSymForType(parent.T);
                const StructFieldDef* fd = sym ? sym->field_def(f) : nullptr;
                if (fd && fd->mods.Final) {
                    error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(ma->member_token, f));
                    return false;
                }
                if (!appendAt(it->second, idxs, 0, tok, ctx, v, commit)) return false;
                if (commit) {
                    bool saved = assign_dunder_disabled;
                    assign_dunder_disabled = true;
                    bool ok = writePathValue(parentNode, ctx, parent, ma->member_token);
                    assign_dunder_disabled = saved;
                    return ok;
                }
                return true;
            }
            default:
                error_container.addError(DEEP_ERRORS::APPEND_TARGET_NOT_LVALUE(tok, "expr"));
                return false;
        }
    }

    Value execute(Node* node, Context& context) {
        if (prof_enabled) {
            auto prof_t0 = std::chrono::steady_clock::now();
            Value prof_r = executeInner(node, context);
            auto prof_t1 = std::chrono::steady_clock::now();
            if (node)
                prof_node_time[(int)node->type] +=
                    std::chrono::duration_cast<std::chrono::nanoseconds>(prof_t1 - prof_t0).count();
            return prof_r;
        }
        return executeInner(node, context);
    }
    Value executeInner(Node* node, Context& context) {
        if (!node) return new_null();
        if (prof_enabled) {
            ++prof_exec_count;
            prof_node_count[(int)node->type]++;
        }
        switch (node->type) {

            case NodeTypes::NodeStructDeclaration: {
                auto* sn = static_cast<struct NodeStructDeclaration*>(node);
                Context& target = active_global ? global_context : context;
                // Повторное исполнение той же ноды декларации (структура объявлена
                // внутри функции, вызываемой многократно) — не переопределение.
                if (structs.count(sn->name) && structs.at(sn->name).decl_node != sn) {
                    if (structs.at(sn->name).final) {
                        error_container.addError(DEEP_ERRORS::STRUCT_REDECLARED(sn->name_token, sn->name));
                        break;
                    }
                    // Передекларация не-`final` структуры: стираем старый символ и
                    // регистрируем заново (последнее объявление побеждает).
                    structs.erase(sn->name);
                }
                if (!structs.count(sn->name)) {
                // помещаем символ в реестр ДО связывания методов: адреса внутри
                // std::map-ноды стабильны, методы держат parent на сиmm.decl_context
                structs.emplace(sn->name, StructSymbol());
                StructSymbol& sym = structs.at(sn->name);
                sym.decl_node = sn;
                sym.name = sn->name;
                sym.name_token = sn->name_token;
                sym.final = active_mods.Final;
                sym.generics = sn->generics;
                sym.decl_token = sn->name_token;
                sym.decl_context = (context.type == ContextType::FunctionT) ? context : target;
                // Снимок-копия официрования сделан ДО регистрации имени самой
                // структуры (ниже), поэтому методы не видят ни собственное имя
                // (list `Point {}` в теле static-метода), ни символы, объявленные
                // после структуры. Перецепляем снимок на живой контекст: цепочка
                // поиска из тел методов продолжается в реальный global_context /
                // контекст декларации (его предки при этом не теряются).
                sym.decl_context.parent = &target;

                // Generic-имена структуры в контексте методов резолвятся символично
                // (FirstT/SecondT -> Type(BASE, имя)): декл-реификация `-> Pair[SecondT,
                // FirstT]` строит symbolic-инстанс, конкретные типы подставит вызов
                // на инстансе (bindingsForType в call_ctx/ret_ctx).
                for (const GenericParam& gp : sym.generics) {
                    bool tp = sym.is_type_generic(gp);
                    ContextObject gco(gp.name, new_type(Type(TypeType::BASE, gp.name)),
                                      tp ? Ty::Type_ : Ty::Null);
                    gco.is_type_decl = tp;
                    sym.decl_context.add_variable(gp.name, gco);
                }

                // Имя самой структуры в контексте методов: `Vec2[T]` / `Vec2` в
                // сигнатурах и телах методов generic-структур реифицируются через
                // sym_fctx -> func.context -> decl_context, а регистрация имени в
                // global происходит ниже (после reification), поэтому без этого
                // слоя падает «Undefined variable 'Vec2'».
                {
                    ContextObject sname_co(sn->name, new_type(Type::StructType(sn->name)), Ty::Type_);
                    sname_co.is_type_decl = true;
                    sym.decl_context.add_variable(sn->name, sname_co);
                }

                // разворачиваем модификаторы/блоки по всему телу
                std::function<void(Node*, ContextObject::Modifiers)> collectFields;
                collectFields = [&](Node* n, ContextObject::Modifiers mods) {
                    if (!n) return;
                    if (n->type == NodeTypes::NodeModifier) {
                        auto* m = static_cast<struct NodeModifier*>(n);
                        if (m->name == "const")   mods.Const   = true;
                        if (m->name == "strict")  mods.Strict  = true;
                        if (m->name == "static")  mods.Static  = true;
                        if (m->name == "final")   mods.Final   = true;
                        if (m->name == "private") mods.Private = true;
                        if (m->name == "public")  mods.Private = false;
                        collectFields(m->node, mods);
                        return;
                    }
                    if (n->type == NodeTypes::NodeStatementsScope) {
                        auto* ss = static_cast<struct NodeStatementsScope*>(n);
                        for (auto* c : ss->nodes) collectFields(c, mods);
                        return;
                    }
                    if (n->type == NodeTypes::NodeVariableDeclaration) {
                        auto* vd = static_cast<struct NodeVariableDeclaration*>(n);
                        for (auto& f : sym.fields)
                            if (f.name == vd->name) {
                                error_container.addError(DEEP_ERRORS::DUPLICATE_FIELD(vd->name_token, vd->name));
                                return;
                            }
                        StructFieldDef fd;
                        fd.name = vd->name;
                        fd.token = vd->name_token;
                        fd.type_node = vd->type_node;
                        fd.default_node = vd->value_node;
                        fd.mods = mods;
                        sym.fields.push_back(std::move(fd));
                        return;
                    }
                };
                collectFields(sn->body, {});

                // static-поля: одна ячейка на структуру в decl_context,
                // адресуемая через `Test::name` (и `&Test::name`).
                for (const StructFieldDef& fd : sym.fields) {
                    if (!fd.mods.Static) continue;
                    // static-значение живёт на типе и адресуется как `S::name`,
                    // поэтому `private` на нём ничего не закрывает
                    if (fd.mods.Private)
                        error_container.addError(DEEP_ERRORS::PRIVATE_STATIC_FIELD(fd.token, fd.name));
                    Value sv = new_null();
                    if (fd.default_node)
                        sv = execute(fd.default_node, sym.decl_context);
                    ContextObject sfo(fd.name, sv, sv.T);
                    // переносим модификаторы поля (strict/final/const) на ячейку
                    // static-поля: иначе запись `Test::name = ...` не проверяется.
                    sfo.modifiers = fd.mods;
                    sym.decl_context.add_variable(fd.name, sfo);
                    ContextObject& sstored = sym.decl_context.get_variable(fd.name);
                    sstored.addr = heap.allocate_high(&sstored);
                    sym.static_fields.push_back(fd.name);
                }

                // методы: parent контекстов = лекс-окружение декларации (копия в символе)
                std::function<void(Node*, ContextObject::Modifiers)> collectMethods;
                collectMethods = [&](Node* n, ContextObject::Modifiers mods) {
                    if (!n) return;
                    if (n->type == NodeTypes::NodeModifier) {
                        auto* m = static_cast<struct NodeModifier*>(n);
                        if (m->name == "const")   mods.Const   = true;
                        if (m->name == "static")  mods.Static  = true;
                        if (m->name == "private") mods.Private = true;
                        if (m->name == "strict")  mods.Strict  = true;
                        if (m->name == "final")   mods.Final   = true;
                        collectMethods(m->node, mods);
                        return;
                    }
                    if (n->type == NodeTypes::NodeStatementsScope) {
                        auto* ss = static_cast<struct NodeStatementsScope*>(n);
                        for (auto* c : ss->nodes) collectMethods(c, mods);
                        return;
                    }
                    if (n->type != NodeTypes::NodeFunctionDeclaration) return;
                    auto* fnd = static_cast<struct NodeFunctionDeclaration*>(n);
                    Function func(fnd->name, fnd->params, fnd->generics,
                                  fnd->body, fnd->return_type);
                    func.decl = fnd;
                    func.is_const = mods.Const;
                    func.is_static = mods.Static;
                    func.returns_ref = fnd->returns_ref;
                    func.decl_token = fnd->gen_open_token.type == TypeOfToken::DUMMY
                                      ? fnd->name_token : fnd->gen_open_token;
                    func.doc_comment = fnd->doc_comment;
                    func.context.parent = &sym.decl_context;
                    func.captured = sym.decl_context;

                    // Символический контекст реификации: generic-имена метода
                    // (A/B) резолвятся как Type(BASE, имя). Без него декл-реификация
                    // `-> Pair[A, B]` выполняет узел в контексте, где A/B не
                    // объявлены, и сыплет «Undefined variable 'A'».
                    Context sym_fctx(ContextType::FunctionT);
                    sym_fctx.parent = &func.context;
                    for (size_t gi = 0; gi < func.generics.size(); ++gi) {
                        ContextObject gco(func.generics[gi].name,
                                          new_type(Type(TypeType::BASE, func.generics[gi].name)),
                                          Ty::Type_);
                        gco.is_type_decl = true;
                        sym_fctx.add_variable(func.generics[gi].name, gco);
                    }

                    GenericBindings bind;
                    NodeEvaluator eval = [this](Node* n, Context& c) { return execute(n, c); };
                    for (size_t pi = 0; pi < func.arguments.size(); ++pi)
                        func.reified_params.push_back(
                            declTypeNode(func.arguments[pi].type, func, bind, eval, sym_fctx));
                    if (func.return_type)
                        func.reified_ret = declTypeNode(func.return_type, func, bind, eval, sym_fctx);
                    for (size_t gi = 0; gi < func.generics.size(); ++gi) {
                        Node* cn = func.generics[gi].type;
                        func.generic_constraints.push_back(
                            cn ? declTypeNode(cn, func, bind, eval, sym_fctx) : Ty::Type_);
                    }

                    auto mit = sym.methods.find(fnd->name);
                    if (mit == sym.methods.end())
                        mit = sym.methods.emplace(fnd->name, FunctionContainer(fnd->name)).first;
                    FunctionContainer& fc = mit->second;
                    if (fc.has_duplicate_signature(func)) {
                        error_container.addError(DEEP_ERRORS::STRUCT_METHOD_REDECLARED(
                            fnd->name_token, fnd->name, sym.name));
                        return;
                    }
                    fc.add_function(func);
                };
                collectMethods(sn->body, {});
                } // первый проход: !structs.count(sn->name)
                // Имя структуры регистрируем в текущем контексте на каждом проходе:
                // `ret Test;` внутри функции требует переменную в свежем call_ctx.
                {
                    ContextObject co0(sn->name, new_type(Type::StructType(sn->name)), Ty::Type_);
                    co0.is_type_decl = true;
                    co0.modifiers.Final = active_mods.Final;
                    target.add_variable(sn->name, co0);
                    ContextObject& stored0 = target.get_variable(sn->name);
                    stored0.addr = heap.allocate_high(&stored0);
                }
                break;
            }

            case NodeTypes::NodeEnumDeclaration: {
                auto* en = static_cast<struct NodeEnumDeclaration*>(node);
                Context& target = active_global ? global_context : context;
                // Повторное исполнение той же ноды декларации (enum внутри
                // функции, вызываемой многократно) — не переопределение.
                if (enums.count(en->name) && enums.at(en->name).decl_node != en) {
                    error_container.addError(DEEP_ERRORS::ENUM_REDECLARED(en->name_token, en->name));
                    break;
                }
                if (!enums.count(en->name)) {
                    enums.emplace(en->name, EnumSymbol());
                    EnumSymbol& esym = enums.at(en->name);
                    esym.name = en->name;
                    esym.name_token = en->name_token;
                    esym.decl_token = en->name_token;
                    esym.decl_node = en;
                    // Пустой контекст вариантов (родитель — лекс-окружение, куда регистрируется имя enum)
                    esym.enum_context = Context(ContextType::FunctionT);
                    esym.enum_context.parent = &target;
                    long long val = 0;
                    for (const Token& vt : en->variants) {
                        ContextObject vco(vt.value, Value(Type::EnumType(en->name), val), Ty::Null);
                        esym.enum_context.add_variable(vt.value, vco);
                        ContextObject& vstored = esym.enum_context.get_variable(vt.value);
                        vstored.addr = heap.allocate_high(&vstored);
                        val++;
                    }
                }
                // Имя enum как тип: `fn f(a: Types)` / `Types::a` / похоже на struct.
                {
                    ContextObject co0(en->name, new_type(Type::EnumType(en->name)), Ty::Type_);
                    co0.is_type_decl = true;
                    target.add_variable(en->name, co0);
                    ContextObject& stored0 = target.get_variable(en->name);
                    stored0.addr = heap.allocate_high(&stored0);
                }
                break;
            }

            case NodeTypes::NodeModifier: {
                auto mod_node = static_cast<struct NodeModifier*>(node);
                if (!mod_node->node) return new_null();

                // СЃРѕС…СЂР°РЅСЏРµРј СЃРѕСЃС‚РѕСЏРЅРёРµ, С‡С‚РѕР±С‹ РјРѕРґРёС„РёРєР°С‚РѕСЂ РЅРµ В«РїСЂРѕС‚С‘РєВ» Р·Р° РїСЂРµРґРµР»С‹
                // СЃРІРѕРµР№ С†РµРїРѕС‡РєРё/Р±Р»РѕРєР°
ContextObject::Modifiers saved_mods = active_mods;
                bool saved_global = active_global;
                std::vector<String> saved_order = modifier_order;

                // накапливаем флаг текущего модификатора
                if (mod_node->name == "const")   active_mods.Const   = true;
                if (mod_node->name == "strict")  active_mods.Strict  = true;
                if (mod_node->name == "static")  active_mods.Static  = true;
                if (mod_node->name == "final")   active_mods.Final   = true;
                if (mod_node->name == "private") active_mods.Private = true;
                if (mod_node->name == "ref")     active_mods.Ref     = true;
                if (std::find(modifier_order.begin(), modifier_order.end(), mod_node->name)
                        == modifier_order.end())
                    modifier_order.push_back(mod_node->name); // порядок написания

                // выполняем внутренний узел (декларацию или блок) с накопленными
                // модификаторами; они применяются в case NodeVariableDeclaration
                Value result = execute(mod_node->node, context);

                // восстанавливаем состояние после блока/цепочки
                active_mods = saved_mods;
                active_global = saved_global;
                modifier_order = saved_order;

                return result;
            }

            case NodeTypes::NodeStatementsScope: {
                auto scope_node = static_cast<struct NodeStatementsScope*>(node);
                Value result = new_null();
                for (auto& child : scope_node->nodes) {
                    result = execute(child, context);
                    if (control_flow != ControlFlow::None) break;
                }
                // блок возвращает значение последнего выражения —
                // это значение-ветка if-выражения: `if (c) { 0 } else { 1 }`
                return result;
            }

            case NodeTypes::NodeVariableDeclaration: {
                auto var_decl = static_cast<struct NodeVariableDeclaration*>(node);
                Context& target = active_global ? global_context : context;

                // ref-декларация: `ref let name = <lvalue>;`. Храним узел-цель
                // алиаса; чтение/запись переменной переадресуются ей (живая ссылка).
                // Цель резолвится в контексте доступа (лексические цепочки родителя
                // достигают глобального контекста); указатель не сохраняем, чтобы не
                // зависеть от времени жизни call_ctx, копируемого лямбдами.
                Node* ref_target_node = nullptr;
                bool is_ref = active_mods.Ref;
                if (is_ref) {
                    if (!var_decl->value_node) {
                        error_container.addError(DEEP_ERRORS::REF_NEEDS_INIT(var_decl->name_token, var_decl->name));
                        break;
                    }
if (!isAssignablePath(var_decl->value_node, context)) {
                    // const-корректность: ссылка на const-ячейку допустима, только
                    // если сама ссылка объявлена `const` (`const ref let b = a;`).
                    // Иначе запись через ref обошла бы неизменяемость источника.
                    bool target_const = pathRootIsConst(var_decl->value_node, context);
                    bool addressable  = isAddressablePath(var_decl->value_node, context);
                    if (!(target_const && active_mods.Const && addressable)) {
                        error_container.addError(SEMANTIC_ERRORS::REF_BAD_TARGET(var_decl->name_token, var_decl->name));
                        break;
                    }
                }
                ref_target_node = var_decl->value_node;
                // Прямая самопривязка: `ref let a = a;` — цикл из одной ссылки
                // (ищем статически, до прогона, чтобы не зависнуть в recursion).
                Node* self_t = ref_target_node;
                while (self_t && self_t->type == NodeTypes::NodeScope)
                    self_t = static_cast<struct NodeScope*>(self_t)->node;
                if (self_t && self_t->type == NodeTypes::NodeVariable) {
                    auto* vn = static_cast<struct NodeVariable*>(self_t);
                    if (vn->name == var_decl->name) {
                        error_container.addError(DEEP_ERRORS::REF_SELF_ALIAS(var_decl->name_token, var_decl->name));
                        break;
                    }
                }
                }

                ContextObject context_object(var_decl->name,
                    is_ref && ref_target_node
                        ? execute(ref_target_node, context)
                        : execute(var_decl->value_node, context));
                // РїСЂРёРјРµРЅСЏРµРј РјРѕРґРёС„РёРєР°С‚РѕСЂС‹, РЅР°РєРѕРїР»РµРЅРЅС‹Рµ РїРѕ С†РµРїРѕС‡РєРµ/Р±Р»РѕРєСѓ
                context_object.modifiers = active_mods;
                context_object.modifiers.order = modifier_order;
                if (is_ref)
                    context_object.ref_alias_node = ref_target_node;

                // РїРѕРІС‚РѕСЂРЅРѕРµ РѕР±СЉСЏРІР»РµРЅРёРµ final-РїРµСЂРµРјРµРЅРЅРѕР№ РІ С‚РѕР№ Р¶Рµ РѕР±Р»Р°СЃС‚Рё вЂ” РѕС€РёР±РєР°
                // (С‚РµРЅРµРІРѕРµ РѕР±СЉСЏРІР»РµРЅРёРµ РІРѕ РІР»РѕР¶РµРЅРЅРѕР№ РѕР±Р»Р°СЃС‚Рё СЂР°Р·СЂРµС€РµРЅРѕ)
                if (target.has_variable_local(var_decl->name)) {
                    ContextObject& existing = target.get_variable_local(var_decl->name);
                    if (existing.modifiers.Final) {
                        error_container.addError(DEEP_ERRORS::FINAL_REDECLARED(var_decl->name_token, var_decl->name));
                        break;
                    }
                }
                if (var_decl->type_node) {
                    Value type_value = execute(var_decl->type_node, context);
                    if (type_value.T != Ty::Type_) {
                        error_container.addError(DEEP_ERRORS::EXPECTED_TYPE_EXPRESSION(var_decl->name_token, type_value.T.toString()));
                        break;
                    }
                    context_object.wait_type = type_value.get<Type>();
                }

                // РџСѓСЃС‚РѕР№ Р»РёС‚РµСЂР°Р» `{}` РІС‹РІРѕРґРёС‚СЃСЏ РєР°Рє {any, 0}; РµСЃР»Рё РѕР±СЉСЏРІР»РµРЅ
                // РєРѕРЅРєСЂРµС‚РЅС‹Р№ С‚РёРї РјР°СЃСЃРёРІР°, РїСѓСЃС‚РѕР№ Р»РёС‚РµСЂР°Р» РїСЂРёРЅРёРјР°РµС‚ РµРіРѕ СЌР»РµРјРµРЅС‚
                // (strict let a: Array[i64] = {}; -> {i64, 0}).
                if (context_object.wait_type.isArray()
                        && context_object.value.T.isArray()
                        && context_object.value.T.container_1.size() == 1
                        && context_object.value.T.container_1[0].isAny()
                        && context_object.value.T.size == 0) {
                    context_object.value.T = Type::ArrayType(
                        context_object.wait_type.container_1[0],
                        context_object.value.T.size);
                }

                // P8: full reified type of the local — LSP reifies generic method returns
                // for hover/completion chains (lb.at(0).first) from this, not from the bare
                // struct name token.
                local_types.emplace(
                    ((unsigned long long)(unsigned int)var_decl->name_token.position.line << 32)
                        | (unsigned int)var_decl->name_token.position.index,
                    context_object.value.T);

                // Inlay-РїРѕРґСЃРєР°Р·РєР° РІС‹РІРµРґРµРЅРЅРѕРіРѕ С‚РёРїР° РґР»СЏ `let x: auto = вЂ¦` РІРЅРµ
                // strict-СЂРµР¶РёРјР° (strict-РІРµСЂСЃРёСЏ РіРµРЅРµСЂРёСЂСѓРµС‚СЃСЏ РЅРёР¶Рµ РІ СЃРІРѕС‘Рј Р±Р»РѕРєРµ).
                // РџРѕРєР°Р·С‹РІР°РµРј С‚РѕР»СЊРєРѕ РєРѕРіРґР° С‚РёРї РЅР°РїРёСЃР°РЅ РёРґРµРЅС‚РёС„РёРєР°С‚РѕСЂРѕРј (`auto`)
                // Рё РµСЃС‚СЊ РёРЅРёС†РёР°Р»РёР·Р°С‚РѕСЂ, РїРѕ РєРѕС‚РѕСЂРѕРјСѓ С‚РёРї РІС‹РІРѕРґРёС‚СЃСЏ.
                if (!context_object.modifiers.Strict
                        && context_object.wait_type.is_auto
                        && var_decl->value_node
                        && var_decl->type_node
                        && var_decl->type_node->type == NodeTypes::NodeVariable) {
                    auto* tn = static_cast<struct NodeVariable*>(var_decl->type_node);
                    int hl = tn->token.position.line;
                    int hc = tn->token.position.index + tn->token.position.size;
                    String hlabel(" > ");
                    hlabel += context_object.value.T.toString();
                    inlay_hints.push_back({hl, hc, hlabel});
                }

                if (context_object.modifiers.Strict) {
                    if (context_object.wait_type.is_auto) {
                        if (!var_decl->value_node) {
                            error_container.addError(DEEP_ERRORS::STRICT_AUTO_REQUIRES_VALUE(var_decl->name_token));
                            break;
                        }
                        context_object.wait_type = context_object.value.T;
                        if (var_decl->type_node && var_decl->type_node->type == NodeTypes::NodeVariable) {
                            auto* tn = static_cast<struct NodeVariable*>(var_decl->type_node);
                            int hl = tn->token.position.line;
                            int hc = tn->token.position.index + tn->token.position.size;
                            String hlabel(" > ");
                            hlabel += context_object.value.T.toString();

                            inlay_hints.push_back({hl, hc, hlabel});
                        }
                    }
                    else if (context_object.wait_type.is_any) {
                        error_container.addError(DEEP_ERRORS::STRICT_ANY_POINTLESS(var_decl->name_token));
                    }
                    if (!(context_object.wait_type << context_object.value.T)) {
                        error_container.addError(DEEP_ERRORS::STRICT_TYPE_MISMATCH(var_decl->name_token, context_object.wait_type.toString(), context_object.value.T.toString()));
                        break;
                    }
                }
                // Аннотация массива связывает и без strict: см. arrayAnnotationOk.
                else if (var_decl->type_node
                        && !arrayAnnotationOk(context_object.wait_type, context_object.value.T)) {
                    error_container.addError(DEEP_ERRORS::ASSIGNMENT_TYPE_MISMATCH(
                        var_decl->name_token, var_decl->name,
                        context_object.wait_type.toString(), context_object.value.T.toString()));
                    break;
                }

                target.add_variable(var_decl->name, context_object);
                ContextObject& declared = target.get_variable(var_decl->name);
                declared.addr = heap.allocate_high(&declared);
                // локаль функции помечается скоупом-владельцем: '&' на неё внутри
                // функции даст указатель, который нельзя выносить за её пределы (P2)
                if (current_fn_scope != 0) local_owner[declared.addr] = current_fn_scope;
                break;
            }

            case NodeTypes::NodeFunctionDeclaration: {
                auto func_node = static_cast<struct NodeFunctionDeclaration*>(node);

                // extern-декларация: определение приходит из #cppinclude C++-файла.
                // Интерпретатор (-r) её выполнить не может — ошибка на этапе вызова
                // (см. call_body); здесь лишь контролируем дубликаты имени.
                if (func_node->is_extern) {
                    // M8: объявление extern-функции внутри `eval` запрещено.
                    if (in_eval)
                        error_container.addError(DEEP_ERRORS::EVAL_NOT_CONST(func_node->name_token, "an 'extern fn' declaration"));
                    for (const auto& en : extern_fns)
                        if (en == func_node->name) {
                            error_container.addError(DEEP_ERRORS::EXTERN_REDECLARED(func_node->name_token, func_node->name));
                            break;
                        }
                    extern_fns.push_back(func_node->name);
                }

                    Function func(func_node->name, func_node->params, func_node->generics,
                                  func_node->body, func_node->return_type);
                    func.decl = func_node;
                    func.returns_ref = func_node->returns_ref;
                func.decl_token = func_node->gen_open_token.type == TypeOfToken::DUMMY ? func_node->name_token : func_node->gen_open_token;
                func.doc_comment = func_node->doc_comment;
                func.context.parent = &(active_global ? global_context : context);
                // Р»РµРєСЃ-Р·Р°С…РІР°С‚: РІР»РѕР¶РµРЅРЅС‹Рµ С„СѓРЅРєС†РёРё С†РµРїР»СЏРµРј СЂРѕРґРёС‚РµР»РµРј РїСЂСЏРјРѕ Рє РІС‹РїРѕР»РЅСЏСЋС‰РµРјСѓСЃСЏ
                // РєРѕРЅС‚РµРєСЃС‚Сѓ (С‡РёСЃР»Р°, СЃС‚СЂРѕРєРё, Р»РѕРєР°Р»СЊРЅС‹Рµ С„СѓРЅРєС†РёРё Рё С‚.Рї.), Р° РЅРµ Рє РіР»РѕР±Р°Р»СЊРЅРѕР№ РїР°РјСЏС‚Рё.
                if (context.type == ContextType::FunctionT)
                    func.context.parent = &context;
                // СЃРЅРёРјРѕРє РѕРєСЂСѓР¶РµРЅРёСЏ Р·Р°С…РІР°С‚Р°: РєР»Р°РґС‘Рј РєРѕРїРёСЋ РІ СЃР°РјСѓ С„СѓРЅРєС†РёСЋ, С‡С‚РѕР±С‹ РїРѕСЃР»Рµ РІС‹С…РѕРґР°
                // РёР· РѕР±СЉС‘РјР»СЋС‰РµР№ С„СѓРЅРєС†РёРё РІС‹Р·С‹РІР°РµРјР°СЏ Р»СЏРјР±РґР°/Р·Р°РјС‹РєР°РЅРёРµ РІРёРґРµР»Рё Р·Р°С…РІР°С‡РµРЅРЅС‹Рµ Р·РЅР°С‡РµРЅРёСЏ.
                if (context.type == ContextType::FunctionT)
                    func.captured = context;

                // Reified-С‚РёРїС‹ РїР°СЂР°РјРµС‚СЂРѕРІ Рё РІРѕР·РІСЂР°С‚Р° РґР»СЏ typeof(РёРјСЏ):
                // РіРµРЅРµСЂРёРє-РёРјСЏ -> Type(BASE, РёРјСЏ), РєРѕРЅРєСЂРµС‚РЅС‹Р№ С‚РёРї -> reified, auto/any -> Ty::Any.
                {
                    GenericBindings bind;
                    NodeEvaluator eval = [this](Node* n, Context& c) { return execute(n, c); };
                    for (size_t pi = 0; pi < func.arguments.size(); ++pi) {
                        func.reified_params.push_back(declTypeNode(func.arguments[pi].type, func, bind, eval, func.context));
                        validateReturnTypeNames(func.arguments[pi].type, func, func.context);
                    }
                    if (func.return_type)
                        func.reified_ret = declTypeNode(func.return_type, func, bind, eval, func.context);
                    if (func.return_type)
                        validateReturnTypeNames(func.return_type, func, func.context);
                    // ограничения generic-параметров (`A: Type` -> Type_, `N: i64` -> i64;
                    // без ограничения — тип-параметр) для слотов typeof(имя)
                    for (size_t gi = 0; gi < func.generics.size(); ++gi) {
                        Node* cn = func.generics[gi].type;
                        func.generic_constraints.push_back(
                            cn ? declTypeNode(cn, func, bind, eval, func.context) : Ty::Type_);
                    }
                }

                // Р›РµРєСЃ-Р·Р°С…РІР°С‚ С‚РёРїРѕРІ: РІР»РѕР¶РµРЅРЅР°СЏ С„СѓРЅРєС†РёСЏ С„РёРєСЃРёСЂСѓРµС‚ reified-С‚РёРїС‹ РіРµРЅРµСЂРёРє-РёРјС‘РЅ
                // РѕР±СЉС‘РјР»СЋС‰РµР№ С„СѓРЅРєС†РёРё РІ РјРѕРјРµРЅС‚ РґРµРєР»Р°СЂР°С†РёРё (T -> f64). РџРѕСЃР»Рµ СЌС‚РѕРіРѕ РІС‹Р·РѕРІС‹
                // РјР°С‚С‡Р°С‚СЃСЏ СЃС‚СЂРѕРіРѕ РїРѕ Р·Р°С„РёРєСЃРёСЂРѕРІР°РЅРЅС‹Рј С‚РёРїР°Рј вЂ” РїРѕРІС‚РѕСЂРЅС‹Р№ РІС‹РІРѕРґ T РёР· Р°СЂРіСѓРјРµРЅС‚РѕРІ
                // РЅРµРІРѕР·РјРѕР¶РµРЅ, РїРѕСЌС‚РѕРјСѓ f(10) РїСЂРё T=f64 РґР°СЃС‚ РѕС€РёР±РєСѓ СЃРѕРѕС‚РІРµС‚СЃС‚РІРёСЏ С‚РёРїРѕРІ.
                if (context.type == ContextType::FunctionT) {
                    GenericBindings bind;
                    NodeEvaluator eval = [this](Node* n, Context& c) { return execute(n, c); };
                    bool capturedTypeUsed = false;
                    std::function<Type(Node*)> lockNode;
                    lockNode = [&](Node* n) -> Type {
                        if (!n) return Ty::Any;
                        if (n->type == NodeTypes::NodeVariable) {
                            String nm = static_cast<struct NodeVariable*>(n)->name;
                            if (context.has_variable_local(nm)) {
                                ContextObject& co = context.get_variable_local(nm);
                                if (co.value.T == Ty::Type_) {
                                    capturedTypeUsed = true;
                                    return co.value.get<Type>();
                                }
                            }
                            if (isGenericName(nm, func)) return Type(TypeType::BASE, nm);
                            if (nm == "auto") return Ty::Auto;
                            if (nm == "any")  return Ty::Any;
                            if (!context.has_variable(nm)) return Ty::Any;
                            Value v = eval(n, context);
                            return (v.T == Ty::Type_) ? v.get<Type>() : Ty::Any;
                        }
                        if (n->type == NodeTypes::NodeUnaryDereference)
                            return *(lockNode(static_cast<struct NodeUnaryDereference*>(n)->node));
                        // `?T` = T | Null; зеркалит ветку в declTypeNode (Functions.cpp),
                        // без неё generic-сигнатура с nullable-типом не лендилась.
                        if (n->type == NodeTypes::NodeUnaryNegation) {
                            auto* un = static_cast<struct NodeUnaryNegation*>(n);
                            if (un->token.value == "?")
                                return lockNode(un->node) | Ty::Null;
                        }
                        if (n->type == NodeTypes::NodeBinaryBitwiseOr || n->type == NodeTypes::NodeBinaryLogicalOr) {
                            auto* bn = static_cast<struct NodeBinaryBitwiseOr*>(n);
                            return lockNode(bn->l_node) | lockNode(bn->r_node);
                        }
                        Value v = eval(n, context);
                        return (v.T == Ty::Type_) ? v.get<Type>() : Ty::Any;
                    };
                    std::vector<Type> lockedParams;
                    Type lockedRet = Ty::Any;
                    for (size_t pi = 0; pi < func.arguments.size(); ++pi)
                        lockedParams.push_back(lockNode(func.arguments[pi].type));
                    if (func.return_type) lockedRet = lockNode(func.return_type);
                    if (capturedTypeUsed) {
                        func.reified_params = std::move(lockedParams);
                        func.reified_ret = lockedRet;
                        func.locked_types = true;
                    }
                }

                Context& target = active_global ? global_context : context;
                if (!target.has_function_container_local(func_node->name)) {
                    target.add_function_container(func_node->name, FunctionContainer(func_node->name));
                }
                FunctionContainer& fc = target.get_function_container_local(func_node->name);
                // РєРѕРЅС‚РµР№РЅРµСЂ РїРµСЂРµРіСЂСѓР·РѕРє С„СѓРЅРєС†РёСЏРј РЅСѓР¶РµРЅ СЃС‚Р°Р±РёР»СЊРЅС‹Р№ Р°РґСЂРµСЃ РІ РєСѓС‡Рµ:
                // `&РёРјСЏ` РІРѕР·РІСЂР°С‰Р°РµС‚ СѓРєР°Р·Р°С‚РµР»СЊ РЅР° РЅРµРіРѕ (РёРЅР°С‡Рµ addr=0 -> РЅСѓР»РµРІРѕР№ СѓРєР°Р·Р°С‚РµР»СЊ)
                ContextObject& fn_stored = target.get_variable_local(func_node->name);
                if (fn_stored.addr == 0)
                    fn_stored.addr = heap.allocate_high(&fn_stored);
                if (fc.has_duplicate_signature(func)) {
                    error_container.addError(DEEP_ERRORS::CALL_DUPLICATE_SIGNATURE(
                        func_node->name_token, func_node->name, signatureKey(func)));
                    break;
                }
                fc.add_function(func);

                // РѕР±РЅРѕРІР»СЏРµРј reified-С‚РёРї РєРѕРЅС‚РµР№РЅРµСЂР° (РґР»СЏ typeof(РёРјСЏ))
                if (target.has_variable_local(func_node->name)) {
                    ContextObject& stored = target.get_variable_local(func_node->name);
                    stored.value.T = fc.callableType();
                    stored.wait_type = stored.value.T;
                    stored.modifiers = active_mods;
                }
                break;
            }

            case NodeTypes::NodeNumber: {
                auto num_node = static_cast<struct NodeNumber*>(node);
                if (num_node->is_integer) {
                    return new_i64(num_node->int_value);
                } else {
                    return new_f64(num_node->float_value);
                }
            }

            case NodeTypes::NodeVariable: {
                auto var_node = static_cast<struct NodeVariable*>(node);
                if (!context.has_variable(var_node->name)) {
                    error_container.addError(DEEP_ERRORS::UNDEFINED_VARIABLE(var_node->token, var_node->name));
                    aborted = true;
                    return new_null();
                }
                ContextObject& covar = context.get_variable(var_node->name);
                // ref-алиас читается «насквозь»: возвращаем актуальное значение цели
                Value v = new_null();
                if (covar.ref_alias_node) {
                    if (++ref_chain_depth > REF_CHAIN_LIMIT) {
                        --ref_chain_depth;
                        refCycleError(var_node->token, var_node->name);
                        return new_null();
                    }
                    v = execute(covar.ref_alias_node, context);
                    --ref_chain_depth;
                } else {
                    v = covar.value;
                }
                // Без неявной автораспаковки: чтение переменной-обёртки
                // возвращает сам объект. Чтобы получить значение, нужен
                // явный постфиксный `?` — `echo r?;`.
                return v;
            }

            case NodeTypes::NodeScope: {
                auto scope_node = static_cast<struct NodeScope*>(node);
                return execute(scope_node->node, context);
                break;
            }

            case NodeTypes::NodeString: {
                auto str_node = static_cast<struct NodeString*>(node);
                if (str_node->is_char && str_node->value.size() == 1) {
                    return new_char(str_node->value[0]);
                } else {
                    return new_string(str_node->value);
                }
            }

            case NodeTypes::NodeBool: {
                auto bool_node = static_cast<struct NodeBool*>(node);
                return new_bool(bool_node->value);
            }

            case NodeTypes::NodeNull: {
                return new_null();
            }

            case NodeTypes::NodeTupleType: {
                std::vector<Type> ts;
                for (auto* t : static_cast<struct NodeTupleType*>(node)->types)
                    ts.push_back(evalTypeExpr(t, context));
                return new_type(Type::TupleType(std::move(ts)));
            }

            case NodeTypes::NodeArrayType: {
                auto* at_node = static_cast<struct NodeArrayType*>(node);
                Type t = evalTypeExpr(at_node, context);
                // `Array[T, N]{…}` — конструктор: тип элемента и длина заданы в
                // `Array[…]`, тело — позиционный список. Без тела остаётся
                // обычное типовое выражение (`let x: Array[i64] = …`).
                if (!at_node->has_body) return new_type(t);

                std::vector<Value> items;
                items.reserve(at_node->items.size());
                for (Node* item : at_node->items) items.push_back(execute(item, context));

                const Type& elem_decl = t.container_1.empty() ? Ty::Any : t.container_1[0];
                const int declared = t.size;
                // `Array[T, N]{}` — ПУСТОЙ массив: элементы не создаются, длина 0.
                // N в этой форме не наполняет массив, а лишь объявляет тип
                // (`Array[T, N]` остаётся типом) и проверяет список в
                // `Array[T, N]{a, b}`. Обращение к элементу пустого массива даёт
                // ARRAY_INDEX_OUT_OF_RANGE(80).
                // Динамический массив: длина значения не равна N, даже когда тело
                // пустое — иначе `fn v[s: i64]() -> Array[any, s] : { ret Array[any, s]{}; }`
                // падал бы с error[98] «expected '{any, 10}', found '{any, 0}'».
                // N в пустом теле только объявляет тип, но не длину значения.
                if (!at_node->items.empty()) {
                    if (declared != -1 && static_cast<int>(items.size()) != declared) {
                        error_container.addError(DEEP_ERRORS::ARRAY_CTOR_SIZE_MISMATCH(
                            at_node->close_token, elem_decl.toString(), declared,
                            static_cast<long long>(items.size())));
                    }
                    // Элемент проверяется против объявленного типа контейнера, а не
                    // выводится как в `{…}`: `Array[i64, 3]{1, "a"}` — ошибка.
                    for (size_t i = 0; i < items.size(); ++i) {
                        if (elem_decl.is_auto || elem_decl.is_any) continue;
                        if (isNumericLiteralNode(at_node->items[i]) && isNumberType(elem_decl))
                            items[i] = adaptNumericLiteral(items[i], elem_decl);
                        if (!(elem_decl >> items[i].T)) {
                            error_container.addError(DEEP_ERRORS::ARRAY_CTOR_ELEMENT_MISMATCH(
                                at_node->open_token, elem_decl.toString(), items[i].T.toString()));
                        }
                    }
                }
                // Длина в типе значения = длина фактических элементов. `declared` в типе НЕ
                // подставляем: у `Array[any, size]` при `size = 10` длина 0, и
                // подстановка дала бы рассинхрон типа значения с `sizeof`.
                // `strict let s: Array[i64, 3] = Array[i64, 3]{1, 2, 3}` проходит
                // и через это, т.к. проверка идёт по assign-типу узла-источника.
                Type arr_ty = Type::ArrayType(elem_decl, static_cast<int>(items.size()));
                return new_array(arr_ty, std::move(items));
            }

            case NodeTypes::NodeArray: {
                auto* arr_node = static_cast<struct NodeArray*>(node);
                std::vector<Value> items;
                items.reserve(arr_node->elements.size());
                // элемент-алиас (`{a_ref, b_ref}`): кладём в массив живой алиас,
                // чтобы `ref_arr[i] = v` писал сквозь ссылку в цель (зеркало
                // transpile-кодгена, где ref-элемент — `rt::refalias`).
                std::map<size_t, std::pair<Node*, Context*>> rarefs;
                for (size_t ei = 0; ei < arr_node->elements.size(); ++ei) {
                    Node* el = arr_node->elements[ei];
                    items.push_back(execute(el, context));
                    while (el && el->type == NodeTypes::NodeScope)
                        el = static_cast<struct NodeScope*>(el)->node;
                    if (el && el->type == NodeTypes::NodeVariable) {
                        auto* ev = static_cast<struct NodeVariable*>(el);
                        if (context.has_variable(ev->name)) {
                            ContextObject& co = context.get_variable(ev->name);
                            if (co.ref_alias_node)
                                rarefs[ei] = {co.ref_alias_node,
                                              co.ref_alias_ctx ? co.ref_alias_ctx : &context};
                        }
                    }
                }
                Type elem = Ty::Auto;
                for (size_t i = 0; i < items.size(); ++i) {
                    if (i == 0) elem = items[i].T;
                    else       elem = commonElementType(elem, items[i].T);
                }
                if (items.empty()) elem = Ty::Any;
                const int n = static_cast<int>(items.size());
                Value arrv = new_array(Type::ArrayType(elem, n), std::move(items));
                if (!rarefs.empty()) arrv.get_ref<RuntimeArray>().refs = std::move(rarefs);
                return arrv;
            }

            case NodeTypes::NodeArrayAccess: {
                auto* acc_node = static_cast<struct NodeArrayAccess*>(node);
                Value cur = execute(acc_node->base, context);
                // `Vec[i64, 8]` — generic-инстанс структуры как тип
                if (cur.T == Ty::Type_ && cur.get<Type>().isStruct()) {
                    String sname = cur.get<Type>().name;
                    auto sit = structs.find(sname);
                    if (sit == structs.end()) {
                        error_container.addError(DEEP_ERRORS::STRUCT_NOT_FOUND(acc_node->token, sname));
                        return new_null();
                    }
                    std::vector<Value> bindings;
                    Type inst = Ty::Any;
                    if (!buildStructInstance(sit->second, acc_node->indices, acc_node->token,
                                             context, bindings, inst))
                        return new_null();
                    return new_type(inst);
                }
                if (acc_node->indices.empty()) return cur;
                for (size_t ii = 0; ii < acc_node->indices.size(); ++ii) {
                    Node* idx_node = acc_node->indices[ii];
                    // dunder-индексация: объект struct с методом __get_index__
                    if (cur.T.isStruct()) {
                        auto gsit = structs.find(cur.T.name);
                        if (gsit != structs.end()
                                && gsit->second.methods.count("__get_index__") > 0) {
                            std::vector<Value> ivals;
                            for (size_t jj = ii; jj < acc_node->indices.size(); ++jj)
                                ivals.push_back(execute(acc_node->indices[jj], context));
                            Value dnd(Ty::Null, nullptr);
                            if (tryDunder("__get_index__", cur, ivals, context,
                                          acc_node->token, acc_node->token, dnd))
                                return dnd;
                            break;   // __get_index__ съедает все оставшиеся индексы
                        }
                    }
                    if (cur.T == Ty::Str) {
                        Value iv = execute(idx_node, context);
                        if (!isIntegerType(iv.T)) {
                            error_container.addError(DEEP_ERRORS::ARRAY_INDEX_NOT_INTEGER(acc_node->token, iv.T.toString()));
                            return new_null();
                        }
                        long long si = static_cast<long long>(getNumberFromType<>(iv));
                        String sval = cur.get<String>();
                        if (si < 0 || si >= static_cast<long long>(sval.size())) {
                            error_container.addError(DEEP_ERRORS::ARRAY_INDEX_OUT_OF_RANGE(acc_node->token, si, static_cast<long long>(sval.size())));
                            return new_null();
                        }
                        cur = new_char(sval[static_cast<size_t>(si)]);
                        continue;
                    }
                    if (!cur.T.isArray() && !cur.T.isTuple()) {
                        error_container.addError(DEEP_ERRORS::CANNOT_INDEX(acc_node->token, cur.T.toString()));
                        return new_null();
                    }
                    Value iv = execute(idx_node, context);
                    if (!isIntegerType(iv.T)) {
                        error_container.addError(DEEP_ERRORS::ARRAY_INDEX_NOT_INTEGER(acc_node->token, iv.T.toString()));
                        return new_null();
                    }
                    long long idx = static_cast<long long>(getNumberFromType<>(iv));
                    const RuntimeArray& arr = cur.get<RuntimeArray>();
                    if (idx < 0 || idx >= static_cast<long long>(arr.size())) {
                        error_container.addError(DEEP_ERRORS::ARRAY_INDEX_OUT_OF_RANGE(acc_node->token, idx, static_cast<long long>(arr.size())));
                        return new_null();
                    }
                    // живой элемент-алиас (`{a_ref, ...}`): читаем актуальное
                    // значение цели, а не скопированное при сборке литерала.
                    auto ait = arr.refs.find(static_cast<size_t>(idx));
                    if (ait != arr.refs.end()) {
                        Node* an = ait->second.first;
                        Context* actx = ait->second.second ? ait->second.second : &context;
                        if (++ref_chain_depth > REF_CHAIN_LIMIT) {
                            --ref_chain_depth;
                            refCycleError(acc_node->token, "ref-array element");
                            return new_null();
                        }
                        cur = execute(an, *actx);
                        --ref_chain_depth;
                    } else {
                        cur = arr.view()[static_cast<size_t>(idx)];
                    }
                }
                return cur;
            }

            case NodeTypes::NodeMemberAccess: {
                auto* ma_n = static_cast<struct NodeMemberAccess*>(node);
                String field = memberNameNode(ma_n->r_node);
                // объект берём как есть (без автораспаковки __get__): поле
                // принадлежит самой обёртке
Value b = execute(ma_n->l_node, context);
                  if (b.T.isPointer() && ma_n->l_node->type != NodeTypes::NodeUnaryDereference) {
                      error_container.addError(DEEP_ERRORS::IMPLICIT_POINTER_MEMBER(
                          ma_n->member_token, b.T.toString(), field));
                      return new_null();
                  }
                  if (b.T.isPointer()) {   // авто-разыменование
                    Pointer p = b.get<Pointer>();
                    auto fs = field_slots.find(p.addr);
                    if (fs != field_slots.end()) {
                        RuntimeStruct& rs = fs->second.first->value.get_ref<RuntimeStruct>();
                        auto fit = rs.view().find(fs->second.second);
                        if (fit == rs.view().end()) {
                            error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                                ma_n->member_token, fs->second.first->value.T.name, fs->second.second));
                            return new_null();
                        }
                        b = fit->second;
                    } else {
                        if (p.addr == 0 || !heap.contains(p.addr)) {
                            error_container.addError(DEEP_ERRORS::NULL_DEREFERENCE(ma_n->member_token));
                            return new_null();
                        }
                        b = heap.get(p.addr)->value;
                    }
                }
                if (b.T.isStruct()) {
                    const RuntimeStruct& rs = b.get<RuntimeStruct>();
                    const StructSymbol* sym = structSymForType(b.T);
                    const StructFieldDef* fd = sym ? sym->field_def(field) : nullptr;
                    if (fd && fd->mods.Private && active_struct != sym) {
                        error_container.addError(DEEP_ERRORS::PRIVATE_FIELD(
                            ma_n->member_token, b.T.name, field));
                        return new_null();
                    }
                    auto it = rs.view().find(field);
                    if (it != rs.view().end()) return it->second;
                    error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                        ma_n->member_token, b.T.name, field));
                    return new_null();
                }
                error_container.addError(DEEP_ERRORS::MEMBER_OF_NON_STRUCT(
                    ma_n->member_token, b.T.toString(), field));
                return new_null();
            }

            case NodeTypes::NodeStructLiteral: {
                auto* sl_n = static_cast<struct NodeStructLiteral*>(node);
                Type base = evalTypeExpr(sl_n->type_expr, context);
                if (!base.isStruct()) {
                    error_container.addError(DEEP_ERRORS::EXPECTED_STRUCT_TYPE(
                        sl_n->open_token, base.toString()));
                    return new_null();
                }
                auto sit = structs.find(base.name);
                if (sit == structs.end()) {
                    error_container.addError(DEEP_ERRORS::STRUCT_NOT_FOUND(
                        sl_n->open_token, base.name));
                    return new_null();
                }
                // Транспилятор берёт имя структуры из type_expr только когда
                // там имя переменной; для `mk(){…}` / локальных структур он
                // иначе не узнает имя и печатает object(?).
                sl_n->struct_name = base.name;
                std::vector<Value> bindings;
                Type inst = Ty::Any;
                // Если тип-выражение уже вернуло конкретизированный тип
                // (например, `get_struct(i64){...}`, где функция вернула
                // `Generic[i64]`), берём его аргументы как есть: пересборка
                // по дефолтам заново вычислила бы их в другом контексте.
                if (sl_n->generics.empty() && isReifiedStructType(base)) {
                    if (!bindingsFromReified(sit->second, base, bindings)) {
                        error_container.addError(DEEP_ERRORS::STRUCT_GENERIC_ARG_COUNT(
                            sl_n->open_token, base.name, 0,
                            static_cast<long long>(sit->second.generics.size())));
                        return new_null();
                    }
                    inst = base;
                }
                else if (!buildStructInstance(sit->second, sl_n->generics, sl_n->open_token,
                                             context, bindings, inst))
                    return new_null();
                std::map<String, Value> fields;
                if (!materializeStruct(sit->second, bindings, sl_n->inits,
                                       context, sl_n->open_token, fields))
                    return new_null();
                return new_struct(inst, std::move(fields));
            }

            case NodeTypes::NodeUnaryMinus: {
                auto unary_minus_node = static_cast<struct NodeUnaryMinus*>(node);
                Value value = execute(unary_minus_node->node, context);
                Value dnd = new_null();
                if (tryDunder("__neg__", value, {}, context, unary_minus_node->token, unary_minus_node->token, dnd)) return dnd;
                if (!isNumberType(value.T)) {
                    error_container.addError(DEEP_ERRORS::UNARY_MINUS_INVALID_TYPE(unary_minus_node->token, value.T.toString()));
                    return new_null();
                }
                return negationNumber(value);
            }

case NodeTypes::NodeUnaryPlus: {
                auto unary_plus_node = static_cast<struct NodeUnaryPlus*>(node);
                Value value = execute(unary_plus_node->node, context);
                Value dnd = new_null();
                if (tryDunder("__pos__", value, {}, context, unary_plus_node->token, unary_plus_node->token, dnd)) return dnd;
                // унарный + не меняет значение (число уже вычислено)
                return execute(unary_plus_node->node, context);
            }

            case NodeTypes::NodeBinaryAdditive: {
                auto add_node = static_cast<struct NodeBinaryAdditive*>(node);
                Value l = execute(add_node->l_node, context);
                Value r = execute(add_node->r_node, context);
                Value dund = new_null();
                if (tryDunder("__add__", l, {r}, context, add_node->start, add_node->end, dund)) return dund;
                return binaryMath(l, r, '+', add_node->start, add_node->end);
            }

            case NodeTypes::NodeBinarySubtraction: {
                auto sub_node = static_cast<struct NodeBinarySubtraction*>(node);
                Value l = execute(sub_node->l_node, context);
                Value r = execute(sub_node->r_node, context);
                Value dund = new_null();
                if (tryDunder("__sub__", l, {r}, context, sub_node->start, sub_node->end, dund)) return dund;
                return binaryMath(l, r, '-', sub_node->start, sub_node->end);
            }

            case NodeTypes::NodeBinaryMultiplicative: {
                auto mul_node = static_cast<struct NodeBinaryMultiplicative*>(node);
                Value l = execute(mul_node->l_node, context);
                Value r = execute(mul_node->r_node, context);
                Value dund = new_null();
                if (tryDunder("__mul__", l, {r}, context, mul_node->start, mul_node->end, dund)) return dund;
                return binaryMath(l, r, '*', mul_node->start, mul_node->end);
            }

            case NodeTypes::NodeBinaryDivision: {
                auto div_node = static_cast<struct NodeBinaryDivision*>(node);
                Value l = execute(div_node->l_node, context);
                Value r = execute(div_node->r_node, context);
                Value dund = new_null();
                if (tryDunder("__truediv__", l, {r}, context, div_node->start, div_node->end, dund)) return dund;
                if (isZeroValue(r)) {
                    error_container.addError(DEEP_ERRORS::DIVISION_BY_ZERO(div_node->start, div_node->end));
                    return new_null();
                }
                return binaryMath(l, r, '/', div_node->start, div_node->end);
            }

            case NodeTypes::NodeBinaryRemainder: {
                auto rem_node = static_cast<struct NodeBinaryRemainder*>(node);
                Value l = execute(rem_node->l_node, context);
                Value r = execute(rem_node->r_node, context);
                Value dund = new_null();
                if (tryDunder("__mod__", l, {r}, context, rem_node->start, rem_node->end, dund)) return dund;
                if (isZeroValue(r)) {
                    error_container.addError(DEEP_ERRORS::MODULO_BY_ZERO(rem_node->start, rem_node->end));
                    return new_null();
                }
                return binaryMath(l, r, '%', rem_node->start, rem_node->end);
            }

            case NodeTypes::NodeUnaryNegation: {
                auto unary_node = static_cast<struct NodeUnaryNegation*>(node);
                Value value = execute(unary_node->node, context);
                Value dnd = new_null();
                if (tryDunder("__bool__", value, {}, context, unary_node->token, unary_node->token, dnd)) {
                    if (dnd.T != Ty::Bool) {
                        error_container.addError(DEEP_ERRORS::UNARY_NEGETATION_NOT_INVALID_TYPE(unary_node->token, value.T.toString()));
                        return new_null();
                    }
                    return new_bool(!dnd.get<bool>());
                }
                if (value.T.isArray()) {
                    // !{ ... } вЂ” РјР°СЃСЃРёРІ В«РїСЂРµРІСЂР°С‰Р°РµС‚СЃСЏВ» РІ РєРѕСЂС‚РµР¶: С‚РёРї РїРѕ РєР°Р¶РґРѕРјСѓ СЌР»РµРјРµРЅС‚Сѓ
                    const RuntimeArray& arr = value.get<RuntimeArray>();
                    std::vector<Type> ts;
                    ts.reserve(arr.size());
                    for (const auto& it : arr.view()) ts.push_back(it.T);
                    return Value(Type::TupleType(std::move(ts)), RuntimeArray(arr));
                }
                if (value.T == Ty::Bool) {
                    return new_bool(!value.get<bool>());
                } else if (isNumberType(value.T)) {
                    return new_bool(getNumberFromType<>(value) == 0.0);
                } else if (value.T == Ty::Type_ && unary_node->token.value == "?") {
                    return new_type(value.get<Type>() | Ty::Null);
                } else {
                    error_container.addError(DEEP_ERRORS::UNARY_NEGETATION_NOT_INVALID_TYPE(unary_node->token, value.T.toString()));
                    return new_null();
                }
            }

            case NodeTypes::NodeUnaryBitwiseNot: {
                auto unary_node = static_cast<struct NodeUnaryBitwiseNot*>(node);
                Value value = execute(unary_node->node, context);
                Value dnd = new_null();
                if (tryDunder("__invert__", value, {}, context, unary_node->token, unary_node->token, dnd)) return dnd;
                if (value.T == Ty::i8)  return new_i8(static_cast<int8_t>(~static_cast<int8_t>(value.get<int8_t>())));
                if (value.T == Ty::i16) return new_i16(static_cast<int16_t>(~static_cast<int16_t>(value.get<int16_t>())));
                if (value.T == Ty::i32) return new_i32(~value.get<int32_t>());
                if (value.T == Ty::i64) return new_i64(~value.get<int64_t>());
                error_container.addError(DEEP_ERRORS::UNARY_BITWISE_NOT_INVALID_TYPE(unary_node->token, value.T.toString()));
                return new_null();
            }

            case NodeTypes::NodeUnaryDereference: {
                auto unary_node = static_cast<struct NodeUnaryDereference*>(node);
                Value value = execute(unary_node->node, context);
                if (value.T.isPointer()) {
                    Pointer p = value.get<Pointer>();
                    auto fs = field_slots.find(p.addr);
                    if (fs != field_slots.end()) {
                        RuntimeStruct& rs = fs->second.first->value.get_ref<RuntimeStruct>();
                        auto fit = rs.view().find(fs->second.second);
                        if (fit == rs.view().end()) {
                            error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                                unary_node->token, fs->second.first->value.T.name, fs->second.second));
                            return new_null();
                        }
                        return fit->second;
                    }
                    if (p.addr == 0 || !heap.contains(p.addr)) {
                        error_container.addError(DEEP_ERRORS::NULL_DEREFERENCE(unary_node->token));
                        return new_null();
                    }
                    return heap.get(p.addr)->value;
                } else if (value.T == Ty::Type_) {
                    return new_type(*value.get<Type>());
                } else {
                    error_container.addError(DEEP_ERRORS::UNARY_DEREFERENCE_NOT_INVALID_TYPE(unary_node->token, value.T.toString()));
                    return new_null();
                }
            }

            case NodeTypes::NodeUnaryPostfixIncrement:
            case NodeTypes::NodeUnaryPostfixDecrement:
            case NodeTypes::NodeUnaryPrefixIncrement:
            case NodeTypes::NodeUnaryPrefixDecrement: {
                Node* operand = nullptr; Token tok;
                bool is_inc = false;
                bool is_prefix = (node->type == NodeTypes::NodeUnaryPrefixIncrement
                               || node->type == NodeTypes::NodeUnaryPrefixDecrement);
                if (node->type == NodeTypes::NodeUnaryPostfixIncrement
                 || node->type == NodeTypes::NodeUnaryPrefixIncrement) {
                    operand = static_cast<struct NodeUnaryPostfixIncrement*>(node)->node;
                    tok = static_cast<struct NodeUnaryPostfixIncrement*>(node)->token;
                    is_inc = true;
                } else {
                    operand = static_cast<struct NodeUnaryPostfixDecrement*>(node)->node;
                    tok = static_cast<struct NodeUnaryPostfixDecrement*>(node)->token;
                }
                Value old = execute(operand, context);
                Node* delta = new struct NodeNumber(1);
                Node* rhs = is_inc
                    ? static_cast<Node*>(new struct NodeBinaryAdditive(operand, delta, tok, tok))
                    : static_cast<Node*>(new struct NodeBinarySubtraction(operand, delta, tok, tok));
                Node* assign = new struct NodeBinaryAssignment(operand, rhs, tok, tok);
                execute(assign, context);
                // префиксный ++/-- возвращает НОВОЕ значение операнда
                return is_prefix ? execute(operand, context) : old;
            }
            // постфиксный ? : развернуть значение через __get__, если метод
            // есть; иначе вернуть сам объект. maybeAutoGet делает ровно это —
            // цикл по __get__, а при его отсутствии значение не меняется.
            case NodeTypes::NodePostfixUnwrap: {
                auto un = static_cast<struct NodePostfixUnwrap*>(node);
                Value v = execute(un->node, context);
                return maybeAutoGet(v, context, un->token);
            }

            // РўРёРї-С„СѓРЅРєС†РёСЏ: Func[G, ...](P, ...) -> R  в†’ Р·РЅР°С‡РµРЅРёРµ-С‚РёРї (CALLABLE).
            case NodeTypes::NodeFunctionType: {
                auto fty = static_cast<struct NodeFunctionType*>(node);
                auto toType = [&](Node* nd) -> Type {
                    if (!nd) return Ty::Any;
                    Value v = execute(nd, context);
                    return (v.T == Ty::Type_) ? v.get<Type>() : Ty::Any;
                };
                std::vector<Type> gs, ps;
                for (auto* g : fty->generics) gs.push_back(toType(g));
                for (auto* p : fty->params)   ps.push_back(toType(p));
                Type rt = toType(fty->return_type);
                return new_type(Type::FunctionType(ps, rt, gs));
            }

            case NodeTypes::NodeBinaryEqual: {
                auto binary_node = static_cast<struct NodeBinaryEqual*>(node);
                Value left = execute(binary_node->l_node, context);
                Value right = execute(binary_node->r_node, context);
                Value dund = new_null();
                if (tryDunder("__eq__", left, {right}, context, binary_node->start, binary_node->end, dund)) return dund;

                // Числовое равенство с приведением: 1 == 1.0 -> true (согласовано с порядком <).
                if (isNumberType(left.T) && isNumberType(right.T))
                    return new_bool(getNumberFromType<>(left) == getNumberFromType<>(right));

                if (left.T != right.T)
                    return new_bool(false);

                if (isNumberType(left.T))
                    return new_bool(getNumberFromType<>(left) == getNumberFromType<>(right));
                if (left.T == Ty::Bool)
                    return new_bool(left.get<bool>() == right.get<bool>());
                if (left.T == Ty::Str)
                    return new_bool(left.get<String>() == right.get<String>());
                if (left.T == Ty::Char)
                    return new_bool(left.get<char32_t>() == right.get<char32_t>());
                if (left.T == Ty::Null)
                    return new_bool(true);
                if (left.T == Ty::Type_)
                    return new_bool(left.get<Type>() == right.get<Type>());
                if (left.T.isEnum())
                    return new_bool(left.get<long long>() == right.get<long long>());
                if (left.T.isPointer() && right.T.isPointer())
                    return new_bool(left.get<Pointer>().addr == right.get<Pointer>().addr);
                error_container.addError(DEEP_ERRORS::BINARY_COMPARE_INVALID_TYPE(binary_node->start, binary_node->end, left.T.toString(), right.T.toString(), "=="));
                return new_bool(false);
            }

            case NodeTypes::NodeBinaryNotEqual: {
                auto binary_node = static_cast<struct NodeBinaryNotEqual*>(node);
                Value left = execute(binary_node->l_node, context);
                Value right = execute(binary_node->r_node, context);
                Value dund = new_null();
                if (tryDunder("__ne__", left, {right}, context, binary_node->start, binary_node->end, dund)) return dund;

                // Числовое неравенство с приведением: 1 != 1.0 -> false.
                if (isNumberType(left.T) && isNumberType(right.T))
                    return new_bool(getNumberFromType<>(left) != getNumberFromType<>(right));

                if (left.T != right.T)
                    return new_bool(true);
                if (isNumberType(left.T))
                    return new_bool(getNumberFromType<>(left) != getNumberFromType<>(right));
                if (left.T == Ty::Bool)
                    return new_bool(left.get<bool>() != right.get<bool>());
                if (left.T == Ty::Str)
                    return new_bool(left.get<String>() != right.get<String>());
                if (left.T == Ty::Char)
                    return new_bool(left.get<char32_t>() != right.get<char32_t>());
                if (left.T == Ty::Null)
                    return new_bool(false);
                if (left.T == Ty::Type_)
                    return new_bool(left.get<Type>() != right.get<Type>());
                if (left.T.isEnum())
                    return new_bool(left.get<long long>() != right.get<long long>());
                if (left.T.isPointer() && right.T.isPointer())
                    return new_bool(left.get<Pointer>().addr != right.get<Pointer>().addr);
                error_container.addError(DEEP_ERRORS::BINARY_COMPARE_INVALID_TYPE(binary_node->start, binary_node->end, left.T.toString(), right.T.toString(), "!="));
                return new_bool(false);
            }

            case NodeTypes::NodeBinaryLess: {
                auto binary_node = static_cast<struct NodeBinaryLess*>(node);
                Value left = execute(binary_node->l_node, context);
                Value right = execute(binary_node->r_node, context);

                Value dund = new_null();
                if (tryDunder("__lt__", left, {right}, context, binary_node->start, binary_node->end, dund)) return dund;
                if (isNumberType(left.T)) {
                    if (!isNumberType(right.T)) {
                        error_container.addError(DEEP_ERRORS::BINARY_COMPARE_INVALID_TYPE(binary_node->start, binary_node->end, left.T.toString(), right.T.toString(), "<"));
                        return new_bool(false);
                    }
                    return new_bool(getNumberFromType<>(left) < getNumberFromType<>(right));
                } else if (left.T.isEnum()) {
                    return new_bool(left.get<long long>() < right.get<long long>());
                } else {
                    error_container.addError(DEEP_ERRORS::BINARY_COMPARE_INVALID_TYPE(binary_node->start, binary_node->end, left.T.toString(), right.T.toString(), "<"));
                    return new_bool(false);
                }
            }

            case NodeTypes::NodeBinaryGreater: {
                auto binary_node = static_cast<struct NodeBinaryGreater*>(node);
                Value left = execute(binary_node->l_node, context);
                Value right = execute(binary_node->r_node, context);

                Value dund = new_null();
                if (tryDunder("__gt__", left, {right}, context, binary_node->start, binary_node->end, dund)) return dund;
                if (isNumberType(left.T)) {
                    if (!isNumberType(right.T)) {
                        error_container.addError(DEEP_ERRORS::BINARY_COMPARE_INVALID_TYPE(binary_node->start, binary_node->end, left.T.toString(), right.T.toString(), ">"));
                        return new_bool(false);
                    }
                    return new_bool(getNumberFromType<>(left) > getNumberFromType<>(right));
                } else if (left.T.isEnum()) {
                    return new_bool(left.get<long long>() > right.get<long long>());
                } else {
                    error_container.addError(DEEP_ERRORS::BINARY_COMPARE_INVALID_TYPE(binary_node->start, binary_node->end, left.T.toString(), right.T.toString(), ">"));
                    return new_bool(false);
                }
            }

            case NodeTypes::NodeBinaryLessEqual: {
                auto binary_node = static_cast<struct NodeBinaryLessEqual*>(node);
                Value left = execute(binary_node->l_node, context);
                Value right = execute(binary_node->r_node, context);

                Value dund = new_null();
                if (tryDunder("__le__", left, {right}, context, binary_node->start, binary_node->end, dund)) return dund;
                if (isNumberType(left.T)) {
                    if (!isNumberType(right.T)) {
                        error_container.addError(DEEP_ERRORS::BINARY_COMPARE_INVALID_TYPE(binary_node->start, binary_node->end, left.T.toString(), right.T.toString(), "<="));
                        return new_bool(false);
                    }
                    return new_bool(getNumberFromType<>(left) <= getNumberFromType<>(right));
                } else if (left.T.isEnum()) {
                    return new_bool(left.get<long long>() <= right.get<long long>());
                } else {
                    error_container.addError(DEEP_ERRORS::BINARY_COMPARE_INVALID_TYPE(binary_node->start, binary_node->end, left.T.toString(), right.T.toString(), "<="));
                    return new_bool(false);
                }
            }

            case NodeTypes::NodeBinaryGreaterEqual: {
                auto binary_node = static_cast<struct NodeBinaryGreaterEqual*>(node);
                Value left = execute(binary_node->l_node, context);
                Value right = execute(binary_node->r_node, context);

                Value dund = new_null();
                if (tryDunder("__ge__", left, {right}, context, binary_node->start, binary_node->end, dund)) return dund;
                if (isNumberType(left.T)) {
                    if (!isNumberType(right.T)) {
                        error_container.addError(DEEP_ERRORS::BINARY_COMPARE_INVALID_TYPE(binary_node->start, binary_node->end, left.T.toString(), right.T.toString(), ">="));
                        return new_bool(false);
                    }
                    return new_bool(getNumberFromType<>(left) >= getNumberFromType<>(right));
                } else if (left.T.isEnum()) {
                    return new_bool(left.get<long long>() >= right.get<long long>());
                } else {
                    error_container.addError(DEEP_ERRORS::BINARY_COMPARE_INVALID_TYPE(binary_node->start, binary_node->end, left.T.toString(), right.T.toString(), ">="));
                    return new_bool(false);
                }
            }

            case NodeTypes::NodeBinaryLogicalAnd: {
                auto binary_node = static_cast<struct NodeBinaryLogicalAnd*>(node);
                Value left = execute(binary_node->l_node, context);
                if (left.T != Ty::Bool) {
                    error_container.addError(DEEP_ERRORS::BINARY_COMPARE_INVALID_TYPE(binary_node->start, binary_node->end, left.T.toString(), left.T.toString(), "&&"));
                    return new_bool(false);
                }
                // короткое замыкание: левый false => правый не вычисляется
                if (!left.get<bool>()) return new_bool(false);
                Value right = execute(binary_node->r_node, context);
                if (right.T != Ty::Bool) {
                    error_container.addError(DEEP_ERRORS::BINARY_COMPARE_INVALID_TYPE(binary_node->start, binary_node->end, left.T.toString(), right.T.toString(), "&&"));
                    return new_bool(false);
                }
                return new_bool(right.get<bool>());
            }

            case NodeTypes::NodeBinaryLogicalOr: {
                auto binary_node = static_cast<struct NodeBinaryLogicalOr*>(node);
                Value left = execute(binary_node->l_node, context);
                if (left.T != Ty::Bool) {
                    error_container.addError(DEEP_ERRORS::BINARY_COMPARE_INVALID_TYPE(binary_node->start, binary_node->end, left.T.toString(), left.T.toString(), "||"));
                    return new_bool(false);
                }
                // короткое замыкание: левый true => правый не вычисляется
                if (left.get<bool>()) return new_bool(true);
                Value right = execute(binary_node->r_node, context);
                if (right.T != Ty::Bool) {
                    error_container.addError(DEEP_ERRORS::BINARY_COMPARE_INVALID_TYPE(binary_node->start, binary_node->end, left.T.toString(), right.T.toString(), "||"));
                    return new_bool(false);
                }
                return new_bool(right.get<bool>());
            }

            // a ?? b  ->  b, если a == null, иначе a. Тип результата — тип a без Null,
            // поэтому nullable-коробка разворачивается в обычный скаляр.
            case NodeTypes::NodeBinaryNullCoalesce: {
                auto nc = static_cast<struct NodeBinaryNullCoalesce*>(node);
                Value left = execute(nc->l_node, context);

                Type result = left.T;   // ниже перезаписывается во всех ветках
                bool left_is_null = false;
                bool needs_right_type = false;   // any/auto: тип результата берётся из правой
                if (left.T.is_any || left.T.is_auto) {
                    needs_right_type = true;
                } else if (left.T == Ty::Null) {
                    left_is_null = true;
                } else if (left.T.isUnion()) {
                    std::vector<Type> members;
                    for (const auto& m : left.T.container_1)
                        if (!m.is_any && !(m == Ty::Null)) members.push_back(m);
                    if (members.empty()) {
                        left_is_null = true;
                    } else if (members.size() == 1) {
                        result = members[0];
                    } else {
                        result = Type(TypeType::UNION);
                        result.container_1 = members;
                    }
                } // иначе result = left.T: левая не nullable, '??' ничего не меняет

                // Правая часть вычисляется лениво — только если левая null/неизвестна.
                // null слева -> берём правую, иначе оставляем левую.
                bool raw_is_null = (left.raw().type() == typeid(Null));
                if (left_is_null || raw_is_null) {
                    Value right = execute(nc->r_node, context);
                    return right;
                }
                if (needs_right_type) {
                    Value right = execute(nc->r_node, context);
                    return Value(right.T, left.raw());
                }
                return Value(result, left.raw());
            }

            case NodeTypes::NodeBinaryBitwiseOr: {
                auto binary_node = static_cast<struct NodeBinaryBitwiseOr*>(node);
                Value left = execute(binary_node->l_node, context);
                Value right = execute(binary_node->r_node, context);
                Value dund = new_null();
                if (tryDunder("__or__", left, {right}, context, binary_node->start, binary_node->end, dund)) return dund;

if (left.T == Ty::Type_ && right.T == Ty::Type_) {
                    return new_type(left.get<Type>() | right.get<Type>());
                } else if (left.T == Ty::Type_ && right.T == Ty::Null) {
                    return new_type(left.get<Type>() | Ty::Null);
                } else if (left.T == Ty::Null && right.T == Ty::Type_) {
                    return new_type(Ty::Null | right.get<Type>());
                } else if (left.T == Ty::Null && right.T == Ty::Null) {
                    return new_type(Ty::Null);
                } else if (isIntegerType(left.T) && isIntegerType(right.T)) {
                    Type rt = resultTypeOf(left.T, right.T);
                    int64_t res = static_cast<int64_t>(getNumberFromType<>(left)) |
                                  static_cast<int64_t>(getNumberFromType<>(right));
                    if (rt == Ty::i8)  return new_i8(static_cast<int8_t>(res));
                    if (rt == Ty::i16) return new_i16(static_cast<int16_t>(res));
                    if (rt == Ty::i32) return new_i32(static_cast<int32_t>(res));
                    if (rt == Ty::i64) return new_i64(res);
                } else {
                    error_container.addError(DEEP_ERRORS::UNSUPPORTED_OPERATOR(binary_node->start, binary_node->end, "|", left.T.toString(), right.T.toString()));
                    return new_null();
                }
            }

            case NodeTypes::NodeBinaryBitwiseAnd: {
                auto binary_node = static_cast<struct NodeBinaryBitwiseAnd*>(node);
                Value left = execute(binary_node->l_node, context);
                Value right = execute(binary_node->r_node, context);
                Value dund = new_null();
                if (tryDunder("__and__", left, {right}, context, binary_node->start, binary_node->end, dund)) return dund;
                if (isIntegerType(left.T) && isIntegerType(right.T)) {
                    int64_t res = static_cast<int64_t>(getNumberFromType<>(left)) &
                                  static_cast<int64_t>(getNumberFromType<>(right));
                    Type rt = resultTypeOf(left.T, right.T);
                    if (rt == Ty::i8)  return new_i8(static_cast<int8_t>(res));
                    if (rt == Ty::i16) return new_i16(static_cast<int16_t>(res));
                    if (rt == Ty::i32) return new_i32(static_cast<int32_t>(res));
                    if (rt == Ty::i64) return new_i64(res);
                } else {
                    error_container.addError(DEEP_ERRORS::UNSUPPORTED_OPERATOR(binary_node->start, binary_node->end, "&", left.T.toString(), right.T.toString()));
                    return new_null();
                }
            }

            case NodeTypes::NodeBinaryBitwiseXor: {
                auto binary_node = static_cast<struct NodeBinaryBitwiseXor*>(node);
                Value left = execute(binary_node->l_node, context);
                Value right = execute(binary_node->r_node, context);
                Value dund = new_null();
                if (tryDunder("__xor__", left, {right}, context, binary_node->start, binary_node->end, dund)) return dund;
                if (isIntegerType(left.T) && isIntegerType(right.T)) {
                    int64_t res = static_cast<int64_t>(getNumberFromType<>(left)) ^
                                  static_cast<int64_t>(getNumberFromType<>(right));
                    Type rt = resultTypeOf(left.T, right.T);
                    if (rt == Ty::i8)  return new_i8(static_cast<int8_t>(res));
                    if (rt == Ty::i16) return new_i16(static_cast<int16_t>(res));
                    if (rt == Ty::i32) return new_i32(static_cast<int32_t>(res));
                    if (rt == Ty::i64) return new_i64(res);
                } else {
                    error_container.addError(DEEP_ERRORS::UNSUPPORTED_OPERATOR(binary_node->start, binary_node->end, "^", left.T.toString(), right.T.toString()));
                    return new_null();
                }
            }

            case NodeTypes::NodeBinaryShiftLeft: {
                auto binary_node = static_cast<struct NodeBinaryShiftLeft*>(node);
                Value left = execute(binary_node->l_node, context);
                Value right = execute(binary_node->r_node, context);
                Value dund = new_null();
                if (tryDunder("__lshift__", left, {right}, context, binary_node->start, binary_node->end, dund)) return dund;
                if (left.T == Ty::Type_ && right.T == Ty::Type_) {
                    return new_bool(left.get<Type>() << right.get<Type>());
                } else if (isIntegerType(left.T) && isIntegerType(right.T)) {
                    int64_t res = static_cast<int64_t>(getNumberFromType<>(left)) <<
                                  static_cast<int64_t>(getNumberFromType<>(right));
                    Type rt = resultTypeOf(left.T, right.T);
                    if (rt == Ty::i8)  return new_i8(static_cast<int8_t>(res));
                    if (rt == Ty::i16) return new_i16(static_cast<int16_t>(res));
                    if (rt == Ty::i32) return new_i32(static_cast<int32_t>(res));
                    if (rt == Ty::i64) return new_i64(res);
                } else {
                    error_container.addError(DEEP_ERRORS::UNSUPPORTED_OPERATOR(binary_node->start, binary_node->end, "<<", left.T.toString(), right.T.toString()));
                    return new_null();
                }
            }

            case NodeTypes::NodeBinaryShiftRight: {
                auto binary_node = static_cast<struct NodeBinaryShiftRight*>(node);
                Value left = execute(binary_node->l_node, context);
                Value right = execute(binary_node->r_node, context);
                Value dund = new_null();
                if (tryDunder("__rshift__", left, {right}, context, binary_node->start, binary_node->end, dund)) return dund;
                if (left.T == Ty::Type_ && right.T == Ty::Type_) {
                    return new_bool(left.get<Type>() >> right.get<Type>());
                } else if (isIntegerType(left.T) && isIntegerType(right.T)) {
                    int64_t res = static_cast<int64_t>(getNumberFromType<>(left)) >>
                                  static_cast<int64_t>(getNumberFromType<>(right));
                    Type rt = resultTypeOf(left.T, right.T);
                    if (rt == Ty::i8)  return new_i8(static_cast<int8_t>(res));
                    if (rt == Ty::i16) return new_i16(static_cast<int16_t>(res));
                    if (rt == Ty::i32) return new_i32(static_cast<int32_t>(res));
                    if (rt == Ty::i64) return new_i64(res);
                } else {
                    error_container.addError(DEEP_ERRORS::UNSUPPORTED_OPERATOR(binary_node->start, binary_node->end, ">>", left.T.toString(), right.T.toString()));
                    return new_null();
                }
            }

            case NodeTypes::NodeBinaryScopeResolution: {
                auto* sr_node = static_cast<struct NodeBinaryScopeResolution*>(node);
                ContextObject* member = resolveScopeMember(sr_node, context);
                if (!member) return new_null();
                return member->value;
            }

            case NodeTypes::NodeInput: {
                auto* input_node = static_cast<struct NodeInput*>(node);
                // M8: внутри `eval` ввод запрещён (блок вычисляется на этапе
                // анализа) — отдельная диагностика EVAL_NOT_CONST.
                if (in_eval)
                    error_container.addError(DEEP_ERRORS::EVAL_NOT_CONST(input_node->token_start, "'input'"));
                else
                    error_container.addError(DEEP_ERRORS::INPUT_RUNTIME(input_node->token_start, input_node->token_end));
                if (input_node->prompt) {
                    Value pv = execute(input_node->prompt, context);
                    if (pv.T != Ty::Str && pv.T != Ty::Char) {
                        error_container.addError(DEEP_ERRORS::INPUT_PROMPT_TYPE(input_node->token_start, pv.T.toString()));
                    } else if (print_input_prompts) {
                        // как рантайм: приглашение печатается в stdout без перевода строки
                        std::cout << formatValue(pv).to_utf8();
                    }
                }
                return new_string(String());
            }
            case NodeTypes::NodeEcho: {
                auto* echo_node = static_cast<struct NodeEcho*>(node);
                if (echo_node->expr) {
                    // Хинт со значением показываем только если выражение
                    // действительно разобралось: при ошибке execute() отдаёт
                    // Null, и подсказка показывала бы фиктивное `null` прямо
                    // на месте, где компилятор уже подсветил проблему.
                    size_t errors_before = error_container.errors.size();
                    Value result = execute(echo_node->expr, context);
                    bool failed = error_container.errors.size() != errors_before;
                    String str = failed ? String() : formatValue(result);
                    if (!str.empty()) {
                        int hl = echo_node->end.position.line;
                        int hc = echo_node->end.position.index + echo_node->end.position.size;
                        String hlabel(" ");
                        hlabel += str;
                        inlay_hints.push_back({hl, hc, hlabel});
                    }
                }
                break;
            }

            case NodeTypes::NodeOut:
            case NodeTypes::NodeOutln: {
                if (node->type == NodeTypes::NodeOut) {
                    for (auto* e : static_cast<struct NodeOut*>(node)->exprs)
                        execute(e, context);
                } else {
                    for (auto* e : static_cast<struct NodeOutln*>(node)->exprs)
                        execute(e, context);
                }
                break;
            }

            case NodeTypes::NodeDelete: {
                auto* del_node = static_cast<struct NodeDelete*>(node);
                if (!del_node->expr) {
                    error_container.addError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(del_node->token));
                    break;
                }
                Value host = execute(del_node->expr, context);
                if (del_node->index == nullptr) {
                    // `del x;` без индекса — только array/tuple умеет удалять по индексу
                    error_container.addError(DEEP_ERRORS::DELETE_WITHOUT_INDEX(
                        del_node->token, host.T.toString()));
                    break;
                }
                Value iv = execute(del_node->index, context);
                // dunder-удаление: `del obj, i` -> `obj.__del__(i)`, если у struct есть метод
                if (host.T.isStruct()) {
                    auto dsit = structs.find(host.T.name);
                    if (dsit != structs.end() && dsit->second.methods.count("__del__") > 0) {
                        std::vector<Value> dargs{iv};
                        if (callDunderWithWriteback("__del__", del_node->expr, host,
                                                    dargs, del_node->token, context))
                            break;
                    }
                }
                if (!host.T.isArray() && !host.T.isTuple()) {
                    error_container.addError(DEEP_ERRORS::DELETE_INVALID_TYPE(
                        del_node->token, host.T.toString()));
                    break;
                }
                if (!isIntegerType(iv.T)) {
                    error_container.addError(DEEP_ERRORS::ARRAY_INDEX_NOT_INTEGER(
                        del_node->token, iv.T.toString()));
                    break;
                }
                long long idx = static_cast<long long>(getNumberFromType<>(iv));
                RuntimeArray arr = host.get<RuntimeArray>();
                auto& v = arr.mut();
                if (idx < 0 || idx >= static_cast<long long>(v.size())) {
                    error_container.addError(DEEP_ERRORS::ARRAY_INDEX_OUT_OF_RANGE(
                        del_node->token, idx, static_cast<long long>(v.size())));
                    break;
                }
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx));
                // Точность длины: после удаления статический размер пересчитываем,
                // иначе `sizeof` = 2, а `typeof` всё ещё `{i64, 3}`.
                Type del_ty = host.T;
                del_ty.size = static_cast<int>(v.size());
                Value new_arr = new_array(del_ty, std::move(v));
                writePathValue(del_node->expr, context, new_arr, del_node->token);
                break;
            }

            case NodeTypes::NodeAssert: {
                auto* a_node = static_cast<struct NodeAssert*>(node);
                // Сухой прогон мёртвой ветки (`if` с заведомо ложным условием):
                // assert в недостижимом коде не должен давать ложное
                // предупреждение «Assertion failed».
                if (dry_exec) break;
                if (!a_node->check) {
                    error_container.addError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(a_node->token));
                    break;
                }
                Value check = execute(a_node->check, context);
                bool ok = false;
                // Правдивость через общий truthyOf: он принимает bool, число и
                // структуру с дюндером __bool__. Раньше здесь был свой набор
                // проверок только для bool/числа, поэтому `assert f;` для
                // объекта с __bool__ давал error[125], хотя тот же объект
                // нормально работал в `if (f)` и в `!f`. Зеркалит
                // rt::truthy(Ctx&, Box) из CodeGenerator.
                if (!truthyOf(check, a_node->token, context, ok)) {
                    error_container.addError(DEEP_ERRORS::ASSERT_CONDITION_NOT_BOOL(
                        a_node->token, check.T.toString()));
                    break;
                }

                String message;
                if (a_node->message) {
                    Value mv = execute(a_node->message, context);
                    if (mv.T == Ty::Str) {
                        message = mv.get<String>();
                    } else if (mv.T == Ty::Char) {
                        message = String(mv.get<char32_t>());
                    } else {
                        error_container.addError(DEEP_ERRORS::ASSERT_MESSAGE_TYPE(
                            a_node->token, mv.T.toString()));
                        break;
                    }
                }

                if (!ok)
                    error_container.addError(DEEP_ERRORS::ASSERT_FAILED(
                        current_call_site ? *current_call_site : a_node->token,
                        (current_call_site_end ? *current_call_site_end
                                               : (current_call_site ? *current_call_site : a_node->token)),
                        message));
                break;
            }

            case NodeTypes::NodeExit: {
                auto* exit_node = static_cast<struct NodeExit*>(node);
                // В «сухом» прогоне мёртвой ветки exit не срабатывает
                // (условие ложно, до exit дело не дошло бы).
                if (dry_exec) break;
                int code = 0;
                if (exit_node->expr) {
                    Value cv = execute(exit_node->expr, context);
                    if (!isIntegerType(cv.T)) {
                        error_container.addError(DEEP_ERRORS::EXIT_CODE_NOT_INTEGER(
                            exit_node->token, cv.T.toString()));
                        break;
                    }
                    code = static_cast<int>(getNumberFromType<>(cv));
                }
                // Deep-анализ (lint/LSP) не исполняет exit: прервав компилятор/сервер
                // здесь, мы оборвали бы проверку остального кода. Действует только
                // настоящий прогон — интерпретатор (-r). Транспилированный код
                // вызывает ::exit() в рантайме.
                if (interpret_mode) {
                    exit_requested = true;
                    exit_code = code;
                    aborted = true;               // верхний цикл и последующие стейтменты
                    control_flow = ControlFlow::Exit_;
                }
                break;
            }

            case NodeTypes::NodeAppend: {
                auto* append_node = static_cast<struct NodeAppend*>(node);
                Value v = execute(append_node->value_expr, context);
                // Мутируем и в deep-анализе (lint/LSP), как `=`/`del`, чтобы inlay
                // в `echo a` показывал актуальное значение. Инертным append остаётся
                // только в «сухом» прогоне мёртвой ветки (dry_exec).
                bool commit = !dry_exec;
                appendInPlace(append_node->array_expr, context, v, append_node->token, commit);
                break;
            }

            case NodeTypes::NodeReserve: {
                auto* reserve_node = static_cast<struct NodeReserve*>(node);
                // nullptr-подузел — уже сообщённая синтаксическая ошибка разбора,
                // не добавляем каскад диагностик.
                if (!reserve_node->array_expr || !reserve_node->count_expr) break;
                Value cv = execute(reserve_node->count_expr, context);
                if (!isIntegerType(cv.T)) {
                    error_container.addError(DEEP_ERRORS::RESERVE_COUNT_NOT_INTEGER(
                        reserve_node->token, cv.T.toString()));
                    break;
                }
                long long count = static_cast<long long>(getNumberFromType<>(cv));
                reserveInPlace(reserve_node->array_expr, context, reserve_node->token, count);
                break;
            }

            case NodeTypes::NodeEvalBlock: {
                // M8: тело eval исполняется в глобальном контексте (top-level).
                // На время блока включаем семантику eval: запрет input/extern,
                // общий per-block бюджет итераций (не сбрасывается на каждом цикле).
                auto* ev = static_cast<struct NodeEvalBlock*>(node);
                if (!ev->body) return new_null();
                bool saved_in_eval = in_eval;
                bool saved_shared = eval_budget_shared;
                int  saved_budget = iter_budget;
                bool saved_exhausted = budget_exhausted;
                in_eval = true;
                eval_budget_shared = true;
                iter_budget = EVAL_LOOP_BUDGET;   // свежий бюджет на блок
                budget_exhausted = false;
                Value result = execute(ev->body, context);
                in_eval = saved_in_eval;
                eval_budget_shared = saved_shared;
                iter_budget = saved_budget;
                budget_exhausted = saved_exhausted;
                return result;
            }

            case NodeTypes::NodeBinaryAssignment: {
                auto binary_node = static_cast<struct NodeBinaryAssignment*>(node);
                while (binary_node->l_node && binary_node->l_node->type == NodeTypes::NodeScope)
                    binary_node->l_node = static_cast<struct NodeScope*>(binary_node->l_node)->node;
                // ==== in-place `+=` на массивах ====
                // Канонический `a += b` / `a = a + b` — RHS-бинарь с тем же LHS-узлом.
                // Для array-переменной не строим конкатенацию O(n) (binaryMath копирует весь
                // вектор): доклеиваем элементы в живую ячейку через COW-мутацию mut().
                // LHS здесь — всегда простое ЧИСТОЕ чтение переменной (execute(NodeVariable)
                // не имеет сайд-эффектов), поэтому его НЕ пре-вычисляем: живая копия массива
                // держала бы use_count=2 и заставляла mut() копировать вектор каждый раз.
                // RHS исполняется ровно один раз (сайд-эффекты/ошибки) в CANONIC-ветке.
                struct NodeBinaryAdditive* fadd = nullptr;
                bool parts_ready = false;
                bool fast_candidate = false;
                Value rhs_v = new_null();
                if (binary_node->l_node && binary_node->l_node->type == NodeTypes::NodeVariable
                        && binary_node->r_node && binary_node->r_node->type == NodeTypes::NodeBinaryAdditive) {
                    auto* cand = static_cast<struct NodeBinaryAdditive*>(binary_node->r_node);
                    Node* ll = cand->l_node;
                    while (ll && ll->type == NodeTypes::NodeScope)
                        ll = static_cast<struct NodeScope*>(ll)->node;
                    if (ll == binary_node->l_node) {
                        auto* lv0 = static_cast<struct NodeVariable*>(binary_node->l_node);
                        if (context.has_variable(lv0->name)) {
                            ContextObject& var0 = context.get_variable(lv0->name);
                            Type wt0 = var0.wait_type;
                            bool is_fn0 = wt0.isCallable()
                                || (wt0.isUnion() && [&wt0]() {
                                    for (const auto& m : wt0.container_1) if (m.isCallable()) return true;
                                    return false;
                                }());
                            if (!var0.ref_alias_node && !var0.modifiers.Const
                                    && !(is_fn0 && var0.modifiers.Strict)
                                    && var0.value.T.isArray()) {
                                fadd = cand;
                                rhs_v = execute(cand->r_node, context);
                                parts_ready = true;
                                if (rhs_v.T.isArray()) fast_candidate = true;
                            }
                        }
                    }
                }
                Value r = new_null();
                if (fast_candidate) {
                    r = new_null();
                } else if (parts_ready) {
                    // чтение LHS откладываем сюда: чистое и не копирует массив на горячем пути
                    Value lhs_v = execute(fadd->l_node, context);
                    r = binaryMath(lhs_v, rhs_v, '+', fadd->start, fadd->end);
                } else {
                    r = execute(binary_node->r_node, context);
                }
                if (binary_node->l_node && binary_node->l_node->type == NodeTypes::NodeVariable) {
                    auto* var_node = static_cast<struct NodeVariable*>(binary_node->l_node);
                    if (!context.has_variable(var_node->name)) {
                        error_container.addError(DEEP_ERRORS::UNDEFINED_VARIABLE(var_node->token, var_node->name));
                        aborted = true;
                        return new_null();
                    }
                    ContextObject& var = context.get_variable(var_node->name);
                    // Барьер dry-прогона мёртвой ветки: не коммитим запись в
                    // переменную, разрешившуюся выше кадра ветки — иначе эффект
                    // недостижимого кода протекает наружу (`res += ...` во внешнюю `res`).
                    const bool commit_ok = dryAllowsWrite(context, var_node->name);
                    // const/final-алиас: менять сам алиас нельзя, даже если цель
                    // изменяема (`const ref let b = a; b = 20;` — ошибка).
                    if (var.modifiers.Final) {
                        error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(var_node->token, var_node->name));
                        return r;
                    }
                    if (var.modifiers.Const) {
                        error_container.addError(DEEP_ERRORS::CONST_REASSIGNED(var_node->token, var_node->name));
                        return r;
                    }
                    // ref-алиас: `r = v` пишет в цель алиаса (проверки Final/Const/Strict
                    // выполнит writePathValue по пути цели)
                    if (var.ref_alias_node) {
                        if (commit_ok)
                            writePathValue(var.ref_alias_node, context, r, var_node->token);
                        return r;
                    }
                    if (tryAssignDunder(binary_node->l_node, context, var.value, r, var_node->token))
                        return r;
                    Type wt = var.wait_type;
                    // РёРјСЏ Р·Р°РЅСЏС‚Рѕ С„СѓРЅРєС†РёРµР№ (РєРѕРЅС‚РµР№РЅРµСЂ РїРµСЂРµРіСЂСѓР·РѕРє): РѕР±С‹С‡РЅР°СЏ С„СѓРЅРєС†РёСЏ вЂ”
                    // В«РїРµСЂРµР·Р°РїРёСЃС‹РІР°РµРјР°СЏВ» РїРµСЂРµРјРµРЅРЅР°СЏ, strict-С„СѓРЅРєС†РёСЋ РїРµСЂРµР·Р°РїРёСЃР°С‚СЊ РЅРµР»СЊР·СЏ
                    bool is_fn = wt.isCallable() || (wt.isUnion() && [&wt]() {
                        for (const auto& m : wt.container_1) if (m.isCallable()) return true;
                        return false;
                    }());
                    if (is_fn && var.modifiers.Strict) {
                        error_container.addError(DEEP_ERRORS::FUNCTION_STRICT_REASSIGNED(var_node->token, var_node->name));
                        return r;
                    }
                    bool constrained = var.modifiers.Strict && !is_fn && !(wt == Ty::Null) && !wt.is_auto && !wt.is_any;
                    if (fast_candidate) {
                        // ==== in-place `+=` на массивах: без копии всей конкатенации ====
                        // Тип элемента обновляем инкрементально (как `append`): совпадает с
                        // пересбором binaryMath для всех практически достижимых последовательностей.
                        Type rhs_elem = rhs_v.T.container_1.empty() ? Ty::Any : rhs_v.T.container_1[0];
                        Type new_elem = Ty::Any;
                        if (var.value.T.container_1.empty())
                            new_elem = rhs_elem;
                        else
                            new_elem = appendUnionType(var.value.T.container_1[0], rhs_elem);
                        // Аннотация массива (элемент И длина) связывает и без
                        // strict — см. arrayAnnotationOk. Проверяется по
                        // расширившемуся `new_elem`: объявленный `i64 | char`
                        // обязан принимать оба члена, а `i64 | char >> i64` =
                        // false (union-источник требует укладки ВСЕХ членов).
                        // strict дополнительно проходит полное `wt << candT`.
                        const int out_size =
                            static_cast<int>(var.value.get_ref<RuntimeArray>().view().size()
                                           + rhs_v.get_ref<RuntimeArray>().view().size());
                        Type candT = Type::ArrayType(new_elem, out_size);
                        if (!arrayAnnotationOk(wt, candT)
                                || (constrained && !(wt << candT))) {
                            // редкий путь ошибки — точное значение общей ветки
                            // (чтение LHS здесь чистое; возможна расхождение, если RHS
                            // мутировал `a` и было ещё и strict-несоответствие — согласовано)
                            Value lhs_v = execute(fadd->l_node, context);
                            Value er = binaryMath(lhs_v, rhs_v, '+', fadd->start, fadd->end);
                            error_container.addError(DEEP_ERRORS::ASSIGNMENT_TYPE_MISMATCH(
                                var_node->token, var_node->name, wt.toString(), candT.toString()));
                            return er;
                        }
                        if (commit_ok) {
                        RuntimeArray& larr = var.value.get_ref<RuntimeArray>();
                        std::vector<Value>& vec = larr.mut();
                        const auto& rv = rhs_v.get_ref<RuntimeArray>().view();
                        const size_t base = vec.size();
                        vec.insert(vec.end(), rv.begin(), rv.end());
                        // реф-элементы RHS переносим со сдвигом (как binaryMath:606-607)
                        const auto& rrefs = rhs_v.get_ref<RuntimeArray>().refs;
                        if (!rrefs.empty())
                            for (auto const& rp : rrefs) larr.refs[base + rp.first] = rp.second;
                        if (var.value.T.container_1.empty())
                            var.value.T.container_1.push_back(new_elem);
                        else
                            var.value.T.container_1[0] = new_elem;
                        // Точность длины: ветка `+=` обходила doAppendValue и не
                        // трогала size — `{i64, 0}` при sizeof == 2.
                        var.value.T.size = static_cast<int>(vec.size());
                        }
                        return var.value;
                    }
                    Value assignR = r;
                    if (r.T.isArray() && r.T.container_1.size() == 1
                            && r.T.container_1[0].isAny() && r.T.size == 0
                            && wt.isArray() && !wt.container_1.empty()) {
                        assignR.T = Type::ArrayType(wt.container_1[0], r.T.size);
                    }
                    if (!arrayAnnotationOk(wt, assignR.T)
                            || (constrained && !(wt << assignR.T))) {
                        error_container.addError(DEEP_ERRORS::ASSIGNMENT_TYPE_MISMATCH(var_node->token, var_node->name, wt.toString(), assignR.T.toString()));
                        return r;
                    }
                    if (commit_ok) var.value = assignR;
                } else if (binary_node->l_node && binary_node->l_node->type == NodeTypes::NodeUnaryDereference) {
                    auto* deref_node = static_cast<struct NodeUnaryDereference*>(binary_node->l_node);
                    Value ptr = execute(deref_node->node, context);
                    if (!ptr.T.isPointer()) {
                        error_container.addError(DEEP_ERRORS::UNARY_DEREFERENCE_NOT_INVALID_TYPE(deref_node->token, ptr.T.toString()));
                        return r;
                    }
                    Pointer p = ptr.get<Pointer>();
                    auto fs = field_slots.find(p.addr);
                    if (fs != field_slots.end()) {
                        ContextObject* owner = fs->second.first;
                        RuntimeStruct& rs = owner->value.get_ref<RuntimeStruct>();
                        auto fit = rs.mut().find(fs->second.second);
                        if (fit == rs.view().end()) {
                            error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                                deref_node->token, owner->value.T.name, fs->second.second));
                            return r;
                        }
                        if (owner->modifiers.Final) {
                            error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(
                                deref_node->token, owner->identifier));
                            return r;
                        }
                        if (owner->modifiers.Const) {
                            error_container.addError(DEEP_ERRORS::CONST_REASSIGNED(
                                deref_node->token, owner->identifier));
                            return r;
                        }
                        if (const_receiver && owner == const_receiver) {
                            error_container.addError(DEEP_ERRORS::CONST_FN_MUTATION(
                                deref_node->token, fs->second.second));
                            return r;
                        }
                        const StructSymbol* cow_sym = structSymForType(owner->value.T);
                        const StructFieldDef* cf_d = cow_sym ? cow_sym->field_def(fs->second.second) : nullptr;
                        if (cf_d && cf_d->mods.Final) {
                            error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(
                                deref_node->token, fs->second.second));
                            return r;
                        }
                        if (cf_d && cf_d->mods.Strict) {
                            Type ft = fieldTypeFor(*cow_sym, owner->value.T, fs->second.second);
                            if (!ft.is_auto && !ft.is_any && !(ft << r.T)) {
                                error_container.addError(DEEP_ERRORS::STRICT_TYPE_MISMATCH(
                                    deref_node->token, ft.toString(), r.T.toString()));
                                return r;
                            }
                        }
                        fit->second = r;
                        return r;
                    }
                    if (p.addr == 0 || !heap.contains(p.addr)) {
                        error_container.addError(DEEP_ERRORS::NULL_DEREFERENCE(deref_node->token));
                        return r;
                    }
                    ContextObject* obj = heap.get(p.addr);
                    if (obj->modifiers.Final) {
                        error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(deref_node->token, obj->identifier));
                        return r;
                    }
                    if (obj->modifiers.Const) {
                        error_container.addError(DEEP_ERRORS::CONST_REASSIGNED(deref_node->token, obj->identifier));
                        return r;
                    }
                    if (tryAssignDunder(binary_node->l_node, context, obj->value, r, deref_node->token))
                        return r;
                    Type wt = obj->wait_type;
                    bool constrained = obj->modifiers.Strict && !(wt == Ty::Null) && !wt.is_auto && !wt.is_any;
                    if (constrained && !(wt << r.T)) {
                        error_container.addError(DEEP_ERRORS::ASSIGNMENT_TYPE_MISMATCH(deref_node->token, obj->identifier, wt.toString(), r.T.toString()));
                        return r;
                    }
                    obj->value = r;
                } else if (binary_node->l_node && binary_node->l_node->type == NodeTypes::NodeMemberAccess) {
                    // Путевая запись: `p.field = x`, `p.field.sub = y`, `c[0].first.value = z`.
                    // Внутри выставляются Final/Const/Private/Strict-проверки и write-back
                    // по всей цепочке MemberAccess/ArrayAccess.
                    writePathValue(binary_node->l_node, context, r,
                                   static_cast<struct NodeMemberAccess*>(binary_node->l_node)->member_token);
                    return r;
                } else if (binary_node->l_node && binary_node->l_node->type == NodeTypes::NodeBinaryScopeResolution) {
                    auto* sr = static_cast<struct NodeBinaryScopeResolution*>(binary_node->l_node);
                    ContextObject* member = resolveScopeMember(sr, context);
                    if (!member) return r;
                    if (member->modifiers.Final) {
                        error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(sr->member_token, member->identifier));
                        return r;
                    }
                    if (tryAssignDunder(binary_node->l_node, context, member->value, r, sr->member_token))
                        return r;
                    Type wt = member->wait_type;
                    bool constrained = member->modifiers.Strict && !(wt == Ty::Null) && !wt.is_auto && !wt.is_any;
                    if (!arrayAnnotationOk(wt, r.T) || (constrained && !(wt << r.T))) {
                        error_container.addError(DEEP_ERRORS::ASSIGNMENT_TYPE_MISMATCH(sr->member_token, member->identifier, wt.toString(), r.T.toString()));
                        return r;
                    }
                    if (!dry_exec) member->value = r;
                } else if (binary_node->l_node && binary_node->l_node->type == NodeTypes::NodeArrayAccess) {
                    auto* acc = static_cast<struct NodeArrayAccess*>(binary_node->l_node);
                    // ref-алиас как корень индексации: `r[i] = v` пишет в цель алиаса
                    {
                        Node* rb = acc;
                        while (rb->type == NodeTypes::NodeScope)
                            rb = static_cast<struct NodeScope*>(rb)->node;
                        if (rb->type == NodeTypes::NodeVariable) {
                            auto* rvn = static_cast<struct NodeVariable*>(rb);
                            if (context.has_variable(rvn->name)
                                    && context.get_variable(rvn->name).ref_alias_node) {
                                writePathValue(acc, context, r, acc->token);
                                return r;
                            }
                        }
                    }
                    // РЈРїР»РѕС‰Р°РµРј С†РµРїРѕС‡РєСѓ РёРЅРґРµРєСЃРѕРІ: a[i] -> a[i][j] (base РјРѕР¶РµС‚ СЃР°Рј Р±С‹С‚СЊ
                    // NodeArrayAccess). РҐСЂР°РЅРёР»РёС‰Рµ РёС‰РµС‚СЃСЏ РїРѕ В«РєРѕСЂРЅРµРІРѕРјСѓВ» base.
                    std::vector<Node*> chain;
                    Node* base = acc;
                    while (base->type == NodeTypes::NodeArrayAccess) {
                        auto* a2 = static_cast<struct NodeArrayAccess*>(base);
                        chain.insert(chain.begin(), a2->indices.begin(), a2->indices.end());
                        base = a2->base;
                    }
                    ContextObject* store = nullptr;
                    {
                        Node* b = base;
                        while (b->type == NodeTypes::NodeScope) {
                            auto* sc = static_cast<struct NodeScope*>(b);
                            b = sc->node;
                        }
                        if (b != base) base = b;
                    }
                    // dunder-индексация: `obj[i] = v` -> `obj.__set_index__(i, v)`
                    {
                        Value bv = execute(base, context);
                        if (bv.T.isStruct()) {
                            auto ssit = structs.find(bv.T.name);
                            if (ssit != structs.end()
                                    && ssit->second.methods.count("__set_index__") > 0) {
                                if (callIndexSetter(base, bv, chain, r, acc->token, context))
                                    return r;
                            }
                        }
                    }
                    if (base->type == NodeTypes::NodeVariable) {
                        auto* var_node = static_cast<struct NodeVariable*>(base);
                        if (!context.has_variable(var_node->name)) {
                            error_container.addError(DEEP_ERRORS::UNDEFINED_VARIABLE(var_node->token, var_node->name));
                            aborted = true;
                            return new_null();
                        }
                        store = &context.get_variable(var_node->name);
                    } else if (base->type == NodeTypes::NodeBinaryScopeResolution) {
                        store = resolveScopeMember(base, context);
                    } else if (base->type == NodeTypes::NodeUnaryDereference) {
                        auto* deref_node = static_cast<struct NodeUnaryDereference*>(base);
                        Value ptr = execute(deref_node->node, context);
                        if (!ptr.T.isPointer()) {
                            error_container.addError(DEEP_ERRORS::UNARY_DEREFERENCE_NOT_INVALID_TYPE(deref_node->token, ptr.T.toString()));
                        } else {
                            Pointer p = ptr.get<Pointer>();
                            auto fs = field_slots.find(p.addr);
                            if (fs != field_slots.end()) {
                                RuntimeStruct& frs = fs->second.first->value.get_ref<RuntimeStruct>();
                                auto ffit = frs.mut().find(fs->second.second);
                                if (ffit == frs.view().end()) {
                                    error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                                        acc->token, fs->second.first->value.T.name, fs->second.second));
                                } else {
                                    // диспатч __assign__ на текущем значении элемента
                                    Value elt = new_null();
                                    if (elementAt(ffit->second, chain, 0, acc->token, context, elt)
                                            && tryAssignDunder(acc, context, elt, r, acc->token)) {
                                        return r;
                                    }
                                    if (!setIndexRec(ffit->second, chain, 0, acc->token, context, r)) {
                                        ; // ошибка уже добавлена
                                    } else {
                                        return r;
                                    }
                                }
                            } else if (p.addr == 0 || !heap.contains(p.addr)) {
                                error_container.addError(DEEP_ERRORS::NULL_DEREFERENCE(deref_node->token));
                            } else {
                                store = heap.get(p.addr);
                            }
                        }
                    } else if (base->type == NodeTypes::NodeMemberAccess) {
                        // `self._data[i] = x` / `p.arr[j] = v`: мутируем копию
                        // поля-массива и записываем её обратно в поле структуры
                        auto* ma_n = static_cast<struct NodeMemberAccess*>(base);
                        String field = memberNameNode(ma_n->r_node);
                        ContextObject* own = structOwnerFor(ma_n->l_node, context);
                        if (!own || !own->value.T.isStruct()) {
                            error_container.addError(DEEP_ERRORS::ASSIGN_TO_EXPRESSION(acc->token, "expr"));
                            return r;
                        }
                        RuntimeStruct& rs = own->value.get_ref<RuntimeStruct>();
                        auto fit = rs.mut().find(field);
                        if (fit == rs.view().end()) {
                            error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                                ma_n->member_token, own->value.T.name, field));
                            return r;
                        }
                        if (own->modifiers.Final) {
                            error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(
                                ma_n->member_token, own->identifier));
                            return r;
                        }
                        if (own->modifiers.Const) {
                            error_container.addError(DEEP_ERRORS::CONST_REASSIGNED(
                                ma_n->member_token, own->identifier));
                            return r;
                        }
                        if (const_receiver && own == const_receiver) {
                            error_container.addError(DEEP_ERRORS::CONST_FN_MUTATION(
                                ma_n->member_token, field));
                            return r;
                        }
                        const StructSymbol* idx_sym = structSymForType(own->value.T);
                        const StructFieldDef* ifd = idx_sym ? idx_sym->field_def(field) : nullptr;
                        if (ifd && ifd->mods.Private && active_struct != idx_sym) {
                            error_container.addError(DEEP_ERRORS::PRIVATE_FIELD(
                                ma_n->member_token, own->value.T.name, field));
                            return r;
                        }
                        if (ifd && ifd->mods.Final) {
                            error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(
                                ma_n->member_token, field));
                            return r;
                        }
                        Value cur = fit->second;
                        // диспатч __assign__ на текущем значении элемента
                        {
                            Value elt = new_null();
                            if (elementAt(cur, chain, 0, acc->token, context, elt)
                                    && tryAssignDunder(acc, context, elt, r, acc->token)) {
                                return r;
                            }
                        }
                        if (!setIndexRec(cur, chain, 0, acc->token, context, r)) return r;
                        if (!dry_exec) fit->second = cur;
                        return r;
                    }
                    if (!store) {
                        error_container.addError(DEEP_ERRORS::ASSIGN_TO_EXPRESSION(acc->token, "expr"));
                        return r;
                    }
                    if (store->modifiers.Final) {
                        error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(acc->token, store->identifier));
                        return r;
                    }
                    if (store->modifiers.Const) {
                        error_container.addError(DEEP_ERRORS::CONST_REASSIGNED(acc->token, store->identifier));
                        return r;
                    }
                    Value cur = store->value;
                    // диспатч __assign__ на текущем значении элемента
                    {
                        Value elt = new_null();
                        if (elementAt(cur, chain, 0, acc->token, context, elt)
                                && tryAssignDunder(acc, context, elt, r, acc->token)) {
                            return r;
                        }
                    }
                    if (!setIndexRec(cur, chain, 0, acc->token, context, r)) return r;
                    if (dryAllowsWrite(context, store->identifier)) store->value = cur;
                } else if (binary_node->l_node && binary_node->l_node->type == NodeTypes::NodeFunctionCall) {
                    // `get() = v` — write-back ref-возврата (для не-ref-функции —
                    // ASSIGN_TO_EXPRESSION внутри writePathValue)
                    writePathValue(binary_node->l_node, context, r, binary_node->start_token);
                    return r;
                }
                return r;
            }

            case NodeTypes::NodeUnaryAddressOf: {
                auto* unary_node = static_cast<struct NodeUnaryAddressOf*>(node);
                if (unary_node->node && unary_node->node->type == NodeTypes::NodeVariable) {
                    auto* var_node = static_cast<struct NodeVariable*>(unary_node->node);
                    if (!context.has_variable(var_node->name)) {
                        error_container.addError(DEEP_ERRORS::UNDEFINED_VARIABLE(var_node->token, var_node->name));
                        aborted = true;
                        return new_null();
                    }
                    ContextObject& var = context.get_variable(var_node->name);
                    // ref-алиас (`ref let x = t;` / ref-параметр): адрес ЖИВОЙ ячейки
                    // цели, а не копии самого алиаса (иначе `&x` — dangling/null).
                    if (var.ref_alias_node) {
                        ContextObject* tgt = structOwnerFor(var.ref_alias_node, context);
                        if (tgt) return new_pointer(tgt->value.T, tgt->addr);
                    }
                    return new_pointer(var.value.T, var.addr);
                } else if (unary_node->node && unary_node->node->type == NodeTypes::NodeMemberAccess) {
                    // `&obj.field` — указатель на живой слот поля структуры.
                    // Владелец и имя поля регистрируются в field_slots; указатель
                    // получает адрес из отдельной арены (allocate_slot), чтобы не
                    // совпасть с адресом самой переменной. Все потребители
                    // (чтение поля, `*p`, запись `*p = v`, приёмник метода)
                    // этот случай уже обрабатывают.
                    auto* ma_n = static_cast<struct NodeMemberAccess*>(unary_node->node);
                    String field = memberNameNode(ma_n->r_node);
                    Node* base = ma_n->l_node;
                    // Скобки не меняют адресуемость: `&(*p).field` — то же, что `&p2.field`.
                    while (base && (base->type == NodeTypes::NodeScope
                                 || base->type == NodeTypes::NodePostfixUnwrap))
                        base = static_cast<struct NodeScope*>(base)->node;
                    // База должна быть адресуемым путём: переменная, `*ptr` или
                    // `Ns::x`. Вложенное поле (`&o.a.b`) и rvalue — отдельно,
                    // чтобы сообщение называло настоящую причину.
                    bool base_ok = base && (base->type == NodeTypes::NodeVariable
                                          || base->type == NodeTypes::NodeUnaryDereference
                                          || base->type == NodeTypes::NodeBinaryScopeResolution);
                    if (!base_ok) {
                        String what = (base && base->type == NodeTypes::NodeMemberAccess)
                            ? "nested member '" + field + "'"
                            : "member of a temporary value";
                        error_container.addError(DEEP_ERRORS::ADDRESS_OF_MEMBER_UNSUPPORTED(
                            ma_n->member_token, what));
                        return new_null();
                    }
                    ContextObject* owner = structOwnerFor(base, context);
                    if (!owner || !owner->value.T.isStruct()) {
                        error_container.addError(DEEP_ERRORS::ADDRESS_OF_MEMBER_UNSUPPORTED(
                            ma_n->member_token, "member '" + field + "' of a non-struct value"));
                        return new_null();
                    }
                    const StructSymbol* osym = structSymForType(owner->value.T);
                    const StructFieldDef* ofd = osym ? osym->field_def(field) : nullptr;
                    if (ofd && ofd->mods.Private && active_struct != osym) {
                        error_container.addError(DEEP_ERRORS::PRIVATE_FIELD(
                            ma_n->member_token, owner->value.T.name, field));
                        return new_null();
                    }
                    RuntimeStruct& ors = owner->value.get_ref<RuntimeStruct>();
                    auto oit = ors.view().find(field);
                    if (oit == ors.view().end()) {
                        error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                            ma_n->member_token, owner->value.T.name, field));
                        return new_null();
                    }
                    auto slot = heap.allocate_slot();
                    field_slots[slot] = std::make_pair(owner, field);
                    // Слот наследует область видимости владельца: иначе `ret &local.field`
                    // проскакивал бы мимо POINTER_ESCAPES_SCOPE (у слота свой адрес, а
                    // проверка выхода ищет адрес локальной переменной), хотя указатель
                    // на поле локалки умирает вместе с ней.
                    if (current_fn_scope != 0) {
                        auto lo_it = local_owner.find(owner->addr);
                        if (lo_it != local_owner.end()) local_owner[slot] = lo_it->second;
                    }
                    return new_pointer(oit->second.T, slot);
                } else if (unary_node->node && unary_node->node->type == NodeTypes::NodeBinaryScopeResolution) {
                    error_container.addError(DEEP_ERRORS::ADDRESS_OF_MEMBER_UNSUPPORTED(
                        unary_node->token, "namespace/scope member"));
                    return new_null();
                }
                Value operand = execute(unary_node->node, context);
                error_container.addError(DEEP_ERRORS::ADDRESS_OF_INVALID(unary_node->token, operand.T.toString()));
                return new_null();
            }

            case NodeTypes::NodeNamespace: {
                auto* namespace_node = static_cast<struct NodeNamespace*>(node);
                if (!namespace_node->name.empty()) {
                    if (context.has_variable_local(namespace_node->name)) {
                        ContextObject& existing = context.get_variable_local(namespace_node->name);
                        if (existing.modifiers.Final) {
                            error_container.addError(DEEP_ERRORS::FINAL_REDECLARED(namespace_node->name_token, namespace_node->name));
                            break;
                        }
                    }

                    // СЂРµРіРёСЃС‚СЂРёСЂСѓРµРј РїСѓСЃС‚РѕР№ РЅРµР№РјСЃРїРµР№СЃ СЃСЂР°Р·Сѓ РІ СѓСЃС‚РѕР№С‡РёРІРѕРј РєРѕРЅС‚РµРєСЃС‚Рµ,
                    // С‡С‚РѕР±С‹ РµРіРѕ С‡Р»РµРЅС‹ (Рё РёС… Р°РґСЂРµСЃР° РІ РєСѓС‡Рµ) СЃСЃС‹Р»Р°Р»РёСЃСЊ РЅР° СЃС‚Р°Р±РёР»СЊРЅРѕРµ С…СЂР°РЅРёР»РёС‰Рµ
                    ContextObject ns_obj(namespace_node->name, Value(Ty::Namespace, Namespace()));
                    ns_obj.modifiers = active_mods;
                    context.add_variable(namespace_node->name, ns_obj);
                    ContextObject& stored = context.get_variable(namespace_node->name);
                    stored.addr = heap.allocate_high(&stored);

                    Namespace& ns = stored.value.get_ref<Namespace>();
                    ns.context.parent = &context;
                    // РјРѕРґРёС„РёРєР°С‚РѕСЂС‹ (РЅР°РїСЂ. strict) РѕС‚РЅРѕСЃСЏС‚СЃСЏ Рє СЃР°РјРѕРјСѓ РЅРµР№РјСЃРїРµР№СЃСѓ,
                    // РЅРѕ РЅРµ РґРѕР»Р¶РЅС‹ В«РїСЂРѕС‚РµРєР°С‚СЊВ» РІРЅСѓС‚СЂСЊ РµРіРѕ С‚РµР»Р°
                    ContextObject::Modifiers saved_mods = active_mods;
                    bool saved_global = active_global;
                    active_mods = {};
                    execute(namespace_node->body, ns.context);
                    active_mods = saved_mods;
                    active_global = saved_global;
                    break;
                }
                Namespace ns;
                ns.context.parent = &context;
                ContextObject::Modifiers saved_mods = active_mods;
                bool saved_global = active_global;
                active_mods = {};
                execute(namespace_node->body, ns.context);
                active_mods = saved_mods;
                active_global = saved_global;
                return Value(Ty::Namespace, ns);
            }

            case NodeTypes::NodeIf: {
                auto* if_node = static_cast<struct NodeIf*>(node);
                Value cond = execute(if_node->condition, context);

                bool taken = false;
                if (!truthyOf(cond, if_node->token, context, taken)) {
                    error_container.addError(DEEP_ERRORS::IF_CONDITION_NOT_BOOL(if_node->token, cond.T.toString()));
                    break;
                }

Node* real_branch = taken ? if_node->then_body : if_node->else_body;
                Node* dead_branch  = taken ? if_node->else_body : if_node->then_body;

                // Статически недостижимая ветка по охране `typeof(x) == T`
                // (T не входит в статический тип x): её `ret` не проверяем на
                // соответствие типу возврата — ветка всё равно не исполняется.
                int guardDead = deadBranchByTypeGuard(if_node->condition, context);
                auto branchSuppressed = [&](Node* br) -> bool {
                    if (!br) return false;
                    if (br == if_node->then_body && guardDead == 1) return true;
                    if (br == if_node->else_body && guardDead == 2) return true;
                    return false;
                };

                // взятая ветка — исполняем по-настоящему (эффекты и подсказки сохраняются);
                // if в выражении (`let a = if (c) { 0 } else { 1 };`) отдаёт значение ветки
                Value result = new_null();
                if (real_branch) {
                    int add = branchSuppressed(real_branch) ? 1 : 0;
                    supp_ret_mismatch += add;
                    result = execute(real_branch, context);
                    supp_ret_mismatch -= add;
                }

                // РЅРµРІР·СЏС‚Р°СЏ РІРµС‚РєР° вЂ” В«СЃСѓС…РѕР№В» РїСЂРѕРіРѕРЅ: СЃРѕР±РёСЂР°РµРј РѕС€РёР±РєРё, РЅРѕ РЅРµ СЃРѕС…СЂР°РЅСЏРµРј
                // СЌС„С„РµРєС‚С‹ (scope / inlay / heap / aborted / СѓРїСЂР°РІР»РµРЅРёРµ РїРѕС‚РѕРєРѕРј), С‡С‚РѕР±С‹
                // РЅРµ РїР»РѕРґРёС‚СЊ РјС‘СЂС‚РІС‹Рµ РїРµСЂРµРјРµРЅРЅС‹Рµ Рё РЅРµ В«РїСЂРѕС‚РµРєР°Р»РёВ» break/continue/return
                if (dead_branch) {
                    int saved_supp = supp_ret_mismatch;
                    if (branchSuppressed(dead_branch)) ++supp_ret_mismatch;
                    // Чистая мёртвая ветка (только чтение / арифметика / сравнение /
                    // return / break — не пишет ни в контекст, ни в кучу, не вызывает
                    // функции) исполняется «всухую» прямо на живом контексте без копии:
                    // иначе сортировка растущего контейнера (Dict.contain) даёт O(N³).
                    if (deadBranchIsPure(dead_branch)) {
                        ++prof_pure_count;
                        auto saved_hints = std::move(inlay_hints);
                        bool saved_aborted = aborted;
                        ControlFlow saved_flow = control_flow;
                        Value saved_ret = return_value;
                        int saved_loop = loop_depth;
                        int saved_budget = iter_budget;
                        bool saved_budget_exhausted = budget_exhausted;
                        bool saved_dry = dry_exec;
                        Context* saved_dry_frame = dry_frame;
                        dry_exec = true;
                        dry_frame = nullptr;
                        execute(dead_branch, context);
                        dry_frame = saved_dry_frame;
                        dry_exec = saved_dry;
                        inlay_hints = std::move(saved_hints);
                        aborted = saved_aborted;
                        control_flow = saved_flow;
                        return_value = saved_ret;
                        loop_depth = saved_loop;
                        iter_budget = saved_budget;
                        budget_exhausted = saved_budget_exhausted;
                    } else {
                    ++prof_copy_count;
                    Context scratch = context;
                    auto saved_hints = std::move(inlay_hints);
                    bool saved_aborted = aborted;
                    VirtualHeap saved_heap = heap;
                    ControlFlow saved_flow = control_flow;
                    Value saved_ret = return_value;
                    int saved_loop = loop_depth;
                    int saved_budget = iter_budget;
                    bool saved_budget_exhausted = budget_exhausted;
                    bool saved_dry = dry_exec;
                    Context* saved_dry_frame = dry_frame;
                    dry_exec = true;
                    dry_frame = &scratch;
                    execute(dead_branch, scratch);
                    dry_frame = saved_dry_frame;
                    dry_exec = saved_dry;
                    inlay_hints = std::move(saved_hints);
                    aborted = saved_aborted;
                    heap = saved_heap;
                    control_flow = saved_flow;
                    return_value = saved_ret;
                    loop_depth = saved_loop;
                    iter_budget = saved_budget;
                    budget_exhausted = saved_budget_exhausted;
                    }
                    supp_ret_mismatch = saved_supp;
                }
                return result;
            }

            case NodeTypes::NodeWhile: {
                auto* w = static_cast<struct NodeWhile*>(node);
                // LSP (symbols_only): тела циклов не прокручиваем — только
                // собираем символы. Типы/хинты даёт TypeAnalyzer. Исключение —
                // циклы внутри `eval`: их исполняет драйвер eval (нужны значения).
                if (symbols_only && !in_eval) break;
                loop_depth++;
                if (loop_depth == 1 && !eval_budget_shared) { iter_budget = 10000; budget_exhausted = false; }
                while (true) {
                    if (budget_exhausted) break;
                    if (--iter_budget < 0) {
                        budget_exhausted = true;
                        any_budget_hit = true;
                        if (in_eval) error_container.addError(DEEP_ERRORS::EVAL_BUDGET_EXCEEDED(w->token));
                        else error_container.addError(DEEP_ERRORS::LOOP_NOT_TERMINATING(w->token));
                        break;
                    }
                    if (control_flow != ControlFlow::None) break;
                    if (!evalCondition(w->condition, w->token, context)) break;
                    // C++/Rust: тело цикла — свежая область видимости на каждую
                    // итерацию. `let` внутри тела не конфликтует между итерациями
                    // (нет FINAL_REDECLARED) и не утекает за пределы цикла.
                    // Контекст сохраняем в ctx_pool: куча может держать указатели.
                    auto iter_ctx_own = std::make_unique<Context>(ContextType::FunctionT);
                    Context& iter_ctx = *iter_ctx_own;
                    ctx_pool.push_back(std::move(iter_ctx_own));
                    iter_ctx.parent = &context;
                    execute(w->body, iter_ctx);
                    if (control_flow == ControlFlow::Break_)    { control_flow = ControlFlow::None; break; }
                    if (control_flow == ControlFlow::Continue_) { control_flow = ControlFlow::None; continue; }
                    if (control_flow == ControlFlow::Return_)   { break; }
                }
                loop_depth--;
                if (loop_depth == 0 && !eval_budget_shared) { iter_budget = 10000; budget_exhausted = false; }
                break;
            }

            case NodeTypes::NodeFor: {
                auto* f = static_cast<struct NodeFor*>(node);
                if (symbols_only && !in_eval) break;   // LSP: тела циклов не прокручиваем (кроме eval)
                // C++/Rust: переменная инициализации живёт в области видимости
                // цикла (init/check/update), тело — свежая область на итерацию.
                // После цикла `let i` из init недоступна снаружи.
                auto loop_ctx_own = std::make_unique<Context>(ContextType::FunctionT);
                Context& loop_ctx = *loop_ctx_own;
                ctx_pool.push_back(std::move(loop_ctx_own));
                loop_ctx.parent = &context;
                if (f->init) execute(f->init, loop_ctx);
                loop_depth++;
                if (loop_depth == 1 && !eval_budget_shared) { iter_budget = 10000; budget_exhausted = false; }
                while (true) {
                    if (budget_exhausted) break;
                    if (--iter_budget < 0) {
                        budget_exhausted = true;
                        any_budget_hit = true;
                        if (in_eval) error_container.addError(DEEP_ERRORS::EVAL_BUDGET_EXCEEDED(f->token));
                        else error_container.addError(DEEP_ERRORS::LOOP_NOT_TERMINATING(f->token));
                        break;
                    }
                    if (control_flow != ControlFlow::None) break;
                    if (f->check) {
                        if (!evalCondition(f->check, f->token, loop_ctx)) break;
                    }
                    auto iter_ctx_own = std::make_unique<Context>(ContextType::FunctionT);
                    Context& iter_ctx = *iter_ctx_own;
                    ctx_pool.push_back(std::move(iter_ctx_own));
                    iter_ctx.parent = &loop_ctx;
                    execute(f->body, iter_ctx);
                    if (control_flow == ControlFlow::Break_)    { control_flow = ControlFlow::None; break; }
                    if (control_flow == ControlFlow::Continue_) { control_flow = ControlFlow::None; /* РїР°РґР°РµРј РІ update */ }
                    if (control_flow == ControlFlow::Return_)   { break; }
                    if (f->update) execute(f->update, loop_ctx);
                }
                loop_depth--;
                if (loop_depth == 0 && !eval_budget_shared) { iter_budget = 10000; budget_exhausted = false; }
                break;
            }

            case NodeTypes::NodeBreak:
                if (loop_depth > 0) control_flow = ControlFlow::Break_;
                break;

            case NodeTypes::NodeContinue:
                if (loop_depth > 0) control_flow = ControlFlow::Continue_;
                break;

            case NodeTypes::NodeReturn: {
                auto* r = static_cast<struct NodeReturn*>(node);
                // ref-функция в write-режиме: вместо чтения пишем отложенное значение
                // в lvalue-путь цели (write-back `get() = v` / `get().field = v`)
                if (current_returns_ref && current_write_active && ref_write_armed) {
                    if (r->expr && isAssignablePath(r->expr, context))
                        writePathValue(r->expr, context, ref_write_val, r->token);
                    ref_write_consumed = true;
                    ref_write_armed = false;
                    return_value = ref_write_val;
                    control_flow = ControlFlow::Return_;
                    break;
                }
                // ref-функция в read-режиме обязана вернуть lvalue (живую цель),
                // иначе `ref let a = get();`/`get() = v` лишены смысла
                if (current_returns_ref && r->expr && !isAssignablePath(r->expr, context)) {
                    error_container.addError(DEEP_ERRORS::REF_RVALUE(r->token, r->token, "return"));
                }
                Value rv = r->expr ? execute(r->expr, context) : new_null();
                // value-функция не может вернуть живой реф (реф-значение): молча
                // разыменовать алиас в копию нельзя. Проверяем ПОСЛЕ execute —
                // resolved_calls к этому моменту содержит декларацию вызова.
                if (!current_returns_ref && r->expr && exprIsLiveRef(r->expr, context))
                    error_container.addError(DEEP_ERRORS::VALUE_RETURNS_REF(r->token));
                if (supp_ret_mismatch == 0
                        && !current_ret_type.is_auto && !current_ret_type.is_any && !(current_ret_type << rv.T))
                    error_container.addError(DEEP_ERRORS::RETURN_TYPE_MISMATCH(
                        r->token, current_ret_type.toString(), rv.T.toString()));
                // указатель на локаль этой функции уходить наружу не должен (P2):
                // он целится в ячейку фрейма, который исчезнет на return
                if (current_fn_scope != 0 && escapedLocalPtr(rv, current_fn_scope))
                    error_container.addError(DEEP_ERRORS::POINTER_ESCAPES_SCOPE(
                        r->token, "captured through '&'"));
                return_value = rv;
                control_flow = ControlFlow::Return_;
                break;
            }

            case NodeTypes::NodeTypeOf: {
                auto* t = static_cast<struct NodeTypeOf*>(node);
                if (t->is_static) {
                    // typeis(...) — статический (объявленный) тип как Type-значение.
                    Type st = staticTypeOfExpr(t->expr, context);
                    Value result = new_type(st);
                    if (st.isArray() && !st.container_1.empty()) {
                        t->static_type_elem = st.container_1[0].toString();
                        t->has_static_elem = true;
                    }
                    String repr = formatValue(result);
                    if (!t->has_static_type) {
                        t->static_type = repr;
                        t->has_static_type = true;
                    } else if (t->static_type != repr) {
                        t->static_type_ambiguous = true;
                    }
                    return result;
                }
                // объект без автораспаковки: __typeof__ диспетчится на самой обёртке
                Value v = execute(t->expr, context);
                std::vector<Value> empty_args;
                Value dd(Ty::Null, nullptr);
                Value result = new_type(v.T);
                if (v.T.isArray() && !v.T.container_1.empty()) {
                    t->static_type_elem = v.T.container_1[0].toString();
                    t->has_static_elem = true;
                }
                if (tryDunder("__typeof__", v, empty_args, context, t->token, t->token, dd))
                    result = dd;
                String repr = formatValue(result);
                if (!t->has_static_type) {
                    t->static_type = repr;
                    t->has_static_type = true;
                } else if (t->static_type != repr) {
                    t->static_type_ambiguous = true;
                }
                return result;
            }

            case NodeTypes::NodeMod: {
                auto* m = static_cast<struct NodeMod*>(node);
                resolveMod(m, context);
                return new_bool(m->value);
            }

            case NodeTypes::NodeSizeOf: {
                auto* s = static_cast<struct NodeSizeOf*>(node);
                // объект без автораспаковки: __sizeof__ диспетчится на самой обёртке
                Value v = execute(s->expr, context);
                std::vector<Value> empty_args;
                Value dd(Ty::Null, nullptr);
                if (tryDunder("__sizeof__", v, empty_args, context, s->token, s->token, dd))
                    return dd;
                if (v.T == Ty::Str)
                    return new_i64(static_cast<long long>(v.get<String>().size()));
                if (v.T == Ty::Char)
                    return new_i64(1);
                if (v.T.isArray() || v.T.isTuple()) {
                    RuntimeArray& arr = v.get_ref<RuntimeArray>();
                    return new_i64(static_cast<long long>(arr.size()));
                }
                error_container.addError(DEEP_ERRORS::SIZEOF_INVALID_TYPE(
                    s->token, v.T.toString()));
                return new_null();
            }

            case NodeTypes::NodePtr: {
                auto* c = static_cast<struct NodePtr*>(node);
                Value av = execute(c->addr, context);
                if (av.T == Ty::Null || !isIntegerType(av.T)) {
                    error_container.addError(DEEP_ERRORS::PTR_INVALID_ADDRESS(
                        c->token, av.T.toString()));
                    return new_null();
                }
                long long a = getNumberFromType<long long>(av);
                if (a < 0) {
                    error_container.addError(DEEP_ERRORS::PTR_INVALID_ADDRESS(
                        c->token, formatValue(av)));
                    return new_null();
                }
                Type ty = evalTypeExpr(c->type, context);
                return new_pointer(ty, static_cast<uint64_t>(a));
            }

            case NodeTypes::NodeCast: {
                auto* c = static_cast<struct NodeCast*>(node);
                Value lv = execute(c->l_node, context);
                Type target = evalTypeExpr(c->r_node, context);
                // РџСЂР°РІРѕ вЂ” РЅРµ С‚РёРї: РїС‹С‚Р°РµРјСЃСЏ РїРѕР»СѓС‡РёС‚СЊ С‚РёРї С‡РµСЂРµР· РїРµСЂРµРјРµРЅРЅСѓСЋ-С‚РёРї.
                if (target.isAny() && c->r_node->type == NodeTypes::NodeVariable) {
                    String nm = static_cast<struct NodeVariable*>(c->r_node)->name;
                    if (context.has_variable(nm)) {
                        ContextObject& co = context.get_variable(nm);
                        if (co.value.T == Ty::Type_) target = co.value.get<Type>();
                    }
                }
                if (target.isAny()) {
                    error_container.addError(DEEP_ERRORS::EXPECTED_TYPE_EXPRESSION(
                        c->r_node->type == NodeTypes::NodeVariable
                            ? static_cast<struct NodeVariable*>(c->r_node)->token
                            : c->token,
                        target.toString()));
                    return lv;
                }
                Type src = lv.T;
                if (src == target) return lv;

                // Перегрузка приведения: `expr as T` -> `expr.__as__(T)`, если у struct есть метод.
                {
                    std::vector<Value> cast_args;
                    cast_args.push_back(new_type(target));
                    Value dd(Ty::Null, nullptr);
                    if (tryDunder("__as__", lv, cast_args, context, c->token, c->token, dd))
                        return dd;
                }

                auto toStr = [](const Type& t) -> String { return t.toString(); };

                // bool -> numeric (i8..i64, f32..f64)
                if (src == Ty::Bool) {
                    bool val = lv.get<bool>();
                    double num = val ? 1.0 : 0.0;
                    if (isIntegerType(target)) {
                        int64_t iv = static_cast<int64_t>(num);
                        if (target == Ty::i8)  return new_i8(static_cast<int8_t>(iv));
                        if (target == Ty::i16) return new_i16(static_cast<int16_t>(iv));
                        if (target == Ty::i32) return new_i32(static_cast<int32_t>(iv));
                        if (target == Ty::i64) return new_i64(iv);
                    }
                    if (isFloatType(target)) {
                        if (target == Ty::f16) return new_f16(num);
                        if (target == Ty::f32) return new_f32(static_cast<float>(num));
                        if (target == Ty::f64) return new_f64(num);
                    }
                }

                // numeric -> bool (ненулевое значение -> true), согласовано с rt::truthy/cast
                if (isNumberType(src) && target == Ty::Bool) {
                    return new_bool(getNumberFromType<double>(lv) != 0);
                }

                // integer -> float
                if (isIntegerType(src) && isFloatType(target)) {
                    double num = getNumberFromType<double>(lv);
                    if (target == Ty::f16) return new_f16(num);
                    if (target == Ty::f32) return new_f32(static_cast<float>(num));
                    if (target == Ty::f64) return new_f64(num);
                }

                // float -> integer (truncation)
                if (isFloatType(src) && isIntegerType(target)) {
                    int64_t num = static_cast<int64_t>(getNumberFromType<double>(lv));
                    if (target == Ty::i8)  return new_i8(static_cast<int8_t>(num));
                    if (target == Ty::i16) return new_i16(static_cast<int16_t>(num));
                    if (target == Ty::i32) return new_i32(static_cast<int32_t>(num));
                    if (target == Ty::i64) return new_i64(num);
                }

                // integer -> integer (widening/narrowing)
                if (isIntegerType(src) && isIntegerType(target)) {
                    int64_t num = getNumberFromType<int64_t>(lv);
                    if (target == Ty::i8)  return new_i8(static_cast<int8_t>(num));
                    if (target == Ty::i16) return new_i16(static_cast<int16_t>(num));
                    if (target == Ty::i32) return new_i32(static_cast<int32_t>(num));
                    if (target == Ty::i64) return new_i64(num);
                }

                // float -> float (widening/narrowing)
                if (isFloatType(src) && isFloatType(target)) {
                    double num = getNumberFromType<double>(lv);
                    if (target == Ty::f16) return new_f16(num);
                    if (target == Ty::f32) return new_f32(static_cast<float>(num));
                    if (target == Ty::f64) return new_f64(num);
                }

                // char -> integer (кодовая точка Unicode)
                if (src == Ty::Char && isIntegerType(target)) {
                    int64_t num = static_cast<int64_t>(lv.get<char32_t>());
                    if (target == Ty::i8)  return new_i8(static_cast<int8_t>(num));
                    if (target == Ty::i16) return new_i16(static_cast<int16_t>(num));
                    if (target == Ty::i32) return new_i32(static_cast<int32_t>(num));
                    if (target == Ty::i64) return new_i64(num);
                }

                // enum -> integer (variant index)
                if (src.isEnum() && isIntegerType(target)) {
                    int64_t num = lv.get<long long>();
                    if (target == Ty::i8)  return new_i8(static_cast<int8_t>(num));
                    if (target == Ty::i16) return new_i16(static_cast<int16_t>(num));
                    if (target == Ty::i32) return new_i32(static_cast<int32_t>(num));
                    if (target == Ty::i64) return new_i64(num);
                }

                // integer -> char
                if (isIntegerType(src) && target == Ty::Char) {
                    int64_t num = getNumberFromType<int64_t>(lv);
                    return new_char(static_cast<char32_t>(num));
                }

                // pointer -> pointer (СЂРµРёРЅС‚РµСЂРїСЂРµС‚Р°С†РёСЏ Р°РґСЂРµСЃР°)
                if (src.isPointer() && target.isPointer()) {
                    Pointer p = lv.get<Pointer>();
                    return new_pointer(target.container_1[0], p.addr);
                }

                error_container.addError(DEEP_ERRORS::INVALID_CAST(
                    c->token, toStr(src), toStr(target)));
                return lv;
            }

            case NodeTypes::NodeNew: {
                auto* new_node = static_cast<struct NodeNew*>(node);
                Value v = new_null();
                if (new_node->value)
                    v = execute(new_node->value, context);

                bool is_strict = false;
                bool is_const  = false;
                Node* strict_type_arg = nullptr;
                for (const auto& m : new_node->modifiers) {
                    if (m.name == "strict") { is_strict = true; strict_type_arg = m.type_arg; }
                    else if (m.name == "const")  is_const  = true;
                    else {
                        error_container.addError(DEEP_ERRORS::NEW_UNKNOWN_MODIFIER(new_node->token, String(m.name)));
                        return new_null();
                    }
                }

                Type pointee = v.T;
                if (is_strict && strict_type_arg) {
                    Value tv = execute(strict_type_arg, context);
                    if (tv.T != Ty::Type_) {
                        error_container.addError(DEEP_ERRORS::NEW_EXPECTED_TYPE(new_node->token, tv.T.toString()));
                        return new_null();
                    }
                    pointee = tv.get<Type>();
                    if (pointee.isAuto())
                        pointee = v.T;  // auto -> вывести тип из значения
                    if (is_strict && !pointee.isAuto() && !pointee.is_any && !(pointee << v.T)) {
                        error_container.addError(DEEP_ERRORS::STRICT_TYPE_MISMATCH(
                            new_node->token, pointee.toString(), v.T.toString()));
                        return new_null();
                    }
                }

                auto cell = std::make_unique<ContextObject>(String("new"), v, pointee);
                cell->modifiers.Strict = is_strict;
                cell->modifiers.Const  = is_const;
                uint64_t addr = heap.allocate(cell.get());
                new_cells.push_back(std::move(cell));
                return new_pointer(pointee, addr);
            }

            case NodeTypes::NodeFunctionCall:
                return executeFunctionCall(static_cast<struct NodeFunctionCall*>(node), context);

            default: break;
        }


        return new_null();
    }

    // РќРµР№С‚СЂР°Р»СЊРЅРѕРµ Р·РЅР°С‡РµРЅРёРµ reified-С‚РёРїР° РґР»СЏ В«СЃСѓС…РёС…В» РІС‹Р·РѕРІРѕРІ (РЅРµС‚ РїРѕР±РѕС‡РЅС‹С… СЌС„С„РµРєС‚РѕРІ),
    // С‡С‚РѕР±С‹ С‡РёСЃР»Р° РЅРµ РєР°СЃРєР°РґРёР»Рё UNSUPPORTED_OPERATOR РІРЅРµ РІР·СЏС‚С‹С… РІРµС‚РѕРє.
    Value neutralValue(const Type& t) {
        if (t == Ty::i8)  return new_i8(0);
        if (t == Ty::i16) return new_i16(0);
        if (t == Ty::i32) return new_i32(0);
        if (t == Ty::i64) return new_i64(0);
        if (t == Ty::f16) return new_f16(0);
        if (t == Ty::f32) return new_f32(0);
        if (t == Ty::f64) return new_f64(0);
        if (t == Ty::Bool) return new_bool(false);
        if (t == Ty::Char) return new_char(0);
        if (t == Ty::Str)  return new_string(String());
        if (t.isStruct())  return new_struct(t, {});
        return new_null();
    }

    // Р’С‹РїРѕР»РЅСЏРµС‚ РІС‹Р·РѕРІ С„СѓРЅРєС†РёРё: СЂРµР·РѕР»СЋС†РёСЏ РїРµСЂРµРіСЂСѓР·РєРё РїРѕ reified-С‚РёРїР°Рј Р°СЂРіСѓРјРµРЅС‚РѕРІ,
    // СЃРІРµР¶РёР№ FunctionT-РєРѕРЅС‚РµРєСЃС‚, РїСЂРёРІСЏР·РєР° РїР°СЂР°РјРµС‚СЂРѕРІ/РіРµРЅРµСЂРёРє-РёРјС‘РЅ, guard СЂРµРєСѓСЂСЃРёРё,
    // РёР·РѕР»СЏС†РёСЏ return/С†РёРєР»РѕРІ РІС‹Р·С‹РІР°СЋС‰РµРіРѕ.
    // Перегрузка операторов (dunder): если левый операнд — struct, у которого
    // найден метод dname (напр. `__add__`), вызываем его обычным диспатчем
    // методов (self = левый операнд, args = правые). Возвращается true, если
    // перегрузка применена, и результат кладётся в out.
    bool tryDunder(const String& dname, Value& self_v,
                   const std::vector<Value>& args,
                   Context& context,
                   const Token& start, const Token& end,
                   Value& out) {
        if (!self_v.T.isStruct()) return false;
        auto sit = structs.find(self_v.T.name);
        if (sit == structs.end()) return false;
        if (sit->second.methods.find(dname) == sit->second.methods.end()) return false;

        // Значения ресивера/аргументов кладём в скрытые временные переменные,
        // чтобы не переисполнять AST-выражения (побочные эффекты) во второй раз.
        auto stash = [&](const char* tag, const Value& v) -> String {
            String nm = String("$") + String(tag) + String(std::to_string(++dunder_temp));
            context.add_variable(nm, ContextObject(nm, v, v.T));
            return nm;
        };

        String recv = stash("dunder_self_", self_v);
        std::vector<Node*> c_args;
        c_args.reserve(args.size());
        for (const Value& a : args)
            c_args.push_back(new struct NodeVariable(stash("dunder_arg_", a)));

        auto* callee = new struct NodeMemberAccess(
            new struct NodeVariable(recv), new struct NodeVariable(dname), start);
        auto* call = new struct NodeFunctionCall(callee, c_args, {}, start, end);
        out = executeFunctionCall(call, context);
        return true;
    }

    // Присваивание через индекс: `obj[i] = v` -> `obj.__set_index__(i, v)`.
    // Значения ресивера/индексов/правой части кладём в скрытые временные
    // переменные (однократное исполнение), мутированный self из временной
    // переменной переносим обратно в реальный слот владельца. self_v уже
    // вычислен вызывающей стороной (base исполняется один раз).
    // Общий путь для dunder-вызовов, мутирующих self (__set_index__, __del__ и т.п.):
    // стягивает self и аргументы во временные переменные, выполняет метод и
    // записывает изменённый self обратно в реальный слот (own / поле структуры).
    bool callDunderWithWriteback(const String& dname, Node* base_node, const Value& self_v,
                                 std::vector<Value>& args, const Token& tok, Context& context) {
        auto stash = [&](const String& tag, const Value& v) -> String {
            String nm = String("$") + tag + String(std::to_string(++dunder_temp));
            context.add_variable(nm, ContextObject(nm, v, v.T));
            return nm;
        };
        String recv = stash(dname + String("_self_"), self_v);
        std::vector<Node*> c_args;
        c_args.reserve(args.size());
        for (Value& a : args)
            c_args.push_back(new struct NodeVariable(stash(dname + String("_arg_"), a)));
        auto* callee = new struct NodeMemberAccess(
            new struct NodeVariable(recv), new struct NodeVariable(dname), tok);
        auto* call = new struct NodeFunctionCall(callee, c_args, {}, tok, tok);
        // пока выполняется тело __assign__, вложенные __assign__-диспетчи глушим
        bool was_assign_active = dunder_assign_active;
        if (dname == String(U"__assign__")) ++dunder_assign_active;
        executeFunctionCall(call, context);
        if (dname == String(U"__assign__")) --dunder_assign_active;
        (void)was_assign_active;

        // write-back: метод мутирует self — переносим результат в реальный слот
        if (!context.has_variable_local(recv)) return false;
        Value mutated = context.get_variable_local(recv).value;
        ContextObject* own = nullptr;
        if (base_node->type == NodeTypes::NodeVariable) {
            String nm = static_cast<struct NodeVariable*>(base_node)->name;
            if (context.has_variable(nm)) own = &context.get_variable(nm);
        } else if (base_node->type == NodeTypes::NodeUnaryDereference) {
            auto* dn = static_cast<struct NodeUnaryDereference*>(base_node);
            Value ptr = execute(dn->node, context);
            if (!ptr.T.isPointer()) return false;
            Pointer p = ptr.get<Pointer>();
            if (p.addr == 0 || !heap.contains(p.addr)) return false;
            own = heap.get(p.addr);
        } else if (base_node->type == NodeTypes::NodeBinaryScopeResolution) {
            own = resolveScopeMember(base_node, context);
        } else if (base_node->type == NodeTypes::NodeMemberAccess) {
            auto* ma_n = static_cast<struct NodeMemberAccess*>(base_node);
            String field = memberNameNode(ma_n->r_node);
            ContextObject* holder = structOwnerFor(ma_n->l_node, context);
            if (!holder || !holder->value.T.isStruct()) return false;
            if (const_receiver && holder == const_receiver) {
                error_container.addError(DEEP_ERRORS::CONST_FN_MUTATION(ma_n->member_token, field));
                return false;
            }
            RuntimeStruct& rs = holder->value.get_ref<RuntimeStruct>();
            auto fit = rs.mut().find(field);
            if (fit == rs.view().end()) return false;
            if (!dry_exec) fit->second = mutated;
            return true;
        } else if (base_node->type == NodeTypes::NodeArrayAccess) {
            // элемент массива: мутируем копию массива и пишем её обратно в базу
            // (в т.ч. сквозь ref-алиас элемента). Повторный __assign__-диспетч
            // при записи результата дюндера в цель подавляем — иначе дюндер
            // исполнится второй раз и затрёт поле значением-структурой.
            auto* aa_n = static_cast<struct NodeArrayAccess*>(base_node);
            Value arr = execute(aa_n->base, context);
            bool saved = assign_dunder_disabled;
            assign_dunder_disabled = true;
            bool ok = setIndexRec(arr, aa_n->indices, 0, tok, context, mutated)
                && writePathValue(aa_n->base, context, arr, tok);
            assign_dunder_disabled = saved;
            return ok;
        }
        if (!own) return false;
        if (own->modifiers.Final) {
            error_container.addError(DEEP_ERRORS::FINAL_REASSIGNED(tok, own->identifier));
            return false;
        }
        if (own->modifiers.Const) {
            error_container.addError(DEEP_ERRORS::CONST_REASSIGNED(tok, own->identifier));
            return false;
        }
        if (const_receiver && own == const_receiver) {
            error_container.addError(DEEP_ERRORS::CONST_FN_MUTATION(tok, "index"));
            return false;
        }
        if (!dry_exec) own->value = mutated;
        return true;
    }

    bool callIndexSetter(Node* base_node, const Value& self_v,
                         const std::vector<Node*>& idxs,
                         const Value& rhs, const Token& tok, Context& context) {
        std::vector<Value> args;
        args.reserve(idxs.size() + 1);
        for (Node* i : idxs)
            args.push_back(execute(i, context));
        args.push_back(rhs);
        return callDunderWithWriteback("__set_index__", base_node, self_v, args, tok, context);
    }

    Value executeFunctionCall(struct NodeFunctionCall* call, Context& context) {
        if (!call->callee) return new_null();
        // новый top-level прогон (вне вложенных вызовов) — бюджет вызовов свежий
        if (call_depth == 0) { call_budget = CALL_BUDGET_LIMIT; call_budget_exhausted = false; }
        FunctionContainer* fc = nullptr;
        String callee_name;
        ContextObject* callee_obj = nullptr;
        Value cv = new_null();

        // метод структуры `p.push(...)`: self подставляется первым аргументом,
        // после тела значение self сохраняется обратно в объект-владелец
        bool struct_method_call = false;
        String recv_self_name;
        ContextObject* recv_owner = nullptr;
        bool call_static = false;   // вызываемый статически известен (можно раскладывать именованные аргументы)
        Node* recv_member_node = nullptr;   // path `a.b.c.method()` для write-back по пути
        Node* recv_arg_node = nullptr;      // левая часть '.' — узел-ресивер (для ref self)
        Value recv_value = new_null();
        bool call_self_temp = false;   // __call__-ресивер — rvalue (цепочка f()(…)): self без write-back
        const StructSymbol* csym = nullptr;   // структура-владелец вызываемого метода

        // Статические type-алиасы структуры («static let Self: Type = Vec2[T]»)
        // кладём в контекст РЕИФИЦИРОВАННЫМИ: значение алиаса пересчитываем в
        // контексте с привязкой generic-имён инстанса (T -> i64), чтобы
        // `self: Self` в параметрах методов сопоставлялось с конкретным типом
        // получателя, а не с символическим `Vec2[T]` (иначе «No matching overload»
        // на первом проходе анализа — алиас-поле хранит символический тип).
        auto addStaticAliases = [this](const StructSymbol* s, const Type& rvT, Context& bctx) -> void {
            std::vector<Value> sbi;
            if (!bindingsForType(*s, rvT, sbi)) return;
            for (size_t gi = 0; gi < s->generics.size() && gi < sbi.size(); ++gi) {
                const GenericParam& sgp = s->generics[gi];
                ContextObject co(sgp.name, sbi[gi], Ty::Null);
                co.is_type_decl = s->is_type_generic(sgp);
                bctx.add_variable(sgp.name, co);
            }
            for (const String& sf : s->static_fields) {
                const StructFieldDef* fd = s->field_def(sf);
                if (!fd || !fd->default_node) continue;
                Value sv = execute(fd->default_node, bctx);
                if (sv.T != Ty::Type_) continue;
                ContextObject co(sf, sv, Ty::Type_);
                co.is_type_decl = true;
                bctx.add_variable(sf, co);
            }
        };

        auto isCallableLike = [](const Type& t) -> bool {
            if (t.isCallable()) return true;
            if (t.isUnion())
                for (const auto& m : t.container_1) if (m.isCallable()) return true;
            return false;
        };

        if (call->callee->type == NodeTypes::NodeVariable) {
            auto* vn = static_cast<struct NodeVariable*>(call->callee);
            callee_name = vn->name;
            if (context.has_function_container(callee_name)) {
                call_static = true;
                fc = &context.get_function_container(callee_name);
            } else if (context.has_variable(callee_name)) {
                callee_obj = &context.get_variable(callee_name);
                if (isCallableLike(callee_obj->value.T))
                    fc = &callee_obj->value.get_ref<FunctionContainer>();
            } else {
                error_container.addError(DEEP_ERRORS::FUNCTION_NOT_FOUND(vn->token, callee_name));
                return new_null();
            }
        } else if (call->callee->type == NodeTypes::NodeBinaryScopeResolution) {
            auto* sr = static_cast<struct NodeBinaryScopeResolution*>(call->callee);
            if (sr->r_node && sr->r_node->type == NodeTypes::NodeVariable)
                callee_name = static_cast<struct NodeVariable*>(sr->r_node)->name;
            callee_obj = resolveScopeMember(call->callee, context);
            if (!callee_obj) return new_null();
            call_static = true;
            if (isCallableLike(callee_obj->value.T))
                fc = &callee_obj->value.get_ref<FunctionContainer>();
            {
                // статические методы структур: метод принадлежит структуре
                Value root = execute(sr->l_node, context);
                if (root.T == Ty::Type_ && root.get<Type>().isStruct())
                    csym = structSymForType(root.get<Type>());
            }
            // Вызов метода структуры через '::' без ресивера: инстанс-методы —
            // это ошибка (self нужен объект), а не «нет перегрузки». Если у имени
            // есть статическая перегрузка — резолв идёт штатно ниже.
            if (csym && fc && !fc->functions.empty()) {
                bool has_static = false;
                for (const Function& fn : fc->functions)
                    if (fn.is_static) { has_static = true; break; }
                if (!has_static) {
                    Token tt = (sr->r_node && sr->r_node->type == NodeTypes::NodeVariable)
                                   ? static_cast<struct NodeVariable*>(sr->r_node)->token
                                   : call->open_token;
                    error_container.addError(DEEP_ERRORS::INSTANCE_METHOD_SCOPE_CALL(
                        tt, callee_name, csym->name));
                    return new_null();
                }
            }
        } else if (call->callee->type == NodeTypes::NodeMemberAccess) {
            auto* ma = static_cast<struct NodeMemberAccess*>(call->callee);
            String mname = memberNameNode(ma->r_node);
            callee_name = mname;
            // ресивер метода — сам объект обёртки (без автораспаковки __get__),
            // иначе дюндер-диспетч рекурсирует на закешированном self
recv_value = execute(ma->l_node, context);
              if (recv_value.T.isPointer()) {
                Pointer p = recv_value.get<Pointer>();
                auto fs = field_slots.find(p.addr);
                if (fs != field_slots.end()) {
                    RuntimeStruct& rs = fs->second.first->value.get_ref<RuntimeStruct>();
                        auto fit = rs.view().find(fs->second.second);
                    if (fit == rs.view().end()) {
                        error_container.addError(DEEP_ERRORS::NO_SUCH_FIELD(
                            ma->member_token, fs->second.first->value.T.name, fs->second.second));
                        return new_null();
                    }
                    recv_value = fit->second;
                } else {
                    if (p.addr == 0 || !heap.contains(p.addr)) {
                        error_container.addError(DEEP_ERRORS::NULL_DEREFERENCE(ma->member_token));
                        return new_null();
                    }
                    recv_value = heap.get(p.addr)->value;
                }
            }
            if (!recv_value.T.isStruct()) {
                error_container.addError(DEEP_ERRORS::MEMBER_OF_NON_STRUCT(
                    ma->member_token, recv_value.T.toString(), mname));
                return new_null();
            }
            auto sit = structs.find(recv_value.T.name);
            if (sit == structs.end()) {
                error_container.addError(DEEP_ERRORS::STRUCT_NOT_FOUND(ma->member_token, recv_value.T.name));
                return new_null();
            }
            auto mit = sit->second.methods.find(mname);
            if (mit == sit->second.methods.end()) {
                error_container.addError(DEEP_ERRORS::METHOD_NOT_FOUND(
                    ma->member_token, mname, recv_value.T.name));
                return new_null();
            }
            fc = &mit->second;
            struct_method_call = true;
            call_static = true;
            csym = &sit->second;
            if (!fc->functions.empty() && !fc->functions[0].arguments.empty())
                recv_self_name = fc->functions[0].arguments[0].name;
            recv_owner = structOwnerFor(ma->l_node, context);
            recv_arg_node = ma->l_node;
            if (!recv_owner) recv_member_node = ma->l_node;
        } else {
            cv = execute(call->callee, context);
            if (isCallableLike(cv.T))
                fc = &cv.get_ref<FunctionContainer>();
        }

        if (!fc) {
            // объект структуры с методом __call__ вызывается как функция:
            // это метод-вызов self.__call__(args) — self = объект, поэтому
            // ref-параметры алиасуются через аргументные узлы и мутации self
            // записываются обратно (write-back), как у обычных методов.
            Value selfv = callee_obj ? callee_obj->value : cv;
            if (selfv.T.isStruct()) {
                auto call_sit = structs.find(selfv.T.name);
                if (call_sit != structs.end()) {
                    auto call_mit = call_sit->second.methods.find(String(U"__call__"));
                    if (call_mit != call_sit->second.methods.end()) {
                        fc = &call_mit->second;
                        struct_method_call = true;
                        call_static = true;
                        csym = &call_sit->second;
                        recv_value = selfv;
                        if (!fc->functions.empty() && !fc->functions[0].arguments.empty())
                            recv_self_name = fc->functions[0].arguments[0].name;
                        recv_arg_node = call->callee;
                        recv_owner = structOwnerFor(call->callee, context);
                        if (!recv_owner) { recv_member_node = call->callee; call_self_temp = true; }
                    }
                }
            }
            // Тип структуры вызывается как функция: `Number(20)` -> статический
            // __call__ этой структуры. Здесь НЕ подставляется self (метод объявлен
            // static), поэтому struct_method_call остаётся false и аргументы
            // уходят в вызов как есть — так же, как для `Number::from(20)`.
            if (!fc) {
                Value tv = callee_obj ? callee_obj->value : cv;
                if (tv.T == Ty::Type_ && tv.get<Type>().isStruct()) {
                    auto tsit = structs.find(tv.get<Type>().name);
                    if (tsit != structs.end()) {
                        auto tmit = tsit->second.methods.find(String(U"__call__"));
                        if (tmit != tsit->second.methods.end()) {
                            bool has_static = false;
                            for (const Function& f : tmit->second.functions)
                                if (f.is_static) { has_static = true; break; }
                            if (has_static) {
                                fc = &tmit->second;
                                call_static = true;
                                csym = &tsit->second;
                                callee_name = String(U"__call__");
                            }
                        }
                    }
                }
            }
            if (!fc) {
                String found = callee_obj ? callee_obj->value.T.toString() : callee_name;
                error_container.addError(DEEP_ERRORS::CALL_WITHOUT_FUNCTION(call->open_token, found));
                return new_null();
            }
        }

        // Именованные аргументы поддерживаются только для статически известного
        // вызываемого: транспилятор раскладывает имена по декларации на этапе
        // компиляции (rt::call не умеет матчить имена в рантайме).
        if (!call_static) {
            bool has_named = false;
            for (auto* a : call->args)
                if (nodeIsNamedArgument(a)) { has_named = true; break; }
            if (has_named) {
                String dynName = callee_name.empty() ? String("callee") : callee_name;
                error_container.addError(DEEP_ERRORS::NAMED_ARG_ON_DYNAMIC_CALL(call->open_token, dynName));
                return new_null();
            }
        }

        // Р°СЂРіСѓРјРµРЅС‚С‹ (позиционные + именованные; мета-список параллелен args)
        std::vector<Value> args;
        args.reserve(call->args.size());
        std::vector<CallArgMeta> arg_meta = buildCallArgMetas(call->args);
        std::vector<Node*> argNodes;   // исходные узлы-выражения аргументов (для ref-алиасов)
        argNodes.reserve(call->args.size());
        for (auto* a : call->args) {
            args.push_back(nodeIsNamedArgument(a)
                               ? execute(static_cast<struct NodeNamedArgument*>(a)->value, context)
                               : execute(a, context));
            argNodes.push_back(nodeIsNamedArgument(a)
                                   ? static_cast<struct NodeNamedArgument*>(a)->value : a);
        }
        if (struct_method_call) {
            args.insert(args.begin(), recv_value);
            arg_meta.insert(arg_meta.begin(), CallArgMeta());
            argNodes.insert(argNodes.begin(), recv_arg_node);
        }

        // РіРµРЅРµСЂРёРє-Р°СЂРіСѓРјРµРЅС‚С‹ РІС‹Р·РѕРІР° (С‚РёРїС‹ РР›Р РєРѕРЅСЃС‚Р°РЅС‚С‹-Р·РЅР°С‡РµРЅРёСЏ)
        std::vector<Value> generic_args;
        for (auto* g : call->generics)
            generic_args.push_back(execute(g, context));

        // Р”РµРЎС„РѕР»С‚С‹ generic-РїР°СЂР°РјРµС‚СЂРѕРІ РјРµС‚РѕРґР° РјРѕРіСѓС‚ СЃСЃС‹Р»Р°С‚СЊСЃСЏ РЅР° generic-РёРјРµРЅР° СЃС‚СЂСѓРєС‚СѓСЂС‹
        // (fn push[PT = T](self, x: PT)): РІС‹С‡РёСЃР»СЏРµРј РёС… РІ РєРѕРЅС‚РµРєСЃС‚Рµ СЃ РїСЂРёРІСЏР·РєРѕР№
        // generic-РёРјС‘РЅ РёРЅСЃС‚Р°РЅСЃР° (T -> i64, Size -> -1) Рё РїРµСЂРµРґР°С‘Рј РєР°Рє Р·РЅР°С‡РµРЅРёСЏ.
        if (struct_method_call && csym && !fc->functions.empty()) {
            Context struct_bindings_ctx(ContextType::FunctionT);
            struct_bindings_ctx.parent = &context;
            addStaticAliases(csym, recv_value.T, struct_bindings_ctx);
            const Function& f0 = fc->functions[0];
            for (size_t gi = call->generics.size(); gi < f0.generics.size(); ++gi) {
                Node* dnode = f0.generics[gi].default_value;
                if (!dnode) { generic_args.push_back(new_null()); continue; }
                Value dv = execute(dnode, struct_bindings_ctx);
                generic_args.push_back(dv.T != Ty::Null ? dv : new_null());
            }
        }

        NodeEvaluator ev = [this](Node* n, Context& c) -> Value { return execute(n, c); };
        ResolvedCall rc;
        Token callStart = call->name_token.type != TypeOfToken::DUMMY ? call->name_token : call->open_token;

        // Контекст подстановки generic-имён структуры (T -> i64) для резолва
        // метода: типы параметров вида `self: Vec2[T]` реифицируются в нём, а не
        // в сыром контексте вызова (иначе «Undefined variable 'T'» и No matching
        // overload — структура называется generic-структурой, а узлы её параметров
        // используют имена её generic-параметров, которых нет в caller).
        Context* resolve_caller = &context;
        if (struct_method_call && csym) {
            Context bctx(ContextType::FunctionT);
            bctx.parent = &context;
            addStaticAliases(csym, recv_value.T, bctx);
            resolve_caller = &bctx;
            if (!resolve_function_call(error_container, *fc, callStart, call->close_token,
                                       args, arg_meta, generic_args, *resolve_caller, ev, rc,
                                       /*compute_return=*/csym == nullptr)) {
                if (fc->functions.empty())
                    error_container.addError(DEEP_ERRORS::FUNCTION_NOT_FOUND(call->open_token, fc->name));
                return new_null();
            }
        } else {
            if (!resolve_function_call(error_container, *fc, callStart, call->close_token,
                                       args, arg_meta, generic_args, *resolve_caller, ev, rc,
                                       /*compute_return=*/csym == nullptr)) {
                if (fc->functions.empty())
                    error_container.addError(DEEP_ERRORS::FUNCTION_NOT_FOUND(call->open_token, fc->name));
                return new_null();
            }
        }
        Function& fn = *rc.function;
        // Решение резолва — на будущее для кодогена: он эмитит вызов именно этой
        // перегрузки вместо собственного подбора по строковым тегам (см. CodeGenUnit).
        if (fn.decl) resolved_calls[call] = fn.decl;

        // Возвращаемый тип метода структуры реифицируем под конкретный вызов:
        // resolve_function_call знает только generic-имена функции, а имена
        // generic-параметров структуры (FirstT/SecondT) у него не привязаны —
        // rc.return_type вырождается (и проверка ret-типа молчит). Пересчитываем
        // декларацию return-типа в контексте с привязкой generic-имён функции и
        // (для instance-методов) generic-имён структуры к типам инстанса.
        if (csym && !fn.locked_types && fn.return_type) {
            Context ret_ctx(ContextType::FunctionT);
            ret_ctx.parent = &context;
            for (size_t gi = 0; gi < fn.generics.size(); ++gi) {
                String gname = fn.generics[gi].name;
                auto sit = rc.generic_values.find(gname);
                if (sit != rc.generic_values.end()) {
                    ret_ctx.add_variable(gname, ContextObject(gname, sit->second));
                } else {
                    auto it = rc.bindings.find(gname);
                    Type bound = (it != rc.bindings.end()) ? it->second : Ty::Any;
                    ContextObject co(gname, new_type(bound), Ty::Type_);
                    co.is_type_decl = true;
                    ret_ctx.add_variable(gname, co);
                }
            }
            if (struct_method_call && recv_value.T.isStruct())
                addStaticAliases(csym, recv_value.T, ret_ctx);
            Value rv = ev(fn.return_type, ret_ctx);
            rc.return_type = (rv.T == Ty::Type_) ? rv.get<Type>() : Ty::Any;
        }

        // состояние структуры/const-метода для тела вызова (save/restore)
        const StructSymbol* saved_active = active_struct;
        ContextObject* saved_const_recv = const_receiver;
        const bool method_is_const = fn.is_const;
        if (csym) active_struct = csym;
        if (struct_method_call && const_receiver && recv_owner == const_receiver && !method_is_const) {
            error_container.addError(DEEP_ERRORS::CONST_FN_INVOKES_MUTATOR(callStart, callee_name));
            active_struct = saved_active;
            return new_null();
        }

        // РІ СЃСѓС…РѕРј РїСЂРѕРіРѕРЅРµ РјС‘СЂС‚РІРѕР№ РІРµС‚РєРё С‚РµР»Рѕ РЅРµ РёСЃРїРѕР»РЅСЏРµРј Рё РЅРµ СЂРµРєСѓСЂСЃРёСЂСѓРµРј
        // (РёРЅР°С‡Рµ dead-РІРµС‚РєР° С„Р°РєС‚РѕСЂРёР°Р»Р° СѓС…РѕРґРёС‚ РІ Р±РµСЃРєРѕРЅРµС‡РЅСѓСЋ СЂРµРєСѓСЂСЃРёСЋ);
        // РІРѕР·РІСЂР°С‰Р°РµРј РЅРµР№С‚СЂР°Р»СЊРЅРѕРµ Р·РЅР°С‡РµРЅРёРµ reified-С‚РёРїР° СЂРµР·СѓР»СЊС‚Р°С‚Р°
        if (dry_exec) return neutralValue(rc.return_type);

        if (++call_depth > CALL_DEPTH_LIMIT) {
            error_container.addError(DEEP_ERRORS::CALL_RECURSION_LIMIT(call->open_token, fn.name));
            any_budget_hit = true;
            --call_depth;
            // РЅРµР№С‚СЂР°Р»СЊРЅРѕРµ Р·РЅР°С‡РµРЅРёРµ РѕР¶РёРґР°РµРјРѕРіРѕ С‚РёРїР°: СЂРµРєСѓСЂСЃРёРІРЅС‹Р№ РІС‹Р·РѕРІ В«РѕР±СЂРµР·Р°РµС‚СЃСЏВ»,
            // Рё ret РЅР° РєР°Р¶РґРѕРј СѓСЂРѕРІРЅРµ РѕР±СЏР·Р°РЅ СѓРІРёРґРµС‚СЊ С‚РёРї СЃРІРѕРµР№ РґРµРєР»Р°СЂР°С†РёРё (Р° РЅРµ Null)
            return neutralValue(current_ret_type);
        }

        // бюджет вызовов исчерпан: прекращаем глубокий прогон, отдаём нейтральное
        // значение объявленного типа (лучше, чем продолжать экспоненциальную рекурсию)
        if (call_budget_exhausted || --call_budget < 0) {
            call_budget_exhausted = true;
            any_budget_hit = true;
            if (!call_budget_warned) {
                call_budget_warned = true;
                error_container.addError(DEEP_ERRORS::CALL_BUDGET_EXHAUSTED(call->open_token, fn.name));
            }
            --call_depth;
            return neutralValue(rc.return_type);
        }

        // РёР·РѕР»РёСЂСѓРµРј СѓРїСЂР°РІР»РµРЅРёРµ РїРѕС‚РѕРєРѕРј РІРЅСѓС‚СЂРё С„СѓРЅРєС†РёРё
        ControlFlow saved_flow = control_flow;
        Value saved_ret = return_value;
        int saved_loop = loop_depth;
        int saved_budget = iter_budget;
        bool saved_budget_exhausted = budget_exhausted;
        Type saved_ret_type = current_ret_type;
        bool saved_cur_ret_ref = current_returns_ref;
        std::set<String> saved_ref_params = current_ref_params;
        bool saved_write_active = current_write_active;
        bool saved_ref_write_armed = ref_write_armed;
        Value saved_ref_write_val = ref_write_val;
        struct NodeFunctionCall* saved_ref_write_rcall = ref_write_rcall;
        int saved_supp_ret = supp_ret_mismatch;
        control_flow = ControlFlow::None;
        loop_depth = 0;
        supp_ret_mismatch = 0;   // охрана «мёртвой» ветки не действует внутри вызванной функции
        current_ret_type = fn.return_type ? rc.return_type : Ty::Any;
        current_returns_ref = fn.returns_ref;
        // имена ref-параметров — в теле это живые ссылки: `ret <такой параметр>`
        // из value-функции не пройдёт (VALUE_RETURNS_REF).
        current_ref_params.clear();
        for (const FuncArg& a : fn.arguments)
            if (std::find(a.modifiers.begin(), a.modifiers.end(), "ref") != a.modifiers.end())
                current_ref_params.insert(a.name);
        // write-back откладывается только для ТОГО же вызова; вложенные вызовы
        // (в т.ч. ref-функции) не должны «съесть» чужую отложенную запись.
        current_write_active = ref_write_armed && ref_write_rcall == call;

        auto call_ctx_own = std::make_unique<Context>(ContextType::FunctionT);
        Context& call_ctx = *call_ctx_own;
        ctx_pool.push_back(std::move(call_ctx_own));
        call_ctx.parent = &fn.context;
        // Р»РµРєСЃ-Р·Р°С…РІР°С‚: С†РµРїРѕС‡РєСѓ РїСЂРѕРґРѕР»Р¶Р°РµРј РЅР° СЃРЅРёРјРѕРє РѕРєСЂСѓР¶РµРЅРёСЏ РґРµРєР»Р°СЂР°С†РёРё (РІР»РѕР¶РµРЅРЅС‹Рµ С„СѓРЅРєС†РёРё
        // РІРёРґСЏС‚ Р»РѕРєР°Р»СЊРЅС‹Рµ РїРµСЂРµРјРµРЅРЅС‹Рµ Рё С„СѓРЅРєС†РёРё РѕР±СЉС‘РјР»СЋС‰РµР№ С„СѓРЅРєС†РёРё).
        fn.context.parent = fn.captured.variables.empty() ? fn.context.parent : &fn.captured;

        // РіРµРЅРµСЂРёРє-РёРјРµРЅР° РґРѕСЃС‚СѓРїРЅС‹ РІРЅСѓС‚СЂРё С‚РµР»Р°: value-РіРµРЅРµСЂР°Р»РёРє -> РєРѕРЅСЃС‚Р°РЅС‚Р° (T = 10),
        // С‚РёРї-РіРµРЅРµСЂР°Р»РёРє -> reified Type (T = i64/Any)
        for (size_t gi = 0; gi < fn.generics.size(); ++gi) {
            String gname = fn.generics[gi].name;
            auto sit = rc.generic_values.find(gname);
            if (sit != rc.generic_values.end()) {
                call_ctx.add_variable(gname, ContextObject(gname, sit->second));
            } else {
                auto it = rc.bindings.find(gname);
                Type bound = (it != rc.bindings.end()) ? it->second : Ty::Any;
                ContextObject co(gname, new_type(bound), Ty::Type_);
                co.is_type_decl = true;   // тип-параметр (T), а не переменная
                call_ctx.add_variable(gname, co);
            }
        }

        // Террические имена структуры тоже должны резолвиться в теле метода
        // (Pair[SecondT, FirstT], ret self.second_): привязываем их к типам
        // конкретного инстанса получателя (FirstT -> str, SecondT -> i64).
        if (struct_method_call && csym) {
            std::vector<Value> sbi;
            if (bindingsForType(*csym, recv_value.T, sbi))
                for (size_t gi = 0; gi < csym->generics.size() && gi < sbi.size(); ++gi) {
                    const GenericParam& sgp = csym->generics[gi];
                    ContextObject co(sgp.name, sbi[gi], Ty::Null);
                    co.is_type_decl = csym->is_type_generic(sgp);
                    call_ctx.add_variable(sgp.name, co);
                }
        }

        // РїР°СЂР°РјРµС‚СЂС‹ (Р·РЅР°С‡РµРЅРёРµ РёР»Рё РґРµС„РѕР»С‚) + wait_type + РјРѕРґРёС„РёРєР°С‚РѕСЂС‹
        // Раскладка rc.arg_for_param приводит фактические аргументы (позиционные и
        // именованные) к позициям параметров выбранного кандидата: слот -> args[].
        const std::vector<int>& afp = rc.arg_for_param;
        for (size_t pi = 0; pi < fn.arguments.size(); ++pi) {
            const FuncArg& pa = fn.arguments[pi];
            Value pv = new_null();
            if (pi < afp.size() && afp[pi] >= 0 && (size_t)afp[pi] < args.size()) {
                pv = args[afp[pi]];
            } else if (pa.default_value) {
                pv = execute(pa.default_value, call_ctx);
            } else {
                pv = new_null();
            }
            Type wt = (pi < rc.param_types.size()) ? rc.param_types[pi] : Ty::Any;
            ContextObject po(pa.name, pv, wt);
            // Параметр с объявленным union-типом (например SecondT = str | char) должен
            // внутри тела вести себя как union, а не сужаться до фактического типа
            // аргумента (char). Иначе дженерики ниже по цепочке (Pair::create, add)
            // утрачивают объявленное сужение и порождают ложные strict-mismatch'и в
            // dry-прогоне мёртвых веток (`self._container[i].second = value`).
            if (!wt.is_auto && !wt.is_any && wt.isUnion() && (wt << pv.T) && !isNumberType(pv.T))
                po.value.T = wt;
            for (size_t mi = 0; mi < pa.modifiers.size(); ++mi) {
                if (pa.modifiers[mi] == "const")    po.modifiers.Const   = true;
                if (pa.modifiers[mi] == "strict")   po.modifiers.Strict  = true;
                if (pa.modifiers[mi] == "final")    po.modifiers.Final   = true;
                if (pa.modifiers[mi] == "static")   po.modifiers.Static  = true;
                if (pa.modifiers[mi] == "private")  po.modifiers.Private = true;
                // ref-параметр: `&l_value` внутри тела должен адресовать ЖИВУЮ ячейку
                // аргумента (как `&var`), а не копию параметра — иначе указатель
                // получается null/dangling. Привязываем addr параметра к адресу
                // переданной переменной из контекста вызова (куча, стабильный адрес).
                // Полная валидация ref-аргумента выполняется ниже, ДО тела.
                if (pa.modifiers[mi] == "ref") {
                    Node* a = (struct_method_call && pi == 0)
                                  ? recv_arg_node
                                  : ((pi < afp.size() && afp[pi] >= 0 && (size_t)afp[pi] < argNodes.size())
                                         ? argNodes[afp[pi]] : nullptr);
                    // ресивер метода: ref self адресует ЖИВУЮ ячейку владельца
                    if (struct_method_call && pi == 0) {
                        if (recv_owner) po.addr = recv_owner->addr;
                        continue;
                    }
                    while (a && a->type == NodeTypes::NodeScope)
                        a = static_cast<struct NodeScope*>(a)->node;
                    if (a && a->type == NodeTypes::NodeVariable) {
                        String an = static_cast<struct NodeVariable*>(a)->name;
                        if (context.has_variable(an)) {
                            po.addr = context.get_variable(an).addr;
                            // модификаторы ОБЪЯВЛЕНИЯ аргумента едут вместе с ref-параметром:
                            // `mod(param, ref)` в теле отражает аргумент (`ref let b` -> true).
                            // Только для обычных функций: у методов ref-аргументы кодогенератор
                            // пока не переносит mods (держим паритет — там всегда false).
                            if (!struct_method_call && context.get_variable(an).modifiers.Ref)
                                po.modifiers.Ref = true;
                        }
                    }
                }
            }
            if (po.modifiers.Strict && !wt.is_auto && !wt.is_any && !(wt << pv.T))
                error_container.addError(DEEP_ERRORS::STRICT_TYPE_MISMATCH(
                    call->open_token, wt.toString(), pv.T.toString()));
            call_ctx.add_variable(pa.name, po);
        }

        size_t errs_before = error_container.errors.size();
        if (method_is_const && struct_method_call && !recv_self_name.empty()
            && call_ctx.has_variable_local(recv_self_name))
            const_receiver = &call_ctx.get_variable_local(recv_self_name);
        // ref-параметры: захват lvalue-путей аргументов ДО исполнения тела.
        // Аргумент обязан быть адресатом (переменная/поле/элемент);
        // rvalue (литерал, результат вызова, арифметика) — REF_RVALUE, а прочие
        // адресуемые формы (*ptr, Ns::x) — REF_ARG_NOT_VARIABLE.
        // Транспилятор эмитит живой алиас теми же путями (refSourceAlias/refSourceCell),
        // поэтому набор разрешённых форм у бэкендов совпадает.
        // Невалидному аргументу отводим невинный скрэтч-слот: `*self._ = ...` в теле
        // не должен порождать вторичный NULL_DEREFERENCE — одной ясной ошибки хватит.
        std::vector<std::pair<String, Node*>> ref_writebacks;
        for (size_t pi = 0; pi < fn.arguments.size(); ++pi) {
            const FuncArg& pa = fn.arguments[pi];
            if (std::find(pa.modifiers.begin(), pa.modifiers.end(), "ref") == pa.modifiers.end())
                continue;
            bool valid = true;
            // __call__-объект с rvalue-ресивером (цепочка f()(…), результат вызова):
            // self не обязан быть lvalue — write-back мутаций временного значения опускается.
            if (struct_method_call && pi == 0 && call_self_temp) continue;
            // ресивер метода (ref self) — аргумент №0: это левая часть '.', а не
            // первый реальный аргумент (call->args его не содержит). Отдельный write-back
            // не нужен: штатное self-write-back выше уже пишет мутации владельцу.
            Node* argNode = (struct_method_call && pi == 0)
                                ? recv_arg_node
                                : ((pi < afp.size() && afp[pi] >= 0 && (size_t)afp[pi] < argNodes.size())
                                       ? argNodes[afp[pi]] : nullptr);
            if (!argNode) {
                error_container.addError(DEEP_ERRORS::REF_NEEDS_ARG(call->open_token, pa.name));
                valid = false;
            } else if (!isAssignablePath(argNode, context)) {
                Token argTok = call->open_token;
                switch (argNode->type) {
                    case NodeTypes::NodeVariable:
                        argTok = static_cast<struct NodeVariable*>(argNode)->token;
                        break;
                    case NodeTypes::NodeUnaryDereference:
                        argTok = static_cast<struct NodeUnaryDereference*>(argNode)->token;
                        break;
                    case NodeTypes::NodeArrayAccess:
                        argTok = static_cast<struct NodeArrayAccess*>(argNode)->token;
                        break;
                    case NodeTypes::NodeMemberAccess:
                        argTok = static_cast<struct NodeMemberAccess*>(argNode)->member_token;
                        break;
                    case NodeTypes::NodeBinaryScopeResolution:
                        argTok = static_cast<struct NodeBinaryScopeResolution*>(argNode)->member_token;
                        break;
                    case NodeTypes::NodeFunctionCall:
                        argTok = static_cast<struct NodeFunctionCall*>(argNode)->open_token;
                        break;
                    default:
                        break;
                }
                error_container.addError(DEEP_ERRORS::REF_RVALUE(call->open_token, argTok, pa.name));
                valid = false;
            } else {
                Node* uv = argNode;
                while (uv && uv->type == NodeTypes::NodeScope)
                    uv = static_cast<struct NodeScope*>(uv)->node;
                bool supported = uv && (uv->type == NodeTypes::NodeVariable
                                        || uv->type == NodeTypes::NodeMemberAccess
                                        || uv->type == NodeTypes::NodeArrayAccess);
                if (!supported) {
                    error_container.addError(DEEP_ERRORS::REF_ARG_NOT_VARIABLE(
                        call->name_token, fn.name, pa.name));
                    valid = false;
                }
            }
            if (!valid) {
                if (call_ctx.has_variable_local(pa.name)) {
                    ContextObject& cell = call_ctx.get_variable_local(pa.name);
                    cell.addr = heap.allocate_high(&cell);
                }
                continue;
            }
            if (!(struct_method_call && pi == 0))
                ref_writebacks.emplace_back(pa.name, argNode);
        }

        // пока исполняется тело, запоминаем точку вызова: assert внутри функции
        // сообщает о нарушенном контракте на месте вызова, а не в теле
        Token* saved_call_site = current_call_site;
        Token* saved_call_site_end = current_call_site_end;
        current_call_site = (call->name_token.type != TypeOfToken::DUMMY)
                                ? &call->name_token : &call->open_token;
        current_call_site_end = &call->close_token;
        // скоуп этой функции: локальные ячейки (`let` в теле) будут помечены им,
        // чтобы указатели на них не выносились за возврат (P2)
        int saved_fn_scope = current_fn_scope;
        current_fn_scope = ++fn_scope_seq;
        // значение тела: последнее выражение скоупа считается неявным return
        // (как у lambda). Ранний «ret» по-прежнему даёт return_value;
        // для ref-функций неявный return не применяем — write-back обязан
        // идти через явный `ret <lvalue>`.
        Value body_result = new_null();
        if (fn.body) body_result = execute(fn.body, call_ctx);
        // extern fn без runtime-тела (`{ <expr> }`) в интерпретаторе возвращает null
        (void)interpret_mode;
        current_fn_scope = saved_fn_scope;
        current_call_site = saved_call_site;
        current_call_site_end = saved_call_site_end;

        // write-back self (метод структуры): мутации, сделанные в теле через self,
        // сохраняем обратно в контекст-объект владельца (переменная / ячейка кучи).
        // Для вложенных путей (`self._container.add(...)`), где владелец — не
        // переменная, а цепочка MemberAccess/ArrayAccess, пишем значение обратно
        // по пути (writePathValue), чтобы мутация «прилипла» в поле структуры.
        if (struct_method_call && !recv_self_name.empty() && !call_self_temp
            && call_ctx.has_variable_local(recv_self_name)) {
            Value mutated = call_ctx.get_variable_local(recv_self_name).value;
            if (recv_owner) {
                recv_owner->value = mutated;
            } else if (recv_member_node) {
                writePathValue(recv_member_node, context, mutated, call->close_token);
            }
        }

        // write-back ref-параметров: мутировавшие локальные копии пишем обратно
        // в lvalue-пути аргументов (вариабельный `p.x = ...` в теле «прилипает»
        // вызывающему, как при передаче по ссылке).
        for (auto& rb : ref_writebacks) {
            Node* targ = rb.second;
            if (!call_ctx.has_variable_local(rb.first)) continue;
            Value mutated = call_ctx.get_variable_local(rb.first).value;
            if (targ) writePathValue(targ, context, mutated, call->close_token);
        }

        // РЎС‚РµРє РІС‹Р·РѕРІР°: РµСЃР»Рё РїСЂРё РёСЃРїРѕР»РЅРµРЅРёРё С‚РµР»Р° РїРѕСЏРІРёР»РёСЃСЊ РѕС€РёР±РєРё вЂ” РїСЂРёРєР»РµРёРІР°РµРј
        // Рє СЃР°РјРѕР№ РіР»СѓР±РѕРєРѕР№ РѕС€РёР±РєРµ (РєРѕРЅРµС† С†РµРїРѕС‡РєРё sub_error) trace РјРµСЃС‚Р° РІС‹Р·РѕРІР°:
        // РґРµР»РµРЅРёРµ -> div(a, b) -> call(div, 10, 0). Р РµРєСѓСЂСЃРёРІРЅРѕ РЅР°СЂР°СЃС‚Р°РµС‚ РЅР°СЂСѓР¶Сѓ.
        if (error_container.errors.size() > errs_before
            && (!error_container.errors.back().is_warning
                || error_container.errors.back().chain_call_site)) {
            BaseError* node = &error_container.errors.back();
            while (node->sub_error) node = node->sub_error.get();
            // Переход через синтетический вызов dunder'а указывает на тот же
            // оператор, что и сам диагноз (`a / b` вызывает __truediv__, а span
            // __truediv__ — это span оператора). Такой «шаг» ничего не добавляет,
            // поэтому пропускаем его и сразу идём к следующему вызову по цепочке.
            bool same_span = node->line == callStart.position.line
                          && node->column == callStart.position.index
                          && node->end_line == call->close_token.position.line;
            if (!same_span) {
                node->withSubError(error_container.errors.back().is_warning
                                       ? String("assert reached from call to '") + fn.name + String("'")
                                       : String("call error in function '") + fn.name + String("'"),
                                   callStart, call->close_token);
            }
        }

        bool fn_exiting = (control_flow == ControlFlow::Exit_);
        Value result = (control_flow == ControlFlow::Return_) ? return_value
                                                              : (control_flow == ControlFlow::Exit_ ? new_null()
                                                              : (current_returns_ref ? new_null() : body_result));

        // exit — глобальный поток: он не поглощается границей функции, а
        // продолжает всплывать до верхнего уровня (по нему останавливается весь прогон)
        if (!fn_exiting) control_flow = saved_flow;
        return_value = saved_ret;
        loop_depth = saved_loop;
        iter_budget = saved_budget;
        budget_exhausted = saved_budget_exhausted;
        supp_ret_mismatch = saved_supp_ret;
        current_ret_type = saved_ret_type;
        current_returns_ref = saved_cur_ret_ref;
        current_ref_params = saved_ref_params;
        current_write_active = saved_write_active;
        ref_write_armed = saved_ref_write_armed;
        ref_write_val = saved_ref_write_val;
        ref_write_rcall = saved_ref_write_rcall;
        active_struct = saved_active;
        const_receiver = saved_const_recv;
        --call_depth;
        return result;
    }

void operator()() {
#ifdef _WIN32
        // Р РµРєСѓСЂСЃРёРІРЅС‹Р№ РІС‹Р·РѕРІ С„СѓРЅРєС†РёР№ Р·Р°РЅРёРјР°РµС‚ ~100KB СЃС‚РµРєР° РЅР° СѓСЂРѕРІРµРЅСЊ вЂ” РЅР° РґРµС„РѕР»С‚РЅС‹С…
        // 1MB СЃС‚РµРєР° РїСЂРѕС†РµСЃСЃР° РїСЂРѕС†РµСЃСЃ РїР°РґР°Р» (0xC00000FD) РЅР° РіР»СѓР±РёРЅРµ ~10, СЂР°РЅСЊС€Рµ С‡РµРј
        // СЃСЂР°Р±Р°С‚С‹РІР°Р» guard CALL_DEPTH_LIMIT. Р‘РѕР»СЊС€РѕР№ СЃС‚РµРє СѓР±РёСЂР°РµС‚ Р·Р°РІРёСЃРёРјРѕСЃС‚СЊ РѕС‚
        // РЅР°СЃС‚СЂРѕР№РєРё СЃС‚РµРєР° С…РѕСЃС‚-РїСЂРёР»РѕР¶РµРЅРёСЏ (РєРѕРЅСЃРѕР»СЊ/LSP).
        auto threadProc = [](LPVOID param) -> DWORD {
            auto* self = static_cast<ASTDeepAnalyzer*>(param);
            self->prof_enabled = std::getenv("LUMEN_PROF") != nullptr;
            self->runAnalysis();
            if (self->prof_enabled) {
                std::fprintf(stderr, "[prof] execute calls = %lld, dry-context copies = %lld, pure fast-path = %lld\n",
                             self->prof_exec_count, self->prof_copy_count, self->prof_pure_count);
                char buf[1024];
                std::snprintf(buf, sizeof(buf), "[prof] top node types:");
                std::fprintf(stderr, "%s\n", buf);
                for (int t = 0; t < 128; ++t)
                    if (self->prof_node_count[t] >= 1000)
                        std::fprintf(stderr, "  %3d: %9lld calls, %8.1f ms, %6.1f us/call\n", t,
                                     (long long)self->prof_node_count[t],
                                     (double)self->prof_node_time[t] / 1e6,
                                     (double)self->prof_node_time[t] / 1e3 / (double)self->prof_node_count[t]);
            }
            return 0;
        };
        DWORD tid = 0;
        HANDLE h = CreateThread(nullptr, 64u * 1024 * 1024, threadProc, this, 0, &tid);
        if (h) {
            WaitForSingleObject(h, INFINITE);
            CloseHandle(h);
        } else {
            runAnalysis();
        }
#else
        runAnalysis();
#endif
    }

    // M8: единая точка входа анализа (вызывается из operator() в отдельном
    // потоке с большим стеком). Обычный режим — исполнить весь файл; eval_only —
    // зарегистрировать объявления и исполнить только собранные eval-блоки.
    void runAnalysis() {
        initContext();
        if (eval_only) { runEvalOnly(); return; }
        for (auto& node : ast) {
            if (aborted) break;
            execute(node, global_context);
        }
    }

    void runEvalOnly() {
        // 1) Регистрируем объявления (функции/структуры/enum/namespace), чтобы
        //    тела eval могли вызывать пользовательские сущности. Прочий код
        //    (в т.ч. циклы вне eval) не исполняем — это и есть «только eval-блоки».
        for (auto* node : ast) {
            if (aborted) break;
            Node* u = node;
            while (u && u->type == NodeTypes::NodeModifier)
                u = static_cast<struct NodeModifier*>(u)->node;
            if (!u) continue;
            if (u->type == NodeTypes::NodeFunctionDeclaration ||
                u->type == NodeTypes::NodeStructDeclaration ||
                u->type == NodeTypes::NodeEnumDeclaration ||
                u->type == NodeTypes::NodeNamespace) {
                execute(u, global_context);
            }
        }
        // 2) Исполняем eval-блоки по очереди в общем глобальном контексте
        //    (let из блока №1 виден блоку №2 и последующему коду).
        for (auto* blk : eval_blocks) {
            if (aborted) break;
            execute(blk, global_context);
        }
    }

};

