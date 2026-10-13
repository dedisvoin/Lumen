#include "NodeBase.cpp"
#pragma once

// mod(expr, modifier) — true, если у объявления, на которое указывает `expr`,
// установлен данный модификатор (static/final/const/strict/private/ref/public).
// Свойство компиляционного времени: результат вычисляется анализатором и
// сохраняется в узле, кодогенератор выдаёт константу.
struct NodeMod final: Node {
    Node* expr;
    String modifier;
    Token token;
    bool resolved = false;
    bool value = false;
    NodeMod(Node* expr, String modifier, Token token) :
        Node(NodeTypes::NodeMod, EX),
        expr(expr), modifier(modifier), token(token) {}
};
