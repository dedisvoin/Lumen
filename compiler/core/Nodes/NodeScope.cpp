#include "NodeBase.cpp"
#include "vector"

#pragma once


struct NodeScope final: Node {
    Node* node;
    NodeScope(Node* node) : Node(NodeTypes::NodeScope, EX), node(node) {}
};

struct NodeStatementsScope final: Node {
    std::vector<Node*> nodes;
    NodeStatementsScope(std::vector<Node*> nodes) : Node(NodeTypes::NodeStatementsScope, ST), nodes(nodes) {}
};
