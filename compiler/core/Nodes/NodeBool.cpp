#include "NodeBase.cpp"

#pragma once

struct NodeBool final: Node {
    bool value;

    NodeBool(bool value) : Node(NodeTypes::NodeBool, ST), value(value) {}
};
