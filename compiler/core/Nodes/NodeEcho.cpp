#include "NodeBase.cpp"
#include "../../core/Tokens.cpp"
#pragma once

struct NodeEcho final: Node {
    Node* expr;
    Token token;
    Token end;
    NodeEcho(Node* expr, Token token = Token(), Token end = Token()) :
        Node(NodeTypes::NodeEcho, ST),
        expr(expr), token(token), end(end) {}
};
