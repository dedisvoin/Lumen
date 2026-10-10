#include "compiler/core/ErrorBase.cpp"
#include "compiler/utils/Strings.cpp"
#include "compiler/utils/File.cpp"
#include "compiler/core/Tokens.cpp"
#include "compiler/utils/ArgParser.cpp"
#include "compiler/utils/Debugs.cpp"

#include "compiler/Lexer.cpp"
#include "compiler/Preprocessor.cpp"
#include "compiler/ASTGenerator.cpp"
#include "compiler/SemanticAnalizer.cpp"
#include "compiler/DeepAnalizer.cpp"
#include "compiler/CodeGenerator.cpp"

#include <filesystem>


// --- РџРѕР»РЅС‹Р№ РєРѕРЅРІРµР№РµСЂ: СЃРєРѕРјРїРёР»РёСЂРѕРІР°С‚СЊ СЃРіРµРЅРµСЂРёСЂРѕРІР°РЅРЅС‹Р№ .cpp РІ .exe С‡РµСЂРµР· g++ ---
String stripExtension(const String& name) {
    std::string s = name.to_utf8();
    size_t dot = s.rfind('.');
    size_t slash = s.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        s = s.substr(0, dot);
    return String(s);
}

void buildExe(const CompilerArgs& args, const String& cpp_file,
              ErrorContainer& error_container, const bool dbg) {
    String exe_name = args.OUT_FILE_NAME.empty()
                        ? String("out.exe")
                        : (stripExtension(args.OUT_FILE_NAME) + String(".exe"));
        String opt = args.OPT_LEVEL.empty() ? String("") : (args.OPT_LEVEL + String(" "));
    String cmd = String("clang ") + opt + String("-std=c++17 -I\"compiler/runtime\" -o \"") + exe_name
                 + String("\" \"") + cpp_file + String("\" 2>&1");
    if (dbg) std::cout << Style(" Compile `", TerminalStyle::BRIGHT_GREEN)
                       << Style(cmd, TerminalStyle::BRIGHT_YELLOW)
                       << Style("`", TerminalStyle::DIM_WHITE) << std::endl;
    auto t_build = std::chrono::steady_clock::now();
    int rc = std::system(cmd.to_utf8().c_str());
    bool ok = (rc == 0);
    if (ok) {
        double t_b = elapsedMs(t_build);
        std::cout << Style("Finished ", TerminalStyle::BRIGHT_GREEN)
                  << Style("`dev` profile [unoptimized + debuginfo] target(s) in ", TerminalStyle::DIM_WHITE)
                  << rustTime(t_b) << std::endl;
    } else {
        std::cout << Style("error: ", TerminalStyle::BRIGHT_RED)
                  << Style("could not compile `" + exe_name + "` due to previous error",
                           TerminalStyle::DIM_WHITE) << std::endl;
    }
}


