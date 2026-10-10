#include "NodeBase.cpp"
#include "../../core/Tokens.cpp"
#pragma once


struct NodeUnaryMinus final: Node {
    Node* node;
    Token token;
    NodeUnaryMinus(Node* node, Token token) : Node(NodeTypes::NodeUnaryMinus, EX), node(node), token(token) {}
};

struct NodeUnaryPlus final: Node {
    Node* node;
    Token token;
    NodeUnaryPlus(Node* node, Token token) : Node(NodeTypes::NodeUnaryPlus, EX), node(node), token(token) {}
};

struct NodeUnaryNegation final: Node {
    Node* node;
    Token token;
    NodeUnaryNegation(Node* node, Token token) : Node(NodeTypes::NodeUnaryNegation, EX), node(node), token(token) {}
};

struct NodeUnaryDereference final: Node {
    Node* node;
    Token token;
    NodeUnaryDereference(Node* node, Token token) : Node(NodeTypes::NodeUnaryDereference, EX), node(node), token(token) {}
};

struct NodeUnaryBitwiseNot final: Node {
    Node* node;
    Token token;
    NodeUnaryBitwiseNot(Node* node, Token token) : Node(NodeTypes::NodeUnaryBitwiseNot, EX), node(node), token(token) {}
};

struct NodeUnaryAddressOf final: Node {
    Node* node;
    Token token;
    NodeUnaryAddressOf(Node* node, Token token) : Node(NodeTypes::NodeUnaryAddressOf, EX), node(node), token(token) {}
};

// постфиксные ++ / -- : выражение возвращает старое значение операнда,
// а побочным действием меняет его (i = i + 1 / i = i - 1).
struct NodeUnaryPostfixIncrement final: Node {
    Node* node;
    Token token;
    NodeUnaryPostfixIncrement(Node* node, Token token) : Node(NodeTypes::NodeUnaryPostfixIncrement, EX), node(node), token(token) {}
};

struct NodeUnaryPostfixDecrement final: Node {
    Node* node;
    Token token;
    NodeUnaryPostfixDecrement(Node* node, Token token) : Node(NodeTypes::NodeUnaryPostfixDecrement, EX), node(node), token(token) {}
};

// постфиксный ? : развернуть значение через __get__
//   - если у объекта есть метод __get__ — вызвать его и вернуть результат;
//   - если метода нет — вернуть сам объект без изменений (безопасная форма).
// Позволяет развернуть вложенные обёртки и значения, где автораспаковка
// подавлена (например, поле структуры: `r.value?`).
struct NodePostfixUnwrap final: Node {
    Node* node;
    Token token;
    NodePostfixUnwrap(Node* node, Token token) : Node(NodeTypes::NodePostfixUnwrap, EX), node(node), token(token) {}
};

// префиксные ++i / --i : выражение возвращает НОВОЕ значение операнда,
// а побочным действием меняет его (i = i + 1 / i = i - 1).
struct NodeUnaryPrefixIncrement final: Node {
    Node* node;
    Token token;
    NodeUnaryPrefixIncrement(Node* node, Token token) : Node(NodeTypes::NodeUnaryPrefixIncrement, EX), node(node), token(token) {}
};

struct NodeUnaryPrefixDecrement final: Node {
    Node* node;
    Token token;
    NodeUnaryPrefixDecrement(Node* node, Token token) : Node(NodeTypes::NodeUnaryPrefixDecrement, EX), node(node), token(token) {}
};
