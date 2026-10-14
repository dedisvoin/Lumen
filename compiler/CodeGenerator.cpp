#include "core/Tokens.cpp"
#include "core/Walker.cpp"
#include "core/Nodes/NodeBase.cpp"
#include "core/Nodes/NodeNumber.cpp"
#include "core/Nodes/NodeUnaryExpression.cpp"
#include "core/Nodes/NodeBinaryExpression.cpp"
#include "core/Nodes/NodeScope.cpp"
#include "core/Nodes/NodeVariableDeclaration.cpp"
#include "core/Nodes/NodeVariable.cpp"
#include "core/Nodes/NodeModifier.cpp"
#include "core/Nodes/NodeBool.cpp"
#include "core/Nodes/NodeNull.cpp"
#include "core/Nodes/NodeString.cpp"
#include "core/Nodes/NodeOut.cpp"
#include "core/Nodes/NodeOutln.cpp"
#include "core/Nodes/NodeEcho.cpp"
#include "core/Nodes/NodeBinaryAssignment.cpp"
#include "core/Nodes/NodeIf.cpp"
#include "core/Nodes/NodeWhile.cpp"
#include "core/Nodes/NodeFor.cpp"
#include "core/Nodes/NodeBreak.cpp"
#include "core/Nodes/NodeContinue.cpp"
#include "core/Nodes/NodeArray.cpp"
#include "core/Nodes/NodeArrayAccess.cpp"
#include "core/Nodes/NodeSizeOf.cpp"
#include "core/Nodes/NodeCast.cpp"
#include "core/Nodes/NodeDelete.cpp"
#include "core/Nodes/NodeFunctionDeclaration.cpp"
#include "core/Nodes/NodeFunctionCall.cpp"
#include "core/Nodes/NodeNamedArgument.cpp"
#include "core/Nodes/NodeReturn.cpp"
#include "core/Nodes/NodeExit.cpp"
#include "core/Nodes/NodeAppend.cpp"
#include "core/Nodes/NodeReserve.cpp"
#include "core/Nodes/NodeLambda.cpp"


#include "core/ErrorBase.cpp"
#include "ASTGenerator.cpp"
#include "utils/File.cpp"
#include <cstdio>
#include <map>
#include <set>
#include <functional>



// ================================================================
// M0-СЌРјРёС‚С‚РµСЂ: С‚СЂР°РЅСЃРїРёР»СЏС†РёСЏ РїРѕРґРјРЅРѕР¶РµСЃС‚РІР° СЃРµРјР°РЅС‚РёРєРё all_semantic.lm в†’
// out.cpp. Р Р°РЅС‚Р°Р№Рј (boxed-СЂР°РЅС‚Р°Р№Рј) РІС‹РЅРµСЃРµРЅ РІ РѕС‚РґРµР»СЊРЅС‹Р№ Р·Р°РіРѕР»РѕРІРѕРє
// compiler/runtime/lumen_rt.h Рё РџРћР”РљР›Р®Р§РђР•РўРЎРЇ РёРЅРєР»СѓРґРѕРј (#include "lumen_rt.h"),
// Р° РЅРµ РІСЃС‚СЂР°РёРІР°РµС‚СЃСЏ РІ РєРѕРґ. РџРѕРІРµРґРµРЅРёРµ С…РµР»РїРµСЂРѕРІ Р·РµСЂРєР°Р»РёСЂСѓРµС‚ DeepAnalizer
// (binaryMath/resultTypeOf/formatValue).
// ================================================================

namespace RT_Prelude {

const String PRELUDE = "#include \"lumen_rt.h\"\n";

} // namespace RT_Prelude



struct CodeGenUnit {
    AbstractTreeT tree;

    // СЂРµРµСЃС‚СЂ С„СѓРЅРєС†РёР№: РёРјСЏ -> [РїРµСЂРµРіСЂСѓР·РєРё]
    struct FnSig {
        struct NodeFunctionDeclaration* node = nullptr;
        String cpp;        // C++-РёРјСЏ С„СѓРЅРєС†РёРё
        String ns;         // namespace (РґР»СЏ РЅРµР№РјРёРЅРіР° ns-С„РµРЅРѕРІ Рё СЂР°Р·СЂРµС€РµРЅРёСЏ ns-РїРµСЂРµРјРµРЅРЅС‹С…)
        String owner;      // РёРјСЏ СЃС‚СЂСѓРєС‚СѓСЂС‹-РІР»Р°РґРµР»СЊС†Р° (instance-РјРµС‚РѕРґС‹), "" вЂ” СЃРІРѕР±РѕРґРЅР°СЏ С„СѓРЅРєС†РёСЏ
    };
    std::map<String, std::vector<FnSig>> fnRegistry;
    std::map<String, std::vector<FnSig>> instanceRegistry; // instance-РјРµС‚РѕРґС‹: РёРјСЏ РјРµС‚РѕРґР° -> [РїРµСЂРµРіСЂСѓР·РєРё]
    std::map<String, std::vector<String>> enumMap;  // РёРјСЏ enum -> РІР°СЂРёР°РЅС‚С‹
    std::map<String, Node*> nsVars;                 // "Ns::var" -> РёРЅРёС†РёР°Р»РёР·РёСЂСѓСЋС‰Р°СЏ РЅРѕРґР°
    std::map<Node*, FnSig> lambdaMap;        // Р»СЏРјР±РґР°-РЅРѕРґР° -> С„СѓРЅРєС†РёСЏ
    std::vector<String> curParms;      // РёРјРµРЅР° РїР°СЂР°РјРµС‚СЂРѕРІ С‚РµРєСѓС‰РµР№ С„СѓРЅРєС†РёРё (C++-Р»РѕРєР°Р»СЊРЅС‹Рµ)
    std::vector<String> curRefs;       // ref-РїР°СЂР°РјРµС‚СЂС‹ С‚РµРєСѓС‰РµР№ С„СѓРЅРєС†РёРё (C++-Р»РѕРєР°Р»СЊРЅС‹Рµ СЃСЃС‹Р»РєРё)
    std::vector<String> refVars;       // Р°РєС‚РёРІРЅС‹Рµ `ref let` (Р¶РёРІСѓС‚ РІ env; РёРјРµРЅР°)
    bool inFn = false;                 // СЌРјРёСЃСЃРёСЏ С‚РµР»Р° С„СѓРЅРєС†РёРё
    bool curReturnsRef = false;        // С‚РµРєСѓС‰Р°СЏ С„СѓРЅРєС†РёСЏ РёРјРµРµС‚ `-> ref T`
    int lambdaSeq = 0;
    int capSeq = 0;                  // СЃС‡С‘С‚С‡РёРє СѓРЅРёРєР°Р»СЊРЅС‹С… РёРјС‘РЅ СЃРЅРёРјРєРѕРІ-Р·Р°С…РІР°С‚РѕРІ Р·Р°РјС‹РєР°РЅРёР№
    std::map<String, int> fnCounts;    // СЃС‡С‘С‚С‡РёРє РїРµСЂРµРіСЂСѓР·РѕРє РїРѕ РєР»СЋС‡Сѓ (СЃРІРѕР±РѕРґРЅС‹Рµ С„СѓРЅРєС†РёРё)
    std::map<String, int> methodSeq;   // СЃС‡С‘С‚С‡РёРє РїРµСЂРµРіСЂСѓР·РѕРє РјРµС‚РѕРґРѕРІ: "<Struct>::<РёРјСЏ>" -> idx
    String curNs;                      // namespace С‚РµРєСѓС‰РµР№ СЌРјРёС‚РёСЂСѓРµРјРѕР№ С„СѓРЅРєС†РёРё (РґР»СЏ ns-РїРµСЂРµРјРµРЅРЅС‹С…)
    // РґСЋРЅРґРµСЂС‹: struct -> [(dunder-РёРјСЏ, cpp, С‚РµРіРё РїР°СЂР°РјРµС‚СЂРѕРІ РїРѕСЃР»Рµ self)]. РЎРїРёСЃРѕРє: РїРµСЂРµРіСЂСѓР·РєРё;
    // С‚РµРіРё РЅСѓР¶РЅС‹ СЂР°РЅС‚Р°Р№РјСѓ, С‡С‚РѕР±С‹ РІС‹Р±СЂР°С‚СЊ РїСЂР°РІРёР»СЊРЅСѓСЋ РїРѕ С‚РёРїР°Рј Р°СЂРіСѓРјРµРЅС‚РѕРІ.
    struct DunderReg {
        String name;
        String cpp;
        std::vector<String> tags;
    };
    std::map<String, std::vector<DunderReg>> structDunders;
    // Обычные (не-дюндерные) методы: struct -> [(имя, cpp, теги параметров после self)].
    // Регистрируются в g_methods, чтобы x.method() на типе-параметре функции
    // резолвился в рантайме по типу значения (см. recvNeedsDynCall).
    std::map<String, std::vector<DunderReg>> structMethods;

    // РїРѕР»СЏ СЃС‚СЂСѓРєС‚СѓСЂС‹: РёРјСЏ СЃС‚СЂСѓРєС‚СѓСЂС‹ -> [(РёРјСЏ РїРѕР»СЏ, default_node)], РґР»СЏ РјР°С‚РµСЂРёР°Р»РёР·Р°С†РёРё
    // РґРµС„РѕР»С‚РѕРІ РІ Р»РёС‚РµСЂР°Р»Р°С… (Р·РµСЂРєР°Р»Рѕ DeepAnalizer::materializeStruct).
    std::map<String, std::vector<std::pair<String, Node*>>> structFields;
    std::map<String, std::vector<std::pair<String, Node*>>> staticFields;
    // объявленные типы полей: struct -> [(имя поля, type_node)] — для typeis(...) поля
    // на generic-структурах (сохраняем реифицированный тип на инстансе в литерале).
    std::map<String, std::vector<std::pair<String, Node*>>> structFieldTypes;

    // generic-РїР°СЂР°РјРµС‚СЂС‹ СЃС‚СЂСѓРєС‚СѓСЂС‹: РёРјСЏ СЃС‚СЂСѓРєС‚СѓСЂС‹ -> [РёРјРµРЅР° РїР°СЂР°РјРµС‚СЂРѕРІ] (РїРѕСЂСЏРґРѕРє РѕР±СЉСЏРІР»РµРЅРёСЏ).
    std::map<String, std::vector<String>> structGenerics;
    // СЃРІСЏР·С‹РІР°РЅРёРµ СЃС‚СЂСѓРєС‚СѓСЂРЅРѕРіРѕ generic-РїР°СЂР°РјРµС‚СЂР° СЃ РїРѕР»РµРј, С‚РёРїРёР·РёСЂРѕРІР°РЅРЅС‹Рј РёРј:
    // struct -> (genericName -> РїРµСЂРІРѕРµ РїРѕР»Рµ СЃ С‚Р°РєРёРј С‚РёРїРѕРј). РќСѓР¶РЅРѕ, С‡С‚РѕР±С‹ СЂР°РЅС‚Р°Р№Рј
    // РјРѕРі РІС‹РІРµСЃС‚Рё РєРѕРЅРєСЂРµС‚РЅС‹Р№ С‚РёРї `as T`/`as P` РёР· Р·РЅР°С‡РµРЅРёСЏ self.
    std::map<String, std::map<String, String>> structGenericField;
    // generic-РїСЂРёРІСЏР·РєРё С‚РµРєСѓС‰РµР№ С„СѓРЅРєС†РёРё: РёРјСЏ generic-РїР°СЂР°РјРµС‚СЂР° -> C++-Р·РЅР°С‡РµРЅРёРµ
    // rt::Box (typ): РєРѕРЅРєСЂРµС‚РёР·РёСЂСѓРµРјС‹Р№ С‚РёРї РёР· С„Р°РєС‚РёС‡РµСЃРєРѕРіРѕ Р°СЂРіСѓРјРµРЅС‚Р°/self.
    std::map<String, String> curGenericVars;
    // Функция, тело которой эмитится прямо сейчас: нужна, чтобы узнать объявленный
    // тип параметра-ресивера (x: T -> тег "generic") и решить, резолвить ли вызов
    // метода статически или в рантайме.
    struct NodeFunctionDeclaration* curFnDecl = nullptr;
    // Структура, метод которой эмитится ("" — обычная функция верхнего уровня):
    // нужна, чтобы опознать self.<field> как поле, объявленное через generic структуры.
    String curStructOwner;
    int genericSeq = 0;      // СЃС‡С‘С‚С‡РёРє СѓРЅРёРєР°Р»СЊРЅС‹С… C++-РёРјС‘РЅ РїСЂРёРІСЏР·РѕРє generic-РїР°СЂР°РјРµС‚СЂРѕРІ

    bool isCurRef(const String& n) const {
        for (const String& r : curRefs) if (r == n) return true;
        return false;
    }
    bool isParam(const String& n) const {
        for (const String& p : curParms) if (p == n) return true;
        return false;
    }

    // C++-РІС‹СЂР°Р¶РµРЅРёРµ: rt::Box СЃРѕ Р·РЅР°С‡РµРЅРёРµРј Type (typ-Р±РѕРєСЃ) РґР»СЏ generic-РїР°СЂР°РјРµС‚СЂР° g
    // С‚РµРєСѓС‰РµР№ С„СѓРЅРєС†РёРё. РџСЂР°РІРёР»Р° СЃРІСЏР·С‹РІР°РЅРёСЏ (self РёРґС‘С‚ РІ РїРѕР·РёС†РёРё 0):
    //   a: T            -> С‚РёРї СЃР°РјРѕРіРѕ Р°СЂРіСѓРјРµРЅС‚Р°
    //   a: Root[T]      -> С‚РёРї "СЃР»РѕС‚Р°" T СЃС‚СЂСѓРєС‚СѓСЂС‹ Root: РїРѕР»Рµ Root, С‚РёРїРёР·РёСЂРѕРІР°РЅРЅРѕРµ
    //                      struct-РіРµРЅРµСЂР°Р№Р·РµСЂРѕРј СЃ С‚РѕР№ Р¶Рµ РїРѕР·РёС†РёРё (Root[T] -> РїРѕР»Рµ x)
    // Р’РѕР·РІСЂР°С‰Р°РµС‚ "" РµСЃР»Рё РїР°СЂР°РјРµС‚СЂ РЅРµ РЅР°Р№РґРµРЅ (РєР°СЃС‚ РѕСЃС‚Р°С‘С‚СЃСЏ С‚РѕР¶РґРµСЃС‚РІРµРЅРЅС‹Рј).
    String genericArgCpp(const String& g, struct NodeFunctionDeclaration* fn, bool has_self = false) {
        for (size_t i = has_self ? 1 : 0; i < fn->params.size(); ++i) {
            Node* t = fn->params[i].type;
            while (t && t->type == NodeTypes::NodeScope)
                t = static_cast<struct NodeScope*>(t)->node;
            if (!t) continue;
            if (t->type == NodeTypes::NodeVariable) {
                if (static_cast<struct NodeVariable*>(t)->name == g)
                    return "rt::Box::typ(rt::tyname(" + fn->params[i].name + "))";
                continue;
            }
            if (t->type == NodeTypes::NodeArrayAccess) {
                auto* aa = static_cast<struct NodeArrayAccess*>(t);
                Node* base = aa->base;
                while (base && base->type == NodeTypes::NodeScope)
                    base = static_cast<struct NodeScope*>(base)->node;
                if (!base || base->type != NodeTypes::NodeVariable) continue;
                String root = static_cast<struct NodeVariable*>(base)->name;
                auto git = structGenerics.find(root);
                if (git == structGenerics.end()) continue;
                for (size_t j = 0; j < aa->indices.size() && j < git->second.size(); ++j) {
                    Node* gx = aa->indices[j];
                    while (gx && gx->type == NodeTypes::NodeScope)
                        gx = static_cast<struct NodeScope*>(gx)->node;
                    if (!gx || gx->type != NodeTypes::NodeVariable) continue;
                    if (static_cast<struct NodeVariable*>(gx)->name != g) continue;
                    String sg = git->second[j];
                    auto fit = structGenericField.find(root);
                    if (fit == structGenericField.end()) continue;
                    auto gf = fit->second.find(sg);
                    if (gf == fit->second.end()) continue;
                    return "rt::Box::typ(rt::tyname(rt::field(" + fn->params[i].name
                         + ", " + quote(gf->second) + ")))";
                }
            }
        }
        return String();
    }
    bool isRefVar(const String& n) const {
        for (const String& r : refVars) if (r == n) return true;
        return false;
    }

    // скрытые generic-value аргументы вызова: дописываются ПОСЛЕ обычных параметров,
    // позиционно соответствуют fn->generics. Выводимые (неуказанные) генерики передаём
    // null-заглушкой — каллируемый выводит значение из параметров сам. Это позволяет
    // рантайму узнать значение генерика там, где вывод из параметров невозможен
    // (ноль-арг `CompileTime::from[VT]()`, генерик только в return-типе).
    String genericHiddenArgs(struct NodeFunctionDeclaration* fn, struct NodeFunctionCall* n) {
        if (fn->generics.empty()) return String();
        String s;
        for (size_t gi = 0; gi < fn->generics.size(); ++gi) {
            if (gi) s += ", ";
            if (gi < n->generics.size() && n->generics[gi])
                s += emitExpr(n->generics[gi]);
            else
                s += "rt::Box::null()";
        }
        return s;
    }
    // refSourceBase: РІС‹СЂР°Р¶РµРЅРёРµ Box& РґР»СЏ СЏС‡РµР№РєРё-РЅРѕСЃРёС‚РµР»СЏ РїРµСЂРµРјРµРЅРЅРѕР№ (С†РµР»СЊ СЃСѓС‰РµСЃС‚РІСѓСЋС‰РёС… ref).
    // "" РµСЃР»Рё СѓР·РµР» РЅРµ СЏРІР»СЏРµС‚СЃСЏ РїРµСЂРµРјРµРЅРЅРѕР№-РёСЃС‚РѕС‡РЅРёРєРѕРј.
    String refSourceBase(Node* base) {
        while (base && base->type == NodeTypes::NodeScope)
            base = static_cast<struct NodeScope*>(base)->node;
        if (!base || base->type != NodeTypes::NodeVariable) return String();
        auto* v = static_cast<struct NodeVariable*>(base);
        if (isCurRef(v->name) && isParam(v->name)) return "*rt::refaddr(" + v->name + ")";
        if (isRefVar(v->name)) return "*rt::refaddr(" + envVar() + ".get(" + quote(v->name) + "))";
        if (isParam(v->name)) return v->name;
        return envVar() + ".get(" + quote(v->name) + ")";
    }

