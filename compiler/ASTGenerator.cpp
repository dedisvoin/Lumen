#include "core/Tokens.cpp"
#include "core/Walker.cpp"
#include "core/Nodes/NodeBase.cpp"
#include "core/Nodes/NodeNumber.cpp"
#include "core/Nodes/NodeUnaryExpression.cpp"
#include "core/Nodes/NodeBinaryExpression.cpp"
#include "core/Nodes/NodeScope.cpp"
#include "core/Nodes/NodeVariableDeclaration.cpp"
#include "core/Nodes/NodeVariable.cpp"
#include "core/Nodes/NodeModifier.cpp"
#include "core/Nodes/NodeIf.cpp"
#include "core/Nodes/NodeWhile.cpp"
#include "core/Nodes/NodeFor.cpp"
#include "core/Nodes/NodeForEach.cpp"
#include "core/Nodes/NodeBreak.cpp"
#include "core/Nodes/NodeContinue.cpp"
#include "core/Nodes/NodeReturn.cpp"
#include "core/Nodes/NodeBool.cpp"
#include "core/Nodes/NodeNull.cpp"
#include "core/Nodes/NodeString.cpp"
#include "core/Nodes/NodeNamespace.cpp"
#include "core/Nodes/NodeBinaryScopeResolution.cpp"
#include "core/Nodes/NodeBinaryAssignment.cpp"
#include "core/Nodes/NodeFunctionDeclaration.cpp"
#include "core/Nodes/NodeLambda.cpp"
#include "core/Nodes/NodeNew.cpp"
#include "core/Nodes/NodeTypeOf.cpp"
#include "core/Nodes/NodeMod.cpp"
#include "core/Nodes/NodeSizeOf.cpp"
#include "core/Nodes/NodePtr.cpp"
#include "core/Nodes/NodeStructDeclaration.cpp"
#include "core/Nodes/NodeEnumDeclaration.cpp"
#include "core/Nodes/NodeStructLiteral.cpp"
#include "core/Nodes/NodeFunctionCall.cpp"
#include "core/Nodes/NodeNamedArgument.cpp"
#include "core/Nodes/NodeArrayAccess.cpp"
#include "core/Nodes/NodeFunctionType.cpp"
#include "core/Nodes/NodeTupleType.cpp"
#include "core/Nodes/NodeArrayType.cpp"
#include "core/Nodes/NodeArray.cpp"
#include "core/Nodes/NodeCast.cpp"
#include "core/Nodes/NodeMemberAccess.cpp"
#include "core/Nodes/NodeOut.cpp"
#include "core/Nodes/NodeOutln.cpp"
#include "core/Nodes/NodeDelete.cpp"
#include "core/Nodes/NodeAssert.cpp"
#include "core/Nodes/NodeExit.cpp"
#include "core/Nodes/NodeAppend.cpp"
#include "core/Nodes/NodeReserve.cpp"
#include "core/Nodes/NodeEvalBlock.cpp"
#include "core/Nodes/NodeEcho.cpp"
#include "core/Nodes/NodeInput.cpp"
#include "core/LangSpec.cpp"

#include "utils/TerminalStyle.cpp"
#include "core/ErrorBase.cpp"

#pragma once

using AbstractTreeT = std:: vector<Node*>;


struct ASTGenerator final {

    bool debug_mode;
    Walker walker;
    AbstractTreeT nodes;
    std::vector<Token> source_tokens;

    // Подавление литералов структур вокруг `-> return_type` (fn/lambda): там
    // `T { тело }` должно быть телом функции, а не литералом структуры.
    bool suppress_struct_literals = false;

    // Глубина вложенности для контекстных проверок синтаксиса
    size_t function_depth = 0;
    size_t loop_depth = 0;

    // Глубина вложенности стейтментов: 1 — верхний уровень файла. Нужна для
    // правила размещения блока `eval { ... }` (только top-level).
    size_t statement_depth = 0;

    // Тело struct: в нём extern-декларации (методы без тела) запрещены (фаза 1)
    bool in_struct_body = false;

    // Глубина вложенности namespace: extern fn допустима только на глобальном уровне
    size_t namespace_depth = 0;

    ErrorContainer& error_container;

    ASTGenerator(std::vector<Token> tokens, std::vector<Token> source_tokens, bool debug_mode, ErrorContainer& error_container):
        walker(Walker(tokens)), source_tokens(std::move(source_tokens)),
        debug_mode(debug_mode), error_container(error_container) {}

    // Reports an error, but only one per statement to avoid cascading errors.
    inline void reportError(const BaseError& err) {
        error_container.addError(err);
    }

    // Detects multi-character operators that the lexer split into single tokens
    String multiCharOperator() const noexcept {
        if (walker.isEnd() || !walker.CheckType(TokenTypes::OPERATOR)) return "";
        String two = walker.safeGet().value + walker.safeGet(1).value;
        // трёхсимвольные составные сдвиги (<<= >>=) проверяем раньше двухсимвольных,
        // иначе '<<' совпадёт раньше, чем '<<='.
        if (walker.CheckType(TokenTypes::OPERATOR, 2)) {
            String three = two + walker.safeGet(2).value;
            if (three == "<<=" || three == ">>=") return three;
        }
        static const std::vector<String> known_ops_2 = {
            "==", "!=", "<=", ">=", "&&", "||", "??", "<<", ">>", "::",
            "+=", "-=", "*=", "/=", "&=", "|=", "^=", "%="
        };
        for (auto& op : known_ops_2)
            if (op == two) return op;
        return "";
    }

    Node* parseScope() {
        walker.Next(); // pass (
        auto operand = parseExpression();
        if (!operand)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
        auto node = new struct NodeScope(operand);
        if (!walker.CheckType(TokenTypes::BRACE_RIGHT)) {
            reportError(SYNTAX_ERRORS::MISSING_CLOSING(walker.safeGet(), "(", ")"));
        } else {
            walker.Next();
        }
        return node;
    }

    Node* parsePrimaryExpression() {
        if (walker.CheckType(TokenTypes::NUMBER)) {
            auto token = walker.safeGet();
            if (NodeNumber::isCorrectNumber(token.value)) {
                walker.Next();
                return new struct NodeNumber(token.value);
            } else {
                error_container.addError(SYNTAX_ERRORS::INVALID_NUMBER_FORMAT(token));
                walker.Next();
                return new struct NodeNumber(0);
            }
        } else if (walker.CheckType(TokenTypes::BRACE_LEFT)) {
            return parseScope();
        } else if (walker.CheckType(TokenTypes::LITERAL)) {
            auto token = walker.safeGet();
            walker.Next();
            return new struct NodeVariable(token.value, token);
        } else if (walker.CheckType(TokenTypes::STRING)) {
            auto token = walker.safeGet();
            walker.Next();
            if (token.is_char && token.value.size() != 1)
                error_container.addError(SYNTAX_ERRORS::INVALID_CHAR_LITERAL(token, int(token.value.size())));
            return new struct NodeString(token.value, token.is_char);
        } else if (walker.Check(TokenTypes::KEYWORD, "true") || walker.Check(TokenTypes::KEYWORD, "false")) {
            bool v = walker.safeGet().value == "true";
            walker.Next();
            return new struct NodeBool(v);
        } else if (walker.Check(TokenTypes::KEYWORD, "null")) {
            walker.Next();
            return new struct NodeNull();
        } else if (walker.Check(TokenTypes::KEYWORD, "Func")) {
            return parseFunctionType();
        } else if (walker.Check(TokenTypes::KEYWORD, "Tuple")) {
            return parseTupleType();
        } else if (walker.Check(TokenTypes::KEYWORD, "Array")) {
            return parseArrayType();
        } else if (walker.Check(TokenTypes::KEYWORD, "lambda")) {
            return parseLambda();
        } else if (walker.Check(TokenTypes::KEYWORD, "new")) {
            return parseNew();
        } else if (walker.Check(TokenTypes::KEYWORD, "typeof")) {
            return parseTypeOf();
        } else if (walker.Check(TokenTypes::KEYWORD, "typeis")) {
            return parseTypeOf(true);
        } else if (walker.Check(TokenTypes::KEYWORD, "mod")) {
            return parseMod();
        } else if (walker.Check(TokenTypes::KEYWORD, "sizeof")) {
            return parseSizeOf();
        } else if (walker.Check(TokenTypes::KEYWORD, "ptr")) {
            return parsePtr();
        } else if (walker.Check(TokenTypes::KEYWORD, "input")) {
            return parseInput();
        } else if (walker.Check(TokenTypes::KEYWORD, "if")) {
            // if в выражении: `let a = if (cond) { expr } else { expr };`
            return parseIf();
        } else if (walker.Check(TokenTypes::KEYWORD, "namespace")) {
            return parseNamespace();
        } else if (walker.CheckType(TokenTypes::BRACE_LEFT_CURVE)) {
            return parseArrayLiteral();
        }
        return nullptr;
    }

