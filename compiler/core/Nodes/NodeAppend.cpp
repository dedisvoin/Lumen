#include "NodeBase.cpp"

#pragma once

// append <array expr>, <value expr>; — in-place добавление элемента в массив.
// array_expr — lvalue (переменная / поле / элемент по индексу); value_expr —
// значение, которое добавляется в конец контейнера.
struct NodeAppend final: Node {
    Token token;
    Node* array_expr;         // lvalue массива/кортежа
    Node* value_expr;         // добавляемый элемент

    NodeAppend(const Token& token, Node* array_expr, Node* value_expr) :
        Node(NodeTypes::NodeAppend, ST), token(token),
        array_expr(array_expr), value_expr(value_expr) {}
};