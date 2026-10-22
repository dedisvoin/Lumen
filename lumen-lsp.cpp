#define NOMINMAX
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cctype>
#include <fstream>
#ifndef _WIN32
#include <dirent.h>
#endif
#include <windows.h>

#include "compiler/utils/JSON.cpp"

#include "compiler/core/ErrorBase.cpp"
#include "compiler/utils/Strings.cpp"
#include "compiler/utils/ArgParser.cpp"
#include "compiler/Lexer.cpp"
#include "compiler/Preprocessor.cpp"
#include "compiler/ASTGenerator.cpp"
#include "compiler/SemanticAnalizer.cpp"
#include "compiler/DeepAnalizer.cpp"
#include "compiler/TypeAnalyzer.cpp"
#include "compiler/utils/File.cpp"


#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

using std::string;

// ---------- JSON-РЎвЂ¦Р ВµР В»Р С—Р ВµРЎР‚РЎвЂ№ Р С—Р С•РЎРѓРЎвЂљРЎР‚Р С•Р ВµР Р…Р С‘РЎРЏ ----------
static JsonValue JString(const string& s) { JsonValue v; v.type = JsonValue::Type::String; v.str = s; return v; }
static JsonValue JNumber(double n)        { JsonValue v; v.type = JsonValue::Type::Number; v.number = n; return v; }
static JsonValue JBool(bool b)             { JsonValue v; v.type = JsonValue::Type::Bool; v.boolean = b; return v; }
static JsonValue JObject()                 { JsonValue v; v.type = JsonValue::Type::Object; return v; }
static JsonValue JArray()                  { JsonValue v; v.type = JsonValue::Type::Array; return v; }

// ---------- Р СџР С•Р В»Р Р…РЎвЂ№Р в„– РЎвЂљР ВµР С”РЎРѓРЎвЂљ Р Т‘Р С•Р С”РЎС“Р СР ВµР Р…РЎвЂљР С•Р Р† (Р Т‘Р В»РЎРЏ Р С‘Р Р…Р С”РЎР‚Р ВµР СР ВµР Р…РЎвЂљР В°Р В»РЎРЉР Р…РЎвЂ№РЎвЂ¦ didChange) ----------
static std::map<std::string, std::string> g_docs; // uri -> Р С—Р С•Р В»Р Р…РЎвЂ№Р в„– РЎвЂљР ВµР С”РЎРѓРЎвЂљ
static std::map<std::string, std::vector<InlayHintData>> g_inlayHints;
static std::map<std::string, std::map<unsigned long long, Type>> g_localTypes; // uri -> (line<<32|index) -> full type of a local (P8) // uri -> Р С‘Р Р…Р В»Р В°Р в„–Р Р…-РЎвЂ¦Р С‘Р Р…РЎвЂљРЎвЂ№
static std::map<std::string, std::vector<Token>> g_tokens;   // uri -> РЎвЂљР С•Р С”Р ВµР Р…РЎвЂ№ Р В»Р ВµР С”РЎРѓР ВµРЎР‚Р В°
static std::map<std::string, Context> g_contexts;             // uri -> global_context
static std::map<std::string, std::vector<Node*>> g_trees;     // uri -> AST
static std::map<std::string, std::vector<Define>> g_defines;  // uri -> #define Р Т‘Р С‘РЎР‚Р ВµР С”РЎвЂљР С‘Р Р†РЎвЂ№
static std::map<std::string, std::vector<Macro>>  g_macros;   // uri -> #macro Р Т‘Р С‘РЎР‚Р ВµР С”РЎвЂљР С‘Р Р†РЎвЂ№
static std::map<std::string, std::map<String, StructSymbol>> g_structs; // uri -> РЎРѓРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚РЎвЂ№
// Deep analysis toggle: settings_apply -> initializationOptions.lumen.deepAnalysis.enable.
// When disabled, only semantic diagnostics + fallback highlighting remain.
static bool g_noDeepAnalysis = false;

static std::map<std::string, std::map<String, EnumSymbol>> g_enums;    // uri -> enum'РЎвЂ№

// UTF-16 code units -> Р В±Р В°Р в„–РЎвЂљР С•Р Р†Р С•Р Вµ РЎРѓР СР ВµРЎвЂ°Р ВµР Р…Р С‘Р Вµ Р Р†Р Р…РЎС“РЎвЂљРЎР‚Р С‘ Р С•Р Т‘Р Р…Р С•Р в„– UTF-8 РЎРѓРЎвЂљРЎР‚Р С•Р С”Р С‘
static size_t utf16ToByteOffset(const std::string& lineText, int character) {
    size_t units = 0, i = 0;
    while (i < lineText.size() && units < (size_t)character) {
        unsigned char c = (unsigned char)lineText[i];
        int cl;
        if (c < 0x80) cl = 1;
        else if ((c & 0xE0) == 0xC0) cl = 2;
        else if ((c & 0xF0) == 0xE0) cl = 3;
        else if ((c & 0xF8) == 0xF0) cl = 4;
        else { i++; continue; }
        int u16 = (cl >= 4) ? 2 : 1;
        if (units + u16 > (size_t)character) break;
        units += u16;
        i += cl;
    }
    return i;
}

// LSP-Р С—Р С•Р В·Р С‘РЎвЂ Р С‘РЎРЏ {line, character}(UTF-16) -> Р В±Р В°Р в„–РЎвЂљР С•Р Р†Р С•Р Вµ РЎРѓР СР ВµРЎвЂ°Р ВµР Р…Р С‘Р Вµ Р Р† UTF-8 РЎвЂљР ВµР С”РЎРѓРЎвЂљР Вµ
static size_t lspPosToByteOffset(const std::string& text, int line, int character) {
    size_t off = 0;
    int curLine = 0;
    while (curLine < line) {
        size_t nl = text.find('\n', off);
        if (nl == std::string::npos) return text.size();
        off = nl + 1;
        curLine++;
    }
    size_t nl = text.find('\n', off);
    std::string lineText = (nl == std::string::npos)
        ? text.substr(off) : text.substr(off, nl - off);
    return off + utf16ToByteOffset(lineText, character);
}

