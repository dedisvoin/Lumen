#include "NodeBase.cpp"

#pragma once

struct NodeReturn final: Node {
    Token token;
    Node* expr;

    NodeReturn(const Token& token, Node* expr) : Node(NodeTypes::NodeReturn, ST), token(token), expr(expr) {}
};
