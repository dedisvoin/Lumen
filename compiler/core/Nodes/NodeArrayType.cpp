#include "NodeBase.cpp"
#pragma once

struct NodeArrayType final: Node {
    Node* element_type;
    Node* count; // может быть nullptr (без фиксированного размера)
    Token count_token; // токен выражения размера (для диагностики)
    // Конструктор `Array[T, N]{…}`: элементы, переданные в '{}'. has_body
    // отличает `Array[i64]` (тип-аннотация) от `Array[i64]{}` (конструктор с
    // пустым телом) — без него пустой список неотличим от отсутствия тела.
    std::vector<Node*> items;
    bool has_body = false;
    Token open_token, close_token;
    NodeArrayType(Node* element_type, Node* count, Token count_token = Token()) :
        Node(NodeTypes::NodeArrayType, EX),
        element_type(element_type),
        count(count),
        count_token(count_token) {}
};