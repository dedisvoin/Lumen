#include "map"
#include "unordered_map"
#include "functional"
#include "algorithm"
#include "set"

#include "../runtime/Values.cpp"
#include "../ASTGenerator.cpp"

#include "../runtime/Context.cpp"
#include "../utils/Strings.cpp"
#include <vector>


#pragma once


typedef std::vector<FuncArg> FunctionArguments;
typedef std::vector<GenericParam> GenericArguments;
typedef std::map<String, Type> GenericBindings;

struct Function {
    String name;
    Context context;
    Context captured;   // копия контекста захвата на момент декларации (лекс-замыкание)
    FunctionArguments arguments;
    GenericArguments generics;
   Node* body;
   Node* return_type;
   // Узел объявления (NodeFunctionDeclaration), которому соответствует эта Function.
   // Мост «символ анализатора -> узел AST»: кодоген по нему находит FnSig и берёт
   // перегрузку, выбранную deep-анализатором, вместо собственного подбора по тегам.
   Node* decl = nullptr;
   
    Function(String name, FunctionArguments arguments, GenericArguments generics, Node* body, Node* return_type) :
        name(name), arguments(arguments), generics(generics), body(body), return_type(return_type) {}

    // Reified типы для typeof(имя): реальные типы параметров и возврата.
    // Для генерик-имён — Type(BASE, имя) (печатается как "T").
    std::vector<Type> reified_params;
    Type reified_ret = Ty::Any;

    // Разрешённые типы ограничений generic-параметров (`A: Type` -> Type_,
    // `N: i64` -> i64; без ограничения -> Type_). Используются в callableType():
    // слоты `typeof(имя)` говорят, тип это или значение, и шаблон
    // `Func[Type, ...]` пропускает только тип-дженерики.
    std::vector<Type> generic_constraints;

    // Типы зафиксированы при декларации (лекс-захват генерик-имени объёмлющей функции):
    // вызовы матчатся строго по reified-типам, без повторного вывода генериков.
    bool locked_types = false;

    // const-метод структуры ('const fn ...'): тело видит ресивер только на чтение.
    bool is_const = false;

    // static-метод структуры ('static fn ...'): вызывается через '::' без ресивера.
    bool is_static = false;

    // функция объявлена как 'fn ... (->) ref T' — возвращает живую ссылку.
    bool returns_ref = false;

    // документирующий комментарий /* ... */ над объявлением функции.
    String doc_comment;

    bool is_generic() const { return !generics.empty(); }

    Token decl_token;   // токен '[' генерик-списка декларации (для диагностик резолва)

    // минимальное число аргументов (обязательные параметры без дефолт-значения)
    size_t min_params() const {
        size_t n = 0;
        for (size_t i = 0; i < arguments.size(); ++i)
            if (!arguments[i].default_value) n++;
        return n;
    }
};

// ============================ сигнатуры ==================================

// Канонический «ключ» типа-ноды для сравнения сигнатур при декларации.
// 'auto' и отсутствие типа считаем эквивалентными (обе — динамическая позиция).
static String typeNodeKey(Node* t) {
    if (!t) return String("auto");
    switch (t->type) {
        case NodeTypes::NodeVariable: {
            String nm = static_cast<struct NodeVariable*>(t)->name;
            if (nm == "auto" || nm == "any") nm = String("auto");
            return String("var:") + nm;
        }
        case NodeTypes::NodeUnaryDereference: {
            String s("*");
            s += typeNodeKey(static_cast<struct NodeUnaryDereference*>(t)->node);
            return s;
        }
        case NodeTypes::NodeBinaryBitwiseOr:
        case NodeTypes::NodeBinaryLogicalOr: {
            auto* bn = static_cast<struct NodeBinaryBitwiseOr*>(t);
            String s;
            s += typeNodeKey(bn->l_node);
            s += "|";
            s += typeNodeKey(bn->r_node);
            return s;
        }
        default:
            return String("type");
    }
}

// Строка-сигнатура "тип1, тип2" для диагностик (по AST, без содержимого контекста).
static String signatureKey(const Function& f) {
    String s;
    for (size_t i = 0; i < f.arguments.size(); ++i) {
        s += typeNodeKey(f.arguments[i].type);
        if (i + 1 < f.arguments.size()) s += ", ";
    }
    return s;
}

// ============================ помощники типов ============================

typedef std::function<Value(Node*, Context&)> NodeEvaluator;

static bool isGenericName(const String& name, const Function& f) {
    for (size_t i = 0; i < f.generics.size(); ++i)
        if (f.generics[i].name == name) return true;
    return false;
}

