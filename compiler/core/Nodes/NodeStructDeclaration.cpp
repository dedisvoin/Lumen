#include "NodeBase.cpp"
#pragma once

struct NodeStructDeclaration final: Node {
    String name;
    Token name_token;   // токен имени (для семантических диагностик)
    Node* body;
    std::vector<GenericParam> generics; // дженерик-параметры структуры
    // Документирующий комментарий (`/* ... */` или стек `//`) непосредственно над объявлением
    String doc_comment;
    NodeStructDeclaration(String name, Node* body, Token name_token = Token(),
                          std::vector<GenericParam> generics = {}) :
        Node(NodeTypes::NodeStructDeclaration, ST),
        name(name),
        name_token(name_token),
        body(body),
        generics(std::move(generics)) {}
};
