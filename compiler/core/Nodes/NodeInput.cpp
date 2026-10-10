#include "NodeBase.cpp"

#pragma once

struct NodeInput final: Node {
    Node* prompt;              // nullptr если input() — без сообщения; иначе NodeString
    Token token_start;
    Token token_end;
    NodeInput(Node* prompt, Token token_start, Token token_end) :
        Node(NodeTypes::NodeInput, EX),
        prompt(prompt), token_start(token_start), token_end(token_end) {}
};
