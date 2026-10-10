#include "NodeBase.cpp"
#include "../../utils/Strings.cpp"

#pragma once

struct FuncArg {
    String name;
    std::vector<String> modifiers;
    Node* type;
    Node* default_value;

    FuncArg() : type(nullptr), default_value(nullptr) {}
    FuncArg(String name, std::vector<String> modifiers, Node* type, Node* default_value) :
        name(name), modifiers(modifiers), type(type), default_value(default_value) {}
};

struct GenericParam {
    String name;
    Token name_token;        // токен имени (для диагностик: каретка на 'T')
    std::vector<String> modifiers;
    Node* type;
    Node* default_value;

    GenericParam() : name_token(Token()), type(nullptr), default_value(nullptr) {}
    GenericParam(String name, Token name_token, std::vector<String> modifiers, Node* type, Node* default_value) :
        name(name), name_token(name_token), modifiers(modifiers), type(type), default_value(default_value) {}
};

struct NodeFunctionDeclaration final: Node {
    String name;
    Token name_token;          // токен имени
    Token gen_open_token;      // токен '[' генерик-параметров
    Token gen_close_token;     // токен ']' генерик-параметров
    Token args_open_token;     // токен '(' списка параметров
    Token args_close_token;    // токен ')' списка параметров
    std::vector<GenericParam> generics;
    std::vector<FuncArg> params;
    Node* return_type;
    Node* body;

    // return-тип объявлен как ссылка (`-> ref T`)
    bool returns_ref = false;

    // Декларация extern (без тела): реализация приходит из C++-файла (#cppinclude),
    // тело отсутствует, кодгенерит вызов, а определение — из подключённого .cpp.
    bool is_extern = false;

    // Документирующий комментарий (`/* ... */` или стек `//`) непосредственно над объявлением
    String doc_comment;

    NodeFunctionDeclaration(String name, std::vector<GenericParam> generics, std::vector<FuncArg> params, Node* return_type, Node* body,
                            Token name_token = Token(), Token gen_open_token = Token(), Token gen_close_token = Token(),
                            Token args_open_token = Token(), Token args_close_token = Token()) :
        Node(NodeTypes::NodeFunctionDeclaration, ST), name(name), name_token(name_token),
        gen_open_token(gen_open_token), gen_close_token(gen_close_token),
        args_open_token(args_open_token), args_close_token(args_close_token),
        generics(generics), params(params), return_type(return_type), body(body) {}
};