// value-генералик: констрейнт — конкретный тип ('[T: i64]'), а не ': Type'.
static bool isValueGeneric(const GenericParam& gp);

static bool nodeUsesGeneric(Node* n, const Function& f) {
    if (!n) return false;
    switch (n->type) {
        case NodeTypes::NodeVariable:
            return isGenericName(static_cast<struct NodeVariable*>(n)->name, f);
        case NodeTypes::NodeUnaryDereference:
            return nodeUsesGeneric(static_cast<struct NodeUnaryDereference*>(n)->node, f);
        case NodeTypes::NodeBinaryBitwiseOr:
        case NodeTypes::NodeBinaryLogicalOr: {
            auto* bn = static_cast<struct NodeBinaryBitwiseOr*>(n);
            return nodeUsesGeneric(bn->l_node, f) || nodeUsesGeneric(bn->r_node, f);
        }
        case NodeTypes::NodeArrayAccess: {
            auto* aa = static_cast<struct NodeArrayAccess*>(n);
            if (nodeUsesGeneric(aa->base, f)) return true;
            for (auto* ix : aa->indices)
                if (nodeUsesGeneric(ix, f)) return true;
            return false;
        }
        default:
            return false;
    }
}

// ============================ типы параметров ============================

// Размер `Array[T, N]` — это ЗНАЧЕНИЕ, а не тип: литерал, либо value-генералик
// функции (`fn vec[size: i64]() -> Array[any, size]`). Возвращает false, если
// число вывести нельзя (генералик без привязки на декларации) — тогда размер
// остаётся динамическим (-1), ошибку не выдаём.
static bool arrayCountFromNode(Node* cn, const Function& f, const GenericBindings& b,
                               const std::map<String, Value>* gvals,
                               const NodeEvaluator& eval, Context& ctx, long long& out) {
    if (!cn) { out = -1; return true; }
    if (cn->type == NodeTypes::NodeNumber) {
        auto* num = static_cast<struct NodeNumber*>(cn);
        if (!num->is_integer) return false;
        out = num->int_value;
        return true;
    }
    if (cn->type == NodeTypes::NodeVariable) {
        String nm = static_cast<struct NodeVariable*>(cn)->name;
        // Сначала значение из привязок вызова (value-генералик), и только потом
        // контекст: в теле функции `size` — обычная переменная.
        if (gvals) {
            auto gv = gvals->find(nm);
            if (gv != gvals->end()) {
                if (!isNumberType(gv->second.T)) return false;
                out = static_cast<long long>(getNumberFromType<>(gv->second));
                return true;
            }
        }
        // Генералик без значения (декларация/typeof) — конкретный размер
        // неизвестен, но это не ошибка.
        if (isGenericName(nm, f)) return false;
        if (b.count(nm) && !ctx.has_variable(nm)) return false;
    }
    Value v = eval(cn, ctx);
    if (!isNumberType(v.T)) return false;
    out = static_cast<long long>(getNumberFromType<>(v));
    return true;
}

