#include "NodeBase.cpp"

#pragma once

// eval { ... } — блок вычисления на этапе анализа (только верхний уровень).
// body — NodeStatementsScope с телом блока. Размещение (top-level) проверяется
// парсером; исполнение/заморозка глобалов — драйвером анализа (M8).
struct NodeEvalBlock final: Node {
    Token eval_token;   // токен 'eval' (для диагностики о размещении)
    Token end_token;    // токен '}' (конец блока) — для фильтрации хинтов по диапазону
    Node* body;         // NodeStatementsScope

    NodeEvalBlock(const Token& eval_token, const Token& end_token, Node* body) :
        Node(NodeTypes::NodeEvalBlock, ST),
        eval_token(eval_token), end_token(end_token), body(body) {}
};
