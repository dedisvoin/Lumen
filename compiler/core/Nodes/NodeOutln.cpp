#include "NodeBase.cpp"
#include "vector"
#include "../../core/Tokens.cpp"
#pragma once

struct NodeOutln final: Node {
    std::vector<Node*> exprs;
    Token token;
    NodeOutln(std::vector<Node*> exprs, Token token = Token()) :
        Node(NodeTypes::NodeOutln, ST),
        exprs(exprs), token(token) {}
};