// ---------- Р СћРЎР‚Р В°Р Р…РЎРѓР С—Р С•РЎР‚РЎвЂљ (JSON-RPC Р С—Р С• stdio, Content-Length framing) ----------
static bool readMessage(string& out) {
    out.clear();
    string line;
    size_t contentLength = 0;
    // РЎвЂЎР С‘РЎвЂљР В°Р ВµР С Р В·Р В°Р С–Р С•Р В»Р С•Р Р†Р С”Р С‘ Р Т‘Р С• Р С—РЎС“РЎРѓРЎвЂљР С•Р в„– РЎРѓРЎвЂљРЎР‚Р С•Р С”Р С‘
    while (true) {
        if (!std::getline(std::cin, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        if (line.compare(0, 15, "Content-Length:") == 0)
            contentLength = std::stoul(line.substr(15));
    }
    if (contentLength == 0) return false;
    out.resize(contentLength);
    std::cin.read(&out[0], (std::streamsize)contentLength);
    return true;
}

static void writeMessage(const string& body) {
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::cout.flush();
}

// ---------- URI -> Р В»Р С•Р С”Р В°Р В»РЎРЉР Р…РЎвЂ№Р в„– Р С—РЎС“РЎвЂљРЎРЉ (Р Т‘Р В»РЎРЏ РЎРѓР С•Р С—Р С•РЎРѓРЎвЂљР В°Р Р†Р В»Р ВµР Р…Р С‘РЎРЏ Р С•РЎв‚¬Р С‘Р В±Р С•Р С”) ----------
static string pathFromUri(const string& uri) {
    string p = uri;
    const string prefix = "file://";
    if (p.compare(0, prefix.size(), prefix) == 0) p = p.substr(prefix.size());
    if (!p.empty() && p[0] == '/') p = p.substr(1);
    size_t pos = 0;
    while ((pos = p.find("%20", pos)) != string::npos) { p.replace(pos, 3, " "); pos++; }
    return p;
}

// ---------- Р С™Р С•Р Р…Р Р†Р ВµРЎР‚РЎвЂљР В°РЎвЂ Р С‘РЎРЏ BaseError -> LSP Diagnostic ----------
static JsonValue diagnosticFromError(const BaseError& err, const string& uri) {
    JsonValue d = JObject();

    JsonValue range = JObject();
    JsonValue start = JObject();
    start.object["line"]      = JNumber(err.line);
    start.object["character"] = JNumber(err.column);
    JsonValue end = JObject();
    end.object["line"]      = JNumber(err.end_line);
    end.object["character"] = JNumber(err.end_column);
    range.object["start"] = start;
    range.object["end"]   = end;
    d.object["range"] = range;

    d.object["severity"] = JNumber(err.is_warning ? 2 : 1);

    string msg = err.message.to_utf8();
    if (!err.special_message.empty())
        msg += ": " + err.special_message.to_utf8();
    d.object["message"] = JString(msg);

    d.object["source"] = JString("lumen");
    d.object["code"]   = JNumber(err.error_code);

    if (err.sub_error) {
        JsonValue rel = JArray();
        JsonValue item = JObject();

        JsonValue loc = JObject();
        loc.object["uri"] = JString(uri);
        JsonValue r = JObject();
        JsonValue s = JObject();
        s.object["line"]      = JNumber(err.sub_error->line);
        s.object["character"] = JNumber(err.sub_error->column);
        JsonValue e = JObject();
        e.object["line"]      = JNumber(err.sub_error->end_line);
        e.object["character"] = JNumber(err.sub_error->end_column);
        r.object["start"] = s;
        r.object["end"]   = e;
        loc.object["range"] = r;
        item.object["location"] = loc;
        item.object["message"]  = JString(err.sub_error->message.to_utf8());

        rel.array.push_back(item);
        d.object["relatedInformation"] = rel;
    }

    return d;
}

// ---------- Р СџРЎР‚Р С•Р С–Р С•Р Р… Р С”Р С•Р Р…Р Р†Р ВµР в„–Р ВµРЎР‚Р В° Р С‘ Р С—РЎС“Р В±Р В»Р С‘Р С”Р В°РЎвЂ Р С‘РЎРЏ Р Т‘Р С‘Р В°Р С–Р Р…Р С•РЎРѓРЎвЂљР С‘Р С” ----------
static void analyzeAndPublish(const string& uri, const string& text) {
    string path = pathFromUri(uri);

    ErrorContainer ec;
    auto tokens = fastLex(text, path);
    Preprocessor pp(false, ec);
    auto result = pp(tokens);
    ASTGenerator ast(result, pp.source_tokens, false, ec);
    auto tree = ast();

    auto sa = ASTAnalizer(tree, ec);
    sa();
    auto ta = TypeAnalyzer(tree, ec);
    auto da = ASTDeepAnalyzer(tree, ec);
    da.symbols_only = true;
    if (!g_noDeepAnalysis) { da(); ta(); }
    // M8: значения, вычисленные интерпретатором внутри top-level eval-блоков
    // (echo -> value-хинты), добавляем к выводу TypeAnalyzer. Берём только
    // хинты-значения (не типы " > ") и только в диапазоне строк eval-блока.
    if (!ta.eval_blocks.empty()) {
        for (auto& h : da.inlay_hints) {
            if (h.label.size() >= 2 && h.label[0] == U' ' && h.label[1] == U'>')
                continue; // тип, не значение
            for (Node* b : ta.eval_blocks) {
                auto* ev = static_cast<struct NodeEvalBlock*>(b);
                int lo = ev->eval_token.position.line;
                int hi = ev->end_token.position.line;
                if (h.line >= lo && h.line <= hi) {
                    ta.inlay_hints.push_back(h);
                    break;
                }
            }
        }
    }
    g_inlayHints[uri] = ta.inlay_hints;
    g_localTypes[uri] = std::move(ta.local_types); // P8: full types of locals for chains
    g_tokens[uri] = tokens;
    g_contexts[uri] = da.global_context;
    g_structs[uri] = std::move(da.structs); // СѓРєР°Р·Р°С‚РµР»Рё Node* Р¶РёРІСѓС‚, РїРѕРєР° Р¶РёРІ g_trees
    g_enums[uri] = std::move(da.enums);     // enum'С‹ вЂ” РґР»СЏ hover/Р°РІС‚РѕРґРѕРїРѕР»РЅРµРЅРёСЏ РІР°СЂРёР°РЅС‚РѕРІ
    g_trees[uri] = tree;
    // Р вЂќР С‘РЎР‚Р ВµР С”РЎвЂљР С‘Р Р†РЎвЂ№ РЎР‚Р В°РЎРѓР С”РЎР‚РЎвЂ№Р Р†Р В°РЎР‹РЎвЂљРЎРѓРЎРЏ Р Р† РЎвЂљР С•Р С”Р ВµР Р…Р В°РЎвЂ¦ Р С‘ Р Р† AST Р С‘РЎвЂ¦ Р Р…Р ВµРЎвЂљ РІР‚вЂќ РЎРѓР С•РЎвЂ¦РЎР‚Р В°Р Р…РЎРЏР ВµР С Р С•РЎвЂљР Т‘Р ВµР В»РЎРЉР Р…Р С•,
    // РЎвЂЎРЎвЂљР С•Р В±РЎвЂ№ Р С—Р С•Р С”Р В°Р В·РЎвЂ№Р Р†Р В°РЎвЂљРЎРЉ hover Р С—Р С• Р С‘Р СР ВµР Р…Р В°Р С #define/#macro.
    g_defines[uri] = pp.defines;
    g_macros[uri] = pp.macros;



    JsonValue diags = JArray();
    for (const auto& err : ec.errors) {
        if (err.filepath.to_utf8() != path) continue; // СљС•Р»СЊРєРѕ РўРµС…РЅРёС‡РµРЎРєРё РўРµРЎРўРЎРўРµРЎРўР‚Р•Р В»РЎРЉР С”Р С• РЎвЂљР ВµР С”РЎС“РЎвЂ°Р С‘Р в„– Р Т‘Р С•Р С”РЎС“Р СР ВµР Р…РЎвЂљ
        diags.array.push_back(diagnosticFromError(err, uri));
        // subError (Р Р…Р В°Р С—РЎР‚. "Declared here") Р С•РЎвЂљР С—РЎР‚Р В°Р Р†Р В»РЎРЏР ВµР С Р С•РЎвЂљР Т‘Р ВµР В»РЎРЉР Р…Р С•Р в„– Р Т‘Р С‘Р В°Р С–Р Р…Р С•РЎРѓРЎвЂљР С‘Р С”Р С•Р в„–,
        // РЎвЂљ.Р С”. РЎР‚Р ВµР Т‘Р В°Р С”РЎвЂљР С•РЎР‚ Р Р…Р Вµ РЎР‚Р ВµР Р…Р Т‘Р ВµРЎР‚Р С‘РЎвЂљ relatedInformation, Р В° Р С—Р С•Р Т‘РЎвЂЎРЎвЂРЎР‚Р С”Р С‘Р Р†Р В°Р ВµРЎвЂљ РЎвЂљР С•Р В»РЎРЉР С”Р С• range Р С”Р В°Р В¶Р Т‘Р С•Р в„– Р Т‘Р С‘Р В°Р С–Р Р…Р С•РЎРѓРЎвЂљР С‘Р С”Р С‘.
        // Р ТђР С•Р Т‘Р С‘Р С Р С—Р С• Р Р†РЎРѓР ВµР в„– РЎвЂ Р ВµР С—Р С•РЎвЂЎР С”Р Вµ: РЎРѓРЎвЂљР ВµР С” Р Р†РЎвЂ№Р В·Р С•Р Р†Р С•Р Р† = sub_error -> sub_error -> ...
        const BaseError* sub = err.sub_error.get();
        while (sub) {
            diags.array.push_back(diagnosticFromError(*sub, uri));
            sub = sub->sub_error.get();
        }
    }

    JsonValue params = JObject();
    params.object["uri"] = JString(uri);
    params.object["diagnostics"] = diags;

    JsonValue msg = JObject();
    msg.object["jsonrpc"] = JString("2.0");
    msg.object["method"]  = JString("textDocument/publishDiagnostics");
    msg.object["params"]  = params;

    writeMessage(toJson(msg));
}

// ---------- Р С›РЎвЂљР Р†Р ВµРЎвЂљРЎвЂ№ Р Р…Р В° request ----------
static void sendResult(const JsonValue& id, const JsonValue& result) {
    JsonValue msg = JObject();
    msg.object["jsonrpc"] = JString("2.0");
    msg.object["id"]      = id;
    msg.object["result"]  = result;
    writeMessage(toJson(msg));
}

static void sendInitializeResult(const JsonValue& id) {
    JsonValue caps = JObject();
    caps.object["textDocumentSync"] = JNumber(1); // full sync
    caps.object["inlayHintProvider"] = JObject(); // textDocument/inlayHint
    caps.object["hoverProvider"] = JBool(true);   // textDocument/hover
    caps.object["documentSymbolProvider"] = JBool(true); // textDocument/documentSymbol

    JsonValue completionProvider = JObject();
    JsonValue triggers = JArray();
    triggers.array.push_back(JString("."));
    triggers.array.push_back(JString(":"));
    triggers.array.push_back(JString(">"));
    completionProvider.object["triggerCharacters"] = triggers;
    completionProvider.object["resolveProvider"] = JBool(true);
    caps.object["completionProvider"] = completionProvider;

    // semanticTokensProvider
    JsonValue semanticTokensProvider = JObject();
    {
        JsonValue legend = JObject();
        JsonValue tokenTypesArr = JArray();
        const char* tokenTypeNames[] = {
            "keyword",    // 0
            "number",     // 1
            "string",     // 2
            "comment",    // 3
            "operator",   // 4
            "type",       // 5
            "function",   // 6
            "variable",   // 7
            "parameter",  // 8
            "namespace",  // 9
            "macro",      // 10
            "modifier",   // 11
            "typeParameter", // 12
            "selfParameter", // 13
            "macroFunction", // 14
        };
        for (const char* name : tokenTypeNames)
            tokenTypesArr.array.push_back(JString(name));
        legend.object["tokenTypes"] = tokenTypesArr;
        legend.object["tokenModifiers"] = JArray();
        semanticTokensProvider.object["legend"] = legend;
        semanticTokensProvider.object["full"] = JObject();
    }
    caps.object["semanticTokensProvider"] = semanticTokensProvider;

    JsonValue result = JObject();
    result.object["capabilities"] = caps;

    JsonValue info = JObject();
    info.object["name"]    = JString("lumen-lsp");
    info.object["version"] = JString("0.1");
    result.object["serverInfo"] = info;

    sendResult(id, result);
}

// ---------- Р С’Р Р†РЎвЂљР С•Р Т‘Р С•Р С—Р С•Р В»Р Р…Р ВµР Р…Р С‘Р Вµ (textDocument/completion) ----------

// LSP CompletionItemKind:
//   3 = Function, 6 = Variable, 7 = Class, 9 = Module, 14 = Keyword, 21 = Constant
static const int KIND_FUNCTION = 3;
static const int KIND_VARIABLE = 6;
static const int KIND_TYPE     = 7;
static const int KIND_MODULE   = 9;
static const int KIND_ENUM     = 13;   // СЃР°Рј enum (С‚РёРї)
static const int KIND_KEYWORD  = 14;
static const int KIND_ENUM_MEMBER = 20; // РІР°СЂРёР°РЅС‚ enum (`Types::a`)
static const int KIND_CONSTANT = 21;
// Lumen-РЎРѓР С—Р ВµРЎвЂ Р С‘РЎвЂћР С‘РЎвЂЎР Р…РЎвЂ№Р Вµ Р С”Р В°РЎвЂљР ВµР С–Р С•РЎР‚Р С‘Р С‘ (Р С”Р В»Р С‘Р ВµР Р…РЎвЂљ Р СР В°Р С—Р С—Р С‘РЎвЂљ Р С‘РЎвЂ¦ Р Р…Р В° РЎРѓР Р†Р С•РЎР‹ Р С‘Р С”Р С•Р Р…Р С”РЎС“ Р’В«Р СР В°Р С”РЎР‚Р С•РЎРѓР’В»).
static const int KIND_DEFINE = 1000;  // #define
static const int KIND_MACRO  = 1001;  // #macro
static const int KIND_DIRECTIVE = 1002; // #pragma/#include/#define и пр.
static const int KIND_MODIFIER = 1003;  // const/static/global/final/private/shadow/strict/ref

static bool isIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// Р СџРЎвЂ№РЎвЂљР В°Р ВµРЎвЂљРЎРѓРЎРЏ РЎР‚Р В°РЎРѓР С—Р С•Р В·Р Р…Р В°РЎвЂљРЎРЉ "...РЎвЂ Р ВµР С—Р С•РЎвЂЎР С”Р В°::Р С‘Р СРЎвЂР Р… X::|Р С—РЎР‚Р ВµРЎвЂћР С‘Р С”РЎРѓ" Р С‘Р В»Р С‘ "РЎвЂ Р ВµР С—Р С•РЎвЂЎР С”Р В°.Р С‘Р СРЎвЂР Р….Р С—РЎР‚Р ВµРЎвЂћР С‘Р С”РЎРѓ"
// Р С—Р ВµРЎР‚Р ВµР Т‘ Р С”РЎС“РЎР‚РЎРѓР С•РЎР‚Р С•Р С (bytePos). Р вЂ”Р В°Р С—Р С•Р В»Р Р…РЎРЏР ВµРЎвЂљ ownerChain (Р С‘Р СР ВµР Р…Р В° Р Р†Р В»Р В°Р Т‘Р ВµР В»РЎРЉРЎвЂ Р В°) Р С‘ prefix.
// isScope = true, Р ВµРЎРѓР В»Р С‘ Р С—Р С•РЎРѓР В»Р ВµР Т‘Р Р…Р С‘Р в„– РЎРѓР ВµР С—Р В°РЎР‚Р В°РЎвЂљР С•РЎР‚ '::' (РЎРѓРЎвЂљР В°РЎвЂљР С‘РЎвЂЎР ВµРЎРѓР С”Р С‘Р в„– Р Т‘Р С•РЎРѓРЎвЂљРЎС“Р С— Р С” РЎвЂЎР В»Р ВµР Р…РЎС“).
// РЎРµРіРјРµРЅС‚С‹-РІС‹Р·РѕРІС‹ (`b.at(0).`): Р·Р° 'РёРјРµРЅРµРј' РёРґС‘С‚ '(', Р°СЂРіСѓРјРµРЅС‚С‹ Р±Р°Р»Р°РЅСЃРѕРј РїСЂРѕРїСѓСЃРєР°СЋС‚СЃСЏ,
// СЃРµРіРјРµРЅС‚ РїРѕРјРµС‡Р°РµС‚СЃСЏ РІ callFlags (parallel ownerChain), РёРјСЏ вЂ” Р±РµР· Р°СЂРіСѓРјРµРЅС‚РѕРІ.
static bool memberAccessContext(const std::string& text, size_t bytePos,
                                std::vector<string>& ownerChain, string& prefix,
                                bool* isScope = nullptr,
                                std::vector<bool>* callFlags = nullptr) {
    size_t p = bytePos;
    while (p > 0 && (text[p-1] == ' ' || text[p-1] == '\t')) p--;

    size_t wordEnd = p;
    while (p > 0 && isIdentChar(text[p-1])) p--;
    size_t wordStart = p;
    // Директивы типа "#pragma" пишутся с ведущим '#': включаем его в слово,
    // иначе префикс при наборе "#pr" станет "pr" и директива не попадёт
    // в автокомплит.
    if (wordStart >= 1 && text[wordStart-1] == '#') wordStart--;
    prefix = text.substr(wordStart, wordEnd - wordStart);

    size_t sep = (size_t)-1;
    if (wordStart >= 2 && text[wordStart-1] == ':' && text[wordStart-2] == ':') {
        sep = wordStart - 2;
        if (isScope) *isScope = true;
    } else if (wordStart >= 1 && text[wordStart-1] == '.') {
        sep = wordStart - 1;
        if (isScope) *isScope = false;
    }
    if (sep == (size_t)-1) return false;

    size_t q = sep;
    while (true) {
        while (q > 0 && (text[q-1] == ' ' || text[q-1] == '\t')) q--;
        size_t e = q;
        while (q > 0 && isIdentChar(text[q-1])) q--;
        if (q == e) {
            // Р С—Р ВµРЎР‚Р ВµР Т‘ РЎРѓР ВµР С—Р В°РЎР‚Р В°РЎвЂљР С•РЎР‚Р С•Р С Р Р…Р Вµ Р С‘Р Т‘Р ВµР Р…РЎвЂљР С‘РЎвЂћР С‘Р С”Р В°РЎвЂљР С•РЎР‚: Р ВµРЎРѓР В»Р С‘ ')' РІР‚вЂќ Р С—РЎР‚Р С•Р С—РЎС“РЎРѓР С”Р В°Р ВµР С
            // РЎР‚Р В°Р В±Р В°Р В»Р В°Р Р…РЎРѓР С‘РЎР‚Р С•Р Р†Р В°Р Р…Р Р…РЎвЂ№Р Вµ Р В°РЎР‚Р С–РЎС“Р СР ВµР Р…РЎвЂљРЎвЂ№ '(' ... ')' Р С‘ Р С‘РЎвЂ°Р ВµР С Р С‘Р СРЎРЏ Р СР ВµРЎвЂљР С•Р Т‘Р В°.
            if (q >= 1 && text[q-1] == ')') {
                q--;
                int depth = 1;
                while (q > 0 && depth) {
                    char c = text[--q];
                    if (c == ')') depth++;
                    else if (c == '(') depth--;
                }
                if (depth != 0) break;   // Р Р…Р ВµРЎРѓР В±Р В°Р В»Р В°Р Р…РЎРѓР С‘РЎР‚Р С•Р Р†Р В°Р Р…Р Р…Р С• РІР‚вЂќ Р Р…Р Вµ РЎР‚Р ВµР В·Р С•Р В»Р Р†Р С‘Р С
                continue;
            }
            if (q >= 1 && text[q-1] == ']') {
                q--;
                int depth = 1;
                while (q > 0 && depth) {
                    char c = text[--q];
                    if (c == ']') depth++;
                    else if (c == '[') depth--;
                }
                if (depth != 0) break;   // Р Р…Р ВµРЎРѓР В±Р В°Р В»Р В°Р Р…РЎРѓР С‘РЎР‚Р С•Р Р†Р В°Р Р…Р Р…Р С• РІР‚вЂќ Р Р…Р Вµ РЎР‚Р ВµР В·Р С•Р В»Р Р†Р С‘Р С
                continue;
            }
            break;   // Р С—Р ВµРЎР‚Р ВµР Т‘ РЎРѓР ВµР С—Р В°РЎР‚Р В°РЎвЂљР С•РЎР‚Р С•Р С Р Р…Р Вµ Р С‘Р Т‘Р ВµР Р…РЎвЂљР С‘РЎвЂћР С‘Р С”Р В°РЎвЂљР С•РЎР‚ (Р Р…Р В°Р С—РЎР‚. ']')
        }
        bool isCall = e < text.size() && text[e] == '(';
        ownerChain.insert(ownerChain.begin(), text.substr(q, e - q));
        if (callFlags) callFlags->insert(callFlags->begin(), isCall);
        if (q >= 2 && text[q-1] == ':' && text[q-2] == ':') { q -= 2; continue; }
        if (q >= 1 && text[q-1] == '.') { q -= 1; continue; }
        break;
    }
    return !ownerChain.empty();
}

// ---------- Р вЂєР С•Р С”Р В°Р В»РЎРЉР Р…РЎвЂ№Р Вµ Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…РЎвЂ№Р Вµ Р С‘ Р С—Р В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚РЎвЂ№ РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р в„– ----------

static std::vector<size_t> buildLineStarts(const std::string& text);

struct LocalCandidate {
    std::string name;
    int kind = KIND_VARIABLE;   // CompletionItemKind Variable
    std::string detail;
    unsigned long long declKey = 0;   // (line<<32|index) РґРµРєР». С‚РѕРєРµРЅР° -> РєР»СЋС‡ РІ g_localTypes (P8)
};

static size_t tokenByteOffset(const Token& t, const std::vector<size_t>& lineStarts) {
    return ((size_t)t.position.line < lineStarts.size())
        ? lineStarts[t.position.line] + (size_t)t.position.index
        : (size_t)t.position.index;
}

// Р СџР В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚РЎвЂ№ Р Р†РЎРѓР ВµРЎвЂ¦ РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р в„–, РЎвЂЎР ВµР в„– Р Т‘Р С‘Р В°Р С—Р В°Р В·Р С•Р Р… [Р С‘Р СРЎРЏ ../Р В·Р В°Р С”РЎР‚РЎвЂ№Р Р†Р В°РЎР‹РЎвЂ°Р В°РЎРЏ '}'] РЎРѓР С•Р Т‘Р ВµРЎР‚Р В¶Р С‘РЎвЂљ Р С—Р С•Р В·Р С‘РЎвЂ Р С‘РЎР‹ pos.
static void collectParamsInRange(const std::vector<Token>& tokens,
                                 const std::vector<size_t>& lineStarts,
                                 size_t pos,
                                 std::map<std::string, LocalCandidate>& out) {
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        const Token& tok = tokens[i];
        if (tok.type != KEYWORD) continue;
        const std::string kw = tok.value.to_utf8();
        if (kw != "fn" && kw != "lambda") continue;

        size_t nameTok = i + 1;
        size_t nameOff;
        if (nameTok < tokens.size() && tokens[nameTok].type == LITERAL) {
            nameOff = tokenByteOffset(tokens[nameTok], lineStarts);
        } else if (nameTok < tokens.size() && tokens[nameTok].type == BRACE_LEFT) {
            nameOff = tokenByteOffset(tok, lineStarts);   // lambda(...) Р В±Р ВµР В· Р С‘Р СР ВµР Р…Р С‘
        } else {
            continue;
        }

        size_t k = nameTok;
        if (k < tokens.size() && tokens[k].type == LITERAL) k++;
        std::vector<std::string> generics;                     // [Р С–Р ВµР Р…Р ВµРЎР‚Р С‘Р С”Р С‘]
        if (k < tokens.size() && tokens[k].type == BRACE_LEFT_RECT) {
            int d = 1;
            while (k < tokens.size() && d) {
                k++;
                if (tokens[k].type == BRACE_LEFT_RECT) d++;
                else if (tokens[k].type == BRACE_RIGHT_RECT) {
                    d--;
                    if (d == 0) break;
                } else if (d == 1 && tokens[k].type == LITERAL) {
                    generics.push_back(tokens[k].value.to_utf8());
                }
            }
            k++;  // Р В·Р В° Р В·Р В°Р С”РЎР‚РЎвЂ№Р Р†Р В°РЎР‹РЎвЂ°РЎС“РЎР‹ ']'
        }
        if (k >= tokens.size() || tokens[k].type != BRACE_LEFT) continue;  // '('
        size_t parenOpen = k;

        int pd = 0;
        size_t parenEnd = tokens.size();
        for (size_t q = parenOpen; q < tokens.size(); ++q) {
            if (tokens[q].type == BRACE_LEFT) pd++;
            else if (tokens[q].type == BRACE_RIGHT) {
                pd--;
                if (pd == 0) { parenEnd = q; break; }
            }
        }
        if (parenEnd >= tokens.size()) continue;

        // Р С‘Р СР ВµР Р…Р В° Р С—Р В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚Р С•Р Р†: Р С—Р ВµРЎР‚Р Р†РЎвЂ№Р в„– LITERAL Р С—Р С•РЎРѓР В»Р Вµ '(' Р С‘Р В»Р С‘ Р С—Р С•РЎРѓР В»Р Вµ ','
        std::vector<std::pair<std::string, std::string>> params; // name, type
        bool expectName = true;
        for (size_t q = parenOpen + 1; q < parenEnd; ++q) {
            const Token& tq = tokens[q];
            if (expectName) {
                if (tq.type != LITERAL) continue;
                std::string pn = tq.value.to_utf8();
                bool isRefParam = (q > 0 && tokens[q-1].type == KEYWORD
                                   && tokens[q-1].value.to_utf8() == "ref");
                std::string pt;
                size_t s = q + 1;
                if (s < parenEnd && tokens[s].type == OPERATOR
                        && tokens[s].value.to_utf8() == ":") {
                    s++;
                    while (s < parenEnd
                           && tokens[s].type != LITERAL
                           && tokens[s].type != KEYWORD
                           && tokens[s].type != STRING)
                        s++;
                    if (s < parenEnd) pt = tokens[s].value.to_utf8();
                }
                if (isRefParam) pt = pt.empty() ? std::string("ref") : "ref " + pt;
                params.push_back(std::make_pair(pn, pt));
                expectName = false;
            } else if (tq.type == OPERATOR && tq.value.to_utf8() == ",") {
                expectName = true;
            }
        }
        if (params.empty() && generics.empty()) continue;

        // Р С”Р С•Р Р…Р ВµРЎвЂ  РЎвЂљР ВµР В»Р В°: '{' Р С—Р С•РЎРѓР В»Р Вµ ')' ... Р В·Р В°Р С”РЎР‚РЎвЂ№Р Р†Р В°РЎР‹РЎвЂ°Р В°РЎРЏ '}'
        size_t bodyOpen = parenEnd + 1;
        while (bodyOpen < tokens.size()
               && tokens[bodyOpen].type != BRACE_LEFT_CURVE)
            bodyOpen++;
        if (bodyOpen >= tokens.size()) continue;

        int d = 0;
        bool opened = false;
        size_t bodyEnd = 0;
        for (size_t e = bodyOpen; e < tokens.size(); ++e) {
            if (tokens[e].type == BRACE_LEFT_CURVE) { d++; opened = true; }
            else if (tokens[e].type == BRACE_RIGHT_CURVE) {
                if (d > 0) d--;
                if (d == 0 && opened) {
                    bodyEnd = tokenByteOffset(tokens[e], lineStarts);
                    break;
                }
            }
        }

        if (pos < nameOff) continue;
        if (bodyEnd != 0 && pos > bodyEnd) continue;
        for (const std::string& g : generics) {
            if (out.count(g)) continue;
            out[g] = LocalCandidate{g, KIND_TYPE, "type"};
        }
        for (auto& pr : params) {
            if (out.count(pr.first)) continue;
            out[pr.first] = LocalCandidate{pr.first, KIND_VARIABLE,
                pr.second.empty() ? std::string("parameter") : pr.second};
        }
    }
}

// Р вЂєР С•Р С”Р В°Р В»РЎРЉР Р…РЎвЂ№Р Вµ Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…РЎвЂ№Р Вµ (let) Р С‘ Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…РЎвЂ№Р Вµ РЎвЂ Р С‘Р С”Р В»Р С•Р Р† (for x ...), Р Р†Р С‘Р Т‘Р С‘Р СРЎвЂ№Р Вµ Р Р† Р С—Р С•Р В·Р С‘РЎвЂ Р С‘Р С‘ pos.
// Р С›Р В±РЎвЂ¦Р С•Р Т‘ РЎвЂљР С•Р С”Р ВµР Р…Р С•Р Р† РЎРѓР С• РЎРѓРЎвЂљР ВµР С”Р С•Р С РЎРѓР С”Р С•Р В±Р С•Р С”: Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…Р В°РЎРЏ Р Р†Р С‘Р Т‘Р Р…Р В°, Р С—Р С•Р С”Р В° Р Р…Р Вµ Р В·Р В°Р С”РЎР‚РЎвЂ№Р В»РЎРѓРЎРЏ Р ВµРЎвЂ Р В±Р В»Р С•Р С”.
static void collectLetVars(const std::vector<Token>& tokens,
                           const std::vector<size_t>& lineStarts,
                           size_t pos,
                           std::map<std::string, LocalCandidate>& out,
                           std::map<std::string, size_t>* declPositions = nullptr) {
    struct Decl { std::string name; int kind; std::string detail;
                  size_t declOff; size_t visEnd;
                  unsigned long long declKey = 0; };
    struct Scope { std::vector<size_t> varIdx; };

    std::vector<Decl> decls;
    std::vector<Scope> scopes;
    std::vector<Decl> pendingFor;   // Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…РЎвЂ№Р Вµ РЎвЂ Р С‘Р С”Р В»Р С•Р Р† Р В¶Р Т‘РЎС“РЎвЂљ '{' РЎвЂљР ВµР В»Р В°

    auto isRefKw = [&](size_t idx) {
        return idx > 0 && tokens[idx-1].type == KEYWORD
            && tokens[idx-1].value.to_utf8() == "ref";
    };

    for (size_t i = 0; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        if (t.type == DIRECTIVE || t.type == COMMENT || t.type == MULTI_COMMENT
                || t.type == END_OF_FILE || t.type == DUMMY)
            continue;
        size_t off = tokenByteOffset(t, lineStarts);

        if (t.type == BRACE_LEFT_CURVE) {
            scopes.push_back(Scope{});
            if (!pendingFor.empty()) {
                // Р С—Р ВµРЎР‚Р Р†РЎвЂ№Р в„– '{' Р С—Р С•РЎРѓР В»Р Вµ 'for' РІР‚вЂќ РЎвЂљР ВµР В»Р С• РЎвЂ Р С‘Р С”Р В»Р В°: Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…Р В°РЎРЏ Р В¶Р С‘Р Р†РЎвЂРЎвЂљ Р Т‘Р С• Р ВµР С–Р С• Р С”Р С•Р Р…РЎвЂ Р В°
                for (auto& pf : pendingFor) {
                    decls.push_back(pf);
                    scopes.back().varIdx.push_back(decls.size() - 1);
                }
                pendingFor.clear();
            }
            continue;
        }
        if (t.type == BRACE_RIGHT_CURVE) {
            if (!scopes.empty()) {
                size_t closeOff = off + t.position.size;
                for (size_t vi : scopes.back().varIdx) decls[vi].visEnd = closeOff;
                scopes.pop_back();
            }
            continue;
        }
        if (t.type != KEYWORD || off >= pos) continue;
        const std::string kw = t.value.to_utf8();

        if (kw == "let" && i + 1 < tokens.size()
                && tokens[i + 1].type == LITERAL) {
            const Token& nm = tokens[i + 1];
            size_t declOff = tokenByteOffset(nm, lineStarts);
            if (declOff >= pos) continue;
            Decl d;
            d.name = nm.value.to_utf8();
            if (d.name.empty()) continue;
            d.kind = KIND_VARIABLE;
            if (isRefKw(i)) d.detail = "ref";
            size_t s = i + 2;
            if (s < tokens.size() && tokens[s].type == OPERATOR
                    && tokens[s].value.to_utf8() == ":") {
                s++;
                while (s < tokens.size()
                       && tokens[s].type != LITERAL
                       && tokens[s].type != KEYWORD
                       && tokens[s].type != STRING)
                    s++;
                if (s < tokens.size()) {
                    std::string annotated = tokens[s].value.to_utf8();
                    if (!d.detail.empty() && d.detail != "ref") annotated = d.detail + " " + annotated;
                    else if (d.detail == "ref") annotated = "ref " + annotated;
                    d.detail = annotated;
                }
            }
            // Р вЂР ВµР В· type-Р В°Р Р…Р Р…Р С•РЎвЂљР В°РЎвЂ Р С‘Р С‘ Р Р†РЎвЂ№Р Р†Р С•Р Т‘Р С‘Р С Р С‘Р СРЎРЏ РЎвЂљР С‘Р С—Р В° Р С‘Р В· РЎРѓРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚Р Р…Р С•Р С–Р С• Р В»Р С‘РЎвЂљР ВµРЎР‚Р В°Р В»Р В°/Р С‘Р Р…РЎРѓРЎвЂљР В°Р Р…РЎРѓР В°
            // РЎРѓР С—РЎР‚Р В°Р Р†Р В°: `let v = Vec[i64, -1] {}` / `let p = Point { x: 1 }` -> "Vec"/"Point"
            if (d.detail.empty()) {
                size_t e = i + 2;
                while (e < tokens.size()
                       && !(tokens[e].type == OPERATOR
                            && tokens[e].value.to_utf8() == "="))
                    e++;
                if (e < tokens.size()) {
                    size_t r = e + 1;
                    if (r < tokens.size() && tokens[r].type == KEYWORD
                            && tokens[r].value.to_utf8() == "new") r++;
                    if (r < tokens.size() && tokens[r].type == LITERAL) {
                        size_t b = r + 1;
                        if (b < tokens.size()
                                && (tokens[b].type == BRACE_LEFT_CURVE
                                    || tokens[b].type == BRACE_LEFT_RECT))
                            d.detail = tokens[r].value.to_utf8();
                    }
                }
            }
            d.declOff = declOff;
            d.declKey = ((unsigned long long)(unsigned int)nm.position.line << 32)
                        | (unsigned int)nm.position.index;
            d.visEnd = 0;
            decls.push_back(d);
            if (!scopes.empty()) scopes.back().varIdx.push_back(decls.size() - 1);
            continue;
        }

        if (kw == "for") {
            // Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…Р В°РЎРЏ РЎвЂ Р С‘Р С”Р В»Р В°: Р С—Р ВµРЎР‚Р Р†РЎвЂ№Р в„– Р С‘Р Т‘Р ВµР Р…РЎвЂљР С‘РЎвЂћР С‘Р С”Р В°РЎвЂљР С•РЎР‚ Р Р† Р В·Р В°Р С–Р С•Р В»Р С•Р Р†Р С”Р Вµ (РЎвЂљР С•Р в„– Р В¶Р Вµ РЎРѓРЎвЂљРЎР‚Р С•Р С”Р С‘)
            size_t s = i + 1;
            while (s < tokens.size()
                   && tokens[s].type != LITERAL
                   && tokens[s].type != BRACE_LEFT_CURVE
                   && tokens[s].type != BRACE_RIGHT_CURVE
                   && tokens[s].position.line == t.position.line)
                s++;
            if (s < tokens.size() && tokens[s].type == LITERAL) {
                Decl d;
                d.name = tokens[s].value.to_utf8();
                if (d.name.empty()) continue;
                d.kind = KIND_VARIABLE;
                size_t r = s + 1;
                if (r < tokens.size() && tokens[r].type == OPERATOR
                        && tokens[r].value.to_utf8() == ":") {
                    r++;
                    while (r < tokens.size()
                           && tokens[r].type != LITERAL
                           && tokens[r].type != KEYWORD
                           && tokens[r].type != STRING)
                        r++;
                    if (r < tokens.size()) d.detail = tokens[r].value.to_utf8();
                }
                d.declOff = tokenByteOffset(tokens[s], lineStarts);
                d.declKey = ((unsigned long long)(unsigned int)tokens[s].position.line << 32)
                            | (unsigned int)tokens[s].position.index;
                d.visEnd = 0;
                pendingFor.push_back(d);
            }
        }
    }

    for (const auto& d : decls) {
        if (d.declOff >= pos) continue;
        if (d.visEnd != 0 && d.visEnd < pos) continue;
        if (out.count(d.name)) continue;
        out[d.name] = LocalCandidate{d.name, d.kind, d.detail, d.declKey};
        if (declPositions)
            (*declPositions).emplace(d.name, d.declOff);
    }
}

// Р РЋРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚РЎвЂ№: РЎР‚Р ВµР Р…Р Т‘Р ВµРЎР‚ Р Т‘Р ВµР С”Р В»Р В°РЎР‚Р В°РЎвЂ Р С‘Р в„– (Р С•Р С—РЎР‚Р ВµР Т‘Р ВµР В»РЎвЂР Р… Р Р…Р С‘Р В¶Р Вµ, Р Т‘Р С• contextHover).
static std::string nodeTypeText(Node* n);
static std::string structFieldText(const StructFieldDef& fd);
static std::string methodSignature(const Function& f);
static std::string funcSignature(const Function& f);
static Type reifyMethodReturn(const StructSymbol& sym, const Function& f, const Type& inst,
                              const std::map<String, StructSymbol>& structs);

static JsonValue buildCompletion(const string& text, const string& path, int lspLine, int lspChar) {
    JsonValue items = JArray();

    ErrorContainer ec;
    auto tokens = fastLex(text, path);
    Preprocessor pp(false, ec);
    auto result = pp(tokens);
    ASTGenerator ast(result, pp.source_tokens, false, ec);
    auto tree = ast();
    auto sa = ASTAnalizer(tree, ec);
    sa();
    ASTDeepAnalyzer da(tree, ec);
    if (!g_noDeepAnalysis) da();

    auto addItem = [&items](const string& label, int kind, const string& detail) {
        JsonValue it = JObject();
        it.object["label"] = JString(label);
        it.object["kind"]  = JNumber(kind);
        if (!detail.empty()) it.object["detail"] = JString(detail);
        items.array.push_back(it);
    };

    size_t pos = lspPosToByteOffset(text, lspLine, lspChar);
    std::vector<string> owner;
    string prefix;
    bool scoped = false;
    std::vector<bool> callFlags;
    bool isMember = memberAccessContext(text, pos, owner, prefix, &scoped, &callFlags);

    auto matchPrefix = [&prefix](const string& name) {
        if (prefix.empty()) return true;
        if (name.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i)
            if (std::tolower((unsigned char)name[i]) != std::tolower((unsigned char)prefix[i]))
                return false;
        return true;
    };

    auto modsString = [](const ContextObject::Modifiers& m) {
        string s;
        if (m.Const)   s += "const ";
        if (m.Strict)  s += "strict ";
        if (m.Static)  s += "static ";
        if (m.Final)   s += "final ";
        if (m.Private) s += "private ";
        if (m.Ref)     s += "ref ";
        return s;
    };

    auto addContextObjects = [&](Context& ctx,
                                 const std::map<std::string, LocalCandidate>* locals = nullptr) {
        for (auto& kv : ctx.variables) {
            const string& name = kv.first.to_utf8();
            if (!matchPrefix(name)) continue;
            if (locals && locals->count(name)) continue;  // Р В»Р С•Р С”Р В°Р В»РЎРЉР Р…Р В°РЎРЏ Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…Р В°РЎРЏ Р С—Р ВµРЎР‚Р ВµР С”РЎР‚РЎвЂ№Р Р†Р В°Р ВµРЎвЂљ
            ContextObject& co = kv.second;
            string mods = modsString(co.modifiers);
            if (co.value.T.isCallable()) {
                std::string detail;
                try {
                    FunctionContainer& fc = co.value.get_ref<FunctionContainer>();
                    if (!fc.functions.empty()) detail = funcSignature(fc.functions[0]);
                } catch (...) {}
                if (detail.empty()) detail = co.value.T.toString().to_utf8();
                addItem(name, KIND_FUNCTION, mods + detail);
            }
            else if (co.value.T == Ty::Namespace)
                addItem(name, KIND_MODULE, mods + "namespace");
            else if (co.value.T == Ty::Type_)
                addItem(name, KIND_TYPE, "type");
            else
                addItem(name, KIND_VARIABLE, mods + co.value.T.toString().to_utf8());
        }
    };

    if (isMember && !owner.empty()) {
        // Р В Р ВµР В·Р С•Р В»Р Р†Р С‘Р С owner-РЎвЂ Р ВµР С—Р С•РЎвЂЎР С”РЎС“: namespace -> namespace (ctx), Р В»Р С‘Р В±Р С• РЎвЂЎР В»Р ВµР Р… РЎРѓРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚РЎвЂ№
        // (Р С—Р С•Р В»РЎРЏ РЎРѓРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚Р Р…Р С•Р С–Р С• РЎвЂљР С‘Р С—Р В° / Р СР ВµРЎвЂљР С•Р Т‘РЎвЂ№). Р вЂєР С•Р С”Р В°Р В»РЎРЉР Р…РЎвЂ№Р Вµ Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…РЎвЂ№Р Вµ (РЎРѓ type-Р В°Р Р…Р Р…Р С•РЎвЂљР В°РЎвЂ Р С‘Р ВµР в„–
        // Р С‘Р В»Р С‘ РЎРѓРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚Р Р…РЎвЂ№Р С Р В»Р С‘РЎвЂљР ВµРЎР‚Р В°Р В»Р С•Р С РЎРѓР С—РЎР‚Р В°Р Р†Р В°) РЎвЂљР С•Р В¶Р Вµ РЎС“РЎвЂЎР В°РЎРѓРЎвЂљР Р†РЎС“РЎР‹РЎвЂљ.
        std::vector<size_t> lineStarts = buildLineStarts(text);
        std::map<std::string, LocalCandidate> locals;
        collectParamsInRange(tokens, lineStarts, pos, locals);
        collectLetVars(tokens, lineStarts, pos, locals);

        Context* ctx = &da.global_context;
        const StructSymbol* curSym = nullptr;
        const EnumSymbol* curEnum = nullptr;
        bool structMode = false;
        bool enumMode = false;
        bool ok = true;
        Type inst = Ty::Any;
        for (size_t oi = 0; oi < owner.size(); ++oi) {
            const string& nm = owner[oi];
            const bool isCall = callFlags.size() > oi && callFlags[oi];
            if (structMode) {
                if (isCall) {
                    // РЎР ВµР С–Р СР ВµР Р…РЎвЂљ-Р Р†РЎвЂ№Р В·Р С•Р Р†: РЎР‚Р ВµР С‘РЎвЂћР С‘РЎвЂ Р С‘РЎР‚РЎС“Р ВµР С РЎвЂљР С‘Р С— Р Р†Р С•Р В·Р Р†РЎР‚Р В°РЎвЂљР В° Р СР ВµРЎвЂљР С•Р Т‘Р В° Р С—Р С•Р Т‘ Р С‘Р Р…РЎРѓРЎвЂљР В°Р Р…РЎРѓР С•Р С inst
                    if (!curSym) { ok = false; break; }
                    auto mit = curSym->methods.find(String(nm));
                    if (mit == curSym->methods.end() || mit->second.functions.empty()) { ok = false; break; }
                    Type rt = reifyMethodReturn(*curSym, mit->second.functions[0], inst, da.structs);
                    auto sit = da.structs.find(rt.name);
                    if (!rt.isStruct() || sit == da.structs.end()) { ok = false; break; }
                    curSym = &sit->second;
                    inst = rt;
                    continue;
                }
                const StructFieldDef* fd =
                    curSym ? curSym->field_def(String(nm)) : nullptr;
                std::string tn = fd ? nodeTypeText(fd->type_node) : "";
                auto sit = da.structs.find(String(tn));
                if (!tn.empty() && sit != da.structs.end()) { curSym = &sit->second; inst = Type::StructType(String(tn)); continue; }
                ok = false; break;
            }
            String key(nm.c_str());
            auto vit = ctx->variables.find(key);
            if (vit != ctx->variables.end()) {
                ContextObject& obj = vit->second;
                if (obj.value.T == Ty::Namespace) { ctx = &obj.value.get_ref<Namespace>().context; continue; }
                auto byName = [&da](const String& n) -> const StructSymbol* {
                    auto sit = da.structs.find(n);
                    return sit == da.structs.end() ? nullptr : &sit->second;
                };
                if (obj.value.T == Ty::Type_ && obj.is_type_decl && obj.value.get<Type>().isStruct()) {
                    inst = obj.value.get<Type>();
                    curSym = byName(inst.name);
                    if (curSym) { structMode = true; continue; }
                } else if (obj.value.T == Ty::Type_ && obj.is_type_decl && obj.value.get<Type>().isEnum()) {
                    auto eit = da.enums.find(obj.value.get<Type>().name);
                    if (eit != da.enums.end()) { curEnum = &eit->second; enumMode = true; continue; }
                } else if (obj.value.T.isStruct()) {
                    inst = obj.value.T;
                    curSym = byName(inst.name);
                    if (curSym) { structMode = true; continue; }
                }
                ok = false; break;
            }
            auto lit = locals.find(nm);
            if (lit != locals.end()) {
                // P8: РїРѕР»РЅС‹Р№ СЂРµРёС„РёС†РёСЂРѕРІР°РЅРЅС‹Р№ С‚РёРї Р»РѕРєР°Р»СЊРЅРѕР№ РёР· DeepAnalizer
                auto ltt = da.local_types.find(lit->second.declKey);
                if (ltt != da.local_types.end()) {
                    inst = ltt->second;
                    auto st = da.structs.find(inst.name);
                    if (st != da.structs.end()) { curSym = &st->second; structMode = true; continue; }
                }
                std::string dt = lit->second.detail;
                if (!dt.empty()) {
                    auto sit = da.structs.find(String(dt));
                    if (sit != da.structs.end()) { curSym = &sit->second; inst = Type::StructType(String(dt)); structMode = true; continue; }
                }
            }
            ok = false; break;
        }
        if (ok && enumMode && curEnum) {
            // enum: РІР°СЂРёР°РЅС‚С‹ (`Types::a`, `Types::b`, ...)
            for (const auto& kv : curEnum->enum_context.variables) {
                std::string nm = kv.first.to_utf8();
                if (!matchPrefix(nm)) continue;
                std::string detail = curEnum->name.to_utf8() + "::" + nm;
                const ContextObject& vco = kv.second;
                if (vco.value.raw().type() == typeid(long long))
                    detail += " = " + std::to_string(std::any_cast<long long>(vco.value.raw()));
                addItem(nm, KIND_ENUM_MEMBER, detail);
            }
        } else if (ok && structMode && curSym) {
            // '::' РІР‚вЂќ РЎРѓРЎвЂљР В°РЎвЂљР С‘РЎвЂЎР ВµРЎРѓР С”Р С‘Р в„– Р Т‘Р С•РЎРѓРЎвЂљРЎС“Р С—: РЎвЂљР С•Р В»РЎРЉР С”Р С• Р СР ВµРЎвЂљР С•Р Т‘РЎвЂ№; '.' РІР‚вЂќ Р С—Р С•Р В»РЎРЏ + Р СР ВµРЎвЂљР С•Р Т‘РЎвЂ№
            if (!scoped) {
                for (const StructFieldDef& fd : curSym->fields) {
                    if (fd.mods.Private) continue;
                    std::string nm = fd.name.to_utf8();
                    if (!matchPrefix(nm)) continue;
                    addItem(nm, KIND_VARIABLE, structFieldText(fd));
                }
            }
            // `::` адресует статические поля структуры (`Test::count`): они лежат
            // в decl_context, а не в экземпляре, и по `.` не адресуются.
            if (scoped) {
                for (const StructFieldDef& fd : curSym->fields) {
                    if (fd.mods.Private || !fd.mods.Static) continue;
                    std::string nm = fd.name.to_utf8();
                    if (!matchPrefix(nm)) continue;
                    addItem(nm, KIND_VARIABLE, curSym->name.to_utf8() + "::"
                                             + structFieldText(fd));
                }
            }
            for (const auto& kv : curSym->methods)
                for (const Function& fn : kv.second.functions) {
                    // статический метод адресуется только через `Struct::`,
                    // обычный — только через экземпляр
                    if (scoped != (bool)fn.is_static) continue;
                    std::string nm = fn.name.to_utf8();
                    if (!matchPrefix(nm)) continue;
                    addItem(nm, KIND_FUNCTION, methodSignature(fn));
                }
        } else if (ok && ctx) {
            addContextObjects(*ctx);
        }
    } else {
        // Р вЂєР С•Р С”Р В°Р В»РЎРЉР Р…РЎвЂ№Р Вµ Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…РЎвЂ№Р Вµ/Р С—Р В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚РЎвЂ№ РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘, Р ВµРЎРѓР В»Р С‘ Р С”РЎС“РЎР‚РЎРѓР С•РЎР‚ Р Р†Р Р…РЎС“РЎвЂљРЎР‚Р С‘ РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘
        std::vector<size_t> lineStarts = buildLineStarts(text);
        std::map<std::string, LocalCandidate> locals;
        collectParamsInRange(tokens, lineStarts, pos, locals);
        collectLetVars(tokens, lineStarts, pos, locals);
        for (const auto& kv : locals) {
            if (!matchPrefix(kv.first)) continue;
            // P8: полный реифицированный тип локальной из DeepAnalyzer важнее
            // текстовой эвристики по токенам: она даёт «static_array» без
            // дженериков для `let x = static_array[...]` и пустую подпись для
            // вызова-конструктора `let x = static_array(...)`. Анализатор знает
            // тип точно (`static_array[char, 5]`).
            std::string det = kv.second.detail;
            auto ltt = da.local_types.find(kv.second.declKey);
            if (ltt != da.local_types.end()
                    && !ltt->second.isAny() && !ltt->second.isAuto())
                det = ltt->second.toString().to_utf8();
            addItem(kv.first, kv.second.kind, det);
        }
        addContextObjects(da.global_context, &locals);
    }

    // РћРїРµСЂР°С‚РѕСЂРЅС‹Рµ (dunder) РїРµСЂРµРіСЂСѓР·РєРё: РїРѕРґСЃРєР°Р·С‹РІР°РµРј РёРјРµРЅР° РїСЂРё РІРІРѕРґРµ `fn __...`
    // (РІРЅРµ member-РґРѕСЃС‚СѓРїР°, РµСЃР»Рё РїСЂРµС„РёРєСЃ СѓР¶Рµ РЅР°С‡РёРЅР°РµС‚СЃСЏ СЃ '_').
    if (!isMember && !prefix.empty() && prefix[0] == '_') {
        static const std::vector<std::pair<string, string>> dunders = {
            {"__add__",     "operator: a + b"},
            {"__sub__",     "operator: a - b"},
            {"__mul__",     "operator: a * b"},
            {"__truediv__", "operator: a / b"},
            {"__mod__",     "operator: a % b"},
            {"__lshift__",  "operator: a << b"},
            {"__rshift__",  "operator: a >> b"},
            {"__and__",     "operator: a & b"},
            {"__or__",      "operator: a | b"},
            {"__xor__",     "operator: a ^ b"},
            {"__eq__",      "operator: a == b"},
            {"__ne__",      "operator: a != b"},
            {"__lt__",      "operator: a < b"},
            {"__le__",      "operator: a <= b"},
            {"__gt__",      "operator: a > b"},
            {"__ge__",      "operator: a >= b"},
            {"__neg__",     "operator: -a (unary)"},
            {"__pos__",     "operator: +a (unary)"},
            {"__bool__",    "operator: !a (bool)"},
            {"__invert__",  "operator: ~a (unary)"},
            {"__get_index__", "indexing: obj[i]"},
            {"__set_index__", "indexing: obj[i] = v"},
            {"__assign__",  "assignment: obj = v"},
            {"__get__",     "read: auto-unpack obj to its value"},
            {"__call__",    "invocation: obj(args)"},
            {"__typeof__", "type query: typeof(obj)"},
            {"__sizeof__", "size query: sizeof(obj)"},
            {"__del__", "deletion: del obj, i"},
            {"__as__", "cast: expr as Type"},
        };
        for (const auto& d : dunders)
            if (matchPrefix(d.first))
                addItem(d.first, KIND_FUNCTION, d.second);
    }

    // #define / #macro Р С‘Р В· Р С—РЎР‚Р ВµР С—РЎР‚Р С•РЎвЂ Р ВµРЎРѓРЎРѓР С•РЎР‚Р В° РІР‚вЂќ РЎРѓР В°Р СР С‘ Р Т‘Р С‘РЎР‚Р ВµР С”РЎвЂљР С‘Р Р†РЎвЂ№ РЎС“Р В¶Р Вµ РЎР‚Р В°РЎРѓР С”РЎР‚РЎвЂ№РЎвЂљРЎвЂ№ Р Р† Р С”Р С•Р Т‘Р Вµ,
    // Р Р…Р С• Р С‘Р СР ВµР Р…Р В° Р СР В°Р С”РЎР‚Р С•РЎРѓР С•Р Р† Р Т‘Р С•Р В»Р В¶Р Р…РЎвЂ№ Р С—РЎР‚Р ВµР Т‘Р В»Р В°Р С–Р В°РЎвЂљРЎРЉРЎРѓРЎРЏ Р Р† Р В°Р Р†РЎвЂљР С•Р Т‘Р С•Р С—Р С•Р В»Р Р…Р ВµР Р…Р С‘Р С‘.
    if (!isMember) {
        auto joinTokens = [](const std::vector<Token>& ts) {
            string s;
            for (const auto& t : ts) {
                if (!s.empty()) s += " ";
                s += t.value.to_utf8();
            }
            return s;
        };
        for (const auto& d : pp.defines) {
            const string name = d.name_token.value.to_utf8();
            if (!matchPrefix(name) || da.global_context.has_variable(String(name)))
                continue;
            addItem(name, KIND_DEFINE,
                    "#define  " + name + " = " + joinTokens(d.tokens));
        }
        for (const auto& m : pp.macros) {
            const string name = m.name_token.value.to_utf8();
            if (!matchPrefix(name) || da.global_context.has_variable(String(name)))
                continue;
            string args;
            for (const auto& a : m.args_tokens) {
                if (!args.empty()) args += ", ";
                args += a.token.value.to_utf8();
            }
            addItem(name, KIND_MACRO, "#macro " + name + "(" + args + ")");
        }
    }

    // Р С™Р В»РЎР‹РЎвЂЎР ВµР Р†РЎвЂ№Р Вµ РЎРѓР В»Р С•Р Р†Р В° / РЎвЂљР С‘Р С—РЎвЂ№ / Р В»Р С‘РЎвЂљР ВµРЎР‚Р В°Р В»РЎвЂ№ РІР‚вЂќ РЎвЂљР С•Р В»РЎРЉР С”Р С• Р Р…Р В° Р С–Р В»Р С•Р В±Р В°Р В»РЎРЉР Р…Р С•Р С РЎС“РЎР‚Р С•Р Р†Р Р…Р Вµ
    // (РЎвЂЎР В»Р ВµР Р…РЎвЂ№ namespace Р С‘Р В·Р В»Р С‘РЎв‚¬Р Р…Р С‘, РЎвЂљ.Р С”. Р С‘РЎвЂ¦ Р Р…Р В°Р В·Р Р†Р В°Р Р…Р С‘РЎРЏ Р Р…Р Вµ Р С—Р ВµРЎР‚Р ВµРЎРѓР ВµР С”Р В°РЎР‹РЎвЂљРЎРѓРЎРЏ РЎРѓ Р С”Р В»РЎР‹РЎвЂЎР ВµР Р†РЎвЂ№Р СР С‘)
    if (!isMember) {
        struct StaticItem { const char* name; int kind; };
        static const StaticItem kStatics[] = {
            {"if",14},{"else",14},{"for",14},{"while",14},{"let",14},{"in",14},
            {"and",14},{"or",14},{"echo",14},{"ret",14},{"assert",14},{"lambda",14},
            {"do",14},{"is",14},{"as",14},{"struct",14},{"enum",14},{"namespace",14},{"fn",14},
            {"continue",14},{"break",14},
            {"const",KIND_MODIFIER},{"static",KIND_MODIFIER},{"global",KIND_MODIFIER},{"final",KIND_MODIFIER},{"private",KIND_MODIFIER},
            {"shadow",KIND_MODIFIER},{"strict",KIND_MODIFIER},{"ref",KIND_MODIFIER},
            {"new",14},{"del",14},{"typeof",14},{"sizeof",14},{"ptr",14},{"out",14},{"outln",14},
            {"input",14},{"exit",14},{"append",14},{"reserve",14},{"typeis",14},
            {"mod",14},{"eval",14},
            {"str",7},{"char",7},{"bool",7},{"Type",7},{"Namespace",7},{"Func",7},
            {"Lambda",7},{"auto",7},{"Null",7},{"any",7},
            {"i8",7},{"i16",7},{"i32",7},{"i64",7},{"f16",7},{"f32",7},{"f64",7},
            {"Array",7},{"Tuple",7},
            {"true",21},{"false",21},{"null",21},{"self",21},{"this",21},
            {"__LINE__",21},{"__FILE__",21},{"__LUMEN__",21},
            {"#define",KIND_DIRECTIVE},{"#macro",KIND_DIRECTIVE},{"#ifdef",KIND_DIRECTIVE},{"#elifdef",KIND_DIRECTIVE},{"#undef",KIND_DIRECTIVE},
            {"#else",KIND_DIRECTIVE},{"#ifndef",KIND_DIRECTIVE},{"#include",KIND_DIRECTIVE},
            {"#cppinclude",KIND_DIRECTIVE},{"#pragma",KIND_DIRECTIVE},
        };
        for (const auto& si : kStatics)
            if (matchPrefix(si.name) && !da.global_context.has_variable(String(si.name)))
                addItem(si.name, si.kind, "");
    }

    return items;
}

// ---------- Hover (textDocument/hover) ----------

struct KeywordDoc {
    std::string name;       // Р С”Р В»РЎР‹РЎвЂЎР ВµР Р†Р С•Р Вµ РЎРѓР В»Р С•Р Р†Р С• / РЎвЂљР С‘Р С— / Р В»Р С‘РЎвЂљР ВµРЎР‚Р В°Р В»
    std::string summary;    // Р С”РЎР‚Р В°РЎвЂљР С”Р С•Р Вµ Р С•Р С—Р С‘РЎРѓР В°Р Р…Р С‘Р Вµ (markdown)
    std::string example;    // Р С—РЎР‚Р С‘Р СР ВµРЎР‚ Р С‘РЎРѓР С—Р С•Р В»РЎРЉР В·Р С•Р Р†Р В°Р Р…Р С‘РЎРЏ (Р СР С•Р В¶Р ВµРЎвЂљ Р В±РЎвЂ№РЎвЂљРЎРЉ Р С—РЎС“РЎРѓРЎвЂљР С•Р в„–)
};

// Р вЂ”Р В°Р С–РЎР‚РЎС“Р В¶Р ВµР Р…Р Р…РЎвЂ№Р Вµ Р С•Р С—Р С‘РЎРѓР В°Р Р…Р С‘РЎРЏ Р С”Р В»РЎР‹РЎвЂЎР ВµР Р†РЎвЂ№РЎвЂ¦ РЎРѓР В»Р С•Р Р† (РЎвЂћР В°Р в„–Р В» keywords.md РЎР‚РЎРЏР Т‘Р С•Р С РЎРѓ exe).
static std::map<std::string, KeywordDoc> g_keywordDocs;

// РџР°РїРєР° РґРѕРєСѓРјРµРЅС‚РѕРІ СЂСЏРґРѕРј СЃ exe (РёР»Рё "keywords" РІ С‚РµРєСѓС‰РµРј РєР°С‚Р°Р»РѕРіРµ).
static std::string keywordDocsDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return "keywords";
    std::string p(buf, len);
    size_t slash = p.find_last_of("/\\");
    return (slash == std::string::npos) ? "keywords" : p.substr(0, slash) + "/keywords";
#else
    return "keywords";
#endif
}