// Вычисляет reified-тип выражения типа (Node*) с учётом привязок генерик-имён.
// Если 'argType' задан и параметр — прямое генерик-имя ещё без привязки — выводим
// из типа аргумента (для `x: B` из 'Vec2[i64]' в слоте инстанса — из вложенного
// типа слота). 'ok' при неконвертируемом узле становится false (кандидат неприменим).
static Type evalTypeNode(Node* n, const Function& f, GenericBindings& b,
                         const NodeEvaluator& eval, Context& ctx,
                         const Type* argType = nullptr, bool* ok = nullptr,
                         const std::map<String, Value>* gvals = nullptr) {
    if (!n) return Ty::Auto;

    switch (n->type) {
        // `Array[T, N]` в позиции типа. Без этой ветки узел уходил в default/eval
        // и generic-размер давал error[73] «Undefined variable 'size'».
        case NodeTypes::NodeArrayType: {
            auto* at = static_cast<struct NodeArrayType*>(n);
            Type elem = evalTypeNode(at->element_type, f, b, eval, ctx, nullptr, ok, gvals);
            if (ok && !*ok) return Ty::Any;
            long long count = -1;
            arrayCountFromNode(at->count, f, b, gvals, eval, ctx, count);
            return Type::ArrayType(elem, static_cast<int>(count));
        }
        case NodeTypes::NodeVariable: {
            String name = static_cast<struct NodeVariable*>(n)->name;
            auto it = b.find(name);
            if (it != b.end()) return it->second;
            if (isGenericName(name, f)) {
                if (argType) { b.emplace(name, *argType); return *argType; }
                return Ty::Any;   // генерик без вывода — ловит всё
            }
            if (name == "auto") return Ty::Auto;
            if (name == "any")  return Ty::Any;
            if (!ctx.has_variable(name)) return Ty::Any;   // неизвестное имя типа
            Value v = eval(n, ctx);
            if (v.T == Ty::Type_) return v.get<Type>();
            if (ok) *ok = false;
            return Ty::Any;
        }
        case NodeTypes::NodeUnaryDereference: {
            Type inner = evalTypeNode(static_cast<struct NodeUnaryDereference*>(n)->node,
                                      f, b, eval, ctx, nullptr, ok, gvals);
            return *(inner);
        }
        // `?T` в сигнатуре разбирается как NodeUnaryNegation с токеном "?" (парсер
        // ASTGenerator:573, эмиттер зовёт rt::typnullable). Ветки на этот узел не
        // было, поэтому он уходил в eval() как в ВЫРАЖЕНИЕ, где generic-имя T не
        // является значением: `-> ?T` давал error[73] «Undefined variable 'T'»,
        // а `-> ?i64` — error[79] (операнд не Type_). Тип-узел нельзя оценивать
        // как значение, поэтому nullable-разбираем здесь, в мире типов.
        case NodeTypes::NodeUnaryNegation: {
            auto* un = static_cast<struct NodeUnaryNegation*>(n);
            if (un->token.value == "?") {
                Type inner = evalTypeNode(un->node, f, b, eval, ctx, nullptr, ok, gvals);
                return inner | Ty::Null;
            }
            if (ok) *ok = false;
            return Ty::Any;
        }
        case NodeTypes::NodeBinaryBitwiseOr:
        case NodeTypes::NodeBinaryLogicalOr: {
            auto* bn = static_cast<struct NodeBinaryBitwiseOr*>(n);
            Type l = evalTypeNode(bn->l_node, f, b, eval, ctx, nullptr, ok, gvals);
            Type r = evalTypeNode(bn->r_node, f, b, eval, ctx, nullptr, ok, gvals);
            return l | r;
        }
        case NodeTypes::NodeArrayAccess: {
            // Структурный инстанс `Vec2[B]` в сигнатуре: генерик-имя функции (B)
            // не привязано в контексте вызова — naive-eval даёт «Undefined variable 'B'».
            // Собираем StructType по слотам: вывод из типа аргумента (Vec2[i64] -> B = i64),
            // из уже привязанных bindings, либо символически Type(BASE, имя) для
            // генерик-имени без значения (декларация). value-слоты — из arg_mask аргумента.
            auto* acc = static_cast<struct NodeArrayAccess*>(n);
            if (acc->base && (ok != nullptr || !b.empty() || nodeUsesGeneric(acc, f))) {
                Type baseTy = evalTypeNode(acc->base, f, b, eval, ctx, nullptr, ok, gvals);
                if (ok && !*ok) return Ty::Any;
                if (baseTy.isStruct()) {
                    const Type* inner = (argType && argType->isStruct() && argType->name == baseTy.name)
                        ? argType : nullptr;
                    std::vector<Type> targs;
                    std::vector<long long> vargs;
                    uint32_t mask = 0;
                    size_t ti = 0, vi = 0;
                    bool slot_ok = true;
                    for (size_t i = 0; i < acc->indices.size() && slot_ok; ++i) {
                        Node* idx = acc->indices[i];
                        if (inner && !(inner->arg_mask & (1u << static_cast<unsigned>(i)))) {
                            // value-слот: берём значение из аргумента инстанса
                            if (vi < inner->int_args.size()) { vargs.push_back(inner->int_args[vi++]); continue; }
                            slot_ok = false;
                            break;
                        }
                        const Type* slot = nullptr;
                        if (inner) {
                            if (ti >= inner->container_1.size()) { slot_ok = false; break; }
                            slot = &inner->container_1[ti++];
                        }
                        size_t gix = f.generics.size();
                        if (idx->type == NodeTypes::NodeVariable) {
                            String iname = static_cast<struct NodeVariable*>(idx)->name;
                            for (size_t g = 0; g < f.generics.size(); ++g)
                                if (f.generics[g].name == iname) { gix = g; break; }
                            if (gix != f.generics.size()) {
                                auto it = b.find(iname);
                                if (it != b.end()) { targs.push_back(it->second); mask |= (1u << static_cast<unsigned>(i)); continue; }
                                if (slot) { b.emplace(iname, *slot); targs.push_back(*slot); mask |= (1u << static_cast<unsigned>(i)); continue; }
                                if (!isValueGeneric(f.generics[gix]))
                                    { targs.push_back(Type(TypeType::BASE, iname)); mask |= (1u << static_cast<unsigned>(i)); continue; }
                            }
                        }
                        Type st = evalTypeNode(idx, f, b, eval, ctx, slot, ok ? &slot_ok : nullptr, gvals);
                        if (!slot_ok) break;
                        targs.push_back(st);
                        mask |= (1u << static_cast<unsigned>(i));
                    }
                    if (slot_ok)
                        return Type::StructType(baseTy.name, targs, vargs, mask);
                }
            }
            // не смогли вывести структурно — прежний путь реификации (eval узла)
            Value v = eval(n, ctx);
            if (v.T == Ty::Type_) return v.get<Type>();
            if (ok) *ok = false;
            return Ty::Any;
        }
        default: {
            Value v = eval(n, ctx);
            if (v.T == Ty::Type_) return v.get<Type>();
            if (ok) *ok = false;
            return Ty::Any;
        }
    }
}