    // Р–РёРІРѕР№ Р°Р»РёР°СЃ РґР»СЏ ref let x = <РёСЃС‚РѕС‡РЅРёРє>: РїСѓС‚СЊ obj.a.b, ref-of-ref, РїРµСЂРµРјРµРЅРЅР°СЏ/РїР°СЂР°РјРµС‚СЂ.
    // "" -> СЃРЅРёРјРѕРє Р·РЅР°С‡РµРЅРёРµРј (РґР»СЏ Р°РґСЂРµСЃР°С‚РѕРІ СЃ РЅРµСЃС‚Р°Р±РёР»СЊРЅРѕР№ СЏС‡РµР№РєРѕР№ РёР»Рё РІС‹Р·РѕРІРѕРІ).
    // refSourceCell: shared_ptr<Box> РЅР° СЏС‡РµР№РєСѓ-РЅРѕСЃРёС‚РµР»СЊ РєРѕРЅС‚РµР№РЅРµСЂР° (РґР»СЏ element-Р°Р»РёР°СЃРѕРІ a[i]).
    // Р Р°Р·СЂРµС€Р°РµС‚СЃСЏ РЅР° РєР°Р¶РґС‹Р№ РґРѕСЃС‚СѓРї: Р±РµР·РѕРїР°СЃРЅРѕ РїСЂРё РїРµСЂРµРЅР°Р·РЅР°С‡РµРЅРёРё РєРѕРЅС‚РµР№РЅРµСЂР°. "" РµСЃР»Рё РЅРµ РїРµСЂРµРјРµРЅРЅР°СЏ.
    String refSourceCell(Node* base) {
        while (base && base->type == NodeTypes::NodeScope)
            base = static_cast<struct NodeScope*>(base)->node;
        if (!base || base->type != NodeTypes::NodeVariable) return String();
        auto* v = static_cast<struct NodeVariable*>(base);
        if (isCurRef(v->name) && isParam(v->name)) return v->name + ".sp";
        if (isRefVar(v->name)) return "rt::refcell_ptr(*rt::refaddr(" + envVar() + ".get(" + quote(v->name) + ")))";
        if (isParam(v->name)) return "rt::refcell_ptr(" + v->name + ")";
        return "rt::refcell_ptr(" + envVar() + ".get(" + quote(v->name) + "))";
    }
    String refSourceAlias(Node* src) {
        while (src && src->type == NodeTypes::NodeScope)
            src = static_cast<struct NodeScope*>(src)->node;
        if (!src) return String();
        if (src->type == NodeTypes::NodeMemberAccess) {
            std::vector<String> path;
            Node* cur = src;
            while (cur && cur->type == NodeTypes::NodeMemberAccess) {
                auto* ma = static_cast<struct NodeMemberAccess*>(cur);
                Node* rr = ma->r_node;
                while (rr && rr->type == NodeTypes::NodeScope)
                    rr = static_cast<struct NodeScope*>(rr)->node;
                if (!rr || rr->type != NodeTypes::NodeVariable) return String();
                path.insert(path.begin(), static_cast<struct NodeVariable*>(rr)->name);
                cur = ma->l_node;
            }
            String base = refSourceBase(cur);
            if (base.empty() || path.empty()) return String();
            String list;
            for (auto& p : path) { if (!list.empty()) list += ", "; list += quote(p); }
            return "rt::refpath(" + base + ", {" + list + "})";
        }
        if (src->type == NodeTypes::NodeArrayAccess) {
            auto* aa = static_cast<struct NodeArrayAccess*>(src);
            Node* ab = aa->base;
            if (aa->indices.size() == 1) {
                String cell = refSourceCell(ab);
                if (!cell.empty())
                    return "rt::refindex_cell(" + cell + ", " + emitExpr(aa->indices[0]) + ")";
            }
            return String();
        }        if (src->type == NodeTypes::NodeVariable) {
            auto* v = static_cast<struct NodeVariable*>(src);
            if (isCurRef(v->name) && isParam(v->name)) return "rt::refalias(" + v->name + ")";
            if (isRefVar(v->name)) return "rt::refalias(" + envVar() + ".get(" + quote(v->name) + "))";
            if (isParam(v->name)) return "rt::Box::refcell(" + v->name + ")";
            return "rt::env_ref(" + envVar() + ", " + quote(v->name) + ")";
        }
        return String();
    }
    static bool argIsRef(const FuncArg& a) {
        for (const String& m : a.modifiers) if (m == String(U"ref")) return true;
        return false;
    }

    // бит модификатора для рантайм-проверки `mod(refParam, m)` (совпадает с lumen_rt.h)
    static long long modBit(const String& name) {
        if (name == String(U"ref"))     return 1LL << 0;
        if (name == String(U"static"))  return 1LL << 1;
        if (name == String(U"const"))   return 1LL << 2;
        if (name == String(U"final"))   return 1LL << 3;
        if (name == String(U"strict"))  return 1LL << 4;
        if (name == String(U"private")) return 1LL << 5;
        if (name == String(U"public"))  return 1LL << 6;
        return 0;
    }

    // Box&-ссылка на ячейку-носитель append-цели (зеркало writePathValue-путей):
    // переменная, параметр, путь полей, Ns::member, *p, a[i]... и их комбинации.
    String appendLvalueRef(Node* n) {
        while (n && n->type == NodeTypes::NodeScope)
            n = static_cast<struct NodeScope*>(n)->node;
        if (!n) return String();
        if (n->type == NodeTypes::NodeVariable) {
            String nm = static_cast<struct NodeVariable*>(n)->name;
            if (isParam(nm) || isCurRef(nm)) return nm;
            return envVar() + ".get(" + quote(nm) + ")";
        }
        if (n->type == NodeTypes::NodeUnaryDereference) {
            Node* p = static_cast<struct NodeUnaryDereference*>(n)->node;
            return "*rt::box_addr(" + emitExpr(p) + ")";
        }
        if (n->type == NodeTypes::NodeBinaryScopeResolution) {
            auto* sr = static_cast<struct NodeBinaryScopeResolution*>(n);
            Node* ll = sr->l_node;
            while (ll && ll->type == NodeTypes::NodeScope)
                ll = static_cast<struct NodeScope*>(ll)->node;
            Node* rr = sr->r_node;
            while (rr && rr->type == NodeTypes::NodeScope)
                rr = static_cast<struct NodeScope*>(rr)->node;
            if (ll && ll->type == NodeTypes::NodeVariable
                && rr && rr->type == NodeTypes::NodeVariable) {
                String key = static_cast<struct NodeVariable*>(ll)->name + "::"
                           + static_cast<struct NodeVariable*>(rr)->name;
                if (nsVars.find(key) != nsVars.end())
                    return envVar() + ".get(" + quote(key) + ")";
            }
            return String();
        }
        if (n->type == NodeTypes::NodeMemberAccess) {
            std::vector<String> names;
            Node* cur = n;
            while (cur && cur->type == NodeTypes::NodeMemberAccess) {
                auto* ma = static_cast<struct NodeMemberAccess*>(cur);
                Node* rr = ma->r_node;
                while (rr && rr->type == NodeTypes::NodeScope)
                    rr = static_cast<struct NodeScope*>(rr)->node;
                if (!rr || rr->type != NodeTypes::NodeVariable) return String();
                names.insert(names.begin(), static_cast<struct NodeVariable*>(rr)->name);
                cur = ma->l_node;
            }
            String base = appendLvalueRef(cur);
            if (base.empty()) return String();
            if (names.empty()) return base;
            String list;
            for (auto& p : names) { if (!list.empty()) list += ", "; list += quote(p); }
            return "*rt::fieldpath(" + base + ", {" + list + "})";
        }
        if (n->type == NodeTypes::NodeArrayAccess) {
            auto* aa = static_cast<struct NodeArrayAccess*>(n);
            String cell = appendLvalueRef(aa->base);
            if (cell.empty()) return String();
            for (auto* idx : aa->indices)
                cell = "*rt::arr_cell(" + cell + ", " + emitExpr(idx) + ")";
            return cell;
        }
        return String();
    }

    // &obj.a.b -> rt::fieldptr(base, {"a","b"}): указатель на ПУТЬ (владелец + имена
    // полей), а не на готовую ячейку std::map. Иначе переприсваивание владельца
    // (`s = p{...}`) оставляло бы указатель в отброшенной структуре: чтение давало
    // старое значение, а запись через него терялась. Интерпретатор резолвит путь
    // заново при каждом обращении - здесь тот же контракт.
    String appendFieldPtr(Node* n) {
        while (n && (n->type == NodeTypes::NodeScope
                  || n->type == NodeTypes::NodePostfixUnwrap))
            n = static_cast<struct NodeScope*>(n)->node;
        if (!n || n->type != NodeTypes::NodeMemberAccess) return String();
        std::vector<String> names;
        Node* cur = n;
        while (cur && cur->type == NodeTypes::NodeMemberAccess) {
            auto* ma = static_cast<struct NodeMemberAccess*>(cur);
            Node* rr = ma->r_node;
            while (rr && rr->type == NodeTypes::NodeScope)
                rr = static_cast<struct NodeScope*>(rr)->node;
            if (!rr || rr->type != NodeTypes::NodeVariable) return String();
            names.insert(names.begin(), static_cast<struct NodeVariable*>(rr)->name);
            cur = ma->l_node;
        }
        String base = appendLvalueRef(cur);
        if (base.empty() || names.empty()) return String();
        String list;
        for (auto& p : names) { if (!list.empty()) list += ", "; list += quote(p); }
        return "rt::fieldptr(" + base + ", {" + list + "})";
    }

CodeGenUnit(AbstractTreeT tree, bool dbg = false,
            const std::vector<String>& cpp_includes = std::vector<String>(),
            const std::map<Node*, Node*>* resolved_calls = nullptr) noexcept
        : tree(tree), dbg(dbg), cpp_includes(cpp_includes),
          resolved_calls(resolved_calls) {}

    // Абсолютные пути C++-файлов из #cppinclude (эмитятся #include-директивами)
    std::vector<String> cpp_includes;

    // Решения резолва перегрузок от deep-анализатора: узел вызова -> узел
    // объявления выбранной функции. null, если deep-проход не запускался (-noanalize).
    // Приоритетный источник для resolveForCall/resolveForMethod: он вычислен на
    // настоящих Type, поэтому union-параметры (Numeric и любые объявленные синонимы)
    // резолвятся так же, как в интерпретаторе.
    const std::map<Node*, Node*>* resolved_calls = nullptr;

    bool dbg = false;                 // -d: РїРµС‡Р°С‚Р°С‚СЊ СЃРІРѕРґРєСѓ РєРѕРґРѕРіРµРЅРµСЂР°С†РёРё
    int tbdCount = 0;                 // С‡РёСЃР»Рѕ РЅРµРѕР±СЂР°Р±РѕС‚Р°РЅРЅС‹С… СѓР·Р»РѕРІ (TBD-Р·Р°РіР»СѓС€РµРє)
    int envDefineCount = 0;           // С‡РёСЃР»Рѕ РѕРїСЂРµРґРµР»РµРЅРёР№ РїРµСЂРµРјРµРЅРЅС‹С… РІ env
    long long codeBytes = 0;          // СЂР°Р·РјРµСЂ СЃРіРµРЅРµСЂРёСЂРѕРІР°РЅРЅРѕРіРѕ C++

    // РіРґРµ Р¶РёРІСѓС‚ РїРµСЂРµРјРµРЅРЅС‹Рµ: РІ С‚РµР»Рµ С„СѓРЅРєС†РёРё вЂ” СЃРІРѕР№ Ctx (РєР°Р¶РґС‹Р№ РІС‹Р·РѕРІ СЃРѕР·РґР°С‘С‚ РЅРѕРІС‹Р№),
    // РІ main вЂ” РіР»РѕР±Р°Р»СЊРЅС‹Р№ rt::g_ctx
    String envVar() const { return inFn ? String("ctx") : String("rt::g_ctx"); }

    // Есть ли у структуры с именем sn статическое поле fn? staticFields индексирован
    // по имени структуры, а резолвится полный ключ "Struct::field".
    bool isStructStatic(const String& key) const {
        size_t p = key.rfind(String(U"::"));
        if (p == std::u32string::npos) return false;
        String sn = key.substr(0, p);
        String fn = key.substr(p + 2);
        auto it = staticFields.find(sn);
        if (it == staticFields.end()) return false;
        for (size_t i = 0; i < it->second.size(); ++i)
            if (it->second[i].first == fn) return true;
        return false;
    }

    // Статические поля структуры (`static let x = ...` в теле `struct`) — это
    // значения уровня типа, а НЕ поля экземпляра: экземпляр структуры не
    // существует, а имя структуры в рантайме указывает на Type-объект, поэтому
    // `rt::field(g_ctx.get("Test"), "a")` всегда даёт null. Храним их в том же
    // контексте под ключом "Struct::field", что и переменные namespace'а
    // ("Ns::var"), — тогда `Test::a` резолвится обычным ctx.get().
    String emitStructStatics(Node* n, const String& nsPrefix) {
        Node* u = unwrapMod(n);
        if (!u) return String();
        if (u->type == NodeTypes::NodeStructDeclaration) {
            auto* st = static_cast<struct NodeStructDeclaration*>(u);
            String full = nsPrefix.empty() ? st->name : nsPrefix + String("::") + st->name;
            auto it = staticFields.find(full);
            if (it == staticFields.end()) return String();
            String s;
            for (size_t i = 0; i < it->second.size(); ++i) {
                String key = full + String("::") + it->second[i].first;
                s += envVar() + ".define(" + quote(key) + ", "
                   + (it->second[i].second ? emitExpr(it->second[i].second)
                                           : String("rt::Box::null()"))
                   + ");\n";
            }
            return s;
        }
        if (u->type == NodeTypes::NodeStatementsScope) {
            String s;
            auto* scope = static_cast<struct NodeStatementsScope*>(u);
            for (size_t i = 0; i < scope->nodes.size(); ++i)
                s += emitStructStatics(scope->nodes[i], nsPrefix);
            return s;
        }
        return String();
    }

    // cpp-РёРјСЏ РёР· scope: "::" -> "_" (Point::origin -> Point_origin)
    static String cppName(const String& scope) {
        String s;
        for (auto c : scope) s += (c == U':') ? U'_' : c;
        return s;
    }

    // Полное имя левой части `::`: для вложенного scope-resolution собирает
    // "A::B::C". Пустая строка — если слева не имя/цепочка имён (напр. вызов),
    // тогда вызывающий уходит в ветку rt::field(...).
    String scopeLeftName(Node* n) {
        while (n && n->type == NodeTypes::NodeScope)
            n = static_cast<struct NodeScope*>(n)->node;
        if (!n) return String();
        if (n->type == NodeTypes::NodeVariable)
            return static_cast<struct NodeVariable*>(n)->name;
        if (n->type == NodeTypes::NodeBinaryScopeResolution) {
            auto* sr = static_cast<struct NodeBinaryScopeResolution*>(n);
            String l = scopeLeftName(sr->l_node);
            String r = scopeLeftName(sr->r_node);
            if (l.empty()) return r;
            if (r.empty()) return l;
            return l + String("::") + r;
        }
        return String();
    }

    static String escapeString(const String& s) {
        String out;
        for (size_t i = 0; i < s.size(); ++i) {
            char32_t ch = s[i];
            if (ch == U'\\') out += "\\\\";
            else if (ch == U'"') out += "\\\"";
            else if (ch == U'\n') out += "\\n";
            else if (ch == U'\r') out += "\\r";
            else if (ch == U'\t') out += "\\t";
            else out += ch;
        }
        return out;
    }

    static String nodeName(Node* node) {
        if (!node) return "null";
        return String(/* РЅР°Р·РІР°РЅРёРµ РЅРѕРґС‹ РїРѕ enum */);
    }