// РџР°СЂСЃРёС‚ РѕРґРёРЅ С„Р°Р№Р»: РїРµСЂРІР°СЏ СЃС‚СЂРѕРєР° `# <РёРјСЏ>` / `## <РёРјСЏ>` вЂ” РєР»СЋС‡, РІСЃС‘ РѕСЃС‚Р°Р»СЊРЅРѕРµ
// С‚РµР»Р° С„Р°Р№Р»Р° СЃРѕС…СЂР°РЅСЏРµС‚СЃСЏ РєР°Рє РµСЃС‚СЊ (РїРѕСЂСЏРґРѕРє В«С‚РµРєСЃС‚ в†’ ```lumen-Р±Р»РѕРє в†’ С‚РµРєСЃС‚В»
// РЅРµ РјРµРЅСЏРµС‚СЃСЏ: РєРѕРґРѕРІС‹Рµ РІСЃС‚Р°РІРєРё РѕСЃС‚Р°СЋС‚СЃСЏ РѕС‚РґРµР»СЊРЅС‹РјРё Рё СЂРµРЅРґРµСЂСЏС‚СЃСЏ СЂР°Р·РґРµР»СЊРЅРѕ).
static void loadKeywordDocFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;
    KeywordDoc cur;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (cur.name.empty() && line.size() >= 2
            && line[0] == '#' && (line[1] == ' ' || (line.size() >= 3 && line[1] == '#' && line[2] == ' '))) {
            size_t off = (line.size() >= 3 && line[1] == '#') ? 3 : 2;
            std::string n = line.substr(off);
            while (!n.empty() && (n.back() == ' ' || n.back() == '\t')) n.pop_back();
            cur.name = n;
            continue;
        }
        if (cur.name.empty()) continue;
        cur.summary += line + "\n";
    }
    while (!cur.summary.empty() && (cur.summary.back() == '\n' || cur.summary.back() == ' '))
        cur.summary.pop_back();
    if (!cur.name.empty())
        g_keywordDocs[cur.name] = std::move(cur);
}

// Р—Р°РіСЂСѓР¶Р°РµС‚ РІСЃРµ keywords/*.md СЂСЏРґРѕРј СЃ exe.
static void loadKeywordDocs(const std::string& dir) {
    g_keywordDocs.clear();
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.md").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            loadKeywordDocFile(dir + "\\" + fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 3 && n.compare(n.size() - 3, 3, ".md") == 0)
            loadKeywordDocFile(dir + "/" + n);
    }
    closedir(d);
#endif
}

#ifdef _WIN32
#include <windows.h>
// Р СџРЎС“РЎвЂљРЎРЉ Р С” РЎвЂћР В°Р в„–Р В»РЎС“ Р С•Р С—Р С‘РЎРѓР В°Р Р…Р С‘Р в„– РЎР‚РЎРЏР Т‘Р С•Р С РЎРѓ Р С‘РЎРѓР С—Р С•Р В»Р Р…РЎРЏР ВµР СРЎвЂ№Р С РЎвЂћР В°Р в„–Р В»Р С•Р С.
static std::string keywordDocsPath() {
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return "keywords.md";
    std::string p(buf, len);
    size_t slash = p.find_last_of("/\\");
    return (slash == std::string::npos) ? "keywords.md" : p.substr(0, slash) + "/keywords.md";
}
#else
static std::string keywordDocsPath() { return "keywords.md"; }
#endif

// Р СџР В°РЎР‚РЎРѓР С‘РЎвЂљ keywords.md РЎвЂћР С•РЎР‚Р СР В°РЎвЂљР В°:
//   ## <Р С‘Р СРЎРЏ>
//   <markdown-Р С•Р С—Р С‘РЎРѓР В°Р Р…Р С‘Р Вµ>
//   ```lumen
//   <Р С—РЎР‚Р С‘Р СР ВµРЎР‚>
//   ```
// Р РЋР ВµР С”РЎвЂ Р С‘Р С‘ РЎР‚Р В°Р В·Р Т‘Р ВµР В»РЎРЏРЎР‹РЎвЂљРЎРѓРЎРЏ "## ". Р СњР В°Р В·Р Р†Р В°Р Р…Р С‘РЎРЏ РЎРѓР ВµР С”РЎвЂ Р С‘Р в„– = Р С”Р В»РЎР‹РЎвЂЎР ВµР Р†РЎвЂ№Р Вµ РЎРѓР В»Р С•Р Р†Р В°/РЎвЂљР С‘Р С—РЎвЂ№/Р В»Р С‘РЎвЂљР ВµРЎР‚Р В°Р В»РЎвЂ№.
static void loadKeywordDocsFile(const std::string& path) {
    g_keywordDocs.clear();
    std::ifstream in(path);
    if (!in) return;

    std::string line;
    KeywordDoc cur;
    bool in_entry = false;
    bool in_code = false;

    auto flush = [&]() {
        if (!in_entry) return;
        // РЎРѓРЎР‚Р ВµР В·Р В°Р ВµР С РЎвЂ¦Р Р†Р С•РЎРѓРЎвЂљР С•Р Р†РЎвЂ№Р Вµ Р С—РЎС“РЎРѓРЎвЂљРЎвЂ№Р Вµ РЎРѓРЎвЂљРЎР‚Р С•Р С”Р С‘ Р С‘Р В· summary
        while (!cur.summary.empty() && (cur.summary.back() == '\n' || cur.summary.back() == ' '))
            cur.summary.pop_back();
        if (!cur.name.empty())
            g_keywordDocs[cur.name] = std::move(cur);
        cur = KeywordDoc();
        in_entry = false;
    };

    while (std::getline(in, line)) {
        // РЎРѓРЎР‚Р ВµР В·Р В°РЎвЂљРЎРЉ \r Р Т‘Р В»РЎРЏ CRLF
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.rfind("## ", 0) == 0) {
            flush();
            in_entry = true;
            cur.name = line.substr(3);
            in_code = false;
        } else if (in_entry && !in_code && line.compare(0, 8, "```lumen") == 0) {
            in_code = true;
        } else if (in_entry && in_code && line == "```") {
            in_code = false;
        } else if (in_entry) {
            if (in_code) {
                if (!cur.example.empty()) cur.example += "\n";
                cur.example += line;
            } else {
                cur.summary += line + "\n";
            }
        }
    }
    flush();
}

// --- Range РЎРѓР В»Р С•Р Р†Р В° Р С—Р С•Р Т‘ Р С—Р С•Р В·Р С‘РЎвЂ Р С‘Р ВµР в„– iniLSP-Р С”Р С•Р С•РЎР‚Р Т‘Р С‘Р Р…Р В°РЎвЂљР В°РЎвЂ¦ ---

// Р СњР В°РЎвЂ¦Р С•Р Т‘Р С‘РЎвЂљ РЎРѓР В»Р С•Р Р†Р С• (Р С‘Р Т‘Р ВµР Р…РЎвЂљР С‘РЎвЂћР С‘Р С”Р В°РЎвЂљР С•РЎР‚) Р Р†Р С•Р С”РЎР‚РЎС“Р С– Р В±Р В°Р в„–РЎвЂљР С•Р Р†Р С•Р в„– Р С—Р С•Р В·Р С‘РЎвЂ Р С‘Р С‘ pos.
// Р вЂ™Р С•Р В·Р Р†РЎР‚Р В°РЎвЂ°Р В°Р ВµРЎвЂљ false, Р ВµРЎРѓР В»Р С‘ Р С—Р С•Р Т‘ Р С”РЎС“РЎР‚РЎРѓР С•РЎР‚Р С•Р С Р Р…Р Вµ Р С‘Р Т‘Р ВµР Р…РЎвЂљР С‘РЎвЂћР С‘Р С”Р В°РЎвЂљР С•РЎР‚.
static bool wordAt(const std::string& text, size_t pos,
                   size_t& start, size_t& end, string& word) {
    if (pos > text.size()) return false;
    size_t s = pos;
    while (s > 0 && isIdentChar(text[s-1])) s--;
    size_t e = pos;
    while (e < text.size() && isIdentChar(text[e])) e++;
    if (s == e) return false;
    start = s; end = e;
    word = text.substr(s, e - s);
    return true;
}

// Р вЂР В°Р в„–РЎвЂљР С•Р Р†Р С•Р Вµ РЎРѓР СР ВµРЎвЂ°Р ВµР Р…Р С‘Р Вµ -> LSP-Р С—Р С•Р В·Р С‘РЎвЂ Р С‘РЎРЏ {line, character(UTF-16)}.
static JsonValue positionFromByte(const std::string& text, const std::vector<size_t>& lineStarts, size_t byte) {
    JsonValue pos = JObject();
    int line = 0;
    for (size_t i = 1; i < lineStarts.size(); ++i) {
        if (lineStarts[i] <= byte) line = (int)i; else break;
    }
    size_t ls = lineStarts[(size_t)line];
    // РЎРѓРЎвЂЎР С‘РЎвЂљР В°Р ВµР С UTF-16 Р ВµР Т‘Р С‘Р Р…Р С‘РЎвЂ РЎвЂ№ Р Т‘Р С• byte
    size_t utf16 = 0, i = ls;
    while (i < byte && i < text.size()) {
        unsigned char c = (unsigned char)text[i];
        int cl;
        if (c < 0x80) cl = 1;
        else if ((c & 0xE0) == 0xC0) cl = 2;
        else if ((c & 0xF0) == 0xE0) cl = 3;
        else if ((c & 0xF8) == 0xF0) cl = 4;
        else { cl = 1; }
        if ((cl >= 4 && i + 1 == byte) || cl == 1) {
            if (cl >= 4) { utf16 += 2; i += cl; } else { utf16 += 1; i += cl; }
            continue;
        }
        utf16 += (cl >= 4) ? 2 : 1;
        i += cl;
    }
    pos.object["line"] = JNumber(line);
    pos.object["character"] = JNumber((double)utf16);
    return pos;
}

// Р РЋРЎвЂљРЎР‚Р С•Р С‘РЎвЂљ hover-Р С”Р С•Р Р…РЎвЂљР ВµР Р…РЎвЂљ Р Т‘Р В»РЎРЏ Р С”Р В»РЎР‹РЎвЂЎР ВµР Р†РЎвЂ№РЎвЂ¦ РЎРѓР В»Р С•Р Р†/РЎвЂљР С‘Р С—Р С•Р Р† Р С‘Р В· РЎРѓР В»Р С•Р Р†Р В°РЎР‚РЎРЏ.
static bool keywordHover(const string& word, JsonValue& contents, JsonValue& range) {
    (void)range;
    auto it = g_keywordDocs.find(word);
    if (it == g_keywordDocs.end()) return false;
    const KeywordDoc& kd = it->second;
    std::string md = std::string("**") + kd.name + "**  \n" + kd.summary;
    if (!kd.example.empty()) {
        md += "\n\n```lumen\n";
        md += kd.example;
        md += "\n```";
    }
    contents.object["kind"] = JString("markdown");
    contents.object["value"] = JString(md);
    return true;
}

// Р РЋРЎвЂљРЎР‚Р С•Р С‘РЎвЂљ hover Р С—Р С• Р С–Р В»Р С•Р В±Р В°Р В»РЎРЉР Р…Р С•Р СРЎС“ Р С”Р С•Р Р…РЎвЂљР ВµР С”РЎРѓРЎвЂљРЎС“ (Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…РЎвЂ№Р Вµ/РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘/РЎвЂљР С‘Р С—РЎвЂ№/namespace).
// Р СџР С•Р В»Р Р…Р В°РЎРЏ РЎРѓР С‘Р С–Р Р…Р В°РЎвЂљРЎС“РЎР‚Р В° РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘ Р Т‘Р В»РЎРЏ hover: `fn sum<T, U>(a: T, b: i64) -> i64`.
// Р ВР СР ВµР Р…Р В° Р С—Р В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚Р С•Р Р†/Р Р†Р С•Р В·Р Р†РЎР‚Р В°РЎвЂљР В° Р В±Р ВµРЎР‚РЎвЂР С Р С‘Р В· Р Т‘Р ВµР С”Р В»Р В°РЎР‚Р В°РЎвЂ Р С‘Р С‘ (Р Р…Р Вµ Р С‘Р В· Р Р†РЎвЂ№Р В·Р С•Р Р†Р В°), РЎвЂљР С‘Р С—РЎвЂ№ РІР‚вЂќ
// reified (Р С–Р ВµР Р…Р ВµРЎР‚Р С‘Р С”-Р С‘Р СРЎРЏ Р С—Р ВµРЎвЂЎР В°РЎвЂљР В°Р ВµРЎвЂљРЎРѓРЎРЏ Р С”Р В°Р С” "T", Р С”Р С•Р Р…Р С”РЎР‚Р ВµРЎвЂљР Р…РЎвЂ№Р в„– РІР‚вЂќ Р С”Р В°Р С” "i64").
static std::string funcSignature(const Function& f) {
    std::string s = "fn " + f.name.to_utf8();
    if (!f.generics.empty()) {
        s += "[";
        for (size_t i = 0; i < f.generics.size(); ++i) {
            if (i) s += ", ";
            s += f.generics[i].name.to_utf8();
        }
        s += "]";
    }
    s += "(";
    for (size_t i = 0; i < f.arguments.size(); ++i) {
        if (i) s += ", ";
        const FuncArg& pa = f.arguments[i];
        for (const String& mod : pa.modifiers)
            if (mod == "ref") { s += "ref "; break; }
        s += pa.name.to_utf8();
        if (i < f.reified_params.size()) {
            const Type& pt = f.reified_params[i];
            s += ": " + (pt.isAny() ? string("any") : pt.toString().to_utf8());
        } else {
            s += ": any";
        }
    }
    s += ")";
    if (f.return_type) {
        s += " -> ";
        if (f.returns_ref) s += "ref ";
        s += f.reified_ret.isAny()
            ? string("any") : f.reified_ret.toString().to_utf8();
    }
    return s;
}

