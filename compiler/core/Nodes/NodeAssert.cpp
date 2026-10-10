#include "NodeBase.cpp"

#pragma once

struct NodeAssert final: Node {
    Token token;
    Node* check;
    Node* message; // nullptr, если только условие: `assert cond;`

    NodeAssert(const Token& token, Node* check, Node* message = nullptr)
        : Node(NodeTypes::NodeAssert, ST), token(token), check(check), message(message) {}
};