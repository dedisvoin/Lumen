#include "NodeBase.cpp"
#pragma once

struct NodeCast final: Node {
    Node* l_node;
    Node* r_node;
    Token token;       // токен оператора 'as'
    NodeCast(Node* l, Node* r, Token token = Token()) :
        Node(NodeTypes::NodeCast, EX),
        l_node(l),
        r_node(r),
        token(token) {}
};
