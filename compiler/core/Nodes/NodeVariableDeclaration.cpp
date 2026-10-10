#include "NodeBase.cpp"
#include "../../utils/Strings.cpp"

#pragma once


struct NodeVariableDeclaration final: Node {
    Node* value_node;
    Node* type_node;
    
    String name;
    Token name_token;   // токен имени (для семантических диагностик)
    
    NodeVariableDeclaration(String name, Node* value_node, Node* type_node, Token name_token = Token()) : 
        Node(NodeTypes::NodeVariableDeclaration, ST), name(name),
        value_node(value_node), type_node(type_node), name_token(name_token) {}
};