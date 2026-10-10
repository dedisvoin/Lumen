#include "NodeBase.cpp"

#pragma once

struct NodeContinue final: Node {
    NodeContinue() : Node(NodeTypes::NodeContinue, ST) {}
};
