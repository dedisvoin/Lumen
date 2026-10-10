#include "NodeBase.cpp"
#include "NodeStructDeclaration.cpp"
#pragma once

struct NodeEnumDeclaration final: Node {
    String name;
    Token name_token;   // токен имени (для семантических диагностик)
    std::vector<Token> variants;  // имена вариантов в порядке объявления
    String doc_comment;
    NodeEnumDeclaration(String name, std::vector<Token> variants,
                        Token name_token = Token()) :
        Node(NodeTypes::NodeEnumDeclaration, ST),
        name(name),
        name_token(name_token),
        variants(std::move(variants)) {}
};