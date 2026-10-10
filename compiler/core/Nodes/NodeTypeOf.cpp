#include "NodeBase.cpp"
#pragma once

struct NodeTypeOf final: Node {
    Node* expr;
    Token token;
    String static_type;
    bool has_static_type = false;
    bool static_type_ambiguous = false;
    NodeTypeOf(Node* expr, Token token) :
        Node(NodeTypes::NodeTypeOf, EX),
        expr(expr), token(token) {}
};
