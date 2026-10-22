#include "ASTGenerator.cpp"

#include "core/Tokens.cpp"
#include "core/ErrorBase.cpp"

#include <vector>
#include <map>

struct ASTAnalizer {
    AbstractTreeT ast;
    ErrorContainer& error_container;
    std::vector<NodeTypes> ctx;   // СЃС‚РµРє РєРѕРЅС‚РµРєСЃС‚Р°: РІ РєР°РєРёРµ declaration РјС‹ РІР»РѕР¶РµРЅС‹

    ASTAnalizer(AbstractTreeT ast, ErrorContainer& error_container) : ast(ast), error_container(error_container) {}

    // РЅР°С…РѕРґРёРјСЃСЏ Р»Рё СЃРµР№С‡Р°СЃ РІРЅСѓС‚СЂРё С‚РµР»Р° struct (РґР»СЏ visibility-РјРѕРґРёС„РёРєР°С‚РѕСЂРѕРІ)
    bool insideStruct() const {
        for (auto t : ctx)
            if (t == NodeTypes::NodeStructDeclaration) return true;
        return false;
    }
    // РЅР°С…РѕРґРёРјСЃСЏ Р»Рё СЃРµР№С‡Р°СЃ РІРЅСѓС‚СЂРё С‚РµР»Р° namespace (private/public РґРѕРїСѓСЃС‚РёРјС‹ Рё С‚Р°Рј)
    bool insideNamespace() const {
        for (auto t : ctx)
            if (t == NodeTypes::NodeNamespace) return true;
        return false;
    }
    // РЅР°С…РѕРґРёРјСЃСЏ Р»Рё СЃРµР№С‡Р°СЃ РІРЅСѓС‚СЂРё С‚РµР»Р° С„СѓРЅРєС†РёРё (static-Р»РѕРєР°Р»СЊРЅС‹Рµ let)
    bool insideFunction() const {
        for (auto t : ctx)
            if (t == NodeTypes::NodeFunctionDeclaration) return true;
        return false;
    }

    // РґРѕРїСѓСЃС‚РёРјС‹Рµ С†РµР»Рё РјРѕРґРёС„РёРєР°С‚РѕСЂР° (РїРѕРІРµСЂС…РЅРѕСЃС‚РЅР°СЏ РїСЂРѕРІРµСЂРєР°)
    bool isModifierTarget(Node* s) const {
        if (!s) return false;
        switch (s->type) {
            case NodeTypes::NodeModifier:
            case NodeTypes::NodeVariableDeclaration:
            case NodeTypes::NodeStatementsScope:
            case NodeTypes::NodeFunctionDeclaration:
            case NodeTypes::NodeStructDeclaration:
            case NodeTypes::NodeNamespace:
                return true;
            default:
                return false;
        }
    }

    // РЎС‚СЂСѓРєС‚СѓСЂРЅР°СЏ РїСЂРѕРІРµСЂРєР° l-value РґР»СЏ `ref let x; of <РёСЃС‚РѕС‡РЅРёРє>`: РёСЃС‚РѕС‡РЅРёРє РґРѕР»Р¶РµРЅ
    // Р±С‹С‚СЊ Р°РґСЂРµСЃСѓРµРјС‹Рј РїСѓС‚С‘Рј (РїРµСЂРµРјРµРЅРЅР°СЏ, РїРѕР»Рµ, СЌР»РµРјРµРЅС‚, СЂР°Р·С‹РјРµРЅРѕРІР°РЅРёРµ, Ns::member,
    // РІС‹Р·РѕРІ, РІРѕР·РІСЂР°С‰Р°СЋС‰РёР№ СЃСЃС‹Р»РєСѓ). РљРѕРїРёСЂСѓСЋС‰Р°СЏ С„РѕСЂРјР° (`let x; of ...`) вЂ” Р±РµР· РѕРіСЂР°РЅРёС‡РµРЅРёР№.
    bool isRefPath(Node* s) const {
        if (!s) return false;
        while (s->type == NodeTypes::NodeScope)
            s = static_cast<struct NodeScope*>(s)->node;
        switch (s->type) {
            case NodeTypes::NodeVariable:
            case NodeTypes::NodeUnaryDereference:
            case NodeTypes::NodeFunctionCall:
            case NodeTypes::NodeBinaryScopeResolution:
                return true;
            case NodeTypes::NodeMemberAccess:
                return isRefPath(static_cast<struct NodeMemberAccess*>(s)->l_node);
            case NodeTypes::NodeArrayAccess:
                return isRefPath(static_cast<struct NodeArrayAccess*>(s)->base);
            default:
                return false;
        }
    }

