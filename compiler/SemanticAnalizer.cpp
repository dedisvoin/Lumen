#include "ASTGenerator.cpp"

#include "core/Tokens.cpp"
#include "core/ErrorBase.cpp"

#include <vector>
#include <map>

struct ASTAnalizer {
    AbstractTreeT ast;
    ErrorContainer& error_container;
    std::vector<NodeTypes> ctx;   // стек контекста: в какие declaration мы вложены

    ASTAnalizer(AbstractTreeT ast, ErrorContainer& error_container) : ast(ast), error_container(error_container) {}

    // находимся ли сейчас внутри тела struct (для visibility-модификаторов)
    bool insideStruct() const {
        for (auto t : ctx)
            if (t == NodeTypes::NodeStructDeclaration) return true;
        return false;
    }
    // находимся ли сейчас внутри тела namespace (private/public допустимы и там)
    bool insideNamespace() const {
        for (auto t : ctx)
            if (t == NodeTypes::NodeNamespace) return true;
        return false;
    }
    // находимся ли сейчас внутри тела функции (static-локальные let)
    bool insideFunction() const {
        for (auto t : ctx)
            if (t == NodeTypes::NodeFunctionDeclaration) return true;
        return false;
    }

    // допустимые цели модификатора (поверхностная проверка)
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

