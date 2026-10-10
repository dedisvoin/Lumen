#include "NodeBase.cpp"

#pragma once

struct NodeFunctionCall final: Node {
    Node* callee;
    std::vector<Node*> generics;
    std::vector<Node*> args;
    Token name_token;      // токен имени функции (начало спана вызова для диагностик)
    Token open_token;      // токен '(' вызова
    Token close_token;     // токен ')' вызова
    Token gen_open_token;  // токен '[' генерик-аргументов
    Token gen_close_token; // токен ']' генерик-аргументов

    NodeFunctionCall(Node* callee, std::vector<Node*> args, std::vector<Node*> generics = {},
                     Token open_token = Token(), Token close_token = Token(),
                     Token gen_open_token = Token(), Token gen_close_token = Token(),
                     Token name_token = Token()) :
        Node(NodeTypes::NodeFunctionCall, EX), callee(callee), generics(generics), args(args),
        name_token(name_token), open_token(open_token), close_token(close_token),
        gen_open_token(gen_open_token), gen_close_token(gen_close_token) {}
};
