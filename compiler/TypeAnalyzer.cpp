#include "ASTGenerator.cpp"

#include "core/Tokens.cpp"
#include "core/ErrorBase.cpp"
#include "core/TypeSystem.cpp"
#include "core/InlayHint.cpp"

#include <cstdlib>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

// TypeAnalyzer — тип-ориентированный анализ БЕЗ исполнения кода.
//
// Задача (см. compiler/plan/type-only-analysis.md): заменить обязательный
// проход-интерпретатор (ASTDeepAnalyzer) статическим выводом типов, чтобы
// большие циклы и рекурсия не «подвешивали» компиляцию и редактор.
//
// M0 — каркас + публичный интерфейс (совпадает с ASTDeepAnalyzer:
//      resolved_calls / local_types / inlay_hints / aborted / operator()).
// M1 — абстрактный домен StaticValue + вывод типов выражений (литералы,
//      переменные, арифметика, сравнения, логика, приведение, массивы).

// «Статическое значение» = тип + свойства места. Значения (конкретные
// константы, куча, ref) на этом этапе не моделируются.
static bool isTypeValue(const Type& t) {
    if (t == Ty::Type_) return true;
    if (t.type == TypeType::BASE && (t.name == "Type" || t.name == "StructType")) return true;
    return false;
}

struct StaticValue {
    Type type      = Ty::Any;   // выведенный тип
    bool is_const  = false;     // известно ли значение на этапе компиляции
    bool is_lvalue = false;     // присваиваемое место (для CONST_ASSIGN, M5)
    bool is_ref    = false;     // живая ссылка (для VALUE_RETURNS_REF, M5)
    bool nullable  = false;     // может быть null
    bool dynamic   = false;     // тип не выводится (Any/union) → runtime-dispatch

    // M5: объявленные модификаторы неизменяемости. Запись в такую переменную —
    // CONST_REASSIGNED (91) / FINAL_REASSIGNED (82). НЕ путать с `is_const`
    // (последний означает «значение известно на этапе компиляции»).
    bool immutable = false;     // `const`
    bool final_decl = false;    // `final`

    // M9: объявленные модификаторы (strict/final/static/ref/private/...).
    // Нужны для `mod(expr, modifier)` — кодоген берёт результат из узла NodeMod.
    std::vector<String> mods;

    bool isAny() const { return type.is_any || type.is_auto; }
};

// Аналог modifierFlag() интерпретатора: есть ли у объявления модификатор `name`.
// `public` истинно при отсутствии `private` (как в интерпретаторе).
inline bool hasModFlag(const std::vector<String>& mods, const String& name) {
    bool priv = false;
    for (const auto& m : mods) if (m == "private") { priv = true; break; }
    if (name == "public") return !priv;
    for (const auto& m : mods) if (m == name) return true;
    return false;
}

struct TypeAnalyzer {
    AbstractTreeT ast;
    ErrorContainer& error_container;

    // Публичный контракт — как у ASTDeepAnalyzer.
    std::map<Node*, Node*> resolved_calls;
    std::map<unsigned long long, Type> local_types;
    std::vector<InlayHintData> inlay_hints;
    bool aborted = false;

    // M8: top-level `eval { ... }` блоки, собранные collect-проходом. По ним
    // lumenc/LSP поднимают интерпретатор (см. §11.5 плана).
    std::vector<Node*> eval_blocks;

    // стек областей видимости (верх — back())
    std::vector<std::map<String, StaticValue>> scopes;

    // Function summaries (M4): имя функции -> список перегрузок с типами
    // параметров и объявленным типом возврата. Заполняется пред-проходом по
    // объявлениям, поэтому вызовы резолвятся независимо от порядка в файле.
    struct ParamSummary {
        Type type = Ty::Any;      // объявленный тип (Any, если generic/auto)
        bool is_generic = false;  // параметр — имя generic-параметра функции
        bool is_auto = false;     // `auto`/отсутствие типа/невыводимый тип
        bool has_default = false; // есть значение по умолчанию
    };
    struct FnSummary {
        Node* decl = nullptr;     // NodeFunctionDeclaration* (служит значением resolved_calls)
        std::vector<ParamSummary> params;
        Type ret = Ty::Any;       // объявленный `-> T`
        bool ret_known = false;   // `ret` валиден (не Any/auto)
        bool is_static = false;   // метод структуры, объявлен `static fn`
    };
    std::map<String, std::vector<FnSummary>> fn_summaries;

    // Символ структуры: типы полей (в т.ч. static) + методы по имени.
    struct StructSym {
        std::map<String, Type> fields;
        std::map<String, std::vector<String>> field_mods;   // M9: модификаторы поля
        std::map<String, std::vector<FnSummary>> methods;
        bool has_generics = false;   // generic-структура: методы/__call__ не резолвим
    };
    std::map<String, StructSym> struct_syms;

    // M9: имена ref-параметров текущей функции (`ref arg`) — для `mod(x, ref)`,
    // который в интерпретаторе становится динамическим (читает mods значения).
    std::set<String> cur_ref_params;

    // M5: диагностики. `diagnosed` — чтобы не дублировать на один узел (infer
    // может вызываться многократно). `cur_ret*` — объявленный возврат текущей
    // функции (для RETURN_TYPE_MISMATCH).
    std::set<Node*> diagnosed;
    Type cur_ret = Ty::Any;
    bool cur_ret_known = false;

    // M9: анализ тел методов структур (ради аннотаций mod/typeof) ведётся в
    // режиме подавления диагностик — у методов своя область и self, ложные
    // срабатывания недопустимы (гейт кодогена).
    bool suppress_diag = false;

    bool debug_dump = false;   // LUMEN_TYPES: печатать выведенные типы (отладка)

    TypeAnalyzer(AbstractTreeT ast, ErrorContainer& error_container)
        : ast(ast), error_container(error_container) {}

    // ---------- окружение ----------
    void pushScope() { scopes.emplace_back(); }
    void popScope()  { if (!scopes.empty()) scopes.pop_back(); }

    void declare(const String& name, const StaticValue& v) {
        if (scopes.empty()) return;
        scopes.back()[name] = v;
    }