static Type evalFunctionParamType(const FuncArg& pa, const Function& f, GenericBindings& b,
                                  const NodeEvaluator& eval, Context& ctx,
                                  const Value* arg = nullptr, bool* ok = nullptr,
                                  const std::map<String, Value>* gvals = nullptr) {
    return evalTypeNode(pa.type, f, b, eval, ctx, arg ? &arg->T : nullptr, ok, gvals);
}

static int specRankFor(const FuncArg& pa, const Function& f) {
    if (f.locked_types) return 0;
    if (!pa.type) return 2;
    if (pa.type->type == NodeTypes::NodeVariable) {
        String nm = static_cast<struct NodeVariable*>(pa.type)->name;
        if (isGenericName(nm, f)) return 1;
        if (nm == "auto" || nm == "any") return 2;
        return 0;
    }
    if (nodeUsesGeneric(pa.type, f)) return 1;
    return 0;
}

// Reified-тип для typeof(имя): генерик-имена подставляются как Type(BASE, имя)
// (печатается "T"/"A | B"), union'ы раскрываются рекурсивно, конкретика — через evalTypeNode.
static Type declTypeNode(Node* n, const Function& f, GenericBindings& b,
                         const NodeEvaluator& eval, Context& ctx,
                         const std::map<String, Value>* gvals = nullptr) {
    if (!n) return Ty::Auto;
    if (n->type == NodeTypes::NodeVariable) {
        String name = static_cast<struct NodeVariable*>(n)->name;
        if (isGenericName(name, f)) return Type(TypeType::BASE, name);
        return evalTypeNode(n, f, b, eval, ctx, nullptr, nullptr, gvals);
    }
    if (n->type == NodeTypes::NodeUnaryDereference)
        return *(declTypeNode(static_cast<struct NodeUnaryDereference*>(n)->node, f, b, eval, ctx, gvals));
    // `?T` разбирается как NodeUnaryNegation с токеном "?" (см. CodeGenerator:
    // rt::typnullable). Без этой ветки он уходил в evalTypeNode -> eval, где
    // generic-имя T не является значением, и `-> ?T` давал "Undefined variable
    // 'T'". Nullable-тип в позиции generic-имени = T | Null.
    if (n->type == NodeTypes::NodeUnaryNegation) {
        auto* un = static_cast<struct NodeUnaryNegation*>(n);
        if (un->token.value == "?")
            return declTypeNode(un->node, f, b, eval, ctx, gvals) | Ty::Null;
    }
    if (n->type == NodeTypes::NodeBinaryBitwiseOr || n->type == NodeTypes::NodeBinaryLogicalOr) {
        auto* bn = static_cast<struct NodeBinaryBitwiseOr*>(n);
        return declTypeNode(bn->l_node, f, b, eval, ctx, gvals) | declTypeNode(bn->r_node, f, b, eval, ctx, gvals);
    }
    return evalTypeNode(n, f, b, eval, ctx, nullptr, nullptr, gvals);
}

// ============================ FunctionContainer ==========================

struct FunctionContainer {
    String name;
    vector<Function> functions;

    FunctionContainer(String name) : name(name) {}

    void add_function(const Function& function) {
        functions.push_back(function);
    }

    bool is_overloaded() const { return functions.size() > 1; }

    bool has_duplicate_signature(const Function& candidate) const {
        String key = signatureKey(candidate);
        for (size_t i = 0; i < functions.size(); ++i)
            if (functions[i].arguments.size() == candidate.arguments.size()
                && signatureKey(functions[i]) == key) return true;
        return false;
    }

