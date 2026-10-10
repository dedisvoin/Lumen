#include "NodeBase.cpp"
#include "../../core/Tokens.cpp"

#pragma once

struct NodeBinaryAssignment final: Node {
    Node* l_node;
    Node* r_node;

    Token start_token;
    Token end_token;

    NodeBinaryAssignment(Node* l, Node* r, Token start, Token end) :
        Node(NodeTypes::NodeBinaryAssignment, ST), l_node(l), r_node(r), start_token(start), end_token(end) {}
};
