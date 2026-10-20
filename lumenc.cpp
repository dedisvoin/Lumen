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
#include "compiler/TypeAnalyzer.cpp"
#include "compiler/CodeGenerator.cpp"

#include <filesystem>
#include <limits>
#include <cmath>


// --- РџРѕР»РЅС‹Р№ РєРѕРЅРІРµР№РµСЂ: СЃРєРѕРјРїРёР»РёСЂРѕРІР°С‚СЊ СЃРіРµРЅРµСЂРёСЂРѕРІР°РЅРЅС‹Р№ .cpp РІ .exe С‡РµСЂРµР· g++ ---
String stripExtension(const String& name) {
    std::string s = name.to_utf8();
    size_t dot = s.rfind('.');
    size_t slash = s.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        s = s.substr(0, dot);
    return String(s);
}

// Собрать exe. Возвращает путь к бинарнику (пустая строка при ошибке).
String buildExe(const CompilerArgs& args, const String& cpp_file,
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
        return exe_name;
    } else {
        std::cout << Style("error: ", TerminalStyle::BRIGHT_RED)
                  << Style("could not compile `" + exe_name + "` due to previous error",
                           TerminalStyle::DIM_WHITE) << std::endl;
        return String();
    }
}

// -bench: множитель ускорения в формате "6.20x" (invariant: точка).
String rustSpeed(double x) noexcept {
    if (!(x > 0.0) || !std::isfinite(x)) return String("--");
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.2f", x);
    return String(buffer);
}

// -bench: прогнать нативный бинарник (лучшее из runs) и напечатать сравнение
// времени интерпретатора и нативной сборки в стиле cargo/debug.
int runBenchmark(const CompilerArgs& args, const String& exe, double interp_ms,
                 bool interp_truncated) {
    if (exe.empty()) return 1;
#ifdef _WIN32
    const String redirect(" > NUL 2>&1");
#else
    const String redirect(" > /dev/null 2>&1");
#endif
    const int runs = (args.BENCH_RUNS < 1) ? 1 : args.BENCH_RUNS;
    String run_cmd = String("\"") + exe + String("\"") + redirect;

    double native_ms = (std::numeric_limits<double>::max)();
    bool native_ok = false;
    for (int i = 0; i < runs; ++i) {
        auto tb = std::chrono::steady_clock::now();
        int rc = std::system(run_cmd.to_utf8().c_str());
        double ms = elapsedMs(tb);
        if (rc == 0 && ms < native_ms) { native_ms = ms; native_ok = true; }
    }

    String opt = args.OPT_LEVEL.empty() ? String("default") : args.OPT_LEVEL;
    std::cout << Style(" Benchmark ", TerminalStyle::BRIGHT_GREEN)
              << Style(args.FILE_PATH, TerminalStyle::BRIGHT_CYAN)
              << Style(" (native ", TerminalStyle::DIM_WHITE)
              << Style(opt, TerminalStyle::BRIGHT_GREEN)
              << Style(", best of ", TerminalStyle::DIM_WHITE)
              << Style(String(std::to_string(runs)), TerminalStyle::BRIGHT_GREEN)
              << Style(")", TerminalStyle::DIM_WHITE) << std::endl;

    std::cout << Style("         interpreter  ", TerminalStyle::DIM_WHITE)
              << rustTime(interp_ms);
    if (interp_truncated)
        std::cout << Style("  (interpreter budget limit reached)", TerminalStyle::DIM);
    std::cout << std::endl;

    std::cout << Style("         native       ", TerminalStyle::DIM_WHITE);
    if (native_ok) std::cout << rustTime(native_ms);
    else           std::cout << Style("failed", TerminalStyle::BRIGHT_RED);
    std::cout << std::endl;

    if (native_ok && native_ms > 0.0) {
        std::cout << Style("         speedup      ", TerminalStyle::DIM_WHITE);
        if (interp_truncated)
            std::cout << Style("--", TerminalStyle::DIM)
                      << Style("  (interpreter truncated by budget)", TerminalStyle::DIM);
        else
            std::cout << Style(rustSpeed(interp_ms / native_ms) + String("x"), TerminalStyle::BRIGHT_GREEN);
        std::cout << std::endl;
    }
    return native_ok ? 0 : 1;
}