    Node* parseArrayLiteral() {
        walker.Next(); // съедаем {

        std::vector<Node*> elements;
        if (!walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE)) {
            elements.push_back(parseExpression());
            while (walker.Check(TokenTypes::OPERATOR, ",")) {
                walker.Next();
                elements.push_back(parseExpression());
            }
        }
        if (!walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "}"));
        else
            walker.Next();

        return new struct NodeArray(elements);
    }

    Node* parseFunctionType() {
        walker.Next(); // съедаем Func

        // заголовок [ ... ] — генерик-аргументы типа (новый синтаксис) либо
        // тип-параметры (старый синтаксис Func[P, ...] -> R без '(')
        if (!walker.CheckType(TokenTypes::BRACE_LEFT_RECT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "["));
        walker.Next();

        std::vector<Node*> head;
        if (!walker.CheckType(TokenTypes::BRACE_RIGHT_RECT)) {
            head.push_back(parseExpression());
            while (walker.Check(TokenTypes::OPERATOR, ",")) {
                walker.Next();
                head.push_back(parseExpression());
            }
        }
        if (!walker.CheckType(TokenTypes::BRACE_RIGHT_RECT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "]"));
        else
            walker.Next();

        // Func[generics](params) -> ret
        std::vector<Node*> generics;
        std::vector<Node*> params;
        if (walker.CheckType(TokenTypes::BRACE_LEFT)) {
            generics = std::move(head);
            walker.Next(); // съедаем (
            if (!walker.CheckType(TokenTypes::BRACE_RIGHT)) {
                params.push_back(parseExpression());
                while (walker.Check(TokenTypes::OPERATOR, ",")) {
                    walker.Next();
                    params.push_back(parseExpression());
                }
            }
            if (!walker.CheckType(TokenTypes::BRACE_RIGHT))
                reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ")"));
            else
                walker.Next();
        } else {
            // старый синтаксис Func[P, ...] -> R: скобки содержали тип-параметры
            params = std::move(head);
        }

        Node* return_type = nullptr;
        bool saved_suppress = suppress_struct_literals;
        suppress_struct_literals = true;   // `-> Array[i64, 2] {…}`: `{` — тело/литерал
        if (walker.Check(TokenTypes::OPERATOR, "-") && !walker.isEnd() && walker.safeGet(1).value == ">") {
            walker.Next(); walker.Next(); // съедаем ->
            return_type = parseExpression();
            if (!return_type)
                reportError(SYNTAX_ERRORS::WAITING_TYPE_EXPRESSION(walker.safeGet()));
        }
        suppress_struct_literals = saved_suppress;

        return new struct NodeFunctionType(generics, params, return_type);
    }

    Node* parseTupleType() {
        walker.Next(); // съедаем Tuple

        if (!walker.CheckType(TokenTypes::BRACE_LEFT_RECT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "["));
        walker.Next();

        std::vector<Node*> types;
        if (walker.CheckType(TokenTypes::BRACE_RIGHT_RECT)) {
            // пустой Tuple[] недопустим: нужен хотя бы один type_expr
            reportError(SYNTAX_ERRORS::TUPLE_REQUIRES_TYPE(walker.safeGet()));
        } else {
            types.push_back(parseExpression());
            if (!types.back())
                reportError(SYNTAX_ERRORS::WAITING_TYPE_EXPRESSION(walker.safeGet()));
            while (walker.Check(TokenTypes::OPERATOR, ",")) {
                walker.Next();
                types.push_back(parseExpression());
                if (!types.back())
                    reportError(SYNTAX_ERRORS::WAITING_TYPE_EXPRESSION(walker.safeGet()));
            }
        }
        if (!walker.CheckType(TokenTypes::BRACE_RIGHT_RECT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "]"));
        else
            walker.Next();

        return new struct NodeTupleType(types);
    }

    Node* parseArrayType() {
        walker.Next(); // съедаем Array

        if (!walker.CheckType(TokenTypes::BRACE_LEFT_RECT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "["));
        walker.Next();

        Node* element_type = nullptr;
        Node* count = nullptr;
        Token count_token;
        if (walker.CheckType(TokenTypes::BRACE_RIGHT_RECT)) {
            // пустой Array[] недопустим: element-тип обязателен (type_expr)
            reportError(SYNTAX_ERRORS::ARRAY_REQUIRES_TYPE(walker.safeGet()));
        } else {
            element_type = parseExpression();
            if (!element_type)
                reportError(SYNTAX_ERRORS::WAITING_TYPE_EXPRESSION(walker.safeGet()));
            if (walker.Check(TokenTypes::OPERATOR, ",")) {
                walker.Next();
                count_token = walker.safeGet();
                count = parseExpression();
                if (!count)
                    reportError(SYNTAX_ERRORS::WAITING_TYPE_EXPRESSION(walker.safeGet()));
            }
        }
        if (!walker.CheckType(TokenTypes::BRACE_RIGHT_RECT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "]"));
        else
            walker.Next();

        auto* at = new struct NodeArrayType(element_type, count, count_token);

        // `Array[T, N]{…}` — конструктор: тип элемента и длина берутся из
        // `Array[…]`, тело — позиционный список элементов. Без '{' остаётся
        // обычная типовая аннотация (`let x: Array[i64, 3] = …`).
        // suppress_struct_literals → `fn f() -> Array[i64, 2] { тело }`:
        // `{` — тело функции, а не список элементов.
        if (!suppress_struct_literals && walker.CheckType(TokenTypes::BRACE_LEFT_CURVE)) {
            at->has_body = true;
            at->open_token = walker.safeGet();
            walker.Next(); // съедаем {
            if (!walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE)) {
                while (true) {
                    Node* item = parseExpression();
                    if (!item) {
                        reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
                        break;
                    }
                    at->items.push_back(item);
                    if (walker.Check(TokenTypes::OPERATOR, ",")) {
                        walker.Next();
                        if (walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE)) break; // хвостовая запятая
                        continue;
                    }
                    break;
                }
            }
            at->close_token = walker.safeGet();
            if (!walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE))
                reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "}"));
            else
                walker.Next();
        }
        return at;
    }

    // Литеральным «именем типа» может быть: простая переменная (Point),
    // ::-цепочка (Geo::Point), уже построенный литерал (члены после {…}.x),
    // либо выражение, возвращающее тип структуры: (*p){…}, (P){…}, create(){…}.
    static bool isStructLiteralBase(Node* n) {
        while (n && n->type == NodeTypes::NodeScope)
            n = static_cast<struct NodeScope*>(n)->node;
        if (!n) return false;
        if (n->type == NodeTypes::NodeVariable) return true;
        if (n->type == NodeTypes::NodeBinaryScopeResolution) return true;
        if (n->type == NodeTypes::NodeStructLiteral) return true;
        if (n->type == NodeTypes::NodeUnaryDereference) return true;
        if (n->type == NodeTypes::NodeFunctionCall) return true;
        return false;
    }

    // `Point { x: 1, y: 2 }` / `Vec[i64, 8] { … }` — парсит { поля }. Текущий
    // токен должен быть '{'. Возвращает NodeStructLiteral (base+gen_args остаются).
    Node* parseStructLiteralBody(Node* base, const std::vector<Node*>& gen_args) {
        Token openTok = walker.safeGet();
        walker.Next(); // съедаем {
        std::vector<StructFieldInit> inits;
        if (!walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE)) {
            while (true) {
                if (!walker.CheckType(TokenTypes::LITERAL)) {
                    reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "field name"));
                    break;
                }
                Token nameTok = walker.safeGet();
                String fname = nameTok.value;
                walker.Next();
                Node* value = nullptr;
                if (walker.Check(TokenTypes::OPERATOR, ":")) {
                    walker.Next();
                    value = parseExpression();
                    if (!value)
                        reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
                } else {
                    // шорткат { x } — поле из одноимённой переменной
                    value = new struct NodeVariable(fname, nameTok);
                }
                inits.push_back(StructFieldInit{fname, nameTok, value});
                if (walker.Check(TokenTypes::OPERATOR, ",")) {
                    walker.Next();
                    if (walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE)) break; // хвостовая запятая
                    continue;
                }
                break;
            }
        }
        Token closeTok = walker.safeGet();
        if (!walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "}"));
        else
            walker.Next();
        return new struct NodeStructLiteral(base, gen_args, std::move(inits), openTok, closeTok);
    }

    Node* parsePostfixExpression() {
        auto node = parsePrimaryExpression();
        if (!node) return nullptr;
        std::vector<Node*> pending_generics;   // генерик-аргументы для следующего вызова
        Token genOpenTok, genCloseTok;          // токены [...] генерик-аргументов вызова
        while (true) {
            // Структурный литерал: `Point { … }` / `Vec[i64] { … }` /
            // `Geo::Point { … }` / `(литерал).member`. Не затрагивает if/while/
            // for/fn/struct (их тела парсятся в parseStatement, не в постфиксе).
            if (walker.CheckType(TokenTypes::BRACE_LEFT_CURVE) && isStructLiteralBase(node)
                && !suppress_struct_literals) {
                node = parseStructLiteralBody(node, pending_generics);
                pending_generics.clear();
                continue;
            }
            String op = multiCharOperator();
            if (op == "::") {
                walker.Next(); walker.Next();
                auto right = parsePrimaryExpression();
                if (!right) {
                    reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "::"));
                    break;
                }
                Token mtok = (right->type == NodeTypes::NodeVariable)
                    ? static_cast<struct NodeVariable*>(right)->token : Token();
                node = new struct NodeBinaryScopeResolution(node, right, mtok);
                continue;
            }
            if (walker.CheckType(TokenTypes::BRACE_LEFT)) {
                Token openTok = walker.safeGet();
                walker.Next(); // съедаем (
                std::vector<Node*> args;
                bool saw_named = false;
                if (!walker.CheckType(TokenTypes::BRACE_RIGHT)) {
                    while (true) {
                        // Именованный аргумент: `:имя = выражение`. Синтаксис задан
                        // так, чтобы не конфликтовать ни с чем: `:` не имеет
                        // префиксных/бинарных вхождений в выражениях Lumen.
                        if (walker.Check(TokenTypes::OPERATOR, ":")) {
                            walker.Next(); // ':'
                            if (!walker.CheckType(TokenTypes::LITERAL)) {
                                reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "argument name"));
                                break;
                            }
                            Token nameTok = walker.safeGet();
                            walker.Next();
                            if (!walker.Check(TokenTypes::OPERATOR, "=")) {
                                reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "="));
                                break;
                            }
                            walker.Next();
                            Node* val = parseExpression();
                            if (!val) {
                                reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
                                break;
                            }
                            args.push_back(new struct NodeNamedArgument(nameTok.value, nameTok, val));
                            saw_named = true;
                        } else {
                            if (saw_named) {
                                reportError(SYNTAX_ERRORS::POSITIONAL_AFTER_NAMED_ARG(walker.safeGet()));
                                break;
                            }
                            Node* a = parseExpression();
                            if (!a) break;
                            args.push_back(a);
                        }
                        if (walker.Check(TokenTypes::OPERATOR, ",")) {
                            walker.Next();
                            if (walker.CheckType(TokenTypes::BRACE_RIGHT)) break; // хвостовая запятая
                            continue;
                        }
                        break;
                    }
                }
                Token closeTok = walker.safeGet();
                if (!walker.CheckType(TokenTypes::BRACE_RIGHT))
                    reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ")"));
                else
                    walker.Next();
                Token calleeTok = (node->type == NodeTypes::NodeVariable)
                    ? static_cast<struct NodeVariable*>(node)->token : openTok;
                node = new struct NodeFunctionCall(node, args, pending_generics, openTok, closeTok, genOpenTok, genCloseTok, calleeTok);
                pending_generics.clear();
                continue;
            }
            if (walker.Check(TokenTypes::OPERATOR, ".")) {
                walker.Next(); // съедаем .
                if (!walker.CheckType(TokenTypes::LITERAL)) {
                    reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "member name"));
                    break;
                }
                Token mtok = walker.safeGet();
                auto right = new struct NodeVariable(mtok.value, mtok);
                walker.Next();
                node = new struct NodeMemberAccess(node, right, mtok);
                continue;
            }
            if (walker.CheckType(TokenTypes::BRACE_LEFT_RECT)) {
                genOpenTok = walker.safeGet();
                walker.Next(); // съедаем [
                std::vector<Node*> idx;
                if (!walker.CheckType(TokenTypes::BRACE_RIGHT_RECT)) {
                    idx.push_back(parseExpression());
                    while (walker.Check(TokenTypes::OPERATOR, ",")) {
                        walker.Next();
                        idx.push_back(parseExpression());
                    }
                }
                genCloseTok = walker.safeGet();
                if (!walker.CheckType(TokenTypes::BRACE_RIGHT_RECT))
                    reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "]"));
                else
                    walker.Next();

                // Дизамбигуация:
                //   expr[gen](args) -> генерик-вызов: аргументы кладём в
                //                      pending_generics, вызов соберётся в '('
                //   expr[gen] { … }  -> структурный литерал с генерик-аргументами
                //   expr[i]          -> индексация (узел NodeArrayAccess)
                // Под suppress_struct_literals (`-> T { тело }`) `[gen]` перед
                // фигурной скобкой — generic-инстанциация типа (Pair[SecondT, FirstT]),
                // а `{` — тело функции/лямбды, а не литерал: скобки не откладываем.
                bool followed_by_call = walker.CheckType(TokenTypes::BRACE_LEFT);
                bool followed_by_curly = walker.CheckType(TokenTypes::BRACE_LEFT_CURVE);

                if (followed_by_call || (followed_by_curly && !suppress_struct_literals)) {
                    pending_generics = idx;
                } else {
                    if (idx.empty()) {
                        reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
                        break;
                    }
                    node = new struct NodeArrayAccess(node, idx, genOpenTok);
                }
                continue;
            }
            // постфиксные ++ / -- : i++ / i-- (два одинаковых символа оператора)
            if (walker.Check(TokenTypes::OPERATOR, "+") && walker.safeGet(1).value == "+") {
                Token op = walker.safeGet();
                walker.Next(); walker.Next();
                node = new struct NodeUnaryPostfixIncrement(node, op);
                continue;
            }
            if (walker.Check(TokenTypes::OPERATOR, "-") && walker.safeGet(1).value == "-") {
                Token op = walker.safeGet();
                walker.Next(); walker.Next();
                node = new struct NodeUnaryPostfixDecrement(node, op);
                continue;
            }
            // постфиксный ? : развернуть через __get__, если метод есть.
            // Проверка multiCharOperator() обязательна — иначе 'a ?? b'
            // разобрался бы как '(a?) ?? b'.
            if (walker.Check(TokenTypes::OPERATOR, "?") && multiCharOperator() != "??") {
                Token op = walker.safeGet();
                walker.Next();
                node = new struct NodePostfixUnwrap(node, op);
                continue;
            }
            break;
        }
        return node;
    }

    Node* parseUnaryExpression() {
        Node* operand;
        Token op = walker.safeGet();
        if (walker.Check(TokenTypes::OPERATOR, "-")) {
            walker.Next();
            // префиксный -- : два подряд идущих токена "-"
            if (walker.Check(TokenTypes::OPERATOR, "-")) {
                walker.Next();
                Node* dec = parseUnaryExpression();
                if (!dec) reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "--"));
                return new struct NodeUnaryPrefixDecrement(dec, op);
            }
            operand = parseUnaryExpression();
            if (!operand) reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "-"));
            return new struct NodeUnaryMinus(operand, op);
        } else if (walker.Check(TokenTypes::OPERATOR, "+")) {
            walker.Next();
            // префиксный ++ : два подряд идущих токена "+"
            if (walker.Check(TokenTypes::OPERATOR, "+")) {
                walker.Next();
                Node* inc = parseUnaryExpression();
                if (!inc) reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "++"));
                return new struct NodeUnaryPrefixIncrement(inc, op);
            }
            operand = parseUnaryExpression();
            if (!operand) reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "+"));
            return new struct NodeUnaryPlus(operand, op);
        } else if (walker.Check(TokenTypes::OPERATOR, "!")) {
            walker.Next();
            operand = parseUnaryExpression();
            if (!operand) reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "!"));
            return new struct NodeUnaryNegation(operand, op);
        } else if (walker.Check(TokenTypes::OPERATOR, "?")) {
            walker.Next();
            operand = parseUnaryExpression();
            if (!operand) reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "?"));
            return new struct NodeUnaryNegation(operand, op);
        } else if (walker.Check(TokenTypes::OPERATOR, "*")) {
            walker.Next();
            operand = parseUnaryExpression();
            if (!operand) reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "*"));
            return new struct NodeUnaryDereference(operand, op);
        } else if (walker.Check(TokenTypes::OPERATOR, "~")) {
            walker.Next();
            operand = parseUnaryExpression();
            if (!operand) reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "~"));
            return new struct NodeUnaryBitwiseNot(operand, op);
        } else if (walker.Check(TokenTypes::OPERATOR, "&") && multiCharOperator() != "&&") {
            walker.Next();
            operand = parseUnaryExpression();
            if (!operand) reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "&"));
            return new struct NodeUnaryAddressOf(operand, op);
        }
        return parsePostfixExpression();
    }

    Node* parseCastExpression() {
        auto l_operand = parseUnaryExpression();
        if (!l_operand) return nullptr;

        if (walker.Check(TokenTypes::KEYWORD, "as")) {
            Token asTok = walker.safeGet();
            walker.Next(); // съедаем as
            auto r_operand = parseCastExpression();
            if (!r_operand) {
                reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "as"));
                return l_operand;
            }
            return new struct NodeCast(l_operand, r_operand, asTok);
        }
        return l_operand;
    }

    Node* parseMultiplicativeExpression() {
        auto start = walker.safeGet();

        auto l_operand = parseCastExpression();
        if (!l_operand) return nullptr;

        while (true) {
            String mop = multiCharOperator();
            if (mop == "*=" || mop == "/=" || mop == "%=") break;   // составное присваивание — оставляем parseStatement
            if (walker.Check(TokenTypes::OPERATOR, "*")) {
                walker.Next();
                auto r_operand = parseUnaryExpression();
                if (!r_operand) {
                    reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "*"));
                    break;
                }
                l_operand = new struct NodeBinaryMultiplicative(l_operand, r_operand, start, walker.safeGet(-1));
            } else if (walker.Check(TokenTypes::OPERATOR, "/")) {
                walker.Next();
                auto r_operand = parseUnaryExpression();
                if (!r_operand) {
                    reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "/"));
                    break;
                }
                l_operand = new struct NodeBinaryDivision(l_operand, r_operand, start, walker.safeGet(-1));
            } else if (walker.Check(TokenTypes::OPERATOR, "%")) {
                walker.Next();
                auto r_operand = parseUnaryExpression();
                if (!r_operand) {
                    reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "%"));
                    break;
                }
                l_operand = new struct NodeBinaryRemainder(l_operand, r_operand, start, walker.safeGet(-1));
            } else {
                break;
            }
        }
        return l_operand;
    }

    Node* parseAdditiveExpression() {
        auto start = walker.safeGet();

        auto l_operand = parseMultiplicativeExpression();
        if (!l_operand) return nullptr;

        while (true) {
            String mop = multiCharOperator();
            if (mop == "+=" || mop == "-=") break;   // составное присваивание — оставляем parseStatement
            if (walker.Check(TokenTypes::OPERATOR, "-")) {
                walker.Next();
                auto r_operand = parseMultiplicativeExpression();
                if (!r_operand) {
                    reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "-"));
                    break;
                }
                l_operand = new struct NodeBinarySubtraction(l_operand, r_operand, start, walker.safeGet(-1));
            } else if (walker.Check(TokenTypes::OPERATOR, "+")) {
                walker.Next();
                auto r_operand = parseMultiplicativeExpression();
                if (!r_operand) {
                    reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "+"));
                    break;
                }
                l_operand = new struct NodeBinaryAdditive(l_operand, r_operand, start, walker.safeGet(-1));
            } else {
                break;
            }
        }
        return l_operand;
    }

    Node* parseShiftExpression() {
        auto start = walker.safeGet();

        auto l_operand = parseAdditiveExpression();
        if (!l_operand) return nullptr;

        while (true) {
            String op = multiCharOperator();
            if (op == "<<=" || op == ">>=") break;   // составное присваивание — оставляем parseStatement
            if (op == "<<") {
                walker.Next(); walker.Next();
                auto r_operand = parseAdditiveExpression();
                if (!r_operand) {
                    reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "<<"));
                    break;
                }
                l_operand = new struct NodeBinaryShiftLeft(l_operand, r_operand, start, walker.safeGet(-1));
            } else if (op == ">>") {
                walker.Next(); walker.Next();
                auto r_operand = parseAdditiveExpression();
                if (!r_operand) {
                    reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), ">>"));
                    break;
                }
                l_operand = new struct NodeBinaryShiftRight(l_operand, r_operand, start, walker.safeGet(-1));
            } else {
                break;
            }
        }
        return l_operand;
    }

    Node* parseComparisonExpression() {
        auto start = walker.safeGet();

        auto l_operand = parseShiftExpression();
        if (!l_operand) return nullptr;

        while (true) {
            String op = multiCharOperator();
            if (op == "<<=" || op == ">>=") break;   // составное присваивание — оставляем parseStatement
            if (op == "==") {
                walker.Next(); walker.Next();
                auto r_operand = parseShiftExpression();
                if (!r_operand) { reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "==")); break; }
                l_operand = new struct NodeBinaryEqual(l_operand, r_operand, start, walker.safeGet(-1));
            } else if (op == "!=") {
                walker.Next(); walker.Next();
                auto r_operand = parseShiftExpression();
                if (!r_operand) { reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "!=")); break; }
                l_operand = new struct NodeBinaryNotEqual(l_operand, r_operand, start, walker.safeGet(-1));
            } else if (op == "<=") {
                walker.Next(); walker.Next();
                auto r_operand = parseShiftExpression();
                if (!r_operand) { reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "<=")); break; }
                l_operand = new struct NodeBinaryLessEqual(l_operand, r_operand, start, walker.safeGet(-1));
            } else if (op == ">=") {
                walker.Next(); walker.Next();
                auto r_operand = parseShiftExpression();
                if (!r_operand) { reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), ">=")); break; }
                l_operand = new struct NodeBinaryGreaterEqual(l_operand, r_operand, start, walker.safeGet(-1));
            } else if (walker.Check(TokenTypes::OPERATOR, "<")) {
                walker.Next();
                auto r_operand = parseShiftExpression();
                if (!r_operand) { reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "<")); break; }
                l_operand = new struct NodeBinaryLess(l_operand, r_operand, start, walker.safeGet(-1));
            } else if (walker.Check(TokenTypes::OPERATOR, ">")) {
                walker.Next();
                auto r_operand = parseShiftExpression();
                if (!r_operand) { reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), ">")); break; }
                l_operand = new struct NodeBinaryGreater(l_operand, r_operand, start, walker.safeGet(-1));
            } else {
                break;
            }
        }
        return l_operand;
    }

    Node* parseBitwiseAndExpression() {
        auto start = walker.safeGet();
        auto l_operand = parseComparisonExpression();
        if (!l_operand) return nullptr;

        while (true) {
            String op = multiCharOperator();
            if (op == "&&") break;   // логическое И разбирается уровнем выше
            if (op == "&=") break;   // составное присваивание — оставляем parseStatement
            if (walker.Check(TokenTypes::OPERATOR, "&")) {
                walker.Next();
                auto r_operand = parseComparisonExpression();
                if (!r_operand) { reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "&")); break; }
                l_operand = new struct NodeBinaryBitwiseAnd(l_operand, r_operand, start, walker.safeGet(-1));
            } else {
                break;
            }
        }
        return l_operand;
    }

    Node* parseBitwiseXorExpression() {
        auto start = walker.safeGet();
        auto l_operand = parseBitwiseAndExpression();
        if (!l_operand) return nullptr;

        while (true) {
            String op = multiCharOperator();
            if (op == "^=") break;   // составное присваивание — оставляем parseStatement
            if (walker.Check(TokenTypes::OPERATOR, "^")) {
                walker.Next();
                auto r_operand = parseBitwiseAndExpression();
                if (!r_operand) { reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "^")); break; }
                l_operand = new struct NodeBinaryBitwiseXor(l_operand, r_operand, start, walker.safeGet(-1));
            } else {
                break;
            }
        }
        return l_operand;
    }

    Node* parseBitwiseOrExpression() {
        auto start = walker.safeGet();
        auto l_operand = parseBitwiseXorExpression();
        if (!l_operand) return nullptr;

        while (true) {
            String op = multiCharOperator();
            if (op == "||") break;   // логическое ИЛИ разбирается уровнем выше
            if (op == "|=") break;   // составное присваивание — оставляем parseStatement
            if (walker.Check(TokenTypes::OPERATOR, "|")) {
                walker.Next();
                auto r_operand = parseBitwiseXorExpression();
                if (!r_operand) { reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "|")); break; }
                l_operand = new struct NodeBinaryBitwiseOr(l_operand, r_operand, start, walker.safeGet(-1));
            } else {
                break;
            }
        }
        return l_operand;
    }

    Node* parseLogicalAndExpression() {
        auto start = walker.safeGet();
        auto l_operand = parseBitwiseOrExpression();
        if (!l_operand) return nullptr;

        while (true) {
            String op = multiCharOperator();
            if (op == "&&") {
                walker.Next(); walker.Next();
                auto r_operand = parseBitwiseOrExpression();
                if (!r_operand) { reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "&&")); break; }
                l_operand = new struct NodeBinaryLogicalAnd(l_operand, r_operand, start, walker.safeGet(-1));
            } else {
                break;
            }
        }
        return l_operand;
    }

    Node* parseLogicalOrExpression() {
        auto start = walker.safeGet();
        auto l_operand = parseLogicalAndExpression();
        if (!l_operand) return nullptr;

        while (true) {
            String op = multiCharOperator();
            if (op == "||") {
                walker.Next(); walker.Next();
                auto r_operand = parseLogicalAndExpression();
                if (!r_operand) { reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "||")); break; }
                l_operand = new struct NodeBinaryLogicalOr(l_operand, r_operand, start, walker.safeGet(-1));
            } else {
                break;
            }
        }
        return l_operand;
    }

    // ?? — самый низкий приоритет среди бинарных операторов и правоассоциативный:
    // a ?? b ?? c  раскладывается в a ?? (b ?? c), поэтому цепочка работает без скобок.
    Node* parseNullCoalesceExpression() {
        auto start = walker.safeGet();
        auto l_operand = parseLogicalOrExpression();
        if (!l_operand) return nullptr;

        while (true) {
            String op = multiCharOperator();
            if (op == "??") {
                walker.Next(); walker.Next();
                auto r_operand = parseNullCoalesceExpression();
                if (!r_operand) { reportError(SYNTAX_ERRORS::MISSING_OPERAND(walker.safeGet(), "??")); break; }
                l_operand = new struct NodeBinaryNullCoalesce(l_operand, r_operand, start, walker.safeGet(-1));
            } else {
                break;
            }
        }
        return l_operand;
    }

    Node* parseExpression() {
        return parseNullCoalesceExpression();
    }

    Node* parseVariableDeclaration() {
        walker.Next();

        if (!walker.CheckType(TokenTypes::LITERAL)) {
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "variable name"));
            walker.passStatement();
            return nullptr;
        }
        String name = walker.safeGet().value;
        Token nameTok = walker.safeGet();
        walker.Next();

        Node* type_node = nullptr;
        if (walker.Check(TokenTypes::OPERATOR, ":")) {
            walker.Next();
            type_node = parseExpression();
            if (!type_node) {
                reportError(SYNTAX_ERRORS::WAITING_TYPE_EXPRESSION(walker.safeGet()));
                walker.passStatement();
                return nullptr;
            }
        }

        Node* value_node = nullptr;
        if (walker.Check(TokenTypes::OPERATOR, "=")) {
            walker.Next();
            value_node = parseExpression();
            if (!value_node)
                reportError(SYNTAX_ERRORS::MISSING_VALUE(walker.safeGet()));
        }

        if (!walker.Check(TokenTypes::OPERATOR, ";")) {
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
            walker.passStatement();
            return nullptr;
        }
        walker.Next();

        return new struct NodeVariableDeclaration(name, value_node, type_node, nameTok);
    }

    Node* parseStatementScope() {
        std::vector<Node*> nodes;
        walker.Next();

        while (!walker.isEnd() && !walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE)) {
            size_t before = walker.index;
            Node* stmt = parseStatement();
            if (stmt) nodes.push_back(stmt);
            if (walker.index == before) {
                reportError(SYNTAX_ERRORS::EXPECTED_STATEMENT(walker.safeGet()));
                walker.Next();
            }
        }
        if (!walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "}"));
        walker.Next();
        return new struct NodeStatementsScope(nodes);
    }

    Node* parseNamespace() {
        walker.Next(); // съедаем namespace

        String name;
        Token nameTok;
        if (walker.CheckType(TokenTypes::LITERAL)) {
            name = walker.safeGet().value;
            nameTok = walker.safeGet();
            walker.Next();
        }

        if (!walker.CheckType(TokenTypes::BRACE_LEFT_CURVE))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "{"));

        namespace_depth++;
        Node* body = parseStatement(); // тело namespace — блок {…} (NodeStatementsScope)
        namespace_depth--;
        return new struct NodeNamespace(name, body, nameTok);
    }

    Node* parseStruct() {
        Token structTok = walker.safeGet(); // токен 'struct' (для док-комментария выше)
        walker.Next(); // съедаем struct

        // struct[G, Size: i64 = -1] Name { ... } — дженерик-параметры до имени
        std::vector<GenericParam> generics;
        Token genOpenTok, genCloseTok;
        if (walker.CheckType(TokenTypes::BRACE_LEFT_RECT)) {
            genOpenTok = walker.safeGet();
            walker.Next();
            if (!walker.CheckType(TokenTypes::BRACE_RIGHT_RECT)) {
                while (true) {
                    generics.push_back(parseGenericParameter());
                    if (walker.Check(TokenTypes::OPERATOR, ",")) {
                        walker.Next();
                        continue;
                    }
                    break;
                }
            }
            genCloseTok = walker.safeGet();
            if (!walker.CheckType(TokenTypes::BRACE_RIGHT_RECT))
                reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "]"));
            else
                walker.Next();
        }

        if (!walker.CheckType(TokenTypes::LITERAL)) {
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "struct name"));
            walker.passStatement();
            return nullptr;
        }
        String name = walker.safeGet().value;
        Token nameTok = walker.safeGet();
        walker.Next();

        bool saved_struct_body = in_struct_body;
        in_struct_body = true;
        Node* body = parseStatement(); // тело struct — блок {…} (NodeStatementsScope)
        in_struct_body = saved_struct_body;
        if (!body)
            reportError(SYNTAX_ERRORS::EXPECTED_STATEMENT(walker.safeGet()));

        auto* st = new struct NodeStructDeclaration(name, body, nameTok, std::move(generics));
        st->doc_comment = docCommentAbove(structTok.position);
        return st;
    }

    Node* parseEnum() {
        Token enumTok = walker.safeGet(); // токен 'enum' (для док-комментария выше)
        walker.Next(); // съедаем enum

        if (!walker.CheckType(TokenTypes::LITERAL)) {
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "enum name"));
            walker.passStatement();
            return nullptr;
        }
        String name = walker.safeGet().value;
        Token nameTok = walker.safeGet();
        walker.Next();

        std::vector<Token> variants;
        if (!walker.CheckType(TokenTypes::BRACE_LEFT_CURVE))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "{"));
        else
            walker.Next();

        if (!walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE)) {
            while (true) {
                if (!walker.CheckType(TokenTypes::LITERAL)) {
                    reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "variant name"));
                    break;
                }
                Token vt = walker.safeGet();
                variants.push_back(vt);
                walker.Next();
                if (walker.Check(TokenTypes::OPERATOR, ",")) {
                    walker.Next();
                    if (walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE)) break; // хвостовая запятая
                    continue;
                }
                break;
            }
        }
        if (!walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "}"));
        else
            walker.Next();

        auto* en = new struct NodeEnumDeclaration(name, std::move(variants), nameTok);
        en->doc_comment = docCommentAbove(enumTok.position);
        return en;
    }

    FuncArg parseFunctionParameter() {
        std::vector<String> mods;
        while (walker.CheckType(TokenTypes::KEYWORD) && isModifierKeyword(walker.safeGet().value)) {
            mods.push_back(walker.safeGet().value);
            walker.Next();
        }

        if (!walker.CheckType(TokenTypes::LITERAL)) {
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "parameter name"));
            walker.passStatement();
            return FuncArg();
        }
        String pname = walker.safeGet().value;
        walker.Next();

        Node* type_node = nullptr;
        if (walker.Check(TokenTypes::OPERATOR, ":")) {
            walker.Next();
            type_node = parseExpression();
            if (!type_node)
                reportError(SYNTAX_ERRORS::WAITING_TYPE_EXPRESSION(walker.safeGet()));
        }

        Node* default_value = nullptr;
        if (walker.Check(TokenTypes::OPERATOR, "=") && walker.safeGet(1).value != "=") {
            walker.Next();
            default_value = parseExpression();
            if (!default_value)
                reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
        }

        return FuncArg(pname, mods, type_node, default_value);
    }

    GenericParam parseGenericParameter() {
        std::vector<String> mods;
        while (walker.CheckType(TokenTypes::KEYWORD) && isModifierKeyword(walker.safeGet().value)) {
            mods.push_back(walker.safeGet().value);
            walker.Next();
        }

        if (!walker.CheckType(TokenTypes::LITERAL)) {
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "generic parameter name"));
            walker.passStatement();
            return GenericParam();
        }
        String gname = walker.safeGet().value;
        Token gnameTok = walker.safeGet();
        walker.Next();

        Node* type_node = nullptr;
        if (walker.Check(TokenTypes::OPERATOR, ":")) {
            walker.Next();
            type_node = parseExpression();
            if (!type_node)
                reportError(SYNTAX_ERRORS::WAITING_TYPE_EXPRESSION(walker.safeGet()));
        }

        Node* default_value = nullptr;
        if (walker.Check(TokenTypes::OPERATOR, "=") && walker.safeGet(1).value != "=") {
            walker.Next();
            default_value = parseExpression();
            if (!default_value)
                reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
        }

        return GenericParam(gname, gnameTok, mods, type_node, default_value);
    }

    // extern fn hello() -> str; — декларация функции, определённой в C++-файле,
    // подключённом директивой #cppinclude. Тела нет; генерация эмитит вызов lf_*,
    // а определение приходит из подключённого .cpp.
    Node* parseExternFunction() {
        Token extTok = walker.safeGet(); // токен 'extern' (для док-комментария выше)
        walker.Next(); // съедаем extern

        if (in_struct_body) {
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "extern functions are not supported as struct members"));
            walker.passStatement();
            return nullptr;
        }

        // extern fn привязана к глобальному C++-символу: вложенная декларация
        // (namespace/функция) даст другое манглированное имя, чем определение
        // в #cppinclude-файле, — линковка не найдёт символ.
        if (namespace_depth > 0 || function_depth > 0) {
            reportError(SYNTAX_ERRORS::EXTERN_NOT_GLOBAL(extTok));
            walker.passStatement();
            return nullptr;
        }

        if (!walker.Check(TokenTypes::KEYWORD, "fn")) {
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "fn after 'extern'"));
            walker.passStatement();
            return nullptr;
        }

        Node* fn_node = parseFunction(true);
        if (fn_node && fn_node->type == NodeTypes::NodeFunctionDeclaration) {
            auto* f = static_cast<struct NodeFunctionDeclaration*>(fn_node);
            f->is_extern = true;
            f->doc_comment = docCommentAbove(extTok.position);
        }
        return fn_node;
    }

    Node* parseFunction(bool is_extern = false) {
        Token fnTok = walker.safeGet(); // токен 'fn' (для док-комментария выше)
        walker.Next(); // съедаем fn

        if (!walker.CheckType(TokenTypes::LITERAL)) {
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "function name"));
            walker.passStatement();
            return nullptr;
        }
        String name = walker.safeGet().value;
        Token nameTok = walker.safeGet();
        walker.Next();

        std::vector<GenericParam> generics;
        Token genOpenTok, genCloseTok;
        if (walker.CheckType(TokenTypes::BRACE_LEFT_RECT)) {
            genOpenTok = walker.safeGet();
            walker.Next(); // съедаем [
            while (!walker.CheckType(TokenTypes::BRACE_RIGHT_RECT) && !walker.isEnd()) {
                generics.push_back(parseGenericParameter());
                if (walker.Check(TokenTypes::OPERATOR, ","))
                    walker.Next();
                else
                    break;
            }
            genCloseTok = walker.safeGet();
            if (!walker.CheckType(TokenTypes::BRACE_RIGHT_RECT))
                reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "]"));
            else
                walker.Next();
        }

        if (is_extern && !generics.empty()) {
            reportError(SYNTAX_ERRORS::WAITING(nameTok, "extern functions do not support generic parameters"));
            generics.clear();
        }

        if (!walker.CheckType(TokenTypes::BRACE_LEFT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "("));
        Token argsOpenTok = walker.safeGet();
        walker.Next();

        std::vector<FuncArg> params;
        while (!walker.CheckType(TokenTypes::BRACE_RIGHT) && !walker.isEnd()) {
            params.push_back(parseFunctionParameter());
            if (walker.Check(TokenTypes::OPERATOR, ","))
                walker.Next();
            else
                break;
        }
        Token argsCloseTok = walker.safeGet();
        if (!walker.CheckType(TokenTypes::BRACE_RIGHT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ")"));
        else
            walker.Next();

        Node* return_type = nullptr;
        bool returns_ref = false;
        bool saved_suppress = suppress_struct_literals;
        suppress_struct_literals = true;   // `-> T { тело }`: `{` — тело, не литерал
        if (walker.Check(TokenTypes::OPERATOR, "-") && !walker.isEnd() && walker.safeGet(1).value == ">") {
            walker.Next(); walker.Next(); // съедаем ->
            if (walker.CheckType(TokenTypes::KEYWORD) && walker.safeGet().value == "ref") {
                returns_ref = true;
                walker.Next();
            }
            return_type = parseExpression();
            if (!return_type)
                reportError(SYNTAX_ERRORS::WAITING_TYPE_EXPRESSION(walker.safeGet()));
        }
        suppress_struct_literals = saved_suppress;

        // `fn f() -> T : { тело }` — `:` отделяет тело функции. Без него `{`
        // неоднозначен: у `-> Array[i64, 2] {…}` это может быть и список
        // элементов конструктора `Array[T, N]{…}`.
        if (walker.Check(TokenTypes::OPERATOR, ":"))
            walker.Next();
        else if (walker.CheckType(TokenTypes::BRACE_LEFT_CURVE))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ":"));

        Node* body = nullptr;
        if (is_extern) {
            if (walker.CheckType(TokenTypes::BRACE_LEFT_CURVE)) {
                // runtime-фолбак: extern fn f() -> T { <expr> }; — значение тела
                // используется интерпретатором, если C++-функция недоступна
                function_depth++;
                body = parseStatement();
                function_depth--;
                if (!body)
                    reportError(SYNTAX_ERRORS::EXPECTED_STATEMENT(walker.safeGet()));
                if (walker.Check(TokenTypes::OPERATOR, ";")) // необязательная ';' после '}'
                    walker.Next();
            } else if (!walker.Check(TokenTypes::OPERATOR, ";"))
                reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
            else
                walker.Next();
        } else {
            function_depth++;
            body = parseStatement();
            function_depth--;
            if (!body)
                reportError(SYNTAX_ERRORS::EXPECTED_STATEMENT(walker.safeGet()));
        }

        if (is_extern && returns_ref) {
            reportError(SYNTAX_ERRORS::WAITING(nameTok, "extern functions cannot return references ('-> ref T')"));
        }

        auto* fn = new struct NodeFunctionDeclaration(name, generics, params, return_type, body, nameTok, genOpenTok, genCloseTok, argsOpenTok, argsCloseTok);
        fn->returns_ref = returns_ref;
        fn->is_extern = is_extern;
        fn->doc_comment = docCommentAbove(fnTok.position);
        return fn;
    }

    bool isModifierKeyword(const String& v) const {
        for (auto& m : Literals::MODIFIERS)
            if (m == v) return true;
        return false;
    }

    // Документирующий комментарий над объявлением: в сыром потоке source_tokens
    // (до Preprocessor::filterComments) ищем токен декларации и идём вверх.
    // Засчитываются только «стоящие отдельно» многострочные комментарии /* ... */:
    // на нижней строке комментария перед ним не должно быть кода (`} /* tail */` — НЕ док).
    String docCommentAbove(const PositionInFile& decl_pos) noexcept {
        size_t di = source_tokens.size();
        for (size_t i = 0; i < source_tokens.size(); ++i) {
            const Token& t = source_tokens[i];
            if (t.position.file_path == decl_pos.file_path &&
                t.position.line == decl_pos.line && t.position.index == decl_pos.index) {
                di = i; break;
            }
        }
        if (di >= source_tokens.size()) return String("");

        // пропускаем модификаторы перед декларацией (`static fn f`, `private fn f`)
        size_t s = di;
        while (s > 0 && source_tokens[s - 1].type == TypeOfToken::KEYWORD &&
               isModifierKeyword(source_tokens[s - 1].value))
            s--;

        std::vector<String> parts; // снизу вверх
        size_t c = s;
        while (c > 0) {
            const Token& t = source_tokens[c - 1];
            if (t.type != TypeOfToken::MULTI_COMMENT) break;
            if (c >= 2 && source_tokens[c - 2].position.line == t.position.line) break;
            parts.push_back(t.value);
            c--;
        }
        if (parts.empty()) return String("");

        String doc;
        for (size_t i = parts.size(); i-- > 0;) {
            doc += parts[i];
            if (i != 0) doc += "\n";
        }
        return doc;
    }

    bool isModifierTarget(Node* s) const {
        if (!s) return false;
        switch (s->type) {
            case NodeTypes::NodeModifier:
            case NodeTypes::NodeVariableDeclaration:
            case NodeTypes::NodeStatementsScope:
            case NodeTypes::NodeFunctionDeclaration:
            case NodeTypes::NodeStructDeclaration:
                return true;
            default:
                return false;
        }
    }

    Node* parseModifier() {
        Token m = walker.safeGet();
        walker.Next();

        Node* stmt = parseStatement();
        if (!stmt)
            reportError(SYNTAX_ERRORS::EXPECTED_STATEMENT_DECLARATION(walker.safeGet()));


        return new struct NodeModifier(m.value, stmt, m);   // цепочка: parseModifier -> parseStatement(modifiers)
    }

    Node* parseIf() {
        Token kw = walker.safeGet(); // токен 'if'
        walker.Next(); // съедаем if

        if (!walker.CheckType(TokenTypes::BRACE_LEFT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "("));
        walker.Next();

        Node* condition = parseExpression();
        if (!condition)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));

        if (!walker.CheckType(TokenTypes::BRACE_RIGHT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ")"));
        walker.Next();

        Node* then_body = parseStatement();
        if (!then_body)
            reportError(SYNTAX_ERRORS::EXPECTED_STATEMENT(walker.safeGet()));

        Node* else_body = nullptr;
        if (walker.Check(TokenTypes::KEYWORD, "else")) {
            walker.Next();
            else_body = parseStatement();
            if (!else_body)
                reportError(SYNTAX_ERRORS::EXPECTED_STATEMENT(walker.safeGet()));
        }

        return new struct NodeIf(condition, then_body, else_body, kw);
    }

    Node* parseWhile() {
        Token keyword = walker.safeGet(); // токен 'while'
        walker.Next(); // съедаем while

        if (!walker.CheckType(TokenTypes::BRACE_LEFT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "("));
        walker.Next();

        Node* condition = parseExpression();
        if (!condition)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));

        if (!walker.CheckType(TokenTypes::BRACE_RIGHT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ")"));
        walker.Next();

        Node* body = nullptr;
        loop_depth++;
        body = parseStatement();
        loop_depth--;
        if (!body)
            reportError(SYNTAX_ERRORS::EXPECTED_STATEMENT(walker.safeGet()));

        return new struct NodeWhile(condition, body, keyword);
    }

    Node* parseLambda() {
        walker.Next(); // съедаем lambda

        if (!walker.CheckType(TokenTypes::BRACE_LEFT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "("));
        walker.Next();

        std::vector<FuncArg> params;
        while (!walker.CheckType(TokenTypes::BRACE_RIGHT) && !walker.isEnd()) {
            params.push_back(parseFunctionParameter());
            if (walker.Check(TokenTypes::OPERATOR, ","))
                walker.Next();
            else
                break;
        }
        if (!walker.CheckType(TokenTypes::BRACE_RIGHT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ")"));
        else
            walker.Next();

        Node* return_type = nullptr;
        bool saved_suppress = suppress_struct_literals;
        suppress_struct_literals = true;   // `-> T { тело }`: `{` — тело лямбды
        if (walker.Check(TokenTypes::OPERATOR, "-") && !walker.isEnd() && walker.safeGet(1).value == ">") {
            walker.Next(); walker.Next(); // съедаем ->
            return_type = parseExpression();
            if (!return_type)
                reportError(SYNTAX_ERRORS::WAITING_TYPE_EXPRESSION(walker.safeGet()));
        } else {
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "->"));
        }
        suppress_struct_literals = saved_suppress;

        // `lambda(...) -> T : { выражение }` — `:` обязателен, как и у `fn`.
        if (walker.Check(TokenTypes::OPERATOR, ":"))
            walker.Next();
        else
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ":"));

        if (!walker.CheckType(TokenTypes::BRACE_LEFT_CURVE))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "{"));
        walker.Next(); // съедаем {

        Node* body = parseExpression();
        if (!body)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));

        if (!walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "}"));
        else
            walker.Next();

        return new struct NodeLambda(params, return_type, body);
    }

    Node* parseNew() {
        Token new_token = walker.safeGet();
        walker.Next(); // съедаем new

        std::vector<NewModifier> modifiers;
        if (walker.Check(TokenTypes::OPERATOR, "<")) {
            walker.Next(); // съедаем <
            while (!walker.Check(TokenTypes::OPERATOR, ">") && !walker.isEnd()) {
                if (!walker.CheckType(TokenTypes::LITERAL) && !walker.CheckType(TokenTypes::KEYWORD)) {
                    reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "modifier name"));
                    break;
                }
                String mname = walker.safeGet().value;
                walker.Next();

                Node* type_arg = nullptr;
                if (walker.CheckType(TokenTypes::BRACE_LEFT)) {
                    walker.Next(); // съедаем (
                    if (walker.CheckType(TokenTypes::BRACE_RIGHT))
                        reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
                    else {
                        type_arg = parseExpression();
                        if (!type_arg)
                            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
                    }
                    if (!walker.CheckType(TokenTypes::BRACE_RIGHT))
                        reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ")"));
                    else
                        walker.Next();
                }
                modifiers.push_back(NewModifier(mname, type_arg));

                if (walker.Check(TokenTypes::OPERATOR, ","))
                    walker.Next();
                else
                    break;
            }
            if (!walker.Check(TokenTypes::OPERATOR, ">"))
                reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ">"));
            else
                walker.Next();
        }

        Node* value = nullptr;
        if (!walker.Check(TokenTypes::OPERATOR, ";")) {
            value = parseExpression();
            if (!value)
                reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
        }

        return new struct NodeNew(modifiers, value, new_token);
    }

    Node* parseTypeOf(bool is_static = false) {
        Token kw = walker.safeGet(); // токен typeof/typeis
        walker.Next(); // съедаем typeof/typeis

        if (!walker.CheckType(TokenTypes::BRACE_LEFT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "("));
        else
            walker.Next();

        Node* expr = parseExpression();
        if (!expr)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));

        if (!walker.CheckType(TokenTypes::BRACE_RIGHT))
            reportError(SYNTAX_ERRORS::MISSING_CLOSING(walker.safeGet(), "(", ")"));
        else
            walker.Next();

        Node* n = new struct NodeTypeOf(expr, kw);
        static_cast<struct NodeTypeOf*>(n)->is_static = is_static;
        return n;
    }

    Node* parseSizeOf() {
        Token kw = walker.safeGet(); // токен sizeof
        walker.Next(); // съедаем sizeof

        if (!walker.CheckType(TokenTypes::BRACE_LEFT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "("));
        else
            walker.Next();

        Node* expr = parseExpression();
        if (!expr)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));

        if (!walker.CheckType(TokenTypes::BRACE_RIGHT))
            reportError(SYNTAX_ERRORS::MISSING_CLOSING(walker.safeGet(), "(", ")"));
        else
            walker.Next();

        return new struct NodeSizeOf(expr, kw);
    }

    Node* parseMod() {
        Token kw = walker.safeGet(); // токен mod
        walker.Next(); // съедаем mod

        if (!walker.CheckType(TokenTypes::BRACE_LEFT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "("));
        else
            walker.Next();

        Node* expr = parseExpression();
        if (!expr)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));

        if (!walker.Check(TokenTypes::OPERATOR, ","))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ","));
        else
            walker.Next();

        Token mt = walker.safeGet();
        String modifier = mt.value;
        if (!inVector(Literals::MODIFIERS, modifier)) {
            reportError(SYNTAX_ERRORS::WAITING(mt,
                "modifier (static, final, const, strict, private, ref, public)"));
        }
        walker.Next();

        if (!walker.CheckType(TokenTypes::BRACE_RIGHT))
            reportError(SYNTAX_ERRORS::MISSING_CLOSING(walker.safeGet(), "(", ")"));
        else
            walker.Next();

        return new struct NodeMod(expr, modifier, kw);
    }

    Node* parseInput() {
        Token token_start = walker.safeGet(); // input
        walker.Next();

        Node* prompt = nullptr; // подсказка по умолчанию отсутствует

        if (!walker.CheckType(TokenTypes::BRACE_LEFT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "("));
        else
            walker.Next();

        Node* prompt_expr = nullptr;
        if (!walker.CheckType(TokenTypes::BRACE_RIGHT)) {
            prompt_expr = parseExpression();
            if (!prompt_expr)
                reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
        }

        if (!walker.CheckType(TokenTypes::BRACE_RIGHT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ")"));
        else
            walker.Next();

        Token token_end = walker.safeGet(-1);

        return new struct NodeInput(prompt_expr, token_start, token_end);
    }

    Node* parsePtr() {
        Token kw = walker.safeGet(); // токен ptr
        walker.Next(); // съедаем ptr

        if (!walker.CheckType(TokenTypes::BRACE_LEFT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "("));
        else
            walker.Next();

        Node* addr = parseExpression();
        if (!addr)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));

        if (!walker.Check(TokenTypes::OPERATOR, ","))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ","));
        else
            walker.Next();

        Node* type = parseExpression();
        if (!type)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));

        if (!walker.CheckType(TokenTypes::BRACE_RIGHT))
            reportError(SYNTAX_ERRORS::MISSING_CLOSING(walker.safeGet(), "(", ")"));
        else
            walker.Next();

        return new struct NodePtr(addr, type, kw);
    }

    // for-each: `for (<decl>; of <expr>;) { ... }`, где <decl> — `let x` / `let x: T`
    // / `ref let x` / `ref let x: T`. Форма распознаётся превентивно: первый ';' на
    // глубине круглых скобок 0, сразу за которым идёт ключевое слово 'of'.
    bool isForEachHeader() {
        int depth = 0;
        for (size_t i = walker.index; i < walker.tokens.size(); i++) {
            const Token& t = walker.tokens[i];
            if (t.type == TokenTypes::BRACE_LEFT || t.type == TokenTypes::BRACE_LEFT_RECT)
                depth++;
            else if (t.type == TokenTypes::BRACE_RIGHT || t.type == TokenTypes::BRACE_RIGHT_RECT) {
                if (depth == 0) return false;   // закрылась ')' до 'of' — это C-цикл
                depth--;
            } else if (t.type == TokenTypes::OPERATOR && t.value == ";" && depth == 0) {
                return (i + 1 < walker.tokens.size())
                    && walker.tokens[i + 1].type == TokenTypes::KEYWORD
                    && walker.tokens[i + 1].value == "of";
            } else if (t.type == TokenTypes::BRACE_LEFT_CURVE && depth == 0) {
                return false;                    // тело цикла началось — C-цикл
            }
        }
        return false;
    }

    // Разбор <decl> в заголовке for-each. Не требует завершающего ';' и допускает
    // тип без значения (`let x: T`). Возвращает NodeVariableDeclaration.
    Node* parseForEachDecl(bool& by_ref) {
        by_ref = false;
        if (walker.CheckType(TokenTypes::KEYWORD) && walker.safeGet().value == "ref") {
            by_ref = true;
            walker.Next(); // 'ref'
        }
        if (!walker.Check(TokenTypes::KEYWORD, "let")) {
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "let"));
            return nullptr;
        }
        walker.Next(); // 'let'

        if (!walker.CheckType(TokenTypes::LITERAL)) {
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "variable name"));
            return nullptr;
        }
        String name = walker.safeGet().value;
        Token nameTok = walker.safeGet();
        walker.Next();

        Node* type_node = nullptr;
        if (walker.Check(TokenTypes::OPERATOR, ":")) {
            walker.Next();
            type_node = parseExpression();
            if (!type_node)
                reportError(SYNTAX_ERRORS::WAITING_TYPE_EXPRESSION(walker.safeGet()));
        }

        return new struct NodeVariableDeclaration(name, nullptr, type_node, nameTok);
    }

    Node* parseForEach(Token kw) {
        // walker уже после '('. Заголовок: <decl> ';' 'of' <expr> ';' ')'
        bool by_ref = false;
        Node* decl = parseForEachDecl(by_ref);
        if (!decl) {
            walker.passStatement();
            return nullptr;
        }
        if (!walker.Check(TokenTypes::OPERATOR, ";"))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
        else
            walker.Next();

        Token ofTok = walker.safeGet();
        if (!walker.Check(TokenTypes::KEYWORD, "of"))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "of"));
        else
            walker.Next();

        Node* source = parseExpression();
        if (!source)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));

        if (!walker.Check(TokenTypes::OPERATOR, ";"))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
        else
            walker.Next();

        if (!walker.CheckType(TokenTypes::BRACE_RIGHT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ")"));
        else
            walker.Next();

        Node* body = nullptr;
        loop_depth++;
        body = parseStatement();
        loop_depth--;
        return new struct NodeForEach(static_cast<struct NodeVariableDeclaration*>(decl),
                                      source, body, by_ref, kw, ofTok);
    }

    Node* parseFor() {
        Token kw = walker.safeGet(); // токен 'for'
        walker.Next(); // съедаем for
        if (!walker.CheckType(TokenTypes::BRACE_LEFT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), "("));
        else
            walker.Next();

        if (isForEachHeader())
            return parseForEach(kw);

        Node* init = parseStatement();
        if (!init)
            reportError(SYNTAX_ERRORS::EXPECTED_STATEMENT(walker.safeGet()));

        Node* check = nullptr;
        if (walker.Check(TokenTypes::OPERATOR, ";")) {
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
            walker.Next(); // съедаем ; для восстановления
        } else {
            check = parseExpression();
            if (!check)
                reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
            if (!walker.Check(TokenTypes::OPERATOR, ";"))
                reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
            else
                walker.Next();
        }

        Node* update = parseStatement();
        if (!update) {
            reportError(SYNTAX_ERRORS::EXPECTED_STATEMENT(walker.safeGet()));
            if (walker.Check(TokenTypes::OPERATOR, ";"))
                walker.Next(); // восстановление при пустом update
        }

        if (!walker.CheckType(TokenTypes::BRACE_RIGHT))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ")"));
        else
            walker.Next();

        Node* body = nullptr;
        loop_depth++;
        body = parseStatement();
        loop_depth--;
        return new struct NodeFor(init, check, update, body, kw);
    }

    Node* parseBreak() {
        auto token = walker.safeGet();
        walker.Next(); // съедаем break
        if (loop_depth == 0)
            reportError(SYNTAX_ERRORS::BREAK_OUTSIDE_LOOP(token));
        if (!walker.Check(TokenTypes::OPERATOR, ";"))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
        else
            walker.Next();
        return new struct NodeBreak();
    }

    Node* parseContinue() {
        auto token = walker.safeGet();
        walker.Next(); // съедаем continue
        if (loop_depth == 0)
            reportError(SYNTAX_ERRORS::CONTINUE_OUTSIDE_LOOP(token));
        if (!walker.Check(TokenTypes::OPERATOR, ";"))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
        else
            walker.Next();
        return new struct NodeContinue();
    }

    Node* parseReturn() {
        auto token = walker.safeGet();
        walker.Next(); // съедаем return
        if (function_depth == 0)
            reportError(SYNTAX_ERRORS::RETURN_OUTSIDE_FUNCTION(token));
        Node* expr = nullptr;
        if (!walker.isEnd() && !walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE)
            && !walker.Check(TokenTypes::OPERATOR, ";")) {
            expr = parseExpression();
            if (!expr)
                reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
        }
        if (!walker.Check(TokenTypes::OPERATOR, ";"))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
        else
            walker.Next();
        return new struct NodeReturn(token, expr);
    }
    Node* parseExit() {
        auto token = walker.safeGet();
        walker.Next(); // съедаем exit
        // строгая форма: `exit <expr>;` — и выражение кода, и «;» обязательны
        Node* expr = parseExpression();
        if (!expr)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
        if (!walker.Check(TokenTypes::OPERATOR, ";"))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
        else
            walker.Next();
        return new struct NodeExit(token, expr);
    }
    Node* parseAppend() {
        auto token = walker.safeGet();
        walker.Next(); // съедаем append
        // строгая форма: `append <array expr>, <value expr>;` — оба выражения и «;» обязательны
        Node* array_expr = parseExpression();
        if (!array_expr)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
        if (!walker.Check(TokenTypes::OPERATOR, ","))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ","));
        else
            walker.Next();
        Node* value_expr = parseExpression();
        if (!value_expr)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
        if (!walker.Check(TokenTypes::OPERATOR, ";"))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
        else
            walker.Next();
        return new struct NodeAppend(token, array_expr, value_expr);
    }
    Node* parseReserve() {
        auto token = walker.safeGet();
        walker.Next(); // съедаем reserve
        // строгая форма: `reserve <array expr>, <count expr>;` — оба выражения и «;» обязательны
        Node* array_expr = parseExpression();
        if (!array_expr)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
        if (!walker.Check(TokenTypes::OPERATOR, ","))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ","));
        else
            walker.Next();
        Node* count_expr = parseExpression();
        if (!count_expr)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
        if (!walker.Check(TokenTypes::OPERATOR, ";"))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
        else
            walker.Next();
        return new struct NodeReserve(token, array_expr, count_expr);
    }
    Node* parseEcho() {
        Token kw = walker.safeGet(); // токен echo
        Token end = walker.safeGet();
        walker.Next(); // съедаем echo


        auto expr = parseExpression();
        if (!expr)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));

        if (!walker.Check(TokenTypes::OPERATOR, ";"))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
        else {
            end = walker.safeGet();
            walker.Next();
        }
        return new struct NodeEcho(expr, kw, end);
    }

    Node* parseOutput(bool newline) {
        Token kw = walker.safeGet(); // токен out / outln
        walker.Next(); // съедаем out / outln

        // out / outln принимают аргументы только через запятую:  out x, y;   out;
        // Скобки после out/outln принадлежат выражению, а не списку аргументов:
        //   outln (i32 | char) << i32;   — скобки группируют union, а << — проверка подтипа.
        // Форма out(x, y) больше не разбирается (иначе она съедает скобки выражения).
        std::vector<Node*> exprs;
        if (!walker.Check(TokenTypes::OPERATOR, ";")) {   // out;  (без аргументов)
            exprs.push_back(parseExpression());
            while (walker.Check(TokenTypes::OPERATOR, ",")) {
                walker.Next();
                exprs.push_back(parseExpression());
            }
        }

        if (!walker.Check(TokenTypes::OPERATOR, ";"))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
        else
            walker.Next();

        return newline
            ? (Node*) new struct NodeOutln(exprs, kw)
            : (Node*) new struct NodeOut(exprs, kw);
    }

    Node* parseAssert() {
        Token kw = walker.safeGet(); // токен assert
        walker.Next(); // съедаем assert

        // assert cond, msg;   assert(cond, msg);
        bool has_parens = walker.CheckType(TokenTypes::BRACE_LEFT);
        if (has_parens) walker.Next();

        Node* check = parseExpression();
        if (!check)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));

        Node* message = nullptr;
        if (walker.Check(TokenTypes::OPERATOR, ",")) {
            walker.Next();
            message = parseExpression();
            if (!message)
                reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
        }

        if (has_parens) {
            if (!walker.CheckType(TokenTypes::BRACE_RIGHT))
                reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ")"));
            else
                walker.Next();
        }

        if (!walker.Check(TokenTypes::OPERATOR, ";"))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
        else
            walker.Next();

        return new struct NodeAssert(kw, check, message);
    }

    Node* parseDel() {
        Token kw = walker.safeGet(); // токен del
        walker.Next(); // съедаем del

        Node* expr = parseExpression();
        if (!expr)
            reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));

        // delete host, index; — удаление элемента из массива/кортежа
        Node* index = nullptr;
        if (walker.Check(TokenTypes::OPERATOR, ",")) {
            walker.Next();
            index = parseExpression();
            if (!index)
                reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
        }

        if (!walker.Check(TokenTypes::OPERATOR, ";"))
            reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
        else
            walker.Next();

        return new struct NodeDelete(expr, index, kw);
    }

    // Собирает бинарный узел для составного присваивания a op= b
    // (в RHS NodeBinaryAssignment кладём (lhs op rhs)).
    Node* makeCompoundBinop(const String& op, Node* l, Node* r, const Token& start, const Token& end) {
        if      (op == "+=") return new struct NodeBinaryAdditive(l, r, start, end);
        else if (op == "-=") return new struct NodeBinarySubtraction(l, r, start, end);
        else if (op == "*=") return new struct NodeBinaryMultiplicative(l, r, start, end);
        else if (op == "/=") return new struct NodeBinaryDivision(l, r, start, end);
        else if (op == "%=") return new struct NodeBinaryRemainder(l, r, start, end);
        else if (op == "&=") return new struct NodeBinaryBitwiseAnd(l, r, start, end);
        else if (op == "|=") return new struct NodeBinaryBitwiseOr(l, r, start, end);
        else if (op == "^=") return new struct NodeBinaryBitwiseXor(l, r, start, end);
        else if (op == "<<=") return new struct NodeBinaryShiftLeft(l, r, start, end);
        else if (op == ">>=") return new struct NodeBinaryShiftRight(l, r, start, end);
        return r;
    }

    // Завершение statement'а: ';' (обычный случай). Перед закрывающей '}' или
    // концом файла ';' можно опустить — это хвостовое выражение блока,
    // например ветки if-выражения: `if (c) { 0 } else { 1 }`.
    bool consumeStatementEnd() {
        if (walker.Check(TokenTypes::OPERATOR, ";")) {
            walker.Next();
            return true;
        }
        if (walker.CheckType(TokenTypes::BRACE_RIGHT_CURVE) || walker.isEnd())
            return true;
        reportError(SYNTAX_ERRORS::WAITING(walker.safeGet(), ";"));
        return false;
    }

    // Обёртка ведёт счётчик глубины стейтментов (см. statement_depth): верхний
    // уровень файла — 1. Используется правилом размещения `eval` (top-level).
    Node* parseStatement() {
        ++statement_depth;
        Node* result = parseStatementInner();
        --statement_depth;
        return result;
    }

    Node* parseStatementInner() {
        if (walker.CheckType(TokenTypes::KEYWORD) && isModifierKeyword(walker.safeGet().value)) {
            return parseModifier();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "let")) {
            return parseVariableDeclaration();
        }
        else if (walker.CheckType(TokenTypes::BRACE_LEFT_CURVE)) {
            return parseStatementScope();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "if")) {
            return parseIf();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "while")) {
            return parseWhile();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "for")) {
            return parseFor();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "eval")) {
            return parseEvalBlock();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "namespace")) {
            return parseNamespace();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "struct")) {
            return parseStruct();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "enum")) {
            return parseEnum();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "fn")) {
            return parseFunction();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "extern")) {
            return parseExternFunction();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "break")) {
            return parseBreak();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "continue")) {
            return parseContinue();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "ret")) {
            return parseReturn();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "exit")) {
            return parseExit();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "append")) {
            return parseAppend();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "reserve")) {
            return parseReserve();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "outln")) {
            return parseOutput(true);
        }
        else if (walker.Check(TokenTypes::KEYWORD, "echo")) {
            return parseEcho();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "out")) {
            return parseOutput(false);
        }
        else if (walker.Check(TokenTypes::KEYWORD, "del")) {
            return parseDel();
        }
        else if (walker.Check(TokenTypes::KEYWORD, "assert")) {
            return parseAssert();
        }
        Token start_token = walker.safeGet();

        Node* lhs = parseExpression();
        String cmpOp = multiCharOperator();
        if (lhs && (cmpOp == "+=" || cmpOp == "-=" || cmpOp == "*=" || cmpOp == "/=" ||
                    cmpOp == "%=" || cmpOp == "&=" || cmpOp == "|=" || cmpOp == "^=" ||
                    cmpOp == "<<=" || cmpOp == ">>=")) {
            // составное присваивание: a op= b  <=>  a = (a op b)
            walker.Next(); walker.Next();   // съедаем первые два символа оператора
            if (cmpOp == "<<=" || cmpOp == ">>=") walker.Next();   // третий символ '='
            Node* rhs = parseExpression();
            if (!rhs)
                reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
            Node* binop = makeCompoundBinop(cmpOp, lhs, rhs, start_token, walker.safeGet(-1));
            Token assign_end = walker.safeGet(-1);
            consumeStatementEnd();
            return new struct NodeBinaryAssignment(lhs, binop, start_token, assign_end);
        }
        if (lhs && walker.Check(TokenTypes::OPERATOR, "=") && walker.safeGet(1).value != "=") {
            walker.Next(); // съедаем =
            Node* rhs = parseExpression();
            if (!rhs)
                reportError(SYNTAX_ERRORS::EXPECTED_EXPRESSION(walker.safeGet()));
            Token assign_end = walker.safeGet(-1); // последний токен RHS, до ';'
            consumeStatementEnd();

            return new struct NodeBinaryAssignment(lhs, rhs, start_token, assign_end);
        }
        if (lhs) {
            consumeStatementEnd();
        }
        return lhs;
    }

    // eval { ... } | eval <стейтмент> — вычисление на этапе анализа. Разрешён
    // только на верхнем уровне файла (не внутри функции/цикла/ветки/namespace/блока).
    Node* parseEvalBlock() {
        Token evalTok = walker.safeGet();
        walker.Next(); // съедаем eval

        // Тело — либо блок `{ ... }`, либо одиночный стейтмент.
        Node* body;
        if (walker.CheckType(TokenTypes::BRACE_LEFT_CURVE))
            body = parseStatementScope();
        else
            body = parseStatement();

        Token endTok = walker.safeGet(-1);   // закрывающая '}' блока или последний токен
        Node* node = new struct NodeEvalBlock(evalTok, endTok, body);

        if (statement_depth > 1)
            reportError(DEEP_ERRORS::EVAL_NOT_TOP_LEVEL(evalTok));

        return node;
    }

    Node* parse() {
        return parseStatement();
    }

    AbstractTreeT operator()() {
        while (!walker.isEnd()) {
            size_t index_before = walker.index;

            auto node = parse();
            if (node) {
                nodes.push_back(node);
            }
            if (walker.index == index_before) {
                reportError(SYNTAX_ERRORS::EXPECTED_STATEMENT(walker.safeGet()));
                walker.Next();
            }
        }
        return nodes;
    }
};


