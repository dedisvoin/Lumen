#include "NodeBase.cpp"
#pragma once

struct NewModifier {
    String name;
    Node* type_arg; // nullptr, если модификатор без аргумента (флаг)
    NewModifier(String name, Node* type_arg = nullptr) : name(name), type_arg(type_arg) {}
};

struct NodeNew final: Node {
    std::vector<NewModifier> modifiers;
    Node* value; // nullptr, если значение не указано (new;)
    Token token;
    NodeNew(std::vector<NewModifier> modifiers, Node* value, const Token& token = Token()) :
        Node(NodeTypes::NodeNew, EX),
        modifiers(modifiers),
        value(value),
        token(token) {}
};