    static String floatLiteral(long double value) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", static_cast<double>(value));
        return String(buf);
    }

    // ------------------- РІС‹СЂР°Р¶РµРЅРёСЏ -------------------

    String emitArrayElement(Node* el) {
        while (el && el->type == NodeTypes::NodeScope)
            el = static_cast<struct NodeScope*>(el)->node;
        if (el && el->type == NodeTypes::NodeVariable) {
            auto* v = static_cast<struct NodeVariable*>(el);
            if (isCurRef(v->name) && isParam(v->name))
                return "rt::refalias(" + v->name + ")";
            if (isRefVar(v->name))
                return "rt::refalias(" + envVar() + ".get(" + quote(v->name) + "))";
        }
        return emitExpr(el);
    }

    // Числовой литерал (`NodeNumber`) — возможно, под оболочкой скопа.
    static bool isNumericLiteralNode(Node* n) {
        while (n && n->type == NodeTypes::NodeScope)
            n = static_cast<struct NodeScope*>(n)->node;
        return n && n->type == NodeTypes::NodeNumber;
    }

    static bool isFloatLiteral(Node* n) {
        while (n && n->type == NodeTypes::NodeScope)
            n = static_cast<struct NodeScope*>(n)->node;
        return n && n->type == NodeTypes::NodeNumber
            && !static_cast<struct NodeNumber*>(n)->is_integer;
    }

    // Тег rt::Box для числового имени типа (`i32` -> "T_i32"); "" если не число.
    static String numericTagOf(Node* t) {
        while (t && t->type == NodeTypes::NodeScope)
            t = static_cast<struct NodeScope*>(t)->node;
        if (!t || t->type != NodeTypes::NodeVariable) return "";
        const String& nm = static_cast<struct NodeVariable*>(t)->name;
        if (nm == "i8")  return "Box::T_i8";
        if (nm == "i16") return "Box::T_i16";
        if (nm == "i32") return "Box::T_i32";
        if (nm == "i64") return "Box::T_i64";
        if (nm == "f16") return "Box::T_f16";
        if (nm == "f32") return "Box::T_f32";
        if (nm == "f64") return "Box::T_f64";
        return "";
    }

    String emitExpr(Node* node) {
        if (!node) return "rt::Box::null()";
        if (dbg) std::cout << "         " << Style("+ ", TerminalStyle::BRIGHT_GREEN) << "[cg::expr] " << nodeName(node->type) << std::endl;
        switch (node->type) {
            case NodeTypes::NodeNumber: {
                auto* n = static_cast<struct NodeNumber*>(node);
                if (n->is_integer) return "rt::Box::i64(" + String(std::to_string(n->int_value)) + ")";
                return "rt::Box::f64(" + floatLiteral(n->float_value) + ")";
            }
            case NodeTypes::NodeBool: {
                auto* n = static_cast<struct NodeBool*>(node);
                return n->value ? "rt::Box::boo(true)" : "rt::Box::boo(false)";
            }
            case NodeTypes::NodeNull:
                return "rt::Box::null()";
            case NodeTypes::NodeString: {
                auto* n = static_cast<struct NodeString*>(node);
                if (n->is_char && n->value.size() == 1) {
                    // char хранит полную кодовую точку Unicode — эмитим число,
                    // чтобы не зависеть от кодировки исходника (например 'я').
                    char32_t ch = n->value[0];
                    return "rt::Box::chr(" + String(std::to_string(static_cast<unsigned long long>(ch))) + ")";
                }
                return "rt::Box::str(\"" + escapeString(n->value) + "\")";
            }
            case NodeTypes::NodeVariable: {
                auto* v = static_cast<struct NodeVariable*>(node);
                auto git = curGenericVars.find(v->name);
                if (git != curGenericVars.end()) return git->second;
                if (isCurRef(v->name)) return "rt::derefval(" + v->name + ")";  // ref-РїР°СЂР°РјРµС‚СЂ (C++-Р»РѕРєР°Р»СЊ)
                for (const String& p : curParms)
                    if (p == v->name) return v->name;      // C++-Р»РѕРєР°Р»СЊ (РїР°СЂР°РјРµС‚СЂ С„СѓРЅРєС†РёРё)
                if (isRefVar(v->name))
                    return "rt::derefval(" + envVar() + ".get(" + quote(v->name) + "))";
                if (!curNs.empty()) {
                    String full = curNs + "::" + v->name;
                    if (nsVars.find(full) != nsVars.end())
                        return envVar() + ".get(" + quote(full) + ")";
                }
                auto it = fnRegistry.find(v->name);
                if (it != fnRegistry.end() && !it->second.empty())
                    return "rt::Box::fn(&" + it->second[0].cpp + ")";  // С„СѓРЅРєС†РёСЏ РєР°Рє Р·РЅР°С‡РµРЅРёРµ
                // встроенные типы: в интерпретаторе предопределены как переменные,
                // в транспилере эмитятся как тип-значения (Box::typ).
                static const char* builtinTypes[] = {
                    "i8","i16","i32","i64","f16","f32","f64","bool","char","str",
                    "any","auto","Type","Null","Namespace"
                };
                String uname = v->name;
                for (const char* bt : builtinTypes)
                    if (uname == String(bt)) return "rt::Box::typ(" + quote(v->name) + ")";
                return envVar() + ".get(" + quote(v->name) + ")";
            }
            case NodeTypes::NodeFunctionCall: {
                auto* n = static_cast<struct NodeFunctionCall*>(node);
                String r = buildCall(n);
                if (r.empty()) return "/* TBD call */ rt::Box::null()";
                return r;
            }
            case NodeTypes::NodeLambda: {
                auto it = lambdaMap.find(node);
                if (it == lambdaMap.end()) return "rt::Box::null()";
                return "rt::Box::fn(&" + it->second.cpp + ")";
            }
            case NodeTypes::NodeScope:
                return emitExpr(static_cast<struct NodeScope*>(node)->node);
            case NodeTypes::NodeUnaryMinus:
                return "rt::unary_dispatch('-', " + envVar() + ", "
                     + emitExpr(static_cast<struct NodeUnaryMinus*>(node)->node) + ")";
            case NodeTypes::NodeUnaryPlus:
                return "rt::unary_dispatch('+', " + envVar() + ", "
                     + emitExpr(static_cast<struct NodeUnaryPlus*>(node)->node) + ")";
            case NodeTypes::NodeBinaryAdditive:
                return binOf('+', static_cast<struct NodeBinaryAdditive*>(node)->l_node,
                                   static_cast<struct NodeBinaryAdditive*>(node)->r_node);
            case NodeTypes::NodeBinarySubtraction:
                return binOf('-', static_cast<struct NodeBinarySubtraction*>(node)->l_node,
                                   static_cast<struct NodeBinarySubtraction*>(node)->r_node);
            case NodeTypes::NodeBinaryMultiplicative:
                return binOf('*', static_cast<struct NodeBinaryMultiplicative*>(node)->l_node,
                                   static_cast<struct NodeBinaryMultiplicative*>(node)->r_node);
            case NodeTypes::NodeBinaryDivision:
                return binOf('/', static_cast<struct NodeBinaryDivision*>(node)->l_node,
                                   static_cast<struct NodeBinaryDivision*>(node)->r_node);
            case NodeTypes::NodeBinaryRemainder:
                return binOf('%', static_cast<struct NodeBinaryRemainder*>(node)->l_node,
                                   static_cast<struct NodeBinaryRemainder*>(node)->r_node);
            case NodeTypes::NodeBinaryEqual:
                return cmpOf('e', static_cast<struct NodeBinaryEqual*>(node)->l_node,
                                    static_cast<struct NodeBinaryEqual*>(node)->r_node);
            case NodeTypes::NodeBinaryNotEqual:
                return cmpOf('n', static_cast<struct NodeBinaryNotEqual*>(node)->l_node,
                                    static_cast<struct NodeBinaryNotEqual*>(node)->r_node);
            case NodeTypes::NodeBinaryLess:
                return cmpOf('<', static_cast<struct NodeBinaryLess*>(node)->l_node,
                                    static_cast<struct NodeBinaryLess*>(node)->r_node);
            case NodeTypes::NodeBinaryGreater:
                return cmpOf('>', static_cast<struct NodeBinaryGreater*>(node)->l_node,
                                    static_cast<struct NodeBinaryGreater*>(node)->r_node);
            case NodeTypes::NodeBinaryLessEqual:
                return cmpOf('L', static_cast<struct NodeBinaryLessEqual*>(node)->l_node,
                                    static_cast<struct NodeBinaryLessEqual*>(node)->r_node);
            case NodeTypes::NodeBinaryGreaterEqual:
                return cmpOf('G', static_cast<struct NodeBinaryGreaterEqual*>(node)->l_node,
                                    static_cast<struct NodeBinaryGreaterEqual*>(node)->r_node);
            case NodeTypes::NodeBinaryLogicalAnd:
                return "rt::land(" + emitExpr(static_cast<struct NodeBinaryLogicalAnd*>(node)->l_node)
                     + ", " + emitExpr(static_cast<struct NodeBinaryLogicalAnd*>(node)->r_node) + ")";
            case NodeTypes::NodeBinaryLogicalOr:
                return "rt::lor(" + emitExpr(static_cast<struct NodeBinaryLogicalOr*>(node)->l_node)
                     + ", " + emitExpr(static_cast<struct NodeBinaryLogicalOr*>(node)->r_node) + ")";
            case NodeTypes::NodeBinaryNullCoalesce:
                return "rt::coalesce(" + emitExpr(static_cast<struct NodeBinaryNullCoalesce*>(node)->l_node)
                     + ", " + emitExpr(static_cast<struct NodeBinaryNullCoalesce*>(node)->r_node) + ")";
            case NodeTypes::NodeUnaryNegation: {
                auto* un = static_cast<struct NodeUnaryNegation*>(node);
                if (un->token.value == "?") {
                    return "rt::typnullable(" + emitExpr(un->node) + ")";
                }
                return "rt::unary_dispatch('!', " + envVar() + ", " + emitExpr(un->node) + ")";
            }
            case NodeTypes::NodeUnaryDereference:
                return "rt::deref(" + emitExpr(static_cast<struct NodeUnaryDereference*>(node)->node) + ")";
            case NodeTypes::NodeUnaryAddressOf: {
                // &var → указатель на живой ячейку. Для ref-параметра (C++-локальная
                // T_REF Box) и ref-переменной (`ref let x = t;`) целевой является *sp той
                // же ячейки, иначе refptr(derefval(x)) — вырожденная копия (нельзя связать).
                // Для &obj.field путь полей — уже lvalue (*rt::fieldpath(...)), поэтому
                // refptr() получает настоящий слот, а не временную копию.
                Node* ao = static_cast<struct NodeUnaryAddressOf*>(node)->node;
                while (ao && (ao->type == NodeTypes::NodeScope
                           || ao->type == NodeTypes::NodePostfixUnwrap))
                    ao = static_cast<struct NodeScope*>(ao)->node;
                if (ao && ao->type == NodeTypes::NodeMemberAccess) {
                    String fp = appendFieldPtr(ao);
                    if (!fp.empty()) return fp;
                }
                if (ao && ao->type == NodeTypes::NodeVariable) {
                    String an = static_cast<struct NodeVariable*>(ao)->name;
                    if (isCurRef(an))
                        return "rt::refptr(*" + an + ".sp)";
                    if (isRefVar(an))
                        return "rt::refptr(*" + envVar() + ".get(" + quote(an) + ").sp)";
                }
                String slot = appendLvalueRef(ao);
                if (!slot.empty()) return "rt::refptr(" + slot + ")";
                return "rt::refptr(" + emitExpr(static_cast<struct NodeUnaryAddressOf*>(node)->node) + ")";
            }
            case NodeTypes::NodeUnaryPostfixIncrement:
            case NodeTypes::NodeUnaryPostfixDecrement:
            case NodeTypes::NodeUnaryPrefixIncrement:
            case NodeTypes::NodeUnaryPrefixDecrement: {
                auto* n = static_cast<struct NodeUnaryPostfixIncrement*>(node);
                bool is_inc = (node->type == NodeTypes::NodeUnaryPostfixIncrement
                            || node->type == NodeTypes::NodeUnaryPrefixIncrement);
                bool is_prefix = (node->type == NodeTypes::NodeUnaryPrefixIncrement
                              || node->type == NodeTypes::NodeUnaryPrefixDecrement);
                Node* operand = n->node;
                while (operand && operand->type == NodeTypes::NodeScope)
                    operand = static_cast<struct NodeScope*>(operand)->node;
                String delta = String("rt::op_dispatch('") + (is_inc ? "+" : "-")
                             + String("', ") + envVar() + ", "
                             + emitExpr(operand) + ", rt::Box::i64(1))";
                if (operand && operand->type == NodeTypes::NodeVariable) {
                    String nm = static_cast<struct NodeVariable*>(operand)->name;
                    String cell;
                    if (isCurRef(nm) || isParam(nm)) cell = nm;
                    else cell = envVar() + ".get(" + quote(nm) + ")";
                    bool is_ref = (isRefVar(nm) || isCurRef(nm));
                    String read = is_ref ? ("rt::derefval(" + cell + ")") : cell;
                    String op = is_inc ? String("+") : String("-");
                    String delta = "rt::op_dispatch('" + op + "', " + envVar()
                                 + ", " + read + ", rt::Box::i64(1))";
                    String write;
                    if (is_ref)
                        write = "rt::setref_dispatch(" + envVar() + ", " + cell + ", " + delta + ")";
                    else
                        write = "rt::assign_dispatch(" + envVar() + ", " + cell + ", " + delta + ")";
                    if (is_prefix)
                        // РїСЂРµС„РёРєСЃРЅС‹Р№: РїСЂРёСЃРІР°РёРІР°РЅРёРµ РєР°Рє РІС‹СЂР°Р¶РµРЅРёРµ РІРѕР·РІСЂР°С‰Р°РµС‚ РЅРѕРІРѕРµ Р·РЅР°С‡РµРЅРёРµ
                        return String("(") + write + ")";
                    // РїРѕСЃС‚С„РёРєСЃРЅС‹Р№: СЃРѕС…СЂР°РЅСЏРµРј СЃС‚Р°СЂРѕРµ Р”Рћ Р·Р°РїРёСЃРё
                    return String("([&]() -> rt::Box { ") + "rt::Box __lo = " + read + "; "
                         + write + "; return __lo; }())";
                }
                // РќР•-РїРµСЂРµРјРµРЅРЅР°СЏ (member-access Рё РїСЂ.): Р·РЅР°С‡РµРЅРёРµ-РґРѕРІРµСЃРѕРє РѕС‚Р±СЂР°СЃС‹РІР°РµРј
                // (СЃРµРјР°РЅС‚РёРєР° Р·РЅР°С‡РµРЅРёСЏ РїРѕСЃС‚С„РёРєСЃР° РґР»СЏ С‡Р»РµРЅРѕРІ РЅРµ РіР°СЂР°РЅС‚РёСЂСѓРµС‚СЃСЏ)
                String lval = emitExpr(n->node);
                return String("(") + lval + " = " + delta + ")";
            }
            // постфиксный ? : rt::auto_get вызывает __get__, если он есть, и
            // возвращает само значение без изменений, если метода нет.
            case NodeTypes::NodePostfixUnwrap: {
                auto* n = static_cast<struct NodePostfixUnwrap*>(node);
                return "rt::auto_get(" + envVar() + ", " + emitExpr(n->node) + ")";
            }
            case NodeTypes::NodePtr:
                return "rt::Box::ptr(" + emitExpr(static_cast<struct NodePtr*>(node)->addr) + ")";
            case NodeTypes::NodeArray: {
                auto* n = static_cast<struct NodeArray*>(node);
                String s = "rt::Box::arr(std::vector<rt::Box>{";
                for (size_t i = 0; i < n->elements.size(); ++i) {
                    if (i) s += ", ";
                    s += emitArrayElement(n->elements[i]);
                }
                s += "})";
                return s;
            }
            case NodeTypes::NodeArrayAccess: {
                auto* n = static_cast<struct NodeArrayAccess*>(node);
                String s = emitExpr(n->base);
                for (auto* idx : n->indices)
                    s = "rt::index_dispatch(" + envVar() + ", " + s + ", " + emitExpr(idx) + ")";
                return s;
            }
            case NodeTypes::NodeSizeOf: {
                auto* n = static_cast<struct NodeSizeOf*>(node);
                return "rt::szof_dispatch(" + envVar() + ", " + emitExpr(n->expr) + ")";
            }
            case NodeTypes::NodeInput: {
                auto* n = static_cast<struct NodeInput*>(node);
                if (n->prompt)
                    return "rt::input(" + emitExpr(n->prompt) + ")";
                return "rt::input(rt::Box::str(\"\"))";
            }
            case NodeTypes::NodeTypeOf: {
                auto* n = static_cast<struct NodeTypeOf*>(node);
                if (n->has_static_type && !n->static_type_ambiguous)
                    // typeof/typeis оба дают type-значение (T_TYPE): так `typeof(x) == i64`
                    // сравнивается с типовым литералом (Box::typ), а не со строкой.
                    return "rt::Box::typ(" + quote(n->static_type) + ")";
                // typeis поля на generic-структуре: узел неоднозначен (разные инстансы),
                // но объявленный тип сохранён на самом инстансе -> читаем его в рантайме.
                if (n->is_static) {
                    Node* e = n->expr;
                    while (e && e->type == NodeTypes::NodeScope)
                        e = static_cast<struct NodeScope*>(e)->node;
                    if (e && e->type == NodeTypes::NodeMemberAccess) {
                        auto* ma = static_cast<struct NodeMemberAccess*>(e);
                        Node* r = ma->r_node;
                        while (r && r->type == NodeTypes::NodeScope)
                            r = static_cast<struct NodeScope*>(r)->node;
                        if (r && r->type == NodeTypes::NodeVariable)
                            return "rt::field_decltype(" + emitExpr(ma->l_node) + ", "
                                 + quote(static_cast<struct NodeVariable*>(r)->name) + ")";
                    }
                }
                return "rt::typeof_dispatch(" + envVar() + ", " + emitExpr(n->expr) + ")";
            }
            case NodeTypes::NodeMod: {
                auto* n = static_cast<struct NodeMod*>(node);
                // ref-параметр: модификаторы принадлежат аргументу и переносятся
                // значением (rt::Box::mods) из места вызова — читаем их в рантайме.
                if (n->dynamic) {
                    Node* e = n->expr;
                    while (e && e->type == NodeTypes::NodeScope)
                        e = static_cast<struct NodeScope*>(e)->node;
                    if (e && e->type == NodeTypes::NodeVariable) {
                        String vn = static_cast<struct NodeVariable*>(e)->name;
                        return "rt::Box::boo(rt::mod_has(" + vn + ", "
                             + String(std::to_string(modBit(n->modifier))) + "LL))";
                    }
                }
                // модификатор — свойство компиляционного времени; анализатор
                // сохранил результат в узле, здесь выдаём константу.
                return n->value ? "rt::Box::boo(true)" : "rt::Box::boo(false)";
            }
            case NodeTypes::NodeUnaryBitwiseNot:
                return "rt::unary_dispatch('~', " + envVar() + ", "
                     + emitExpr(static_cast<struct NodeUnaryBitwiseNot*>(node)->node) + ")";
            case NodeTypes::NodeCast: {
                auto* n = static_cast<struct NodeCast*>(node);
                Node* tgt = n->r_node;
                while (tgt && tgt->type == NodeTypes::NodeScope)
                    tgt = static_cast<struct NodeScope*>(tgt)->node;
                if (tgt && tgt->type == NodeTypes::NodeVariable) {
                    String tn = static_cast<struct NodeVariable*>(tgt)->name;
                    // Р¦РµР»СЊ: 1) generic-РїСЂРёРІСЏР·РєР° С‚РµРєСѓС‰РµР№ С„СѓРЅРєС†РёРё/СЃС‚СЂСѓРєС‚СѓСЂС‹ (`as T`);
                    // 2) РїР°СЂР°РјРµС‚СЂ/РїРµСЂРµРјРµРЅРЅР°СЏ СЃРѕ Р·РЅР°С‡РµРЅРёРµРј Type (`type: Type`) вЂ” Р±РµСЂС‘Рј
                    // С‚РёРї РёР· СЂР°РЅС‚Р°Р№Рј-Р·РЅР°С‡РµРЅРёСЏ; 3) РёРЅР°С‡Рµ вЂ” СЃС‚Р°С‚РёС‡РµСЃРєРѕРµ РёРјСЏ С‚РёРїР°.
                    String targetExpr;
                    auto git = curGenericVars.find(tn);
                    if (git != curGenericVars.end())
                        targetExpr = git->second;
                    else if (isParam(tn))
                        targetExpr = tn;   // параметр — C++-локальная, несущая значение Type
                    else
                        targetExpr = String("rt::Box::typ(") + quote(tn) + ")";
                    return "rt::cast_dispatch(" + envVar() + ", " + emitExpr(n->l_node) + ", " + targetExpr + ")";
                }
                return "/* TBD cast target */ rt::Box::null()";
            }
            case NodeTypes::NodeIf: {
                auto* n = static_cast<struct NodeIf*>(node);
                String s = "[&]() -> rt::Box {";
                s += "\n    if (rt::truthy(" + envVar() + ", " + emitExpr(n->condition) + ")) {";
                s += "\n        " + branchValue(n->then_body);
                s += "\n    } else {";
                s += "\n        " + branchValue(n->else_body);
                s += "\n    }";
                s += "\n    return rt::Box::null();";
                s += "\n}()";
                return s;
            }
            case NodeTypes::NodeBinaryBitwiseAnd:
                return binOf('&', static_cast<struct NodeBinaryBitwiseAnd*>(node)->l_node,
                                   static_cast<struct NodeBinaryBitwiseAnd*>(node)->r_node);
            case NodeTypes::NodeBinaryBitwiseXor:
                return binOf('^', static_cast<struct NodeBinaryBitwiseXor*>(node)->l_node,
                                   static_cast<struct NodeBinaryBitwiseXor*>(node)->r_node);
            case NodeTypes::NodeBinaryBitwiseOr:
                return binOf('|', static_cast<struct NodeBinaryBitwiseOr*>(node)->l_node,
                                   static_cast<struct NodeBinaryBitwiseOr*>(node)->r_node);
            case NodeTypes::NodeBinaryShiftLeft:
                return binOf('l', static_cast<struct NodeBinaryShiftLeft*>(node)->l_node,
                                   static_cast<struct NodeBinaryShiftLeft*>(node)->r_node);
            case NodeTypes::NodeBinaryShiftRight:
                return binOf('r', static_cast<struct NodeBinaryShiftRight*>(node)->l_node,
                                   static_cast<struct NodeBinaryShiftRight*>(node)->r_node);
            case NodeTypes::NodeMemberAccess: {
                auto* n = static_cast<struct NodeMemberAccess*>(node);
                Node* r = n->r_node;
                while (r && r->type == NodeTypes::NodeScope)
                    r = static_cast<struct NodeScope*>(r)->node;
                String member;
                if (r && r->type == NodeTypes::NodeVariable)
                    member = static_cast<struct NodeVariable*>(r)->name;
                else
                    member = String(U"?");
                return "rt::field(" + emitExpr(n->l_node) + ", " + quote(member) + ")";
            }
            case NodeTypes::NodeBinaryScopeResolution: {
                auto* n = static_cast<struct NodeBinaryScopeResolution*>(node);
                // левая часть может быть цепочкой (`N::E::B`): собираем полное имя
                String lname = scopeLeftName(n->l_node);
                Node* r = n->r_node;
                while (r && r->type == NodeTypes::NodeScope)
                    r = static_cast<struct NodeScope*>(r)->node;
                String rname;
                if (r && r->type == NodeTypes::NodeVariable)
                    rname = static_cast<struct NodeVariable*>(r)->name;
                String key = lname + "::" + rname;
                auto ei = enumMap.find(lname);
                if (ei != enumMap.end()) {
                    for (size_t i = 0; i < ei->second.size(); ++i)
                        if (ei->second[i] == rname)
                            return "rt::Box::i64(" + String(std::to_string(i)) + ")";
                    return "rt::Box::i64(-1)";
                }
                auto fi = fnRegistry.find(key);
                if (fi != fnRegistry.end() && !fi->second.empty())
                    return "rt::Box::fn(&" + fi->second[0].cpp + ")";
                // С‡С‚РµРЅРёРµ namespace-РїРµСЂРµРјРµРЅРЅРѕР№: Ns::var
                if (nsVars.find(key) != nsVars.end())
                    return envVar() + ".get(" + quote(key) + ")";
                // С‡Р»РµРЅ-РґРѕСЃС‚СѓРї Рє namespace-Р·РЅР°С‡РµРЅРёСЋ (namespace-Р»РёС‚РµСЂР°Р» РІ РїРµСЂРµРјРµРЅРЅРѕР№): `my_str::testificate`
                if (isStructStatic(key))
                    return envVar() + ".get(" + quote(key) + ")";
                if (!rname.empty())
                    return "rt::field(" + emitExpr(n->l_node) + ", " + quote(rname) + ")";
                return "/* TBD scope res */ rt::Box::null()";
            }
            case NodeTypes::NodeStructLiteral: {
                auto* n = static_cast<struct NodeStructLiteral*>(node);
                String tn = "?";
                Node* te = n->type_expr;
                while (te && te->type == NodeTypes::NodeScope)
                    te = static_cast<struct NodeScope*>(te)->node;
                if (te && te->type == NodeTypes::NodeVariable)
                    tn = static_cast<struct NodeVariable*>(te)->name;
                // Имя, вычисленное анализатором: covers `mk(){…}`,
                // `get_struct(i64){…}` и структуры, объявленные в функции
                // (в type_expr там не имя, а выражение-вызов).
                if (!n->struct_name.empty()) tn = n->struct_name;
                String s = "rt::Box::strct(" + quote(tn) + ", std::map<std::string, rt::Box>{";
                // СѓР¶Рµ Р·Р°РґР°РЅРЅС‹Рµ РёРЅРёС†РёР°Р»РёР·Р°С‚РѕСЂР°РјРё РїРѕР»СЏ
                std::map<String, String> inited;
                for (size_t i = 0; i < n->inits.size(); ++i) {
                    if (i) s += ", ";
                    String val = emitExpr(n->inits[i].value);
                    s += "{ " + quote(n->inits[i].name) + ", " + val + " }";
                    inited[n->inits[i].name] = val;
                }
                // РјР°С‚РµСЂРёР°Р»РёР·Р°С†РёСЏ РґРµС„РѕР»С‚РЅС‹С… РїРѕР»РµР№ (Р·РµСЂРєР°Р»Рѕ DeepAnalizer::materializeStruct):
                // РїСЂРѕРїСѓСЃРєР°РµРј СѓР¶Рµ Р·Р°РґР°РЅРЅС‹Рµ Рё РґРµС„РѕР»С‚С‹, РЅРµ СЌРјРёС‚РёСЂСѓРµРјС‹Рµ РєРѕРґРѕРіРµРЅРµСЂР°С‚РѕСЂРѕРј (TBD-Р·Р°РіР»СѓС€РєРё)
                // bind the literal's generic args so field defaults referencing
                // struct generics (`type: Type = T`) reify to their values instead
                // of a runtime ctx.get("T") lookup (which is null at runtime).
                std::map<String, String> genBind;
                auto sgit = structGenerics.find(tn);
                if (sgit != structGenerics.end())
                    for (size_t gi = 0; gi < n->generics.size() && gi < sgit->second.size(); ++gi) {
                        const String& gname = sgit->second[gi];
                        String gexpr = emitExpr(n->generics[gi]);
                        genBind[gname] = gexpr;
                    }
                std::map<String, String> savedGen = curGenericVars;
                for (auto& gb : genBind) curGenericVars[gb.first] = gb.second;
                size_t cnt = n->inits.size();
                auto it = structFields.find(tn);
                if (it != structFields.end()) {
                    for (auto& f : it->second) {
                        if (inited.find(f.first) != inited.end()) continue;
                        if (!f.second) continue;
                        String val = emitExpr(f.second);
                        if (val.find(String(U"/* TBD")) != std::u32string::npos) continue;
                        if (cnt) s += ", ";
                        s += "{ " + quote(f.first) + ", " + val + " }";
                        ++cnt;
                    }
                }
                // Объявленные типы generic-полей (для typeis(...) поля): вычисляем
                // реифицированный тип поля в момент создания инстанса (curGenericVars
                // уже содержит тип-аргументы литерала) и сохраняем его в структуре.
                String decls;
                {
                    std::vector<String> stg;
                    auto sgit2 = structGenerics.find(tn);
                    if (sgit2 != structGenerics.end()) stg = sgit2->second;
                    auto ftit = structFieldTypes.find(tn);
                    if (ftit != structFieldTypes.end() && !stg.empty()) {
                        for (auto& fp : ftit->second) {
                            if (!fp.second) continue;
                            bool usesGeneric = false;
                            collectGenericTypeNames(fp.second, [&](const String& gn) {
                                for (auto& gg : stg) if (gg == gn) { usesGeneric = true; break; }
                            });
                            if (!usesGeneric) continue;
                            String val = emitExpr(fp.second);
                            if (val.find(String(U"/* TBD")) != std::u32string::npos) continue;
                            if (!decls.empty()) decls += ", ";
                            decls += "{ " + quote(fp.first) + ", rt::typename_of(" + val + ") }";
                        }
                    }
                }
                curGenericVars = savedGen;
                s += "}";
                if (!decls.empty())
                    s += ", std::map<std::string, std::string>{" + decls + "}";
                s += ")";
                return s;
            }
            case NodeTypes::NodeNew: {
                auto* n = static_cast<struct NodeNew*>(node);
                if (!n->value) return "rt::Box::null()";
                return "rt::make_new(" + emitExpr(n->value) + ")";
            }
            case NodeTypes::NodeNamespace: {
                auto* nsp = static_cast<struct NodeNamespace*>(node);
                Node* b = nsp->body;
                while (b && b->type == NodeTypes::NodeModifier)
                    b = static_cast<struct NodeModifier*>(b)->node;
                String s = "rt::nslit(" + envVar() + ", " + quote(String("namespace"))
                         + ", std::map<std::string, rt::Box>{";
                bool first = true;
                if (b && b->type == NodeTypes::NodeStatementsScope) {
                    for (auto* m : static_cast<struct NodeStatementsScope*>(b)->nodes) {
                        Node* u = m;
                        while (u && u->type == NodeTypes::NodeModifier)
                            u = static_cast<struct NodeModifier*>(u)->node;
                        if (!u) continue;
                        if (u->type == NodeTypes::NodeVariableDeclaration) {
                            auto* vd = static_cast<struct NodeVariableDeclaration*>(u);
                            if (!first) s += ", ";
                            s += "{ " + quote(vd->name) + ", "
                               + (vd->value_node ? emitExpr(vd->value_node) : "rt::Box::null()") + " }";
                            first = false;
                        } else if (u->type == NodeTypes::NodeNamespace) {
                            // вложенный namespace — член внешнего (зеркало live-контекста
                            // интерпретатора): значение строится рекурсивно тем же nslit.
                            auto* inner = static_cast<struct NodeNamespace*>(u);
                            if (inner->name.empty()) continue;
                            if (!first) s += ", ";
                            s += "{ " + quote(inner->name) + ", " + emitExpr(u) + " }";
                            first = false;
                        }
                    }
                }
                // анонимный namespace наследует видимую область (интерпретатор: parent-цепочка):
                // параметры текущей функции живут как C++-локали и в ctx не входят — захватываем их
                s += "}, std::map<std::string, rt::Box>{";
                bool cfirst = true;
                for (const String& p : curParms) {
                    if (!cfirst) s += ", ";
                    s += "{ " + quote(p) + ", "
                       + (isCurRef(p) ? "rt::derefval(" + p + ")" : p) + " }";
                    cfirst = false;
                }
                s += "})";
                return s;
            }
            case NodeTypes::NodeTupleType: {
                auto* n = static_cast<struct NodeTupleType*>(node);
                String s = "rt::Box::typ(std::string(\"Tuple[\")";
                for (size_t i = 0; i < n->types.size(); ++i) {
                    s += " + " + typeText(n->types[i]);
                    if (i + 1 == n->types.size()) s += " + \"]\"";
                    else s += " + \", \"";
                }
                s += ")";
                return s;
            }
            case NodeTypes::NodeArrayType: {
                auto* n = static_cast<struct NodeArrayType*>(node);
                if (!n->has_body)
                    return "rt::Box::typ(std::string(\"Array[\") + " + typeText(n->element_type) + " + \"]\")";
                // `Array[T, N]{}` — пустой массив (длина 0), элементы не создаются.
                if (n->items.empty())
                    return "rt::Box::arr(std::vector<rt::Box>{})";
                // Числовой литерал на месте элемента приводится к объявленному
                // типу контейнера: `Array[i32, 3]{1, 2, 3}` хранит i32, а не i64
                // (иначе тип контейнера врал бы относительно содержимого).
                String lit_tag = numericTagOf(n->element_type);
                String s = "rt::Box::arr(std::vector<rt::Box>{";
                for (size_t i = 0; i < n->items.size(); ++i) {
                    if (i) s += ", ";
                    String el = emitArrayElement(n->items[i]);
                    if (!lit_tag.empty() && isNumericLiteralNode(n->items[i])
                        && isFloatLiteral(n->items[i]))
                        el = "rt::make_f(rt::" + lit_tag + ", " + el + ".f)";
                    else if (!lit_tag.empty() && isNumericLiteralNode(n->items[i]))
                        el = "rt::make_i(rt::" + lit_tag + ", (long long)(" + el + ").i)";
                    s += el;
                }
                s += "})";
                return s;
            }
            case NodeTypes::NodeFunctionType: {
                auto* n = static_cast<struct NodeFunctionType*>(node);
                String s = "rt::Box::typ(std::string(\"Func[\")";
                for (size_t i = 0; i < n->params.size(); ++i) {
                    if (i) s += " + \", \"";
                    s += " + " + typeText(n->params[i]);
                }
                s += " + \"] -> \" + " + typeText(n->return_type) + ")";
                return s;
            }
            default:
                if (dbg) tbdCount++;
                return "/* TBD expr " + typeName(node->type) + " */ rt::Box::null()";
        }
    }

    String binOf(char op, Node* l, Node* r) {
        String s = "rt::op_dispatch('";
        s += op;
        s += "', ";
        s += envVar();
        s += ", ";
        s += emitExpr(l);
        s += ", ";
        s += emitExpr(r);
        s += ")";
        return s;
    }

    String cmpOf(char op, Node* l, Node* r) {
        String s = "rt::op_dispatch('";
        s += op;
        s += "', ";
        s += envVar();
        s += ", ";
        s += emitExpr(l);
        s += ", ";
        s += emitExpr(r);
        s += ")";
        return s;
    }

    // СЃРїРѕСЃРѕР±РµРЅ Р»Рё СѓР·РµР» Р±С‹С‚СЊ C++-РІС‹СЂР°Р¶РµРЅРёРµРј (РґР»СЏ РІРµС‚РѕРє if-РІС‹СЂР°Р¶РµРЅРёСЏ Рё С€Р°РіРѕРІ for)
    static bool isValueNode(Node* n) {
        if (!n) return false;
        switch (n->type) {
            case NodeTypes::NodeNumber:
            case NodeTypes::NodeString:
            case NodeTypes::NodeBool:
            case NodeTypes::NodeNull:
            case NodeTypes::NodeVariable:
            case NodeTypes::NodeBinaryAdditive:
            case NodeTypes::NodeBinarySubtraction:
            case NodeTypes::NodeBinaryMultiplicative:
            case NodeTypes::NodeBinaryDivision:
            case NodeTypes::NodeBinaryRemainder:
            case NodeTypes::NodeBinaryEqual:
            case NodeTypes::NodeBinaryNotEqual:
            case NodeTypes::NodeBinaryLess:
            case NodeTypes::NodeBinaryGreater:
            case NodeTypes::NodeBinaryLessEqual:
            case NodeTypes::NodeBinaryGreaterEqual:
            case NodeTypes::NodeBinaryLogicalAnd:
            case NodeTypes::NodeBinaryLogicalOr:
            case NodeTypes::NodeBinaryNullCoalesce:
            case NodeTypes::NodeUnaryMinus:
            case NodeTypes::NodeUnaryPlus:
            case NodeTypes::NodeUnaryNegation:
            case NodeTypes::NodeUnaryDereference:
            case NodeTypes::NodeUnaryAddressOf:
            case NodeTypes::NodePtr:
            case NodeTypes::NodeArray:
            case NodeTypes::NodeArrayAccess:
            case NodeTypes::NodeSizeOf:
            case NodeTypes::NodeMod:
            case NodeTypes::NodeCast:
            case NodeTypes::NodeFunctionCall:
            case NodeTypes::NodeLambda:
            case NodeTypes::NodeIf:
            case NodeTypes::NodeScope:
            case NodeTypes::NodeMemberAccess:
            case NodeTypes::NodeBinaryScopeResolution:
            case NodeTypes::NodeStructLiteral:
            case NodeTypes::NodeNew:
                return true;
            default:
                return false;
        }
    }

    // Р·РЅР°С‡РµРЅРёРµ РІРµС‚РєРё (РїРѕСЃР»РµРґРЅСЏСЏ РЅРѕРґР° СЃРєРѕСѓРїР° вЂ” РІС‹СЂР°Р¶РµРЅРёРµ): `return <expr>;`
    String branchValue(Node* node) {
        if (!node) return "return rt::Box::null(); // РІРѕР·РІСЂР°С‚ Р±РµР· Р·РЅР°С‡РµРЅРёСЏ";
        if (node->type == NodeTypes::NodeScope)
            return branchValue(static_cast<struct NodeScope*>(node)->node);
        if (node->type == NodeTypes::NodeStatementsScope) {
            auto* sc = static_cast<struct NodeStatementsScope*>(node);
            if (sc->nodes.empty()) return "return rt::Box::null(); // РІРѕР·РІСЂР°С‚ Р±РµР· Р·РЅР°С‡РµРЅРёСЏ";
            String s;
            for (size_t i = 0; i + 1 < sc->nodes.size(); ++i) {
                String st = emitStmt(sc->nodes[i]);
                if (!st.empty()) { s += st; s += "\n"; }
            }
            Node* last = sc->nodes.back();
            if (isValueNode(last)) {
                s += "return ";
                s += emitExpr(last);
                s += ";";
            } else {
                String st = emitStmt(last);
                if (!st.empty()) { s += st; s += "\n"; }
                s += "return rt::Box::null();";
            }
            return s;
        }
        if (isValueNode(node)) {
            return "return " + emitExpr(node) + ";";
        }
        String st = emitStmt(node);
        if (!st.empty()) st += "\n";
        return st + "return rt::Box::null();";
    }

    // init/step РІ for: СѓР·РµР»-РґРµРєР»Р°СЂР°С†РёСЋ РёР»Рё РїСЂРёСЃРІР°РёРІР°РЅРёРµ РїСЂРµРІСЂР°С‰Р°РµРј РІ C++-РІС‹СЂР°Р¶РµРЅРёРµ
    String emitInitOrStep(Node* n) {
        if (!n) return String();
        while (n->type == NodeTypes::NodeScope)
            n = static_cast<struct NodeScope*>(n)->node;
        if (n->type == NodeTypes::NodeModifier)
            return emitInitOrStep(static_cast<struct NodeModifier*>(n)->node);
        if (n->type == NodeTypes::NodeVariableDeclaration) {
            auto* v = static_cast<struct NodeVariableDeclaration*>(n);
            return envVar() + ".define(" + quote(v->name) + ", "
                 + (v->value_node ? emitExpr(v->value_node) : "rt::Box::null()") + ")";
        }
        if (n->type == NodeTypes::NodeBinaryAssignment) {
            auto* a = static_cast<struct NodeBinaryAssignment*>(n);
            Node* l = a->l_node;
            while (l && l->type == NodeTypes::NodeScope)
                l = static_cast<struct NodeScope*>(l)->node;
            if (l && l->type == NodeTypes::NodeVariable)
                return envVar() + ".get(" + quote(static_cast<struct NodeVariable*>(l)->name)
                     + ") = " + emitExpr(a->r_node);
            return "/* TBD forstep */ rt::Box::null()";
        }
        String s = emitStmt(n);
        if (s.size() >= 2 && s[s.size() - 1] == U';') s = s.substr(0, s.size() - 1);
        return s;
    }

    static String quote(const String& s) {
        return "\"" + escapeString(s) + "\"";
    }

    // ------------------- statements -------------------

    // Statement-набор namespace: определить само значение и его членов.
    // Вложенные namespace обрабатываются рекурсивно (ключ "outer::inner::...").
    String emitNamespaceStmt(struct NodeNamespace* nsp, const String& full) {
        String s;
        if (!full.empty())
            s += envVar() + ".define(" + quote(full) + ", " + emitExpr(nsp)
               + "); // namespace-значение " + full + "\n";
        Node* b = nsp->body;
        while (b && b->type == NodeTypes::NodeModifier)
            b = static_cast<struct NodeModifier*>(b)->node;
        if (b && b->type == NodeTypes::NodeStatementsScope) {
            for (auto* m : static_cast<struct NodeStatementsScope*>(b)->nodes) {
                Node* u = m;
                while (u && u->type == NodeTypes::NodeModifier)
                    u = static_cast<struct NodeModifier*>(u)->node;
                if (!u) continue;
                if (u->type == NodeTypes::NodeVariableDeclaration) {
                    auto* vd = static_cast<struct NodeVariableDeclaration*>(u);
                    s += envVar() + ".define(" + quote(full + "::" + vd->name) + ", "
                       + (vd->value_node ? emitExpr(vd->value_node) : "rt::Box::null()")
                       + ");\n";
                } else if (u->type == NodeTypes::NodeStructDeclaration) {
                    // структура внутри namespace: static-поля живут
                    // под ключом "Ns::Struct::field"
                    s += emitStructStatics(u, full);
                } else if (u->type == NodeTypes::NodeNamespace) {
                    auto* inner = static_cast<struct NodeNamespace*>(u);
                    if (inner->name.empty()) continue;
                    s += emitNamespaceStmt(inner, full + "::" + inner->name);
                }
            }
        }
        return s;
    }

    String emitStmt(Node* node) {
        if (!node) return "";
        if (dbg) std::cout << "         " << Style("+ ", TerminalStyle::BRIGHT_GREEN) << "[cg::stmt] " << nodeName(node->type) << std::endl;
        switch (node->type) {
            case NodeTypes::NodeModifier: {
                auto* mod = static_cast<struct NodeModifier*>(node);
                if (mod->name == String(U"ref")) {
                    Node* inner = mod->node;
                    while (inner && inner->type == NodeTypes::NodeScope)
                        inner = static_cast<struct NodeScope*>(inner)->node;
                    if (inner && inner->type == NodeTypes::NodeVariableDeclaration) {
                        auto* vd = static_cast<struct NodeVariableDeclaration*>(inner);
                        Node* src = vd->value_node;
                        while (src && src->type == NodeTypes::NodeScope)
                            src = static_cast<struct NodeScope*>(src)->node;
                        String expr;
                        if (src) expr = refSourceAlias(src);
                        if (expr.empty() && src && src->type == NodeTypes::NodeArrayAccess) {
                            auto* aa = static_cast<struct NodeArrayAccess*>(src);
                            Node* ab = aa->base;
                            String base = refSourceCell(ab);
                            if (!base.empty() && aa->indices.size() == 1)
                                expr = "rt::refindex_cell(" + base + ", " + emitExpr(aa->indices[0]) + ")";
                        }
                        String s = envVar() + ".define(" + quote(vd->name) + ", ";
                        if (!expr.empty()) s += expr + ");";
                        else if (src) s += emitExpr(src) + ");";
                        else s += "rt::Box::null());";
                        refVars.push_back(vd->name);
                        return s;
                    }
                }return emitStmt(mod->node);
            }
            case NodeTypes::NodeStatementsScope: {
                auto* sc = static_cast<struct NodeStatementsScope*>(node);
                String s;
                for (auto* child : sc->nodes) {
                    String cs = emitStmt(child);
                    if (!cs.empty()) { s += cs; s += "\n"; }
                }
                return s;
            }
            case NodeTypes::NodeVariableDeclaration: {
                auto* n = static_cast<struct NodeVariableDeclaration*>(node);
                String s;
                // namespace-значение (`let X = namespace { ... }`): переменные тела кладём в env
                // под "X::var" — их читают ns-функции тела (аналог ветки NodeNamespace),
                // сама X остаётся struct-значением.
                Node* nv = n->value_node;
                while (nv && nv->type == NodeTypes::NodeScope)
                    nv = static_cast<struct NodeScope*>(nv)->node;
                nv = unwrapMod(nv);
                if (nv && nv->type == NodeTypes::NodeNamespace) {
                    Node* nsb = unwrapMod(static_cast<struct NodeNamespace*>(nv)->body);
                    if (nsb && nsb->type == NodeTypes::NodeStatementsScope) {
                        for (auto* m : static_cast<struct NodeStatementsScope*>(nsb)->nodes) {
                            Node* u = m;
                            if (u && u->type == NodeTypes::NodeModifier)
                                u = static_cast<struct NodeModifier*>(u)->node;
                            u = unwrapMod(u);
                            if (!u || u->type != NodeTypes::NodeVariableDeclaration) continue;
                            auto* inner = static_cast<struct NodeVariableDeclaration*>(u);
                            String key = n->name + "::" + inner->name;
                            s += envVar() + ".define(" + quote(key) + ", "
                               + (inner->value_node ? emitExpr(inner->value_node) : "rt::Box::null()")
                               + "); // ns-переменная namespace-значения\n";
                        }
                    }
                }
                s += envVar() + ".define(";
                s += quote(n->name);
                s += ", ";
                s += n->value_node ? emitExpr(n->value_node) : "rt::Box::null()";
                s += "); // объявление переменной " + n->name;
                if (dbg) envDefineCount++;
                return s;
            }
            case NodeTypes::NodeFunctionDeclaration:
                // top-level С„СѓРЅРєС†РёРё СЂРµРіРёСЃС‚СЂРёСЂСѓСЋС‚СЃСЏ РІ РїСЂРµРїР°СЃСЃРµ Рё СЌРјРёС‚СЏС‚СЃСЏ РѕС‚РґРµР»СЊРЅРѕ;
                // РІР»РѕР¶РµРЅРЅС‹Рµ вЂ” Р·Р°РјС‹РєР°РЅРёРµ РІ env (Р»РµРєСЃ-Р·Р°С…РІР°С‚ РїРѕ СЃРЅРёРјРєСѓ РєРѕРЅС‚РµРєСЃС‚Р°)
                return inFn ? emitNestedFn(static_cast<struct NodeFunctionDeclaration*>(node)) : "";
            case NodeTypes::NodeBinaryAssignment: {
                auto* n = static_cast<struct NodeBinaryAssignment*>(node);
                Node* l = n->l_node;
                while (l && l->type == NodeTypes::NodeScope)
                    l = static_cast<struct NodeScope*>(l)->node;
                // каноническая форма `a += b` / `a = a + b` (RHS-бинарь с тем же LHS)
                // → in-place rt::arr_plus_assign для обычной переменной-массива
                struct NodeBinaryAdditive* add = nullptr;
                Node* rw = n->r_node;
                while (rw && rw->type == NodeTypes::NodeScope)
                    rw = static_cast<struct NodeScope*>(rw)->node;
                if (rw && rw->type == NodeTypes::NodeBinaryAdditive) {
                    Node* all = static_cast<struct NodeBinaryAdditive*>(rw)->l_node;
                    while (all && all->type == NodeTypes::NodeScope)
                        all = static_cast<struct NodeScope*>(all)->node;
                    if (all == l)
                        add = static_cast<struct NodeBinaryAdditive*>(rw);
                }
                String rhs = emitExpr(n->r_node);
                if (l && l->type == NodeTypes::NodeVariable) {
                    String nm = static_cast<struct NodeVariable*>(l)->name;
                    if (isCurRef(nm))
                        return "rt::setref_dispatch(" + envVar() + ", " + nm + ", " + rhs + ");";
                    if (isRefVar(nm))
                        return "rt::setref_dispatch(" + envVar() + ", " + envVar() + ".get(" + quote(nm) + "), " + rhs + "); // присваивание";
                    if (isParam(nm))
                        return "rt::assign_dispatch(" + envVar() + ", " + nm + ", " + rhs + "); // присваивание";
                    if (add)
                        return "rt::arr_plus_assign(" + envVar() + ", " + envVar() + ".get("
                             + quote(nm) + "), " + emitExpr(add->r_node) + "); // вливание массива";
                    return "rt::assign_dispatch(" + envVar() + ", " + envVar() + ".get("
                         + quote(nm) + "), " + rhs + "); // присваивание";
                }
                if (l && l->type == NodeTypes::NodeMemberAccess) {
                    auto* ma = static_cast<struct NodeMemberAccess*>(l);
                    Node* mr = ma->r_node;
                    while (mr && mr->type == NodeTypes::NodeScope)
                        mr = static_cast<struct NodeScope*>(mr)->node;
                    String member;
                    if (mr && mr->type == NodeTypes::NodeVariable)
                        member = static_cast<struct NodeVariable*>(mr)->name;
                    return "rt::putf_dispatch(" + envVar() + ", " + emitExpr(ma->l_node) + ", "
                         + quote(member) + ", " + rhs + "); // присваивание поля";
                }
                if (l && l->type == NodeTypes::NodeFunctionCall) {
                    // Р·Р°РїРёСЃСЊ РІ РІРѕР·РІСЂР°С‰С‘РЅРЅСѓСЋ СЃСЃС‹Р»РєСѓ: slot_get() = ...
                    // lf_...() РІРѕР·РІСЂР°С‰Р°РµС‚ T_REF Box (РІСЂРµРјРµРЅРЅС‹Р№); СЂР°Р·С‹РјРµРЅРѕРІС‹РІР°РµРј .sp РЅР°РїСЂСЏРјСѓСЋ
                    String callExpr = buildCall(static_cast<struct NodeFunctionCall*>(l));
                    return "rt::setref_call(" + callExpr + ", " + rhs + ");";
                }
                if (l && l->type == NodeTypes::NodeUnaryDereference) {
                    // *p = v вЂ” Р·Р°РїРёСЃСЊ С‡РµСЂРµР· СѓРєР°Р·Р°С‚РµР»СЊ
                    Node* p = static_cast<struct NodeUnaryDereference*>(l)->node;
                    while (p && p->type == NodeTypes::NodeScope)
                        p = static_cast<struct NodeScope*>(p)->node;
                    return "rt::setptr_dispatch(" + envVar() + ", " + emitExpr(p) + ", " + rhs + "); // запись через указатель";
                }
                if (l && l->type == NodeTypes::NodeBinaryScopeResolution) {
                    // Ns::var = v вЂ” write-back namespace-РїРµСЂРµРјРµРЅРЅРѕР№
                    auto* sr = static_cast<struct NodeBinaryScopeResolution*>(l);
                    Node* ll = sr->l_node;
                    while (ll && ll->type == NodeTypes::NodeScope)
                        ll = static_cast<struct NodeScope*>(ll)->node;
                    Node* rr = sr->r_node;
                    while (rr && rr->type == NodeTypes::NodeScope)
                        rr = static_cast<struct NodeScope*>(rr)->node;
                    if (ll && ll->type == NodeTypes::NodeVariable
                        && rr && rr->type == NodeTypes::NodeVariable) {
                        String key = static_cast<struct NodeVariable*>(ll)->name + "::"
                                   + static_cast<struct NodeVariable*>(rr)->name;
                        if (nsVars.find(key) != nsVars.end())
                            return "rt::assign_dispatch(" + envVar() + ", " + envVar() + ".get("
                                 + quote(key) + "), " + rhs + ");";
                        if (isStructStatic(key))
                            return "rt::assign_dispatch(" + envVar() + ", " + envVar() + ".get("
                                 + quote(key) + "), " + rhs + ");";
                    }
                    return "/* TBD scope-res lhs */(void)(0);";
                }
                if (l && l->type == NodeTypes::NodeArrayAccess) {
                    // a[i] = v вЂ” РґРёСЃРїРµС‚С‡РёРј РЅР° __set_index__, fallback вЂ” Р·Р°РјРµРЅР° СЌР»РµРјРµРЅС‚Р° РјР°СЃСЃРёРІР°
                    auto* aa = static_cast<struct NodeArrayAccess*>(l);
                    String base = emitExpr(aa->base);
                    if (aa->indices.size() == 1)
                        return "rt::setindex_dispatch(" + envVar() + ", " + base + ", "
                             + emitExpr(aa->indices[0]) + ", " + rhs + "); // запись в элемент";
                }
                return "/* TBD lhs " + typeName(n->l_node->type) + " */";
            }
            case NodeTypes::NodeEcho: {
                auto* n = static_cast<struct NodeEcho*>(node);
                if (n->expr) return "rt::println(rt::auto_get(rt::g_ctx, " + emitExpr(n->expr) + ")); // вывод значения\n";
                return "";
            }
            case NodeTypes::NodeOut: {
                auto* n = static_cast<struct NodeOut*>(node);
                String s;
                for (auto* e : n->exprs) {
                    s += "rt::print(rt::auto_get(rt::g_ctx, " + emitExpr(e) + ")); // вывод без перевода строки\n";
                }
                return s;
            }
            case NodeTypes::NodeOutln: {
                auto* n = static_cast<struct NodeOutln*>(node);
                String s;
                for (auto* e : n->exprs) {
                    s += "rt::println(rt::auto_get(rt::g_ctx, " + emitExpr(e) + ")); // вывод с новой строкой\n";
                }
                return s;
            }
            case NodeTypes::NodeIf: {
                auto* n = static_cast<struct NodeIf*>(node);
                String s = "if (rt::truthy(" + envVar() + ", " + emitExpr(n->condition) + ")) { // ветвление if/else";
                String then_s = emitStmt(n->then_body);
                if (!then_s.empty()) { s += "\n"; s += then_s; }
                s += "\n}";
                if (n->else_body) {
                    if (n->else_body->type == NodeTypes::NodeIf) {
                        s += " else ";
                        s += emitStmt(n->else_body);
                    } else {
                        s += " else {";
                        String else_s = emitStmt(n->else_body);
                        if (!else_s.empty()) { s += "\n"; s += else_s; }
                        s += "\n}";
                    }
                }
                return s;
            }
            case NodeTypes::NodeWhile: {
                auto* n = static_cast<struct NodeWhile*>(node);
                String s = "while (rt::truthy(" + envVar() + ", " + emitExpr(n->condition) + ")) { // цикл while";
                String body = emitStmt(n->body);
                if (!body.empty()) { s += "\n"; s += body; }
                s += "\n}";
                return s;
            }
            case NodeTypes::NodeFor: {
                auto* n = static_cast<struct NodeFor*>(node);
                String s = "for (";
                s += emitInitOrStep(n->init);
                s += ";";
                if (n->check) {
                    s += " rt::truthy(" + envVar() + ", " + emitExpr(n->check) + ")";
                }
                s += ";";
                String upd = emitInitOrStep(n->update);
                if (!upd.empty()) { s += " "; s += upd; }
                s += ") { // цикл for";
                String body = emitStmt(n->body);
                if (!body.empty()) { s += "\n"; s += body; }
                s += "\n}";
                return s;
            }
            case NodeTypes::NodeBreak:
                return "break; // выход из цикла";
            case NodeTypes::NodeContinue:
                return "continue; // следующая итерация";
            case NodeTypes::NodeFunctionCall: {
                auto* n = static_cast<struct NodeFunctionCall*>(node);
                String r = buildCall(n);
                if (r.empty()) return "/* TBD call stmt */;";
                return r + "; // вызов функции";
            }
            case NodeTypes::NodeReturn: {
                auto* n = static_cast<struct NodeReturn*>(node);
                if (n->expr && curReturnsRef) {
                    Node* e = n->expr;
                    while (e && e->type == NodeTypes::NodeScope)
                        e = static_cast<struct NodeScope*>(e)->node;
                    String al = refSourceAlias(e);
                    if (!al.empty()) return "return " + al + "; // live ref return";
                    return "return rt::retpromote(" + emitExpr(n->expr) + "); // ref return (стабильная ячейка)";
                }
                if (n->expr)
                    return "return rt::derefval(" + emitExpr(n->expr) + "); // возврат значения (live-ref -> значение)";
                return "return rt::Box::null(); // возврат без значения";
            }
            case NodeTypes::NodeAssert: {
                auto* n = static_cast<struct NodeAssert*>(node);
                // assert cond; -> if (!truthy(cond)) { abort }.
                // Раньше здесь стоял `if (rt::truthy(...))` БЕЗ отрицания, хотя
                // комментарий и DeepAnalizer (DEEP_ERRORS::ASSERT_FAILED при !ok)
                // задают обратную полярность: срабатывание происходило на ИСТИННОМ
                // условии, а на ложном assert молчал. Теперь polarity совпадает.
                return "if (!rt::truthy(" + envVar() + ", " + emitExpr(n->check) + ")) { std::cout << \"assertion failed"
                     + (n->message ? " (\" << rt::fmt(" + emitExpr(n->message) + ") << \")\" " : " \"")
                     + "<< std::endl; std::abort(); } // проверка assert";
            }
            case NodeTypes::NodeExit: {
                auto* n = static_cast<struct NodeExit*>(node);
                if (n->expr)
                    return "::exit((int)rt::num(" + emitExpr(n->expr) + ")); // завершение программы с кодом";
                return "::exit(0); // завершение программы";
            }
            case NodeTypes::NodeAppend: {
                auto* an = static_cast<struct NodeAppend*>(node);
                String target = appendLvalueRef(an->array_expr);
                if (target.empty())
                    return "/* TBD append lhs */;";
                return "rt::append_dispatch(" + envVar() + ", " + target + ", "
                     + emitExpr(an->value_expr) + "); // append элемент в массив";
            }
            case NodeTypes::NodeReserve: {
                auto* rn = static_cast<struct NodeReserve*>(node);
                String target = appendLvalueRef(rn->array_expr);
                if (target.empty())
                    return "/* TBD reserve lhs */;";
                return "rt::reserve_dispatch(" + envVar() + ", " + target + ", "
                     + emitExpr(rn->count_expr) + "); // reserve ёмкость массива";
            }
            case NodeTypes::NodeDelete: {
                auto* n = static_cast<struct NodeDelete*>(node);
                if (!n->index) {
                    // del РїРµСЂРµРјРµРЅРЅР°СЏ вЂ” РѕСЃРІРѕР±РѕР¶РґРµРЅРёРµ СЏС‡РµР№РєРё (СЃР±СЂРѕСЃ РІ null)
                    Node* host = n->expr;
                    while (host && host->type == NodeTypes::NodeScope)
                        host = static_cast<struct NodeScope*>(host)->node;
                    if (host && host->type == NodeTypes::NodeVariable) {
                        String nm = static_cast<struct NodeVariable*>(host)->name;
                        return envVar() + ".get(" + quote(nm) + ") = rt::Box::null(); // освобождение переменной";
                    }
                    return "/* TBD del erase for compound host */";
                }
                Node* host = n->expr;
                while (host && host->type == NodeTypes::NodeScope)
                    host = static_cast<struct NodeScope*>(host)->node;
                if (host && host->type == NodeTypes::NodeVariable) {
                    String nm = static_cast<struct NodeVariable*>(host)->name;
                    return envVar() + ".get(" + quote(nm) + ") = rt::del(" + emitExpr(host)
                            + ", " + emitExpr(n->index) + ");";
                }
                return "/* TBD del write-back for compound host */";
            }
case NodeTypes::NodeStructDeclaration:
                return emitStructStatics(node, String());
            case NodeTypes::NodeEnumDeclaration:
                return "";
            case NodeTypes::NodeNamespace: {
                auto* nsp = static_cast<struct NodeNamespace*>(node);
                return emitNamespaceStmt(nsp, nsp->name);
            }
            case NodeTypes::NodeUnaryPostfixIncrement:
            case NodeTypes::NodeUnaryPostfixDecrement:
            case NodeTypes::NodeUnaryPrefixIncrement:
            case NodeTypes::NodeUnaryPrefixDecrement: {
                // i++ / ++i / i-- / --i РєР°Рє СЃР°РјРѕСЃС‚РѕСЏС‚РµР»СЊРЅС‹Р№ statement:
                // РїРѕР±РѕС‡РЅРѕРµ РґРµР№СЃС‚РІРёРµ вЂ” i = i + 1 (РёР»Рё i - 1), Р·РЅР°С‡РµРЅРёРµ РѕС‚Р±СЂР°СЃС‹РІР°РµС‚СЃСЏ
                auto* n = static_cast<struct NodeUnaryPostfixIncrement*>(node);
                bool is_inc = (node->type == NodeTypes::NodeUnaryPostfixIncrement
                            || node->type == NodeTypes::NodeUnaryPrefixIncrement);
                Node* operand = n->node;
                while (operand && operand->type == NodeTypes::NodeScope)
                    operand = static_cast<struct NodeScope*>(operand)->node;
                String delta = String("rt::op_dispatch('") + (is_inc ? "+" : "-")
                             + String("', ") + envVar() + ", "
                             + emitExpr(operand) + ", rt::Box::i64(1))";
                if (operand && operand->type == NodeTypes::NodeVariable) {
                    String nm = static_cast<struct NodeVariable*>(operand)->name;
                    if (isCurRef(nm))
                        return "rt::setref_dispatch(" + envVar() + ", " + nm + ", " + delta + ");";
                    if (isRefVar(nm))
                        return "rt::setref_dispatch(" + envVar() + ", " + envVar() + ".get(" + quote(nm) + "), " + delta + ");";
                    if (isParam(nm))
                        return "rt::assign_dispatch(" + envVar() + ", " + nm + ", " + delta + ");";
                    return "rt::assign_dispatch(" + envVar() + ", " + envVar() + ".get("
                         + quote(nm) + "), " + delta + ");";
                }
                // member access / Р·Р°РїРёСЃСЊ С‡РµСЂРµР· СѓРєР°Р·Р°С‚РµР»СЊ
                String lval = emitExpr(n->node);
                return lval + " = " + delta + ";";
            }
            default:
                if (node->op == EX) {
                    // РІС‹СЂР°Р¶РµРЅРёРµ-РІС‹СЂР°Р¶РµРЅРёРµ (РЅР°РїСЂ. i++, typeof(x) РєР°Рє stmt)
                    String e = emitExpr(node);
                    if (!e.empty() && e != "/* TBD expr 55 */ rt::Box::null()")
                        return e + ";";
                }
                if (dbg) tbdCount++;
                return "/* TBD stmt " + typeName(node->type) + " */";
        }
    }

    static String typeName(NodeTypes t) {
        return String(std::to_string(static_cast<int>(t)));
    }

    // Имя типа ноды для отладочной трассировки кодогенерации (-d).
    static String nodeName(NodeTypes t) {
        switch (t) {
            case NodeTypes::NodeNumber: return "Number";
            case NodeTypes::NodeString: return "String";
            case NodeTypes::NodeBool: return "Bool";
            case NodeTypes::NodeScope: return "Scope";
            case NodeTypes::NodeStatementsScope: return "StatementsScope";
            case NodeTypes::NodeModifier: return "Modifier";
            case NodeTypes::NodeNamespace: return "Namespace";
            case NodeTypes::NodeIf: return "If";
            case NodeTypes::NodeWhile: return "While";
            case NodeTypes::NodeFor: return "For";
            case NodeTypes::NodeBreak: return "Break";
            case NodeTypes::NodeContinue: return "Continue";
            case NodeTypes::NodeReturn: return "Return";
            case NodeTypes::NodeOut: return "Out";
            case NodeTypes::NodeOutln: return "Outln";
            case NodeTypes::NodeEcho: return "Echo";
            case NodeTypes::NodeInput: return "Input";
            case NodeTypes::NodeDelete: return "Delete";
            case NodeTypes::NodeNull: return "Null";
            case NodeTypes::NodeVariable: return "Variable";
            case NodeTypes::NodeUnaryMinus: return "UnaryMinus";
            case NodeTypes::NodeUnaryPlus: return "UnaryPlus";
            case NodeTypes::NodeUnaryNegation: return "UnaryNegation";
            case NodeTypes::NodeUnaryDereference: return "UnaryDereference";
            case NodeTypes::NodeUnaryBitwiseNot: return "UnaryBitwiseNot";
            case NodeTypes::NodeUnaryAddressOf: return "UnaryAddressOf";
            case NodeTypes::NodeBinaryMultiplicative: return "Binary*";
            case NodeTypes::NodeBinaryDivision: return "Binary/";
            case NodeTypes::NodeBinaryRemainder: return "Binary%";
            case NodeTypes::NodeBinaryAdditive: return "Binary+";
            case NodeTypes::NodeBinarySubtraction: return "Binary-";
            case NodeTypes::NodeBinaryShiftLeft: return "Binary<<";
            case NodeTypes::NodeBinaryShiftRight: return "Binary>>";
            case NodeTypes::NodeBinaryEqual: return "Binary==";
            case NodeTypes::NodeBinaryNotEqual: return "Binary!=";
            case NodeTypes::NodeBinaryLess: return "Binary<";
            case NodeTypes::NodeBinaryGreater: return "Binary>";
            case NodeTypes::NodeBinaryLessEqual: return "Binary<=";
            case NodeTypes::NodeBinaryGreaterEqual: return "Binary>=";
            case NodeTypes::NodeBinaryBitwiseAnd: return "Binary&";
            case NodeTypes::NodeBinaryBitwiseXor: return "Binary^";
            case NodeTypes::NodeBinaryBitwiseOr: return "Binary|";
            case NodeTypes::NodeBinaryLogicalAnd: return "Binary&&";
            case NodeTypes::NodeBinaryLogicalOr: return "Binary||";
            case NodeTypes::NodeBinaryNullCoalesce: return "Binary??";
            case NodeTypes::NodeBinaryScopeResolution: return "ScopeResolution";
            case NodeTypes::NodeBinaryAssignment: return "Assignment";
            case NodeTypes::NodeCast: return "Cast";
            case NodeTypes::NodeMemberAccess: return "MemberAccess";
            case NodeTypes::NodeFunctionCall: return "FunctionCall";
            case NodeTypes::NodeNamedArgument: return "NamedArgument";
            case NodeTypes::NodeFunctionType: return "FunctionType";
            case NodeTypes::NodeTupleType: return "TupleType";
            case NodeTypes::NodeArrayType: return "ArrayType";
            case NodeTypes::NodeArray: return "Array";
            case NodeTypes::NodeArrayAccess: return "ArrayAccess";
            case NodeTypes::NodeLambda: return "Lambda";
            case NodeTypes::NodeNew: return "New";
            case NodeTypes::NodeTypeOf: return "TypeOf";
            case NodeTypes::NodeMod: return "Mod";
            case NodeTypes::NodeSizeOf: return "SizeOf";
            case NodeTypes::NodePtr: return "Ptr";
            case NodeTypes::NodeVariableDeclaration: return "VarDecl";
            case NodeTypes::NodeFunctionDeclaration: return "FnDecl";
            case NodeTypes::NodeStructDeclaration: return "StructDecl";
            case NodeTypes::NodeEnumDeclaration: return "EnumDecl";
            case NodeTypes::NodeStructLiteral: return "StructLiteral";
            case NodeTypes::NodeUnaryPostfixIncrement: return "Postfix++";
            case NodeTypes::NodeUnaryPostfixDecrement: return "Postfix--";
            case NodeTypes::NodePostfixUnwrap: return "PostfixUnwrap";
            case NodeTypes::NodeUnaryPrefixIncrement: return "Prefix++";
            case NodeTypes::NodeUnaryPrefixDecrement: return "Prefix--";
            case NodeTypes::NodeAssert: return "Assert";
            case NodeTypes::NodeExit: return "Exit";
            case NodeTypes::NodeAppend: return "Append";
            case NodeTypes::NodeReserve: return "Reserve";
            default: return String("T") + std::to_string(static_cast<int>(t));
        }
    }

    // C++-РІС‹СЂР°Р¶РµРЅРёРµ, РґР°СЋС‰РµРµ СЃС‚СЂРѕРєРѕРІРѕРµ РёРјСЏ С‚РёРїР° (РґР»СЏ СЂРµРёС„Р°Р№ С‚РёРїР° РєР°Рє Р·РЅР°С‡РµРЅРёСЏ).
    static String typeText(Node* n) {
        while (n && n->type == NodeTypes::NodeScope)
            n = static_cast<struct NodeScope*>(n)->node;
        if (n && n->type == NodeTypes::NodeVariable)
            return quote(static_cast<struct NodeVariable*>(n)->name);
        if (n && n->type == NodeTypes::NodeUnaryDereference)
            return "std::string(\"*\") + " + typeText(static_cast<struct NodeUnaryDereference*>(n)->node);
        return String("std::string(\"?type\")");
    }

    // ------------------- С„СѓРЅРєС†РёРё -------------------

    // РіСЂСѓР±С‹Р№ СЃС‚Р°С‚РёС‡РµСЃРєРёР№ С‚РµРі РІС‹СЂР°Р¶РµРЅРёСЏ вЂ” РґР»СЏ СЂР°Р·СЂРµС€РµРЅРёСЏ РїРµСЂРµРіСЂСѓР·РѕРє
    static String exprTag(Node* n) {
        if (!n) return "unknown";
        switch (n->type) {
            case NodeTypes::NodeScope:
                return exprTag(static_cast<struct NodeScope*>(n)->node);
            case NodeTypes::NodeNumber: {
                auto* num = static_cast<struct NodeNumber*>(n);
                return num->is_integer ? String("i64") : String("f64");
            }
            case NodeTypes::NodeString:  return static_cast<struct NodeString*>(n)->is_char ? "char" : "str";
            case NodeTypes::NodeBool:    return "bool";
            case NodeTypes::NodeNull:    return "null";
            default:                     return "unknown";
        }
    }

    // С‚РµРі РѕР±СЉСЏРІР»РµРЅРЅРѕРіРѕ С‚РёРїР° РїР°СЂР°РјРµС‚СЂР°; "generic" вЂ” РёРјСЏ РёР· generic-СЃРїРёСЃРєР° С„СѓРЅРєС†РёРё
    static String paramTag(const FuncArg& a, struct NodeFunctionDeclaration* fn) {
        Node* t = a.type;
        while (t && t->type == NodeTypes::NodeScope)
            t = static_cast<struct NodeScope*>(t)->node;
        if (!t || t->type != NodeTypes::NodeVariable) return "unknown";
        String tn = static_cast<struct NodeVariable*>(t)->name;
        for (auto& g : fn->generics)
            if (g.name == tn) return "generic";
        // auto / any вЂ” РїСЂРёРЅРёРјР°СЋС‚ Р·РЅР°С‡РµРЅРёРµ Р»СЋР±РѕРіРѕ С‚РёРїР°
        if (tn == "auto" || tn == "any") return "generic";
        return tn;
    }

    String mangledDunderTag(const FuncArg& a, struct NodeFunctionDeclaration* fn,
                            struct NodeStructDeclaration* st) {
        Node* t = a.type;
        while (t && t->type == NodeTypes::NodeScope)
            t = static_cast<struct NodeScope*>(t)->node;
        if (!t || t->type != NodeTypes::NodeVariable) return "unknown";
        String tn = static_cast<struct NodeVariable*>(t)->name;
        // generic-РїР°СЂР°РјРµС‚СЂ РјРѕР¶РµС‚ РїСЂРёРЅР°РґР»РµР¶Р°С‚СЊ С„СѓРЅРєС†РёРё РР›Р СЃС‚СЂСѓРєС‚СѓСЂРµ (Ref[T].__assign__(self, T))
        for (auto& g : fn->generics)   if (g.name == tn) return "generic";
        if (st) for (auto& g : st->generics) if (g.name == tn) return "generic";
        if (tn == "auto" || tn == "any") return "generic";
        return tn;
    }

    // Базовые (листовые) типы языка: единственные теги, для которых несовпадение
    // с тегом аргумента точно означает «другая перегрузка». Всё остальное (union-типы
    // вроде Numeric, составные `Pair[i64]`, `ref T`, `auto`) в строковом виде не
    // разбирается, поэтому такие параметры здесь не отсекаются.
    static bool isBaseTag(const String& t) {
        static const char* kBase[] = {
            "i8", "i16", "i32", "i64", "f16", "f32", "f64",
            "bool", "char", "str", "byte", "Null", "Type", "Namespace", "Fn", "void"
        };
        for (const char* b : kBase) if (t == b) return true;
        return false;
    }

    // Группы Numeric/Integer/Float не хардкодим: их раскрывает настоящий
    // TypeSystem (см. compiler/core/TypeSystem.cpp), а этот fallback сознательно
    // оставлен только для мест без решения анализатора (-noanalize, тело
    // невызванной функции) и отвечает «в принципе подходит ли», а не «точное ли
    // совпадение». Неизвестные/составные теги считаются кандидатом с низким
    // приоритетом; точное совпадение по-прежнему выигрывает по скорингу.
    bool argCompatible(const String& at, const String& pt) const {
        if (pt == "generic") return true;
        if (at == "unknown" || pt == "unknown") return true;
        if (at == pt) return true;
        return !isBaseTag(pt);
    }

    // Решение deep-анализатора для конкретного узла вызова -> FnSig с тем же
    // узлом объявления. null, если анализатор этот вызов не разбирал.
    FnSig* sigFromResolvedCall(Node* call) const {
        if (!resolved_calls || !call) return nullptr;
        auto rit = resolved_calls->find(call);
        if (rit == resolved_calls->end()) return nullptr;
        for (auto& reg : fnRegistry)
            for (const FnSig& sig : reg.second)
                if (sig.node == rit->second)
                    return const_cast<FnSig*>(&sig);
        for (auto& reg : instanceRegistry)
            for (const FnSig& sig : reg.second)
                if (sig.node == rit->second)
                    return const_cast<FnSig*>(&sig);
        return nullptr;
    }

    // выбор перегрузки по аргументам и типам; детали закоплены в buildCall
    FnSig* resolveForCall(const String& name, const std::vector<Node*>& args, Node* call = nullptr) {
        if (FnSig* decided = sigFromResolvedCall(call)) return decided;
        auto it = fnRegistry.find(name);
        if (it == fnRegistry.end()) return nullptr;
        std::vector<CallArgMeta> metas = buildCallArgMetas(args);
        FnSig* best = nullptr;
        int bestScore = -1;
        for (auto& sig : it->second) {
            struct NodeFunctionDeclaration* fn = sig.node;
            std::vector<String> pnames;
            pnames.reserve(fn->params.size());
            for (const FuncArg& p : fn->params) pnames.push_back(p.name);
            std::vector<int> afp;
            if (!placeCallArgs(pnames, metas, afp)) continue;
            bool missingOk = true;
            for (size_t j = 0; j < fn->params.size() && missingOk; ++j)
                if (afp[j] < 0 && !fn->params[j].default_value) missingOk = false;
            if (!missingOk) continue;
            int score = 0;
            bool ok = true;
            for (size_t j = 0; j < fn->params.size() && ok; ++j) {
                int ai = afp[j];
                if (ai < 0) continue;
                String pt = paramTag(fn->params[j], fn);
                String at = exprTag(callArgValueNode(args[ai]));
                if (!argCompatible(at, pt)) { ok = false; break; }
                score += (pt == "generic") ? 1 : 3;
                if (at == pt) score += 2;
            }
            if (!ok) continue;
            for (size_t j = 0; j < fn->params.size(); ++j)
                if (afp[j] < 0)
                    score += (paramTag(fn->params[j], fn) == "generic") ? 1 : 2;
            if (score > bestScore) { bestScore = score; best = &sig; }
        }
        return best;
    }

    // РІС‹Р±РѕСЂ РїРµСЂРµРіСЂСѓР·РєРё РјРµС‚РѕРґР° (params[0] = self/recv; args РЈР–Р• Р±РµР· self)
    FnSig* resolveForMethod(const String& name, const std::vector<Node*>& args, Node* call = nullptr) {
        if (FnSig* decided = sigFromResolvedCall(call)) return decided;
        auto it = instanceRegistry.find(name);
        if (it == instanceRegistry.end()) return nullptr;
        std::vector<CallArgMeta> metas = buildCallArgMetas(args);
        metas.insert(metas.begin(), CallArgMeta());   // ресивер = позиционный слот self
        FnSig* best = nullptr;
        int bestScore = -1;
        for (auto& sig : it->second) {
            struct NodeFunctionDeclaration* fn = sig.node;
            std::vector<String> pnames;
            pnames.reserve(fn->params.size());
            for (const FuncArg& p : fn->params) pnames.push_back(p.name);
            std::vector<int> afp;
            if (!placeCallArgs(pnames, metas, afp)) continue;
            bool missingOk = true;
            for (size_t j = 0; j < fn->params.size() && missingOk; ++j)
                if (afp[j] < 0 && !fn->params[j].default_value) missingOk = false;
            if (!missingOk) continue;
            int score = 0;
            bool ok = true;
            for (size_t j = 1; j < fn->params.size() && ok; ++j) {   // self не скорим
                int ai = afp[j];
                if (ai < 0 || ai == 0) continue;   // дефолт / ресивер
                String pt = paramTag(fn->params[j], fn);
                String at = exprTag(callArgValueNode(args[ai - 1]));
                if (!argCompatible(at, pt)) { ok = false; break; }
                score += (pt == "generic") ? 1 : 3;
                if (at == pt) score += 2;
            }
            if (!ok) continue;
            for (size_t j = 1; j < fn->params.size(); ++j)
                if (afp[j] < 0)
                    score += (paramTag(fn->params[j], fn) == "generic") ? 1 : 2;
            if (score > bestScore) { bestScore = score; best = &sig; }
        }
        return best;
    }

    // СЌРјРёСЃСЃРёСЏ РІС‹Р·РѕРІР° С„СѓРЅРєС†РёРё/С„СѓРЅРєС†РёРё-Р·РЅР°С‡РµРЅРёСЏ/РјРµС‚РѕРґР°/scope-РІС‹Р·РѕРІР°
    // Ресивер-тип-параметр: конкретная структура на этапе кодогенерации неизвестна,
    // поэтому вызов x.method() нельзя запекать в один mf_*_метод — иначе все
    // instantiations получают тело последней найденной структуры. Такие вызовы
    // уходят в rt::method_dispatch и резолвятся по типу значения в рантайме.
    //   recvDynVar  — ресивер- переменная типа-генерика (x: T, x: any, generic-значение)
    //   recvDynField— ресивер self.<field>, поле generic-структуры (self.v в struct[T] Box)
    bool recvDynVar = false;
    bool recvDynField = false;
    void classifyRecv(Node* recv) {
        recvDynVar = recvDynField = false;
        if (!recv) return;
        Node* r = recv;
        while (r && r->type == NodeTypes::NodeScope)
            r = static_cast<struct NodeScope*>(r)->node;
        if (!r) return;
        if (r->type == NodeTypes::NodeVariable) {
            const String& nm = static_cast<struct NodeVariable*>(r)->name;
            if (curGenericVars.count(nm)) { recvDynVar = true; return; }
            if (!curFnDecl) return;
            for (const FuncArg& p : curFnDecl->params)
                if (p.name == nm) { recvDynVar = (paramTag(p, curFnDecl) == "generic"); return; }
            return;
        }
        // self.<field>: тип поля = параметр типа текущей структуры
        if (r->type != NodeTypes::NodeMemberAccess) return;
        if (curStructOwner.empty() || !curFnDecl || curFnDecl->params.empty()) return;
        auto* ma = static_cast<struct NodeMemberAccess*>(r);
        Node* base = ma->l_node;
        while (base && base->type == NodeTypes::NodeScope)
            base = static_cast<struct NodeScope*>(base)->node;
        Node* fld = ma->r_node;
        while (fld && fld->type == NodeTypes::NodeScope)
            fld = static_cast<struct NodeScope*>(fld)->node;
        if (!base || base->type != NodeTypes::NodeVariable) return;
        if (!fld || fld->type != NodeTypes::NodeVariable) return;
        if (static_cast<struct NodeVariable*>(base)->name != curFnDecl->params[0].name) return;
        auto git = structGenericField.find(curStructOwner);
        if (git == structGenericField.end()) return;
        const String& fname = static_cast<struct NodeVariable*>(fld)->name;
        for (const auto& kv : git->second)
            if (kv.second == fname) { recvDynField = true; return; }
    }

    String buildCall(struct NodeFunctionCall* n) {
        Node* callee = n->callee;
        while (callee && callee->type == NodeTypes::NodeScope)
            callee = static_cast<struct NodeScope*>(callee)->node;
        if (callee && callee->type == NodeTypes::NodeFunctionCall) {
            // С†РµРїРѕС‡РµС‡РЅС‹Р№ РІС‹Р·РѕРІ: f(...)(...) вЂ” Р·РЅР°С‡РµРЅРёРµ РІС‹Р·РѕРІР° СЃРЅРѕРІР° РІС‹Р·С‹РІР°РµРј РґРёРЅР°РјРёС‡РµСЃРєРё
            String args = argVectorText(n->args);
            return "rt::call(" + emitExpr(callee) + ", " + envVar()
                 + ", std::vector<rt::Box>{" + args + "})";
        }
        if (callee && callee->type == NodeTypes::NodeVariable) {
            String name = static_cast<struct NodeVariable*>(callee)->name;
            bool isParam = false;
            for (const String& p : curParms) if (p == name) { isParam = true; break; }
            // Тип структуры вызван как функция: `Number(20)` -> статический
            // __call__ этой структуры. Имени типа нет в fnRegistry (это не
            // функция), поэтому ниже сработала бы ветка rt::call по ЗНАЧЕНИЮ
            // типа — такой вызов всегда возвращает null. Решение deep-анализатора
            // уже указывает на нужную перегрузку __call__, сверяемся с ней.
            if (!isParam) {
                if (FnSig* decided = sigFromResolvedCall(n)) {
                    FnSig* tsig = resolveForCall(name + "::__call__", n->args, nullptr);
                    if (tsig && tsig->node == decided->node) {
                        String targs = argVectorText(n->args, tsig->node);
                        String tgh = genericHiddenArgs(tsig->node, n);
                        String tal = targs.empty() ? tgh : (tgh.empty() ? targs : targs + ", " + tgh);
                        return tsig->cpp + "(rt::Ctx(&rt::g_ctx), std::vector<rt::Box>{" + tal + "})";
                    }
                }
            }
            // РІС‹Р·РѕРІ С‡РµСЂРµР· РїРµСЂРµРјРµРЅРЅСѓСЋ, С…СЂР°РЅСЏС‰СѓСЋ С„СѓРЅРєС†РёСЋ-Р·РЅР°С‡РµРЅРёРµ (let f = fn_name)
            if (!isParam && fnRegistry.find(name) == fnRegistry.end()
                && (curNs.empty() || fnRegistry.find(curNs + "::" + name) == fnRegistry.end())) {
                String args = argVectorText(n->args);
                return "rt::call(" + envVar() + ".get(" + quote(name) + "), "
                     + envVar() + ", std::vector<rt::Box>{" + args + "})";
            }
            // РїР°СЂР°РјРµС‚СЂ С„СѓРЅРєС†РёРѕРЅР°Р»СЊРЅРѕРіРѕ С‚РёРїР°: РІС‹Р·С‹РІР°РµРј Р·РЅР°С‡РµРЅРёРµ (Func[...]) РґРёРЅР°РјРёС‡РµСЃРєРё
            if (isParam) {
                String args = argVectorText(n->args);
                return "rt::call(" + emitExpr(callee) + ", "
                     + envVar() + ", std::vector<rt::Box>{" + args + "})";
            }
            FnSig* sig = nullptr;
            if (!isParam) {
                sig = resolveForCall(name, n->args, n);
                // соседняя функция namespace, вызванная голым именем из ns-функции
                // (Geometry::centroid вызывает sum_all(vs)): в реестре она лежит как "Ns::name"
                if (!sig && !curNs.empty())
                    sig = resolveForCall(curNs + "::" + name, n->args, n);
            }
            if (!sig) return "";
            String args = argVectorText(n->args, sig->node);
            String gh = genericHiddenArgs(sig->node, n);
            String al = args.empty() ? gh : (gh.empty() ? args : args + ", " + gh);
            return sig->cpp + "(rt::Ctx(&rt::g_ctx), std::vector<rt::Box>{" + al + "})";
        }
        if (callee && callee->type == NodeTypes::NodeMemberAccess) {
            auto* ma = static_cast<struct NodeMemberAccess*>(callee);
            Node* mr = ma->r_node;
            while (mr && mr->type == NodeTypes::NodeScope)
                mr = static_cast<struct NodeScope*>(mr)->node;
            String method;
            if (mr && mr->type == NodeTypes::NodeVariable)
                method = static_cast<struct NodeVariable*>(mr)->name;
            FnSig* sig = resolveForMethod(method, n->args, n);
            if (!sig) return "";
            // Р°СЂРіСѓРјРµРЅС‚С‹ РјРµС‚РѕРґР°: recv РІСЃРµРіРґР° РїРµСЂРІС‹Р№; РґР°Р»РµРµ вЂ” СЏРІРЅС‹Рµ Р°СЂРіСѓРјРµРЅС‚С‹ + РґРµС„РѕР»С‚С‹
            struct NodeFunctionDeclaration* fn = sig->node;
            String recv = emitExpr(ma->l_node);
            classifyRecv(ma->l_node);
            bool dynRecv = recvDynVar || recvDynField;
            // ref self: ресивер оборачиваем в живой алиас (как ref-аргумент в argVectorText), чтобы
            // self внутри метода был T_REF на ячейку ресивера, а &self указывал на неё же.
            if (!fn->params.empty() && argIsRef(fn->params[0])) {
                Node* rl = ma->l_node;
                while (rl && rl->type == NodeTypes::NodeScope)
                    rl = static_cast<struct NodeScope*>(rl)->node;
                if (rl && rl->type == NodeTypes::NodeVariable) {
                    String rnm = static_cast<struct NodeVariable*>(rl)->name;
                    if (isCurRef(rnm)) {
                        recv = rnm;  // C++-локальная ref-переменная (уже T_REF)
                    } else if (isRefVar(rnm)) {
                        recv = envVar() + ".get(" + quote(rnm) + ")"; // ref-ячейка в env (уже T_REF)
                    } else {
                        recv = "rt::Box::rref(rt::refcell_ptr(" + envVar() + ".get(" + quote(rnm) + ")))";
                    }
                } else if (rl) {
                    String al = refSourceAlias(rl);
                    if (!al.empty()) recv = al;
                }
            }
            // ref self для тип-параметра- переменной: ресивер — C++-локаль
            // (rt::Box x = args_[0]), а не ячейка в env, поэтому алиас строим прямо
            // на неё. Для self.<field> ресивер уже собран выше как живой refpath.
            if (recvDynVar && !fn->params.empty() && argIsRef(fn->params[0]))
                recv = "rt::Box::rref(rt::refcell_ptr(" + emitExpr(ma->l_node) + "))";
            std::vector<String> parts;
            parts.push_back(recv);             // self всегда первый
            std::vector<CallArgMeta> mmetas = buildCallArgMetas(n->args);
            mmetas.insert(mmetas.begin(), CallArgMeta());   // ресивер = слот self
            std::vector<String> mpnames;
            mpnames.reserve(fn->params.size());
            for (const FuncArg& p : fn->params) mpnames.push_back(p.name);
            std::vector<int> mafp;
            if (placeCallArgs(mpnames, mmetas, mafp)) {
                for (size_t j = 1; j < fn->params.size(); ++j) {
                    if (mafp[j] >= 0) {
                        if (mafp[j] == 0) continue;   // ресивер уже добавлен
                        parts.push_back(emitExpr(callArgValueNode(n->args[mafp[j] - 1])));
                    } else if (fn->params[j].default_value) {
                        parts.push_back(emitExpr(fn->params[j].default_value));
                    }
                }
            } else {
                for (auto* a : n->args)
                    parts.push_back(emitExpr(callArgValueNode(a)));
                for (size_t i = n->args.size() + 1; i < fn->params.size(); ++i)
                    if (fn->params[i].default_value)
                        parts.push_back(emitExpr(fn->params[i].default_value));
            }
            String argsStr;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i) argsStr += ", ";
                argsStr += parts[i];
            }
            String gh = genericHiddenArgs(fn, n);
            if (!gh.empty()) {
                if (!argsStr.empty()) argsStr += ", ";
                argsStr += gh;
            }
            if (dynRecv)
                return "rt::method_dispatch(rt::g_ctx, " + quote(method)
                     + ", std::vector<rt::Box>{" + argsStr + "}, "
                     + String(std::to_string(fn->params.size())) + ")";
            return sig->cpp + "(rt::Ctx(&rt::g_ctx), std::vector<rt::Box>{" + argsStr + "})";
        }
        if (callee && callee->type == NodeTypes::NodeBinaryScopeResolution) {
            auto* sr = static_cast<struct NodeBinaryScopeResolution*>(callee);
            Node* l = sr->l_node;
            while (l && l->type == NodeTypes::NodeScope)
                l = static_cast<struct NodeScope*>(l)->node;
            Node* r = sr->r_node;
            while (r && r->type == NodeTypes::NodeScope)
                r = static_cast<struct NodeScope*>(r)->node;
            String lname, rname;
            if (l && l->type == NodeTypes::NodeVariable)
                lname = static_cast<struct NodeVariable*>(l)->name;
            if (r && r->type == NodeTypes::NodeVariable)
                rname = static_cast<struct NodeVariable*>(r)->name;
            String key = lname + "::" + rname;
            FnSig* sig = resolveForCall(key, n->args, n);
            if (!sig) return "";
            String args = argVectorText(n->args, sig->node);
            String gh = genericHiddenArgs(sig->node, n);
            String al = args.empty() ? gh : (gh.empty() ? args : args + ", " + gh);
            return sig->cpp + "(rt::Ctx(&rt::g_ctx), std::vector<rt::Box>{" + al + "})";
        }
        // Динамический вызов значения (объект-литерал {Test{a:10}()}, лямбда
        // как выражение): rt::call сам находит __call__ по типу значения.
        String args = argVectorText(n->args);
        return "rt::call(" + emitExpr(callee) + ", " + envVar()
             + ", std::vector<rt::Box>{" + args + "})";
    }

    String argVectorText(const std::vector<Node*>& args, struct NodeFunctionDeclaration* fn = nullptr) {
        String s;
        std::vector<Node*> filled;
        if (fn) {
            // Раскладываем фактические аргументы (в т.ч. именованные) по параметрам
            // выбранного кандидата и заполняем дефолтами пропущенные слоты.
            std::vector<String> pnames;
            pnames.reserve(fn->params.size());
            for (const FuncArg& p : fn->params) pnames.push_back(p.name);
            std::vector<CallArgMeta> metas = buildCallArgMetas(args);
            std::vector<int> afp;
            if (placeCallArgs(pnames, metas, afp)) {
                filled.resize(fn->params.size(), nullptr);
                for (size_t j = 0; j < fn->params.size(); ++j) {
                    if (afp[j] >= 0) filled[j] = callArgValueNode(args[afp[j]]);
                    else if (fn->params[j].default_value) filled[j] = fn->params[j].default_value;
                    else return "";
                }
            } else {
                // Некорректная раскладка уже заблокирована интерпретатором
                filled = args;
                for (size_t i = filled.size(); i < fn->params.size(); ++i)
                    if (fn->params[i].default_value) filled.push_back(fn->params[i].default_value);
            }
        } else {
            filled = args;
        }
        for (size_t i = 0; i < filled.size(); ++i) {
            if (i) s += ", ";
            Node* a = callArgValueNode(filled[i]);
            while (a && a->type == NodeTypes::NodeScope)
                a = static_cast<struct NodeScope*>(a)->node;
            // ref-параметр: если аргумент — lvalue-переменная, оборачиваем в rref(refcell_ptr(...)).
            // Для динамического вызова (fn == nullptr) оборачиваем ВСЕ переменные-аргументы:
            // преамбула каллируемого снимает rref с value-параметров (копия), а ref-параметры
            // получают живую ячейку — write-back к вызывающему (как в интерпретаторе).
            bool want_ref = (fn != nullptr) && (i < fn->params.size()) && argIsRef(fn->params[i]);
            if (!fn) want_ref = true;
            if (want_ref && a && a->type == NodeTypes::NodeVariable) {
                String nm = static_cast<struct NodeVariable*>(a)->name;
                // модификаторы объявления аргумента едут в rt::Box::mods, чтобы
                // `mod(refParam, ref)` в теле видел `ref let` источник.
                // Внимание: ref-ПАРАМЕТР (`isCurRef`) свои mods уже несёт значением,
                // его не помечаем — иначе терялась бы транзитивность (via(a) != true).
                bool arg_ref = isRefVar(nm);
                String ae;
                if (isCurRef(nm)) {
                    ae = nm;  // C++-локальная ref-переменная (уже T_REF)
                } else if (isRefVar(nm)) {
                    ae = envVar() + ".get(" + quote(nm) + ")"; // ref-ячейка в env (уже T_REF)
                } else if (isParam(nm)) {
                    ae = "rt::Box::rref(rt::refcell_ptr(" + nm + "))";   // C++-локаль
                } else {
                    auto fit = fnRegistry.find(nm);
                    if (fit != fnRegistry.end() && !fit->second.empty()) {
                        if (fn) {
                            // имя функции как значение: своя ячейка (fns не лежат в env)
                            ae = "rt::Box::refcell(rt::Box::fn(&" + fit->second[0].cpp + "))";
                        } else {
                            ae = emitExpr(a);   // функция-значение: в env её нет
                        }
                    } else {
                        ae = "rt::Box::rref(rt::refcell_ptr(" + envVar() + ".get(" + quote(nm) + ")))";
                    }
                }
                if (arg_ref) ae = "rt::refmods(" + ae + ", rt::MOD_REF)";
                s += ae;
                continue;
            }
            if (want_ref && a) {
                String al = refSourceAlias(a);
                if (!al.empty()) { s += al; continue; }
            }
            s += emitExpr(callArgValueNode(filled[i]));
        }
        return s;
    }

    // Р»СЏРјР±РґР° РєР°Рє Р°РЅРѕРЅРёРјРЅР°СЏ С„СѓРЅРєС†РёСЏ (С„РёРєСЃРёСЂРѕРІР°РЅРЅС‹Р№, РЅРѕ СѓРЅРёРєР°Р»СЊРЅС‹Р№ cpp-РёРјСЏ)
    struct NodeFunctionDeclaration* makeLambdaAsFunction(struct NodeLambda* n, const String& cpp) {
        String name = cpp;   // РёРјСЏ РЅРµ РєРѕРЅС„Р»РёРєС‚СѓРµС‚: СЂРµРіРёСЃС‚СЂ СЃ Р»СЏРјР±РґР°РјРё РѕС‚РґРµР»СЊРЅС‹Р№
        struct NodeFunctionDeclaration* f = new struct NodeFunctionDeclaration(name, {}, n->params, n->return_type, n->body);
        (void)cpp;
        return f;
    }

    // Р»СЏРјР±РґС‹ РІСЃС‚СЂРµС‡Р°СЋС‚СЃСЏ РїРѕ СЃРёРЅС‚Р°РєСЃРёСЃСѓ С‚РѕР»СЊРєРѕ РєР°Рє РёРЅРёС†РёР°Р»РёР·Р°С‚РѕСЂС‹ `let x = lambda ...`
    // (Р·РЅР°С‡РёС‚, РІ value_node РїРµСЂРµРјРµРЅРЅС‹С…). Р РµРіРёСЃС‚СЂРёСЂСѓРµРј Р·Р°СЂР°РЅРµРµ, РґРѕ РїСЂРѕС…РѕРґР° РѕРїСЂРµРґРµР»РµРЅРёР№.
    void collectLambdas() {
        for (auto* node : tree) {
            Node* n = node;
            while (n && n->type == NodeTypes::NodeModifier)
                n = static_cast<struct NodeModifier*>(n)->node;
            if (!n || n->type != NodeTypes::NodeVariableDeclaration) continue;
            Node* v = static_cast<struct NodeVariableDeclaration*>(n)->value_node;
            while (v && v->type == NodeTypes::NodeScope)
                v = static_cast<struct NodeScope*>(v)->node;
            if (v && v->type == NodeTypes::NodeLambda) {
                String cpp = "lf_lambda_" + String(std::to_string(lambdaSeq++));
                struct NodeFunctionDeclaration* f = makeLambdaAsFunction(static_cast<struct NodeLambda*>(v), cpp);
                lambdaMap[v] = FnSig{f, cpp};
            }
        }
    }

    // СЂРµРіРёСЃС‚СЂРёСЂСѓРµРј С„СѓРЅРєС†РёРё (РїСЂРµРїР°СЃСЃ): top-level, static-РјРµС‚РѕРґС‹ СЃС‚СЂСѓРєС‚СѓСЂ, С„СѓРЅРєС†РёРё namespace,
    // Р° С‚Р°РєР¶Рµ instance-РјРµС‚РѕРґС‹ РІ instanceRegistry, enum'С‹ РІ enumMap, ns-РїРµСЂРµРјРµРЅРЅС‹Рµ РІ nsVars
    void collectFunctions() {
        for (auto* node : tree)
            collectFrom(node, String());
    }

    void registerFn(struct NodeFunctionDeclaration* fn, const String& scope, const String& ns) {
        String key = scope.empty() ? fn->name : scope + "::" + fn->name;
        int idx = fnCounts[key]++;
        String suffix = idx ? "_" + String(std::to_string(idx)) : String("");
        String cpp;
        if (scope.empty()) cpp = "lf_" + fn->name + suffix;
        else               cpp = "lf_" + cppName(scope) + "_" + fn->name + suffix;
        fnRegistry[key].push_back(FnSig{fn, cpp, ns});
    }

    static Node* unwrapMod(Node* n) {
        while (n && n->type == NodeTypes::NodeModifier)
            n = static_cast<struct NodeModifier*>(n)->node;
        return n;
    }

    // РѕР±С…РѕРґ РЅРѕРґС‹-С‚РёРїР°: РІС‹Р·С‹РІР°РµС‚ cb(РёРјСЏ) РґР»СЏ РєР°Р¶РґРѕРіРѕ РІСЃС‚СЂРµС‡РµРЅРЅРѕРіРѕ NodeVariable
    // (generic-РёРјРµРЅР° Рё РїСЂРёРјРёС‚РёРІС‹), СЂРµРєСѓСЂСЃРёРІРЅРѕ РїРѕРіСЂСѓР¶Р°СЏСЃСЊ РІ РєРѕРЅС‚РµР№РЅРµСЂС‹ С‚РёРїРѕРІ.
    static void collectGenericTypeNames(Node* n,
                                        const std::function<void(const String&)>& cb) {
        if (!n) return;
        switch (n->type) {
            case NodeTypes::NodeScope:
                collectGenericTypeNames(static_cast<struct NodeScope*>(n)->node, cb);
                break;
            case NodeTypes::NodeVariable:
                cb(static_cast<struct NodeVariable*>(n)->name);
                break;
            case NodeTypes::NodeUnaryDereference:
                collectGenericTypeNames(static_cast<struct NodeUnaryDereference*>(n)->node, cb);
                break;
            case NodeTypes::NodeUnaryNegation:
                // `?T` (nullable-тип): обходим вложенный тип.
                collectGenericTypeNames(static_cast<struct NodeUnaryNegation*>(n)->node, cb);
                break;
            case NodeTypes::NodeBinaryBitwiseOr:
                // `A|B` (union-тип): обходим обе ветви.
                collectGenericTypeNames(static_cast<struct NodeBinaryBitwiseOr*>(n)->l_node, cb);
                collectGenericTypeNames(static_cast<struct NodeBinaryBitwiseOr*>(n)->r_node, cb);
                break;
            case NodeTypes::NodeArrayType:
                collectGenericTypeNames(static_cast<struct NodeArrayType*>(n)->element_type, cb);
                break;
            case NodeTypes::NodeArrayAccess:
                collectGenericTypeNames(static_cast<struct NodeArrayAccess*>(n)->base, cb);
                for (auto* ix : static_cast<struct NodeArrayAccess*>(n)->indices)
                    collectGenericTypeNames(ix, cb);
                break;
            case NodeTypes::NodeTupleType:
                for (auto* t : static_cast<struct NodeTupleType*>(n)->types)
                    collectGenericTypeNames(t, cb);
                break;
            case NodeTypes::NodeFunctionType: {
                auto* ft = static_cast<struct NodeFunctionType*>(n);
                for (auto* g : ft->generics) collectGenericTypeNames(g, cb);
                for (auto* p : ft->params)  collectGenericTypeNames(p, cb);
                collectGenericTypeNames(ft->return_type, cb);
                break;
            }
            default: break;
        }
    }

    // рекурсивный обход членов структуры/неймспейса: разворачивает NodeModifier,
    // NodeStatementsScope и NodeScope; для каждого члена вызывает cb(член, модификатор).
    // Модификаторы накапливаются по всей цепочке NodeModifier: у члена их может быть
    // несколько (`private static let x`), и потеря внутренних делала поле статическим
    // или наоборот — так `private static let` уезжал в structFields вместо staticFields.
    static void collectStructMembers(Node* n,
                                     const std::function<void(Node*, const String&)>& cb,
                                     const String& inheritedMod = String()) {
        n = unwrapMod(n);
        if (!n) return;
        if (n->type == NodeTypes::NodeStatementsScope) {
            for (auto* ch : static_cast<struct NodeStatementsScope*>(n)->nodes) {
                String mods = inheritedMod;
                for (Node* cc = ch; cc && cc->type == NodeTypes::NodeModifier;
                     cc = static_cast<struct NodeModifier*>(cc)->node) {
                    String nm = static_cast<struct NodeModifier*>(cc)->name;
                    if (!nm.empty() && mods.find(nm) == (size_t)-1)
                        mods += mods.empty() ? nm : String(",") + nm;
                }
                collectStructMembers(unwrapMod(ch), cb, mods);
            }
            return;
        }
        if (n->type == NodeTypes::NodeScope) {
            collectStructMembers(static_cast<struct NodeScope*>(n)->node, cb, inheritedMod);
            return;
        }
        cb(n, inheritedMod);
    }

    // содержит ли модификатор "static" (для отделения static-полей/методов)
    static bool hasStaticMod(const String& mods) {
        size_t pos = mods.find(String(U"static"));
        return pos != (size_t)-1;
    }

    void collectFrom(Node* top, const String& ns) {
        Node* n = unwrapMod(top);
        if (!n) return;
        if (n->type == NodeTypes::NodeEnumDeclaration) {
            auto* e = static_cast<struct NodeEnumDeclaration*>(n);
            std::vector<String> vs;
            for (auto& tok : e->variants) vs.push_back(tok.value);
            String qn = ns.empty() ? e->name : ns + "::" + e->name;
            enumMap[qn] = vs;
            enumMap[e->name] = std::move(vs);
            return;
        }
        if (n->type == NodeTypes::NodeNamespace) {
            auto* nsNode = static_cast<struct NodeNamespace*>(n);
            String full = ns.empty() ? nsNode->name : ns + "::" + nsNode->name;
            Node* body = unwrapMod(nsNode->body);
            if (body && body->type == NodeTypes::NodeStatementsScope) {
                for (auto* m : static_cast<struct NodeStatementsScope*>(body)->nodes) {
                    Node* u = m;
                    String mod;
                    if (u && u->type == NodeTypes::NodeModifier) {
                        mod = static_cast<struct NodeModifier*>(u)->name;
                        u   = static_cast<struct NodeModifier*>(u)->node;
                    }
                    u = unwrapMod(u);
                    if (u && u->type == NodeTypes::NodeVariableDeclaration) {
                        auto* vd = static_cast<struct NodeVariableDeclaration*>(u);
                        nsVars[full + "::" + vd->name] = vd;
                    } else if (u && u->type == NodeTypes::NodeFunctionDeclaration) {
                        registerFn(static_cast<struct NodeFunctionDeclaration*>(u), full, full);
                    } else {
                        collectFrom(m, full);
                    }
                }
            }
            return;
        }
        // namespace-значение в переменной (`let X = namespace { ... }`): функции и
        // переменные тела регистрируются как члены "X::" (зеркало ветки NodeNamespace)
        if (n->type == NodeTypes::NodeVariableDeclaration) {
            auto* vd = static_cast<struct NodeVariableDeclaration*>(n);
            Node* val = vd->value_node;
            while (val && val->type == NodeTypes::NodeScope)
                val = static_cast<struct NodeScope*>(val)->node;
            val = unwrapMod(val);
            if (val && val->type == NodeTypes::NodeNamespace) {
                String full = ns.empty() ? vd->name : ns + "::" + vd->name;
                Node* body = unwrapMod(static_cast<struct NodeNamespace*>(val)->body);
                if (body && body->type == NodeTypes::NodeStatementsScope) {
                    for (auto* m : static_cast<struct NodeStatementsScope*>(body)->nodes) {
                        Node* u = m;
                        if (u && u->type == NodeTypes::NodeModifier)
                            u = static_cast<struct NodeModifier*>(u)->node;
                        u = unwrapMod(u);
                        if (u && u->type == NodeTypes::NodeVariableDeclaration) {
                            auto* inner = static_cast<struct NodeVariableDeclaration*>(u);
                            nsVars[full + "::" + inner->name] = inner;
                        } else if (u && u->type == NodeTypes::NodeFunctionDeclaration) {
                            registerFn(static_cast<struct NodeFunctionDeclaration*>(u), full, full);
                        } else {
                            collectFrom(u, full);
                        }
                    }
                }
            }
            return;
        }
        if (n->type == NodeTypes::NodeStructDeclaration) {
            auto* st = static_cast<struct NodeStructDeclaration*>(n);
            String full = ns.empty() ? st->name : ns + "::" + st->name;
            Node* body = unwrapMod(st->body);
            if (body && body->type == NodeTypes::NodeStatementsScope) {
                // СЃР±РѕСЂ РїРѕР»РµР№ (РґР»СЏ РґРµС„РѕР»С‚РѕРІ РІ Р»РёС‚РµСЂР°Р»Р°С…)
                std::vector<std::pair<String, Node*>>& fields = structFields[full];
                std::vector<String>& stg = structGenerics[full];
                if (stg.empty() && !st->generics.empty())
                    for (auto& gp : st->generics) stg.push_back(gp.name);
                auto& gfm = structGenericField[full];
                collectStructMembers(body, [&](Node* m, const String& mod) {
                    if (m->type != NodeTypes::NodeVariableDeclaration) return;
                    auto* vd = static_cast<struct NodeVariableDeclaration*>(m);
                    if (hasStaticMod(mod)) {
                        staticFields[full].push_back({vd->name, vd->value_node});
                        return;
                    }
                    fields.push_back({vd->name, vd->value_node});
                    if (vd->type_node) structFieldTypes[full].push_back({vd->name, vd->type_node});
                    // generic-поле: тип поля содержит generic-имя структуры →
                    // запоминаем первое поле, типизированное этим параметром.
                    String fnme = vd->name;
                    collectGenericTypeNames(vd->type_node, [&](const String& gn) {
                        for (auto& gg : stg) if (gg == gn) {
                            if (!gfm.count(gn)) gfm[gn] = fnme;
                            break;
                        }
                    });
                });
                collectStructMembers(body, [&](Node* m, const String& mod) {
                    if (m->type != NodeTypes::NodeFunctionDeclaration) return;
                    auto* fn = static_cast<struct NodeFunctionDeclaration*>(m);
                    if (hasStaticMod(mod)) {
                        registerFn(fn, full, ns);
                    } else {
                        int idx = methodSeq[full + "::" + fn->name]++;
                        String suffix = idx ? "_" + String(std::to_string(idx)) : String("");
                        String cppm = "mf_" + cppName(full) + "_" + fn->name + suffix;
                        instanceRegistry[fn->name].push_back(FnSig{fn, cppm, ns, full});
                        // РґСЋРЅРґРµСЂС‹ (РёРјСЏ РЅР°С‡РёРЅР°РµС‚СЃСЏ Рё Р·Р°РєР°РЅС‡РёРІР°РµС‚СЃСЏ РЅР° __) в†’ СЂРµРіРёСЃС‚СЂРёСЂСѓРµРј
                        // РґР»СЏ РґРёСЃРїРµС‚С‡РµСЂР°. РЎРїРёСЃРѕРє РїРµСЂРµРіСЂСѓР·РѕРє + С‚РµРіРё РїР°СЂР°РјРµС‚СЂРѕРІ РїРѕСЃР»Рµ self:
                        // СЂР°РЅС‚Р°Р№Рј РІС‹Р±РёСЂР°РµС‚ РїРµСЂРµРіСЂСѓР·РєСѓ РїРѕ С„Р°РєС‚РёС‡РµСЃРєРёРј С‚РёРїР°Рј Р°СЂРіСѓРјРµРЅС‚РѕРІ.
                        String nm = fn->name;
                        bool isDunder = nm.size() >= 4
                                     && nm.substr(0, 2) == String(U"__")
                                     && nm.substr(nm.size() - 2) == String(U"__");
                        if (!isDunder) {
                            // Обычный метод: теги параметров после self нужны
                            // g_methods, чтобы rt::method_dispatch подобрал
                            // перегрузку по рантайм-типам аргументов.
                            std::vector<String> tags;
                            for (size_t ai = 1; ai < fn->params.size(); ++ai)
                                tags.push_back(mangledDunderTag(fn->params[ai], fn, st));
                            structMethods[full].push_back(DunderReg{nm, cppm, tags});
                        }
                        if (isDunder) {
                            std::vector<String> tags;
                            for (size_t ai = 1; ai < fn->params.size(); ++ai)
                                tags.push_back(mangledDunderTag(fn->params[ai], fn, st));
                            structDunders[full].push_back(DunderReg{nm, cppm, tags});
                        }
                    }
                });
            }
            return;
        }
        if (n->type == NodeTypes::NodeFunctionDeclaration) {
            auto* fn = static_cast<struct NodeFunctionDeclaration*>(n);
            registerFn(fn, String(), ns);
            collectLocalStructs(fn->body);
        }
    }

    // Локальные структуры (объявленные в теле функции): collectFrom обходит только
    // верхний уровень и namespace'ы, поэтому их поля и generic-параметры не
    // попадают в structFields/structGenerics, и литерал не получает значения по
    // умолчанию (поле выходит null). Регистрируем под голым именем — именно его
    // анализатор кладёт в NodeStructLiteral::struct_name.
    void collectLocalStructs(Node* body) {
        if (!body) return;
        Node* n = unwrapMod(body);
        if (!n) return;
        if (n->type == NodeTypes::NodeStructDeclaration) {
            collectFrom(n, String());
            return;
        }
        if (n->type == NodeTypes::NodeStatementsScope) {
            for (auto* m : static_cast<struct NodeStatementsScope*>(n)->nodes)
                collectLocalStructs(m);
        }
    }

    // С‚РµР»Рѕ С„СѓРЅРєС†РёРё: РѕР±С‰Р°СЏ С‡Р°СЃС‚СЊ СЌРјРёСЃСЃРёРё (РїР°СЂР°РјРµС‚СЂС‹ вЂ” C++-Р»РѕРєР°Р»СЊРЅС‹Рµ, Р»РѕРєР°Р»СЊРЅС‹Рµ
    // РїРµСЂРµРјРµРЅРЅС‹Рµ вЂ” СЃРІРѕР№ Ctx). РСЃРїРѕР»СЊР·СѓРµС‚СЃСЏ РґР»СЏ top-level С„СѓРЅРєС†РёР№ Рё Р·Р°РјС‹РєР°РЅРёР№.
    String fnBody(struct NodeFunctionDeclaration* fn, const String& ns, const String& owner = String()) {
        std::vector<String> savedParms = curParms;
        std::vector<String> savedRefs = curRefs;
        std::map<String, String> savedGen = curGenericVars;
          curParms.clear();
          curRefs.clear();
          curGenericVars.clear();
          struct NodeFunctionDeclaration* savedFnDecl = curFnDecl;
          curFnDecl = fn;
          String savedOwner = curStructOwner;
          curStructOwner = owner;
        String savedNs = curNs;
        curNs = ns;
        curReturnsRef = fn->returns_ref;
        String s;
        for (size_t i = 0; i < fn->params.size(); ++i) {
            s += "\n    rt::Box " + fn->params[i].name + " = args_[" + String(std::to_string(i)) + "];";
            curParms.push_back(fn->params[i].name);
            if (argIsRef(fn->params[i])) {
                curRefs.push_back(fn->params[i].name);
            } else {
                // value-параметр: динамический вызов передаёт переменные как живые rref;
                // снимаем их — параметр работает с копией значения (зеркало интерпретатора).
                s += "\n    if (" + fn->params[i].name + ".t == rt::Box::T_REF || "
                     + fn->params[i].name + ".t == rt::Box::T_REFI) "
                     + fn->params[i].name + " = rt::derefval(" + fn->params[i].name + ");";
            }
        }
        // РїСЂРёРІСЏР·РєРё generic-РїР°СЂР°РјРµС‚СЂРѕРІ (`as T` РІРЅСѓС‚СЂРё generic-РјРµС‚РѕРґРѕРІ): РєРѕРЅРєСЂРµС‚РЅС‹Р№ С‚РёРї
        // РІС‹РІРѕРґРёРј РёР· С„Р°РєС‚РёС‡РµСЃРєРёС… Р°СЂРіСѓРјРµРЅС‚РѕРІ вЂ” T СЃС‚СЂСѓРєС‚СѓСЂС‹ РёР· РїРѕР»СЏ self, P РёР· РїРѕР»СЏ
        // Р°СЂРіСѓРјРµРЅС‚Р° (Р·РµСЂРєР°Р»Рѕ РёРЅС‚РµСЂРїСЂРµС‚Р°С‚РѕСЂР°: СѓРЅРёС„РёРєР°С†РёСЏ РїРѕ С‚РёРїСѓ Р°СЂРіСѓРјРµРЅС‚Р°/РїРѕР»СЏ).
        if (!owner.empty()) {
            auto git = structGenerics.find(owner);
            if (git != structGenerics.end()) {
                auto gfm = structGenericField.find(owner);
                for (auto& g : git->second) {
                    if (gfm == structGenericField.end() || !gfm->second.count(g) || fn->params.empty())
                        continue;
                    String v = "gen_g" + String(std::to_string(genericSeq++));
                    s += "\n    rt::Box " + v + " = rt::Box::typ(rt::tyname(rt::field("
                       + fn->params[0].name + ", " + quote(gfm->second[g]) + ")));";
                    curGenericVars[g] = v;
                }
            }
        }
        // Привязка generic-параметров ФУНКЦИИ в порядке приоритета:
        //   (1) скрытый generic-value аргумент от call-site (явный `f[f64]()` или
        //       переданный по дедукции); null-заглушка означает «выведи сам»;
        //   (2) вывод из типизированного параметра (`x: T`);
        //   (3) зеркало resolve_function_call (Functions.cpp): если остался ровно
        //       ОДИН непривязанный тип-дженерик, берём тип первого нетипизированного
        //       параметра (`CompileTime::from[VT](value)` -> VT = типу value).
        std::vector<String> underived;
        for (auto& gp : fn->generics)
            if (!curGenericVars.count(gp.name) && genericArgCpp(gp.name, fn, !owner.empty()).empty())
                underived.push_back(gp.name);
        String untypedDerive;
        if (underived.size() == 1)
            for (size_t pi = (owner.empty() ? 0 : 1); pi < fn->params.size(); ++pi) {
                Node* pt = fn->params[pi].type;
                while (pt && pt->type == NodeTypes::NodeScope)
                    pt = static_cast<struct NodeScope*>(pt)->node;
                bool untyped = !pt || (pt->type == NodeTypes::NodeVariable
                    && (static_cast<struct NodeVariable*>(pt)->name == "auto"
                     || static_cast<struct NodeVariable*>(pt)->name == "any"));
                if (untyped) {
                    untypedDerive = "rt::Box::typ(rt::tyname(" + fn->params[pi].name + "))";
                    break;
                }
            }
        for (size_t gi = 0; gi < fn->generics.size(); ++gi) {
            const String& gname = fn->generics[gi].name;
            if (curGenericVars.count(gname)) continue;
            size_t hidx = fn->params.size() + gi;
            String idx = String(std::to_string(hidx));
            String cond = "args_.size() > " + idx + " && args_[" + idx + "].t != rt::Box::T_NULL";
            String ld = genericArgCpp(gname, fn, !owner.empty());
            String fall;
            if (!ld.empty()) fall = ld;
            else if (!underived.empty() && underived[0] == gname) fall = untypedDerive;
            String v = "gen_h" + String(std::to_string(genericSeq++));
            if (fall.empty())
                s += "\n    rt::Box " + v + " = " + cond + " ? args_[" + idx + "] : rt::Box::null();";
            else
                s += "\n    rt::Box " + v + " = (" + cond + ") ? args_[" + idx + "] : " + fall + ";";
            curGenericVars[gname] = v;
        }
        bool savedFn = inFn;
        inFn = true;
        String body;
        if (fn->body && fn->body->type == NodeTypes::NodeStatementsScope) {
            if (fn->return_type && !fn->returns_ref)
                body = branchValue(fn->body);   // неявный return последнего выражения (как у lambda)
            else
                body = emitStmt(fn->body);
        }
        else if (fn->body)
            body = branchValue(fn->body);
        inFn = savedFn;
        curNs = savedNs;
        curReturnsRef = false;
        if (!body.empty()) { s += "\n"; s += body; }
        curParms = savedParms;
        curRefs = savedRefs;
        curGenericVars = savedGen;
        curFnDecl = savedFnDecl;
        curStructOwner = savedOwner;
        return s;
    }

    String emitFn(struct NodeFunctionDeclaration* fn, const String& cpp,
                  const String& ns = String(), const String& owner = String()) {
        if (dbg) std::cout << "         " << Style("+ ", TerminalStyle::BRIGHT_GREEN) << "[cg::fn] " << cpp << " (owner=" << (owner.empty() ? "top" : owner) << ")" << std::endl;
        String s = "static rt::Box " + cpp + "(rt::Ctx ctx, const std::vector<rt::Box>& args_) {";
        s += fnBody(fn, ns, owner);
        s += "\n    return rt::Box::null();\n}";
        return s;
    }

    // РІР»РѕР¶РµРЅРЅР°СЏ С„СѓРЅРєС†РёСЏ вЂ” Р·Р°РјС‹РєР°РЅРёРµ (Р»РµРєСЃ-Р·Р°С…РІР°С‚ РєР°Рє РІ РёРЅС‚РµСЂРїСЂРµС‚Р°С‚РѕСЂРµ: func.captured).
    // РџР°СЂР°РјРµС‚СЂС‹ РѕР±СЉС‘РјР»СЋС‰РµР№ С„СѓРЅРєС†РёРё вЂ” C++-Р»РѕРєР°Р»СЊРЅС‹Рµ: РєРѕРїРёСЂСѓРµРј РёС… РІ ctx, Р·Р°С‚РµРј СЃРЅРёРјР°РµРј
    // СЃРЅРёРјРѕРє РєРѕРЅС‚РµРєСЃС‚Р° Рё РєР»Р°РґС‘Рј РІ env Р·Р°РјС‹РєР°РЅРёРµ rt::Box::clos. РўРµР»Рѕ Р·Р°РјС‹РєР°РЅРёСЏ РІРёРґРёС‚
    // СЃРЅРёРјРѕРє РїРѕ parent-С†РµРїРѕС‡РєРµ (ctx -> cap -> РѕР±СЉС‘РјР»СЋС‰РёР№ РєРѕРЅС‚РµРєСЃС‚ -> РіР»РѕР±Р°Р»СЊРЅС‹Р№).
    String emitNestedFn(struct NodeFunctionDeclaration* fn) {
        String s;
        for (const String& pm : curParms)
            s += "\n    " + envVar() + ".define(" + quote(pm) + ", " + pm + ");";
        String cap = "cap" + String(std::to_string(capSeq++));
        s += "\n    rt::Ctx " + cap + " = " + envVar() + ";";
        s += "\n    " + envVar() + ".define(" + quote(fn->name) + ", rt::Box::clos([" + cap
           + "](rt::Ctx, const std::vector<rt::Box>& args_) -> rt::Box {";
        s += "\n        rt::Ctx ctx;";
        s += "\n        ctx.parent = const_cast<rt::Ctx*>(&" + cap + ");";
        s += fnBody(fn, curNs);
        s += "\n        return rt::Box::null();\n    }));";
        return s;
    }

    // ------------------- СЃР±РѕСЂРєР° С„Р°Р№Р»Р° -------------------

    void generate(const String& out_path) {
        String code;
        code += RT_Prelude::PRELUDE;
        // C++-файлы, подключённые #cppinclude: их определения реализуют extern fn.
        // Инклудим сразу после lumen_rt.h, до всех объявлений, чтобы имена lf_* были видны.
        std::set<String> emitted_includes;
        for (const auto& inc : cpp_includes)
            if (emitted_includes.insert(inc).second)
                code += "#include \"" + inc + "\"\n";
        collectFunctions();
        collectLambdas();
        // РіР»РѕР±Р°Р»СЊРЅС‹Р№ РєРѕРЅС‚РµРєСЃС‚ (СЂРѕРґРёС‚РµР»СЊ РІСЃРµС… call-РєРѕРЅС‚РµРєСЃС‚РѕРІ)
        code += "\nnamespace rt { Ctx g_ctx; } // глобальный контекст программы (родитель всех call-контекстов)\n";
        // forward-РѕР±СЉСЏРІР»РµРЅРёСЏ РІСЃРµС… С„СѓРЅРєС†РёР№ (РІР·Р°РёРјРЅР°СЏ СЂРµРєСѓСЂСЃРёСЏ)
        code += "// forward-объявления всех функций (для взаимной рекурсии)\n";
        for (auto& kv : fnRegistry)
            for (auto& sig : kv.second)
                code += (sig.node && sig.node->is_extern ? String() : String("static "))
                      + "rt::Box " + sig.cpp + "(rt::Ctx, const std::vector<rt::Box>&);\n";
        for (auto& kv : lambdaMap)
            code += "static rt::Box " + kv.second.cpp + "(rt::Ctx, const std::vector<rt::Box>&);\n";
        for (auto& kv : instanceRegistry)
            for (auto& sig : kv.second)
                code += "static rt::Box " + sig.cpp + "(rt::Ctx, const std::vector<rt::Box>&);\n";
        // РѕРїСЂРµРґРµР»РµРЅРёСЏ
for (auto& kv : fnRegistry)
            for (auto& sig : kv.second) {
                if (sig.node && sig.node->is_extern) continue; // определение — в #cppinclude-файле
                code += "// функция " + kv.first + "\n";
                code += emitFn(sig.node, sig.cpp, sig.ns);
                code += "\n";
            }
        for (auto& kv : lambdaMap) {
            code += "// Р»СЏРјР±РґР°\n";
            code += emitFn(kv.second.node, kv.second.cpp);
            code += "\n";
        }
        for (auto& kv : instanceRegistry)
            for (auto& sig : kv.second) {
                code += "// метод " + kv.first + "\n";
                code += emitFn(sig.node, sig.cpp, sig.ns, sig.owner);
                code += "\n";
            }
        code += "\nint main() {\n";
        // СЂРµРіРёСЃС‚СЂР°С†РёСЏ РґСЋРЅРґРµСЂРѕРІ РІ g_dunders (РґРѕ Р»СЋР±РѕРіРѕ РёСЃРїРѕР»СЊР·РѕРІР°РЅРёСЏ РѕРїРµСЂР°С‚РѕСЂРѕРІ);
        // РїРµСЂРµРіСЂСѓР·РєРё вЂ” РѕС‚РґРµР»СЊРЅС‹РјРё Р·Р°РїРёСЃСЏРјРё СЃ С‚РµРіР°РјРё С‚РёРїРѕРІ РїР°СЂР°РјРµС‚СЂРѕРІ
        for (auto& sd : structDunders)
            for (auto& d : sd.second) {
                String tags = "std::vector<std::string>{";
                for (size_t ti = 0; ti < d.tags.size(); ++ti) {
                    if (ti) tags += ", ";
                    tags += quote(d.tags[ti]);
                }
                tags += "}";
                code += "    rt::reg_dunder(" + quote(sd.first) + ", " + quote(d.name)
                      + ", " + tags + ", &" + d.cpp + ");\n";
            }
        for (auto& sd : structMethods)
            for (auto& d : sd.second) {
                String tags = "std::vector<std::string>{";
                for (size_t ti = 0; ti < d.tags.size(); ++ti) {
                    if (ti) tags += ", ";
                    tags += quote(d.tags[ti]);
                }
                tags += "}";
                code += "    rt::reg_method(" + quote(sd.first) + ", " + quote(d.name)
                      + ", " + tags + ", &" + d.cpp + ");\n";
            }
        for (auto* node : tree) {
            // С„СѓРЅРєС†РёРё, СЃС‚СЂСѓРєС‚СѓСЂС‹ Рё enum СѓР¶Рµ СЌРјРёС‡РµРЅС‹ РѕС‚РґРµР»СЊРЅРѕ
            Node* n = node;
            while (n && n->type == NodeTypes::NodeModifier)
                n = static_cast<struct NodeModifier*>(n)->node;
            if (n && (n->type == NodeTypes::NodeFunctionDeclaration
                   || n->type == NodeTypes::NodeEnumDeclaration)) continue;
            String s = emitStmt(node);
            if (!s.empty()) {
                code += "    ";
                code += s;
                if (s[s.size() - 1] != U'\n') code += "\n";
            }
        }
        code += "    return 0;\n";
        code += "}\n";
        codeBytes = (long long)code.size();
        saveFile(out_path, code);
        if (dbg) printDebugSummary(out_path);
    }

    // РЎРІРѕРґРєР° РєРѕРґРѕРіРµРЅРµСЂР°С†РёРё РґР»СЏ -d: СЃРѕСЃС‚Р°РІ Рё TBD-Р·Р°РіР»СѓС€РєРё.
    void printDebugSummary(const String& out_path) {
        (void)out_path;
        if (tbdCount > 0) {
            std::cout << Style("warning: ", TerminalStyle::BRIGHT_YELLOW)
                      << Style(String(std::to_string(tbdCount))
                              + String(" unresolved codegen nodes remain (TBD); "
                                       "generated code may be partial.\n"), TerminalStyle::DIM_WHITE);
        }
    }

    // Сводка для строки "Emitting C++": число функций и объём кода.
    String emitStats() const {
        return String(std::to_string(fnRegistry.size() + lambdaMap.size() + instanceRegistry.size()))
             + String(" fn, ") + String(std::to_string(codeBytes)) + String(" bytes");
    }
};