void printPrefix(const std::vector<bool>& levelsStatus, int currentLevel, bool isCurrentLast) {
    if (currentLevel == 0) return;

    // Проходим по всем уровням выше текущего
    for (int i = 1; i < currentLevel; ++i) {
        // Если узел на уровне i НЕ был последним (значит, ветка продолжается), рисуем вертикальную линию
        if (!levelsStatus[i]) {
            std::cout << Style("│   ", TerminalStyle::DIM_WHITE);
        } else {
            // Иначе рисуем пробелы, чтобы выровнять структуру
            std::cout << "    ";
        }
    }

    // Печатаем сам разделитель для текущего узла
    if (isCurrentLast) {
        std::cout << Style("└── ", TerminalStyle::DIM_WHITE);
    } else {
        std::cout << Style("├── ", TerminalStyle::DIM_WHITE);
    }
}

void printNode(Node* node, int level, const std::vector<bool>& levelsStatus, bool isCurrentLast) {
    if (!node) {
        printPrefix(levelsStatus, level, isCurrentLast);
        std::cout << Style("[nullptr]", TerminalStyle::DIM_WHITE) << "\n";
        return;
    }

    printPrefix(levelsStatus, level, isCurrentLast);

    switch (node->type) {
        case NodeTypes::NodeVariableDeclaration: {
            auto n = static_cast<struct NodeVariableDeclaration*>(node);
            std::cout << Style("NodeVariableDeclaration (" + n->name + ")", TerminalStyle::YELLOW) << "\n";

            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->type_node, level + 1, newStatus, false);
            printNode(n->value_node, level + 1, newStatus, true);

            return;
        }
        case NodeTypes::NodeVariable: {
            auto n = static_cast<struct NodeVariable*>(node);
            std::cout << Style("NodeVariable (" + n->name + ")", TerminalStyle::GREEN) << "\n";
            return;
        }
        case NodeTypes::NodeNumber: {
            auto n = static_cast<struct NodeNumber*>(node);
            String numLabel = String("NodeNumber (") +
                (n->is_integer ? "integer" : "float") + "): " +
                std::to_string(n->is_integer ? n->int_value : n->float_value);
            std::cout << Style(numLabel, TerminalStyle::MAGENTA) << "\n";
            return;
        }
        case NodeTypes::NodeBool: {
            auto n = static_cast<struct NodeBool*>(node);
            std::cout << Style("NodeBool (" + String(n->value ? "true" : "false") + ")", TerminalStyle::MAGENTA) << "\n";
            return;
        }
        case NodeTypes::NodeNull: {
            std::cout << Style("NodeNull ()", TerminalStyle::MAGENTA) << "\n";
            return;
        }
        case NodeTypes::NodeString: {
            auto n = static_cast<struct NodeString*>(node);
            String q = n->is_char ? String("'") : String("\"");
            std::cout << Style("NodeString (" + q + n->value + q + ")", TerminalStyle::MAGENTA) << "\n";
            return;
        }
        case NodeTypes::NodeScope: {
            auto n = static_cast<struct NodeScope*>(node);
            std::cout << Style("NodeScope ()", TerminalStyle::CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast); // Статус родителя важен для детей
            printNode(n->node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeStatementsScope: {
            auto n = static_cast<struct NodeStatementsScope*>(node);
            std::cout << Style("NodeStatementsScope {}", TerminalStyle::CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            for (size_t i = 0; i < n->nodes.size(); ++i) {
                bool last = (i == n->nodes.size() - 1);
                printNode(n->nodes[i], level + 1, newStatus, last);
            }
            return;
        }
        case NodeTypes::NodeNamespace: {
            auto n = static_cast<struct NodeNamespace*>(node);
            String label = n->name.empty() ? "NodeNamespace (anonymous)" : "NodeNamespace (" + n->name + ")";
            std::cout << Style(label, TerminalStyle::CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->body, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeStructDeclaration: {
            auto n = static_cast<struct NodeStructDeclaration*>(node);
            std::cout << Style("NodeStructDeclaration (" + n->name + ")", TerminalStyle::CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->body, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeEnumDeclaration: {
            auto n = static_cast<struct NodeEnumDeclaration*>(node);
            std::cout << Style("NodeEnumDeclaration (" + n->name + ")", TerminalStyle::CYAN) << "\n";
            return;
        }
        case NodeTypes::NodeIf: {
            auto n = static_cast<struct NodeIf*>(node);
            std::cout << Style("NodeIf", TerminalStyle::MAGENTA) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->condition, level + 1, newStatus, false);
            printNode(n->then_body, level + 1, newStatus, !n->else_body);
            if (n->else_body) {
                printPrefix(newStatus, level + 1, true);
                std::cout << Style("else", TerminalStyle::DIM) << "\n";
                std::vector<bool> elseStatus = newStatus;
                elseStatus.push_back(true);
                printNode(n->else_body, level + 2, elseStatus, true);
            }
            return;
        }
        case NodeTypes::NodeWhile: {
            auto n = static_cast<struct NodeWhile*>(node);
            std::cout << Style("NodeWhile", TerminalStyle::MAGENTA) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->condition, level + 1, newStatus, false);
            printNode(n->body, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeFor: {
            auto n = static_cast<struct NodeFor*>(node);
            std::cout << Style("NodeFor", TerminalStyle::MAGENTA) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            size_t childCount = (n->init ? 1 : 0) + (n->check ? 1 : 0) + (n->update ? 1 : 0) + 1;
            size_t idx = 0;
            if (n->init) {
                printNode(n->init, level + 1, newStatus, idx + 1 == childCount);
            } else {
                printPrefix(newStatus, level + 1, idx + 1 == childCount);
                std::cout << Style("[nullptr]", TerminalStyle::DIM_WHITE) << "\n";
            }
            idx++;
            if (n->check) {
                printNode(n->check, level + 1, newStatus, idx + 1 == childCount);
            } else {
                printPrefix(newStatus, level + 1, idx + 1 == childCount);
                std::cout << Style("[nullptr]", TerminalStyle::DIM_WHITE) << "\n";
            }
            idx++;
            if (n->update) {
                printNode(n->update, level + 1, newStatus, idx + 1 == childCount);
            } else {
                printPrefix(newStatus, level + 1, idx + 1 == childCount);
                std::cout << Style("[nullptr]", TerminalStyle::DIM_WHITE) << "\n";
            }
            idx++;
            printNode(n->body, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeForEach: {
            auto n = static_cast<struct NodeForEach*>(node);
            std::cout << Style("NodeForEach" + String(n->by_ref ? " (ref)" : ""), TerminalStyle::MAGENTA) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->decl, level + 1, newStatus, false);
            printNode(n->source, level + 1, newStatus, false);
            printNode(n->body, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBreak: {
            std::cout << Style("NodeBreak", TerminalStyle::MAGENTA) << "\n";
            return;
        }
        case NodeTypes::NodeContinue: {
            std::cout << Style("NodeContinue", TerminalStyle::MAGENTA) << "\n";
            return;
        }
        case NodeTypes::NodeReturn: {
            auto n = static_cast<struct NodeReturn*>(node);
            std::cout << Style("NodeReturn", TerminalStyle::MAGENTA) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            if (n->expr)
                printNode(n->expr, level + 1, newStatus, true);
            else {
                printPrefix(newStatus, level + 1, true);
                std::cout << Style("[nullptr]", TerminalStyle::DIM_WHITE) << "\n";
            }
            return;
        }
        case NodeTypes::NodeExit: {
            auto n = static_cast<struct NodeExit*>(node);
            std::cout << Style("NodeExit (exit)", TerminalStyle::MAGENTA) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            if (n->expr)
                printNode(n->expr, level + 1, newStatus, true);
            else {
                printPrefix(newStatus, level + 1, true);
                std::cout << Style("[nullptr]", TerminalStyle::DIM_WHITE) << "\n";
            }
            return;
        }
        case NodeTypes::NodeAppend: {
            auto n = static_cast<struct NodeAppend*>(node);
            std::cout << Style("NodeAppend (append)", TerminalStyle::MAGENTA) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->array_expr, level + 1, newStatus, false);
            printNode(n->value_expr, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeReserve: {
            auto n = static_cast<struct NodeReserve*>(node);
            std::cout << Style("NodeReserve (reserve)", TerminalStyle::MAGENTA) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->array_expr, level + 1, newStatus, false);
            printNode(n->count_expr, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeOut: {
            auto n = static_cast<struct NodeOut*>(node);
            std::cout << Style("NodeOut", TerminalStyle::MAGENTA) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            for (size_t i = 0; i < n->exprs.size(); ++i)
                printNode(n->exprs[i], level + 1, newStatus, i + 1 == n->exprs.size());
            return;
        }
        case NodeTypes::NodeOutln: {
            auto n = static_cast<struct NodeOutln*>(node);
            std::cout << Style("NodeOutln", TerminalStyle::MAGENTA) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            for (size_t i = 0; i < n->exprs.size(); ++i)
                printNode(n->exprs[i], level + 1, newStatus, i + 1 == n->exprs.size());
            return;
        }
        case NodeTypes::NodeDelete: {
            auto n = static_cast<struct NodeDelete*>(node);
            std::cout << Style("NodeDelete", TerminalStyle::MAGENTA) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->expr, level + 1, newStatus, n->index == nullptr);
            if (n->index != nullptr)
                printNode(n->index, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeAssert: {
            auto n = static_cast<struct NodeAssert*>(node);
            std::cout << Style("NodeAssert", TerminalStyle::MAGENTA) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->check, level + 1, newStatus, n->message == nullptr);
            if (n->message != nullptr)
                printNode(n->message, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeModifier: {
            auto n = static_cast<struct NodeModifier*>(node);
            std::cout << Style("NodeModifier (" + n->name + ")", TerminalStyle::BRIGHT_YELLOW) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeUnaryPlus: {
            auto n = static_cast<struct NodeUnaryPlus*>(node);
            std::cout << Style("NodeUnaryPlus (+)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast); // Статус родителя важен для детей
            printNode(n->node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeUnaryMinus: {
            auto n = static_cast<struct NodeUnaryMinus*>(node);
            std::cout << Style("NodeUnaryMinus (-)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeUnaryNegation: {
            auto n = static_cast<struct NodeUnaryNegation*>(node);
            std::cout << Style("NodeUnaryNegation (!)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeUnaryDereference: {
            auto n = static_cast<struct NodeUnaryDereference*>(node);
            std::cout << Style("NodeUnaryDereference (*)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->node, level + 1, newStatus, true);
            return;
        }

        case NodeTypes::NodeBinaryAdditive: {
            auto n = static_cast<struct NodeBinaryAdditive*>(node);
            std::cout << Style("NodeBinaryAdditive (+)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinarySubtraction: {
            auto n = static_cast<struct NodeBinarySubtraction*>(node);
            std::cout << Style("NodeBinarySubtraction (-)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryMultiplicative: {
            auto n = static_cast<struct NodeBinaryMultiplicative*>(node);
            std::cout << Style("NodeBinaryMultiplicative (*)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryDivision: {
            auto n = static_cast<struct NodeBinaryDivision*>(node);
            std::cout << Style("NodeBinaryDivision (/)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }

        case NodeTypes::NodeBinaryRemainder: {
            auto n = static_cast<struct NodeBinaryRemainder*>(node);
            std::cout << Style("NodeBinaryRemainder (%)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryShiftLeft: {
            auto n = static_cast<struct NodeBinaryShiftLeft*>(node);
            std::cout << Style("NodeBinaryShiftLeft (<<)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryShiftRight: {
            auto n = static_cast<struct NodeBinaryShiftRight*>(node);
            std::cout << Style("NodeBinaryShiftRight (>>)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryEqual: {
            auto n = static_cast<struct NodeBinaryEqual*>(node);
            std::cout << Style("NodeBinaryEqual (==)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryNotEqual: {
            auto n = static_cast<struct NodeBinaryNotEqual*>(node);
            std::cout << Style("NodeBinaryNotEqual (!=)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryLess: {
            auto n = static_cast<struct NodeBinaryLess*>(node);
            std::cout << Style("NodeBinaryLess (<)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryGreater: {
            auto n = static_cast<struct NodeBinaryGreater*>(node);
            std::cout << Style("NodeBinaryGreater (>)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryLessEqual: {
            auto n = static_cast<struct NodeBinaryLessEqual*>(node);
            std::cout << Style("NodeBinaryLessEqual (<=)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryGreaterEqual: {
            auto n = static_cast<struct NodeBinaryGreaterEqual*>(node);
            std::cout << Style("NodeBinaryGreaterEqual (>=)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryBitwiseAnd: {
            auto n = static_cast<struct NodeBinaryBitwiseAnd*>(node);
            std::cout << Style("NodeBinaryBitwiseAnd (&)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryBitwiseXor: {
            auto n = static_cast<struct NodeBinaryBitwiseXor*>(node);
            std::cout << Style("NodeBinaryBitwiseXor (^)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryBitwiseOr: {
            auto n = static_cast<struct NodeBinaryBitwiseOr*>(node);
            std::cout << Style("NodeBinaryBitwiseOr (|)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryLogicalAnd: {
            auto n = static_cast<struct NodeBinaryLogicalAnd*>(node);
            std::cout << Style("NodeBinaryLogicalAnd (&&)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryLogicalOr: {
            auto n = static_cast<struct NodeBinaryLogicalOr*>(node);
            std::cout << Style("NodeBinaryLogicalOr (||)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }

        case NodeTypes::NodeBinaryNullCoalesce: {
            auto n = static_cast<struct NodeBinaryNullCoalesce*>(node);
            std::cout << Style("NodeBinaryNullCoalesce (??)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }

        case NodeTypes::NodeBinaryScopeResolution: {
            auto n = static_cast<struct NodeBinaryScopeResolution*>(node);
            std::cout << Style("NodeBinaryScopeResolution (::)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeBinaryAssignment: {
            auto n = static_cast<struct NodeBinaryAssignment*>(node);
            std::cout << Style("NodeBinaryAssignment (=)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeCast: {
            auto n = static_cast<struct NodeCast*>(node);
            std::cout << Style("NodeCast (as)", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeMemberAccess: {
            auto n = static_cast<struct NodeMemberAccess*>(node);
            std::cout << Style("NodeMemberAccess (.)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->l_node, level + 1, newStatus, false);
            printNode(n->r_node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeArrayAccess: {
            auto n = static_cast<struct NodeArrayAccess*>(node);
            std::cout << Style("NodeArrayAccess ([])", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->base, level + 1, newStatus, n->indices.empty());
            for (size_t i = 0; i < n->indices.size(); ++i)
                printNode(n->indices[i], level + 1, newStatus, i + 1 == n->indices.size());
            return;
        }
        case NodeTypes::NodeFunctionType: {
            auto n = static_cast<struct NodeFunctionType*>(node);
            std::cout << Style("NodeFunctionType", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            bool hasReturn = (n->return_type != nullptr);

            printPrefix(newStatus, level + 1, !hasReturn);
            std::cout << Style("FuncArgTypes", TerminalStyle::CYAN) << "\n";
            std::vector<bool> atStatus = newStatus;
            atStatus.push_back(!hasReturn);
            for (size_t i = 0; i < n->params.size(); ++i)
                printNode(n->params[i], level + 2, atStatus, i + 1 == n->params.size());

            printPrefix(newStatus, level + 1, true);
            std::cout << Style("FuncReturnType", TerminalStyle::CYAN) << "\n";
            std::vector<bool> rtStatus = newStatus;
            rtStatus.push_back(true);
            if (n->return_type)
                printNode(n->return_type, level + 2, rtStatus, true);
            else {
                printPrefix(rtStatus, level + 2, true);
                std::cout << Style("[nullptr]", TerminalStyle::DIM_WHITE) << "\n";
            }
            return;
        }
        case NodeTypes::NodeTupleType: {
            auto n = static_cast<struct NodeTupleType*>(node);
            std::cout << Style("NodeTupleType", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            for (size_t i = 0; i < n->types.size(); ++i)
                printNode(n->types[i], level + 1, newStatus, i + 1 == n->types.size());
            return;
        }
        case NodeTypes::NodeArrayType: {
            auto n = static_cast<struct NodeArrayType*>(node);
            std::cout << Style("NodeArrayType", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            bool hasCount = (n->count != nullptr);
            printNode(n->element_type, level + 1, newStatus, !hasCount);
            if (hasCount)
                printNode(n->count, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeArray: {
            auto n = static_cast<struct NodeArray*>(node);
            std::cout << Style("NodeArray", TerminalStyle::MAGENTA) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            for (size_t i = 0; i < n->elements.size(); ++i)
                printNode(n->elements[i], level + 1, newStatus, i + 1 == n->elements.size());
            return;
        }
        case NodeTypes::NodeFunctionCall: {
            auto n = static_cast<struct NodeFunctionCall*>(node);
            std::cout << Style("NodeFunctionCall", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->callee, level + 1, newStatus, n->generics.empty() && n->args.empty());
            if (!n->generics.empty()) {
                printPrefix(newStatus, level + 1, n->args.empty());
                std::cout << Style("Generics", TerminalStyle::CYAN) << "\n";
                std::vector<bool> gensStatus = newStatus;
                gensStatus.push_back(n->args.empty());
                for (size_t i = 0; i < n->generics.size(); ++i)
                    printNode(n->generics[i], level + 2, gensStatus, i + 1 == n->generics.size());
            }
            if (!n->args.empty()) {
                printPrefix(newStatus, level + 1, true);
                std::cout << Style("Args", TerminalStyle::CYAN) << "\n";
                std::vector<bool> argsStatus = newStatus;
                argsStatus.push_back(true);
                for (size_t i = 0; i < n->args.size(); ++i)
                    printNode(n->args[i], level + 2, argsStatus, i + 1 == n->args.size());
            }
            return;
        }
        case NodeTypes::NodeFunctionDeclaration: {
            auto n = static_cast<struct NodeFunctionDeclaration*>(node);
            std::cout << Style("NodeFunctionDeclaration (" + n->name + ")", TerminalStyle::YELLOW) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            size_t childCount = n->generics.size() + n->params.size() + 1; // FuncReturnType всегда
            if (n->body) childCount++;

            for (size_t i = 0; i < n->generics.size(); ++i) {
                bool isLast = (i == childCount - 1);
                printPrefix(newStatus, level + 1, isLast);
                String gmodStr;
                if (!n->generics[i].modifiers.empty()) {
                    gmodStr = " [";
                    for (size_t m = 0; m < n->generics[i].modifiers.size(); ++m) {
                        if (m) gmodStr += ", ";
                        gmodStr += n->generics[i].modifiers[m];
                    }
                    gmodStr += "]";
                }
                std::cout << Style("GenericArg (" + n->generics[i].name + ")" + gmodStr, TerminalStyle::YELLOW) << "\n";
                std::vector<bool> argStatus = newStatus;
                argStatus.push_back(isLast);
                if (n->generics[i].type)
                    printNode(n->generics[i].type, level + 2, argStatus, !n->generics[i].default_value);
                if (n->generics[i].default_value)
                    printNode(n->generics[i].default_value, level + 2, argStatus, true);
            }

            for (size_t i = 0; i < n->params.size(); ++i) {
                bool isLast = (n->generics.size() + i == childCount - 1);
                printPrefix(newStatus, level + 1, isLast);
                String modStr;
                if (!n->params[i].modifiers.empty()) {
                    modStr = " [";
                    for (size_t m = 0; m < n->params[i].modifiers.size(); ++m) {
                        if (m) modStr += ", ";
                        modStr += n->params[i].modifiers[m];
                    }
                    modStr += "]";
                }
                std::cout << Style("FuncArg (" + n->params[i].name + ")" + modStr, TerminalStyle::YELLOW) << "\n";
                std::vector<bool> argStatus = newStatus;
                argStatus.push_back(isLast);
                if (n->params[i].type)
                    printNode(n->params[i].type, level + 2, argStatus, !n->params[i].default_value);
                if (n->params[i].default_value)
                    printNode(n->params[i].default_value, level + 2, argStatus, true);
            }

            {
                bool isLast = (n->generics.size() + n->params.size() == childCount - 1);
                printPrefix(newStatus, level + 1, isLast);
                std::cout << Style(n->returns_ref ? "FuncReturnType (ref)" : "FuncReturnType", TerminalStyle::CYAN) << "\n";
                std::vector<bool> rtStatus = newStatus;
                rtStatus.push_back(isLast);
                if (n->return_type)
                    printNode(n->return_type, level + 2, rtStatus, true);
                else {
                    printPrefix(rtStatus, level + 2, true);
                    std::cout << Style("[nullptr]", TerminalStyle::DIM_WHITE) << "\n";
                }
            }
            if (n->body) {
                printPrefix(newStatus, level + 1, true);
                std::cout << Style("Body", TerminalStyle::CYAN) << "\n";
                std::vector<bool> bodyStatus = newStatus;
                bodyStatus.push_back(true);
                printNode(n->body, level + 2, bodyStatus, true);
            }
            return;
        }
        case NodeTypes::NodeLambda: {
            auto n = static_cast<struct NodeLambda*>(node);
            std::cout << Style("NodeLambda", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);

            for (size_t i = 0; i < n->params.size(); ++i) {
                printPrefix(newStatus, level + 1, false);
                String modStr;
                if (!n->params[i].modifiers.empty()) {
                    modStr = " [";
                    for (size_t m = 0; m < n->params[i].modifiers.size(); ++m) {
                        if (m) modStr += ", ";
                        modStr += n->params[i].modifiers[m];
                    }
                    modStr += "]";
                }
                std::cout << Style("FuncArg (" + n->params[i].name + ")" + modStr, TerminalStyle::YELLOW) << "\n";
                std::vector<bool> argStatus = newStatus;
                argStatus.push_back(false);
                if (n->params[i].type)
                    printNode(n->params[i].type, level + 2, argStatus, !n->params[i].default_value);
                if (n->params[i].default_value)
                    printNode(n->params[i].default_value, level + 2, argStatus, true);
            }

            printPrefix(newStatus, level + 1, !n->body);
            std::cout << Style("FuncReturnType", TerminalStyle::CYAN) << "\n";
            std::vector<bool> rtStatus = newStatus;
            rtStatus.push_back(!n->body);
            if (n->return_type)
                printNode(n->return_type, level + 2, rtStatus, true);
            else {
                printPrefix(rtStatus, level + 2, true);
                std::cout << Style("[nullptr]", TerminalStyle::DIM_WHITE) << "\n";
            }

            if (n->body) {
                printPrefix(newStatus, level + 1, true);
                std::cout << Style("Body", TerminalStyle::CYAN) << "\n";
                std::vector<bool> bodyStatus = newStatus;
                bodyStatus.push_back(true);
                printNode(n->body, level + 2, bodyStatus, true);
            }
            return;
        }
        case NodeTypes::NodeUnaryBitwiseNot: {
            auto n = static_cast<struct NodeUnaryBitwiseNot*>(node);
            std::cout << Style("NodeUnaryBitwiseNot (~)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeUnaryAddressOf: {
            auto n = static_cast<struct NodeUnaryAddressOf*>(node);
            std::cout << Style("NodeUnaryAddressOf (&)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeUnaryPostfixIncrement: {
            auto n = static_cast<struct NodeUnaryPostfixIncrement*>(node);
            std::cout << Style("NodeUnaryPostfixIncrement (++)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeUnaryPostfixDecrement: {
            auto n = static_cast<struct NodeUnaryPostfixDecrement*>(node);
            std::cout << Style("NodeUnaryPostfixDecrement (--)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodePostfixUnwrap: {
            auto n = static_cast<struct NodePostfixUnwrap*>(node);
            std::cout << Style("NodePostfixUnwrap (a?)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeUnaryPrefixIncrement: {
            auto n = static_cast<struct NodeUnaryPrefixIncrement*>(node);
            std::cout << Style("NodeUnaryPrefixIncrement (++i)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeUnaryPrefixDecrement: {
            auto n = static_cast<struct NodeUnaryPrefixDecrement*>(node);
            std::cout << Style("NodeUnaryPrefixDecrement (--i)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->node, level + 1, newStatus, true);
            return;
        }
        case NodeTypes::NodeNew: {
            auto n = static_cast<struct NodeNew*>(node);
            std::cout << Style("NodeNew", TerminalStyle::BLUE) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);

            for (size_t i = 0; i < n->modifiers.size(); ++i) {
                bool isLastMod = (n->value == nullptr && i == n->modifiers.size() - 1);
                printPrefix(newStatus, level + 1, isLastMod);
                std::cout << Style("Modifier (" + n->modifiers[i].name + ")", TerminalStyle::YELLOW) << "\n";
                if (n->modifiers[i].type_arg) {
                    std::vector<bool> mStatus = newStatus;
                    mStatus.push_back(isLastMod);
                    printNode(n->modifiers[i].type_arg, level + 2, mStatus, true);
                }
            }

            if (n->value) {
                printPrefix(newStatus, level + 1, true);
                std::cout << Style("Value", TerminalStyle::CYAN) << "\n";
                std::vector<bool> vStatus = newStatus;
                vStatus.push_back(true);
                printNode(n->value, level + 2, vStatus, true);
            } else {
                printPrefix(newStatus, level + 1, true);
                std::cout << Style("[nullptr]", TerminalStyle::DIM_WHITE) << "\n";
            }
            return;
        }

        case NodeTypes::NodeTypeOf: {
            auto n = static_cast<struct NodeTypeOf*>(node);
            std::cout << Style(n->is_static ? "NodeTypeOf (typeis)" : "NodeTypeOf (typeof)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->expr, level + 1, newStatus, true);
            return;
        }

        case NodeTypes::NodeSizeOf: {
            auto n = static_cast<struct NodeSizeOf*>(node);
            std::cout << Style("NodeSizeOf (sizeof)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->expr, level + 1, newStatus, true);
            return;
        }

        case NodeTypes::NodeMod: {
            auto n = static_cast<struct NodeMod*>(node);
            std::cout << Style("NodeMod (mod " + n->modifier + ")", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->expr, level + 1, newStatus, true);
            return;
        }

        case NodeTypes::NodeInput: {
            auto n = static_cast<struct NodeInput*>(node);
            std::cout << Style("NodeInput (input)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            if (n->prompt)
                printNode(n->prompt, level + 1, newStatus, true);
            else {
                printPrefix(newStatus, level + 1, true);
                std::cout << Style("[no prompt]", TerminalStyle::DIM_WHITE) << "\n";
            }
            return;
        }

        case NodeTypes::NodePtr: {
            auto p = static_cast<struct NodePtr*>(node);
            std::cout << Style("NodePtr (ptr)", TerminalStyle::BRIGHT_CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(p->addr, level + 1, newStatus, false);
            printNode(p->type, level + 1, newStatus, true);
            return;
        }

        case NodeTypes::NodeStructLiteral: {
            auto n = static_cast<struct NodeStructLiteral*>(node);
            std::cout << Style("NodeStructLiteral { }", TerminalStyle::CYAN) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            bool hasGenerics = !n->generics.empty();
            bool hasInits = !n->inits.empty();
            printNode(n->type_expr, level + 1, newStatus, !hasGenerics && !hasInits);
            if (hasGenerics) {
                printPrefix(newStatus, level + 1, !hasInits);
                std::cout << Style("Generics", TerminalStyle::CYAN) << "\n";
                std::vector<bool> gensStatus = newStatus;
                gensStatus.push_back(!hasInits);
                for (size_t i = 0; i < n->generics.size(); ++i)
                    printNode(n->generics[i], level + 2, gensStatus, i + 1 == n->generics.size());
            }
            if (hasInits) {
                printPrefix(newStatus, level + 1, true);
                std::cout << Style("Fields", TerminalStyle::CYAN) << "\n";
                std::vector<bool> fStatus = newStatus;
                fStatus.push_back(true);
                for (size_t i = 0; i < n->inits.size(); ++i) {
                    bool isLastInit = (i == n->inits.size() - 1);
                    printPrefix(fStatus, level + 2, isLastInit);
                    std::cout << Style("Field (" + n->inits[i].name + ")", TerminalStyle::YELLOW) << "\n";
                    std::vector<bool> vStatus = fStatus;
                    vStatus.push_back(isLastInit);
                    printNode(n->inits[i].value, level + 3, vStatus, true);
                }
            }
            return;
        }

        case NodeTypes::NodeEcho: {
            auto n = static_cast<struct NodeEcho*>(node);
            std::cout << Style("NodeEcho (echo)", TerminalStyle::MAGENTA) << "\n";
            std::vector<bool> newStatus = levelsStatus;
            newStatus.push_back(isCurrentLast);
            printNode(n->expr, level + 1, newStatus, true);
            return;
        }

        default:
            printPrefix(levelsStatus, level, isCurrentLast);
            std::cout << Style("Unknown Node Type", TerminalStyle::BRIGHT_RED) << "\n";
            break;
    }
}

void printNodes(AbstractTreeT nodes) {
    // Начальный вектор статусов. Индекс 0 не используется (корень), но нам нужно место под корень.
    // levelsStatus[i] хранит статус узла на глубине i.
    std::vector<bool> initialStatus;
    initialStatus.push_back(true); // Заглушка для уровня 0

    for (size_t i = 0; i < nodes.size(); ++i) {
        bool isLast = (i == nodes.size() - 1);
        // Для верхнего уровня parent status не важен, но передадим true
        printNode(nodes[i], 1, initialStatus, isLast);
    }
}
