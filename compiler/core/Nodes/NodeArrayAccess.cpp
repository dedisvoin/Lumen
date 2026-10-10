#include "NodeBase.cpp"

#pragma once

struct NodeArrayAccess final: Node {
    Node* base;
    std::vector<Node*> indices;
    Token token;

    NodeArrayAccess(Node* base, std::vector<Node*> indices, Token token = Token()) :
        Node(NodeTypes::NodeArrayAccess, EX), base(base), indices(indices), token(token) {}
};
