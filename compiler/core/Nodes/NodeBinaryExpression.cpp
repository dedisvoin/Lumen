#include "NodeBase.cpp"
#include "type_traits"
#include "cstdlib"
#include "../../utils/Strings.cpp"

#include "../../core/Tokens.cpp"

#pragma once


struct NodeBinaryMultiplicative final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryMultiplicative(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryMultiplicative, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryDivision final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryDivision(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryDivision, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryAdditive final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryAdditive(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryAdditive, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinarySubtraction final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinarySubtraction(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinarySubtraction, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryRemainder final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryRemainder(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryRemainder, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryShiftLeft final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryShiftLeft(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryShiftLeft, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryShiftRight final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryShiftRight(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryShiftRight, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryEqual final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryEqual(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryEqual, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryNotEqual final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryNotEqual(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryNotEqual, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryLess final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryLess(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryLess, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryGreater final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryGreater(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryGreater, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryLessEqual final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryLessEqual(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryLessEqual, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryGreaterEqual final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryGreaterEqual(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryGreaterEqual, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryBitwiseAnd final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryBitwiseAnd(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryBitwiseAnd, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryBitwiseXor final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryBitwiseXor(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryBitwiseXor, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryBitwiseOr final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryBitwiseOr(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryBitwiseOr, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryLogicalAnd final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryLogicalAnd(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryLogicalAnd, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

struct NodeBinaryLogicalOr final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryLogicalOr(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryLogicalOr, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};

// null-coalescing: l ?? r  ->  r, если l == null, иначе l (r вычисляется лениво на уровне rt::coalesce)
struct NodeBinaryNullCoalesce final: Node {
    Node* l_node;
    Node* r_node;

    Token start;
    Token end;
    NodeBinaryNullCoalesce(Node* l_node, Node* r_node, const Token& start, const Token& end) :
        Node(NodeTypes::NodeBinaryNullCoalesce, EX), l_node(l_node), r_node(r_node), start(start), end(end) {}
};
