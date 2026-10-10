#pragma once

enum NodeTypes {
    NodeNumber,
    NodeString,
    NodeBool,
    NodeScope,
    NodeStatementsScope,
    NodeModifier,
    NodeNamespace,
    NodeIf,
    NodeWhile,
    NodeFor,
    NodeBreak,
    NodeContinue,
    NodeReturn,
    NodeOut,
    NodeOutln,
    NodeEcho,
    NodeInput,
    NodeDelete,
    NodeNull,
    NodeVariable,

    NodeUnaryMinus,
    NodeUnaryPlus,
    NodeUnaryNegation,
    NodeUnaryDereference,
    NodeUnaryBitwiseNot,
    NodeUnaryAddressOf,

    NodeBinaryMultiplicative,
    NodeBinaryDivision,
    NodeBinaryRemainder,
    NodeBinaryAdditive,
    NodeBinarySubtraction,
    NodeBinaryShiftLeft,
    NodeBinaryShiftRight,
    NodeBinaryEqual,
    NodeBinaryNotEqual,
    NodeBinaryLess,
    NodeBinaryGreater,
    NodeBinaryLessEqual,
    NodeBinaryGreaterEqual,
    NodeBinaryBitwiseAnd,
    NodeBinaryBitwiseXor,
    NodeBinaryBitwiseOr,
    NodeBinaryLogicalAnd,
    NodeBinaryLogicalOr,
    NodeBinaryNullCoalesce,
    NodeBinaryScopeResolution,
    NodeBinaryAssignment,
    NodeCast,
    NodeMemberAccess,
    NodeFunctionCall,
    NodeNamedArgument,
    NodeFunctionType,
    NodeTupleType,
    NodeArrayType,
    NodeArray,
    NodeArrayAccess,
    NodeLambda,
    NodeNew,
    NodeTypeOf,
    NodeSizeOf,
    NodePtr,

    NodeVariableDeclaration,
    NodeFunctionDeclaration,
    NodeStructDeclaration,
    NodeEnumDeclaration,
    NodeStructLiteral,
    NodeUnaryPostfixIncrement,
    NodeUnaryPostfixDecrement,
    NodePostfixUnwrap,
    NodeUnaryPrefixIncrement,
    NodeUnaryPrefixDecrement,
    NodeAssert,
    NodeExit,
    NodeAppend,
    NodeReserve
};

enum NodeOperation {
    EXPR,
    STMT
};

constexpr NodeOperation EX = NodeOperation::EXPR;
constexpr NodeOperation ST = NodeOperation::STMT;

struct Node {
    NodeTypes type;
    NodeOperation op;
    Node(NodeTypes type, NodeOperation op): type(type), op(op) {};
    virtual ~Node() = default;
};