    // Прототипный тип набора для typeof(имя):
    //   одна функция  -> FunctionType(params, ret, generics)
    //   перегрузки    -> union сигнатур
    Type callableType() const {
        if (functions.empty()) return Type::FunctionType(std::vector<Type>(), Ty::Null);
        bool first = true;
        Type set = Ty::Null;
        for (size_t k = 0; k < functions.size(); ++k) {
            const Function& f = functions[k];
            std::vector<Type> ps;
            for (size_t i = 0; i < f.arguments.size(); ++i)
                ps.push_back(f.reified_params.size() > i ? f.reified_params[i] : Ty::Any);
            std::vector<Type> gs;
            for (size_t i = 0; i < f.generics.size(); ++i)
                gs.push_back(f.generic_constraints.size() > i
                    ? f.generic_constraints[i] : Ty::Type_);
            Type ft = Type::FunctionType(ps, f.reified_ret.isAny() ? Ty::Any : f.reified_ret, gs);
            set = first ? ft : (set | ft);
            first = false;
        }
        return set;
    }
};

// ============================ методы Context =============================

// Копия namespace: перепривязываем контексты функций/вложенных неймспейсов
// внутри копии с исходного (source) контекста на объект копии (dst).
void Namespace::rebind(Context* src, Context* dst) {
    for (auto& kv : dst->variables) {
        if (!kv.second.value.T.isCallable()) continue;
        FunctionContainer& fc = kv.second.value.get_ref<FunctionContainer>();
        for (auto& f : fc.functions)
            if (f.context.parent == src)
                f.context.parent = dst;
    }
    for (auto& kv : dst->variables) {
        if (kv.second.value.T != Ty::Namespace) continue;
        Namespace& child = kv.second.value.get_ref<Namespace>();
        if (child.context.parent == src)
            child.context.parent = dst;
        auto sit = src->variables.find(kv.first);
        if (sit == src->variables.end() || sit->second.value.T != Ty::Namespace) continue;
        rebind(&sit->second.value.get_ref<Namespace>().context, &child.context);
    }
}

Namespace::Namespace(const Namespace& other) : context(other.context) {
    rebind(const_cast<Context*>(&other.context), &context);
}

Namespace& Namespace::operator=(const Namespace& other) {
    if (this != &other) {
        context = other.context;
        rebind(const_cast<Context*>(&other.context), &context);
    }
    return *this;
}

void Context::add_function_container(CtxObjectIdentifier identifier, FunctionContainer container) {
    ContextObject obj(identifier, Value(container.callableType(), std::move(container)));
    variables.insert_or_assign(identifier, std::move(obj));
}

bool Context::has_function_container_local(CtxObjectIdentifier identifier) const {
    auto it = variables.find(identifier);
    return it != variables.end() && it->second.value.T.isCallable();
}

FunctionContainer& Context::get_function_container_local(CtxObjectIdentifier identifier) {
    auto it = variables.find(identifier);
    if (it == variables.end() || !it->second.value.T.isCallable())
        throw std::runtime_error("Function not found (local): " + identifier.to_utf8());
    return it->second.value.get_ref<FunctionContainer>();
}

bool Context::has_function_container(CtxObjectIdentifier identifier) const {
    if (has_function_container_local(identifier)) return true;
    if (parent) return parent->has_function_container(identifier);
    return false;
}

FunctionContainer& Context::get_function_container(CtxObjectIdentifier identifier) {
    if (has_function_container_local(identifier)) return get_function_container_local(identifier);
    if (parent) return parent->get_function_container(identifier);
    throw std::runtime_error("Function not found: " + identifier.to_utf8());
}

// ======================== резолюция перегрузок ===========================

struct ResolvedCall {
    Function* function = nullptr;
    GenericBindings bindings;           // имя генерик-параметра -> reified Type
    std::map<String, Value> generic_values; // имя -> значение (для value-генераликов: T = 10)
    std::vector<Type> param_types;      // вычисленные типы параметров (для привязки)
    Type return_type = Ty::Null;        // тип результата (с учётом привязок)
    // Раскладка фактических аргументов по параметрам выбранного кандидата:
    // параметр j -> индекс в векторе args/meta (-1 = слот заполняется дефолтом).
    std::vector<int> arg_for_param;
};

static String argsTypeString(const std::vector<Value>& args) {
    String s;
    for (size_t i = 0; i < args.size(); ++i) {
        s += args[i].T.toString();
        if (i + 1 < args.size()) s += ", ";
    }
    return s;
}