// ---------- Р РЋРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚РЎвЂ№: РЎР‚Р ВµР Р…Р Т‘Р ВµРЎР‚ Р Т‘Р ВµР С”Р В»Р В°РЎР‚Р В°РЎвЂ Р С‘Р в„– Р С‘ РЎР‚Р ВµР В·Р С•Р В»Р Р† owner-РЎвЂ Р ВµР С—Р С•РЎвЂЎР ВµР С” ----------
// Р СџР ВµРЎвЂЎР В°РЎвЂљР В°Р ВµРЎвЂљ Р Р†РЎвЂ№РЎР‚Р В°Р В¶Р ВµР Р…Р С‘Р Вµ Р С‘Р В· AST Р С”Р С•РЎР‚Р С•РЎвЂљР С”Р С• (Р Т‘Р В»РЎРЏ Р Т‘Р ВµРЎвЂћР С•Р В»РЎвЂљР Р…РЎвЂ№РЎвЂ¦ Р В·Р Р…Р В°РЎвЂЎР ВµР Р…Р С‘Р в„– Р С—Р В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚Р С•Р Р†).
static std::string exprText(Node* n) {
    if (!n) return "";
    switch (n->type) {
        case NodeTypes::NodeVariable:
            return static_cast<struct NodeVariable*>(n)->name.to_utf8();
        case NodeTypes::NodeNumber: {
            auto* num = static_cast<struct NodeNumber*>(n);
            if (num->is_integer) return std::to_string(num->int_value);
            long double v = num->float_value;
            std::string s = std::to_string((double)v);
            while (s.size() > 1 && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
            return s;
        }
        case NodeTypes::NodeUnaryMinus:
            return "-" + exprText(static_cast<struct NodeUnaryMinus*>(n)->node);
        case NodeTypes::NodeString: {
            auto* sn = static_cast<struct NodeString*>(n);
            const std::string q = sn->is_char ? "'" : "\"";
            return q + sn->value.to_utf8() + q;
        }
        case NodeTypes::NodeArray: {
            std::string s = "{";
            for (size_t i = 0; i < static_cast<struct NodeArray*>(n)->elements.size(); ++i) {
                if (i) s += ", ";
                s += exprText(static_cast<struct NodeArray*>(n)->elements[i]);
            }
            return s + " }";
        }
        case NodeTypes::NodeScope:
            return "(" + exprText(static_cast<struct NodeScope*>(n)->node) + ")";
        default:
            return "";
    }
}

// Р СџР ВµРЎвЂЎР В°РЎвЂљР В°Р ВµРЎвЂљ РЎС“Р В·Р ВµР В»-РЎвЂљР С‘Р С— Р С”Р В°Р С” Р Р† Р С‘РЎРѓРЎвЂ¦Р С•Р Т‘Р Р…Р С‘Р С”Р Вµ: NodeVariable -> Р С‘Р СРЎРЏ, Array[T, N] -> Р С”Р В°Р С” Р ВµРЎРѓРЎвЂљРЎРЉ,
// Vec[i64, 8] Р Р† РЎвЂљР С‘Р С—-Р С—Р С•Р В·Р С‘РЎвЂ Р С‘Р С‘ -> NodeArrayAccess, '-> T' РЎС“ РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘ -> РЎРѓРЎРѓРЎвЂ№Р В»Р С”Р В° Р Р…Р В° РЎвЂљР С‘Р С—.
static std::string nodeTypeText(Node* n) {
    if (!n) return "";
    switch (n->type) {
        case NodeTypes::NodeVariable:
            return static_cast<struct NodeVariable*>(n)->name.to_utf8();
        case NodeTypes::NodeUnaryDereference:
            return "*" + nodeTypeText(static_cast<struct NodeUnaryDereference*>(n)->node);
        case NodeTypes::NodeBinaryBitwiseOr: {
            auto* bn = static_cast<struct NodeBinaryBitwiseOr*>(n);
            return nodeTypeText(bn->l_node) + " | " + nodeTypeText(bn->r_node);
        }
        case NodeTypes::NodeArrayAccess: {
            auto* nac = static_cast<struct NodeArrayAccess*>(n);
            std::string s = nodeTypeText(nac->base) + "[";
            for (size_t i = 0; i < nac->indices.size(); ++i) {
                if (i) s += ", ";
                s += exprText(nac->indices[i]);
            }
            return s + "]";
        }
        case NodeTypes::NodeArrayType: {
            auto* at = static_cast<struct NodeArrayType*>(n);
            std::string s = "Array[" + nodeTypeText(at->element_type);
            if (at->count) s += ", " + exprText(at->count);
            return s + "]";
        }
        default:
            return "";
    }
}

// Reified-С‚РёРї РІРѕР·РІСЂР°С‚Р° РјРµС‚РѕРґР° РїРѕРґ РёРЅСЃС‚Р°РЅСЃРѕРј-РІР»Р°РґРµР»СЊС†РµРј inst: generic-РёРјРµРЅР°
// СЃС‚СЂСѓРєС‚СѓСЂС‹ (T/Size) РїРѕРґСЃС‚Р°РІР»СЏСЋС‚СЃСЏ РёР· РёРЅСЃС‚Р°РЅСЃ-С‚РёРїР° (arg_mask РєР°Рє РІ bindingsForType);
// РёРјРµРЅР°, РЅРµ СЏРІР»СЏСЋС‰РёРµСЃСЏ СЃС‚СЂСѓРєС‚СѓСЂР°РјРё, РґР°СЋС‚ Ty::Any (С†РµРїРѕС‡РєР° Р·Р°РєР°РЅС‡РёРІР°РµС‚СЃСЏ).
static Type reifyMethodReturn(const StructSymbol& sym, const Function& f, const Type& inst,
                              const std::map<String, StructSymbol>& structs) {
    Node* rt = f.return_type;
    if (!rt) return Ty::Any;
    std::map<String, Type> names;
    size_t ti = 0;
    for (size_t i = 0; i < sym.generics.size(); ++i) {
        if (inst.arg_mask & (1u << static_cast<unsigned>(i))) {
            if (ti < inst.container_1.size())
                names.emplace(sym.generics[i].name, inst.container_1[ti]);
            ++ti;
        }
    }
    std::function<Type(Node*)> rec = [&](Node* n) -> Type {
        if (!n) return Ty::Any;
        switch (n->type) {
            case NodeTypes::NodeVariable: {
                String nm = static_cast<struct NodeVariable*>(n)->name;
                auto it = names.find(nm);
                if (it != names.end()) return it->second;
                if (structs.count(nm)) return Type::StructType(nm);
                return Ty::Any;
            }
            case NodeTypes::NodeUnaryDereference:
                return *(rec(static_cast<struct NodeUnaryDereference*>(n)->node));
            case NodeTypes::NodeBinaryBitwiseOr:
            case NodeTypes::NodeBinaryLogicalOr: {
                auto* bn = static_cast<struct NodeBinaryBitwiseOr*>(n);
                return rec(bn->l_node) | rec(bn->r_node);
            }
            case NodeTypes::NodeArrayType: {
                auto* at = static_cast<struct NodeArrayType*>(n);
                return Type::ArrayType(rec(at->element_type), -1);
            }
            default:
                return Ty::Any;
        }
    };
    return rec(rt);
}

// `<Р СР С•Р Т‘Р С‘РЎвЂћР С‘Р С”Р В°РЎвЂљР С•РЎР‚РЎвЂ№> let <Р С‘Р СРЎРЏ>: <РЎвЂљР С‘Р С—>` (Р В±Р ВµР В· Р Т‘Р ВµРЎвЂћР С•Р В»РЎвЂљР В°).
static std::string structFieldText(const StructFieldDef& fd) {
    std::string s;
    if (fd.mods.Static)  s += "static ";
    if (fd.mods.Const)   s += "const ";
    if (fd.mods.Final)   s += "final ";
    if (fd.mods.Strict)  s += "strict ";
    if (fd.mods.Private) s += "private ";
    s += "let " + fd.name.to_utf8();
    std::string t = nodeTypeText(fd.type_node);
    if (!t.empty()) s += ": " + t;
    return s;
}

// Р РЋР С‘Р С–Р Р…Р В°РЎвЂљРЎС“РЎР‚Р В° Р СР ВµРЎвЂљР С•Р Т‘Р В° as-declared: `fn push[T](self, x: T)` -> Р С•РЎвЂљР С•Р В±РЎР‚Р В°Р В¶Р ВµР Р…Р С‘Р Вµ generics.
static std::string methodSignature(const Function& f) {
    std::string s = std::string(f.is_const ? "const fn " : "fn ") + f.name.to_utf8();
    if (!f.generics.empty()) {
        s += "[";
        for (size_t i = 0; i < f.generics.size(); ++i) {
            if (i) s += ", ";
            const GenericParam& gp = f.generics[i];
            s += gp.name.to_utf8();
            std::string t = nodeTypeText(gp.type);
            if (!t.empty()) s += ": " + t;
            if (gp.default_value) s += " = " + exprText(gp.default_value);
        }
        s += "]";
    }
    s += "(";
    for (size_t i = 0; i < f.arguments.size(); ++i) {
        if (i) s += ", ";
        const FuncArg& pa = f.arguments[i];
        // self РІР‚вЂќ Р Р…Р ВµРЎРЏР Р†Р Р…РЎвЂ№Р в„– Р С—Р В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚ Р СР ВµРЎвЂљР С•Р Т‘Р В°, РЎвЂљР С‘Р С— Р Р…Р Вµ Р С—Р ВµРЎвЂЎР В°РЎвЂљР В°Р ВµР С
        if (pa.name == "self") { s += "self"; continue; }
        for (const String& mod : pa.modifiers)
            if (mod == "ref") { s += "ref "; break; }
        std::string t = nodeTypeText(pa.type);
        if (!t.empty()) s += pa.name.to_utf8() + ": " + t;
        else s += pa.name.to_utf8();
        if (pa.default_value) s += " = " + exprText(pa.default_value);
    }
    s += ")";
    std::string rt = nodeTypeText(f.return_type);
    if (!rt.empty()) s += " -> " + std::string(f.returns_ref ? "ref " : "") + rt;
    return s;
}

// Р—Р°РіРѕР»РѕРІРѕРє СЃС‚СЂСѓРєС‚СѓСЂС‹ as-declared: `struct[L, R] Pair` (generic-СЃРїРёСЃРѕРє РїРµСЂРµРґ РёРјРµРЅРµРј).
static std::string structHeaderText(const StructSymbol& sym) {
    std::string s = "struct";
    if (!sym.generics.empty()) {
        s += "[";
        for (size_t i = 0; i < sym.generics.size(); ++i) {
            if (i) s += ", ";
            const GenericParam& gp = sym.generics[i];
            s += gp.name.to_utf8();
            std::string t = nodeTypeText(gp.type);
            if (!t.empty()) s += ": " + t;
            if (gp.default_value) s += " = " + exprText(gp.default_value);
        }
        s += "]";
    }
    s += " " + sym.name.to_utf8();
    return s;
}

// Р”РѕРєСѓРјРµРЅС‚-РѕРїРёСЃР°РЅРёРµ enum: header + РІР°СЂРёР°РЅС‚С‹ (as-declared: `enum Types { a = 0, b = 1, ... }`).
static std::string enumDocBody(const EnumSymbol& sym) {
    std::string s = "**enum " + sym.name.to_utf8() + " {";
    bool first = true;
    for (const auto& kv : sym.enum_context.variables) {
        if (first) { s += " "; first = false; }
        else s += ", ";
        s += kv.first.to_utf8();
        const ContextObject& vco = kv.second;
        if (vco.value.raw().type() == typeid(long long))
            s += " = " + std::to_string(std::any_cast<long long>(vco.value.raw()));
    }
    s += " }**";
    return s;
}

// Р‘РµР· markdown вЂ” РґР»СЏ documentSymbol detail (РєР°Рє enumDocBody РІ СЂРµРЅРґРµСЂРµ hover)
static std::string enumPlainHeader(const EnumSymbol& sym) {
    std::string s = "enum " + sym.name.to_utf8() + " { ";
    bool first = true;
    for (const auto& kv : sym.enum_context.variables) {
        if (!first) s += ", ";
        first = false;
        s += kv.first.to_utf8();
        const ContextObject& vco = kv.second;
        if (vco.value.raw().type() == typeid(long long))
            s += " = " + std::to_string(std::any_cast<long long>(vco.value.raw()));
    }
    s += " }";
    return s;
}

// Р Р°Р·СЂРµС€Р°РµС‚ owner-С†РµРїРѕС‡РєСѓ РїРµСЂРµРґ РєСѓСЂСЃРѕСЂРѕРј РєР°Рє enum: `Types::a`. РћРґРЅРѕСѓСЂРѕРІРЅРµРІР°СЏ С†РµРїРѕС‡РєР°:
// РїРµСЂРІС‹Р№ СЃРµРіРјРµРЅС‚ вЂ” РёРјСЏ С‚РёРїР°-enum РІ РєРѕРЅС‚РµРєСЃС‚Рµ, РѕСЃС‚Р°Р»СЊРЅС‹Рµ СЃРµРіРјРµРЅС‚С‹ РЅРµ РёСЃРїРѕР»СЊР·СѓСЋС‚СЃСЏ.
static const EnumSymbol* resolveEnumOwner(const Context& ctx,
                                          const std::map<String, EnumSymbol>* reg,
                                          const std::vector<string>& owner) {
    if (!reg || owner.empty()) return nullptr;
    const ContextObject* root = nullptr;
    auto it = ctx.variables.find(String(owner[0]));
    if (it != ctx.variables.end()) root = &it->second;
    if (!root) return nullptr;
    if (!(root->value.T == Ty::Type_ && root->is_type_decl)) return nullptr;
    const Type tt = root->value.get<Type>();
    if (!tt.isEnum()) return nullptr;
    auto eit = reg->find(tt.name);
    return eit == reg->end() ? nullptr : &eit->second;
}

// Р В Р ВµР В·Р С•Р В»Р Р†Р С‘РЎвЂљ owner-РЎвЂ Р ВµР С—Р С•РЎвЂЎР С”РЎС“ Р С—Р ВµРЎР‚Р ВµР Т‘ Р С”РЎС“РЎР‚РЎРѓР С•РЎР‚Р С•Р С Р С”Р В°Р С” РЎРѓРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚РЎС“: Р С—Р ВµРЎР‚Р Р†РЎвЂ№Р в„– РЎРѓР ВµР С–Р СР ВµР Р…РЎвЂљ РІР‚вЂќ Р С‘Р СРЎРЏ
// РЎРѓРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚РЎвЂ№-РЎвЂљР С‘Р С—Р В° (`Vec::...`) Р С‘Р В»Р С‘ Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…Р В°РЎРЏ РЎРѓРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚Р Р…Р С•Р С–Р С• РЎвЂљР С‘Р С—Р В° (`p.x`), Р Т‘Р В°Р В»Р ВµР Вµ Р С—Р С•
// Р С—Р С•Р В»РЎРЏР С РЎРѓРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚Р Р…Р С•Р С–Р С• РЎвЂљР С‘Р С—Р В° (`r.b.y`). Р вЂ™Р С•Р В·Р Р†РЎР‚Р В°РЎвЂ°Р В°Р ВµРЎвЂљ РЎРѓР С‘Р СР Р†Р С•Р В» Р С—Р С•РЎРѓР В»Р ВµР Т‘Р Р…Р ВµР в„– РЎРѓРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚РЎвЂ№.
static const StructSymbol* resolveStructOwner(const Context& ctx,
                                              const std::map<std::string, LocalCandidate>* locals,
                                              const std::vector<string>& owner,
                                              const std::vector<bool>* callFlags,
                                              const std::map<String, StructSymbol>* reg,
                                              const StructSymbol*& sym,
                                              const std::map<unsigned long long, Type>* localTypes = nullptr) {
    sym = nullptr;
    if (!reg || owner.empty()) return nullptr;
    auto byName = [reg](const String& nm) -> const StructSymbol* {
        auto it = reg->find(nm);
        return it == reg->end() ? nullptr : &it->second;
    };
    // Р С—Р ВµРЎР‚Р Р†РЎвЂ№Р в„– РЎРѓР ВµР С–Р СР ВµР Р…РЎвЂљ: Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…Р В°РЎРЏ Р С”Р С•Р Р…РЎвЂљР ВµР С”РЎРѓРЎвЂљР В° (Р С–Р В»Р С•Р В±Р В°Р В»/Р В»Р С•Р С”Р В°Р В»РЎРЉР Р…Р В°РЎРЏ)
    const StructSymbol* first = nullptr;
    Type inst = Ty::Any;
    auto it = ctx.variables.find(String(owner[0]));
    if (it != ctx.variables.end()) {
        const ContextObject& co = it->second;
        if (co.value.T == Ty::Type_ && co.is_type_decl && co.value.get<Type>().isStruct()) {
            inst = co.value.get<Type>();
            first = byName(inst.name);
        } else if (co.value.T.isStruct()) {
            inst = co.value.T;
            first = byName(inst.name);
        }
    }
    if (!first && locals) {
        auto lit = locals->find(owner[0]);
        if (lit != locals->end() && !lit->second.detail.empty()) {
            // P8: РїРѕР»РЅС‹Р№ СЂРµРёС„РёС†РёСЂРѕРІР°РЅРЅС‹Р№ С‚РёРї Р»РѕРєР°Р»СЊРЅРѕР№ РёР· DeepAnalizer (declare-node
            // hint) вЂ” РІРјРµСЃС‚Рѕ РёРјРµРЅРё СЃС‚СЂСѓРєС‚СѓСЂС‹ Р±РµР· С‚РёРїРѕРІС‹С… Р°СЂРіСѓРјРµРЅС‚РѕРІ.
            if (localTypes) {
                auto ltt = localTypes->find(lit->second.declKey);
                if (ltt != localTypes->end()) {
                    inst = ltt->second;
                    first = byName(inst.name);
                }
            }
            if (!first) {
                inst = Type::StructType(String(lit->second.detail));
                first = byName(inst.name);
            }
        }
    }
    sym = first;
    if (!sym) return nullptr;
    // Р С—РЎР‚Р С•Р СР ВµР В¶РЎС“РЎвЂљР С•РЎвЂЎР Р…РЎвЂ№Р Вµ РЎРѓР ВµР С–Р СР ВµР Р…РЎвЂљРЎвЂ№ РІР‚вЂќ Р С—Р С•Р В»РЎРЏ РЎРѓРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚Р Р…Р С•Р С–Р С• РЎвЂљР С‘Р С—Р В° / РЎР‚Р ВµР С‘РЎвЂћР С‘РЎвЂ Р С‘РЎР‚Р С•Р Р†Р В°Р Р…Р Р…РЎвЂ№Р Вµ Р Р†РЎвЂ№Р В·Р С•Р Р†РЎвЂ№
    for (size_t i = 1; i < owner.size(); ++i) {
        const bool isCall = callFlags && callFlags->size() > i && (*callFlags)[i];
        if (isCall) {
            auto mit = sym->methods.find(String(owner[i]));
            if (mit == sym->methods.end() || mit->second.functions.empty()) return nullptr;
            Type rt = reifyMethodReturn(*sym, mit->second.functions[0], inst, *reg);
            sym = rt.isStruct() ? byName(rt.name) : nullptr;
            if (!sym) return nullptr;
            inst = rt;
            continue;
        }
        const StructFieldDef* fd = sym->field_def(String(owner[i]));
        if (!fd || !fd->type_node) return nullptr;
        std::string tn = nodeTypeText(fd->type_node);
        if (tn.empty()) return nullptr;
        inst = Type::StructType(String(tn));
        sym = byName(inst.name);
        if (!sym) return nullptr;
    }
    return sym;
}

// Р  Р ВµР С”РЎС“РЎР‚РЎРѓР С‘Р Р†Р Р…РЎвЂ№Р в„– Р С•Р В±РЎвЂ¦Р С•Р Т‘ AST: Р Т‘Р С•Р С”-Р С”Р С•Р СР СР ВµР Р…РЎвЂљР В°РЎР‚Р С‘Р в„– РЅР°Рґ РѕР±СЉСЏРІР»РµРЅРёРµРј С„СѓРЅРєС†РёРё (РїРѕ РёРјРµРЅРё).
// РџРѕСЃРєРѕР»СЊРєСѓ РїСЂРµРїСЂРѕС†РµСЃСЃРѕСЂ РІС‹РєРёРґС‹РІР°РµС‚ РєРѕРјРјРµРЅС‚Р°СЂРёРё РіРѕР»С‹Рµ (Р±РµР· РёСЃС…РѕРґРЅРёРєР°), РёСЃРїРѕР»СЊР·СѓРµРј РђРЎРў-РїРѕР»Рµ,
// РєРѕС‚РѕСЂРѕРµ РїР°СЂСЃРµСЂ Р·Р°РїРѕР»РЅРёР» РёР· СЃС‹СЂС‹С… С‚РѕРєРµРЅРѕРІ РґРѕ filterComments.
static std::string findFunctionDoc(Node* n, const std::string& name) {
    if (!n) return "";
    switch (n->type) {
        case NodeTypes::NodeFunctionDeclaration: {
            auto* fn = static_cast<struct NodeFunctionDeclaration*>(n);
            std::string r;
            if (fn->name.to_utf8() == name) r = fn->doc_comment.to_utf8();
            std::string c = findFunctionDoc(fn->body, name);
            return !c.empty() ? c : r;
        }
        case NodeTypes::NodeNamespace:
            return findFunctionDoc(static_cast<struct NodeNamespace*>(n)->body, name);
        case NodeTypes::NodeStructDeclaration:
            return findFunctionDoc(static_cast<struct NodeStructDeclaration*>(n)->body, name);
        case NodeTypes::NodeStatementsScope: {
            auto* s = static_cast<struct NodeStatementsScope*>(n);
            for (auto* child : s->nodes) {
                std::string r = findFunctionDoc(child, name);
                if (!r.empty()) return r;
            }
            return "";
        }
        case NodeTypes::NodeScope:
            return findFunctionDoc(static_cast<struct NodeScope*>(n)->node, name);
        case NodeTypes::NodeIf: {
            auto* i = static_cast<struct NodeIf*>(n);
            std::string a = findFunctionDoc(i->then_body, name);
            return !a.empty() ? a : findFunctionDoc(i->else_body, name);
        }
        case NodeTypes::NodeWhile:
            return findFunctionDoc(static_cast<struct NodeWhile*>(n)->body, name);
        case NodeTypes::NodeFor:
            return findFunctionDoc(static_cast<struct NodeFor*>(n)->body, name);
        case NodeTypes::NodeForEach:
            return findFunctionDoc(static_cast<struct NodeForEach*>(n)->body, name);
        case NodeTypes::NodeLambda:
            return findFunctionDoc(static_cast<struct NodeLambda*>(n)->body, name);
        case NodeTypes::NodeReturn:
            return findFunctionDoc(static_cast<struct NodeReturn*>(n)->expr, name);
        case NodeTypes::NodeModifier:
            return findFunctionDoc(static_cast<struct NodeModifier*>(n)->node, name);
        default: return "";
    }
}

static void appendDocBlock(std::string& md, const std::string& doc, bool withRule = true) {
    if (doc.empty()) return;

    // РќРѕСЂРјР°Р»РёР·Р°С†РёСЏ: СЂРµР¶РµРј СЃС‚СЂРѕРєРё, СѓР±РёСЂР°РµРј РІРЅРµС€РЅРёРµ РїСѓСЃС‚С‹Рµ СЃС‚СЂРѕРєРё, РѕР±С‰РёР№ РѕС‚СЃС‚СѓРї
    // Рё РїСЂРµС„РёРєСЃС‹ `* ` Рё `//` (РєРѕРЅРІРµРЅС†РёСЏ Р±Р»РѕС‡РЅС‹С… Рё РїРѕСЃС‚СЂРѕС‡РЅС‹С… РґРѕРє-РєРѕРјРјРµРЅС‚Р°СЂРёРµРІ).
    std::vector<std::string> raw;
    {
        std::string cur;
        for (char c : doc) {
            if (c == '\n') { raw.push_back(cur); cur.clear(); }
            else cur += c;
        }
        raw.push_back(cur);
    }
    while (!raw.empty() && raw.front().find_first_not_of(" \t\r") == std::string::npos) raw.erase(raw.begin());
    while (!raw.empty() && raw.back().find_first_not_of(" \t\r") == std::string::npos) raw.pop_back();
    if (raw.empty()) return;

    size_t minIndent = std::string::npos;
    for (const auto& ln : raw) {
        size_t p = 0;
        while (p < ln.size() && (ln[p] == ' ' || ln[p] == '\t')) p++;
        if (p < ln.size()) minIndent = (minIndent == std::string::npos) ? p : std::min(minIndent, p);
    }
    if (minIndent == std::string::npos) minIndent = 0;

    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        std::string ln = raw[i];
        size_t p = 0;
        while (p < ln.size() && p < minIndent && (ln[p] == ' ' || ln[p] == '\t')) p++;
        ln = ln.substr(p);
        // Снимаем только маркер построчного стиля `// ` (или `* ` у блоков в стиле
        // «звёздочек»). Никаких произвольных ведущих пробелов не трогаем — иначе
        // код внутри ``` ```lumen ``` теряет отступы.
        if (ln.compare(0, 2, "//") == 0) {
            ln = ln.substr(2);
            if (!ln.empty() && ln[0] == ' ') ln = ln.substr(1);
        } else if (!ln.empty() && ln[0] == '*') {
            ln = ln.substr(1);
            if (!ln.empty() && ln[0] == ' ') ln = ln.substr(1);
        }
        while (!ln.empty() && (ln.back() == ' ' || ln.back() == '\t' || ln.back() == '\r')) ln.pop_back();
        if (i) out += "\n";
        out += ln;
    }

    if (withRule) md += "\n\n---\n" + out;
    else md += "\n\n" + out;
}

// Hover у списка перегрузок: каждая сигнатура со своим док-комментарием. Одиночная
// функция — классический вид «сигнатура / --- / док». Несколько перегрузок идут
// отдельными блоками, разделёнными горизонтальной линией (---), чтобы доки разных
// сигнатур не слипались в одну текстовую простыню.
static void appendOverloadDocs(std::string& md, const FunctionContainer& fc,
                               std::string (*signatureOf)(const Function&)) {
    if (fc.functions.size() == 1) {
        md += "**" + signatureOf(fc.functions[0]) + "**";
        appendDocBlock(md, fc.functions[0].doc_comment.to_utf8());
        return;
    }
    for (size_t i = 0; i < fc.functions.size(); ++i) {
        if (i) md += "\n\n---\n\n";
        md += "**" + signatureOf(fc.functions[i]) + "**";
        appendDocBlock(md, fc.functions[i].doc_comment.to_utf8(), false);
    }
}

// РћР±С…РѕРґ РІСЃРµРіРѕ РґРµСЂРµРІР° РІРµСЂС…РЅРµРіРѕ СѓСЂРѕРІРЅСЏ (tree вЂ” РІРµРєС‚РѕСЂ РІРµСЂС€РёРЅ, Р° РЅРµ РѕРґРёРЅ РєРѕСЂРµРЅСЊ).
static std::string findFunctionDocInTree(const std::vector<Node*>& tree, const std::string& name) {
    for (auto* top : tree) {
        std::string r = findFunctionDoc(top, name);
        if (!r.empty()) return r;
    }
    return "";
}

// Р”РѕРє-РєРѕРјРјРµРЅС‚Р°СЂРёР№ РјРµС‚РѕРґР° РІРЅСѓС‚СЂРё СЃС‚СЂСѓРєС‚СѓСЂС‹ (РѕР±СЂР°С‰Р°РµРјСЃСЏ Рє С‚РµР»Сѓ NodeStructDeclaration).
static std::string findStructMethodDoc(const std::vector<Node*>& tree,
                                       const std::string& structName,
                                       const std::string& methodName) {
    for (auto* top : tree) {
        if (!top || top->type != NodeTypes::NodeStructDeclaration) continue;
        auto* st = static_cast<struct NodeStructDeclaration*>(top);
        if (st->name.to_utf8() != structName) continue;
        return findFunctionDoc(st->body, methodName);
    }
    return "";
}

// Р”РѕРє-РєРѕРјРјРµРЅС‚Р°СЂРёР№ СЃР°РјРѕРіРѕ РѕР±СЉСЏРІР»РµРЅРёСЏ СЃС‚СЂСѓРєС‚СѓСЂС‹.
static std::string findStructDocInTree(const std::vector<Node*>& tree, const std::string& structName) {
    for (auto* top : tree) {
        if (!top || top->type != NodeTypes::NodeStructDeclaration) continue;
        auto* st = static_cast<struct NodeStructDeclaration*>(top);
        if (st->name.to_utf8() == structName) return st->doc_comment.to_utf8();
    }
    return "";
}

static bool contextHover(const Context& ctx, const string& word,
                         const std::map<String, StructSymbol>* structs,
                         JsonValue& contents, JsonValue& range,
                         const std::string& doc = "",
                         const std::string& structDoc = "",
                         const std::map<String, EnumSymbol>* enums = nullptr) {
    auto it = ctx.variables.find(String(word));
    if (it == ctx.variables.end())
        return false; // СЃР»РѕРІР° РЅРµС‚ РІ РєРѕРЅС‚РµРєСЃС‚Рµ вЂ” РёС‰РµРј РґР°Р»СЊС€Рµ (#define/#macro/namespace)
    const ContextObject& co = it->second;
    std::string md;
    // Р В¤РЎС“Р Р…Р С”РЎвЂ Р С‘РЎРЏ (Р Р†Р С•Р В·Р СР С•Р В¶Р Р…Р С•, Р С—Р ВµРЎР‚Р ВµР С–РЎР‚РЎС“Р В·Р С”Р С‘): Р С”Р С•Р Р…РЎвЂљР ВµР в„–Р Р…Р ВµРЎР‚ Р Р† value РЎРѓР С•Р Т‘Р ВµРЎР‚Р В¶Р С‘РЎвЂљ Р Т‘Р ВµР С”Р В»Р В°РЎР‚Р В°РЎвЂ Р С‘Р С‘
    // РЎРѓ Р С—Р В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚Р В°Р СР С‘/Р Т‘Р В¶Р ВµР Р…Р ВµРЎР‚Р С‘Р С”Р В°Р СР С‘/РЎвЂљР С‘Р С—Р С•Р С Р Р†Р С•Р В·Р Р†РЎР‚Р В°РЎвЂљР В°. Р С›Р Т‘Р Р…Р В° РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘РЎРЏ РІР‚вЂќ CALLABLE-РЎвЂљР С‘Р С—,
    // Р С—Р ВµРЎР‚Р ВµР С–РЎР‚РЎС“Р В·Р С”Р С‘ РІР‚вЂќ UNION РЎРѓР С‘Р С–Р Р…Р В°РЎвЂљРЎС“РЎР‚.
    bool isFunc = co.value.T.isCallable();
    if (!isFunc && co.value.T.isUnion()) {
        const std::vector<Type>& arms = co.value.T.container_1;
        isFunc = !arms.empty();
        for (const Type& a : arms)
            if (!a.isCallable()) { isFunc = false; break; }
    }
    const FunctionContainer* fc =
        isFunc ? std::any_cast<const FunctionContainer>(&co.value.raw()) : nullptr;
    if (fc) {
        appendOverloadDocs(md, *fc, &funcSignature);
    } else if (co.value.T == Ty::Namespace) {
        md += std::string("**namespace ") + word + "**";
    } else if (co.is_type_decl) {
        const Type tt = co.value.get<Type>();
        if (structs && tt.isStruct()) {
            auto sit = structs->find(String(word));
            if (sit != structs->end())
                md = std::string("**") + structHeaderText(sit->second) + "**";
            else
                md = std::string("**type ") + word + "**  \n" + tt.toString().to_utf8();
            appendDocBlock(md, structDoc);
        } else if (enums && tt.isEnum()) {
            auto eit = enums->find(String(word));
            if (eit != enums->end())
                md = enumDocBody(eit->second);
            else
                md = std::string("**enum ") + word + "**  \n" + tt.toString().to_utf8();
        } else {
            md = std::string("**type ") + word + "**  \n" + tt.toString().to_utf8();
        }
    } else {
        std::string mods;
        const ContextObject::Modifiers m = co.modifiers;
        // Р С—Р С•РЎР‚РЎРЏР Т‘Р С•Р С” Р СР С•Р Т‘Р С‘РЎвЂћР С‘Р С”Р В°РЎвЂљР С•РЎР‚Р С•Р Р† Р С‘Р В· Р С•Р В±РЎР‰РЎРЏР Р†Р В»Р ВµР Р…Р С‘РЎРЏ (`final strict let pi РІР‚В¦`); Р ВµРЎРѓР В»Р С‘
        // Р В°Р Р…Р В°Р В»Р С‘Р В·Р В°РЎвЂљР С•РЎР‚ Р ВµР С–Р С• Р Р…Р Вµ РЎРѓР С•РЎвЂ¦РЎР‚Р В°Р Р…Р С‘Р В», РЎРѓР С•Р В±Р С‘РЎР‚Р В°Р ВµР С Р С—Р С• РЎвЂћР В»Р В°Р С–Р В°Р С Р Р† Р С”Р В°Р Р…Р С•Р Р…Р С‘РЎвЂЎР ВµРЎРѓР С”Р С•Р С Р С—Р С•РЎР‚РЎРЏР Т‘Р С”Р Вµ
        if (!m.order.empty()) {
            for (size_t i = 0; i < m.order.size(); ++i)
                mods += m.order[i].to_utf8() + " ";
        } else {
            if (m.Const)   mods += "const ";
            if (m.Strict)  mods += "strict ";
            if (m.Final)   mods += "final ";
            if (m.Static)  mods += "static ";
            if (m.Private) mods += "private ";
        }
        // РЎвЂћР С•РЎР‚Р СР В°РЎвЂљ Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…Р С•Р в„– Р Р†РЎРѓР ВµР С–Р Т‘Р В° Р С•Р Т‘Р С‘Р Р… Р С‘ РЎвЂљР С•РЎвЂљ Р В¶Р Вµ: <Р СР С•Р Т‘Р С‘РЎвЂћР С‘Р С”Р В°РЎвЂљР С•РЎР‚РЎвЂ№> let <Р С‘Р СРЎРЏ>: <РЎвЂљР С‘Р С—> РІР‚вЂќ
        // РЎвЂљР С‘Р С— Р В±Р ВµРЎР‚РЎвЂРЎвЂљРЎРѓРЎРЏ Р С‘Р В· value, Р Т‘Р В°Р В¶Р Вµ Р ВµРЎРѓР В»Р С‘ РЎРЊРЎвЂљР С• РЎвЂљР С‘Р С—-Р В·Р Р…Р В°РЎвЂЎР ВµР Р…Р С‘Р Вµ (`let a: auto = i32; -> Type`)
        md += std::string("**") + mods + "let " + word + ": " +
              co.value.T.toString().to_utf8() + "**";
    }
    contents.object["kind"] = JString("markdown");
    contents.object["value"] = JString(md);
    return true;
}

// Р вЂњР В»Р В°Р Р†Р Р…РЎвЂ№Р в„– Р С•Р В±РЎР‚Р В°Р В±Р С•РЎвЂљРЎвЂЎР С‘Р С” hover.
// ---------- Enclosing function Р Т‘Р В»РЎРЏ РЎР‚Р В°Р В·РЎР‚Р ВµРЎв‚¬Р ВµР Р…Р С‘РЎРЏ Р С—Р В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚Р С•Р Р† ----------

// Р вЂР В°Р в„–РЎвЂљР С•Р Р†РЎвЂ№Р в„– Р Т‘Р С‘Р В°Р С—Р В°Р В·Р С•Р Р… РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘ Р С•РЎвЂљ Р С‘Р СР ВµР Р…Р С‘ Р Т‘Р С• Р В·Р В°Р С”РЎР‚РЎвЂ№Р Р†Р В°РЎР‹РЎвЂ°Р ВµР в„– '}' (Р С—Р С• РЎвЂљР С•Р С”Р ВµР Р…Р В°Р С).
// NB: `enum NodeTypes` РІР‚вЂќ Р СњР вЂў enum class, Р ВµР С–Р С• РЎРЊР В»Р ВµР СР ВµРЎР‚РЎвЂ№ (Р Р† РЎвЂљ.РЎвЂЎ. NodeFunctionDeclaration)
// Р В»Р ВµР В¶Р В°РЎвЂљ Р Р† Р С–Р В»Р С•Р В±Р В°Р В»РЎРЉР Р…Р С•Р в„– Р С•Р В±Р В»Р В°РЎРѓРЎвЂљР С‘ Р С”Р В°Р С” РЎвЂ Р ВµР В»РЎвЂ№Р Вµ Р С”Р С•Р Р…РЎРѓРЎвЂљР В°Р Р…РЎвЂљРЎвЂ№ Р С‘ Р С—Р ВµРЎР‚Р ВµР С”РЎР‚РЎвЂ№Р Р†Р В°РЎР‹РЎвЂљ Р С‘Р СРЎРЏ РЎРѓРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚РЎвЂ№.
// Р СћР С‘Р С—-РЎвЂљР ВµР С– Р В±Р ВµРЎР‚РЎвЂР С РЎвЂЎР ВµРЎР‚Р ВµР В· typedef, РЎвЂЎРЎвЂљР С•Р В±РЎвЂ№ Р Р…Р Вµ РЎвЂљР В°РЎвЂ°Р С‘РЎвЂљРЎРЉ `struct` Р Р† Р С”Р В°Р В¶Р Т‘Р С•Р С Р С•Р В±РЎР‰РЎРЏР Р†Р В»Р ВµР Р…Р С‘Р С‘.
typedef struct NodeFunctionDeclaration LmFn;

static void fnRange(LmFn* fn,
                    const std::vector<Token>& tokens,
                    const std::vector<size_t>& lineStarts,
                    size_t& begin, size_t& end) {
    if (!fn || fn->name_token.type == TypeOfToken::DUMMY) return;
    int nl = fn->name_token.position.line;
    begin = (nl >= 0 && (size_t)nl < lineStarts.size())
        ? lineStarts[nl] + fn->name_token.position.index : 0;
    size_t beginIdx = tokens.size();
    for (size_t ti = 0; ti < tokens.size(); ++ti)
        if (tokens[ti].position.line == nl
                && tokens[ti].position.index == fn->name_token.position.index) {
            beginIdx = ti; break;
        }
    if (beginIdx >= tokens.size()) { begin = 0; return; }
    int depth = 0; bool opened = false;
    for (size_t ti = beginIdx + 1; ti < tokens.size(); ++ti) {
        const Token& t = tokens[ti];
        if (t.type == TypeOfToken::BRACE_LEFT_CURVE) { depth++; opened = true; }
        else if (t.type == TypeOfToken::BRACE_RIGHT_CURVE) {
            if (depth > 0) depth--;
            if (depth == 0 && opened) {
                size_t bl = (size_t)t.position.line;
                end = (bl < lineStarts.size() ? lineStarts[bl] : 0)
                    + t.position.index + t.position.size;
                return;
            }
        }
    }
    begin = 0;
}

// Р В Р ВµР С”РЎС“РЎР‚РЎРѓР С‘Р Р†Р Р…РЎвЂ№Р в„– Р С•Р В±РЎвЂ¦Р С•Р Т‘: innermost РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘РЎРЏ, РЎвЂЎР ВµР в„– Р Т‘Р С‘Р В°Р С—Р В°Р В·Р С•Р Р… РЎРѓР С•Р Т‘Р ВµРЎР‚Р В¶Р С‘РЎвЂљ Р В±Р В°Р в„–РЎвЂљР С•Р Р†РЎвЂ№Р в„– off.
static void enclosingFunctionWalk(Node* n,
                                  const std::vector<Token>& tokens,
                                  const std::vector<size_t>& lineStarts,
                                  size_t off,
                                  LmFn** best,
                                  size_t* bestLen) {
    if (!n) return;
    switch (n->type) {
        case NodeTypes::NodeFunctionDeclaration: {
            auto* fn = static_cast<struct NodeFunctionDeclaration*>(n);
            size_t begin = 0, end = 0;
            fnRange(fn, tokens, lineStarts, begin, end);
            if (end > begin && off >= begin && off < end) {
                size_t len = end - begin;
                if (!*best || len < *bestLen) { *best = fn; *bestLen = len; }
            }
            enclosingFunctionWalk(fn->body, tokens, lineStarts, off, best, bestLen);
            return;
        }
        case NodeTypes::NodeStructDeclaration:
            enclosingFunctionWalk(static_cast<struct NodeStructDeclaration*>(n)->body,
                                  tokens, lineStarts, off, best, bestLen);
            return;
        case NodeTypes::NodeNamespace:
            enclosingFunctionWalk(static_cast<struct NodeNamespace*>(n)->body,
                                  tokens, lineStarts, off, best, bestLen);
            return;
        case NodeTypes::NodeStatementsScope: {
            auto* s = static_cast<struct NodeStatementsScope*>(n);
            for (auto* c : s->nodes)
                enclosingFunctionWalk(c, tokens, lineStarts, off, best, bestLen);
            return;
        }
        case NodeTypes::NodeScope:
            enclosingFunctionWalk(static_cast<struct NodeScope*>(n)->node,
                                  tokens, lineStarts, off, best, bestLen);
            return;
        case NodeTypes::NodeIf: {
            auto* i = static_cast<struct NodeIf*>(n);
            enclosingFunctionWalk(i->then_body, tokens, lineStarts, off, best, bestLen);
            enclosingFunctionWalk(i->else_body, tokens, lineStarts, off, best, bestLen);
            return;
        }
        case NodeTypes::NodeWhile:
            enclosingFunctionWalk(static_cast<struct NodeWhile*>(n)->body,
                                  tokens, lineStarts, off, best, bestLen);
            return;
        case NodeTypes::NodeFor:
            enclosingFunctionWalk(static_cast<struct NodeFor*>(n)->body,
                                  tokens, lineStarts, off, best, bestLen);
            return;
        case NodeTypes::NodeForEach:
            enclosingFunctionWalk(static_cast<struct NodeForEach*>(n)->body,
                                  tokens, lineStarts, off, best, bestLen);
            return;
        case NodeTypes::NodeLambda:
            enclosingFunctionWalk(static_cast<struct NodeLambda*>(n)->body,
                                  tokens, lineStarts, off, best, bestLen);
            return;
        case NodeTypes::NodeReturn:
            enclosingFunctionWalk(static_cast<struct NodeReturn*>(n)->expr,
                                  tokens, lineStarts, off, best, bestLen);
            return;
        case NodeTypes::NodeModifier:
            enclosingFunctionWalk(static_cast<struct NodeModifier*>(n)->node,
                                  tokens, lineStarts, off, best, bestLen);
            return;
        default: break;
    }
}

static LmFn* enclosingFunction(const std::vector<Node*>& tree,
                                                  const std::vector<Token>& tokens,
                                                  const std::vector<size_t>& lineStarts,
                                                  size_t off) {
    LmFn* best = nullptr;
    size_t bestLen = 0;
    for (auto* node : tree) {
        LmFn* cand = nullptr;
        size_t candLen = 0;
        enclosingFunctionWalk(node, tokens, lineStarts, off, &cand, &candLen);
        if (cand && (!best || candLen < bestLen)) { best = cand; bestLen = candLen; }
    }
    return best;
}

