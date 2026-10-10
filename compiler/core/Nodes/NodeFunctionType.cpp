#include "NodeBase.cpp"

#pragma once

struct NodeFunctionType final: Node {
    std::vector<Node*> generics;   // генерик-аргументы типа: Func[G, ...]( ... ) -> R
    std::vector<Node*> params;
    Node* return_type;

    NodeFunctionType(std::vector<Node*> generics, std::vector<Node*> params, Node* return_type) :
        Node(NodeTypes::NodeFunctionType, EX), generics(generics), params(params), return_type(return_type) {}
};
