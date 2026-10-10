#include "NodeBase.cpp"
#pragma once

struct NodeDelete final: Node {
    Node* expr;
    Node* index;
    Token token;
    NodeDelete(Node* expr, Node* index, Token token) :
        Node(NodeTypes::NodeDelete, ST),
        expr(expr), index(index), token(token) {}
};