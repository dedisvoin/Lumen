#include "NodeBase.cpp"

#pragma once

struct NodeNull final: Node {
    NodeNull() : Node(NodeTypes::NodeNull, ST) {}
};
