#include "NodeBase.cpp"

#pragma once

struct NodeExit final: Node {
    Token token;
    Node* expr;              // выражение кода возврата; nullptr => 0

    NodeExit(const Token& token, Node* expr) : Node(NodeTypes::NodeExit, ST), token(token), expr(expr) {}
};