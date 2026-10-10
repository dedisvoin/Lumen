#include "NodeBase.cpp"
#pragma once

struct NodeSizeOf final: Node {
    Node* expr;
    Token token;
    NodeSizeOf(Node* expr, Token token) :
        Node(NodeTypes::NodeSizeOf, EX),
        expr(expr),
        token(token) {}
};