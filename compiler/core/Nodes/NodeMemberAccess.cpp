#include "NodeBase.cpp"
#pragma once

struct NodeMemberAccess final: Node {
    Node* l_node;
    Node* r_node;
    Token member_token;   // токен имени члена (для семантических диагностик)
    NodeMemberAccess(Node* l, Node* r, Token member_token = Token()) :
        Node(NodeTypes::NodeMemberAccess, EX),
        l_node(l),
        r_node(r),
        member_token(member_token) {}
};
