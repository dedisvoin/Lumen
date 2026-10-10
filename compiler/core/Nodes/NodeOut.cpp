#include "NodeBase.cpp"
#include "vector"
#include "../../core/Tokens.cpp"
#pragma once

struct NodeOut final: Node {
    std::vector<Node*> exprs;
    Token token;
    NodeOut(std::vector<Node*> exprs, Token token = Token()) :
        Node(NodeTypes::NodeOut, ST),
        exprs(exprs), token(token) {}
};