// Find the struct declaration whose body contains `pos`.
// Uses token scan: forward scan for `struct` keywords and brace matching.
struct EnclosingStruct {
    size_t begin, end;
    std::string name;
};
static bool findEnclosingStruct(const std::vector<Token>& tokens,
                                const std::vector<size_t>& lineStarts,
                                size_t pos,
                                EnclosingStruct& out) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].type != KEYWORD || tokens[i].value.to_utf8() != "struct")
            continue;
        size_t j = i + 1;
        // skip optional generic params `[T, U]`
        if (j < tokens.size() && tokens[j].type == BRACE_LEFT_RECT) {
            int d = 1; ++j;
            while (j < tokens.size() && d) {
                if (tokens[j].type == BRACE_LEFT_RECT) ++d;
                else if (tokens[j].type == BRACE_RIGHT_RECT) { --d; if (!d) break; }
                ++j;
            }
            ++j;
        }
        if (j >= tokens.size() || tokens[j].type != LITERAL) continue;
        size_t nameIdx = j; ++j;
        while (j < tokens.size() && tokens[j].type != BRACE_LEFT_CURVE) ++j;
        if (j >= tokens.size()) continue;
        size_t bodyOpen = j;
        int depth = 0; size_t bodyClose = 0;
        for (size_t k = bodyOpen; k < tokens.size(); ++k) {
            if (tokens[k].type == BRACE_LEFT_CURVE) ++depth;
            else if (tokens[k].type == BRACE_RIGHT_CURVE) { --depth; if (!depth) { bodyClose = k; break; } }
        }
        if (!bodyClose) continue;
        size_t begin = tokenByteOffset(tokens[i], lineStarts);
        size_t end = tokenByteOffset(tokens[bodyClose], lineStarts)
                    + tokens[bodyClose].value.to_utf8().size();
        if (pos >= begin && pos < end) {
            out.begin = begin;
            out.end = end;
            out.name = tokens[nameIdx].value.to_utf8();
            return true;
        }
    }
    return false;
}

// Р В¤РЎС“Р Р…Р С”РЎвЂ Р С‘РЎРЏ Р С‘Р В· Р С”Р С•Р Р…РЎвЂљР ВµР в„–Р Р…Р ВµРЎР‚Р В°, РЎРѓР С•Р С•РЎвЂљР Р†Р ВµРЎвЂљРЎРѓРЎвЂљР Р†РЎС“РЎР‹РЎвЂ°Р В°РЎРЏ Р Т‘Р ВµР С”Р В»Р В°РЎР‚Р В°РЎвЂ Р С‘Р С‘ (Р С—Р С• РЎвЂЎР С‘РЎРѓР В»РЎС“ Р В°РЎР‚Р С–РЎС“Р СР ВµР Р…РЎвЂљР С•Р Р†).
static const FunctionContainer* findFunctionContainer(const string& name,
                                                      const Context& searchCtx,
                                                      const Context& moduleCtx) {
    auto it = searchCtx.variables.find(String(name));
    const Context* owner = &searchCtx;
    if (it == searchCtx.variables.end())
        for (const auto& kv : moduleCtx.variables) {
            if (kv.second.value.T != Ty::Namespace) continue;
            const Context& nsCtx =
                std::any_cast<const Namespace&>(kv.second.value.raw()).context;
            auto nit = nsCtx.variables.find(String(name));
            if (nit != nsCtx.variables.end()) { it = nit; owner = &nsCtx; break; }
        }
    if (it == owner->variables.end()) return nullptr;
    const ContextObject& co = it->second;
    bool isFunc = co.value.T.isCallable();
    if (!isFunc && co.value.T.isUnion()) {
        const std::vector<Type>& arms = co.value.T.container_1;
        isFunc = !arms.empty();
        for (const Type& a : arms)
            if (!a.isCallable()) { isFunc = false; break; }
    }
    return isFunc ? std::any_cast<const FunctionContainer>(&co.value.raw()) : nullptr;
}

// Р СџР В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚ РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘ (Р С•Р В±Р В»Р В°РЎРѓРЎвЂљРЎРЉ Р Р†Р С‘Р Т‘Р С‘Р СР С•РЎРѓРЎвЂљР С‘ РЎвЂљР ВµР В»Р В°) Р В·Р В°РЎвЂљР ВµР Р…РЎРЏР ВµРЎвЂљ Р С•Р Т‘Р Р…Р С•Р С‘Р СРЎвЂР Р…Р Р…РЎС“РЎР‹ Р С–Р В»Р С•Р В±Р В°Р В»РЎРЉР Р…РЎС“РЎР‹
// Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…РЎС“РЎР‹: hover Р С—Р С• Р С—Р В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚РЎС“/Р С–Р В»Р С•Р В±Р В°Р В» Р Р…Р Вµ Р Т‘Р С•Р В»Р В¶Р ВµР Р… Р Р†Р С•Р В·Р Р†РЎР‚Р В°РЎвЂ°Р В°РЎвЂљРЎРЉ Р С–Р В»Р С•Р В±Р В°Р В».
static bool paramHover(LmFn* fn,
                       const Context& searchCtx, const Context& moduleCtx,
                       const string& word, std::string& md) {
    if (!fn) return false;
    const FunctionContainer* fc = findFunctionContainer(fn->name.to_utf8(), searchCtx, moduleCtx);
    const Function* matching = nullptr;
    if (fc)
        for (const Function& f : fc->functions)
            if (f.arguments.size() == fn->params.size()) { matching = &f; break; }
    for (size_t i = 0; i < fn->params.size(); ++i) {
        if (fn->params[i].name.to_utf8() != word) continue;
        std::string declared = nodeTypeText(fn->params[i].type);
        // Реифицированный тип точнее (`Number[i64]`), но контейнер доступен не
        // всегда: у методов он лежит в StructSymbol, а не в контексте, и поиск по
        // имени может попасть в одноимённую глобальную функцию. Поэтому при
        // совпадении с дженериком функции берём объявленный текст как есть.
        bool is_generic = false;
        for (const GenericParam& g : fn->generics)
            if (declared == g.name.to_utf8()) { is_generic = true; break; }
        string ty;
        if (!is_generic && matching && i < matching->reified_params.size())
            ty = matching->reified_params[i].toString().to_utf8();
        if (ty.empty() || ty == "any") ty = declared;
        if (ty.empty()) ty = "any";
        md = "**let " + word + ": " + ty + "**";
        return true;
    }
    return false;
}

// Р”Р¶РµРЅРµСЂРёРє-РїР°СЂР°РјРµС‚СЂ РѕР±СЉС‘РјР»СЋС‰РµР№ С„СѓРЅРєС†РёРё (`A` РІ `fn sum[A, B](a: A, b: B)`):
// hover `*generic param A: Type*`, РІРёРґРёРј РІ РґРёР°РїР°Р·РѕРЅРµ С„СѓРЅРєС†РёРё.
// Дженерик-параметр по имени в готовом списке: `T` в `struct[T] Number` или `fn sum[T]`,
// `NT: Type`, `N: i64`. Голое `T` и `T: Type` — тип-параметр, `N: i64` — value-параметр.
static bool genericParamHoverIn(const std::vector<GenericParam>& generics,
                                const string& word, std::string& md,
                                const char* ownerKind, const string& ownerName) {
    for (const GenericParam& g : generics) {
        if (g.name.to_utf8() != word) continue;
        std::string bound = nodeTypeText(g.type);
        if (bound.empty()) bound = "Type";   // без явной границы неявно Type
        bool type_param = true;
        if (g.type && g.type->type == NodeTypes::NodeVariable)
            type_param = static_cast<struct NodeVariable*>(g.type)->name == "Type";
        md = "**generic param " + word + ": " + bound + "**  \n*"
            + (type_param ? "type" : "value") + " param of " + ownerKind
            + " " + ownerName + "*";
        return true;
    }
    return false;
}

// Дженерик-параметр объявленной функции (`A` в `fn sum[A, B](a: A, b: B)`):
// hover `*generic param A: Type*`, плюс имя функции-владельца.
static bool genericParamHover(LmFn* fn, const string& word, std::string& md,
                              const std::vector<Token>& tokens,
                              const std::vector<size_t>& lineStarts,
                              size_t pos) {
    if (!fn) return false;
    size_t begin = 0, end = 0;
    fnRange(fn, tokens, lineStarts, begin, end);
    if (!(end > begin && pos >= begin && pos < end)) return false;
    return genericParamHoverIn(fn->generics, word, md, "fn", fn->name.to_utf8());
}

// ---------- Hover Р С—Р С•РЎРѓРЎвЂљРЎР‚Р С•Р ВµР Р…Р С‘Р Вµ ----------

static JsonValue buildHover(const string& text, const string& path, int lspLine, int lspChar,
                            const Context& ctx,
                            const std::vector<Token>& tokens,
                            const std::vector<Node*>& tree,
                            const std::vector<Define>& defines,
                            const std::vector<Macro>& macros,
                            const std::map<String, StructSymbol>* structs,
                            const std::map<String, EnumSymbol>* enums = nullptr,
                            const std::map<unsigned long long, Type>* localTypes = nullptr) {
    (void)path;
    JsonValue res = JObject();

    size_t pos = lspPosToByteOffset(text, lspLine, lspChar);
    size_t wstart = 0, wend = 0;
    string word;
    if (!wordAt(text, pos, wstart, wend, word))
        return res; // Р С—РЎС“РЎРѓРЎвЂљР С•Р в„– Р С•Р В±РЎР‰Р ВµР С”РЎвЂљ -> null hover

    std::vector<size_t> lineStarts = buildLineStarts(text);
    JsonValue range = JObject();
    range.object["start"] = positionFromByte(text, lineStarts, wstart);
    range.object["end"]   = positionFromByte(text, lineStarts, wend);

    JsonValue contents = JObject();

    // Р В Р ВµР В·Р С•Р В»Р Р†Р С‘Р С Р С”Р С•Р Р…РЎвЂљР ВµР С”РЎРѓРЎвЂљ, Р Р† Р С”Р С•РЎвЂљР С•РЎР‚Р С•Р С Р С‘РЎвЂ°Р ВµР С РЎРѓР В»Р С•Р Р†Р С•. Р вЂўРЎРѓР В»Р С‘ Р С—Р ВµРЎР‚Р ВµР Т‘ РЎРѓР В»Р С•Р Р†Р С•Р С РЎРѓРЎвЂљР С•Р С‘РЎвЂљ РЎвЂ Р ВµР С—Р С•РЎвЂЎР С”Р В°
    // Р С‘Р СРЎвЂР Р… (Р Р…Р В°Р С—РЎР‚. `Math::sq`, `A::B::x`, `p.x`, `Vec::filled`), Р С—РЎвЂ№РЎвЂљР В°Р ВµР СРЎРѓРЎРЏ Р С—РЎР‚Р С•Р в„–РЎвЂљР С‘:
    // РЎРѓР Р…Р В°РЎвЂЎР В°Р В»Р В° Р С—Р С• namespace-Р С”Р С•Р Р…РЎвЂљР ВµР С”РЎРѓРЎвЂљР В°Р С, Р В·Р В°РЎвЂљР ВµР С Р С”Р В°Р С” РЎвЂЎР В»Р ВµР Р… РЎРѓРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚РЎвЂ№ (Р С—Р С•Р В»Р Вµ/Р СР ВµРЎвЂљР С•Р Т‘).
    const Context* searchCtx = &ctx;
    std::vector<string> owner;
    string prefix;
    std::vector<bool> callFlags;
    bool structHovered = false;
    if (memberAccessContext(text, pos, owner, prefix, nullptr, &callFlags) && !owner.empty()) {
        const Context* cur = &ctx;
        bool ok = true;
        for (const auto& nm : owner) {
            auto it = cur->variables.find(String(nm));
            if (it == cur->variables.end() ||
                it->second.value.T != Ty::Namespace) { ok = false; break; }
            cur = &std::any_cast<const Namespace&>(it->second.value.raw()).context;
        }
        if (ok) searchCtx = cur;
else {
            // С‡Р»РµРЅ СЃС‚СЂСѓРєС‚СѓСЂС‹ / РІР°СЂРёР°РЅС‚ enum: `p.x`, `p.get(вЂ¦)`, `Vec::filled`, `Types::a`
            const EnumSymbol* esym = resolveEnumOwner(ctx, enums, owner);
            if (esym) {
                auto vit = esym->enum_context.variables.find(String(word));
                if (vit != esym->enum_context.variables.end()) {
                    const ContextObject& vco = vit->second;
                    std::string md2 = "**" + esym->name.to_utf8() + "::" + word + "**";
                    if (vco.value.raw().type() == typeid(long long))
                        md2 += " = " + std::to_string(std::any_cast<long long>(vco.value.raw()));
                    md2 += "  \n*enum " + esym->name.to_utf8() + "*";
                    contents.object["value"] = JString(md2);
                    contents.object["kind"] = JString("markdown");
                    structHovered = true;
                }
            }
            if (!structHovered) {
            const StructSymbol* sym = nullptr;
            std::map<std::string, LocalCandidate> locals;
            collectParamsInRange(tokens, lineStarts, pos, locals);
            collectLetVars(tokens, lineStarts, pos, locals);
            if (resolveStructOwner(ctx, &locals, owner, &callFlags, structs, sym, localTypes)) {
                // hover РЅР° РїРѕР»РЅРѕРј СЃР»РѕРІРµ С‡Р»РµРЅР° (wordAt), Р° РЅРµ РЅР° prefix РёР· С†РµРїРѕС‡РєРё
                const StructFieldDef* fd = sym->field_def(String(word));
                if (fd) {
                    contents.object["value"] = JString("**" + structFieldText(*fd) + "**  \n*Provided by " + sym->name.to_utf8() + "*");
                    contents.object["kind"] = JString("markdown");
                    structHovered = true;
                } else {
                    auto mit = sym->methods.find(String(word));
                    if (mit != sym->methods.end()) {
                        std::string mmd;
                        appendOverloadDocs(mmd, mit->second, &methodSignature);
                        mmd += "  \n*Provided by " + sym->name.to_utf8() + "*";
                        contents.object["value"] = JString(mmd);
                        contents.object["kind"] = JString("markdown");
                        structHovered = true;
                    }
                }
            }
            }
        }
    }

    // 1. Р С™Р В»РЎР‹РЎвЂЎР ВµР Р†Р С•Р Вµ РЎРѓР В»Р С•Р Р†Р С• / РЎвЂљР С‘Р С— / Р В»Р С‘РЎвЂљР ВµРЎР‚Р В°Р В» Р С‘Р В· РЎРѓР В»Р С•Р Р†Р В°РЎР‚РЎРЏ
    bool found = structHovered || keywordHover(word, contents, range);
    if (!found) {
        // 1b. Р СџР В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚ Р С•Р В±РЎР‰Р ВµР СР В»РЎР‹РЎвЂ°Р ВµР в„– РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘: Р В·Р В°РЎвЂљР ВµР Р…РЎРЏР ВµРЎвЂљ Р С•Р Т‘Р Р…Р С•Р С‘Р СРЎвЂР Р…Р Р…РЎС“РЎР‹ Р С–Р В»Р С•Р В±Р В°Р В»РЎРЉР Р…РЎС“РЎР‹
        // Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…РЎС“РЎР‹/РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘РЎР‹ (Р Р…Р В°Р С—РЎР‚. `a` Р Р†Р Р…РЎС“РЎвЂљРЎР‚Р С‘ `fn sum[T](a: T, b: T)`).
        std::string pv;
        LmFn* enc =
            enclosingFunction(tree, tokens, lineStarts, pos);
        if (enc && paramHover(enc, *searchCtx, ctx, word, pv)) {
            contents.object["value"] = JString(pv);
            contents.object["kind"] = JString("markdown");
            found = true;
        }
        // 1b2. Р”Р¶РµРЅРµСЂРёРє-РїР°СЂР°РјРµС‚СЂ (`A` РІ `fn sum[A, B]`): С‚РѕР¶Рµ РІРёРґРµРЅ РІ С‚РµР»Рµ С„СѓРЅРєС†РёРё.
        if (!found && enc && genericParamHover(enc, word, pv, tokens, lineStarts, pos)) {
            contents.object["value"] = JString(pv);
            contents.object["kind"] = JString("markdown");
            found = true;
        }
    }
    if (!found) {
        // 1b3. РџРѕР»Рµ РёР»Рё РјРµС‚РѕРґ РІРЅСѓС‚СЂРё РѕР±СЉСЏРІР»РµРЅРёСЏ СЃС‚СЂСѓРєС‚СѓСЂС‹ (`struct Foo { let a; fn bar() ... }`):
        //     СЃР»РѕРІРѕ СЃС‚РѕРёС‚ РІ С‚РµР»Рµ struct -> РїРѕРєР°Р·С‹РІР°РµРј hover РѕС‚ StructSymbol.
        EnclosingStruct es;
        if (findEnclosingStruct(tokens, lineStarts, pos, es)) {
            auto sit = structs->find(String(es.name));
            if (sit != structs->end()) {
                const StructSymbol& sym = sit->second;
                const StructFieldDef* fd = sym.field_def(String(word));
                if (fd) {
                    contents.object["value"] = JString("**" + structFieldText(*fd) + "**  \n*Provided by " + es.name + "*");
                    contents.object["kind"] = JString("markdown");
                    found = true;
                } else {
                    auto mit = sym.methods.find(String(word));
                    if (mit != sym.methods.end()) {
                        std::string mmd;
                        appendOverloadDocs(mmd, mit->second, &methodSignature);
                        mmd += "  \n*Provided by " + es.name + "*";
                        contents.object["value"] = JString(mmd);
                        contents.object["kind"] = JString("markdown");
                        found = true;
                    }
                }
                // 1b4. Дженерик-параметр структуры (`T` в `struct[T] Number`):
                //      и в декларации, и в полях/возвратах/методах тела структуры.
                //      Собственный дженерик метода (проверка 1b2) имеет приоритет.
                if (!found) {
                    std::string gmd;
                    if (genericParamHoverIn(sym.generics, word, gmd,
                                            "struct", sym.name.to_utf8())) {
                        contents.object["value"] = JString(gmd);
                        contents.object["kind"] = JString("markdown");
                        found = true;
                    }
                }
            }
        }
    }
    if (!found) {
        // 2. Р РЋР С‘Р СР Р†Р С•Р В»РЎвЂ№ Р С‘Р В· Р С”Р С•Р Р…РЎвЂљР ВµР С”РЎРѓРЎвЂљР В° (Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…РЎвЂ№Р Вµ/РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘/РЎвЂљР С‘Р С—РЎвЂ№/namespace)
        found = contextHover(*searchCtx, word, structs, contents, range,
                              findFunctionDocInTree(tree, word),
                              findStructDocInTree(tree, word), enums);
    }
    if (!found) {
        // 2b. Р вЂєР С•Р С”Р В°Р В»РЎРЉР Р…Р В°РЎРЏ Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…Р В°РЎРЏ (let / ref let / for) Р Р†Р Р…РЎС“РЎвЂљРЎР‚Р С‘ РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘:
        //     Р С‘ Р С‘РЎРѓР С—Р С•Р В»РЎРЉР В·Р С•Р Р†Р В°Р Р…Р С‘Р Вµ, Р С‘ Р С‘Р СРЎРЏ РЎРѓР В°Р СР С•Р С–Р С• Р С•Р В±РЎР‰РЎРЏР Р†Р В»Р ВµР Р…Р С‘РЎРЏ (declOff == wstart).
        //     wstart + 1 Р Т‘Р ВµР В»Р В°Р ВµРЎвЂљ Р В±Р В»Р С‘Р В·Р С”Р С•Р Вµ РЎРѓР В»Р ВµР Р†Р В° Р С•Р В±РЎР‰РЎРЏР Р†Р В»Р ВµР Р…Р С‘Р Вµ Р Р†Р С‘Р Т‘Р С‘Р СРЎвЂ№Р С (declOff < pos).
        std::map<std::string, LocalCandidate> localVars;
        collectLetVars(tokens, lineStarts, wstart + 1, localVars);
        auto lv = localVars.find(word);
        if (lv != localVars.end()) {
            std::string md;
            std::string d = lv->second.detail;
            if (localTypes && lv->second.declKey != 0 && !d.empty() && d != "ref" && d.compare(0, 4, "ref ") != 0) {
                auto lt = localTypes->find(lv->second.declKey);
                if (lt != localTypes->end())
                    d = lt->second.toString().to_utf8();
            }
            if (d.compare(0, 4, "ref ") == 0)
                md = "**ref let " + word + ": " + d.substr(4) + "**";
            else if (d == "ref")
                md = "**ref let " + word + "**";
            else if (!d.empty())
                md = "**let " + word + ": " + d + "**";
            else
                md = "**let " + word + "**";
            contents.object["value"] = JString(md);
            contents.object["kind"] = JString("markdown");
            found = true;
        }
    }
    // 1c. #define / #macro: Р Т‘Р С‘РЎР‚Р ВµР С”РЎвЂљР С‘Р Р†РЎвЂ№ РЎР‚Р В°РЎРѓР С”РЎР‚РЎвЂ№Р Р†Р В°РЎР‹РЎвЂљРЎРѓРЎРЏ Р С—РЎР‚Р ВµР С—РЎР‚Р С•РЎвЂ Р ВµРЎРѓРЎРѓР С•РЎР‚Р С•Р С Р Р† РЎвЂљР С•Р С”Р ВµР Р…Р В°РЎвЂ¦,
    //    Р С—Р С•РЎРЊРЎвЂљР С•Р СРЎС“ Р С‘РЎвЂ¦ Р Р…Р ВµРЎвЂљ Р Р…Р С‘ Р Р† AST, Р Р…Р С‘ Р Р† Р С”Р С•Р Р…РЎвЂљР ВµР С”РЎРѓРЎвЂљР В°РЎвЂ¦ РІР‚вЂќ Р С‘РЎвЂ°Р ВµР С Р С—Р С• Р С‘Р СР ВµР Р…Р С‘ Р С‘Р В· pp.
    if (!found) {
        auto joinTokens = [](const std::vector<Token>& ts) {
            string s;
            for (const auto& t : ts) {
                if (!s.empty()) s += " ";
                s += t.value.to_utf8();
            }
            return s;
        };
        for (const auto& d : defines) {
            if (d.name_token.value.to_utf8() != word) continue;
            contents.object["value"] = JString(
                "**#define  " + word + " = " + joinTokens(d.tokens) + "**");
            contents.object["kind"] = JString("markdown");
            found = true;
            break;
        }
        if (!found) {
            for (const auto& m : macros) {
                if (m.name_token.value.to_utf8() != word) continue;
                string args;
                for (const auto& a : m.args_tokens) {
                    if (!args.empty()) args += ", ";
                    args += a.token.value.to_utf8();
                }
                contents.object["value"] = JString(
                    "**#macro " + word + "(" + args + ")**");
                contents.object["kind"] = JString("markdown");
                found = true;
                break;
            }
        }
    }
    // 3. Р вЂўРЎРѓР В»Р С‘ Р Р…Р Вµ Р Р…Р В°РЎв‚¬Р В»Р С‘ Р Р† Р С–Р В»Р С•Р В±Р В°Р В»РЎРЉР Р…Р С•Р С Р С”Р С•Р Р…РЎвЂљР ВµР С”РЎРѓРЎвЂљР Вµ Р С‘ Р Р…Р ВµРЎвЂљ РЎРЏР Р†Р Р…Р С•Р в„– РЎвЂ Р ВµР С—Р С•РЎвЂЎР С”Р С‘ '::',
    //    Р С‘РЎвЂ°Р ВµР С РЎРѓР В»Р С•Р Р†Р С• Р Р†Р Р…РЎС“РЎвЂљРЎР‚Р С‘ Р Р†РЎРѓР ВµРЎвЂ¦ namespace-Р С”Р С•Р Р…РЎвЂљР ВµР С”РЎРѓРЎвЂљР С•Р Р† (hover Р Р…Р В° Р С•Р В±РЎР‰РЎРЏР Р†Р В»Р ВµР Р…Р С‘Р Вµ
    //    РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘/Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…Р С•Р в„– Р Р†Р Р…РЎС“РЎвЂљРЎР‚Р С‘ namespace).
    if (!found && owner.empty()) {
        for (const auto& kv : ctx.variables) {
            if (kv.second.value.T != Ty::Namespace) continue;
            const Context& nsCtx =
                std::any_cast<const Namespace&>(kv.second.value.raw()).context;
            if (contextHover(nsCtx, word, structs, contents, range,
                             findFunctionDocInTree(tree, word),
                             findStructDocInTree(tree, word), enums)) { found = true; break; }
        }
    }

    if (!found)
        return res; // Р Р…Р С‘РЎвЂЎР ВµР С–Р С• Р Р…Р Вµ Р Р…Р В°РЎв‚¬Р В»Р С‘ -> null hover

    res.object["contents"] = contents;
    res.object["range"] = range;
    return res;
}

// ---------- Semantic Tokens ----------

static int mapTokenSemanticType(TypeOfToken tt) {
    switch (tt) {
        case KEYWORD:        return 0;
        case NUMBER:         return 1;
        case STRING:         return 2;
        case COMMENT:
        case MULTI_COMMENT:  return 3;
        case OPERATOR:       return 4;
        case DIRECTIVE:      return 10;
        case VA_ARGS:        return 4;
        case LITERAL:        return 7;
        default:             return -1;
    }
}

// Р В Р ВµР С”РЎС“РЎР‚РЎРѓР С‘Р Р†Р Р…РЎвЂ№Р в„– Р С•Р В±РЎвЂ¦Р С•Р Т‘ AST Р Т‘Р В»РЎРЏ РЎРѓР В±Р С•РЎР‚Р В° Р С‘Р СРЎвЂР Р… Р С—Р В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚Р С•Р Р†
// РџР°СЂР°РјРµС‚СЂС‹ С„СѓРЅРєС†РёРё: РёРјСЏ РїР°СЂР°РјРµС‚СЂР° РґРµР№СЃС‚РІРёС‚РµР»СЊРЅРѕ РўРћР›Р¬РљРћ РІ РѕР±Р»Р°СЃС‚Рё РІРёРґРёРјРѕСЃС‚Рё
// СЃРІРѕРµР№ С„СѓРЅРєС†РёРё (РѕС‚ РёРјРµРЅРё С„СѓРЅРєС†РёРё РґРѕ Р·Р°РєСЂС‹РІР°СЋС‰РµР№ '}' С‚РµР»Р°). Р Р°РЅСЊС€Рµ РїР°СЂР°РјРµС‚СЂС‹
// СЃРѕР±РёСЂР°Р»РёСЃСЊ РІ РіР»РѕР±Р°Р»СЊРЅРѕРµ РјРЅРѕР¶РµСЃС‚РІРѕ Р±РµР· СѓС‡С‘С‚Р° РєРѕРЅС‚РµРєСЃС‚Р°, Рё РёРјСЏ РїР°СЂР°РјРµС‚СЂР°
// (РЅР°РїСЂРёРјРµСЂ `first`, `second` РІ `fn create(first, second)`) РїРµСЂРµРєСЂР°С€РёРІР°Р»Рѕ
// РѕРґРЅРѕРёРјС‘РЅРЅС‹Рµ РјРµС‚РѕРґС‹/РІС‹Р·РѕРІС‹ РІРѕ РІСЃС‘Рј С„Р°Р№Р»Рµ РІ "parameter".
struct ParamScope {
    size_t begin = 0;  // Р±Р°Р№С‚РѕРІС‹Р№ offset РёРјРµРЅРё С„СѓРЅРєС†РёРё
    size_t end = 0;    // Р±Р°Р№С‚РѕРІС‹Р№ offset РџРћРЎР›Р• Р·Р°РєСЂС‹РІР°СЋС‰РµР№ '}' С‚РµР»Р°
    std::set<std::string> names;
};

// Р—Р°РєСЂС‹РІР°СЋС‰Р°СЏ '}' С‚РµР»Р° С„СѓРЅРєС†РёРё: СЃРєРІРѕР·РЅСЏРє РїРѕ С‚РѕРєРµРЅР°Рј РѕС‚ С‚РѕРєРµРЅР° СЃ РєРѕРѕСЂРґРёРЅР°С‚Р°РјРё
// [line, index] РґРѕ РїРµСЂРІРѕР№ РїР°СЂРЅРѕР№ С„РёРіСѓСЂРЅРѕР№ СЃРєРѕР±РєРё. 'fallback' вЂ” РµСЃР»Рё СЃРєРѕР±РєР°
// РЅРµ РЅР°Р№РґРµРЅР° (РѕР±СЂС‹РІ С„Р°Р№Р»Р°), РѕСЃС‚Р°РІР»СЏРµРј РєРѕРЅРµС† РѕС…РІР°С‚С‹РІР°СЋС‰РµРіРѕ СЃРєРѕСѓРїР°.
static size_t declBodyEndOffset(const std::vector<Token>& tokens,
                                const std::vector<size_t>& lineStarts,
                                int line, int index, size_t fallback) {
    for (size_t ti = 0; ti < tokens.size(); ++ti)
        if (tokens[ti].position.line == line
                && tokens[ti].position.index == index) {
            int depth = 0;
            bool opened = false;
            for (size_t tj = ti + 1; tj < tokens.size(); ++tj) {
                const Token& t = tokens[tj];
                if (t.type == TypeOfToken::BRACE_LEFT_CURVE) { depth++; opened = true; }
                else if (t.type == TypeOfToken::BRACE_RIGHT_CURVE) {
                    if (depth > 0) depth--;
                    if (depth == 0 && opened) {
                        size_t bl = (size_t)t.position.line;
                        return (bl < lineStarts.size() ? lineStarts[bl] : 0)
                             + t.position.index + t.position.size;
                    }
                }
            }
            break;
        }
    return fallback;
}

