#include "NodeBase.cpp"
#pragma once

struct NodeTupleType final: Node {
    std::vector<Node*> types;
    NodeTupleType(std::vector<Node*> types) :
        Node(NodeTypes::NodeTupleType, EX),
        types(types) {}
};
