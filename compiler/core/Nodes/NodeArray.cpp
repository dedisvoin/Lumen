#include "NodeBase.cpp"
#pragma once

struct NodeArray final: Node {
    std::vector<Node*> elements;
    NodeArray(std::vector<Node*> elements) :
        Node(NodeTypes::NodeArray, EX),
        elements(elements) {}
};