    const StaticValue* lookup(const String& name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    StaticValue* lookupMutable(const String& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    // Запись в место: для переменной обновляем её статический тип. Более
    // сложные места (индексы, поля, разыменование) — M5+.
    void assignTo(Node* lval, const StaticValue& v) {
        Node* l = unwrapScope(lval);
        if (l && l->type == NodeTypes::NodeVariable) {
            auto* var = static_cast<struct NodeVariable*>(l);
            if (StaticValue* slot = lookupMutable(var->name)) {
                slot->type = v.type;
                slot->dynamic = v.dynamic || v.type.is_any;
                slot->nullable = v.nullable;
            }
        }
    }

    static const StaticValue* findInStack(
            const std::vector<std::map<String, StaticValue>>& stack,
            const String& name) {
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    // Слияние значений после ветвления: тип — объединение, const только
    // если const в обеих ветвях, nullable — если хотя бы в одной.
    StaticValue mergeValue(const StaticValue& a, const StaticValue& b) const {
        StaticValue m = a;
        m.type = joinType(a.type, b.type);
        m.is_const = a.is_const && b.is_const;
        m.nullable = a.nullable || b.nullable;
        m.dynamic = a.dynamic || b.dynamic || m.type.is_any;
        return m;
    }

    // Слить изменения пред-существующих переменных из двух прогонов ветвлений.
    // Новые локали, объявленные только в ветви, не переносятся (они вне scope).
    void mergeBranches(
            const std::vector<std::map<String, StaticValue>>& base,
            const std::vector<std::map<String, StaticValue>>& then_s,
            const std::vector<std::map<String, StaticValue>>& else_s) {
        for (size_t lvl = 0; lvl < base.size() && lvl < scopes.size(); lvl++) {
            for (const auto& kv : base[lvl]) {
                const StaticValue* a = findInStack(then_s, kv.first);
                const StaticValue* b = findInStack(else_s, kv.first);
                if (!a) a = &kv.second;
                if (!b) b = &kv.second;
                scopes[lvl][kv.first] = mergeValue(*a, *b);
            }
        }
    }

    // ---------- типы из тип-выражений ----------
    // Внимание: Type::operator== считает Any равным чему угодно, поэтому
    // «встроенное ли имя» проверяем строками, а не сравнением с Ty::Any.
    static bool builtinType(const String& nm, Type& out) {
        if (nm == "i8")  { out = Ty::i8;  return true; }
        if (nm == "i16") { out = Ty::i16; return true; }
        if (nm == "i32") { out = Ty::i32; return true; }
        if (nm == "i64") { out = Ty::i64; return true; }
        if (nm == "f16") { out = Ty::f16; return true; }
        if (nm == "f32") { out = Ty::f32; return true; }
        if (nm == "f64") { out = Ty::f64; return true; }
        if (nm == "bool") { out = Ty::Bool; return true; }
        if (nm == "char") { out = Ty::Char; return true; }
        if (nm == "str")  { out = Ty::Str;  return true; }
        if (nm == "Type") { out = Ty::Type_; return true; }
        if (nm == "StructType") { out = Ty::Type_; return true; }
        if (nm == "Null") { out = Ty::Null; return true; }
        if (nm == "any")  { out = Ty::Any;  return true; }
        if (nm == "auto") { out = Ty::Auto; return true; }
        return false;
    }
    static bool isBuiltinName(const String& nm) {
        Type t = Ty::Any; return builtinType(nm, t);
    }

    static Node* unwrapScope(Node* n) {
        while (n && n->type == NodeTypes::NodeScope)
            n = static_cast<struct NodeScope*>(n)->node;
        return n;
    }

    // Снять обёртки scope/modifier (модификаторы членов структуры: static и пр.).
    static Node* unwrapMod(Node* n) {
        while (n && (n->type == NodeTypes::NodeScope
                  || n->type == NodeTypes::NodeModifier)) {
            if (n->type == NodeTypes::NodeScope)
                n = static_cast<struct NodeScope*>(n)->node;
            else
                n = static_cast<struct NodeModifier*>(n)->node;
        }
        return n;
    }

    // Тип из узла-выражения типа (`i64`, `bool`, ...). M1: встроенные и struct.
    Type typeFromTypeExpr(Node* t) {
        t = unwrapMod(t);
        if (!t) return Ty::Any;
        if (t->type == NodeTypes::NodeVariable) {
            String nm = static_cast<struct NodeVariable*>(t)->name;
            Type out = Ty::Any;
            if (builtinType(nm, out)) return out;
        if (nm == "auto" || nm == "any") return Ty::Any;
        if (nm == "StructType") return Ty::Type_;
        if (struct_syms.count(nm)) return Type::StructType(nm);
        return Ty::Any;
        }
        // M9: составные типы — массив, кортеж, union, nullable (?T).
        if (t->type == NodeTypes::NodeArrayType) {
            auto* at = static_cast<struct NodeArrayType*>(t);
            Type elem = typeFromTypeExpr(at->element_type);
            int sz = -1;
            if (at->count) {
                Node* c = unwrapMod(at->count);
                if (c && c->type == NodeTypes::NodeNumber) {
                    auto* num = static_cast<struct NodeNumber*>(c);
                    if (num->is_integer) sz = num->int_value;
                }
            }
            return Type::ArrayType(elem, sz);
        }
        if (t->type == NodeTypes::NodeTupleType) {
            auto* tt = static_cast<struct NodeTupleType*>(t);
            std::vector<Type> ts;
            ts.reserve(tt->types.size());
            for (auto* e : tt->types) ts.push_back(typeFromTypeExpr(e));
            return Type::TupleType(std::move(ts));
        }
        if (t->type == NodeTypes::NodeBinaryBitwiseOr) {
            auto* b = static_cast<struct NodeBinaryBitwiseOr*>(t);
            Type l = typeFromTypeExpr(b->l_node);
            Type r = typeFromTypeExpr(b->r_node);
            if (isAnyType(l) || isAnyType(r)) return Ty::Any;
            return l | r;
        }
        if (t->type == NodeTypes::NodeUnaryNegation) {
            auto* u = static_cast<struct NodeUnaryNegation*>(t);
            if (u->token.value == "?") {
                Type inner = typeFromTypeExpr(u->node);
                if (isAnyType(inner)) return Ty::Any;
                return inner | Ty::Null;
            }
        }
        return Ty::Any;
    }

    // ---------- function summaries и резолв вызовов (M4) ----------
    // Собираем объявления функций (только top-level, без захода в тела и
    // namespace) в таблицу перегрузок, затем резолвим вызовы и заполняем
    // resolved_calls[call] = NodeFunctionDeclaration* — контракт кодогена.
    // Заполнить сигнатуру из объявления функции (параметры + объявленный `-> T`).
    void fillSummary(FnSummary& s, struct NodeFunctionDeclaration* fd) {
        s.decl = (Node*)fd;
        std::set<String> gnames;
        for (const auto& g : fd->generics) gnames.insert(g.name);
        for (const auto& p : fd->params) {
            ParamSummary ps;
            ps.has_default = p.default_value != nullptr;
            Node* tn = unwrapMod(p.type);
            if (tn && tn->type == NodeTypes::NodeVariable) {
                String nm = static_cast<struct NodeVariable*>(tn)->name;
                if (gnames.count(nm)) ps.is_generic = true;
                else if (nm == "auto" || nm == "any") ps.is_auto = true;
                else {
                    ps.type = typeFromTypeExpr(tn);
                    if (isAnyType(ps.type)) ps.is_auto = true;
                }
            } else {
                ps.is_auto = true;   // нет типа / составной тип — не резолвим по нему
            }
            s.params.push_back(ps);
        }
        Type rt = typeFromTypeExpr(fd->return_type);
        if (!isAnyType(rt)) { s.ret = rt; s.ret_known = true; }
    }

    void collectFunction(struct NodeFunctionDeclaration* fd) {
        FnSummary s;
        fillSummary(s, fd);
        fn_summaries[fd->name].push_back(std::move(s));
    }

    // Собрать структуру: типы полей (включая static) и методы (с флагом static).
    void collectStruct(struct NodeStructDeclaration* sd) {
        // Регистрируем имя структуры заранее: типы её полей/методов могут
        // ссылаться на саму структуру (self-referencing).
        StructSym& sym = struct_syms[sd->name];
        std::set<String> gnames;
        for (const auto& g : sd->generics) gnames.insert(g.name);
        sym.has_generics = !sd->generics.empty();
        Node* body = unwrapMod(sd->body);
        if (body && body->type == NodeTypes::NodeStatementsScope) {
            auto* ss = static_cast<struct NodeStatementsScope*>(body);
            for (Node* m : ss->nodes) {
                bool is_static = false;
                std::vector<String> field_mods;
                Node* u = m;
                while (u && u->type == NodeTypes::NodeModifier) {
                    auto* mod = static_cast<struct NodeModifier*>(u);
                    field_mods.push_back(mod->name);
                    if (mod->name == "static") is_static = true;
                    u = mod->node;
                }
                u = unwrapMod(u);
                if (!u) continue;
                if (u->type == NodeTypes::NodeVariableDeclaration) {
                    auto* vd = static_cast<struct NodeVariableDeclaration*>(u);
                    Type ft = Ty::Any;
                    Node* tn = unwrapMod(vd->type_node);
                    if (tn && tn->type == NodeTypes::NodeVariable) {
                        String nm = static_cast<struct NodeVariable*>(tn)->name;
                        if (!gnames.count(nm)) ft = typeFromTypeExpr(tn);
                    }
                    sym.fields.emplace(vd->name, ft);
                    sym.field_mods[vd->name] = field_mods;
                } else if (u->type == NodeTypes::NodeFunctionDeclaration) {
                    auto* fd = static_cast<struct NodeFunctionDeclaration*>(u);
                    FnSummary s;
                    fillSummary(s, fd);
                    s.is_static = is_static;
                    sym.methods[fd->name].push_back(std::move(s));
                }
            }
        }
    }

    void collectFunctions(const std::vector<Node*>& nodes) {
        for (Node* n : nodes) {
            Node* u = unwrapMod(n);
            if (!u) continue;
            if (u->type == NodeTypes::NodeFunctionDeclaration)
                collectFunction(static_cast<struct NodeFunctionDeclaration*>(u));
            else if (u->type == NodeTypes::NodeStatementsScope) {
                auto* s = static_cast<struct NodeStatementsScope*>(u);
                collectFunctions(s->nodes);
            }
            // не заходим в тела функций и namespace (консервативно: там свои
            // области, разрешение которых кодоген делает сам).
        }
    }

    // Отдельный проход: сначала все структуры (чтобы struct-типы были известны
    // при разборе сигнатур функций), затем — функции.
    void collectStructs(const std::vector<Node*>& nodes) {
        for (Node* n : nodes) {
            Node* u = unwrapMod(n);
            if (!u) continue;
            if (u->type == NodeTypes::NodeStructDeclaration)
                collectStruct(static_cast<struct NodeStructDeclaration*>(u));
            else if (u->type == NodeTypes::NodeStatementsScope) {
                auto* s = static_cast<struct NodeStatementsScope*>(u);
                collectStructs(s->nodes);
            }
        }
    }

    static int specRankOf(const ParamSummary& p) {
        if (p.is_auto) return 2;
        if (p.is_generic) return 1;
        return 0;
    }

    // Ранг совпадения аргумента с параметром: 0 exact, 1 convert, 2 слабое, -1 мимо.
    int matchRank(const ParamSummary& p, const StaticValue& a) {
        if (p.is_generic) return 2;
        if (p.is_auto || isAnyType(p.type)) return 2;
        if (isAnyType(a.type)) return 2;   // неизвестный тип аргумента — слабое
        if (a.type == p.type) return 0;
        if (a.type >> p.type) return 1;
        return -1;
    }

    // Выбор перегрузки по статическим типам аргументов. `skipSelf` — первый
    // параметр это `self` (instance-метод), он не сопоставляется с аргументами.
    // Возвращает nullptr, если кандидатов нет или выбор неоднозначен (тогда
    // кодоген резолвит сам).
    const FnSummary* chooseOverloadList(const std::vector<FnSummary>& cands,
                                        const std::vector<StaticValue>& args,
                                        bool skipSelf) {
        if (cands.empty()) return nullptr;
        size_t off = skipSelf ? 1 : 0;

        struct Cand {
            const FnSummary* f;
            std::vector<std::pair<int, int>> pos;   // (ранг совпадения, специфичность)
        };
        std::vector<Cand> good;
        for (const auto& f : cands) {
            if (f.params.size() < off) continue;
            size_t np = f.params.size() - off;   // число «настоящих» параметров
            if (args.size() > np) continue;
            bool missing = false;
            for (size_t j = args.size(); j < np; j++)
                if (!f.params[off + j].has_default) { missing = true; break; }
            if (missing) continue;
            Cand c; c.f = &f;
            bool ok = true;
            for (size_t j = 0; j < args.size(); j++) {
                int mr = matchRank(f.params[off + j], args[j]);
                if (mr < 0) { ok = false; break; }
                c.pos.push_back({mr, specRankOf(f.params[off + j])});
            }
            if (ok) good.push_back(std::move(c));
        }
        if (good.empty()) return nullptr;
        if (good.size() == 1) return good[0].f;

        auto posLess = [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
            if (a.first != b.first) return a.first < b.first;
            return a.second < b.second;
        };
        const Cand* best = &good[0];
        bool ambiguous = false;
        for (size_t k = 1; k < good.size(); k++) {
            const Cand& c = good[k];
            bool domAB = true, domBA = true, strictAB = false, strictBA = false;
            size_t n = best->pos.size() < c.pos.size() ? best->pos.size() : c.pos.size();
            for (size_t i = 0; i < n; i++) {
                if (posLess(best->pos[i], c.pos[i])) { strictAB = true; domBA = false; }
                else if (posLess(c.pos[i], best->pos[i])) { strictBA = true; domAB = false; }
            }
            if (strictAB && domAB) { /* best не хуже — остаётся */ }
            else if (strictBA && domBA) { best = &c; ambiguous = false; }
            else ambiguous = true;
        }
        if (ambiguous) return nullptr;
        return best->f;
    }

    const FnSummary* chooseOverload(const String& name,
                                    const std::vector<StaticValue>& args) {
        auto it = fn_summaries.find(name);
        if (it == fn_summaries.end()) return nullptr;
        return chooseOverloadList(it->second, args, /*skipSelf=*/false);
    }

    // ---------- диагностики (M5) ----------
    // Паритет по статическим кодам с интерпретатором: имена/члены/арность,
    // объявленный возврат, запись в const/final. Диагностика эмитится один раз
    // на узел (infer может вызываться многократно).
    void addDiag(Node* n, const BaseError& e) {
        if (suppress_diag) return;
        if (!n) return;
        if (!diagnosed.insert(n).second) return;
        error_container.addError(e);
    }

    // Принимает ли перегрузка такое число аргументов (с учётом дефолтов).
    static bool candidateAcceptsArity(const FnSummary& f, size_t nargs, bool skipSelf) {
        size_t off = skipSelf ? 1 : 0;
        if (f.params.size() < off) return false;
        size_t np = f.params.size() - off;
        if (nargs > np) return false;
        for (size_t j = nargs; j < np; j++)
            if (!f.params[off + j].has_default) return false;
        return true;
    }
    static bool anyAcceptsArity(const std::vector<FnSummary>& cands, size_t nargs, bool skipSelf) {
        for (const auto& f : cands) if (candidateAcceptsArity(f, nargs, skipSelf)) return true;
        return false;
    }

    static String argsTypeStringOf(const std::vector<StaticValue>& args) {
        String s;
        for (size_t i = 0; i < args.size(); i++) {
            if (i) s += ", ";
            s += args[i].type.toString();
        }
        return s;
    }

    // M5: запись в переменную, объявленную const/final. Диагностика вешается на
    // узел-владелец (assign/inc/dec), чтобы не задваивать.
    void checkAssignTarget(Node* target, const Token& tok, Node* diag_owner) {
        Node* l = unwrapScope(target);
        if (!l || l->type != NodeTypes::NodeVariable) return;
        auto* v = static_cast<struct NodeVariable*>(l);
        const StaticValue* sv = lookup(v->name);
        if (!sv) return;
        if (sv->final_decl)
            addDiag(diag_owner, DEEP_ERRORS::FINAL_REASSIGNED(tok, v->name));
        else if (sv->immutable)
            addDiag(diag_owner, DEEP_ERRORS::CONST_REASSIGNED(tok, v->name));
    }

    // ---------- вывод типа выражений ----------
    static bool isAnyType(const Type& t) { return t.is_any || t.is_auto; }

    // Наращивающий тип: меньший ранг -> больший; int+float -> float.
    Type resultTypeOf(const Type& a, const Type& b) const {
        if (isAnyType(a) || isAnyType(b)) return Ty::Any;
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
            return af ? a : b;
        }
        if (intRank(a) == 0 || intRank(b) == 0) return Ty::Any;
        return (intRank(a) >= intRank(b)) ? a : b;
    }

    // Тип-объединение: совпадающие -> сам тип; иначе Any (для v1).
    Type joinType(const Type& a, const Type& b) const {
        if (isAnyType(a) || isAnyType(b)) return Ty::Any;
        if (a == b) return a;
        if (a.isUnion() || b.isUnion()) return a | b;
        return Ty::Any;
    }

    // Общий тип элемента массива — зеркало DeepAnalizer::commonElementType.
    // Используется для литералов `{...}`, конкатенации и append.
    Type commonElemType(const Type& a, const Type& b) const {
        if (isAnyType(a) || isAnyType(b)) return Ty::Any;
        if (a == b) return a;
        if (b >> a) return a;
        if (a >> b) return b;
        return a | b;
    }

    // Тип без члена Null (для `l ?? r`): union -> union без Null, одиночный Null
    // -> Any (значение неопределимо), прочее -> как есть.
    Type dropNull(const Type& t) const {
        if (isAnyType(t)) return Ty::Any;
        if (t == Ty::Null) return Ty::Any;
        if (!t.isUnion()) return t;
        std::vector<Type> keep;
        for (const auto& e : t.container_1) {
            if (!isAnyType(e) && e == Ty::Null) continue;
            keep.push_back(e);
        }
        if (keep.empty()) return Ty::Any;
        Type u = keep[0];
        for (size_t i = 1; i < keep.size(); i++) u = u | keep[i];
        return u;
    }

    StaticValue infer(Node* n) {
        StaticValue sv;
        n = unwrapScope(n);
        if (!n) return sv;

        switch (n->type) {
            case NodeTypes::NodeNumber: {
                auto* num = static_cast<struct NodeNumber*>(n);
                sv.type = num->is_integer ? Ty::i64 : Ty::f64;
                sv.is_const = true;
                return sv;
            }
            case NodeTypes::NodeString: {
                auto* s = static_cast<struct NodeString*>(n);
                sv.type = s->is_char ? Ty::Char : Ty::Str;
                sv.is_const = true;
                return sv;
            }
            case NodeTypes::NodeBool:
                sv.type = Ty::Bool;
                sv.is_const = true;
                return sv;
            case NodeTypes::NodeNull:
                sv.type = Ty::Null;
                sv.nullable = true;
                return sv;

            case NodeTypes::NodeVariable: {
                auto* v = static_cast<struct NodeVariable*>(n);
                if (const StaticValue* found = lookup(v->name)) return *found;
                if (isBuiltinName(v->name)) { sv.type = Ty::Type_; return sv; }
                sv.type = Ty::Any;
                sv.dynamic = true;
                return sv;
            }

            case NodeTypes::NodeArray: {
                auto* arr = static_cast<struct NodeArray*>(n);
                Type elem = Ty::Any;
                bool first = true;
                for (auto* e : arr->elements) {
                    Type et = infer(e).type;
                    elem = first ? et : commonElemType(elem, et);
                    first = false;
                }
                sv.type = Type::ArrayType(elem, (int)arr->elements.size());
                sv.dynamic = elem.is_any;
                return sv;
            }

            // `Array[T, N]{…}` — конструктор: тип элемента из `Array[…]`,
            // длина — фактическое число элементов (как в интерпретаторе).
            case NodeTypes::NodeArrayType: {
                auto* at = static_cast<struct NodeArrayType*>(n);
                for (auto* it : at->items) infer(it);
                if (!at->has_body) { sv.type = typeFromTypeExpr(at); return sv; }
                Type elem = typeFromTypeExpr(at->element_type);
                sv.type = Type::ArrayType(elem, (int)at->items.size());
                sv.dynamic = elem.is_any;
                return sv;
            }

            case NodeTypes::NodeArrayAccess: {
                auto* aa = static_cast<struct NodeArrayAccess*>(n);
                Type bt = infer(aa->base).type;
                if (bt.isArray() && !bt.container_1.empty()) { sv.type = bt.container_1[0]; return sv; }
                sv.type = Ty::Any; sv.dynamic = true;
                return sv;
            }

            case NodeTypes::NodeCast: {
                auto* c = static_cast<struct NodeCast*>(n);
                sv.type = typeFromTypeExpr(c->r_node);
                sv.dynamic = sv.type.is_any;
                return sv;
            }

            case NodeTypes::NodeStructLiteral: {
                auto* sl = static_cast<struct NodeStructLiteral*>(n);
                for (const auto& init : sl->inits) infer(init.value);
                String nm = sl->struct_name;
                if (nm.empty()) {
                    Node* te = unwrapMod(sl->type_expr);
                    if (te && te->type == NodeTypes::NodeVariable)
                        nm = static_cast<struct NodeVariable*>(te)->name;
                }
                if (!nm.empty() && struct_syms.count(nm)) { sv.type = Type::StructType(nm); return sv; }
                sv.type = Ty::Any; sv.dynamic = true;
                return sv;
            }

            case NodeTypes::NodeMemberAccess: {
                auto* ma = static_cast<struct NodeMemberAccess*>(n);
                StaticValue recv = infer(ma->l_node);
                String memb;
                Node* mr = unwrapMod(ma->r_node);
                if (mr && mr->type == NodeTypes::NodeVariable)
                    memb = static_cast<struct NodeVariable*>(mr)->name;
                if (recv.type.isStruct() && !memb.empty()) {
                    auto sit = struct_syms.find(recv.type.name);
                    if (sit != struct_syms.end()) {
                        auto fit = sit->second.fields.find(memb);
                        if (fit != sit->second.fields.end()) {
                            sv.type = fit->second;
                            sv.dynamic = sv.type.is_any;
                            return sv;
                        }
                        // M5: у известной не-generic структуры нет ни поля, ни
                        // метода с таким именем -> NO_SUCH_FIELD.
                        if (!sit->second.has_generics
                                && !sit->second.methods.count(memb)) {
                            addDiag(ma, DEEP_ERRORS::NO_SUCH_FIELD(
                                ma->member_token, recv.type.name, memb));
                        }
                    }
                }
                sv.type = Ty::Any; sv.dynamic = true;
                return sv;
            }

            // --- арифметика ---
            case NodeTypes::NodeBinaryMultiplicative:
            case NodeTypes::NodeBinaryDivision:
            case NodeTypes::NodeBinaryRemainder:
            case NodeTypes::NodeBinaryAdditive:
            case NodeTypes::NodeBinarySubtraction:
            case NodeTypes::NodeBinaryShiftLeft:
            case NodeTypes::NodeBinaryShiftRight:
            case NodeTypes::NodeBinaryBitwiseAnd:
            case NodeTypes::NodeBinaryBitwiseXor:
            case NodeTypes::NodeBinaryBitwiseOr: {
                auto* b = static_cast<struct NodeBinaryAdditive*>(n);
                StaticValue l = infer(b->l_node), r = infer(b->r_node);
                // M9: конкатенация массивов `a + b` -> Array[общий_элемент, n1+n2].
                if (n->type == NodeTypes::NodeBinaryAdditive
                        && l.type.isArray() && r.type.isArray()
                        && !l.type.container_1.empty() && !r.type.container_1.empty()) {
                    Type elem = commonElemType(l.type.container_1[0], r.type.container_1[0]);
                    int lsz = l.type.size, rsz = r.type.size;
                    int total = (lsz >= 0 && rsz >= 0) ? lsz + rsz : -1;
                    sv.type = Type::ArrayType(elem, total);
                    sv.dynamic = sv.type.is_any || elem.is_any;
                    return sv;
                }
                sv.type = resultTypeOf(l.type, r.type);
                sv.is_const = l.is_const && r.is_const;
                sv.dynamic = sv.type.is_any;
                return sv;
            }

            // --- сравнения и логика -> bool ---
            case NodeTypes::NodeBinaryEqual:
            case NodeTypes::NodeBinaryNotEqual:
            case NodeTypes::NodeBinaryLess:
            case NodeTypes::NodeBinaryGreater:
            case NodeTypes::NodeBinaryLessEqual:
            case NodeTypes::NodeBinaryGreaterEqual:
            case NodeTypes::NodeBinaryLogicalAnd:
            case NodeTypes::NodeBinaryLogicalOr: {
                auto* b = static_cast<struct NodeBinaryAdditive*>(n);
                StaticValue l = infer(b->l_node), r = infer(b->r_node);
                sv.type = Ty::Bool;
                sv.is_const = l.is_const && r.is_const;
                return sv;
            }

            case NodeTypes::NodeBinaryNullCoalesce: {
                auto* b = static_cast<struct NodeBinaryNullCoalesce*>(n);
                StaticValue l = infer(b->l_node), r = infer(b->r_node);
                if (!isAnyType(l.type) && l.type == Ty::Null) { sv.type = r.type; }
                else {
                    // `l ?? r` никогда не даёт Null: убираем Null из типа l.
                    Type ln = dropNull(l.type);
                    sv.type = isAnyType(ln) ? joinType(l.type, r.type)
                                            : joinType(ln, r.type);
                }
                sv.dynamic = sv.type.is_any;
                return sv;
            }

            // Присваивание: `a = expr`, `a += b` (канонизовано в `a = a + b`).
            // RHS выводим ДО записи (важно для `a = a + b`), потом обновляем
            // тип места. Тип выражения = тип записанного значения.
            case NodeTypes::NodeBinaryAssignment: {
                auto* a = static_cast<struct NodeBinaryAssignment*>(n);
                StaticValue lhs = infer(a->l_node);   // текущее значение места
                StaticValue rhs = infer(a->r_node);
                checkAssignTarget(a->l_node, a->start_token, a);   // M5: const/final
                assignTo(a->l_node, rhs);
                sv = lhs;
                sv.type = rhs.type;
                sv.is_const = rhs.is_const;
                sv.nullable = rhs.nullable;
                sv.dynamic = rhs.dynamic || sv.type.is_any;
                return sv;
            }

            // --- унарные ---
            case NodeTypes::NodeUnaryMinus:
            case NodeTypes::NodeUnaryPlus: {
                auto* u = static_cast<struct NodeUnaryMinus*>(n);
                return infer(u->node);
            }
            case NodeTypes::NodeUnaryBitwiseNot: {
                auto* u = static_cast<struct NodeUnaryBitwiseNot*>(n);
                return infer(u->node);
            }
            case NodeTypes::NodeUnaryNegation: {
                auto* u = static_cast<struct NodeUnaryNegation*>(n);
                // `?T` — тип-nullable (в контексте типа даёт Type-значение).
                if (u->token.value == "?") {
                    infer(u->node);
                    sv.type = Ty::Type_;
                    return sv;
                }
                // `!{...}` — литерал кортежа: тип = Tuple[тип_i каждого элемента].
                Node* op = unwrapMod(u->node);
                if (op && op->type == NodeTypes::NodeArray) {
                    auto* arr = static_cast<struct NodeArray*>(op);
                    std::vector<Type> ts;
                    ts.reserve(arr->elements.size());
                    for (auto* e : arr->elements) ts.push_back(infer(e).type);
                    sv.type = Type::TupleType(std::move(ts));
                    return sv;
                }
                StaticValue o = infer(u->node);
                if (o.type.isArray() && !o.type.container_1.empty()) {
                    // `!arr` — кортеж из типов элементов (как интерпретатор).
                    std::vector<Type> ts;
                    int cnt = o.type.size >= 0 ? o.type.size : 1;
                    for (int i = 0; i < cnt; i++) ts.push_back(o.type.container_1[0]);
                    sv.type = Type::TupleType(std::move(ts));
                    return sv;
                }
                sv.type = Ty::Bool;
                return sv;
            }
            case NodeTypes::NodeUnaryDereference:
            case NodeTypes::NodeUnaryAddressOf:
            case NodeTypes::NodePtr:
            case NodeTypes::NodeNew:
            case NodeTypes::NodeLambda:
                sv.type = Ty::Any;
                sv.dynamic = true;
                return sv;

            case NodeTypes::NodePostfixUnwrap: {
                // `x?` (авто-распаковка через __get__): вывести тип операнда,
                // чтобы резолвить вызовы/аннотации внутри (иначе `Num(9)?` не
                // резолвится). Тип результата неизвестен статически.
                auto* u = static_cast<struct NodePostfixUnwrap*>(n);
                infer(u->node);
                sv.type = Ty::Any;
                sv.dynamic = true;
                return sv;
            }

            case NodeTypes::NodeUnaryPostfixIncrement:
            case NodeTypes::NodeUnaryPostfixDecrement:
            case NodeTypes::NodeUnaryPrefixIncrement:
            case NodeTypes::NodeUnaryPrefixDecrement: {
                auto* u = static_cast<struct NodeUnaryPostfixIncrement*>(n);
                checkAssignTarget(u->node, u->token, n);   // M5: const/final
                return infer(u->node);
            }

            case NodeTypes::NodeMod: {
                // M9: кодоген читает n->value/n->resolved/n->dynamic, вычисленные
                // анализатором (интерпретатор делает это в resolveMod). Повторяем
                // дословно: переменная в области -> её модификаторы; поле структуры
                // -> модификаторы поля. `ref`-параметр делает проверку динамической.
                auto* nm = static_cast<struct NodeMod*>(n);
                nm->resolved = false;
                nm->value = false;
                Node* e = unwrapMod(nm->expr);
                if (e && e->type == NodeTypes::NodeVariable) {
                    auto* var = static_cast<struct NodeVariable*>(e);
                    if (const StaticValue* sv0 = lookup(var->name)) {
                        nm->resolved = true;
                        nm->value = hasModFlag(sv0->mods, nm->modifier);
                        if (nm->modifier == "ref" && cur_ref_params.count(var->name))
                            nm->dynamic = true;
                    }
                } else if (e && e->type == NodeTypes::NodeMemberAccess) {
                    auto* ma = static_cast<struct NodeMemberAccess*>(e);
                    String field;
                    Node* mr = unwrapMod(ma->r_node);
                    if (mr && mr->type == NodeTypes::NodeVariable)
                        field = static_cast<struct NodeVariable*>(mr)->name;
                    StaticValue base = infer(ma->l_node);
                    if (base.type.isStruct() && !field.empty()) {
                        auto sit = struct_syms.find(base.type.name);
                        if (sit != struct_syms.end()) {
                            auto mit = sit->second.field_mods.find(field);
                            if (mit != sit->second.field_mods.end()) {
                                nm->resolved = true;
                                nm->value = hasModFlag(mit->second, nm->modifier);
                            }
                        }
                    }
                }
                sv.type = Ty::Bool;
                return sv;
            }
            case NodeTypes::NodeSizeOf:
                sv.type = Ty::i64;
                return sv;
            case NodeTypes::NodeTypeOf: {
                // M9 (вариант B): typeof/typeis сворачиваются в СТАТИЧЕСКИЙ тип.
                // Интерпретатор использует рантайм-тип значения; здесь — тип,
                // выведенный статикой (может быть шире, напр. i64|f64 вместо i64).
                auto* t = static_cast<struct NodeTypeOf*>(n);
                StaticValue inner = infer(t->expr);
                Type st = inner.type;
                if (!st.is_any && !st.is_auto) {
                    bool arr_unknown_size = st.isArray() && st.size < 0;
                    // ::type на типе-массиве: элемент известен даже при size<0.
                    if (st.isArray() && !st.container_1.empty()
                            && !isAnyType(st.container_1[0])) {
                        t->static_type_elem = st.container_1[0].toString();
                        t->has_static_elem = true;
                    }
                    // Массив с неизвестной длиной сворачивать нельзя (интерпретатор
                    // знает фактическую длину значения) — оставляем runtime-dispatch.
                    if (!arr_unknown_size) {
                        String repr;
                        if (st.isStruct() && !st.name.empty()) repr = String("StructType");
                        else if (st.type == TypeType::BASE && st.name == "StructType") repr = String("StructType");
                        else if (st.type == TypeType::BASE && st.name == "Type") repr = String("Type");
                        else repr = st.toString();
                        if (!t->has_static_type) {
                            t->static_type = repr;
                            t->has_static_type = true;
                        } else if (t->static_type != repr) {
                            t->static_type_ambiguous = true;
                        }
                    }
                }
                sv.type = Ty::Type_;
                return sv;
            }

            // `typeof(x)::type` / `::size`, enum-доступ, статики структур:
            // просто выводим обе стороны (важно — вывести l_node, чтобы у
            // вложенного typeof проставились static_type_elem/static_type).
            case NodeTypes::NodeBinaryScopeResolution: {
                auto* sr = static_cast<struct NodeBinaryScopeResolution*>(n);
                StaticValue l = infer(sr->l_node);
                infer(sr->r_node);
                String rname;
                Node* r = unwrapMod(sr->r_node);
                if (r && r->type == NodeTypes::NodeVariable)
                    rname = static_cast<struct NodeVariable*>(r)->name;
                if (isTypeValue(l.type) && rname == "size") { sv.type = Ty::i64; return sv; }
                if (isTypeValue(l.type) && rname == "type") { sv.type = Ty::Type_; return sv; }
                sv.type = Ty::Any;
                sv.dynamic = true;
                return sv;
            }

            case NodeTypes::NodeFunctionCall: {
                auto* call = static_cast<struct NodeFunctionCall*>(n);
                std::vector<StaticValue> argv;
                argv.reserve(call->args.size());
                bool has_named = false;
                for (auto* a : call->args) {
                    Node* au = unwrapScope(a);
                    if (au && au->type == NodeTypes::NodeNamedArgument) {
                        has_named = true;
                        argv.push_back(infer(static_cast<struct NodeNamedArgument*>(au)->value));
                    } else {
                        argv.push_back(infer(a));
                    }
                }
                Node* callee = unwrapScope(call->callee);
                const FnSummary* resolved = nullptr;
                String dbg_name;
                if (!has_named) {
                    if (callee && callee->type == NodeTypes::NodeVariable) {
                        auto* cn = static_cast<struct NodeVariable*>(callee);
                        // не резолвим по имени, если это локальная переменная
                        // (вызов значения-функции — кодоген разбирает сам)
                        if (!lookup(cn->name)) {
                            resolved = chooseOverload(cn->name, argv);
                            if (!resolved) {
                                // структура как вызываемое значение: `Number(20)` -> __call__ (static)
                                auto sit = struct_syms.find(cn->name);
                                if (sit != struct_syms.end()) {
                                    auto mit = sit->second.methods.find("__call__");
                                    if (mit != sit->second.methods.end())
                                        resolved = chooseOverloadList(mit->second, argv, /*skipSelf=*/false);
                                }
                            }
                            dbg_name = cn->name;
                            // M5: диагностика неразрешённого вызова. Консервативно:
                            // структуры/enum/встроенные имена не трогаем (у них своя
                            // семантика конструктора/приведения).
                            if (!resolved && !struct_syms.count(cn->name)
                                    && !isBuiltinName(cn->name)) {
                                auto fit = fn_summaries.find(cn->name);
                                if (fit != fn_summaries.end()) {
                                    if (!anyAcceptsArity(fit->second, argv.size(), false))
                                        addDiag(call, DEEP_ERRORS::NO_MATCHING_OVERLOAD(
                                            call->open_token, call->close_token, cn->name,
                                            argsTypeStringOf(argv)));
                                } else {
                                    addDiag(call, DEEP_ERRORS::FUNCTION_NOT_FOUND(
                                        cn->token, cn->name));
                                }
                            }
                        }
                    } else if (callee && callee->type == NodeTypes::NodeMemberAccess) {
                        auto* ma = static_cast<struct NodeMemberAccess*>(callee);
                        String memb;
                        Node* mr = unwrapMod(ma->r_node);
                        if (mr && mr->type == NodeTypes::NodeVariable)
                            memb = static_cast<struct NodeVariable*>(mr)->name;
                        StaticValue recv = infer(ma->l_node);
                        if (recv.type.isStruct() && !memb.empty()) {
                            auto sit = struct_syms.find(recv.type.name);
                            if (sit != struct_syms.end() && !sit->second.has_generics) {
                                auto mit = sit->second.methods.find(memb);
                                if (mit != sit->second.methods.end()) {
                                    resolved = chooseOverloadList(mit->second, argv, /*skipSelf=*/true);
                                    // M5: метод есть, но ни одна перегрузка не принимает
                                    // такое число аргументов.
                                    if (!resolved
                                            && !anyAcceptsArity(mit->second, argv.size(), true))
                                        addDiag(call, DEEP_ERRORS::NO_MATCHING_OVERLOAD(
                                            call->open_token, call->close_token, memb,
                                            argsTypeStringOf(argv)));
                                } else if (!sit->second.fields.count(memb)) {
                                    // M5: у структуры нет ни поля, ни метода с таким именем.
                                    addDiag(call, DEEP_ERRORS::METHOD_NOT_FOUND(
                                        ma->member_token, memb, recv.type.name));
                                }
                            }
                        }
                        dbg_name = memb;
                    } else if (callee && callee->type == NodeTypes::NodeBinaryScopeResolution) {
                        auto* sr = static_cast<struct NodeBinaryScopeResolution*>(callee);
                        Node* ln = unwrapMod(sr->l_node);
                        Node* rn = unwrapMod(sr->r_node);
                        String lname, rname;
                        if (ln && ln->type == NodeTypes::NodeVariable)
                            lname = static_cast<struct NodeVariable*>(ln)->name;
                        if (rn && rn->type == NodeTypes::NodeVariable)
                            rname = static_cast<struct NodeVariable*>(rn)->name;
                        if (!lname.empty() && !rname.empty()) {
                            auto sit = struct_syms.find(lname);
                            if (sit != struct_syms.end()) {
                                if (!sit->second.has_generics) {
                                    auto mit = sit->second.methods.find(rname);
                                    if (mit != sit->second.methods.end()) {
                                        resolved = chooseOverloadList(mit->second, argv, /*skipSelf=*/false);
                                        // M5: только instance-методы вызваны как `Struct::m()`
                                        // без ресивера.
                                        bool has_static = false;
                                        for (const auto& f : mit->second) if (f.is_static) { has_static = true; break; }
                                        if (!has_static)
                                            addDiag(call, DEEP_ERRORS::INSTANCE_METHOD_SCOPE_CALL(
                                                sr->member_token, rname, lname));
                                    }
                                }
                            } else {
                                auto it = fn_summaries.find(lname + "::" + rname);
                                if (it != fn_summaries.end())
                                    resolved = chooseOverloadList(it->second, argv, /*skipSelf=*/false);
                            }
                            dbg_name = lname + "::" + rname;
                        }
                    }
                }
                if (resolved) {
                    resolved_calls[(Node*)call] = resolved->decl;
                    if (debug_dump)
                        std::fprintf(stderr, "[types] call %s -> ret %s\n",
                                     dbg_name.to_utf8().c_str(),
                                     resolved->ret.toString().to_utf8().c_str());
                    if (resolved->ret_known) { sv.type = resolved->ret; return sv; }
                }
                sv.type = Ty::Any;
                sv.dynamic = true;
                return sv;
            }

            default:
                sv.type = Ty::Any;
                sv.dynamic = true;
                return sv;
        }
    }

    // ---------- операторы/объявления (минимально, M1) ----------
    void statement(Node* n) {
        n = unwrapScope(n);
        if (!n) return;
        // Модификаторы (const/final/strict/ref/static/public/...): снимаем
        // обёртки NodeModifier, запоминая неизменяемость (M5). Иначе объявление
        // `const let x = ...;` вообще не попало бы в область видимости.
        bool m_const = false, m_final = false;
        std::vector<String> decl_mods;
        while (n && n->type == NodeTypes::NodeModifier) {
            auto* mod = static_cast<struct NodeModifier*>(n);
            decl_mods.push_back(mod->name);
            if (mod->name == "const") m_const = true;
            else if (mod->name == "final") m_final = true;
            n = unwrapScope(mod->node);
        }
        if (!n) return;
        switch (n->type) {
            case NodeTypes::NodeStatementsScope: {
                auto* s = static_cast<struct NodeStatementsScope*>(n);
                pushScope();
                for (auto* c : s->nodes) statement(c);
                popScope();
                return;
            }
            case NodeTypes::NodeStructDeclaration: {
                // M9: анализ тел методов структуры — только ради аннотаций
                // (mod/typeof) внутри тел; диагностики подавлены, self типизирован
                // структурой. Иначе `mod(self._inner, strict)` не резолвится.
                auto* sd = static_cast<struct NodeStructDeclaration*>(n);
                Node* body = unwrapMod(sd->body);
                if (!body || body->type != NodeTypes::NodeStatementsScope) return;
                auto* ss = static_cast<struct NodeStatementsScope*>(body);
                Type self_t = Type::StructType(sd->name);
                for (Node* m : ss->nodes) {
                    Node* u = m;
                    while (u && u->type == NodeTypes::NodeModifier)
                        u = unwrapMod(static_cast<struct NodeModifier*>(u)->node);
                    u = unwrapMod(u);
                    if (!u || u->type != NodeTypes::NodeFunctionDeclaration) continue;
                    auto* fd = static_cast<struct NodeFunctionDeclaration*>(u);
                    bool saved_suppress = suppress_diag;
                    suppress_diag = true;
                    pushScope();
                    for (const auto& p : fd->params) {
                        StaticValue pv;
                        pv.type = (p.name == "self") ? self_t : typeFromTypeExpr(p.type);
                        if (pv.type.is_auto) pv.type = Ty::Any;
                        pv.mods = p.modifiers;
                        declare(p.name, pv);
                        if (hasModFlag(p.modifiers, "ref")) cur_ref_params.insert(p.name);
                    }
                    Type saved_ret = cur_ret;
                    bool saved_known = cur_ret_known;
                    std::set<String> saved_ref = cur_ref_params;
                    cur_ret = typeFromTypeExpr(fd->return_type);
                    cur_ret_known = !isAnyType(cur_ret);
                    statement(fd->body);
                    cur_ret = saved_ret;
                    cur_ret_known = saved_known;
                    cur_ref_params = saved_ref;
                    popScope();
                    suppress_diag = saved_suppress;
                }
                return;
            }
            case NodeTypes::NodeVariableDeclaration: {
                auto* d = static_cast<struct NodeVariableDeclaration*>(n);
                StaticValue v;
                bool auto_typed = false;
                if (d->type_node) {
                    v.type = typeFromTypeExpr(d->type_node);
                    auto_typed = (v.type.is_auto);
                }
                if (d->value_node) {
                    StaticValue rhs = infer(d->value_node);
                    if (!d->type_node || auto_typed) {
                        v.type = rhs.type;
                        v.is_const = rhs.is_const;
                    }
                    v.nullable = rhs.nullable;
                }
                if (v.type.is_auto) v.type = Ty::Any;
                v.immutable = m_const;
                v.final_decl = m_final;
                v.mods = decl_mods;
                declare(d->name, v);

                if (d->value_node) {
                    local_types.emplace(
                        ((unsigned long long)(unsigned int)d->name_token.position.line << 32)
                            | (unsigned int)d->name_token.position.index,
                        v.type);
                    if (auto_typed && d->type_node
                            && d->type_node->type == NodeTypes::NodeVariable) {
                        auto* tn = static_cast<struct NodeVariable*>(d->type_node);
                        int hl = tn->token.position.line;
                        int hc = tn->token.position.index + tn->token.position.size;
                        String hlabel(" > ");
                        hlabel += v.type.toString();
                        inlay_hints.push_back({hl, hc, hlabel});
                        if (debug_dump)
                            std::fprintf(stderr, "[types] let %s : %s\n",
                                         d->name.to_utf8().c_str(), v.type.toString().to_utf8().c_str());
                    }
                }
                return;
            }
            case NodeTypes::NodeFunctionDeclaration: {
                auto* fd = static_cast<struct NodeFunctionDeclaration*>(n);
                // Сигнатура уже собрана пред-проходом (collectFunctions).
                // Тело функции: параметры в области, хинты локалов внутри функций.
                pushScope();
                for (const auto& p : fd->params) {
                    StaticValue pv;
                    pv.type = typeFromTypeExpr(p.type);
                    if (pv.type.is_auto) pv.type = Ty::Any;
                    pv.mods = p.modifiers;
                    declare(p.name, pv);
                    if (hasModFlag(p.modifiers, "ref")) cur_ref_params.insert(p.name);
                }
                // M5: объявленный возврат — контекст для проверки `ret`.
                Type saved_ret = cur_ret;
                bool saved_known = cur_ret_known;
                std::set<String> saved_ref_params = cur_ref_params;
                cur_ret = typeFromTypeExpr(fd->return_type);
                cur_ret_known = !isAnyType(cur_ret);
                statement(fd->body);
                cur_ret = saved_ret;
                cur_ret_known = saved_known;
                cur_ref_params = saved_ref_params;
                popScope();
                return;
            }
            case NodeTypes::NodeIf: {
                auto* i = static_cast<struct NodeIf*>(n);
                infer(i->condition);
                std::vector<std::map<String, StaticValue>> base = scopes;
                scopes = base; pushScope(); statement(i->then_body);
                auto then_s = scopes;
                scopes = base; pushScope(); statement(i->else_body);
                auto else_s = scopes;
                scopes = base;
                mergeBranches(base, then_s, else_s);
                return;
            }
            case NodeTypes::NodeWhile: {
                auto* w = static_cast<struct NodeWhile*>(n);
                infer(w->condition);
                // M3: один проход по телу + widening (join входа и выхода) —
                // тип переменной, меняющейся в теле, безопасно расширяется.
                auto base = scopes;
                scopes = base; pushScope(); statement(w->body);
                auto after = scopes;
                scopes = base;
                mergeBranches(base, after, base);
                return;
            }
            case NodeTypes::NodeFor: {
                auto* f = static_cast<struct NodeFor*>(n);
                auto base = scopes;
                pushScope();
                statement(f->init);
                infer(f->check);
                pushScope(); statement(f->body); popScope();
                infer(f->update);
                auto after = scopes;
                scopes = base;
                mergeBranches(base, after, base);
                return;
            }
            case NodeTypes::NodeForEach: {
                auto* fe = static_cast<struct NodeForEach*>(n);
                StaticValue src = infer(fe->source);
                Type elem = Ty::Any;
                // `Any` = тип неизвестен (например, обобщённая структура, чей тип
                // не удалось вывести). Такие случаи НЕ диагностируем: сравнение
                // Type с Any всегда истинно, поэтому `src.type == Ty::Str` и т.п.
                // дают ложные срабатывания.
                const bool known = !src.type.is_any && !src.type.is_auto;
                if ((src.type.isArray() || src.type.isTuple()) && !src.type.container_1.empty())
                    elem = src.type.container_1[0];
                else if (known && src.type == Ty::Str)
                    elem = Ty::Char;
                if (fe->by_ref && known && src.type == Ty::Str) {
                    Token tk = fe->decl ? fe->decl->name_token : fe->of_token;
                    addDiag(fe, SEMANTIC_ERRORS::REF_BAD_TARGET(
                        tk, fe->decl ? fe->decl->name : String("element")));
                }
                // `ref`-цикл по обёртке с `__of__`, возвращающим контейнер ПО ЗНАЧЕНИЮ,
                // бесполезен (правка ушла бы в копию) — отклоняем, как и интерпретатор.
                if (fe->by_ref && src.type.isStruct()) {
                    auto sit = struct_syms.find(src.type.name);
                    if (sit != struct_syms.end()) {
                        auto mit = sit->second.methods.find(String(U"__of__"));
                        if (mit != sit->second.methods.end() && !mit->second.empty()
                                && mit->second[0].decl
                                && !static_cast<struct NodeFunctionDeclaration*>(mit->second[0].decl)->returns_ref) {
                            Token tk = fe->decl ? fe->decl->name_token : fe->of_token;
                            addDiag(fe, SEMANTIC_ERRORS::REF_BAD_TARGET(
                                tk, fe->decl ? fe->decl->name : String("element")));
                        }
                    }
                }
                auto base = scopes;
                pushScope();
                StaticValue ev;
                ev.type = elem;
                if (fe->decl && fe->decl->type_node) {
                    Type ann = typeFromTypeExpr(fe->decl->type_node);
                    if (!ann.is_auto) ev.type = ann;
                }
                if (fe->by_ref) ev.immutable = false;
                if (fe->decl) declare(fe->decl->name, ev);
                pushScope(); statement(fe->body); popScope();
                auto after = scopes;
                scopes = base;
                mergeBranches(base, after, base);
                return;
            }
            case NodeTypes::NodeReturn: {
                auto* r = static_cast<struct NodeReturn*>(n);
                StaticValue v = infer(r->expr);
                // M5: `ret expr` несовместим с объявленным `-> T`. Только при
                // известном типе выражения (Any не диагностируем).
                if (cur_ret_known && r->expr && !isAnyType(v.type)
                        && !(cur_ret << v.type))
                    addDiag(r, DEEP_ERRORS::RETURN_TYPE_MISMATCH(
                        r->token, cur_ret.toString(), v.type.toString()));
                return;
            }
            case NodeTypes::NodeEcho:
                infer(static_cast<struct NodeEcho*>(n)->expr);
                return;
            case NodeTypes::NodeOut: {
                auto* o = static_cast<struct NodeOut*>(n);
                for (auto* e : o->exprs) infer(e);
                return;
            }
            case NodeTypes::NodeOutln: {
                auto* o = static_cast<struct NodeOutln*>(n);
                for (auto* e : o->exprs) infer(e);
                return;
            }
            case NodeTypes::NodeAssert: {
                auto* a = static_cast<struct NodeAssert*>(n);
                infer(a->check);
                if (a->message) infer(a->message);
                return;
            }
            case NodeTypes::NodeExit:
                infer(static_cast<struct NodeExit*>(n)->expr);
                return;
            case NodeTypes::NodeDelete: {
                auto* d = static_cast<struct NodeDelete*>(n);
                StaticValue base = infer(d->expr);
                if (d->index) infer(d->index);
                // M9: `del arr, i` уменьшает длину массива-переменной на 1.
                Node* tgt = unwrapScope(d->expr);
                if (tgt && tgt->type == NodeTypes::NodeVariable
                        && base.type.isArray() && base.type.size >= 0) {
                    StaticValue nv = base;
                    nv.type = Type::ArrayType(base.type.container_1.empty() ? Ty::Any
                                                : base.type.container_1[0],
                                              base.type.size - 1);
                    assignTo(d->expr, nv);
                }
                return;
            }
            case NodeTypes::NodeAppend: {
                auto* a = static_cast<struct NodeAppend*>(n);
                infer(a->array_expr);
                infer(a->value_expr);
                return;
            }
            case NodeTypes::NodeReserve: {
                auto* r = static_cast<struct NodeReserve*>(n);
                infer(r->array_expr);
                infer(r->count_expr);
                return;
            }
            case NodeTypes::NodeEvalBlock: {
                // M7: тело eval анализируется прозрачно в текущей области (top-level
                // → объявления попадают в глобальную область). Размещение проверяет
                // парсер; исполнение/заморозка значений — M8.
                auto* ev = static_cast<struct NodeEvalBlock*>(n);
                if (ev->body) statement(ev->body);
                return;
            }
            case NodeTypes::NodeBinaryAssignment:
                // Присваивание — узел-оператор (ST), но имеет побочный эффект:
                // обновляет статический тип переменной. Прогоняем через вывод.
                infer(n);
                return;
            default:
                // Выражения-операторы (присваивания, ++/--, вызовы) прогоняем
                // через вывод типа — это обновляет статический тип переменных.
                if (n->op == EX) infer(n);
                return;
        }
    }

    void operator()() {
        debug_dump = std::getenv("LUMEN_TYPES") != nullptr;
        collectStructs(ast);     // M4: сначала структуры (типы полей/методов)
        collectFunctions(ast);   // M4: затем функции (до анализа вызовов)
        pushScope();   // глобальная область
        for (Node* node : ast) {
            if (aborted) break;
            // M8: собрать top-level eval-блоки для последующего исполнения
            // драйвером (движок интерпретатора исполнит только их). Снимаем
            // возможные обёртки-модификаторы/wrapper-скоупы.
            Node* top = unwrapScope(node);
            while (top && top->type == NodeTypes::NodeModifier)
                top = unwrapScope(static_cast<struct NodeModifier*>(top)->node);
            if (top && top->type == NodeTypes::NodeEvalBlock)
                eval_blocks.push_back(top);
            statement(node);
        }
        if (debug_dump && !scopes.empty()) {
            std::fprintf(stderr, "[types] --- globals ---\n");
            for (const auto& kv : scopes.front())
                std::fprintf(stderr, "[types]   %s : %s%s\n",
                             kv.first.to_utf8().c_str(), kv.second.type.toString().to_utf8().c_str(),
                             kv.second.dynamic ? " (dynamic)" : "");
        }
        popScope();
    }
};