// value-генералик: констрейнт — конкретный тип ('[T: i64]'), а не ': Type'.
// Такие генерики не выводятся из аргументов — только явный аргумент вызова
// либо default-значение на декларации.
static bool isValueGeneric(const GenericParam& gp) {
    if (!gp.type) return false;
    if (gp.type->type != NodeTypes::NodeVariable) return false;
    return static_cast<struct NodeVariable*>(gp.type)->name != "Type";
}

// Подбирает перегрузку под фактический вызов (значения аргументов).
// Возвращает true при успешном выборе единственной; иначе добавляет ошибку и false.
static bool resolve_function_call(ErrorContainer& errs,
                                  FunctionContainer& container,
                                  const Token& tok_start, const Token& tok_end,
                                  const std::vector<Value>& args,
                                  const std::vector<CallArgMeta>& arg_meta,
                                  const std::vector<Value>& generic_args,
                                  Context& caller,
                                  const NodeEvaluator& eval,
                                  ResolvedCall& out,
                                  bool compute_return = true) {
    if (container.functions.empty()) return false;

    // Именованные аргументы — ошибки на стороне вызова проверяем один раз
    // (дубликат и «имени нет ни в одной перегрузке» не зависят от кандидата).
    bool has_named = false;
    for (const CallArgMeta& m : arg_meta)
        if (m.named) { has_named = true; break; }
    if (has_named) {
        std::set<String> allParamNames;
        for (const Function& f : container.functions)
            for (const FuncArg& a : f.arguments) allParamNames.insert(a.name);
        std::set<String> namedSeen;
        for (const CallArgMeta& m : arg_meta) {
            if (!m.named) continue;
            Token anchor = m.name_token.type == TypeOfToken::DUMMY ? tok_start : m.name_token;
            if (!allParamNames.count(m.name)) {
                errs.addError(DEEP_ERRORS::NAMED_ARG_UNKNOWN(anchor, container.name, m.name));
                return false;
            }
            if (!namedSeen.insert(m.name).second) {
                errs.addError(DEEP_ERRORS::NAMED_ARG_DUPLICATE(anchor, container.name, m.name));
                return false;
            }
        }
    }

    struct Cand {
        Function* f;
        GenericBindings b;
        std::map<String, Value> gvals;  // value-генералики: имя -> константа
        std::vector<int> m;   // ранг совпадения на позицию: 0 exact, 1 widen/sub, 2 auto
        std::vector<int> s;   // ранг специфичности:           0 concrete, 1 generic, 2 auto
        std::vector<int> afp; // параметр j -> индекс аргумента (-1 = дефолт)
        int topParam = 0;     // максимальный предоставленный слот + 1 (для сравнения)
        Cand(Function* f_) : f(f_) {}
    };
    std::vector<Cand> good;
    bool reported_generic_err = false;
    std::set<String> reported_missing_generics;

    for (size_t fi = 0; fi < container.functions.size(); ++fi) {
        Function& f = container.functions[fi];
        size_t np = f.arguments.size();
        std::vector<String> pnames;
        pnames.reserve(np);
        for (const FuncArg& a : f.arguments) pnames.push_back(a.name);
#ifdef LUMEN_DEBUG
        fprintf(stderr, "[RSO-SKIP] name='%s' fi=%zu candArgs=%zu hasArgs=%zu np=%zu minp=%zu\n",
            container.name.to_utf8().c_str(), fi, args.size(), args.size(), np, f.min_params());
#endif
        // Раскладка фактических аргументов (позиционные + именованные) по
        // параметрам кандидата. Позиционные аргументы заполняют свободные слоты
        // по порядку — ровно так работает прежняя позиционная логика.
        std::vector<int> argForParam;
        if (!placeCallArgs(pnames, arg_meta, argForParam)) continue;
        // Обязательный параметр без аргумента и без дефолта — кандидат не годится
        // (заменяет прежнюю проверку min_params/max arity).
        bool missingOk = true;
        for (size_t j = 0; j < np && missingOk; ++j)
            if (argForParam[j] < 0 && !f.arguments[j].default_value) missingOk = false;
        if (!missingOk) continue;

        GenericBindings b;
        std::map<String, Value> gvals;
        bool gbound_ok = true;
        for (size_t gi = 0; gi < f.generics.size(); ++gi) {
            const GenericParam& gp = f.generics[gi];
            // ограничение ': Type' — генерик-аргументом обязан быть тип
            bool expects_type = gp.type
                && gp.type->type == NodeTypes::NodeVariable
                && static_cast<struct NodeVariable*>(gp.type)->name == "Type";
            bool has_explicit = gi < generic_args.size() && generic_args[gi].T != Ty::Null;
            if (has_explicit) {
                Value gv = generic_args[gi];
                if (expects_type && gv.T != Ty::Type_) {
                    if (!reported_generic_err) {
                        errs.addError(DEEP_ERRORS::EXPECTED_TYPE_EXPRESSION(tok_start, gv.T.toString()));
                        reported_generic_err = true;
                    }
                    gbound_ok = false;
                    break;
                }
                if (gv.T == Ty::Type_) {
                    b.emplace(gp.name, gv.get<Type>());
                } else {
                    gvals.emplace(gp.name, gv);
                    b.emplace(gp.name, gv.T);
                }
            } else if (gp.default_value) {
                Value gv = eval(gp.default_value, caller);
                if (gv.T == Ty::Type_) {
                    b.emplace(gp.name, gv.get<Type>());
                } else {
                    gvals.emplace(gp.name, gv);
                    b.emplace(gp.name, gv.T);
                }
            } else if (isValueGeneric(gp)) {
                // value-генералик без аргумента и без дефолта: из аргументов он
                // не выводится — ошибка указывает на имя генерика у декларации.
                // Собираем ВСЕ недостающие генерики (не останавливаемся на первом).
                if (!reported_missing_generics.count(f.name + "\x1F" + gp.name)) {
                    Token where = gp.name_token.type == TypeOfToken::DUMMY ? f.decl_token : gp.name_token;
                    auto err = DEEP_ERRORS::MISSING_GENERIC_ARGUMENT(where, gp.name, f.name)
                        // error trace: указываем на место вызова (span имя( ... ))
                        .withSubError("call error in function '" + f.name + "'", tok_start, tok_end);
                    errs.addError(err);
                    reported_missing_generics.insert(f.name + "\x1F" + gp.name);
                }
                gbound_ok = false;
            }
        }
        if (!gbound_ok) continue;

        Cand cand(&f);
        cand.b = b;
        cand.gvals = std::move(gvals);
        cand.m.assign(np, 0);
        cand.s.assign(np, 0);
        bool ok = true;
        for (size_t j = 0; j < np; ++j) {
            int ai = argForParam[j];
            if (ai < 0) continue;   // дефолт-параметр — не участвует в матче
            if ((size_t)ai >= args.size()) { ok = false; break; }
            cand.topParam = (cand.topParam > (int)j + 1) ? cand.topParam : (int)j + 1;
            const FuncArg& pa = f.arguments[j];
            Type pt = f.locked_types
                ? (f.reified_params.size() > j ? f.reified_params[j] : Ty::Any)
                : evalFunctionParamType(pa, f, b, eval, caller, &args[ai], &ok, &cand.gvals);
#ifdef LUMEN_DEBUG
            fprintf(stderr, "[RSO] name='%s' fi=%zu param %zu pname='%s' arg='%s' pt='%s' ok=%d locked=%d\n",
                container.name.to_utf8().c_str(), fi, j, pa.name.to_utf8().c_str(),
                args[ai].T.toString().to_utf8().c_str(), pt.toString().to_utf8().c_str(), (int)ok, (int)f.locked_types);
#endif
            if (!ok) break;
            if (pt.is_auto || pt.is_any) {
                cand.m[j] = 2;
                cand.s[j] = 2;
                // Generic deduction for an untyped param (`from[VT](value)`):
                // if the function still has exactly ONE unbound type-generic,
                // bind it to the actual argument type. Otherwise the static
                // type degrades to `any` (Undefined::from(10) -> Undefined[i64]).
                if (!args[ai].T.is_auto && !args[ai].T.is_any && !(args[ai].T == Ty::Null)) {
                    String unbound;
                    bool multi = false;
                    for (size_t g = 0; g < f.generics.size(); ++g) {
                        const GenericParam& gp = f.generics[g];
                        if (isValueGeneric(gp) || b.count(gp.name)) continue;
                        if (!unbound.empty()) { multi = true; break; }
                        unbound = gp.name;
                    }
                    if (!multi && !unbound.empty()) b.emplace(unbound, args[ai].T);
                }
                continue;
            }
            if (args[ai].T == pt) {
                cand.m[j] = 0;
            } else if (args[ai].T >> pt) {
                cand.m[j] = 1;
            } else {
                // Число на месте другого числа: не расширяем молча (раньше здесь
                // стоял widenTo -> i8->i64, f32->f64, int->float) - такой вызов
                // просто не матчится ни одной перегрузкой, как и с неверным
                // нечисловым типом.
                ok = false;
                break;
            }
            cand.s[j] = specRankFor(pa, f);
        }
        if (ok) {
            cand.afp = std::move(argForParam);
            cand.b = std::move(b);
            good.push_back(std::move(cand));
        }
    }

    if (good.empty()) {
#ifdef LUMEN_DEBUG
        fprintf(stderr, "[RSO-EMPTY] name='%s' args='%s' nCand=%zu\n",
            container.name.to_utf8().c_str(), argsTypeString(args).to_utf8().c_str(), container.functions.size());
#endif
        if (reported_missing_generics.empty() && !reported_generic_err) {
            errs.addError(DEEP_ERRORS::NO_MATCHING_OVERLOAD(tok_start, tok_end, container.name, argsTypeString(args)));
        }
        return false;
    }

    auto posLess = [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;
    };

    Cand* best = &good[0];
    bool ambiguous = false;
    for (size_t k = 1; k < good.size(); ++k) {
        Cand& c = good[k];
        bool domAB = true, domBA = true, strictAB = false, strictBA = false;
        size_t n = (size_t)((best->topParam > c.topParam) ? best->topParam : c.topParam);
        if (best->f->arguments.size() < n) n = best->f->arguments.size();
        if (c.f->arguments.size() < n) n = c.f->arguments.size();
        for (size_t i = 0; i < n; ++i) {
            std::pair<int, int> ai(best->m[i], best->s[i]);
            std::pair<int, int> bi(c.m[i], c.s[i]);
            if (posLess(ai, bi)) { strictAB = true; domBA = false; }
            else if (posLess(bi, ai)) { strictBA = true; domAB = false; }
        }
        if (strictAB && domAB) { /* best строго лучше или равен c — остаётся */ }
        else if (strictBA && domBA) { /* c строго лучше best — переключаемся на него */
            best = &c; ambiguous = false;
        }
        else ambiguous = true;
    }

    if (ambiguous) {
        errs.addError(DEEP_ERRORS::AMBIGUOUS_CALL(tok_start, tok_end, container.name));
        return false;
    }

    out.function = best->f;
    out.bindings = best->b;
    out.generic_values = best->gvals;
    out.arg_for_param = best->afp;

    // Тип-генералик, который не выведен ни из явного аргумента `f[T](...)`, ни из
    // типа фактического параметра, — это не «any», а ошибка вызова: иначе
    // `fn vec[size: i64, T: Type]() -> Array[T, size]` тихо работал бы как
    // `Array[any, size]`. Проверка после выбора кандидата, чтобы не ругаться на
    // перегрузки, которые не подошли. Аналогичная проверка value-генериков —
    // выше, в ветке isValueGeneric.
    for (size_t gi = 0; gi < out.function->generics.size(); ++gi) {
        const GenericParam& gp = out.function->generics[gi];
        if (out.bindings.count(gp.name)) continue;
        String key = out.function->name + "\x1F" + gp.name;
        if (reported_missing_generics.count(key)) continue;
        Token where = gp.name_token.type == TypeOfToken::DUMMY
            ? out.function->decl_token : gp.name_token;
        errs.addError(DEEP_ERRORS::MISSING_GENERIC_ARGUMENT(where, gp.name, out.function->name)
            .withSubError("call error in function '" + out.function->name + "'", tok_start, tok_end));
        reported_missing_generics.insert(key);
    }

    out.param_types.clear();
    if (out.function->locked_types) {
        out.param_types = out.function->reified_params;
    } else {
        for (size_t i = 0; i < out.function->arguments.size(); ++i)
            out.param_types.push_back(evalFunctionParamType(out.function->arguments[i], *out.function, out.bindings, eval, caller, nullptr, nullptr, &out.generic_values));
    }
    if (out.function->return_type && compute_return) {
        // Для методов структур (struct_method_call) return-тип не реифицируем здесь:
        // имена generic-параметров структуры (FirstT/SecondT) не входят в bindings
        // функции и удят в глобальном контексте «Undefined variable». Тип вычисляет
        // вызывающая сторона (DeepAnalizer) на инстансе получателя.
        bool okr = true;
        out.return_type = out.function->locked_types
            ? out.function->reified_ret
            : evalTypeNode(out.function->return_type, *out.function, out.bindings, eval, caller, nullptr, &okr, &out.generic_values);
        if (!okr) out.return_type = Ty::Any;
    } else {
        out.return_type = Ty::Null;
    }
    return true;
}