#include "NodeBase.cpp"
#include "../../utils/Strings.cpp"

#include <map>
#include <vector>

#pragma once

// Именованный аргумент вызова: `fn_name(:param = expr)`.
// Узел существует только внутри списка аргументов NodeFunctionCall и вместе
// с позиционными аргументами обеспечивает раскладку по параметрам кандидата.
struct NodeNamedArgument final: Node {
    String name;
    Token name_token;      // токен имени (каретка для диагностик)
    Node* value;           // выражение значения

    NodeNamedArgument(String name, Token name_token, Node* value) :
        Node(NodeTypes::NodeNamedArgument, EX), name(name), name_token(name_token), value(value) {}
};

// ---- мета-описание фактических аргументов (для раскладки по сигнатуре) ----

struct CallArgMeta {
    bool named = false;    // ':name = value' vs позиционный
    String name;           // для named — имя параметра
    Token name_token;      // для named — токен имени (каретка диагностик)
};

inline bool nodeIsNamedArgument(const Node* n) {
    return n && n->type == NodeTypes::NodeNamedArgument;
}

// Выражение-значение аргумента (для named — value, иначе сам аргумент).
inline Node* callArgValueNode(Node* n) {
    return nodeIsNamedArgument(n) ? static_cast<struct NodeNamedArgument*>(n)->value : n;
}

inline std::vector<CallArgMeta> buildCallArgMetas(const std::vector<Node*>& args) {
    std::vector<CallArgMeta> metas;
    metas.reserve(args.size());
    for (auto* a : args) {
        CallArgMeta m;
        if (nodeIsNamedArgument(a)) {
            struct NodeNamedArgument* na = static_cast<struct NodeNamedArgument*>(a);
            m.named = true;
            m.name = na->name;
            m.name_token = na->name_token;
        }
        metas.push_back(m);
    }
    return metas;
}

// Раскладывает фактические аргументы (позиционные + именованные) по параметрам
// конкретного кандидата. Позиционные аргументы занимают свободные слоты по
// порядку (обязаны идти ДО первого именованного — это проверяет парсер);
// именованные — слот по имени. Возвращает true при успехе; outArgForParam
// размера paramNames.size(): параметр j -> индекс в metas (-1 = не передан,
// заполняется дефолтом). Невалидная раскладка для кандидата — false.
inline bool placeCallArgs(const std::vector<String>& paramNames,
                          const std::vector<CallArgMeta>& metas,
                          std::vector<int>& outArgForParam) {
    outArgForParam.assign(paramNames.size(), -1);
    std::vector<bool> slotTaken(paramNames.size(), false);
    std::map<String, int> nameToParam;
    for (size_t j = 0; j < paramNames.size(); ++j)
        nameToParam[paramNames[j]] = (int)j;

    for (size_t i = 0; i < metas.size(); ++i) {
        if (!metas[i].named) continue;
        auto it = nameToParam.find(metas[i].name);
        if (it == nameToParam.end()) return false;
        int j = it->second;
        if (slotTaken[j]) return false;
        slotTaken[j] = true;
        outArgForParam[j] = (int)i;
    }
    int posCursor = 0;
    for (size_t i = 0; i < metas.size(); ++i) {
        if (metas[i].named) continue;
        while (posCursor < (int)paramNames.size() && slotTaken[posCursor]) ++posCursor;
        if (posCursor >= (int)paramNames.size()) return false;
        slotTaken[posCursor] = true;
        outArgForParam[posCursor] = (int)i;
        ++posCursor;
    }
    return true;
}