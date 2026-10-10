#include "NodeBase.cpp"
#include "../../utils/Strings.cpp"
#include "../../core/Tokens.cpp"


#pragma once

struct NodeModifier final: Node {
    String name;
    Node* node;
    Token token;

    NodeModifier(String name, Node* node, Token token) : Node(NodeTypes::NodeModifier, ST),
        name(name), node(node), token(token) {}
};
