#include "NodeBase.cpp"

#pragma once

struct NodeIf final: Node {
Node* condition;
Node* then_body;
Node* else_body;
Token token;               // токен 'if'
NodeIf(Node* condition, Node* then_body, Node* else_body, Token token = Token()) :
        Node(NodeTypes::NodeIf, ST), condition(condition),
        then_body(then_body), else_body(else_body), token(token) {}
};
