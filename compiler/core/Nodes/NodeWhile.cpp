#include "NodeBase.cpp"

#pragma once

struct NodeWhile final: Node {
    Node* condition;
    Node* body;
    Token token;

    NodeWhile(Node* condition, Node* body, Token token = Token()) :
        Node(NodeTypes::NodeWhile, ST), condition(condition), body(body), token(token) {}
};
