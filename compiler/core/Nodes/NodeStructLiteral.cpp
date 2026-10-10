#include "NodeBase.cpp"
#pragma once

struct StructFieldInit {
    String name;
    Token token;
    Node* value;
};

// Литерал структуры: `Point { x: 1, y: 2 }` / `Vec[i64, 8] { … }`.
// type_expr — выражение-«тип» (имя или ::-цепочка), generics — аргументы.
// struct_name — имя структуры, вычисленное анализатором: у литерала тип может
// задаваться не именем (`local_no_generic(){…}`, `get_struct(i64){…}`), и
// транспилятору нужно настоящее имя для object(...) и полей по умолчанию.
struct NodeStructLiteral final: Node {
    Node* type_expr;
    std::vector<Node*> generics;
    std::vector<StructFieldInit> inits;
    Token open_token;
    Token close_token;
    String struct_name;
    NodeStructLiteral(Node* type_expr,
                      std::vector<Node*> generics,
                      std::vector<StructFieldInit> inits,
                      Token open_token = Token(), Token close_token = Token()) :
        Node(NodeTypes::NodeStructLiteral, EX),
        type_expr(type_expr),
        generics(std::move(generics)),
        inits(std::move(inits)),
        open_token(open_token),
        close_token(close_token) {}
};