static void collectParamScopes(Node* n,
                               const std::vector<Token>& tokens,
                               const std::vector<size_t>& lineStarts,
                               std::vector<ParamScope>& out) {
    if (!n) return;
    switch (n->type) {
        case NodeTypes::NodeFunctionDeclaration: {
            auto* fn = static_cast<struct NodeFunctionDeclaration*>(n);
            ParamScope sc;
            for (auto& p : fn->params) sc.names.insert(p.name.to_utf8());
            size_t fallback = out.empty() ? 0 : out.back().end;
            if (fn->name_token.type != TypeOfToken::DUMMY) {
                int nl = fn->name_token.position.line;
                size_t nlOff = (nl >= 0 && (size_t)nl < lineStarts.size())
                    ? lineStarts[(size_t)nl] : 0;
                sc.begin = nlOff + fn->name_token.position.index;
                sc.end = declBodyEndOffset(tokens, lineStarts, nl,
                                           fn->name_token.position.index, fallback);
            } else {
                sc.begin = 0;
                sc.end = fallback;
            }
            if (!sc.names.empty() && sc.end > sc.begin)
                out.push_back(sc);
            collectParamScopes(fn->body, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeStatementsScope: {
            auto* s = static_cast<struct NodeStatementsScope*>(n);
            for (auto* child : s->nodes) collectParamScopes(child, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeScope: {
            auto* s = static_cast<struct NodeScope*>(n);
            collectParamScopes(s->node, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeIf: {
            auto* i = static_cast<struct NodeIf*>(n);
            collectParamScopes(i->then_body, tokens, lineStarts, out);
            collectParamScopes(i->else_body, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeWhile: {
            auto* w = static_cast<struct NodeWhile*>(n);
            collectParamScopes(w->body, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeFor: {
            auto* f = static_cast<struct NodeFor*>(n);
            collectParamScopes(f->body, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeForEach: {
            auto* f = static_cast<struct NodeForEach*>(n);
            collectParamScopes(f->body, tokens, lineStarts, out);
            break;
        }
case NodeTypes::NodeLambda: {
            auto* l = static_cast<struct NodeLambda*>(n);
            // Р»СЏРјР±РґР° РЅРµ РЅРµСЃС‘С‚ С‚РѕРєРµРЅРѕРІ РїРѕР»РѕР¶РµРЅРёСЏ вЂ” РїР°СЂР°РјРµС‚СЂС‹ РїСЂРёРїРёСЃС‹РІР°РµРј СЃРєРѕСѓРїСѓ
            // РѕС…РІР°С‚С‹РІР°СЋС‰РµР№ С„СѓРЅРєС†РёРё (С€РёСЂРµ, С‡РµРј РїРѕР»РѕР¶РµРЅРѕ, РЅРѕ РЅРµ Р»РѕРјР°РµС‚ РёРјРµРЅР°
            // С„СѓРЅРєС†РёР№, РІ РѕС‚Р»РёС‡РёРµ РѕС‚ РіР»РѕР±Р°Р»СЊРЅРѕРіРѕ РјРЅРѕР¶РµСЃС‚РІР°)
            if (!out.empty())
                for (auto& p : l->params) out.back().names.insert(p.name.to_utf8());
            collectParamScopes(l->body, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeReturn:
            collectParamScopes(static_cast<struct NodeReturn*>(n)->expr, tokens, lineStarts, out);
            break;
        case NodeTypes::NodeNamespace:
            collectParamScopes(static_cast<struct NodeNamespace*>(n)->body, tokens, lineStarts, out);
            break;
        case NodeTypes::NodeStructDeclaration:
            collectParamScopes(static_cast<struct NodeStructDeclaration*>(n)->body, tokens, lineStarts, out);
            break;
        case NodeTypes::NodeModifier:
            collectParamScopes(static_cast<struct NodeModifier*>(n)->node, tokens, lineStarts, out);
            break;
        default: break;
    }
}

static bool isParamNameAt(const std::string& name, size_t off,
                          const std::vector<ParamScope>& scopes) {
    for (const auto& sc : scopes)
        if (off >= sc.begin && off < sc.end && sc.names.count(name))
            return true;
    return false;
}

// Р В Р ВµР С”РЎС“РЎР‚РЎРѓР С‘Р Р†Р Р…РЎвЂ№Р в„– Р С•Р В±РЎвЂ¦Р С•Р Т‘ AST: Р С‘Р СР ВµР Р…Р В° РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р в„–/namespace/РЎвЂљР С‘Р С—Р С•Р Р† Р С—Р С• Р вЂ™Р РЋР вЂўР СљР Р€ Р Т‘Р ВµРЎР‚Р ВµР Р†РЎС“
// (Р Р†Р С”Р В»РЎР‹РЎвЂЎР В°РЎРЏ Р Р†Р В»Р С•Р В¶Р ВµР Р…Р Р…РЎвЂ№Р Вµ Р Р† ret/namespace/struct), РЎвЂЎРЎвЂљР С•Р В±РЎвЂ№ Р С•Р Р…Р С‘ Р С—Р С•Р Т‘РЎРѓР Р†Р ВµРЎвЂЎР С‘Р Р†Р В°Р В»Р С‘РЎРѓРЎРЉ
// Р С”Р В°Р С” Р Р…Р В°РЎРѓРЎвЂљР С•РЎРЏРЎвЂ°Р С‘Р Вµ РЎРѓР С‘Р СР Р†Р С•Р В»РЎвЂ№, Р В° Р Р…Р Вµ Р С—Р ВµРЎР‚Р ВµР СР ВµР Р…Р Р…РЎвЂ№Р Вµ.
static void collectSymbolNames(Node* n,
                               std::set<std::string>& fns,
                               std::set<std::string>& nss,
                               std::set<std::string>& types) {
    if (!n) return;
    switch (n->type) {
        case NodeTypes::NodeFunctionDeclaration: {
            auto* fn = static_cast<struct NodeFunctionDeclaration*>(n);
            if (fn->name_token.type != TypeOfToken::DUMMY)
                fns.insert(fn->name.to_utf8());
            collectSymbolNames(fn->body, fns, nss, types);
            break;
        }
        case NodeTypes::NodeNamespace: {
            auto* ns = static_cast<struct NodeNamespace*>(n);
            if (ns->name_token.type != TypeOfToken::DUMMY && !ns->name.empty())
                nss.insert(ns->name.to_utf8());
            collectSymbolNames(ns->body, fns, nss, types);
            break;
        }
        case NodeTypes::NodeStructDeclaration: {
            auto* st = static_cast<struct NodeStructDeclaration*>(n);
            if (st->name_token.type != TypeOfToken::DUMMY)
                types.insert(st->name.to_utf8());
            collectSymbolNames(st->body, fns, nss, types);
            break;
        }
        case NodeTypes::NodeReturn:
            collectSymbolNames(static_cast<struct NodeReturn*>(n)->expr, fns, nss, types);
            break;
        case NodeTypes::NodeStatementsScope: {
            auto* s = static_cast<struct NodeStatementsScope*>(n);
            for (auto* child : s->nodes) collectSymbolNames(child, fns, nss, types);
            break;
        }
        case NodeTypes::NodeScope: {
            auto* s = static_cast<struct NodeScope*>(n);
            collectSymbolNames(s->node, fns, nss, types);
            break;
        }
        case NodeTypes::NodeIf: {
            auto* i = static_cast<struct NodeIf*>(n);
            collectSymbolNames(i->then_body, fns, nss, types);
            collectSymbolNames(i->else_body, fns, nss, types);
            break;
        }
        case NodeTypes::NodeWhile: {
            auto* w = static_cast<struct NodeWhile*>(n);
            collectSymbolNames(w->body, fns, nss, types);
            break;
        }
        case NodeTypes::NodeFor: {
            auto* f = static_cast<struct NodeFor*>(n);
            collectSymbolNames(f->body, fns, nss, types);
            break;
        }
        case NodeTypes::NodeForEach: {
            auto* f = static_cast<struct NodeForEach*>(n);
            collectSymbolNames(f->body, fns, nss, types);
            break;
        }
        case NodeTypes::NodeLambda: {
            auto* l = static_cast<struct NodeLambda*>(n);
            collectSymbolNames(l->body, fns, nss, types);
            break;
        }
        case NodeTypes::NodeModifier:
            collectSymbolNames(static_cast<struct NodeModifier*>(n)->node, fns, nss, types);
            break;
        default: break;
    }
}

// ---------- GoTo Definition (textDocument/definition) ----------
// Resolves the token a definition Location should point at. `outTok` keeps its
// file_path, so cross-file (#include) jumps are possible; false -> no definition.

// Byte-position of a token in the def file -> UTF-16 LSP character on its line.
static JsonValue tokenLspPos(const Token& tok, const std::vector<size_t>& dls,
                             const string& fileText, size_t extra = 0) {
    JsonValue p = JObject();
    p.object["line"] = JNumber(tok.position.line);
    int chars = 0;
    if (tok.position.line >= 0 && (size_t)tok.position.line < dls.size()) {
        size_t base = dls[(size_t)tok.position.line];
        size_t cut = (size_t)tok.position.index + extra;
        for (size_t i = 0; i < cut && base + i < fileText.size();) {
            unsigned char c = (unsigned char)fileText[base + i];
            int cl = (c < 0x80) ? 1
                  : ((c & 0xE0) == 0xC0) ? 2
                  : ((c & 0xF0) == 0xE0) ? 3
                  : ((c & 0xF8) == 0xF0) ? 4 : 1;
            chars += (cl >= 4) ? 2 : 1;
            i += cl;
        }
    }
    p.object["character"] = JNumber(chars);
    return p;
}

static string uriFromPath(const string& path) {
    string res = "file:///";
    for (char c : path) {
        if (c == '\\') res += '/';
        else if (c == ' ') res += "%20";
        else res += c;
    }
    return res;
}

static string fileTail(const string& p) {
    size_t s = p.find_last_of("/\\");
    return (s == string::npos) ? p : p.substr(s + 1);
}

static bool sameFile(const string& a, const string& b) {
    return fileTail(a) == fileTail(b);
}

// РќРѕСЂРјР°Р»РёР·Р°С†РёСЏ РїСѓС‚Рё СЃРёРјРІРѕР»Р° РёР· РґСЂСѓРіРѕРіРѕ С„Р°Р№Р»Р° (#include): РѕС‚РЅРѕСЃРёС‚РµР»СЊРЅС‹Р№
// РІРєР»СЋС‡Р°РµРјС‹Р№ РїСѓС‚СЊ -> Р°Р±СЃРѕР»СЋС‚РЅС‹Р№ (Р±Р°Р·Р° = CWD СЃРµСЂРІРµСЂР°, РєР°Рє РІ buildDefinition),
// СЃР»СЌС€Рё -> '/'. РџСѓСЃС‚РѕР№ РїСѓС‚СЊ РѕСЃС‚Р°С‘С‚СЃСЏ РїСѓСЃС‚С‹Рј.
static string absIncludePath(const string& p) {
    if (p.empty()) return p;
    bool hasDrive = p.size() >= 2 && p[1] == ':';
    bool rooted   = p[0] == '/' || p[0] == '\\';
    string out = p;
    if (!hasDrive && !rooted) {
        char buf[MAX_PATH];
        DWORD n = GetCurrentDirectoryA(MAX_PATH, buf);
        if (n > 0 && n < MAX_PATH) {
            string base(buf);
            if (base[base.size() - 1] != '\\') base += "\\";
            out = base + out;
        }
    }
    for (auto& c : out) if (c == '\\') c = '/';
    return out;
}

// Р Р°РІРµРЅСЃС‚РІРѕ РїСѓС‚РµР№ С„Р°Р№Р»РѕРІ: РїРѕ РЅРѕСЂРјР°Р»РёР·РѕРІР°РЅРЅРѕРјСѓ Р°Р±СЃРѕР»СЋС‚РЅРѕРјСѓ РїСѓС‚Рё,
// СЂРµРіРёСЃС‚СЂРѕРЅРµР·Р°РІРёСЃРёРјРѕ (Windows).
static bool pathsEqual(const string& a, const string& b) {
    string x = absIncludePath(a);
    string y = absIncludePath(b);
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i)
        if (tolower((unsigned char)x[i]) != tolower((unsigned char)y[i]))
            return false;
    return true;
}

static size_t tokenIndexAt(const std::vector<Token>& tokens, int line, int index) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        if (t.type == DUMMY || t.type == END_OF_FILE) continue;
        if (t.position.line == line && t.position.index == index) return i;
    }
    return tokens.size();
}

static size_t braceCloseIdx(const std::vector<Token>& tokens, size_t openIdx) {
    int depth = 0;
    for (size_t i = openIdx; i < tokens.size(); ++i) {
        if (tokens[i].type == BRACE_LEFT_CURVE) depth++;
        else if (tokens[i].type == BRACE_RIGHT_CURVE) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return tokens.size();
}

// Searches direct body members of a struct/enum/namespace starting at `nameIdx`
// (token of the decl's name). Handles members written as
//   fn swap(...) / let first_: FirstT / strict let first_: FirstT / const X = 3
// and bare enum-variant LITERALs. Returns the member's name token.
static bool scanBodyMemberToken(const std::vector<Token>& tokens, size_t nameIdx,
                                const string& word, Token& outTok) {
    if (nameIdx >= tokens.size()) return false;
    static const std::set<std::string> memberKw = {
        "fn", "let", "const", "struct", "enum", "namespace",
        "strict", "private", "static", "final", "public", "var", "mut"
    };
    static const std::set<std::string> nameMods = {
        "strict", "const", "private", "static", "final", "fn", "let", "var", "mut", "public"
    };
    size_t end = braceCloseIdx(tokens, nameIdx);
    if (end >= tokens.size()) return false;
    int depth = 0;
    for (size_t i = nameIdx + 1; i < end; ++i) {
        const Token& t = tokens[i];
        if (t.type == BRACE_LEFT_CURVE) { depth++; continue; }
        if (t.type == BRACE_RIGHT_CURVE) { if (depth > 0) depth--; continue; }
        if (t.type == DIRECTIVE || t.type == COMMENT || t.type == MULTI_COMMENT) continue;
        if (depth != 1) continue;
        if (t.type == KEYWORD && memberKw.count(t.value.to_utf8())) {
            for (size_t j = i + 1; j < end; ++j) {
                const Token& u = tokens[j];
                if (u.type == BRACE_LEFT_CURVE || u.type == BRACE_RIGHT_CURVE) break;
                if (u.type == LITERAL) {
                    if (u.value.to_utf8() == word) { outTok = u; return true; }
                    break;
                }
                if (u.type == KEYWORD && nameMods.count(u.value.to_utf8())) continue;
                break;
            }
        } else if (t.type == LITERAL && t.value.to_utf8() == word) {
            outTok = t;   // bare enum variant
            return true;
        }
    }
    return false;
}

// Top-level decl whose name follows a keyword, e.g. `fn create(...)` or
// `namespace Data {`. Returns the LITERAL name token.
static bool findTopKeywordDeclTok(const std::vector<Token>& tokens,
                                  const string& wantFile,
                                  const string& kw, const string& word,
                                  Token& outTok) {
    int depth = 0;
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        const Token& t = tokens[i];
        if (t.type == BRACE_LEFT_CURVE) { depth++; continue; }
        if (t.type == BRACE_RIGHT_CURVE) { if (depth > 0) depth--; continue; }
        if (depth != 0) continue;
        if (t.type != KEYWORD || t.value.to_utf8() != kw) continue;
        const Token& n = tokens[i + 1];
        if (n.type != LITERAL) continue;
        if (!n.position.file_path.to_utf8().empty()
                && !sameFile(n.position.file_path.to_utf8(), wantFile))
            continue;
        if (n.value.to_utf8() == word) { outTok = n; return true; }
    }
    return false;
}

// Param-name token inside a function signature: a LITERAL right after '(' or ','.
static bool scanParamNameToken(LmFn* fn, const std::vector<Token>& tokens,
                               const string& word, Token& outTok) {
    if (!fn || fn->params.empty()) return false;
    bool wanted = false;
    for (auto& p : fn->params)
        if (p.name.to_utf8() == word) { wanted = true; break; }
    if (!wanted) return false;
    size_t ob = tokens.size();
    if (fn->args_open_token.type != TypeOfToken::DUMMY) {
        ob = tokenIndexAt(tokens, fn->args_open_token.position.line,
                          fn->args_open_token.position.index);
    }
    if (ob >= tokens.size()) {
        size_t ni = tokenIndexAt(tokens, fn->name_token.position.line,
                                 fn->name_token.position.index);
        for (size_t i = ni; i < tokens.size(); ++i)
            if (tokens[i].type == BRACE_LEFT) { ob = i; break; }
    }
    if (ob >= tokens.size()) return false;
    bool expectName = true;
    for (size_t i = ob + 1; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        if (t.type == BRACE_LEFT) continue;
        if (t.type == BRACE_RIGHT) break;
        if (t.type == BRACE_LEFT_RECT || t.type == BRACE_RIGHT_RECT) continue;
        if (t.type == OPERATOR) {
            string v = t.value.to_utf8();
            if (v == ":") { expectName = false; continue; }
            if (v == ",") { expectName = true; continue; }
            if (v == "=") {
                int nested = 0;
                while (++i < tokens.size()) {
                    if (tokens[i].type == BRACE_LEFT) nested++;
                    else if (tokens[i].type == BRACE_RIGHT) { if (nested) nested--; else break; }
                    else if (nested == 0 && tokens[i].type == OPERATOR
                             && tokens[i].value.to_utf8() == ",") break;
                }
                expectName = true;
                continue;
            }
        }
        if (t.type != LITERAL) continue;
        if (expectName) {
            if (t.value.to_utf8() == word) { outTok = t; return true; }
            expectName = false;
        }
    }
    return false;
}

// All enclosing functions whose byte range contains `off`, innermost LAST.
static void collectEnclosingFnsWalk(Node* n, const std::vector<Token>& tokens,
                                    const std::vector<size_t>& lineStarts,
                                    size_t off, std::vector<LmFn*>& chain) {
    if (!n) return;
    switch (n->type) {
        case NodeTypes::NodeFunctionDeclaration: {
            auto* f = static_cast<struct NodeFunctionDeclaration*>(n);
            size_t b = 0, e = 0;
            fnRange(f, tokens, lineStarts, b, e);
            if (e > b && off >= b && off < e) {
                chain.push_back(f);
                collectEnclosingFnsWalk(f->body, tokens, lineStarts, off, chain);
            }
            return;
        }
        case NodeTypes::NodeNamespace:
            collectEnclosingFnsWalk(static_cast<struct NodeNamespace*>(n)->body,
                                    tokens, lineStarts, off, chain);
            return;
        case NodeTypes::NodeStatementsScope: {
            auto* s = static_cast<struct NodeStatementsScope*>(n);
            for (auto* c : s->nodes)
                collectEnclosingFnsWalk(c, tokens, lineStarts, off, chain);
            return;
        }
        case NodeTypes::NodeScope:
            collectEnclosingFnsWalk(static_cast<struct NodeScope*>(n)->node,
                                    tokens, lineStarts, off, chain);
            return;
        case NodeTypes::NodeIf: {
            auto* i = static_cast<struct NodeIf*>(n);
            collectEnclosingFnsWalk(i->then_body, tokens, lineStarts, off, chain);
            collectEnclosingFnsWalk(i->else_body, tokens, lineStarts, off, chain);
            return;
        }
        case NodeTypes::NodeWhile:
            collectEnclosingFnsWalk(static_cast<struct NodeWhile*>(n)->body,
                                    tokens, lineStarts, off, chain);
            return;
        case NodeTypes::NodeFor:
            collectEnclosingFnsWalk(static_cast<struct NodeFor*>(n)->body,
                                    tokens, lineStarts, off, chain);
            return;
        case NodeTypes::NodeForEach:
            collectEnclosingFnsWalk(static_cast<struct NodeForEach*>(n)->body,
                                    tokens, lineStarts, off, chain);
            return;
        case NodeTypes::NodeLambda:
            collectEnclosingFnsWalk(static_cast<struct NodeLambda*>(n)->body,
                                    tokens, lineStarts, off, chain);
            return;
        case NodeTypes::NodeStructDeclaration:
            collectEnclosingFnsWalk(static_cast<struct NodeStructDeclaration*>(n)->body,
                                    tokens, lineStarts, off, chain);
            return;
        case NodeTypes::NodeModifier:
            collectEnclosingFnsWalk(static_cast<struct NodeModifier*>(n)->node,
                                    tokens, lineStarts, off, chain);
            return;
        case NodeTypes::NodeReturn:
            collectEnclosingFnsWalk(static_cast<struct NodeReturn*>(n)->expr,
                                    tokens, lineStarts, off, chain);
            return;
        default: break;
    }
}

// Resolves the word under cursor to a definition token (may live in another file).
static bool definitionToken(const string& text, size_t pos,
                            const Context& ctx,
                            const std::vector<Token>& tokens,
                            const std::vector<Node*>& tree,
                            const std::vector<size_t>& lineStarts,
                            const std::vector<Define>& defines,
                            const std::vector<Macro>& macros,
                            const std::map<String, StructSymbol>* structs,
                            const std::map<String, EnumSymbol>* enums,
                            const string& curPath, Token& outTok,
                            const std::map<unsigned long long, Type>* localTypes = nullptr) {
    size_t wordStart = pos;
    while (wordStart > 0 && isIdentChar(text[wordStart - 1])) wordStart--;
    size_t wordEnd = pos;
    while (wordEnd < text.size() && isIdentChar(text[wordEnd])) wordEnd++;
    if (wordStart == wordEnd) return false;
    string word = text.substr(wordStart, wordEnd - wordStart);

    std::map<std::string, LocalCandidate> locals;
    collectLetVars(tokens, lineStarts, wordStart, locals);
    collectParamsInRange(tokens, lineStarts, wordStart, locals);

    std::vector<ParamScope> paramScopes;
    for (auto* node : tree) collectParamScopes(node, tokens, lineStarts, paramScopes);

    std::vector<LmFn*> chain;
    for (auto* node : tree)
        collectEnclosingFnsWalk(node, tokens, lineStarts, wordStart, chain);

    // 0) member chain: enum variant / struct field / method / namespace member
    std::vector<string> owner;
    string prefix;
    if (memberAccessContext(text, pos, owner, prefix)) {
        const EnumSymbol* es = resolveEnumOwner(ctx, enums, owner);
        if (es && es->name_token.type != TypeOfToken::DUMMY) {
            size_t nameIdx = tokenIndexAt(tokens, es->name_token.position.line,
                                          es->name_token.position.index);
            if (nameIdx < tokens.size() && scanBodyMemberToken(tokens, nameIdx, word, outTok))
                return true;
        }

        const StructSymbol* sym = nullptr;
        resolveStructOwner(ctx, &locals, owner, nullptr, structs, sym, localTypes);
        if (sym && sym->name_token.type != TypeOfToken::DUMMY) {
            auto mit = sym->methods.find(String(word));
            if (mit != sym->methods.end() && !mit->second.functions.empty()
                    && mit->second.functions.back().decl_token.type != TypeOfToken::DUMMY) {
                outTok = mit->second.functions.back().decl_token;
                return true;
            }
            size_t nameIdx = tokenIndexAt(tokens, sym->name_token.position.line,
                                          sym->name_token.position.index);
            if (nameIdx < tokens.size() && scanBodyMemberToken(tokens, nameIdx, word, outTok))
                return true;
        }

        // namespace chain: Data::Math::sq
        const Context* nsc = &ctx;
        bool isNs = true;
        for (const auto& nm : owner) {
            auto it = nsc->variables.find(String(nm));
            if (it == nsc->variables.end() || it->second.value.T != Ty::Namespace) {
                isNs = false;
                break;
            }
            nsc = &std::any_cast<const Namespace&>(it->second.value.raw()).context;
        }
        if (isNs) {
            Token base;
            if (findTopKeywordDeclTok(tokens, curPath, "namespace", owner[0], base)) {
                size_t nameIdx = tokenIndexAt(tokens, base.position.line, base.position.index);
                bool ok = (nameIdx < tokens.size());
                for (size_t oi = 1; ok && oi < owner.size(); ++oi) {
                    Token inner;
                    ok = scanBodyMemberToken(tokens, nameIdx, owner[oi], inner);
                    if (ok) nameIdx = tokenIndexAt(tokens, inner.position.line, inner.position.index);
                }
                if (ok && nameIdx < tokens.size()
                        && scanBodyMemberToken(tokens, nameIdx, word, outTok))
                    return true;
            }
            auto cit = nsc->variables.find(String(word));
            if (cit != nsc->variables.end() && cit->second.value.T.isCallable()) {
                const FunctionContainer& fc =
                    std::any_cast<const FunctionContainer&>(cit->second.value.raw());
                if (!fc.functions.empty()
                        && fc.functions.back().decl_token.type != TypeOfToken::DUMMY) {
                    outTok = fc.functions.back().decl_token;
                    return true;
                }
            }
        }
        return false;   // '.'/'::'-chain did not resolve the final word
    }

    // 1) generic params of enclosing functions (innermost first)
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        for (size_t gi = 0; gi < (*it)->generics.size(); ++gi) {
            const GenericParam& g = (*it)->generics[gi];
            if (g.name_token.type != TypeOfToken::DUMMY && g.name.to_utf8() == word) {
                outTok = g.name_token;
                return true;
            }
        }
    }

    // 2) generic params of the enclosing struct type
    EnclosingStruct en;
    if (findEnclosingStruct(tokens, lineStarts, wordStart, en) && structs) {
        auto sit = structs->find(String(en.name));
        if (sit != structs->end()) {
            for (size_t gi = 0; gi < sit->second.generics.size(); ++gi) {
                const GenericParam& g = sit->second.generics[gi];
                if (g.name_token.type != TypeOfToken::DUMMY && g.name.to_utf8() == word) {
                    outTok = g.name_token;
                    return true;
                }
            }
        }
    }

    // 3) let/for variable
    {
        std::map<std::string, LocalCandidate> letBuf;
        std::map<std::string, size_t> letDeclPositions;
        collectLetVars(tokens, lineStarts, wordStart, letBuf, &letDeclPositions);
        auto ld = letDeclPositions.find(word);
        if (ld != letDeclPositions.end()) {
            for (size_t ti = 0; ti < tokens.size(); ++ti)
                if (tokenByteOffset(tokens[ti], lineStarts) == ld->second) {
                    outTok = tokens[ti];
                    return true;
                }
        }
    }

    // 4) function parameter
    if (isParamNameAt(word, wordStart, paramScopes)) {
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            Token pt;
            if (scanParamNameToken(*it, tokens, word, pt)) { outTok = pt; return true; }
        }
        return false;   // param Р±РµР· С‚РѕРєРµРЅР° (Р»СЏРјР±РґР°) -> РїРµСЂРµС…РѕРґР° РЅРµС‚
    }

    // 4b) member of the enclosing struct: field/method decl name (self-jump)
    EnclosingStruct selfEn;
    if (structs && findEnclosingStruct(tokens, lineStarts, wordStart, selfEn)) {
        auto sfit = structs->find(String(selfEn.name));
        if (sfit != structs->end()
                && sfit->second.name_token.type != TypeOfToken::DUMMY) {
            size_t nameIdx = tokenIndexAt(tokens, sfit->second.name_token.position.line,
                                          sfit->second.name_token.position.index);
            if (nameIdx < tokens.size() && scanBodyMemberToken(tokens, nameIdx, word, outTok))
                return true;
        }
    }

    // 5) #define / #macro
    for (const auto& d : defines)
        if (d.name_token.type != TypeOfToken::DUMMY && d.name_token.value.to_utf8() == word) {
            outTok = d.name_token;
            return true;
        }
    for (const auto& m : macros)
        if (m.name_token.type != TypeOfToken::DUMMY && m.name_token.value.to_utf8() == word) {
            outTok = m.name_token;
            return true;
        }
    // fallback: СЂР°СЃСЃРєСЂС‹С‚С‹Рµ define РІ pp.defines СѓР¶Рµ СЃС‚С‘СЂС‚С‹, РЅРѕ РІ raw-С‚РѕРєРµРЅР°С… (g_tokens)
    // РґРёСЂРµРєС‚РёРІР° СЃРѕС…СЂР°РЅСЏРµС‚СЃСЏ С†РµР»РёРєРѕРј: #define / #macro + Р»РёС‚РµСЂР°Р»-РёРјСЏ
    for (size_t di = 0; di + 1 < tokens.size(); ++di)
        if (tokens[di].type == DIRECTIVE
                && (tokens[di].value.to_utf8() == "define"
                    || tokens[di].value.to_utf8() == "macro")
                && tokens[di + 1].type == LITERAL
                && tokens[di + 1].value.to_utf8() == word) {
            outTok = tokens[di + 1];
            return true;
        }

    // 6) namespace name
    if (findTopKeywordDeclTok(tokens, curPath, "namespace", word, outTok))
        return true;

    // 7) struct / enum type
    if (structs) {
        auto sit = structs->find(String(word));
        if (sit != structs->end() && sit->second.name_token.type != TypeOfToken::DUMMY) {
            outTok = sit->second.name_token;
            return true;
        }
    }
    if (enums) {
        auto eit = enums->find(String(word));
        if (eit != enums->end() && eit->second.name_token.type != TypeOfToken::DUMMY) {
            outTok = eit->second.name_token;
            return true;
        }
    }

    // 8) free function
    if (findTopKeywordDeclTok(tokens, curPath, "fn", word, outTok))
        return true;

    // 8b) С„СѓРЅРєС†РёСЏ РёР· РІРєР»СЋС‡С‘РЅРЅРѕРіРѕ С„Р°Р№Р»Р°: РІ raw-С‚РѕРєРµРЅР°С… РґРµРєР»Р°СЂР°С†РёРё РЅРµС‚
    // (РѕРЅР° Р¶РёРІС‘С‚ РІ merged pp-РїРѕС‚РѕРєРµ) вЂ” Р±РµСЂС‘Рј decl_token РёР· FunctionContainer.
    auto fit = ctx.variables.find(String(word));
    if (fit != ctx.variables.end() && fit->second.value.T.isCallable()) {
        const FunctionContainer& fc =
            std::any_cast<const FunctionContainer&>(fit->second.value.raw());
        if (!fc.functions.empty()
                && fc.functions.back().decl_token.type != TypeOfToken::DUMMY) {
            outTok = fc.functions.back().decl_token;
            return true;
        }
    }

    return false;
}

static JsonValue buildDefinition(const string& text, const string& uri,
                                 int lspLine, int lspChar,
                                 const Context& ctx,
                                 const std::vector<Token>& tokens,
                                 const std::vector<Node*>& tree,
                                 const std::vector<Define>& defines,
                                 const std::vector<Macro>& macros,
                                 const std::map<String, StructSymbol>* structs,
                                 const std::map<String, EnumSymbol>* enums,
                                 const std::map<unsigned long long, Type>* localTypes = nullptr) {
    JsonValue res = JObject();   // empty -> "no definition"
    size_t pos = lspPosToByteOffset(text, lspLine, lspChar);
    std::vector<size_t> lineStarts = buildLineStarts(text);
    string curPath = pathFromUri(uri);

    Token def;
    if (!definitionToken(text, pos, ctx, tokens, tree, lineStarts, defines, macros,
                         structs, enums, curPath, def, localTypes))
        return res;

    string defPath = def.position.file_path.to_utf8();
    // РџСѓС‚Рё РІРєР»СЋС‡С‘РЅРЅС‹С… С„Р°Р№Р»РѕРІ (#include) РІ С‚РѕРєРµРЅР°С… вЂ” РѕС‚РЅРѕСЃРёС‚РµР»СЊРЅС‹Рµ (РєР°Рє РІ РґРёСЂРµРєС‚РёРІРµ).
    // РќРѕСЂРјР°Р»РёР·СѓРµРј Рє Р°Р±СЃРѕР»СЋС‚РЅРѕРјСѓ (Р±Р°Р·Р° СЃРѕРІРїР°РґР°РµС‚ СЃ СЂРµР·РѕР»РІРѕРј openFile = CWD СЃРµСЂРІРµСЂР°),
    // РёРЅР°С‡Рµ РєР»РёРµРЅС‚ РїРѕР»СѓС‡РёС‚ РЅРµРІР°Р»РёРґРЅС‹Р№ URI РІРёРґР° file:///std/Pair.lm.
    if (!defPath.empty()) {
        bool hasDrive = defPath.size() >= 2 && defPath[1] == ':';
        bool rooted   = defPath[0] == '/' || defPath[0] == '\\';
        if (!hasDrive && !rooted) {
            char buf[MAX_PATH];
            DWORD n = GetCurrentDirectoryA(MAX_PATH, buf);
            if (n > 0 && n < MAX_PATH) {
                std::string base(buf);
                if (base[base.size() - 1] != '\\') base += "\\";
                defPath = base + defPath;
            }
        }
        for (auto& c : defPath) if (c == '\\') c = '/';
    }
    string defText = text;
    string defUri  = uri;
    if (!defPath.empty() && !sameFile(defPath, curPath)) {
        defUri = uriFromPath(defPath);
        try {
            String ft = openFile(defPath);
            defText = ft.to_utf8();
        } catch (...) {
            defText.clear();
        }
    }
    std::vector<size_t> dls = buildLineStarts(defText.empty() ? text : defText);

    JsonValue rng = JObject();
    rng.object["start"] = tokenLspPos(def, dls, (defText.empty() ? text : defText), 0);
    rng.object["end"]   = tokenLspPos(def, dls, (defText.empty() ? text : defText),
                                      (size_t)def.position.size);
    res.object["uri"] = JString(defUri);
    res.object["range"] = rng;
    return res;
}

// Р С›Р В±Р В»Р В°РЎРѓРЎвЂљРЎРЉ Р Р†Р С‘Р Т‘Р С‘Р СР С•РЎРѓРЎвЂљР С‘ Р С–Р ВµР Р…Р ВµРЎР‚Р С‘Р С”-Р С—Р В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚Р С•Р Р† РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘:
// Р С‘Р СР ВµР Р…Р В° Р В°Р С”РЎвЂљР С‘Р Р†Р Р…РЎвЂ№ Р С•РЎвЂљ Р С‘Р СР ВµР Р…Р С‘ РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘ Р Т‘Р С• Р С”Р С•Р Р…РЎвЂ Р В° Р ВµРЎвЂ РЎвЂљР ВµР В»Р В°.
struct GenericScope {
    size_t begin = 0;
    size_t end = 0;  // Р В·Р В°Р С”РЎР‚РЎвЂ№Р Р†Р В°РЎР‹РЎвЂ°Р В°РЎРЏ '}' Р Р†Р С”Р В»РЎР‹РЎвЂЎР С‘РЎвЂљР ВµР В»РЎРЉР Р…Р С•
    std::set<std::string> names;
};

// Р В Р ВµР С”РЎС“РЎР‚РЎРѓР С‘Р Р†Р Р…РЎвЂ№Р в„– Р С•Р В±РЎвЂ¦Р С•Р Т‘ AST: Р Т‘Р В»РЎРЏ Р С”Р В°Р В¶Р Т‘Р С•Р в„– РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘ РЎРѓ Р С–Р ВµР Р…Р ВµРЎР‚Р С‘Р С”-Р С—Р В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚Р В°Р СР С‘ (fn foo[T])
// Р Р†РЎвЂ№РЎвЂЎР С‘РЎРѓР В»РЎРЏР ВµР С Р ВµРЎвЂ Р В±Р В°Р в„–РЎвЂљР С•Р Р†РЎвЂ№Р в„– Р Т‘Р С‘Р В°Р С—Р В°Р В·Р С•Р Р… Р С—Р С• РЎвЂљР С•Р С”Р ВµР Р…Р В°Р С (Р С‘Р СРЎРЏ РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘ ... Р В·Р В°Р С”РЎР‚РЎвЂ№Р Р†Р В°РЎР‹РЎвЂ°Р В°РЎРЏ '}').
// РћР±СЉСЏРІР»РµРЅРёРµ СЃ РґР¶РµРЅРµСЂРёРє-РїР°СЂР°РјРµС‚СЂР°РјРё (fn/struct): РІС‹С‡РёСЃР»СЏРµРј Р±Р°Р№С‚РѕРІС‹Р№ РґРёР°РїР°Р·РѕРЅ
// РѕС‚ С‚РѕРєРµРЅР° РёРјРµРЅРё РґРѕ Р·Р°РєСЂС‹РІР°СЋС‰РµР№ '}' РІРєР»СЋС‡РёС‚РµР»СЊРЅРѕ вЂ” T/Size РїРѕРґСЃРІРµС‡РёРІР°СЋС‚СЃСЏ
// Рё РІ С€Р°РїРєРµ `[T, Size: i64 = -1]`, Рё РІРѕ РІСЃС‘Рј С‚РµР»Рµ.
static void addDeclGenericScope(const Token& name_token,
                                const std::vector<GenericParam>& generics,
                                const std::vector<Token>& tokens,
                                const std::vector<size_t>& lineStarts,
                                std::vector<GenericScope>& out,
                                bool beginAtLineStart) {
    if (generics.empty() || name_token.type == TypeOfToken::DUMMY) return;
    GenericScope sc;
    for (auto& g : generics)
        sc.names.insert(g.name.to_utf8());
    int nl = name_token.position.line;
    size_t nlOff = (nl >= 0 && (size_t)nl < lineStarts.size()) ? lineStarts[nl] : 0;
    // struct[T, ...] Vec вЂ” РїР°СЂР°РјРµС‚СЂС‹ РёРґСѓС‚ Р”Рћ РёРјРµРЅРё, РїРѕСЌС‚РѕРјСѓ РґР»СЏ struct СЃРєРѕСѓРї
    // РЅР°С‡РёРЅР°РµРј СЃ РЅР°С‡Р°Р»Р° СЃС‚СЂРѕРєРё РѕР±СЉСЏРІР»РµРЅРёСЏ (fn[T] вЂ” РёРјСЏ РёРґС‘С‚ РїРµСЂРІС‹Рј, РґРѕСЃС‚Р°С‚РѕС‡РЅРѕ С‚РѕРєРµРЅР°).
    sc.begin = beginAtLineStart ? nlOff : nlOff + name_token.position.index;
    size_t beginIdx = tokens.size();
    for (size_t ti = 0; ti < tokens.size(); ++ti)
        if (tokens[ti].position.line == nl
                && tokens[ti].position.index == name_token.position.index) {
            beginIdx = ti;
            break;
        }
    if (beginIdx < tokens.size()) {
        int depth = 0;
        bool opened = false;
        for (size_t ti = beginIdx + 1; ti < tokens.size(); ++ti) {
            const Token& t = tokens[ti];
            if (t.type == TypeOfToken::BRACE_LEFT_CURVE) { depth++; opened = true; }
            else if (t.type == TypeOfToken::BRACE_RIGHT_CURVE) {
                if (depth > 0) depth--;
                if (depth == 0 && opened) {
                    size_t bl = (size_t)t.position.line;
                    sc.end = (bl < lineStarts.size() ? lineStarts[bl] : 0)
                           + t.position.index + t.position.size;
                    break;
                }
            }
        }
    }
    if (sc.end > sc.begin && !sc.names.empty())
        out.push_back(std::move(sc));
}

static void buildGenericScopes(Node* n,
                               const std::vector<Token>& tokens,
                               const std::vector<size_t>& lineStarts,
                               std::vector<GenericScope>& out) {
    if (!n) return;
    switch (n->type) {
        case NodeTypes::NodeFunctionDeclaration: {
            auto* fn = static_cast<struct NodeFunctionDeclaration*>(n);
            addDeclGenericScope(fn->name_token, fn->generics, tokens, lineStarts, out, false);
            buildGenericScopes(fn->body, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeStatementsScope: {
            auto* s = static_cast<struct NodeStatementsScope*>(n);
            for (auto* child : s->nodes) buildGenericScopes(child, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeScope: {
            auto* s = static_cast<struct NodeScope*>(n);
            buildGenericScopes(s->node, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeIf: {
            auto* i = static_cast<struct NodeIf*>(n);
            buildGenericScopes(i->then_body, tokens, lineStarts, out);
            buildGenericScopes(i->else_body, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeWhile: {
            auto* w = static_cast<struct NodeWhile*>(n);
            buildGenericScopes(w->body, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeFor: {
            auto* f = static_cast<struct NodeFor*>(n);
            buildGenericScopes(f->body, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeForEach: {
            auto* f = static_cast<struct NodeForEach*>(n);
            buildGenericScopes(f->body, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeLambda: {
            auto* l = static_cast<struct NodeLambda*>(n);
            buildGenericScopes(l->body, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeReturn:
            buildGenericScopes(static_cast<struct NodeReturn*>(n)->expr, tokens, lineStarts, out);
            break;
        case NodeTypes::NodeNamespace:
            buildGenericScopes(static_cast<struct NodeNamespace*>(n)->body, tokens, lineStarts, out);
            break;
        case NodeTypes::NodeStructDeclaration: {
            auto* sn = static_cast<struct NodeStructDeclaration*>(n);
            addDeclGenericScope(sn->name_token, sn->generics, tokens, lineStarts, out, true);
            buildGenericScopes(sn->body, tokens, lineStarts, out);
            break;
        }
        case NodeTypes::NodeModifier:
            buildGenericScopes(static_cast<struct NodeModifier*>(n)->node, tokens, lineStarts, out);
            break;
        default: break;
    }
}

static bool isGenericParamRef(const std::string& name, size_t off,
                              const std::vector<GenericScope>& scopes) {
    for (const auto& s : scopes)
        if (s.names.count(name) && off >= s.begin && off < s.end)
            return true;
    return false;
}

// Р вЂР В°Р в„–РЎвЂљР С•Р Р†РЎвЂ№Р Вµ РЎРѓР СР ВµРЎвЂ°Р ВµР Р…Р С‘РЎРЏ Р Р…Р В°РЎвЂЎР В°Р В»Р В° РЎРѓРЎвЂљРЎР‚Р С•Р С” (Р Т‘Р В»РЎРЏ Р С—Р ВµРЎР‚Р ВµР Р†Р С•Р Т‘Р В° line:index РЎвЂљР С•Р С”Р ВµР Р…Р С•Р Р† Р Р† Р В°Р В±РЎРѓ. Р С•РЎвЂћРЎРѓР ВµРЎвЂљ)
static std::vector<size_t> buildLineStarts(const std::string& text) {
    std::vector<size_t> starts;
    starts.push_back(0);
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n') starts.push_back(i + 1);
    return starts;
}

static JsonValue buildSemanticTokens(const std::vector<Token>& tokens,
                                     const Context& ctx,
                                     const std::vector<Node*>& tree,
                                     const std::vector<size_t>& lineStarts,
                                     const std::vector<Macro>& macros) {
    JsonValue data = JArray();

    // 0. Р С›Р В±Р В»Р В°РЎРѓРЎвЂљР С‘ Р Р†Р С‘Р Т‘Р С‘Р СР С•РЎРѓРЎвЂљР С‘ Р С–Р ВµР Р…Р ВµРЎР‚Р С‘Р С”-Р С—Р В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚Р С•Р Р†
    std::vector<GenericScope> genericScopes;
    for (auto* node : tree)
        buildGenericScopes(node, tokens, lineStarts, genericScopes);

    // 1. Р РЋРЎвЂљРЎР‚Р С•Р С‘Р С Р С”Р В°РЎР‚РЎвЂљРЎС“ Р С‘Р СРЎвЂР Р… РІвЂ вЂ™ semantic type Р С‘Р В· context
    std::map<std::string, int> nameType;
    for (auto& kv : ctx.variables) {
        const string& name = kv.first.to_utf8();
        const ContextObject& co = kv.second;
        if (co.value.T.isCallable())          nameType[name] = 6;
        else if (co.value.T == Ty::Namespace)  nameType[name] = 9;
        else if (co.value.T == Ty::Type_)      nameType[name] = 5;
        else                                   nameType[name] = 7;
    }

    // 1b. Имена #macro-функций: отдельный семантический тип (macroFunction),
    // чтобы имя макроса не красилось как обычная функция/переменная. Держим
    // и offset определения, и множество имён (для подсветки вызовов).
    std::set<std::string> macroNames;
    std::set<size_t> macroDefOffsets;
    for (const auto& m : macros) {
        const std::string mname = m.name_token.value.to_utf8();
        if (mname.empty()) continue;
        macroNames.insert(mname);
        size_t moff = ((size_t)m.name_token.position.line < lineStarts.size())
            ? lineStarts[m.name_token.position.line] + m.name_token.position.index
            : (size_t)m.name_token.position.index;
        macroDefOffsets.insert(moff);
    }

    // 2. Р РЋР С•Р В±Р С‘РЎР‚Р В°Р ВµР С Р С•Р В±Р В»Р В°РЎРѓРЎвЂљР С‘ Р Р†Р С‘Р Т‘Р С‘Р СР С•РЎРѓРЎвЂљР С‘ Р С—Р В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚Р С•Р Р† Р С‘Р В· AST
    std::vector<ParamScope> paramScopes;
    for (auto* node : tree)
        collectParamScopes(node, tokens, lineStarts, paramScopes);

    // 2b. Р ВР СР ВµР Р…Р В° РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р в„–/namespace/РЎвЂљР С‘Р С—Р С•Р Р† Р С—Р С• Р Р†РЎРѓР ВµР СРЎС“ Р Т‘Р ВµРЎР‚Р ВµР Р†РЎС“
    std::set<std::string> fnNames, nsNames, typeNames;
    for (auto* node : tree) collectSymbolNames(node, fnNames, nsNames, typeNames);

    // 3. Р вЂњР ВµР Р…Р ВµРЎР‚Р С‘РЎР‚РЎС“Р ВµР С delta-encoded tokens
    int prevLine = 0, prevStart = 0;
    for (const auto& tok : tokens) {
        if (tok.type == END_OF_FILE || tok.type == DUMMY) continue;

        int semType = mapTokenSemanticType(tok.type);
        if (semType == -1) continue;

        if (tok.type == KEYWORD) {
            // Р СљР С•Р Т‘Р С‘РЎвЂћР С‘Р С”Р В°РЎвЂљР С•РЎР‚РЎвЂ№ Р С”РЎР‚Р В°РЎРѓРЎРЏРЎвЂљРЎРѓРЎРЏ Р С•РЎвЂљР Т‘Р ВµР В»РЎРЉР Р…РЎвЂ№Р С РЎвЂљР С‘Р С—Р С•Р С, Р В° Р Р…Р Вµ Р С”Р В°Р С” Р С”Р В»РЎР‹РЎвЂЎР ВµР Р†РЎвЂ№Р Вµ РЎРѓР В»Р С•Р Р†Р В°.
            // Р СћР С•Р С”Р ВµР Р…Р С‘Р В·Р В°РЎвЂљР С•РЎР‚ Р Т‘Р В°РЎвЂРЎвЂљ KEYWORD Р С‘ Р Т‘Р В»РЎРЏ Р СР С•Р Т‘Р С‘РЎвЂћР С‘Р С”Р В°РЎвЂљР С•РЎР‚Р С•Р Р†, Р С—Р С•РЎРЊРЎвЂљР С•Р СРЎС“ РЎР‚Р В°Р В·Р В»Р С‘РЎвЂЎР В°Р ВµР С
            // Р С‘РЎвЂ¦ Р С—Р С• Р В·Р Р…Р В°РЎвЂЎР ВµР Р…Р С‘РЎР‹ Р С—Р С• РЎРѓР С—Р С‘РЎРѓР С”РЎС“ Literals::MODIFIERS.
            const std::string v = tok.value.to_utf8();
            for (const auto& m : Literals::MODIFIERS)
                if (m.to_utf8() == v) { semType = 11; break; }
        }

        if (tok.type == LITERAL) {
            string name = tok.value.to_utf8();
            size_t off = ((size_t)tok.position.line < lineStarts.size())
                ? lineStarts[tok.position.line] + tok.position.index
                : (size_t)tok.position.index;
            if (name == "self")
                semType = 13;  // selfParameter (self-РїР°СЂР°РјРµС‚СЂ РјРµС‚РѕРґР° СЃС‚СЂСѓРєС‚СѓСЂС‹)
            else if (isGenericParamRef(name, off, genericScopes))
                semType = 12;  // typeParameter
            else if (isParamNameAt(name, off, paramScopes))
                semType = 8;  // parameter
            else if (macroDefOffsets.count(off) || macroNames.count(name))
                semType = 14;  // macroFunction (#macro)
            else if (fnNames.count(name))
                semType = 6;  // function
            else if (nsNames.count(name))
                semType = 9;  // namespace
            else if (typeNames.count(name))
                semType = 5;  // type
            else {
                auto it = nameType.find(name);
                if (it != nameType.end()) semType = it->second;
            }
        }

        int deltaLine  = tok.position.line - prevLine;
        int deltaStart = (deltaLine == 0)
            ? tok.position.index - prevStart
            : tok.position.index;

        data.array.push_back(JNumber(deltaLine));
        data.array.push_back(JNumber(deltaStart));
        data.array.push_back(JNumber(tok.position.size));
        data.array.push_back(JNumber(semType));
        data.array.push_back(JNumber(0));

        prevLine  = tok.position.line;
        prevStart = tok.position.index;
    }
    return data;
}

// ---------- Document symbols (textDocument/documentSymbol) ----------
// LSP SymbolKind: Namespace=3, Enum=10, Function=12, Variable=13,
//                 EnumMember=22, Struct=23
static const int SYM_NS           = 3;
static const int SYM_ENUM         = 10;
static const int SYM_FUNCTION     = 12;
static const int SYM_VARIABLE     = 13;
static const int SYM_ENUM_MEMBER  = 22;
static const int SYM_STRUCT       = 23;

// РўРµРєСЃС‚ С‚РѕРєРµРЅР° РїРѕ Р±Р°Р№С‚Р°Рј РёСЃС…РѕРґРЅРёРєР° (РёРјРµРЅР° РІР°СЂРёР°РЅС‚РѕРІ enum Рё С‚.Рї.)
static string tokenTextAt(const Token& t, const std::vector<size_t>& lineStarts,
                          const string& text) {
    if (t.position.line < 0 || (size_t)t.position.line >= lineStarts.size())
        return "";
    size_t base = lineStarts[(size_t)t.position.line] + t.position.index;
    if (base >= text.size()) return "";
    return text.substr(base, (size_t)t.position.size);
}

// Р‘Р°Р№С‚РѕРІС‹Р№ РґРёР°РїР°Р·РѕРЅ РѕР±СЉСЏРІР»РµРЅРёСЏ В«РѕС‚ РёРјРµРЅРё РґРѕ Р·Р°РєСЂС‹РІР°СЋС‰РµР№ }В» (Р»РѕРіРёРєР° fnRange).
// Р’РѕР·РІСЂР°С‰Р°РµС‚ false, РµСЃР»Рё С‚РµР»Р° РЅРµС‚ (extern-С„СѓРЅРєС†РёСЏ) вЂ” РІС‹Р·С‹РІР°СЋС‰РёР№ РёСЃРїРѕР»СЊР·СѓРµС‚
// selectionRange РєР°Рє range.
static bool declBraceRange(const Token& nameTok,
                           const std::vector<Token>& tokens,
                           const std::vector<size_t>& lineStarts,
                           size_t& begin, size_t& end) {
    if (nameTok.type == TypeOfToken::DUMMY) return false;
    int nl = nameTok.position.line;
    if (nl < 0 || (size_t)nl >= lineStarts.size()) return false;
    begin = lineStarts[(size_t)nl] + nameTok.position.index;
    size_t beginIdx = tokens.size();
    for (size_t ti = 0; ti < tokens.size(); ++ti)
        if (tokens[ti].position.line == nl
                && tokens[ti].position.index == nameTok.position.index) {
            beginIdx = ti; break;
        }
    if (beginIdx >= tokens.size()) return false;
    int depth = 0; bool opened = false;
    for (size_t ti = beginIdx + 1; ti < tokens.size(); ++ti) {
        const Token& t = tokens[ti];
        if (t.type == TypeOfToken::BRACE_LEFT_CURVE) { depth++; opened = true; }
        else if (t.type == TypeOfToken::BRACE_RIGHT_CURVE) {
            if (depth > 0) depth--;
            if (depth == 0 && opened) {
                size_t bl = (size_t)t.position.line;
                end = (bl < lineStarts.size() ? lineStarts[bl] : 0)
                    + t.position.index + t.position.size;
                return true;
            }
        }
    }
    return false;
}

// РљРѕРЅС‚РµРєСЃС‚ documentSymbol-Р·Р°РїСЂРѕСЃР°: С‚РµРєСѓС‰РёР№ С„Р°Р№Р» + Р»РµРЅРёРІС‹Р№ РєСЌС€ С‚РµРєСЃС‚РѕРІ
// РІРєР»СЋС‡С‘РЅРЅС‹С… С„Р°Р№Р»РѕРІ (РґР»СЏ РїРѕР·РёС†РёР№ СЃРёРјРІРѕР»РѕРІ РёР· #include РІ РёС… СЃРѕР±СЃС‚РІРµРЅРЅС‹С…
// РєРѕРѕСЂРґРёРЅР°С‚Р°С… вЂ” РєР°Рє buildDefinition С‡РёС‚Р°РµС‚ С„Р°Р№Р»-С†РµР»СЊ РїРµСЂРµС…РѕРґР°).
struct SymEnv {
    string curPath;                       // РїСѓС‚СЊ Р·Р°РїСЂРѕС€РµРЅРЅРѕРіРѕ С„Р°Р№Р»Р°
    std::map<string, string> fileTexts;   // abs-РїСѓС‚СЊ -> СЃРѕРґРµСЂР¶РёРјРѕРµ (РїСѓСЃС‚Рѕ = РЅРµ С‡РёС‚Р°РµС‚СЃСЏ)
    // РўРµРєСЃС‚ С„Р°Р№Р»Р° СЃРёРјРІРѕР»Р° РїРѕ Р°Р±СЃ. РїСѓС‚Рё (nullptr, РµСЃР»Рё СЌС‚Рѕ С‚РµРєСѓС‰РёР№ С„Р°Р№Р»).
    const string* fileText(const string& absPath) {
        if (absPath.empty()) return nullptr;
        auto it = fileTexts.find(absPath);
        if (it != fileTexts.end()) return &it->second;
        string txt;
        try { txt = openFile(absPath).to_utf8(); } catch (...) { txt.clear(); }
        return &(fileTexts[absPath] = txt);
    }
};

// РћРґРёРЅ DocumentSymbol: name, kind, detail (С‚Р° Р¶Рµ РїРѕРґРїРёСЃСЊ, С‡С‚Рѕ РІ hover-РїРѕРїР°РїРµ),
// selectionRange = РёРјСЏ, range = РёРјСЏ..С‚РµР»Рѕ (withBodyRange=true), children.
// withBodyRange=false вЂ” С‚РѕР»СЊРєРѕ РёРјСЏ (РїРµСЂРµРјРµРЅРЅС‹Рµ, РІР°СЂРёР°РЅС‚С‹ enum: СЃРєР°РЅРёСЂРѕРІР°РЅРёРµ {} РІРїРµСЂС‘Рґ
// РѕРїСѓС‰РµРЅРѕ, С‡С‚РѕР±С‹ РЅРµ В«РїСЂРѕРіР»РѕС‚РёС‚СЊВ» РѕС‚РєСЂС‹РІР°СЋС‰СѓСЋ СЃРєРѕР±РєСѓ СЃР»РµРґСѓСЋС‰РµРіРѕ РѕР±СЉСЏРІР»РµРЅРёСЏ).
// РЎРёРјРІРѕР» РёР· РґСЂСѓРіРѕРіРѕ С„Р°Р№Р»Р° (#include): РґРѕР±Р°РІР»СЏРµС‚СЃСЏ "file" (uri РµРіРѕ С„Р°Р№Р»Р°), РїРѕР·РёС†РёРё
// СЃС‡РёС‚Р°СЋС‚СЃСЏ РІ РєРѕРѕСЂРґРёРЅР°С‚Р°С… С‚РѕРіРѕ С„Р°Р№Р»Р°, range = РёРјСЏ (С‚РµР»Рѕ РѕС‚СЂР°Р±Р°С‚С‹РІР°С‚СЊ РЅРµ РїС‹С‚Р°РµРјСЃСЏ).
static JsonValue makeDocSymbol(const string& name, int kind,
                               const Token& nameTok,
                               const std::vector<Token>& tokens,
                               const std::vector<size_t>& lineStarts,
                               const string& text,
                               SymEnv& env,
                               JsonValue& children,
                               bool withBodyRange,
                               const std::string& detail = "") {
    JsonValue s = JObject();
    s.object["name"] = JString(name);
    s.object["kind"] = JNumber(kind);
    if (!detail.empty()) s.object["detail"] = JString(detail);
    string fp = nameTok.position.file_path.to_utf8();
    bool cross = !fp.empty() && !pathsEqual(fp, env.curPath);
    std::vector<size_t> crossLS;
    const std::vector<size_t>* lsPtr = &lineStarts;
    const string* txtPtr = &text;
    if (cross) {
        string abs = absIncludePath(fp);
        s.object["file"] = JString(uriFromPath(abs));
        const string* ft = env.fileText(abs);
        if (ft && !ft->empty()) {
            crossLS = buildLineStarts(*ft);
            lsPtr = &crossLS;
            txtPtr = ft;
        }
    }
    JsonValue sr = JObject();
    sr.object["start"] = tokenLspPos(nameTok, *lsPtr, *txtPtr, 0);
    sr.object["end"]   = tokenLspPos(nameTok, *lsPtr, *txtPtr,
                                     (size_t)nameTok.position.size);
    s.object["selectionRange"] = sr;
    size_t begin = 0, end = 0;
    if (cross) {
        // Р§СѓР¶РѕР№ С„Р°Р№Р»: С‚РµР»Рѕ РЅРµ СЃРєР°РЅРёСЂСѓРµРј (РґРёР°РїР°Р·РѕРЅ В«РёРјСЏ..}В» РІ РЅС‘Рј РёР· СЃРјРµС€Р°РЅРЅРѕРіРѕ
        // РїРѕС‚РѕРєР° С‚РѕРєРµРЅРѕРІ СЃРѕР±РёСЂР°С‚СЊ РЅРµРЅР°РґС‘Р¶РЅРѕ) вЂ” РєР»РёРµРЅС‚Сѓ С…РІР°С‚Р°РµС‚ position РёРјРµРЅРё.
        s.object["range"] = sr;
    } else if (withBodyRange
               && declBraceRange(nameTok, tokens, lineStarts, begin, end)) {
        JsonValue r = JObject();
        if (end > text.size()) end = text.size();
        r.object["start"] = positionFromByte(text, lineStarts, begin);
        r.object["end"]   = positionFromByte(text, lineStarts, end);
        s.object["range"] = r;
    } else {
        s.object["range"] = sr;
    }
    s.object["children"] = children;
    return s;
}

static void pushSymbols(Node* n,
                        const std::vector<Token>& tokens,
                        const std::vector<size_t>& lineStarts,
                        const string& text,
                        SymEnv& env,
                        const Context& ctx,
                        const std::map<String, StructSymbol>* structs,
                        const std::map<String, EnumSymbol>* enums,
                        const string& container,
                        JsonValue& out);

// ---------- РџРѕРґРїРёСЃРё СЃРёРјРІРѕР»РѕРІ РєР°Рє РІ hover-РїРѕРїР°РїРµ ----------
// РўРѕС‚ Р¶Рµ С‚РµРєСЃС‚, С‡С‚Рѕ textDocument/hover РѕС‚РґР°С‘С‚ РґР»СЏ РёРјРµРЅРё СЃРёРјРІРѕР»Р° (СЃРј.
// contextHover): В«let x: i64В», В«fn sum(...) -> ...В», В«struct PairВ»,
// В«enum Types { ... }В», В«namespace FooВ». РњР°СЂРєРµСЂС‹ markdown (**) СѓР±СЂР°РЅС‹,
// РґРѕРєРё РЅРµ РґРѕР±Р°РІР»СЏСЋС‚СЃСЏ вЂ” РєРѕРјРїР°РєС‚РЅР°СЏ РїРѕРґРїРёСЃСЊ РґР»СЏ СЃРїРёСЃРєР° СЃРёРјРІРѕР»РѕРІ.

static std::string hoverDeclText(const String& name,
                                 const Context& searchCtx,
                                 const std::map<String, StructSymbol>* structs,
                                 const std::map<String, EnumSymbol>* enums) {
    auto it = searchCtx.variables.find(name);
    if (it == searchCtx.variables.end()) return "";
    const ContextObject& co = it->second;
    // Р¤СѓРЅРєС†РёСЏ (РІРѕР·РјРѕР¶РЅРѕ, РїРµСЂРµРіСЂСѓР·РєРё): РєРѕРЅС‚РµР№РЅРµСЂ РІ value СЃ РґРµРєР»Р°СЂР°С†РёСЏРјРё
    bool isFunc = co.value.T.isCallable();
    if (!isFunc && co.value.T.isUnion()) {
        const std::vector<Type>& arms = co.value.T.container_1;
        isFunc = !arms.empty();
        for (const Type& a : arms)
            if (!a.isCallable()) { isFunc = false; break; }
    }
    const FunctionContainer* fc =
        isFunc ? std::any_cast<const FunctionContainer>(&co.value.raw()) : nullptr;
    if (fc) {
        std::string md;
        for (size_t i = 0; i < fc->functions.size(); ++i) {
            if (i) md += "\n";
            md += funcSignature(fc->functions[i]);
        }
        return md;
    }
    if (co.value.T == Ty::Namespace)
        return "namespace " + name.to_utf8();
    if (co.is_type_decl) {
        const Type tt = co.value.get<Type>();
        if (structs && tt.isStruct()) {
            auto sit = structs->find(name);
            if (sit != structs->end()) return structHeaderText(sit->second);
            return "type " + name.to_utf8();
        }
        if (enums && tt.isEnum()) {
            auto eit = enums->find(name);
            if (eit != enums->end()) return enumPlainHeader(eit->second);
            return "enum " + name.to_utf8();
        }
        return "type " + name.to_utf8();
    }
    // РџРµСЂРµРјРµРЅРЅР°СЏ: РјРѕРґРёС„РёРєР°С‚РѕСЂС‹ + В«let РёРјСЏ: С‚РёРїВ» (РІСЃРµРіРґР° РѕРґРёРЅ С„РѕСЂРјР°С‚, РєР°Рє hover)
    std::string mods;
    const ContextObject::Modifiers m = co.modifiers;
    if (!m.order.empty()) {
        for (size_t i = 0; i < m.order.size(); ++i)
            mods += m.order[i].to_utf8() + " ";
    } else {
        if (m.Const)   mods += "const ";
        if (m.Strict)  mods += "strict ";
        if (m.Final)   mods += "final ";
        if (m.Static)  mods += "static ";
        if (m.Private) mods += "private ";
    }
    return mods + "let " + name.to_utf8() + ": " +
           co.value.T.toString().to_utf8();
}

// РС‰РµРј РёРјСЏ РІРѕ РІСЃРµС… namespace-РєРѕРЅС‚РµРєСЃС‚Р°С… РјРѕРґСѓР»СЏ (РєР°Рє hover, С€Р°Рі 3)
static std::string hoverDeclInNamespaces(const String& name,
                                         const Context& ctx,
                                         const std::map<String, StructSymbol>* structs,
                                         const std::map<String, EnumSymbol>* enums) {
    for (const auto& kv : ctx.variables) {
        if (kv.second.value.T != Ty::Namespace) continue;
        const Context& nsCtx =
            std::any_cast<const Namespace&>(kv.second.value.raw()).context;
        std::string s = hoverDeclText(name, nsCtx, structs, enums);
        if (!s.empty()) return s;
    }
    return "";
}

// РњРµС‚РѕРґ СЃС‚СЂСѓРєС‚СѓСЂС‹: РєР°Рє РІ hover (В«fn push[T](self, x: T) -> TВ»)
static std::string methodDeclText(const StructSymbol& sym, const String& name) {
    auto it = sym.methods.find(name);
    if (it == sym.methods.end()) return "";
    std::string md;
    for (size_t i = 0; i < it->second.functions.size(); ++i) {
        if (i) md += "\n";
        md += methodSignature(it->second.functions[i]);
    }
    return md;
}

// РџРѕР»Рµ СЃС‚СЂСѓРєС‚СѓСЂС‹: РєР°Рє РІ hover (В«let x: i64В», В«const let x: ...В»)
static std::string fieldDeclText(const StructSymbol& sym, const String& name) {
    const StructFieldDef* fd = sym.field_def(name);
    if (!fd) return "";
    return structFieldText(*fd);
}

// Р’Р°СЂРёР°РЅС‚ enum: РєР°Рє РІ hover (В«Types::a = 0В»)
static std::string enumMemberDeclText(const EnumSymbol& sym, const String& name) {
    auto vit = sym.enum_context.variables.find(name);
    if (vit == sym.enum_context.variables.end()) return "";
    std::string s = sym.name.to_utf8() + "::" + name.to_utf8();
    if (vit->second.value.raw().type() == typeid(long long))
        s += " = " + std::to_string(std::any_cast<long long>(vit->second.value.raw()));
    return s;
}

// РџРѕРґРїРёСЃСЊ СЃРёРјРІРѕР»Р° СЃРїРёСЃРєР°: container вЂ” РёРјСЏ СЂРѕРґРёС‚РµР»СЏ (Сѓ РґРµС‚РµР№ struct/enum).
// Подпись конкретной декларации функции/метода прямо из исходника («fn sum[T, C](a: T, b: C)»).
// В отличие от symbolDeclText не сливает ВСЕ перегрузки одной строкой: при дубликатах имён в
// списке символов каждая декларация должна показывать только свою сигнатуру.
static std::string funcDeclHeaderFromText(const Token& nameTok,
                                          const std::vector<size_t>& lineStarts,
                                          const string& text,
                                          const string& curPath) {
    std::string fp = nameTok.position.file_path.to_utf8();
    if (!fp.empty() && !pathsEqual(fp, curPath)) return "";
    int line = (int)nameTok.position.line;
    if (line < 0 || (size_t)line >= lineStarts.size()) return "";
    size_t nameOfs = lineStarts[(size_t)line] + (size_t)nameTok.position.index;
    size_t nameEnd = nameOfs + (size_t)nameTok.position.size;
    if (nameOfs >= text.size() || nameEnd > text.size()) return "";
    size_t p = nameOfs;
    while (p > 0 && isspace((unsigned char)text[p - 1])) --p;
    size_t kwEnd = p;
    size_t kwStart = kwEnd;
    while (kwStart > 0 && (isalnum((unsigned char)text[kwStart - 1]) || text[kwStart - 1] == '_'))
        --kwStart;
    if (kwEnd - kwStart != 2 || text.compare(kwStart, 2, "fn") != 0) return "";
    std::string h;
    size_t depth = 0;
    for (size_t i = nameOfs; i < text.size(); ++i) {
        char c = text[i];
        if (c == '{' || c == ';') break;
        if (c == '(' || c == '[') ++depth;
        else if (c == ')' || c == ']') { if (depth > 0) --depth; }
        if (isspace((unsigned char)c)) {
            if (!h.empty() && h.back() != ' ') h += ' ';
        } else {
            h += c;
        }
    }
    while (!h.empty() && h.back() == ' ') h.pop_back();
    if (h.empty()) return "";
    return "fn " + h;
}

static std::string symbolDeclText(int kind, const String& name,
                                  const string& container,
                                  const Context& ctx,
                                  const std::map<String, StructSymbol>* structs,
                                  const std::map<String, EnumSymbol>* enums) {
    if (!container.empty()) {
        // Р”РµС‚Рё struct: РјРµС‚РѕРґС‹ Рё РїРѕР»СЏ; РґРµС‚Рё enum: РІР°СЂРёР°РЅС‚С‹ вЂ” Р±РµСЂС‘Рј РёР· РєРѕРЅС‚РµР№РЅРµСЂР°
        const String c = String(container);
        if (structs) {
            auto sit = structs->find(c);
            if (sit != structs->end()) {
                if (kind == SYM_FUNCTION) {
                    std::string s = methodDeclText(sit->second, name);
                    if (!s.empty()) return s;
                } else if (kind == SYM_VARIABLE) {
                    std::string s = fieldDeclText(sit->second, name);
                    if (!s.empty()) return s;
                }
            }
        }
        if (kind == SYM_ENUM_MEMBER && enums) {
            auto eit = enums->find(c);
            if (eit != enums->end()) {
                std::string s = enumMemberDeclText(eit->second, name);
                if (!s.empty()) return s;
            }
        }
    }
    // Р’РµСЂС…РЅРёР№ СѓСЂРѕРІРµРЅСЊ (Рё namespace): РёС‰РµРј РІ РіР»РѕР±Р°Р»СЊРЅРѕРј РєРѕРЅС‚РµРєСЃС‚Рµ, Р·Р°С‚РµРј
    // РїРѕ РІСЃРµРј namespace-РєРѕРЅС‚РµРєСЃС‚Р°Рј вЂ” РєР°Рє hover РЅР° РѕР±СЉСЏРІР»РµРЅРёРё
    std::string s = hoverDeclText(name, ctx, structs, enums);
    if (!s.empty()) return s;
    return hoverDeclInNamespaces(name, ctx, structs, enums);
}

// РџСЂРѕС…РѕРґРёС‚ СЃРѕРґРµСЂР¶РёРјРѕРµ С‚РµР»Р° (NodeStatementsScope РёР»Рё РѕРґРёРЅРѕС‡РЅС‹Р№ СѓР·РµР»)
static void pushBody(Node* body,
                     const std::vector<Token>& tokens,
                     const std::vector<size_t>& lineStarts,
                     const string& text,
                     SymEnv& env,
                     const Context& ctx,
                     const std::map<String, StructSymbol>* structs,
                     const std::map<String, EnumSymbol>* enums,
                     const string& container,
                     JsonValue& out) {
    if (!body) return;
    if (body->type == NodeTypes::NodeStatementsScope) {
        auto* s = static_cast<struct NodeStatementsScope*>(body);
        for (auto* c : s->nodes)
            pushSymbols(c, tokens, lineStarts, text, env, ctx, structs, enums,
                        container, out);
    } else {
        pushSymbols(body, tokens, lineStarts, text, env, ctx, structs, enums,
                    container, out);
    }
}

static void pushSymbols(Node* n,
                        const std::vector<Token>& tokens,
                        const std::vector<size_t>& lineStarts,
                        const string& text,
                        SymEnv& env,
                        const Context& ctx,
                        const std::map<String, StructSymbol>* structs,
                        const std::map<String, EnumSymbol>* enums,
                        const string& container,
                        JsonValue& out) {
    if (!n) return;
    // РњРѕРґРёС„РёРєР°С‚РѕСЂС‹ (pub/extern/...) вЂ” РѕР±С‘СЂС‚РєРё, СЂР°Р·РІРѕСЂР°С‡РёРІР°РµРј
    while (n->type == NodeTypes::NodeModifier) {
        n = static_cast<struct NodeModifier*>(n)->node;
        if (!n) return;
    }
    switch (n->type) {
        case NodeTypes::NodeNamespace: {
            auto* ns = static_cast<struct NodeNamespace*>(n);
            if (ns->name_token.type == TypeOfToken::DUMMY || ns->name.empty())
                return;
            JsonValue ch = JArray();
            pushBody(ns->body, tokens, lineStarts, text, env, ctx, structs, enums,
                     container, ch);
            string d = symbolDeclText(SYM_NS, ns->name, "", ctx, structs, enums);
            out.array.push_back(makeDocSymbol(ns->name.to_utf8(), SYM_NS,
                                              ns->name_token, tokens,
                                              lineStarts, text, env, ch, true, d));
            return;
        }
        case NodeTypes::NodeFunctionDeclaration: {
            auto* fn = static_cast<struct NodeFunctionDeclaration*>(n);
            if (fn->name_token.type == TypeOfToken::DUMMY || fn->name.empty())
                return;
            JsonValue ch = JArray();
            // РњРµС‚РѕРґ СЃС‚СЂСѓРєС‚СѓСЂС‹ Р»РµР¶РёС‚ РІ container = РёРјРµРЅРё struct (hover РїРѕ С‡Р»РµРЅСѓ
            // РїРѕРєР°Р·С‹РІР°РµС‚ methodSignature); extern-С„СѓРЅРєС†РёСЏ Р±РµР· С‚РµР»Р°: range = РёРјСЏ
            string d = funcDeclHeaderFromText(fn->name_token, lineStarts, text,
                                              env.curPath);
            if (d.empty())
                d = symbolDeclText(SYM_FUNCTION, fn->name, container,
                                   ctx, structs, enums);
            out.array.push_back(makeDocSymbol(fn->name.to_utf8(), SYM_FUNCTION,
                                              fn->name_token, tokens,
                                              lineStarts, text, env, ch,
                                              fn->body != nullptr, d));
            return;
        }
        case NodeTypes::NodeStructDeclaration: {
            auto* st = static_cast<struct NodeStructDeclaration*>(n);
            if (st->name_token.type == TypeOfToken::DUMMY || st->name.empty())
                return;
            JsonValue ch = JArray();
            pushBody(st->body, tokens, lineStarts, text, env, ctx, structs, enums,
                     st->name.to_utf8(), ch);
            string d = symbolDeclText(SYM_STRUCT, st->name, "", ctx, structs, enums);
            out.array.push_back(makeDocSymbol(st->name.to_utf8(), SYM_STRUCT,
                                              st->name_token, tokens,
                                              lineStarts, text, env, ch, true, d));
            return;
        }
        case NodeTypes::NodeEnumDeclaration: {
            auto* en = static_cast<struct NodeEnumDeclaration*>(n);
            if (en->name_token.type == TypeOfToken::DUMMY || en->name.empty())
                return;
            JsonValue ch = JArray();
            string enName = en->name.to_utf8();
            for (const Token& v : en->variants) {
                if (v.type == TypeOfToken::DUMMY) continue;
                string vname = tokenTextAt(v, lineStarts, text);
                if (vname.empty()) continue;
                JsonValue empty = JArray();
                string memDetail = symbolDeclText(SYM_ENUM_MEMBER, vname,
                                                  enName, ctx, structs, enums);
                JsonValue mem = makeDocSymbol(vname, SYM_ENUM_MEMBER, v, tokens,
                                              lineStarts, text, env, empty, false,
                                              memDetail);
                ch.array.push_back(mem);
            }
            string d = symbolDeclText(SYM_ENUM, en->name, "", ctx, structs, enums);
            out.array.push_back(makeDocSymbol(en->name.to_utf8(), SYM_ENUM,
                                              en->name_token, tokens,
                                              lineStarts, text, env, ch, false, d));
            return;
        }
        case NodeTypes::NodeVariableDeclaration: {
            auto* vd = static_cast<struct NodeVariableDeclaration*>(n);
            if (vd->name_token.type == TypeOfToken::DUMMY || vd->name.empty())
                return;
            JsonValue ch = JArray();
            string d = symbolDeclText(SYM_VARIABLE, vd->name, container,
                                      ctx, structs, enums);
            out.array.push_back(makeDocSymbol(vd->name.to_utf8(), SYM_VARIABLE,
                                              vd->name_token, tokens,
                                              lineStarts, text, env, ch, false, d));
            return;
        }
        default:
            return;
    }
}

// textDocument/documentSymbol: DocumentSymbol[] РІРµСЂС…РЅРµРіРѕ СѓСЂРѕРІРЅСЏ (РґРµСЂРµРІРѕ: РґРµС‚Рё
// СЃС‚СЂСѓРєС‚СѓСЂ/СЌРЅСѓРјРѕРІ/РЅРµР№РјСЃРїРµР№СЃРѕРІ РІРЅСѓС‚СЂРё РєР°Р¶РґРѕР№ Р·Р°РїРёСЃРё).
static JsonValue buildDocumentSymbols(const string& uri, const string& text) {
    JsonValue arr = JArray();
    auto treeIt = g_trees.find(uri);
    if (treeIt == g_trees.end()) return arr;
    std::vector<size_t> lineStarts = buildLineStarts(text);
    auto tokIt = g_tokens.find(uri);
    std::vector<Token> tokens = (tokIt != g_tokens.end())
        ? tokIt->second : std::vector<Token>();
    // РЎРµРјР°РЅС‚РёРєР° вЂ” РєР°Рє Сѓ hover: РєРѕРЅС‚РµРєСЃС‚, СЃС‚СЂСѓРєС‚СѓСЂС‹, enum'С‹ (РґР»СЏ detail-РїРѕРґРїРёСЃРµР№)
    Context ctx;
    auto ctxIt = g_contexts.find(uri);
    if (ctxIt != g_contexts.end()) ctx = ctxIt->second;
    const std::map<String, StructSymbol>* structBuf = nullptr;
    auto structIt = g_structs.find(uri);
    if (structIt != g_structs.end()) structBuf = &structIt->second;
    const std::map<String, EnumSymbol>* enumBuf = nullptr;
    auto enumIt = g_enums.find(uri);
    if (enumIt != g_enums.end()) enumBuf = &enumIt->second;
    SymEnv env;
    env.curPath = pathFromUri(uri);
    for (Node* n : treeIt->second)
        pushSymbols(n, tokens, lineStarts, text, env, ctx, structBuf, enumBuf, "", arr);
    return arr;
}

// ---------- Р СћР С•РЎвЂЎР С”Р В° Р Р†РЎвЂ¦Р С•Р Т‘Р В° ----------
int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    loadKeywordDocs(keywordDocsDir());

    string raw;
    while (readMessage(raw)) {
        JsonValue req = parseJson(raw);
        string method = req["method"].asString();
        JsonValue id = req["id"]; // Р С”Р С•Р С—Р С‘РЎРЏ (Р СР С•Р В¶Р ВµРЎвЂљ Р В±РЎвЂ№РЎвЂљРЎРЉ null Р Т‘Р В»РЎРЏ Р Р…Р С•РЎвЂљР С‘РЎвЂћР С‘Р С”Р В°РЎвЂ Р С‘Р в„–)
        bool responded = false;

        if (method == "initialize") {
            // настройки редактора: initializationOptions.lumen.deepAnalysis.enable=false
            // отключает глубокий анализ (типы/ошибки/inlay) — остаётся семантика + подсветка
            JsonValue deepOpts = req["params"]["initializationOptions"]["lumen"]["deepAnalysis"];
            if (deepOpts.isObject())
                g_noDeepAnalysis = !(deepOpts["enable"].isNull() ? true : deepOpts["enable"].asBool());
            sendInitializeResult(id);
            responded = true;
        }
        else if (method == "textDocument/didOpen" || method == "textDocument/didChange") {
            JsonValue td = req["params"]["textDocument"];
            string uri  = td["uri"].asString();
            if (method == "textDocument/didOpen") {
                string text = td["text"].asString();
                g_docs[uri] = text;
                analyzeAndPublish(uri, text);
            } else {
                JsonValue changes = req["params"]["contentChanges"];
                if (changes.isArray()) {
                    string& doc = g_docs[uri];
                    for (const auto& ch : changes.array) {
                        JsonValue range = ch["range"];
                        string ctext = ch["text"].asString();
                        if (range.isObject()) {
                            int sl = (int)range["start"]["line"].asNumber();
                            int sc = (int)range["start"]["character"].asNumber();
                            int el = (int)range["end"]["line"].asNumber();
                            int ec = (int)range["end"]["character"].asNumber();
                            size_t so = lspPosToByteOffset(doc, sl, sc);
                            size_t eo = lspPosToByteOffset(doc, el, ec);
                            if (so > doc.size()) so = doc.size();
                            if (eo < so) eo = so;
                            doc = doc.substr(0, so) + ctext + doc.substr(eo);
                        } else {
                            doc = ctext; // РЎвЂћР С•Р В»Р В±РЎРЊР С”: Р С—Р С•Р В»Р Р…Р В°РЎРЏ Р С—Р ВµРЎР‚Р ВµРЎРѓР С‘Р Р…РЎвЂ¦РЎР‚Р С•Р Р…Р С‘Р В·Р В°РЎвЂ Р С‘РЎРЏ
                        }
                    }
                    analyzeAndPublish(uri, doc);
                }
            }
        }
        else if (method == "textDocument/didClose") {
            string uri = req["params"]["textDocument"]["uri"].asString();
            g_docs.erase(uri);
            analyzeAndPublish(uri, ""); // Р С—РЎС“РЎРѓРЎвЂљРЎвЂ№Р Вµ Р Т‘Р С‘Р В°Р С–Р Р…Р С•РЎРѓРЎвЂљР С‘Р С”Р С‘
        }
        else if (method == "textDocument/inlayHint") {
            JsonValue params = req["params"];
            string uri = params["textDocument"]["uri"].asString();
            int sLine = (int)params["range"]["start"]["line"].asNumber();
            int eLine = (int)params["range"]["end"]["line"].asNumber();

            JsonValue arr = JArray();
            auto it = g_inlayHints.find(uri);
            if (it != g_inlayHints.end()) {
                for (const auto& h : it->second) {
                    if (h.line < sLine || h.line > eLine) continue;
                    JsonValue hint = JObject();
                    JsonValue pos = JObject();
                    pos.object["line"]      = JNumber(h.line);
                    pos.object["character"] = JNumber(h.character);
                    hint.object["position"]    = pos;
                    hint.object["label"]       = JString(h.label.to_utf8());
                    hint.object["kind"]        = JNumber(1); // Type
                    hint.object["paddingLeft"] = JBool(true);
                    arr.array.push_back(hint);
                }
            }
            sendResult(id, arr);
            responded = true;
        }
        else if (method == "textDocument/semanticTokens/full") {
            JsonValue params = req["params"];
            string uri = params["textDocument"]["uri"].asString();

            JsonValue result = JObject();
            JsonValue data = JArray();

            auto tokIt = g_tokens.find(uri);
            auto ctxIt = g_contexts.find(uri);
            auto treeIt = g_trees.find(uri);
            auto docIt = g_docs.find(uri);
            if (tokIt != g_tokens.end() && ctxIt != g_contexts.end() && treeIt != g_trees.end()) {
                std::vector<size_t> lineStarts = buildLineStarts(
                    docIt != g_docs.end() ? docIt->second : "");
            std::vector<Macro> macBuf;
            auto macIt = g_macros.find(uri);
            if (macIt != g_macros.end()) macBuf = macIt->second;
            data = buildSemanticTokens(tokIt->second, ctxIt->second,
                                       treeIt->second, lineStarts, macBuf);
            }

            result.object["data"] = data;
            sendResult(id, result);
            responded = true;
        }
        else if (method == "textDocument/completion") {
            JsonValue params = req["params"];
            string uri = params["textDocument"]["uri"].asString();
            int cLine = (int)params["position"]["line"].asNumber();
            int cChar = (int)params["position"]["character"].asNumber();
            auto it = g_docs.find(uri);
            string text = (it != g_docs.end()) ? it->second : "";

            JsonValue res = JObject();
            res.object["isIncomplete"] = JBool(false);
            res.object["items"] = buildCompletion(text, pathFromUri(uri), cLine, cChar);
            sendResult(id, res);
            responded = true;
        }
        else if (method == "textDocument/hover") {
            JsonValue params = req["params"];
            string uri = params["textDocument"]["uri"].asString();
            int hLine = (int)params["position"]["line"].asNumber();
            int hChar = (int)params["position"]["character"].asNumber();

            auto it = g_docs.find(uri);
            string text = (it != g_docs.end()) ? it->second : "";

            JsonValue res = JObject();
            auto ctxIt = g_contexts.find(uri);
            Context ctx;
            if (ctxIt != g_contexts.end()) ctx = ctxIt->second;

            // Р СћР С•Р С”Р ВµР Р…РЎвЂ№ Р С‘ AST РІР‚вЂќ Р Т‘Р В»РЎРЏ РЎР‚Р В°Р В·РЎР‚Р ВµРЎв‚¬Р ВµР Р…Р С‘РЎРЏ Р С—Р В°РЎР‚Р В°Р СР ВµРЎвЂљРЎР‚Р С•Р Р† Р С•Р В±РЎР‰Р ВµР СР В»РЎР‹РЎвЂ°Р ВµР в„– РЎвЂћРЎС“Р Р…Р С”РЎвЂ Р С‘Р С‘
            std::vector<Token> tokBuf;
            std::vector<Node*> treeBuf;
            auto tokIt = g_tokens.find(uri);
            auto treeIt = g_trees.find(uri);
            if (tokIt != g_tokens.end()) tokBuf = tokIt->second;
            if (treeIt != g_trees.end()) treeBuf = treeIt->second;

            // #define / #macro Р С—РЎР‚Р ВµР С—РЎР‚Р С•РЎвЂ Р ВµРЎРѓРЎРѓР С•РЎР‚Р В° РЎРЊРЎвЂљР С•Р С–Р С• Р Т‘Р С•Р С”РЎС“Р СР ВµР Р…РЎвЂљР В° (Р Т‘Р В»РЎРЏ hover)
            std::vector<Define> defBuf;
            std::vector<Macro> macBuf;
            auto defIt = g_defines.find(uri);
            auto macIt = g_macros.find(uri);
            if (defIt != g_defines.end()) defBuf = defIt->second;
            if (macIt != g_macros.end()) macBuf = macIt->second;

            // Р РЋРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚РЎвЂ№ РЎРЊРЎвЂљР С•Р С–Р С• Р Т‘Р С•Р С”РЎС“Р СР ВµР Р…РЎвЂљР В° (Р Т‘Р В»РЎРЏ hover Р С—Р С• РЎвЂЎР В»Р ВµР Р…Р В°Р С)
            const std::map<String, StructSymbol>* structBuf = nullptr;
            auto structIt = g_structs.find(uri);
            if (structIt != g_structs.end()) structBuf = &structIt->second;

            const std::map<String, EnumSymbol>* enumBuf = nullptr;
            auto enumIt = g_enums.find(uri);
            if (enumIt != g_enums.end()) enumBuf = &enumIt->second;

            const std::map<unsigned long long, Type>* ltBuf = nullptr;
            auto ltIt = g_localTypes.find(uri);
            if (ltIt != g_localTypes.end()) ltBuf = &ltIt->second;

            JsonValue hover = buildHover(text, pathFromUri(uri), hLine, hChar, ctx,
                                         tokBuf, treeBuf, defBuf, macBuf, structBuf, enumBuf, ltBuf);
            if (hover.object.find("contents") == hover.object.end())
                sendResult(id, JObject()); // null hover
            else
                sendResult(id, hover);
            responded = true;
        }
        else if (method == "textDocument/documentSymbol") {
            string uri  = req["params"]["textDocument"]["uri"].asString();
            auto it = g_docs.find(uri);
            string text = (it != g_docs.end()) ? it->second : "";
            sendResult(id, buildDocumentSymbols(uri, text));
            responded = true;
        }
        else if (method == "textDocument/definition") {
            JsonValue params = req["params"];
            string uri = params["textDocument"]["uri"].asString();
            int dLine = (int)params["position"]["line"].asNumber();
            int dChar = (int)params["position"]["character"].asNumber();

            auto it = g_docs.find(uri);
            string text = (it != g_docs.end()) ? it->second : "";

            Context ctx;
            auto ctxIt = g_contexts.find(uri);
            if (ctxIt != g_contexts.end()) ctx = ctxIt->second;

            std::vector<Token> tokBuf;
            std::vector<Node*> treeBuf;
            auto tokIt = g_tokens.find(uri);
            auto treeIt = g_trees.find(uri);
            if (tokIt != g_tokens.end()) tokBuf = tokIt->second;
            if (treeIt != g_trees.end()) treeBuf = treeIt->second;

            std::vector<Define> defBuf;
            std::vector<Macro> macBuf;
            auto defIt = g_defines.find(uri);
            auto macIt = g_macros.find(uri);
            if (defIt != g_defines.end()) defBuf = defIt->second;
            if (macIt != g_macros.end()) macBuf = macIt->second;

            const std::map<String, StructSymbol>* structBuf = nullptr;
            auto structIt = g_structs.find(uri);
            if (structIt != g_structs.end()) structBuf = &structIt->second;

            const std::map<String, EnumSymbol>* enumBuf = nullptr;
            auto enumIt = g_enums.find(uri);
            if (enumIt != g_enums.end()) enumBuf = &enumIt->second;

            const std::map<unsigned long long, Type>* ltBuf = nullptr;
            auto ltIt = g_localTypes.find(uri);
            if (ltIt != g_localTypes.end()) ltBuf = &ltIt->second;

            JsonValue res = buildDefinition(text, uri, dLine, dChar, ctx,
                                            tokBuf, treeBuf, defBuf, macBuf,
                                            structBuf, enumBuf, ltBuf);
            if (res.object.empty())
                sendResult(id, JObject());   // РЅРµС‚ РѕРїСЂРµРґРµР»РµРЅРёСЏ
            else
                sendResult(id, res);
            responded = true;
        }
        else if (method == "completionItem/resolve") {
            // lumen-lsp не кладёт документацию в completion-элементы, а hover
            // по позиции видит только набранный префикс (i вместо i16), поэтому
            // клиент кладёт в item.data реальный диапазон префикса. Здесь мы
            // "дописываем" слово в буфере до полного label и считаем hover на
            // получившемся тексте — документация получается ровно та же, что
            // при наведении мыши на полностью набранное слово.
            JsonValue item = req["params"];
            JsonValue data = item["data"];
            string uri = data["uri"].asString();
            string label = item["label"].asString();
            JsonValue rp = data["repStart"];
            int rLine = (int)rp["line"].asNumber();
            int rChar = (int)rp["character"].asNumber();
            int rLen  = (int)data["prefixLen"].asNumber();
            if (!label.empty() && !uri.empty()) {
                auto it = g_docs.find(uri);
                string text = (it != g_docs.end()) ? it->second : "";
                if (!text.empty()) {
                    size_t off = lspPosToByteOffset(text, rLine, rChar);
                    if (off > text.size()) off = text.size();
                    size_t lim = text.size() - off;
                    if ((size_t)rLen > lim) rLen = (int)lim;
                    text = text.substr(0, off) + label
                         + text.substr(off + (size_t)rLen);

                    // Позиция hover — на последнем символе вставленного слова
                    size_t u16 = 0, ii = 0;
                    while (ii < label.size()) {
                        unsigned char c = (unsigned char)label[ii];
                        int cl;
                        if (c < 0x80) cl = 1;
                        else if ((c & 0xE0) == 0xC0) cl = 2;
                        else if ((c & 0xF0) == 0xE0) cl = 3;
                        else if ((c & 0xF8) == 0xF0) cl = 4;
                        else { ii++; continue; }
                        u16 += (cl >= 4) ? 2 : 1;
                        ii += cl;
                    }
                    int cChar = rChar + (int)u16 - 1;
                    if (cChar < 0) cChar = 0;

                    Context ctx;
                    auto ctxIt = g_contexts.find(uri);
                    if (ctxIt != g_contexts.end()) ctx = ctxIt->second;

                    std::vector<Token> tokBuf;
                    std::vector<Node*> treeBuf;
                    auto tokIt = g_tokens.find(uri);
                    auto treeIt = g_trees.find(uri);
                    if (tokIt != g_tokens.end()) tokBuf = tokIt->second;
                    if (treeIt != g_trees.end()) treeBuf = treeIt->second;

                    std::vector<Define> defBuf;
                    std::vector<Macro> macBuf;
                    auto defIt = g_defines.find(uri);
                    auto macIt = g_macros.find(uri);
                    if (defIt != g_defines.end()) defBuf = defIt->second;
                    if (macIt != g_macros.end()) macBuf = macIt->second;

                    const std::map<String, StructSymbol>* structBuf = nullptr;
                    auto structIt = g_structs.find(uri);
                    if (structIt != g_structs.end()) structBuf = &structIt->second;

                    const std::map<String, EnumSymbol>* enumBuf = nullptr;
                    auto enumIt = g_enums.find(uri);
                    if (enumIt != g_enums.end()) enumBuf = &enumIt->second;

                    const std::map<unsigned long long, Type>* ltBuf = nullptr;
                    auto ltIt = g_localTypes.find(uri);
                    if (ltIt != g_localTypes.end()) ltBuf = &ltIt->second;

                    JsonValue hover = buildHover(text, pathFromUri(uri), rLine,
                                                 cChar, ctx, tokBuf, treeBuf,
                                                 defBuf, macBuf, structBuf,
                                                 enumBuf, ltBuf);
                    if (hover.object.find("contents") != hover.object.end()) {
                        JsonValue doc = JObject();
                        doc.object["kind"] = JString("markdown");
                        doc.object["value"] = hover["contents"]["value"];
                        item.object["documentation"] = doc;
                    }
                }
            }
            sendResult(id, item);
            responded = true;
        }
        else if (method == "shutdown") {
            sendResult(id, JObject());
            responded = true;
        }
        else if (method == "exit") {
            break;
        }

        // Р СњР ВµР С•Р В±РЎР‚Р В°Р В±Р С•РЎвЂљР В°Р Р…Р Р…РЎвЂ№Р в„– request (Р ВµРЎРѓРЎвЂљРЎРЉ id) РІР‚вЂќ Р С•РЎвЂљР Р†Р ВµРЎвЂЎР В°Р ВµР С Р С—РЎС“РЎРѓРЎвЂљРЎвЂ№Р С РЎР‚Р ВµР В·РЎС“Р В»РЎРЉРЎвЂљР В°РЎвЂљР С•Р С,
        // РЎвЂЎРЎвЂљР С•Р В±РЎвЂ№ Р С”Р В»Р С‘Р ВµР Р…РЎвЂљ Р Р…Р Вµ Р В·Р В°Р Р†Р С‘РЎРѓР В°Р В» (Р Р…Р В°Р С—РЎР‚Р С‘Р СР ВµРЎР‚, workspace/* Р В·Р В°Р С—РЎР‚Р С•РЎРѓРЎвЂ№).
        if (!responded && !id.isNull())
            sendResult(id, JObject());
    }
    return 0;
}
