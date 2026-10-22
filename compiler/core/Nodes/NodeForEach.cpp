#include "NodeBase.cpp"
#include "NodeVariableDeclaration.cpp"
#pragma once

struct NodeForEach final: Node {
    struct NodeVariableDeclaration* decl;   // декларация переменной-элемента (name/type)
    Node* source;                    // выражение-контейнер после 'of'
    Node* body;
    bool by_ref;                     // 'ref let x' -> живой алиас на элемент
    Token token;                     // токен 'for'
    Token of_token;                  // токен 'of'
    NodeForEach(struct NodeVariableDeclaration* decl, Node* source, Node* body, bool by_ref,
                Token token = Token(), Token of_token = Token()) :
        Node(NodeTypes::NodeForEach, ST),
        decl(decl),
        source(source),
        body(body),
        by_ref(by_ref),
        token(token),
        of_token(of_token) {}
};
