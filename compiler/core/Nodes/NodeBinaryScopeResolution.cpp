#include "NodeBase.cpp"

#pragma once

struct NodeBinaryScopeResolution final: Node {
    Node* l_node;
    Node* r_node;
    Token member_token;   // токен правого имени (для семантических диагностик)

    NodeBinaryScopeResolution(Node* l, Node* r, Token member_token = Token()) :
        Node(NodeTypes::NodeBinaryScopeResolution, EX), l_node(l), r_node(r), member_token(member_token) {}
};
