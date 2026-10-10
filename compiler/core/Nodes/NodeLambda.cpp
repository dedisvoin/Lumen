#include "NodeBase.cpp"
#include "NodeFunctionDeclaration.cpp"
#pragma once

struct NodeLambda final: Node {
    std::vector<FuncArg> params;
    Node* return_type;
    Node* body;
    NodeLambda(std::vector<FuncArg> params, Node* return_type, Node* body) :
        Node(NodeTypes::NodeLambda, EX),
        params(params),
        return_type(return_type),
        body(body) {}
};