    bool containsExit(Node* node, bool in_loop = false, bool in_func = false) {
        if (!node) return false;
        if (node->type == NodeTypes::NodeBreak)
            return !in_loop && !in_func;
        if (node->type == NodeTypes::NodeReturn)
            return !in_func;
        if (node->type == NodeTypes::NodeExit)
            return true; // exit вЂ” Р±РµР·СѓСЃР»РѕРІРЅРѕРµ Р·Р°РІРµСЂС€РµРЅРёРµ РІСЃРµР№ РїСЂРѕРіСЂР°РјРјС‹
        switch (node->type) {
            case NodeTypes::NodeStatementsScope:
                for (auto c : static_cast<struct NodeStatementsScope*>(node)->nodes)
                    if (containsExit(c, in_loop, in_func)) return true;
                return false;
            case NodeTypes::NodeScope:
                return containsExit(static_cast<struct NodeScope*>(node)->node, in_loop, in_func);
            case NodeTypes::NodeIf: {
                auto n = static_cast<struct NodeIf*>(node);
                return containsExit(n->then_body, in_loop, in_func) || containsExit(n->else_body, in_loop, in_func);
            }
            case NodeTypes::NodeWhile: {
                auto n = static_cast<struct NodeWhile*>(node);
                return containsExit(n->condition, in_loop, in_func) || containsExit(n->body, true, in_func);
            }
            case NodeTypes::NodeFor: {
                auto n = static_cast<struct NodeFor*>(node);
                return containsExit(n->init, in_loop, in_func) || containsExit(n->check, in_loop, in_func) ||
                       containsExit(n->update, in_loop, in_func) || containsExit(n->body, true, in_func);
            }
            case NodeTypes::NodeForEach: {
                auto n = static_cast<struct NodeForEach*>(node);
                return containsExit(n->source, in_loop, in_func) || containsExit(n->body, true, in_func);
            }
            case NodeTypes::NodeFunctionDeclaration:
                return containsExit(static_cast<struct NodeFunctionDeclaration*>(node)->body, in_loop, true);
            case NodeTypes::NodeStructDeclaration:
                return containsExit(static_cast<struct NodeStructDeclaration*>(node)->body, in_loop, in_func);
            case NodeTypes::NodeEnumDeclaration:
                return false;
            case NodeTypes::NodeNamespace:
                return containsExit(static_cast<struct NodeNamespace*>(node)->body, in_loop, in_func);
            case NodeTypes::NodeLambda:
                return containsExit(static_cast<struct NodeLambda*>(node)->body, in_loop, true);
            case NodeTypes::NodeVariableDeclaration:
                return false; // РЅРµ Р·Р°С…РѕРґРёРј РІ РІС‹СЂР°Р¶РµРЅРёСЏ РґРµРєР»Р°СЂР°С†РёР№
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
                Node* l = nullptr; Node* r = nullptr;
                switch (node->type) {
                    case NodeTypes::NodeBinaryMultiplicative: l = static_cast<struct NodeBinaryMultiplicative*>(node)->l_node; r = static_cast<struct NodeBinaryMultiplicative*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryDivision:       l = static_cast<struct NodeBinaryDivision*>(node)->l_node; r = static_cast<struct NodeBinaryDivision*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryRemainder:      l = static_cast<struct NodeBinaryRemainder*>(node)->l_node; r = static_cast<struct NodeBinaryRemainder*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryAdditive:       l = static_cast<struct NodeBinaryAdditive*>(node)->l_node; r = static_cast<struct NodeBinaryAdditive*>(node)->r_node; break;
                    case NodeTypes::NodeBinarySubtraction:    l = static_cast<struct NodeBinarySubtraction*>(node)->l_node; r = static_cast<struct NodeBinarySubtraction*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryShiftLeft:      l = static_cast<struct NodeBinaryShiftLeft*>(node)->l_node; r = static_cast<struct NodeBinaryShiftLeft*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryShiftRight:     l = static_cast<struct NodeBinaryShiftRight*>(node)->l_node; r = static_cast<struct NodeBinaryShiftRight*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryEqual:          l = static_cast<struct NodeBinaryEqual*>(node)->l_node; r = static_cast<struct NodeBinaryEqual*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryNotEqual:       l = static_cast<struct NodeBinaryNotEqual*>(node)->l_node; r = static_cast<struct NodeBinaryNotEqual*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryLess:           l = static_cast<struct NodeBinaryLess*>(node)->l_node; r = static_cast<struct NodeBinaryLess*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryGreater:        l = static_cast<struct NodeBinaryGreater*>(node)->l_node; r = static_cast<struct NodeBinaryGreater*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryLessEqual:      l = static_cast<struct NodeBinaryLessEqual*>(node)->l_node; r = static_cast<struct NodeBinaryLessEqual*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryGreaterEqual:   l = static_cast<struct NodeBinaryGreaterEqual*>(node)->l_node; r = static_cast<struct NodeBinaryGreaterEqual*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryBitwiseAnd:     l = static_cast<struct NodeBinaryBitwiseAnd*>(node)->l_node; r = static_cast<struct NodeBinaryBitwiseAnd*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryBitwiseOr:      l = static_cast<struct NodeBinaryBitwiseOr*>(node)->l_node; r = static_cast<struct NodeBinaryBitwiseOr*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryBitwiseXor:     l = static_cast<struct NodeBinaryBitwiseXor*>(node)->l_node; r = static_cast<struct NodeBinaryBitwiseXor*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryLogicalAnd:     l = static_cast<struct NodeBinaryLogicalAnd*>(node)->l_node; r = static_cast<struct NodeBinaryLogicalAnd*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryLogicalOr:      l = static_cast<struct NodeBinaryLogicalOr*>(node)->l_node; r = static_cast<struct NodeBinaryLogicalOr*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryNullCoalesce:  l = static_cast<struct NodeBinaryNullCoalesce*>(node)->l_node; r = static_cast<struct NodeBinaryNullCoalesce*>(node)->r_node; break;
                    default: break;
                }
                return containsExit(l) || containsExit(r);
            }
            case NodeTypes::NodeUnaryMinus:
            case NodeTypes::NodeUnaryPlus:
            case NodeTypes::NodeUnaryNegation:
            case NodeTypes::NodeUnaryDereference:
            case NodeTypes::NodeUnaryBitwiseNot:
            case NodeTypes::NodeUnaryAddressOf:
            case NodeTypes::NodeUnaryPostfixIncrement:
            case NodeTypes::NodeUnaryPostfixDecrement:
            case NodeTypes::NodeUnaryPrefixIncrement:
            case NodeTypes::NodeUnaryPrefixDecrement: {
                Node* c = nullptr;
                switch (node->type) {
                    case NodeTypes::NodeUnaryMinus:       c = static_cast<struct NodeUnaryMinus*>(node)->node; break;
                    case NodeTypes::NodeUnaryPlus:        c = static_cast<struct NodeUnaryPlus*>(node)->node; break;
                    case NodeTypes::NodeUnaryNegation:     c = static_cast<struct NodeUnaryNegation*>(node)->node; break;
                    case NodeTypes::NodeUnaryDereference:  c = static_cast<struct NodeUnaryDereference*>(node)->node; break;
                    case NodeTypes::NodeUnaryBitwiseNot:   c = static_cast<struct NodeUnaryBitwiseNot*>(node)->node; break;
                    case NodeTypes::NodeUnaryAddressOf:    c = static_cast<struct NodeUnaryAddressOf*>(node)->node; break;
                    case NodeTypes::NodeUnaryPostfixIncrement: c = static_cast<struct NodeUnaryPostfixIncrement*>(node)->node; break;
                    case NodeTypes::NodeUnaryPostfixDecrement: c = static_cast<struct NodeUnaryPostfixDecrement*>(node)->node; break;
                    case NodeTypes::NodeUnaryPrefixIncrement:  c = static_cast<struct NodeUnaryPrefixIncrement*>(node)->node; break;
                    case NodeTypes::NodeUnaryPrefixDecrement:  c = static_cast<struct NodeUnaryPrefixDecrement*>(node)->node; break;
                    default: break;
                }
                return containsExit(c);
            }
            case NodeTypes::NodeBinaryAssignment: {
                auto n = static_cast<struct NodeBinaryAssignment*>(node);
                return containsExit(n->l_node) || containsExit(n->r_node);
            }
            case NodeTypes::NodeFunctionCall: {
                auto n = static_cast<struct NodeFunctionCall*>(node);
                for (auto a : n->args) if (containsExit(a)) return true;
                for (auto g : n->generics) if (containsExit(g)) return true;
                return containsExit(n->callee);
            }
            case NodeTypes::NodeArrayAccess: {
                auto n = static_cast<struct NodeArrayAccess*>(node);
                for (auto i : n->indices) if (containsExit(i)) return true;
                return containsExit(n->base);
            }
            case NodeTypes::NodeArray:
                for (auto e : static_cast<struct NodeArray*>(node)->elements)
                    if (containsExit(e)) return true;
                return false;
            case NodeTypes::NodeMemberAccess: {
                auto n = static_cast<struct NodeMemberAccess*>(node);
                return containsExit(n->l_node) || containsExit(n->r_node);
            }
            case NodeTypes::NodeBinaryScopeResolution: {
                auto n = static_cast<struct NodeBinaryScopeResolution*>(node);
                return containsExit(n->l_node) || containsExit(n->r_node);
            }
            case NodeTypes::NodeCast: {
                auto n = static_cast<struct NodeCast*>(node);
                return containsExit(n->l_node) || containsExit(n->r_node);
            }
            case NodeTypes::NodeTypeOf:
                return containsExit(static_cast<struct NodeTypeOf*>(node)->expr);
            case NodeTypes::NodeMod:
                return containsExit(static_cast<struct NodeMod*>(node)->expr);
            case NodeTypes::NodeSizeOf:
                return containsExit(static_cast<struct NodeSizeOf*>(node)->expr);
            case NodeTypes::NodePtr:
                return containsExit(static_cast<struct NodePtr*>(node)->addr)
                    || containsExit(static_cast<struct NodePtr*>(node)->type);
            case NodeTypes::NodeNew:
                return containsExit(static_cast<struct NodeNew*>(node)->value);
            case NodeTypes::NodeOut:
                for (auto e : static_cast<struct NodeOut*>(node)->exprs)
                    if (containsExit(e)) return true;
                return false;
            case NodeTypes::NodeOutln:
                for (auto e : static_cast<struct NodeOutln*>(node)->exprs)
                    if (containsExit(e)) return true;
                return false;
            case NodeTypes::NodeDelete:
                if (containsExit(static_cast<struct NodeDelete*>(node)->expr)) return true;
                return static_cast<struct NodeDelete*>(node)->index
                    && containsExit(static_cast<struct NodeDelete*>(node)->index);
            case NodeTypes::NodeAssert:
                if (containsExit(static_cast<struct NodeAssert*>(node)->check)) return true;
                return static_cast<struct NodeAssert*>(node)->message
                    && containsExit(static_cast<struct NodeAssert*>(node)->message);
            default:
                return false;
        }
    }

    // СѓСЃР»РѕРІРёРµ С†РёРєР»Р° СЃС‚Р°С‚РёС‡РµСЃРєРё РёСЃС‚РёРЅРЅРѕ (while (true) / while (1) / while (1.0))
    bool isAlwaysTrue(Node* cond) {
        if (!cond) return false;
        if (cond->type == NodeTypes::NodeBool)
            return static_cast<struct NodeBool*>(cond)->value;
        if (cond->type == NodeTypes::NodeNumber) {
            auto n = static_cast<struct NodeNumber*>(cond);
            return n->is_integer ? (n->int_value != 0) : (n->float_value != 0.0L);
        }
        return false;
    }

    // СѓСЃР»РѕРІРёРµ СЃС‚Р°С‚РёС‡РµСЃРєРё Р»РѕР¶РЅРѕ (if(false)/while(false)/while(0)/while(0.0))
    bool isAlwaysFalse(Node* cond) {
        if (!cond) return false;
        if (cond->type == NodeTypes::NodeBool)
            return !static_cast<struct NodeBool*>(cond)->value;
        if (cond->type == NodeTypes::NodeNumber) {
            auto n = static_cast<struct NodeNumber*>(cond);
            return n->is_integer ? (n->int_value == 0) : (n->float_value == 0.0L);
        }
        return false;
    }

    // С‚РµР»Рѕ С†РёРєР»Р°/РІРµС‚РєРё РїСѓСЃС‚РѕРµ (Р±Р»РѕРє {} Р±РµР· РѕРїРµСЂР°С‚РѕСЂРѕРІ)
    bool isEmptyBody(Node* body) {
        if (!body) return false;
        if (body->type == NodeTypes::NodeStatementsScope)
            return static_cast<struct NodeStatementsScope*>(body)->nodes.empty();
        if (body->type == NodeTypes::NodeScope)
            return isEmptyBody(static_cast<struct NodeScope*>(body)->node);
        return false;
    }

    // СЃС‚СЂСѓРєС‚СѓСЂРЅРѕРµ СЂР°РІРµРЅСЃС‚РІРѕ РґРІСѓС… РІС‹СЂР°Р¶РµРЅРёР№ (РїРѕРІРµСЂС…РЅРѕСЃС‚РЅРѕ, Р±РµР· С‚РёРїРѕРІ/СЃРёРјРІРѕР»РѕРІ):
    // РёСЃРїРѕР»СЊР·СѓРµС‚СЃСЏ РґР»СЏ РѕР±РЅР°СЂСѓР¶РµРЅРёСЏ СЃР°РјРѕРїСЂРёСЃРІР°РёРІР°РЅРёСЏ a = a, std::p = std::p, a.b = a.b, arr[0] = arr[0]
    bool sameExpr(Node* a, Node* b) {
        if (!a && !b) return true;
        if (!a || !b) return false;
        if (a->type != b->type) return false;

        auto binLR = [](Node* n) -> std::pair<Node*, Node*> {
            switch (n->type) {
                case NodeTypes::NodeBinaryMultiplicative: { auto p = static_cast<struct NodeBinaryMultiplicative*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryDivision:       { auto p = static_cast<struct NodeBinaryDivision*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryRemainder:      { auto p = static_cast<struct NodeBinaryRemainder*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryAdditive:       { auto p = static_cast<struct NodeBinaryAdditive*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinarySubtraction:    { auto p = static_cast<struct NodeBinarySubtraction*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryShiftLeft:      { auto p = static_cast<struct NodeBinaryShiftLeft*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryShiftRight:     { auto p = static_cast<struct NodeBinaryShiftRight*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryEqual:          { auto p = static_cast<struct NodeBinaryEqual*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryNotEqual:       { auto p = static_cast<struct NodeBinaryNotEqual*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryLess:           { auto p = static_cast<struct NodeBinaryLess*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryGreater:        { auto p = static_cast<struct NodeBinaryGreater*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryLessEqual:      { auto p = static_cast<struct NodeBinaryLessEqual*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryGreaterEqual:   { auto p = static_cast<struct NodeBinaryGreaterEqual*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryBitwiseAnd:     { auto p = static_cast<struct NodeBinaryBitwiseAnd*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryBitwiseOr:      { auto p = static_cast<struct NodeBinaryBitwiseOr*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryBitwiseXor:     { auto p = static_cast<struct NodeBinaryBitwiseXor*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryLogicalAnd:     { auto p = static_cast<struct NodeBinaryLogicalAnd*>(n); return {p->l_node, p->r_node}; }
                  case NodeTypes::NodeBinaryLogicalOr:      { auto p = static_cast<struct NodeBinaryLogicalOr*>(n); return {p->l_node, p->r_node}; }
                  case NodeTypes::NodeBinaryNullCoalesce:  { auto p = static_cast<struct NodeBinaryNullCoalesce*>(n); return {p->l_node, p->r_node}; }
                case NodeTypes::NodeBinaryAssignment:     { auto p = static_cast<struct NodeBinaryAssignment*>(n); return {p->l_node, p->r_node}; }
                default: return {nullptr, nullptr};
            }
        };

        switch (a->type) {
            case NodeTypes::NodeVariable:
                return static_cast<struct NodeVariable*>(a)->name == static_cast<struct NodeVariable*>(b)->name;
            case NodeTypes::NodeNumber: {
                auto x = static_cast<struct NodeNumber*>(a), y = static_cast<struct NodeNumber*>(b);
                if (x->is_integer != y->is_integer) return false;
                return x->is_integer ? (x->int_value == y->int_value) : (x->float_value == y->float_value);
            }
            case NodeTypes::NodeBool:
                return static_cast<struct NodeBool*>(a)->value == static_cast<struct NodeBool*>(b)->value;
            case NodeTypes::NodeString:
                return static_cast<struct NodeString*>(a)->value == static_cast<struct NodeString*>(b)->value;
            case NodeTypes::NodeNull:
                return true;
            case NodeTypes::NodeBinaryScopeResolution: {
                auto x = static_cast<struct NodeBinaryScopeResolution*>(a), y = static_cast<struct NodeBinaryScopeResolution*>(b);
                return sameExpr(x->l_node, y->l_node) && sameExpr(x->r_node, y->r_node);
            }
            case NodeTypes::NodeMemberAccess: {
                auto x = static_cast<struct NodeMemberAccess*>(a), y = static_cast<struct NodeMemberAccess*>(b);
                return sameExpr(x->l_node, y->l_node) && sameExpr(x->r_node, y->r_node);
            }
            case NodeTypes::NodeCast: {
                auto x = static_cast<struct NodeCast*>(a), y = static_cast<struct NodeCast*>(b);
                return sameExpr(x->l_node, y->l_node) && sameExpr(x->r_node, y->r_node);
            }
            case NodeTypes::NodeTypeOf: {
                auto x = static_cast<struct NodeTypeOf*>(a), y = static_cast<struct NodeTypeOf*>(b);
                return x->is_static == y->is_static && sameExpr(x->expr, y->expr);
            }
            case NodeTypes::NodeMod: {
                auto x = static_cast<struct NodeMod*>(a), y = static_cast<struct NodeMod*>(b);
                return x->modifier == y->modifier && sameExpr(x->expr, y->expr);
            }
            case NodeTypes::NodeSizeOf: {
                auto x = static_cast<struct NodeSizeOf*>(a), y = static_cast<struct NodeSizeOf*>(b);
                return sameExpr(x->expr, y->expr);
            }
            case NodeTypes::NodePtr: {
                auto x = static_cast<struct NodePtr*>(a), y = static_cast<struct NodePtr*>(b);
                return sameExpr(x->addr, y->addr) && sameExpr(x->type, y->type);
            }
            case NodeTypes::NodeArrayAccess: {
                auto x = static_cast<struct NodeArrayAccess*>(a), y = static_cast<struct NodeArrayAccess*>(b);
                if (!sameExpr(x->base, y->base)) return false;
                if (x->indices.size() != y->indices.size()) return false;
                for (size_t i = 0; i < x->indices.size(); ++i)
                    if (!sameExpr(x->indices[i], y->indices[i])) return false;
                return true;
            }
            case NodeTypes::NodeArray: {
                auto x = static_cast<struct NodeArray*>(a), y = static_cast<struct NodeArray*>(b);
                if (x->elements.size() != y->elements.size()) return false;
                for (size_t i = 0; i < x->elements.size(); ++i)
                    if (!sameExpr(x->elements[i], y->elements[i])) return false;
                return true;
            }
            case NodeTypes::NodeFunctionCall: {
                auto x = static_cast<struct NodeFunctionCall*>(a), y = static_cast<struct NodeFunctionCall*>(b);
                if (!sameExpr(x->callee, y->callee)) return false;
                if (x->args.size() != y->args.size()) return false;
                for (size_t i = 0; i < x->args.size(); ++i)
                    if (!sameExpr(x->args[i], y->args[i])) return false;
                if (x->generics.size() != y->generics.size()) return false;
                for (size_t i = 0; i < x->generics.size(); ++i)
                    if (!sameExpr(x->generics[i], y->generics[i])) return false;
                return true;
            }
            case NodeTypes::NodeUnaryMinus:
            case NodeTypes::NodeUnaryPlus:
            case NodeTypes::NodeUnaryNegation:
            case NodeTypes::NodeUnaryDereference:
            case NodeTypes::NodeUnaryBitwiseNot:
            case NodeTypes::NodeUnaryAddressOf:
            case NodeTypes::NodeUnaryPostfixIncrement:
            case NodeTypes::NodeUnaryPostfixDecrement:
            case NodeTypes::NodeUnaryPrefixIncrement:
            case NodeTypes::NodeUnaryPrefixDecrement: {
                Node* ca = nullptr; Node* cb = nullptr;
                switch (a->type) {
                    case NodeTypes::NodeUnaryMinus:       ca = static_cast<struct NodeUnaryMinus*>(a)->node;       cb = static_cast<struct NodeUnaryMinus*>(b)->node; break;
                    case NodeTypes::NodeUnaryPlus:        ca = static_cast<struct NodeUnaryPlus*>(a)->node;        cb = static_cast<struct NodeUnaryPlus*>(b)->node; break;
                    case NodeTypes::NodeUnaryNegation:     ca = static_cast<struct NodeUnaryNegation*>(a)->node;     cb = static_cast<struct NodeUnaryNegation*>(b)->node; break;
                    case NodeTypes::NodeUnaryDereference:  ca = static_cast<struct NodeUnaryDereference*>(a)->node;  cb = static_cast<struct NodeUnaryDereference*>(b)->node; break;
                    case NodeTypes::NodeUnaryBitwiseNot:   ca = static_cast<struct NodeUnaryBitwiseNot*>(a)->node;   cb = static_cast<struct NodeUnaryBitwiseNot*>(b)->node; break;
                    case NodeTypes::NodeUnaryAddressOf:    ca = static_cast<struct NodeUnaryAddressOf*>(a)->node;    cb = static_cast<struct NodeUnaryAddressOf*>(b)->node; break;
                    case NodeTypes::NodeUnaryPostfixIncrement: ca = static_cast<struct NodeUnaryPostfixIncrement*>(a)->node; cb = static_cast<struct NodeUnaryPostfixIncrement*>(b)->node; break;
                    case NodeTypes::NodeUnaryPostfixDecrement: ca = static_cast<struct NodeUnaryPostfixDecrement*>(a)->node; cb = static_cast<struct NodeUnaryPostfixDecrement*>(b)->node; break;
                    case NodeTypes::NodeUnaryPrefixIncrement:  ca = static_cast<struct NodeUnaryPrefixIncrement*>(a)->node;  cb = static_cast<struct NodeUnaryPrefixIncrement*>(b)->node; break;
                    case NodeTypes::NodeUnaryPrefixDecrement:  ca = static_cast<struct NodeUnaryPrefixDecrement*>(a)->node;  cb = static_cast<struct NodeUnaryPrefixDecrement*>(b)->node; break;
                    default: break;
                }
                return sameExpr(ca, cb);
            }
            case NodeTypes::NodeScope:
                return sameExpr(static_cast<struct NodeScope*>(a)->node, static_cast<struct NodeScope*>(b)->node);
            case NodeTypes::NodeStatementsScope: {
                auto x = static_cast<struct NodeStatementsScope*>(a), y = static_cast<struct NodeStatementsScope*>(b);
                if (x->nodes.size() != y->nodes.size()) return false;
                for (size_t i = 0; i < x->nodes.size(); ++i)
                    if (!sameExpr(x->nodes[i], y->nodes[i])) return false;
                return true;
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
              case NodeTypes::NodeBinaryLogicalOr:
              case NodeTypes::NodeBinaryNullCoalesce:
              case NodeTypes::NodeBinaryAssignment: {
                auto [la, ra] = binLR(a);
                auto [lb, rb] = binLR(b);
                return sameExpr(la, lb) && sameExpr(ra, rb);
            }
            case NodeTypes::NodeVariableDeclaration: {
                auto x = static_cast<struct NodeVariableDeclaration*>(a), y = static_cast<struct NodeVariableDeclaration*>(b);
                if (x->name != y->name) return false;
                if (!sameExpr(x->value_node, y->value_node)) return false;
                if (!sameExpr(x->type_node, y->type_node)) return false;
                return true;
            }
            case NodeTypes::NodeReturn: {
                auto x = static_cast<struct NodeReturn*>(a), y = static_cast<struct NodeReturn*>(b);
                return sameExpr(x->expr, y->expr);
            }
            case NodeTypes::NodeAssert: {
                auto x = static_cast<struct NodeAssert*>(a), y = static_cast<struct NodeAssert*>(b);
                return sameExpr(x->check, y->check) && sameExpr(x->message, y->message);
            }
            default:
                return false;
        }
    }

    // С‚РѕРєРµРЅ РїРµСЂРІРѕР№ В«СЂРµР°Р»СЊРЅРѕР№В» РґРµРєР»Р°СЂР°С†РёРё РІ РєРѕСЂРЅРµ (РґР»СЏ РїРѕР·РёС†РёРё РѕС€РёР±РєРё РѕС‚СЃСѓС‚СЃС‚РІРёСЏ main)
    Token firstDeclarationToken() {
        for (auto node : ast) {
            Node* n = node;
            while (n && n->type == NodeTypes::NodeModifier)
                n = static_cast<struct NodeModifier*>(n)->node;
            if (!n) continue;
            switch (n->type) {
                case NodeTypes::NodeFunctionDeclaration:
                    return static_cast<struct NodeFunctionDeclaration*>(n)->name_token;
                case NodeTypes::NodeVariableDeclaration:
                    return static_cast<struct NodeVariableDeclaration*>(n)->name_token;
                case NodeTypes::NodeStructDeclaration:
                    return static_cast<struct NodeStructDeclaration*>(n)->name_token;
                case NodeTypes::NodeEnumDeclaration:
                    return static_cast<struct NodeEnumDeclaration*>(n)->name_token;
                case NodeTypes::NodeNamespace:
                    return static_cast<struct NodeNamespace*>(n)->name_token;
                default:
                    break;
            }
        }
        return Token();
    }

    /*
        semanticVisit вЂ” С‚РѕС‡РєР° РІС…РѕРґР° РІ РїРѕРІРµСЂС…РЅРѕСЃС‚РЅС‹Р№ (shallow) СЃРµРјР°РЅС‚РёС‡РµСЃРєРёР№ Р°РЅР°Р»РёР·, Pass 1.

        Р РµРєСѓСЂСЃРёРІРЅРѕ РѕР±С…РѕРґРёС‚ РІСЃС‘ РґРµСЂРµРІРѕ СЂР°Р·Р±РѕСЂР° (AST), РЅР°С‡РёРЅР°СЏ СЃ РєРѕСЂРЅСЏ, Рё РґР»СЏ РєР°Р¶РґРѕРіРѕ
        СѓР·Р»Р° РІС‹РїРѕР»РЅСЏРµС‚ РЅР°Р±РѕСЂ РґРµС€С‘РІС‹С… СЃС‚Р°С‚РёС‡РµСЃРєРёС… РїСЂРѕРІРµСЂРѕРє, РЅРµ С‚СЂРµР±СѓСЋС‰РёС… РїРѕСЃС‚СЂРѕРµРЅРёСЏ
        С‚Р°Р±Р»РёС†С‹ СЃРёРјРІРѕР»РѕРІ, СЂР°Р·СЂРµС€РµРЅРёСЏ С‚РёРїРѕРІ РёР»Рё Р°РЅР°Р»РёР·Р° РѕР±Р»Р°СЃС‚Рё РІРёРґРёРјРѕСЃС‚Рё. Р’СЃРµ РЅР°Р№РґРµРЅРЅС‹Рµ
        РїСЂРѕР±Р»РµРјС‹ РґРѕР±Р°РІР»СЏСЋС‚СЃСЏ РІ error_container С‡РµСЂРµР· С„Р°Р±СЂРёРєРё РёР· namespace SEMANTIC_ERRORS.
        РћР±С…РѕРґ РїСЂРѕРґРѕР»Р¶Р°РµС‚СЃСЏ РґРѕ РєРѕРЅС†Р° РґРµСЂРµРІР° (Р°РЅР°Р»РёР·Р°С‚РѕСЂ РЅРµ РѕСЃС‚Р°РЅР°РІР»РёРІР°РµС‚СЃСЏ РЅР° РїРµСЂРІРѕР№
        РѕС€РёР±РєРµ), РїРѕСЌС‚РѕРјСѓ Р·Р° РѕРґРёРЅ РїСЂРѕС…РѕРґ РјРѕР¶РЅРѕ СЃРѕР±СЂР°С‚СЊ СЃСЂР°Р·Сѓ РЅРµСЃРєРѕР»СЊРєРѕ СЃРѕРѕР±С‰РµРЅРёР№.

        Р§РўРћ РРњР•РќРќРћ РџР РћР’Р•Р РЇР•РўРЎРЇ (РїРѕ С‚РёРїР°Рј СѓР·Р»РѕРІ)
        ===========================================================================================
        NodeModifier (СЂРµР°Р»РёР·РѕРІР°РЅРѕ РІ modifiersCheck, РІС‹Р·С‹РІР°РµС‚СЃСЏ РёР· СЌС‚РѕРіРѕ case):
            - РґСѓР±Р»РёСЂСѓСЋС‰РёР№СЃСЏ РјРѕРґРёС„РёРєР°С‚РѕСЂ РІ РѕРґРЅРѕР№ С†РµРїРѕС‡РєРµ            -> DUPLICATE_MODIFIER (51)
            - РјРѕРґРёС„РёРєР°С‚РѕСЂ, РїСЂРёРјРµРЅС‘РЅРЅС‹Р№ РЅРµ Рє С‚РѕРјСѓ РІРёРґСѓ СѓР·Р»Р°         -> MODIFIER_MISPLACED (50)
            - РЅР° NodeVariableDeclaration:
                * strict С‚СЂРµР±СѓРµС‚ СЏРІРЅРѕРіРѕ С‚РёРїР°                        -> STRICT_REQUIRES_TYPE (53)
                * const С‚СЂРµР±СѓРµС‚ РёРЅРёС†РёР°Р»РёР·Р°С‚РѕСЂР° (Р·РЅР°С‡РµРЅРёСЏ)           -> CONST_REQUIRES_VALUE (54)
            - СЃРµРјР°РЅС‚РёРєР° РјРѕРґРёС„РёРєР°С‚РѕСЂР° РѕС‚РЅРѕСЃРёС‚РµР»СЊРЅРѕ С†РµР»Рё/РєРѕРЅС‚РµРєСЃС‚Р°   -> MODIFIER_SEMANTICS (62, Warn):
                * const/strict вЂ” С‚РѕР»СЊРєРѕ РґР»СЏ РїРµСЂРµРјРµРЅРЅС‹С… Рё namespace
                * final        вЂ” РґРѕРїСѓСЃС‚РёРј РІ Р»СЋР±РѕРј РєРѕРЅС‚РµРєСЃС‚Рµ (Р±РµР· РѕРіСЂР°РЅРёС‡РµРЅРёР№)
                * static       вЂ” С‚РѕР»СЊРєРѕ РґР»СЏ С‡Р»РµРЅРѕРІ struct (fn/let), РІРЅСѓС‚СЂРё struct (C++-style)
                 * private/publicвЂ” РІРЅСѓС‚СЂРё struct РР›Р namespace (СЃРј. ctx-СЃС‚РµРє)
          Р—Р°С‚РµРј СЂРµРєСѓСЂСЃРёРІРЅРѕ РѕР±С…РѕРґСЏС‚СЃСЏ РґРѕС‡РµСЂРЅРёРµ СѓР·Р»С‹ РјРѕРґРёС„РёРєР°С‚РѕСЂР°.

        NodeStatementsScope / С‚РµР»Р° С„СѓРЅРєС†РёР№ (NodeFunctionDeclaration), СЃС‚СЂСѓРєС‚СѓСЂ
          (NodeStructDeclaration), РїСЂРѕСЃС‚СЂР°РЅСЃС‚РІ РёРјС‘РЅ (NodeNamespace), Р»СЏРјР±Рґ (NodeLambda):
            РїСЂРѕСЃС‚Рѕ СЂРµРєСѓСЂСЃРёРІРЅРѕ РїРѕСЃРµС‰Р°СЋС‚СЃСЏ РІСЃРµ РІР»РѕР¶РµРЅРЅС‹Рµ СѓР·Р»С‹.

        NodeVariableDeclaration:
            СЂРµРєСѓСЂСЃРёРІРЅРѕ РѕР±С…РѕРґСЏС‚СЃСЏ value_node (РµСЃР»Рё РµСЃС‚СЊ вЂ” let x; Р±РµР· Р·РЅР°С‡РµРЅРёСЏ РґРѕРїСѓСЃС‚РёРјРѕ)
            Рё type_node (РµСЃР»Рё СѓРєР°Р·Р°РЅ).

        NodeWhile:
            - РµСЃР»Рё СѓСЃР»РѕРІРёРµ С†РёРєР»Р° СЃС‚Р°С‚РёС‡РµСЃРєРё РёСЃС‚РёРЅРЅРѕ (while(true)/while(1)/while(1.0),
              СЃРј. isAlwaysTrue) Р РІ С‚РµР»Рµ РЅРµС‚ РЅРё break, РЅРё ret, РІС‹С…РѕРґСЏС‰РёС… РёРјРµРЅРЅРѕ РёР· СЌС‚РѕРіРѕ
              С†РёРєР»Р°, РІС‹РґР°С‘С‚СЃСЏ РїСЂРµРґСѓРїСЂРµР¶РґРµРЅРёРµ                     -> INFINITE_LOOP (52, Warning)
            - СЂРµРєСѓСЂСЃРёРІРЅРѕ РѕР±С…РѕРґСЏС‚СЃСЏ СѓСЃР»РѕРІРёРµ Рё С‚РµР»Рѕ.

        NodeBinaryAssignment:
            - РµСЃР»Рё Р»РµРІР°СЏ Рё РїСЂР°РІР°СЏ С‡Р°СЃС‚Рё РїСЂРёСЃРІР°РёРІР°РЅРёСЏ СЃС‚СЂСѓРєС‚СѓСЂРЅРѕ РёРґРµРЅС‚РёС‡РЅС‹ (СЃРј. sameExpr:
              x = x, a.b = a.b, arr[0] = arr[0], std::p = std::p Рё С‚.Рї.), РІС‹РґР°С‘С‚СЃСЏ
              РїСЂРµРґСѓРїСЂРµР¶РґРµРЅРёРµ                                    -> SELF_ASSIGNMENT (55, Warning)

        NodeOut / NodeOutln:
            - РІС‹Р·РѕРІ Р±РµР· Р°СЂРіСѓРјРµРЅС‚РѕРІ (out; / out()) РёР»Рё СЃРѕ СЃРєРѕР±РєР°РјРё, РЅРѕ РїСѓСЃС‚С‹Рј СЃРїРёСЃРєРѕРј,
              РІС‹РґР°С‘С‚ РїСЂРµРґСѓРїСЂРµР¶РґРµРЅРёРµ                             -> EMPTY_OUTPUT (56, Warning)

        РћСЃС‚Р°Р»СЊРЅС‹Рµ РІС‹СЂР°Р¶РµРЅРёСЏ (NodeIf, NodeFor, NodeReturn, NodeBreak, NodeContinue,
          NodeNew, NodeArray, СѓРЅР°СЂРЅС‹Рµ Рё Р±РёРЅР°СЂРЅС‹Рµ РѕРїРµСЂР°С†РёРё, NodeBinaryScopeResolution,
          NodeMemberAccess, NodeCast, NodeFunctionCall, NodeArrayAccess, С‚РёРїС‹ РґР°РЅРЅС‹С…,
          NodeDelete, NodeScope Рё РїСЂ.):
            РЅРµ СЃРѕРґРµСЂР¶Р°С‚ СЃРѕР±СЃС‚РІРµРЅРЅС‹С… РїСЂРѕРІРµСЂРѕРє РЅР° СЌС‚РѕРј СЌС‚Р°РїРµ вЂ” РїСЂРѕРёР·РІРѕРґРёС‚СЃСЏ С‚РѕР»СЊРєРѕ
            СЂРµРєСѓСЂСЃРёРІРЅС‹Р№ СЃРїСѓСЃРє РІ РґРѕС‡РµСЂРЅРёРµ СѓР·Р»С‹, С‡С‚РѕР±С‹ РґРѕР№С‚Рё РґРѕ РІС‹С€РµРїРµСЂРµС‡РёСЃР»РµРЅРЅС‹С… СЃР»СѓС‡Р°РµРІ.

        Р§Р•Р“Рћ Р­РўРђ Р¤РђР—Рђ РќР• Р”Р•Р›РђР•Рў (РѕСЃС‚Р°С‘С‚СЃСЏ РЅР° РіР»СѓР±РѕРєСѓСЋ С„Р°Р·Сѓ, Pass 2 / M3вЂ“M6):
          - РїСЂРѕРІРµСЂРєСѓ СЃРѕРІРјРµСЃС‚РёРјРѕСЃС‚Рё С‚РёРїРѕРІ Рё РєРѕСЂСЂРµРєС‚РЅРѕСЃС‚Рё РѕРїРµСЂР°С†РёР№ РїРѕ С‚РёРїР°Рј;
          - РёСЃРїРѕР»СЊР·РѕРІР°РЅРёРµ РЅРµРёРЅРёС†РёР°Р»РёР·РёСЂРѕРІР°РЅРЅС‹С… / РЅРµСЃСѓС‰РµСЃС‚РІСѓСЋС‰РёС… РёРјС‘РЅ;
          - РїРµСЂРµРѕРїСЂРµРґРµР»РµРЅРёРµ/Р·Р°С‚РµРЅРµРЅРёРµ РёРјС‘РЅ, РѕР±Р»Р°СЃС‚Рё РІРёРґРёРјРѕСЃС‚Рё;
          - СЃРµРјР°РЅС‚РёС‡РµСЃРєРёРµ Р°Р»РёР°СЃС‹ (let y = &x; x = *y;) Рё РєРѕРЅСЃС‚Р°РЅС‚РЅС‹Р№ С„РѕР»Рґ-Р°РЅР°Р»РёР·;
          - РїСЂРѕРІРµСЂРєСѓ const-РјСѓС‚Р°С†РёРё РїРѕ СЂРµР°Р»СЊРЅС‹Рј СЃРёРјРІРѕР»Р°Рј.
        */
    // РїСЂРѕРІРµСЂРєРё, СЃРІСЏР·Р°РЅРЅС‹Рµ СЃ РјРѕРґРёС„РёРєР°С‚РѕСЂР°РјРё (РїРѕРІРµСЂС…РЅРѕСЃС‚РЅР°СЏ СЃРµРјР°РЅС‚РёРєР° Pass 1):
    // РґСѓР±Р»Рё РІ С†РµРїРѕС‡РєРµ, РЅРµРІРµСЂРЅРѕРµ РїСЂРёРјРµРЅРµРЅРёРµ Рє С†РµР»Рё, С‚СЂРµР±РѕРІР°РЅРёСЏ strict/const РЅР° РїРµСЂРµРјРµРЅРЅРѕР№.
    // РЎСЋРґР° Р¶Рµ РїРѕР·Р¶Рµ РґРѕР±Р°РІР»СЏСЋС‚СЃСЏ СЃРµРјР°РЅС‚РёС‡РµСЃРєРёРµ РїСЂР°РІРёР»Р° (const/strict/final РЅР° РЅСѓР¶РЅРѕР№ С†РµР»Рё,
    // static/public/private РІ РЅСѓР¶РЅРѕРј РєРѕРЅС‚РµРєСЃС‚Рµ).
    void modifiersCheck(Node* node) {
        if (!node || node->type != NodeTypes::NodeModifier) return;
        auto m = static_cast<struct NodeModifier*>(node);

        // СЃРѕР±СЂР°С‚СЊ РІСЃСЋ С†РµРїРѕС‡РєСѓ РІР»РѕР¶РµРЅРЅС‹С… РјРѕРґРёС„РёРєР°С‚РѕСЂРѕРІ Рё РЅР°Р№С‚Рё С†РµР»СЊ
        std::vector<String> mods;
        std::vector<Token> mod_tokens;
        Node* target = node;
        while (target && target->type == NodeTypes::NodeModifier) {
            auto mm = static_cast<struct NodeModifier*>(target);
            mods.push_back(mm->name);
            mod_tokens.push_back(mm->token);
            target = mm->node;
        }

        // РґСѓР±Р»РёСЂРѕРІР°РЅРёРµ РјРѕРґРёС„РёРєР°С‚РѕСЂРѕРІ (РЅР°РїСЂ. const const let x)
        std::map<String, bool> seen;
        for (size_t i = 0; i < mods.size(); ++i) {
            if (seen.count(mods[i])) {
                error_container.addError(SEMANTIC_ERRORS::DUPLICATE_MODIFIER(mod_tokens[i], mods[i]));
                break;   // РѕРґРЅР° РѕС€РёР±РєР° РЅР° С†РµРїРѕС‡РєСѓ
            }
            seen[mods[i]] = true;
        }

        // РЅРµРІРµСЂРЅРѕРµ РїСЂРёРјРµРЅРµРЅРёРµ: С†РµР»СЊ РЅРµ РґРµРєР»Р°СЂР°С†РёСЏ/Р±Р»РѕРє
        if (target && !isModifierTarget(target)) {
            error_container.addError(SEMANTIC_ERRORS::MODIFIER_MISPLACED(m->token, m->name));
        }

        // РјРѕРґРёС„РёРєР°С‚РѕСЂС‹ strict/const РЅР° С†РµР»Рё-РґРµРєР»Р°СЂР°С†РёРё РР›Р Р±Р»РѕРєРµ (strict { ... } / const { ... }):
        // С‚СЂРµР±РѕРІР°РЅРёРµ (СЏРІРЅС‹Р№ С‚РёРї / Р·РЅР°С‡РµРЅРёРµ) РїРµСЂРµРЅРѕСЃРёС‚СЃСЏ РЅР° РІСЃРµ РІР»РѕР¶РµРЅРЅС‹Рµ let-РѕР±СЉСЏРІР»РµРЅРёСЏ
        if (target && (target->type == NodeTypes::NodeVariableDeclaration ||
                       target->type == NodeTypes::NodeStatementsScope)) {
            bool has_strict = false, has_const = false;
            for (auto& mod : mods) {
                if (mod == "strict") has_strict = true;
                if (mod == "const")  has_const  = true;
            }
            auto checkVar = [&](Node* vd_node) {
                if (!vd_node || vd_node->type != NodeTypes::NodeVariableDeclaration) return;
                auto vd = static_cast<struct NodeVariableDeclaration*>(vd_node);
                if (has_strict && !vd->type_node)
                    error_container.addError(SEMANTIC_ERRORS::STRICT_REQUIRES_TYPE(vd->name_token));
                if (has_const && !vd->value_node)
                    error_container.addError(SEMANTIC_ERRORS::CONST_REQUIRES_VALUE(vd->name_token));
            };
            if (target->type == NodeTypes::NodeVariableDeclaration) {
                checkVar(target);
            } else {
                // Р±Р»РѕРє: СЃРїСѓСЃРєР°РµРјСЃСЏ РІРѕ РІР»РѕР¶РµРЅРЅС‹Рµ let/Р±Р»РѕРєРё/С„СѓРЅРєС†РёРё/СЃС‚СЂСѓРєС‚СѓСЂС‹/РїСЂРѕСЃС‚СЂР°РЅСЃС‚РІР° РёРјС‘РЅ
                std::vector<Node*> stack{ target };
                while (!stack.empty()) {
                    Node* n = stack.back(); stack.pop_back();
                    switch (n->type) {
                        case NodeTypes::NodeStatementsScope:
                            for (auto c : static_cast<struct NodeStatementsScope*>(n)->nodes) stack.push_back(c);
                            break;
                        case NodeTypes::NodeModifier:
                            stack.push_back(static_cast<struct NodeModifier*>(n)->node);
                            break;
                        case NodeTypes::NodeVariableDeclaration:
                            checkVar(n);
                            break;
                        case NodeTypes::NodeFunctionDeclaration:
                            stack.push_back(static_cast<struct NodeFunctionDeclaration*>(n)->body);
                            break;
                        case NodeTypes::NodeStructDeclaration:
                            stack.push_back(static_cast<struct NodeStructDeclaration*>(n)->body);
                            break;
                        case NodeTypes::NodeNamespace:
                            stack.push_back(static_cast<struct NodeNamespace*>(n)->body);
                            break;
                        default: break;
                    }
                }
            }
        }

        // СЃРµРјР°РЅС‚РёРєР° РјРѕРґРёС„РёРєР°С‚РѕСЂРѕРІ: Рє С‡РµРјСѓ РїСЂРёРјРµРЅРёРј Рё РІ РєР°РєРѕРј РєРѕРЅС‚РµРєСЃС‚Рµ
        if (target) {
            for (size_t i = 0; i < mods.size(); ++i) {
                const String& mod = mods[i];
                Token mtok = mod_tokens[i];
                if (mod == "const") {
                    // `const fn` вЂ” read-only РјРµС‚РѕРґ СЃС‚СЂСѓРєС‚СѓСЂС‹ (РЅРµ РјРѕР¶РµС‚ РјРµРЅСЏС‚СЊ self)
                    bool is_const_method = target->type == NodeTypes::NodeFunctionDeclaration
                                           && insideStruct();
                    if (target->type != NodeTypes::NodeVariableDeclaration &&
                        target->type != NodeTypes::NodeNamespace &&
                        target->type != NodeTypes::NodeStatementsScope &&
                        !is_const_method)
                        error_container.addError(SEMANTIC_ERRORS::MODIFIER_SEMANTICS(
                            mtok, mod, "const is meaningful only for variables, namespaces, blocks and struct methods"));
                } else if (mod == "strict") {
                    if (target->type != NodeTypes::NodeVariableDeclaration &&
                        target->type != NodeTypes::NodeFunctionDeclaration &&
                        target->type != NodeTypes::NodeNamespace &&
                        target->type != NodeTypes::NodeStatementsScope)
                        error_container.addError(SEMANTIC_ERRORS::MODIFIER_SEMANTICS(
                            mtok, mod, "strict is meaningful only for variables, functions, namespaces and blocks"));
                } else if (mod == "static") {
                    bool is_member = (target->type == NodeTypes::NodeFunctionDeclaration ||
                                      target->type == NodeTypes::NodeVariableDeclaration);
                    bool in_struct_member = insideStruct() && is_member;
                    bool in_func_local = insideFunction() &&
                                         target->type == NodeTypes::NodeVariableDeclaration;
                    if (!in_struct_member && !in_func_local)
                        error_container.addError(SEMANTIC_ERRORS::MODIFIER_SEMANTICS(
                            mtok, mod, "static is allowed only for members (fn/let) inside a struct, "
                                      "or for a local 'let' inside a function body"));
                } else if (mod == "ref") {
                    // ref-РґРµРєР»Р°СЂР°С†РёСЏ РѕР±СЏР·Р°РЅР° Р±С‹С‚СЊ `ref let name = <lvalue>;`
                    if (target->type != NodeTypes::NodeVariableDeclaration)
                        error_container.addError(SEMANTIC_ERRORS::MODIFIER_SEMANTICS(
                            mtok, mod, "ref is allowed only on let-declarations ('ref let name = <lvalue>;')"));
                    // ref РЅРµР»СЊР·СЏ РєРѕРјР±РёРЅРёСЂРѕРІР°С‚СЊ СЃРѕ static/private/final (РїР»Р°РЅ В§8)
                    for (size_t j = 0; j < mods.size(); ++j) {
                        if (j == i) continue;
                        if (mods[j] == "static" || mods[j] == "private" || mods[j] == "final")
                            error_container.addError(SEMANTIC_ERRORS::MODIFIER_SEMANTICS(
                                mtok, mod, "ref cannot be combined with '" + mods[j] + "'"));
                    }
                } else if (mod == "private" || mod == "public") {
                    if (!insideStruct() && !insideNamespace())
                        error_container.addError(SEMANTIC_ERRORS::MODIFIER_SEMANTICS(
                            mtok, mod, (mod + " is allowed only inside a struct or namespace declaration")));
                }
            }
        }

        // СЂРµРєСѓСЂСЃРёСЏ СЃСЂР°Р·Сѓ РІ С†РµР»СЊ, Р±РµР· РїРѕРІС‚РѕСЂРЅРѕРіРѕ РѕР±С…РѕРґР° РјРѕРґРёС„РёРєР°С‚РѕСЂРѕРІ
        // (РёРЅР°С‡Рµ С†РµРїРѕС‡РєР° РїСЂРѕРІРµСЂСЏР»Р°СЃСЊ Р±С‹ РЅРµСЃРєРѕР»СЊРєРѕ СЂР°Р·)
        semanticVisit(target);
    }

    void semanticVisit(Node* node) {
        if (!node) return;
        switch (node->type) {
            case NodeTypes::NodeModifier: {
                modifiersCheck(node);
                break;
            }
            case NodeTypes::NodeStatementsScope: {
                auto n = static_cast<struct NodeStatementsScope*>(node);
                for (auto c : n->nodes) semanticVisit(c);
                break;
            }
            case NodeTypes::NodeFunctionDeclaration: {
                ctx.push_back(NodeTypes::NodeFunctionDeclaration);
                semanticVisit(static_cast<struct NodeFunctionDeclaration*>(node)->body);
                ctx.pop_back();
                break;
            }
            case NodeTypes::NodeStructDeclaration: {
                ctx.push_back(NodeTypes::NodeStructDeclaration);
                semanticVisit(static_cast<struct NodeStructDeclaration*>(node)->body);
                ctx.pop_back();
                break;
            }
            case NodeTypes::NodeEnumDeclaration:
                break;
            case NodeTypes::NodeNamespace: {
                ctx.push_back(NodeTypes::NodeNamespace);
                semanticVisit(static_cast<struct NodeNamespace*>(node)->body);
                ctx.pop_back();
                break;
            }
            case NodeTypes::NodeVariableDeclaration: {
                auto n = static_cast<struct NodeVariableDeclaration*>(node);
                semanticVisit(n->value_node);
                semanticVisit(n->type_node);
                break;
            }

            // ---- СѓРїСЂР°РІР»РµРЅРёРµ РїРѕС‚РѕРєРѕРј ----
            case NodeTypes::NodeIf: {
                auto n = static_cast<struct NodeIf*>(node);
                // РєРѕРЅСЃС‚Р°РЅС‚РЅРѕРµ СѓСЃР»РѕРІРёРµ if(true)/if(false) -> РѕРґРЅР° РІРµС‚РєР° РјРµСЂС‚РІР°
                if (isAlwaysTrue(n->condition) || isAlwaysFalse(n->condition)) {
                    error_container.addError(SEMANTIC_ERRORS::CONSTANT_CONDITION(n->token));
                }
                // РїСѓСЃС‚РѕРµ С‚РµР»Рѕ then/else
                if (isEmptyBody(n->then_body))
                    error_container.addError(SEMANTIC_ERRORS::EMPTY_BODY(n->token));
                // РѕРґРёРЅР°РєРѕРІС‹Рµ РІРµС‚РєРё then/else
                if (n->else_body && sameExpr(n->then_body, n->else_body)) {
                    error_container.addError(SEMANTIC_ERRORS::IDENTICAL_BRANCHES(n->token));
                }
                semanticVisit(n->condition);
                semanticVisit(n->then_body);
                semanticVisit(n->else_body);
                break;
            }
            case NodeTypes::NodeWhile: {
                auto n = static_cast<struct NodeWhile*>(node);
                if (isAlwaysTrue(n->condition) && !containsExit(n->body)) {
                    error_container.addError(SEMANTIC_ERRORS::INFINITE_LOOP(n->token));
                } else if (isAlwaysFalse(n->condition)) {
                    error_container.addError(SEMANTIC_ERRORS::CONSTANT_CONDITION(n->token));
                }
                if (isEmptyBody(n->body))
                    error_container.addError(SEMANTIC_ERRORS::EMPTY_BODY(n->token));
                semanticVisit(n->condition);
                semanticVisit(n->body);
                break;
            }
            case NodeTypes::NodeFor: {
                auto n = static_cast<struct NodeFor*>(node);
                if (n->check && isAlwaysFalse(n->check)) {
                    error_container.addError(SEMANTIC_ERRORS::CONSTANT_CONDITION(n->token));
                }
                if (isEmptyBody(n->body))
                    error_container.addError(SEMANTIC_ERRORS::EMPTY_BODY(n->token));
                semanticVisit(n->init);
                semanticVisit(n->check);
                semanticVisit(n->update);
                semanticVisit(n->body);
                break;
            }
            case NodeTypes::NodeForEach: {
                auto n = static_cast<struct NodeForEach*>(node);
                if (isEmptyBody(n->body))
                    error_container.addError(SEMANTIC_ERRORS::EMPTY_BODY(n->token));
                if (n->by_ref && !isRefPath(n->source)) {
                    error_container.addError(SEMANTIC_ERRORS::REF_BAD_TARGET(
                        n->decl ? n->decl->name_token : n->of_token,
                        n->decl ? n->decl->name : String("element")));
                }
                semanticVisit(n->decl);
                semanticVisit(n->source);
                semanticVisit(n->body);
                break;
            }
            case NodeTypes::NodeReturn:
                semanticVisit(static_cast<struct NodeReturn*>(node)->expr);
                break;
            case NodeTypes::NodeExit:
                semanticVisit(static_cast<struct NodeExit*>(node)->expr);
                break;
            case NodeTypes::NodeAppend: {
                auto n = static_cast<struct NodeAppend*>(node);
                semanticVisit(n->array_expr);
                semanticVisit(n->value_expr);
                break;
            }
            case NodeTypes::NodeReserve: {
                auto n = static_cast<struct NodeReserve*>(node);
                semanticVisit(n->array_expr);
                semanticVisit(n->count_expr);
                break;
            }
            case NodeTypes::NodeAssert: {
                auto n = static_cast<struct NodeAssert*>(node);
                semanticVisit(n->check);
                semanticVisit(n->message);
                break;
            }
            case NodeTypes::NodeScope:
                semanticVisit(static_cast<struct NodeScope*>(node)->node);
                break;

            // ---- РІС‹СЂР°Р¶РµРЅРёСЏ ----
            case NodeTypes::NodeBinaryAssignment: {
                auto n = static_cast<struct NodeBinaryAssignment*>(node);
                // D1: СЃР°РјРѕРїСЂРёСЃРІР°РёРІР°РЅРёРµ (СЃС‚СЂСѓРєС‚СѓСЂРЅРѕ СЂР°РІРЅС‹Рµ Р»РµРІР°СЏ Рё РїСЂР°РІР°СЏ С‡Р°СЃС‚Рё)
                if (n->l_node && n->r_node && sameExpr(n->l_node, n->r_node)) {
                    error_container.addError(SEMANTIC_ERRORS::SELF_ASSIGNMENT(n->start_token, n->end_token));
                }
                semanticVisit(n->l_node);
                semanticVisit(n->r_node);
                break;
            }
            case NodeTypes::NodeBinaryScopeResolution: {
                auto n = static_cast<struct NodeBinaryScopeResolution*>(node);
                semanticVisit(n->l_node);
                semanticVisit(n->r_node);
                break;
            }
            case NodeTypes::NodeMemberAccess: {
                auto n = static_cast<struct NodeMemberAccess*>(node);
                semanticVisit(n->l_node);
                semanticVisit(n->r_node);
                break;
            }
            case NodeTypes::NodeCast: {
                auto n = static_cast<struct NodeCast*>(node);
                semanticVisit(n->l_node);
                semanticVisit(n->r_node);
                break;
            }
            case NodeTypes::NodeFunctionCall: {
                auto n = static_cast<struct NodeFunctionCall*>(node);
                semanticVisit(n->callee);
                for (auto a : n->args) semanticVisit(nodeIsNamedArgument(a)
                    ? static_cast<struct NodeNamedArgument*>(a)->value : a);
                for (auto g : n->generics) semanticVisit(g);
                break;
            }
            case NodeTypes::NodeNamedArgument: {
                auto n = static_cast<struct NodeNamedArgument*>(node);
                if (n->value) semanticVisit(n->value);
                break;
            }
            case NodeTypes::NodeArrayAccess: {
                auto n = static_cast<struct NodeArrayAccess*>(node);
                semanticVisit(n->base);
                for (auto i : n->indices) semanticVisit(i);
                break;
            }
            case NodeTypes::NodeArray:
                for (auto e : static_cast<struct NodeArray*>(node)->elements) semanticVisit(e);
                break;
            case NodeTypes::NodeLambda:
                semanticVisit(static_cast<struct NodeLambda*>(node)->body);
                break;
            case NodeTypes::NodeNew: {
                auto n = static_cast<struct NodeNew*>(node);
                semanticVisit(n->value);
                for (auto& mod : n->modifiers) semanticVisit(mod.type_arg);
                break;
            }
            case NodeTypes::NodeOut: {
                auto n = static_cast<struct NodeOut*>(node);
                // D2: out() Р±РµР· Р°СЂРіСѓРјРµРЅС‚РѕРІ
                if (n->exprs.empty())
                    error_container.addError(SEMANTIC_ERRORS::EMPTY_OUTPUT(n->token));
                for (auto e : n->exprs) semanticVisit(e);
                break;
            }
            case NodeTypes::NodeOutln: {
                auto n = static_cast<struct NodeOutln*>(node);
                // D2: outln() Р±РµР· Р°СЂРіСѓРјРµРЅС‚РѕРІ
                if (n->exprs.empty())
                    error_container.addError(SEMANTIC_ERRORS::EMPTY_OUTPUT(n->token));
                for (auto e : n->exprs) semanticVisit(e);
                break;
            }
            case NodeTypes::NodeDelete: {
                auto* dn = static_cast<struct NodeDelete*>(node);
                semanticVisit(dn->expr);
                if (dn->index) semanticVisit(dn->index);
                break;
            }
            case NodeTypes::NodeTypeOf:
                semanticVisit(static_cast<struct NodeTypeOf*>(node)->expr);
                break;
            case NodeTypes::NodeMod:
                semanticVisit(static_cast<struct NodeMod*>(node)->expr);
                break;
            case NodeTypes::NodeSizeOf:
                semanticVisit(static_cast<struct NodeSizeOf*>(node)->expr);
                break;
            case NodeTypes::NodePtr:
                semanticVisit(static_cast<struct NodePtr*>(node)->addr);
                semanticVisit(static_cast<struct NodePtr*>(node)->type);
                break;
            case NodeTypes::NodeInput:
                semanticVisit(static_cast<struct NodeInput*>(node)->prompt);
                break;

            // ---- СѓРЅР°СЂРЅС‹Рµ РѕРїРµСЂР°С†РёРё ----
            case NodeTypes::NodeUnaryMinus:
            case NodeTypes::NodeUnaryPlus:
            case NodeTypes::NodeUnaryNegation:
            case NodeTypes::NodeUnaryDereference:
            case NodeTypes::NodeUnaryBitwiseNot:
            case NodeTypes::NodeUnaryAddressOf:
            case NodeTypes::NodeUnaryPostfixIncrement:
            case NodeTypes::NodeUnaryPostfixDecrement:
            case NodeTypes::NodeUnaryPrefixIncrement:
            case NodeTypes::NodeUnaryPrefixDecrement: {
                Node* child = nullptr;
                switch (node->type) {
                    case NodeTypes::NodeUnaryMinus:        child = static_cast<struct NodeUnaryMinus*>(node)->node; break;
                    case NodeTypes::NodeUnaryPlus:         child = static_cast<struct NodeUnaryPlus*>(node)->node; break;
                    case NodeTypes::NodeUnaryNegation: {
                auto self = static_cast<struct NodeUnaryNegation*>(node);
                Node* inner = self->node;
                while (inner && inner->type == NodeTypes::NodeScope)
                    inner = static_cast<struct NodeScope*>(inner)->node;
                if (inner && inner->type == NodeTypes::NodeUnaryNegation) {
                    error_container.addError(SEMANTIC_ERRORS::DOUBLE_NEGATION(self->token));
                }
                child = self->node;
                break;
            }
                    case NodeTypes::NodeUnaryDereference:  child = static_cast<struct NodeUnaryDereference*>(node)->node; break;
                    case NodeTypes::NodeUnaryBitwiseNot:   child = static_cast<struct NodeUnaryBitwiseNot*>(node)->node; break;
                    case NodeTypes::NodeUnaryAddressOf:    child = static_cast<struct NodeUnaryAddressOf*>(node)->node; break;
                    case NodeTypes::NodeUnaryPostfixIncrement: child = static_cast<struct NodeUnaryPostfixIncrement*>(node)->node; break;
                    case NodeTypes::NodeUnaryPostfixDecrement: child = static_cast<struct NodeUnaryPostfixDecrement*>(node)->node; break;
                    case NodeTypes::NodeUnaryPrefixIncrement:  child = static_cast<struct NodeUnaryPrefixIncrement*>(node)->node; break;
                    case NodeTypes::NodeUnaryPrefixDecrement:  child = static_cast<struct NodeUnaryPrefixDecrement*>(node)->node; break;
                    default: break;
                }
                semanticVisit(child);
                break;
            }

            // ---- Р±РёРЅР°СЂРЅС‹Рµ РѕРїРµСЂР°С†РёРё ----
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
                Node* l = nullptr; Node* r = nullptr;
                switch (node->type) {
                    case NodeTypes::NodeBinaryMultiplicative: l = static_cast<struct NodeBinaryMultiplicative*>(node)->l_node; r = static_cast<struct NodeBinaryMultiplicative*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryDivision:       l = static_cast<struct NodeBinaryDivision*>(node)->l_node; r = static_cast<struct NodeBinaryDivision*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryRemainder:      l = static_cast<struct NodeBinaryRemainder*>(node)->l_node; r = static_cast<struct NodeBinaryRemainder*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryAdditive:       l = static_cast<struct NodeBinaryAdditive*>(node)->l_node; r = static_cast<struct NodeBinaryAdditive*>(node)->r_node; break;
                    case NodeTypes::NodeBinarySubtraction:    l = static_cast<struct NodeBinarySubtraction*>(node)->l_node; r = static_cast<struct NodeBinarySubtraction*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryShiftLeft:      l = static_cast<struct NodeBinaryShiftLeft*>(node)->l_node; r = static_cast<struct NodeBinaryShiftLeft*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryShiftRight:     l = static_cast<struct NodeBinaryShiftRight*>(node)->l_node; r = static_cast<struct NodeBinaryShiftRight*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryEqual:          l = static_cast<struct NodeBinaryEqual*>(node)->l_node; r = static_cast<struct NodeBinaryEqual*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryNotEqual:       l = static_cast<struct NodeBinaryNotEqual*>(node)->l_node; r = static_cast<struct NodeBinaryNotEqual*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryLess:           l = static_cast<struct NodeBinaryLess*>(node)->l_node; r = static_cast<struct NodeBinaryLess*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryGreater:        l = static_cast<struct NodeBinaryGreater*>(node)->l_node; r = static_cast<struct NodeBinaryGreater*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryLessEqual:      l = static_cast<struct NodeBinaryLessEqual*>(node)->l_node; r = static_cast<struct NodeBinaryLessEqual*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryGreaterEqual:   l = static_cast<struct NodeBinaryGreaterEqual*>(node)->l_node; r = static_cast<struct NodeBinaryGreaterEqual*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryBitwiseAnd:     l = static_cast<struct NodeBinaryBitwiseAnd*>(node)->l_node; r = static_cast<struct NodeBinaryBitwiseAnd*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryBitwiseOr:      l = static_cast<struct NodeBinaryBitwiseOr*>(node)->l_node; r = static_cast<struct NodeBinaryBitwiseOr*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryBitwiseXor:     l = static_cast<struct NodeBinaryBitwiseXor*>(node)->l_node; r = static_cast<struct NodeBinaryBitwiseXor*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryLogicalAnd:     l = static_cast<struct NodeBinaryLogicalAnd*>(node)->l_node; r = static_cast<struct NodeBinaryLogicalAnd*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryLogicalOr:      l = static_cast<struct NodeBinaryLogicalOr*>(node)->l_node; r = static_cast<struct NodeBinaryLogicalOr*>(node)->r_node; break;
                    case NodeTypes::NodeBinaryNullCoalesce:  l = static_cast<struct NodeBinaryNullCoalesce*>(node)->l_node; r = static_cast<struct NodeBinaryNullCoalesce*>(node)->r_node; break;
                    default: break;
                }
                semanticVisit(l);
                semanticVisit(r);
                break;
            }

            // ---- С‚РёРїС‹ ----
            case NodeTypes::NodeFunctionType: {
                auto n = static_cast<struct NodeFunctionType*>(node);
                for (auto g : n->generics) semanticVisit(g);
                for (auto p : n->params) semanticVisit(p);
                semanticVisit(n->return_type);
                break;
            }
            case NodeTypes::NodeTupleType:
                for (auto t : static_cast<struct NodeTupleType*>(node)->types) semanticVisit(t);
                break;
            case NodeTypes::NodeArrayType: {
                auto n = static_cast<struct NodeArrayType*>(node);
                semanticVisit(n->element_type);
                semanticVisit(n->count);
                // `Array[T, N]{вЂ¦}` вЂ” С‚РµР»Рѕ РєРѕРЅСЃС‚СЂСѓРєС‚РѕСЂР°: РІС‹СЂР°Р¶РµРЅРёСЏ СЌР»РµРјРµРЅС‚РѕРІ
                // РґРѕР»Р¶РЅС‹ РїСЂРѕР№С‚Рё СЃРµРјР°РЅС‚РёРєСѓ (РїРµСЂРµРјРµРЅРЅС‹Рµ РґРѕР»Р¶РЅС‹ Р±С‹С‚СЊ РѕР±СЉСЏРІР»РµРЅС‹).
                for (auto item : n->items) semanticVisit(item);
                break;
            }

            // ---- Р»РёСЃС‚СЊСЏ: РЅРµС‚ РґРµС‚РµР№ ----
            case NodeTypes::NodeNumber:
            case NodeTypes::NodeString:
            case NodeTypes::NodeBool:
            case NodeTypes::NodeNull:
            case NodeTypes::NodeVariable:
            case NodeTypes::NodeBreak:
            case NodeTypes::NodeContinue:
                break;

            default:
                break;
        }
    }

    void operator()() {
        for (auto node : ast) {
            semanticVisit(node);
        }

    }
};
