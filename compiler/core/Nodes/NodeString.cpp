#include "NodeBase.cpp"

#pragma once

struct NodeString final: Node {
    String value;
    bool   is_char;   // true, если литерал набран в одинарных кавычках ('a')

    NodeString(String value, bool is_char = false) : Node(NodeTypes::NodeString, ST), value(value), is_char(is_char) {}
};
