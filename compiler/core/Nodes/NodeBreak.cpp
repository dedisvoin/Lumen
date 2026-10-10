#include "NodeBase.cpp"

#pragma once

struct NodeBreak final: Node {
    NodeBreak() : Node(NodeTypes::NodeBreak, ST) {}
};
