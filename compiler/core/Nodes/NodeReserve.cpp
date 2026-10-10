#include "NodeBase.cpp"

#pragma once

// reserve <array expr>, <count expr>; — предвыделение ёмкости массива.
// array_expr — lvalue (переменная / поле / элемент по индексу); count_expr —
// количество элементов, место под которое гарантируется (НЕ «ещё count»).
// Длина (len) не меняется, усадки нет: count меньше текущей длины — no-op.
struct NodeReserve final: Node {
    Token token;
    Node* array_expr;         // lvalue массива
    Node* count_expr;         // желаемая ёмкость (целое)

    NodeReserve(const Token& token, Node* array_expr, Node* count_expr) :
        Node(NodeTypes::NodeReserve, ST), token(token),
        array_expr(array_expr), count_expr(count_expr) {}
};
