#include "NodeBase.cpp"
#pragma once

struct NodePtr final: Node {
    Node* addr;      // выражение-адрес (целое число)
    Node* type;      // выражение-тип (i64, str, ...)
    Token token;     // токен 'ptr'
    NodePtr(Node* addr, Node* type, Token token) :
        Node(NodeTypes::NodePtr, EX),
        addr(addr),
        type(type),
        token(token) {}
};