    bool containsExit(Node* node, bool in_loop = false, bool in_func = false) {
        if (!node) return false;
        if (node->type == NodeTypes::NodeBreak)
            return !in_loop && !in_func;
        if (node->type == NodeTypes::NodeReturn)
            return !in_func;
        if (node->type == NodeTypes::NodeExit)
            return true; // exit — безусловное завершение всей программы
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
                return false; // не заходим в выражения деклараций
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

    // условие цикла статически истинно (while (true) / while (1) / while (1.0))
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

    // условие статически ложно (if(false)/while(false)/while(0)/while(0.0))
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

    // тело цикла/ветки пустое (блок {} без операторов)
    bool isEmptyBody(Node* body) {
        if (!body) return false;
        if (body->type == NodeTypes::NodeStatementsScope)
            return static_cast<struct NodeStatementsScope*>(body)->nodes.empty();
        if (body->type == NodeTypes::NodeScope)
            return isEmptyBody(static_cast<struct NodeScope*>(body)->node);
        return false;
    }

    // структурное равенство двух выражений (поверхностно, без типов/символов):
    // используется для обнаружения самоприсваивания a = a, std::p = std::p, a.b = a.b, arr[0] = arr[0]
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

    // токен первой «реальной» декларации в корне (для позиции ошибки отсутствия main)
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
        semanticVisit — точка входа в поверхностный (shallow) семантический анализ, Pass 1.

        Рекурсивно обходит всё дерево разбора (AST), начиная с корня, и для каждого
        узла выполняет набор дешёвых статических проверок, не требующих построения
        таблицы символов, разрешения типов или анализа области видимости. Все найденные
        проблемы добавляются в error_container через фабрики из namespace SEMANTIC_ERRORS.
        Обход продолжается до конца дерева (анализатор не останавливается на первой
        ошибке), поэтому за один проход можно собрать сразу несколько сообщений.

        ЧТО ИМЕННО ПРОВЕРЯЕТСЯ (по типам узлов)
        ===========================================================================================
        NodeModifier (реализовано в modifiersCheck, вызывается из этого case):
            - дублирующийся модификатор в одной цепочке            -> DUPLICATE_MODIFIER (51)
            - модификатор, применённый не к тому виду узла         -> MODIFIER_MISPLACED (50)
            - на NodeVariableDeclaration:
                * strict требует явного типа                        -> STRICT_REQUIRES_TYPE (53)
                * const требует инициализатора (значения)           -> CONST_REQUIRES_VALUE (54)
            - семантика модификатора относительно цели/контекста   -> MODIFIER_SEMANTICS (62, Warn):
                * const/strict — только для переменных и namespace
                * final        — допустим в любом контексте (без ограничений)
                * static       — только для членов struct (fn/let), внутри struct (C++-style)
                 * private/public— внутри struct ИЛИ namespace (см. ctx-стек)
          Затем рекурсивно обходятся дочерние узлы модификатора.

        NodeStatementsScope / тела функций (NodeFunctionDeclaration), структур
          (NodeStructDeclaration), пространств имён (NodeNamespace), лямбд (NodeLambda):
            просто рекурсивно посещаются все вложенные узлы.

        NodeVariableDeclaration:
            рекурсивно обходятся value_node (если есть — let x; без значения допустимо)
            и type_node (если указан).

        NodeWhile:
            - если условие цикла статически истинно (while(true)/while(1)/while(1.0),
              см. isAlwaysTrue) И в теле нет ни break, ни ret, выходящих именно из этого
              цикла, выдаётся предупреждение                     -> INFINITE_LOOP (52, Warning)
            - рекурсивно обходятся условие и тело.

        NodeBinaryAssignment:
            - если левая и правая части присваивания структурно идентичны (см. sameExpr:
              x = x, a.b = a.b, arr[0] = arr[0], std::p = std::p и т.п.), выдаётся
              предупреждение                                    -> SELF_ASSIGNMENT (55, Warning)

        NodeOut / NodeOutln:
            - вызов без аргументов (out; / out()) или со скобками, но пустым списком,
              выдаёт предупреждение                             -> EMPTY_OUTPUT (56, Warning)

        Остальные выражения (NodeIf, NodeFor, NodeReturn, NodeBreak, NodeContinue,
          NodeNew, NodeArray, унарные и бинарные операции, NodeBinaryScopeResolution,
          NodeMemberAccess, NodeCast, NodeFunctionCall, NodeArrayAccess, типы данных,
          NodeDelete, NodeScope и пр.):
            не содержат собственных проверок на этом этапе — производится только
            рекурсивный спуск в дочерние узлы, чтобы дойти до вышеперечисленных случаев.

        ЧЕГО ЭТА ФАЗА НЕ ДЕЛАЕТ (остаётся на глубокую фазу, Pass 2 / M3–M6):
          - проверку совместимости типов и корректности операций по типам;
          - использование неинициализированных / несуществующих имён;
          - переопределение/затенение имён, области видимости;
          - семантические алиасы (let y = &x; x = *y;) и константный фолд-анализ;
          - проверку const-мутации по реальным символам.
        */
    // проверки, связанные с модификаторами (поверхностная семантика Pass 1):
    // дубли в цепочке, неверное применение к цели, требования strict/const на переменной.
    // Сюда же позже добавляются семантические правила (const/strict/final на нужной цели,
    // static/public/private в нужном контексте).
    void modifiersCheck(Node* node) {
        if (!node || node->type != NodeTypes::NodeModifier) return;
        auto m = static_cast<struct NodeModifier*>(node);

        // собрать всю цепочку вложенных модификаторов и найти цель
        std::vector<String> mods;
        std::vector<Token> mod_tokens;
        Node* target = node;
        while (target && target->type == NodeTypes::NodeModifier) {
            auto mm = static_cast<struct NodeModifier*>(target);
            mods.push_back(mm->name);
            mod_tokens.push_back(mm->token);
            target = mm->node;
        }

        // дублирование модификаторов (напр. const const let x)
        std::map<String, bool> seen;
        for (size_t i = 0; i < mods.size(); ++i) {
            if (seen.count(mods[i])) {
                error_container.addError(SEMANTIC_ERRORS::DUPLICATE_MODIFIER(mod_tokens[i], mods[i]));
                break;   // одна ошибка на цепочку
            }
            seen[mods[i]] = true;
        }

        // неверное применение: цель не декларация/блок
        if (target && !isModifierTarget(target)) {
            error_container.addError(SEMANTIC_ERRORS::MODIFIER_MISPLACED(m->token, m->name));
        }

        // модификаторы strict/const на цели-декларации ИЛИ блоке (strict { ... } / const { ... }):
        // требование (явный тип / значение) переносится на все вложенные let-объявления
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
                // блок: спускаемся во вложенные let/блоки/функции/структуры/пространства имён
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

        // семантика модификаторов: к чему применим и в каком контексте
        if (target) {
            for (size_t i = 0; i < mods.size(); ++i) {
                const String& mod = mods[i];
                Token mtok = mod_tokens[i];
                if (mod == "const") {
                    // `const fn` — read-only метод структуры (не может менять self)
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
                    // ref-декларация обязана быть `ref let name = <lvalue>;`
                    if (target->type != NodeTypes::NodeVariableDeclaration)
                        error_container.addError(SEMANTIC_ERRORS::MODIFIER_SEMANTICS(
                            mtok, mod, "ref is allowed only on let-declarations ('ref let name = <lvalue>;')"));
                    // ref нельзя комбинировать со static/private/final (план §8)
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

        // рекурсия сразу в цель, без повторного обхода модификаторов
        // (иначе цепочка проверялась бы несколько раз)
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

            // ---- управление потоком ----
            case NodeTypes::NodeIf: {
                auto n = static_cast<struct NodeIf*>(node);
                // константное условие if(true)/if(false) -> одна ветка мертва
                if (isAlwaysTrue(n->condition) || isAlwaysFalse(n->condition)) {
                    error_container.addError(SEMANTIC_ERRORS::CONSTANT_CONDITION(n->token));
                }
                // пустое тело then/else
                if (isEmptyBody(n->then_body))
                    error_container.addError(SEMANTIC_ERRORS::EMPTY_BODY(n->token));
                // одинаковые ветки then/else
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

            // ---- выражения ----
            case NodeTypes::NodeBinaryAssignment: {
                auto n = static_cast<struct NodeBinaryAssignment*>(node);
                // D1: самоприсваивание (структурно равные левая и правая части)
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
                // D2: out() без аргументов
                if (n->exprs.empty())
                    error_container.addError(SEMANTIC_ERRORS::EMPTY_OUTPUT(n->token));
                for (auto e : n->exprs) semanticVisit(e);
                break;
            }
            case NodeTypes::NodeOutln: {
                auto n = static_cast<struct NodeOutln*>(node);
                // D2: outln() без аргументов
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

            // ---- унарные операции ----
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

            // ---- бинарные операции ----
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

            // ---- типы ----
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
                // `Array[T, N]{…}` — тело конструктора: выражения элементов
                // должны пройти семантику (переменные должны быть объявлены).
                for (auto item : n->items) semanticVisit(item);
                break;
            }

            // ---- листья: нет детей ----
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
