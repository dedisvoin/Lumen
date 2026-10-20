#include "NodeBase.cpp"
#pragma once

struct NodeTypeOf final: Node {
    Node* expr;
    Token token;
    String static_type;
    bool has_static_type = false;
    bool static_type_ambiguous = false;
    String static_type_elem;   // элемент массива (для typeof(arr)::type)
    bool has_static_elem = false;
    bool is_static = false;   // typeis(...) — статический тип (Type), иначе typeof(...) — рантайм
    NodeTypeOf(Node* expr, Token token) :
        Node(NodeTypes::NodeTypeOf, EX),
        expr(expr), token(token) {}
};
