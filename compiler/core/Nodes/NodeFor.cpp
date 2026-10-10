#include "NodeBase.cpp"
#pragma once

struct NodeFor final: Node {
    Node* init;
    Node* check;
    Node* update;
    Node* body;
    Token token;               // токен 'for'
    NodeFor(Node* init, Node* check, Node* update, Node* body, Token token = Token()) :
        Node(NodeTypes::NodeFor, ST),
        init(init),
        check(check),
        update(update),
        body(body),
        token(token) {}
};