int main(int argc, char** argv) {
    system("cls");

        auto args = convertArgs(argc, argv);
    auto compiler_args = parseToArgs(args);
    // outCompilerArgs(compiler_args);

    if (compiler_args.SHOW_HELP) { printHelp(); return 0; }
    if (compiler_args.FILE_PATH.empty()) return 0;

    // -bench: замер времени выполнения. Требует сборки exe; если уровень
    // оптимизации не задан явно — берём -O2 (как в регрессионном бенчмарке).
    if (compiler_args.BENCHMARK) {
        compiler_args.BUILD_EXE = true;
        if (compiler_args.OPT_LEVEL.empty()) compiler_args.OPT_LEVEL = "-O2";
    }

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
    auto type_analyzer = TypeAnalyzer(nodes, error_container);
    deep_analizer.print_input_prompts = compiler_args.RUN_INTERPRET;
    deep_analizer.interpret_mode = compiler_args.RUN_INTERPRET || compiler_args.BENCHMARK;
    const bool run_interp = compiler_args.RUN_INTERPRET || compiler_args.BENCHMARK;
    // M9: TypeAnalyzer — движок по умолчанию. Откат к полному интерпретатору:
    // -r/-bench (нужен именно интерпретатор), -deep/--strict, а также -noanalize
    // (пропустить анализ вовсе). Флаг -types остаётся для явного выбора статики.
    const bool want_interp = run_interp || compiler_args.STRICT_DEEP;
    const bool use_types = !compiler_args.NO_DEEP_ANALYZE && !want_interp;
    const bool run_deep  = !compiler_args.NO_DEEP_ANALYZE && want_interp;
    if (use_types)      type_analyzer();
    else if (run_deep)  deep_analizer();
    // Для ТРАНСПИЛЯТОРА `eval { ... }` — прозрачная обёртка: тело эмитится как
    // обычный рантайм-код (CodeGenerator, case NodeEvalBlock), а compile-time
    // исполнение eval здесь НЕ запускается. Так транспиляция не зависит от
    // содержимого блока (изоляция/бюджет/заморозка — прерогатива LSP-хинтов,
    // где eval исполняет lumen-lsp.cpp). `-r`/`-bench`/`-deep` и так гоняют
    // полный интерпретатор.
    const bool ran_eval = false;
    (void)ran_eval;
    double t_deep = (use_types || run_deep || ran_eval) ? elapsedMs(t0) : 0.0;
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


    // -r: вывод результата интерпретатора. Фильтр: только echo-подсказки (метки " > " — типы).
    // При -bench ранний возврат не делаем: нужно ещё собрать и прогнать нативный бинарник.
    if (compiler_args.RUN_INTERPRET && !compiler_args.BENCHMARK) {
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

    String built_exe;
    if (compiler_args.EMIT_CPP || compiler_args.BUILD_EXE) {
        size_t hard_errors = 0;
        for (auto& e : error_container.errors)
            if (!e.is_warning) hard_errors++;
        if (hard_errors == 0) {
            // Решения резолва перегрузок от выбранного движка (nullptr, если
            // анализ не запускался: тогда кодоген работает на своём fallback).
            const std::map<Node*, Node*>* resolved =
                use_types ? &type_analyzer.resolved_calls
                          : (run_deep ? &deep_analizer.resolved_calls : nullptr);
            auto generator = CodeGenUnit(ast_generator.nodes, dbg, preproccesor.cpp_includes,
                                        resolved);
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
            if (compiler_args.BUILD_EXE) built_exe = buildExe(compiler_args, cpp_path, error_container, dbg);
        }
    }

    // -bench: сравнить время выполнения интерпретатора и нативного бинарника.
    if (compiler_args.BENCHMARK) {
        bool interp_truncated = deep_analizer.any_budget_hit;
        return runBenchmark(compiler_args, built_exe, t_deep, interp_truncated);
    }

}
