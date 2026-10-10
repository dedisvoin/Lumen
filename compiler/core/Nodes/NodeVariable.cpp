#include "NodeBase.cpp"
#include "../../utils/Strings.cpp"

#pragma once


struct NodeVariable final: Node { 
    String name;
    Token token;   // токен имени (для семантических диагностик)

    NodeVariable(String name, Token token = Token()): 
        Node(NodeTypes::NodeVariable, EX), name(name), token(token) {}
};