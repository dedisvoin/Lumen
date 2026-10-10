#include "NodeBase.cpp"

#pragma once

struct NodeString final: Node {
    String value;

    NodeString(String value) : Node(NodeTypes::NodeString, ST), value(value) {}
};
