#include "NodeBase.cpp"
#include "../../utils/Strings.cpp"
#include "../../core/Tokens.cpp"

#pragma once

struct NodeNamespace final: Node {
    String name;
    Token name_token;   // токен имени (для семантических диагностик)
    Node* body;

    NodeNamespace(String name, Node* body, Token name_token = Token()) :
        Node(NodeTypes::NodeNamespace, ST), name(name), name_token(name_token), body(body) {}
};