int main(int argc, char** argv) {
    system("cls");

        auto args = convertArgs(argc, argv);
    auto compiler_args = parseToArgs(args);
    // outCompilerArgs(compiler_args);

    if (compiler_args.SHOW_HELP) { printHelp(); return 0; }
    if (compiler_args.FILE_PATH.empty()) return 0;

    const bool dbg = compiler_args.DEBUG_MODE;
    ErrorContainer error_container;

    auto lexer = Lexer(dbg);
    String file_data = openFile(compiler_args.FILE_PATH);
    if (dbg) DEBUG_PIPELINE_HEADER(compiler_args.FILE_PATH, file_data.size());

    auto t0 = std::chrono::steady_clock::now();
    auto tokens = lexer(file_data, compiler_args.FILE_PATH);
    double t_lex = elapsedMs(t0);
    if (dbg) DEBUG_PIPELINE_STAGE("Lexer", String(std::to_string(tokens.size())) + " tokens", t_lex, true);

    if (compiler_args.SAVE_TOKENS_FROM_FILE) saveTokensVector(tokens, "tokens.txt");

    size_t err_before = error_container.errors.size();
    t0 = std::chrono::steady_clock::now();
    auto preproccesor = Preprocessor(dbg, error_container);
    auto result = preproccesor(tokens);
    double t_pp = elapsedMs(t0);
    size_t pp_errors = error_container.errors.size() - err_before;
    if (dbg) {
        TRACE_DETAIL("Preprocessor",
            Style("pulled ", TerminalStyle::DIM_WHITE)
            + Style(String(std::to_string(preproccesor.includes_pulled)), TerminalStyle::BRIGHT_GREEN)
            + Style(" includes, defined ", TerminalStyle::DIM_WHITE)
            + Style(String(std::to_string(preproccesor.defines_seen)), TerminalStyle::BRIGHT_GREEN)
            + Style(" defines, ", TerminalStyle::DIM_WHITE)
            + Style(String(std::to_string(preproccesor.macros_seen)), TerminalStyle::BRIGHT_GREEN)
            + Style(" macros", TerminalStyle::DIM_WHITE));
    }
    if (dbg) DEBUG_PIPELINE_STAGE("Preprocessor", String(std::to_string(result.size())) + " tokens", t_pp, pp_errors == 0);
    if (compiler_args.SAVE_TOKENS_FROM_FILE) saveTokensVector(result, "tokens_p.txt");
    if (compiler_args.SAVE_PREPROCESSED_FILE) saveFile("pp.lm", stringFromTokens(result));


    err_before = error_container.errors.size();
    t0 = std::chrono::steady_clock::now();
    auto ast_generator = ASTGenerator(result, preproccesor.source_tokens, dbg, error_container);
    auto nodes = ast_generator();
    double t_ast = elapsedMs(t0);
    size_t ast_errors = error_container.errors.size() - err_before;
    if (dbg) DEBUG_PIPELINE_STAGE("AST generation", String(std::to_string(nodes.size())) + " nodes", t_ast, ast_errors == 0);


    err_before = error_container.errors.size();
    t0 = std::chrono::steady_clock::now();
    auto semantic_analizer = ASTAnalizer(nodes, error_container);
    semantic_analizer();
    double t_sem = elapsedMs(t0);
    size_t sem_errors = error_container.errors.size() - err_before;
    size_t sem_hard = 0;
    for (size_t i = err_before; i < error_container.errors.size(); i++) if (!error_container.errors[i].is_warning) sem_hard++;
    if (dbg) DEBUG_PIPELINE_STAGE("Semantic analysis", String(std::to_string(sem_errors)) + " diagnostics", t_sem, sem_hard == 0);

    err_before = error_container.errors.size();
    t0 = std::chrono::steady_clock::now();
    auto deep_analizer = ASTDeepAnalyzer(nodes, error_container);
    deep_analizer.print_input_prompts = compiler_args.RUN_INTERPRET;
    deep_analizer.interpret_mode = compiler_args.RUN_INTERPRET;
    const bool run_deep = !compiler_args.NO_DEEP_ANALYZE || compiler_args.RUN_INTERPRET;
    if (run_deep) deep_analizer();
    double t_deep = run_deep ? elapsedMs(t0) : 0.0;
    size_t deep_errors = error_container.errors.size() - err_before;
    size_t deep_hard = 0;
    for (size_t i = err_before; i < error_container.errors.size(); i++) if (!error_container.errors[i].is_warning) deep_hard++;
    if (dbg) DEBUG_PIPELINE_STAGE("Deep analysis", String(std::to_string(deep_errors)) + " diagnostics", t_deep, deep_hard == 0);

    if (dbg) {
        int errors = 0, warnings = 0;
        for (auto& e : error_container.errors) { e.is_warning ? warnings++ : errors++; }
        DEBUG_PIPELINE_DEFER_FINISHED(compiler_args.BUILD_EXE);
        DEBUG_PIPELINE_SUMMARY(compiler_args.FILE_PATH, t_lex + t_pp + t_ast + t_sem + t_deep, errors, warnings);
        DEBUG_PIPELINE_FLUSH();
    }

    error_container.viewError();


    // -r: РІС‹РІРѕРґ СЂРµР·СѓР»СЊС‚Р°С‚Р° РёРЅС‚РµСЂРїСЂРµС‚Р°С‚РѕСЂР°. Р¤РёР»СЊС‚СЂ: С‚РѕР»СЊРєРѕ echo-РїРѕРґСЃРєР°Р·РєРё (РјРµС‚РєРё " > " вЂ” С‚РёРїС‹).
    if (compiler_args.RUN_INTERPRET) {
        for (auto& h : deep_analizer.inlay_hints)
            if (h.label.size() < 2 || h.label[0] != U' ' || h.label[1] != U'>') {
                String v = h.label;
                while (!v.empty() && (v[0] == U' ' || v[0] == U'\t')) v = v.substr(1);
                std::cout << v << std::endl;
            }
        if (deep_analizer.exit_requested) return deep_analizer.exit_code;
        return 0;
    }

    if (compiler_args.PRINT_AST_TREE) printNodes(ast_generator.nodes);

    if (compiler_args.EMIT_CPP || compiler_args.BUILD_EXE) {
        size_t hard_errors = 0;
        for (auto& e : error_container.errors)
            if (!e.is_warning) hard_errors++;
        if (hard_errors == 0) {
            // Решения резолва перегрузок от deep-анализатора (nullptr при -noanalize:
            // тогда кодоген работает на своём строковом fallback).
            auto generator = CodeGenUnit(ast_generator.nodes, dbg, preproccesor.cpp_includes,
                                        run_deep ? &deep_analizer.resolved_calls : nullptr);
            String cpp_path = compiler_args.OUT_FILE_NAME.empty()
                                ? String("out.cpp")
                                : stripExtension(compiler_args.OUT_FILE_NAME) + String(".cpp");
            String exe_path;
            if (compiler_args.BUILD_EXE) exe_path = stripExtension(cpp_path) + String(".exe");
            if (dbg) {
                std::cout << Style("Emitting (C++) ", TerminalStyle::BRIGHT_GREEN)
                          << Style(std::filesystem::absolute(cpp_path.to_utf8()).make_preferred().u8string(),
                                   TerminalStyle::BRIGHT_CYAN) << std::endl;
            }
            generator.generate(cpp_path);
            if (dbg) {
                size_t gen_lines = 0;
                String cpp_data = openFile(cpp_path);
                for (size_t i = 0; i < cpp_data.size(); i++) if (cpp_data[i] == '\n') gen_lines++;
                std::cout << Style("         Generated ", TerminalStyle::DIM_WHITE)
                          << Style(String(std::to_string(gen_lines)) + " lines", TerminalStyle::BRIGHT_GREEN) << std::endl;
            }
            if (compiler_args.BUILD_EXE) buildExe(compiler_args, cpp_path, error_container, dbg);
        }
    }

}
