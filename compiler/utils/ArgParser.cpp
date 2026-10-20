#include "Strings.cpp"
#include "vector"
#include "TerminalStyle.cpp"

[[noreturn]] void outError(const String& msg) noexcept {
    std:: cout << Style("error: ", TerminalStyle::RED) << msg << std:: endl;
    exit(-1);
}

struct CompilerArgs {
    // flags
    bool SAVE_TOKENS_FROM_FILE = false;
    bool DEBUG_MODE = false;
    bool SAVE_PREPROCESSED_FILE = false;
    bool PRINT_AST_TREE = false;
    bool EMIT_CPP = false;
    bool BUILD_EXE = false;
    bool RUN_INTERPRET = false;   // -r: выполнить DeepAnalizer (интерпретатор) и вывести результат echo
    bool SHOW_HELP = false;       // -h: показать справку
    bool NO_DEEP_ANALYZE = false; // -noanalize: пропустить глубокий анализ при компиляции
    bool BENCHMARK = false;       // -bench: замерить время выполнения (интерпретатор vs нативный бинарник)
    int  BENCH_RUNS = 3;          // -benchruns <n>: прогонов нативного бинарника (берётся лучший)
    bool USE_TYPE_ANALYZER = false; // -types: тип-ориентированный анализ без исполнения (вместо интерпретатора)
    bool STRICT_DEEP = false;       // -deep/--strict: полный интерпретатор в обычной сборке (откат к старому поведению)

    // Optimization
    String OPT_LEVEL;             // -O1, -O2, -O3: уровень оптимизации g++ (по умолчанию -O2)

    // Data
    String FILE_PATH;
    String OUT_FILE_NAME;
};

std:: vector<String> convertArgs(const int argc, char** argv) noexcept {
    std:: vector<String> args;
    for (int i = 0; i < argc; i++) { args.push_back(String(argv[i])); }
    return args;
}

CompilerArgs parseToArgs(const std:: vector<String>& data) noexcept {
    CompilerArgs args;
    for (size_t i = 0; i < data.size(); i++) {
        if (data[i] == "-f") {
            if (i == data.size() - 1) outError("waited the file path next the '-f' flag");
            args.FILE_PATH = data[i + 1];
            i++;
        } else if (data[i] == "-o") {
            if (i == data.size() - 1) outError("waited the output file name next the '-o' flag");
            args.OUT_FILE_NAME = data[i + 1];
            i++;
        } else if (data[i] == "-d") {
            args.DEBUG_MODE = true;
        } else if (data[i] == "-t") {
            args.SAVE_TOKENS_FROM_FILE = true;
        } else if (data[i] == "-p") {
            args.SAVE_PREPROCESSED_FILE = true;
        } else if (data[i] == "-tree") {
            args.PRINT_AST_TREE = true;
        } else if (data[i] == "-c") {
            args.EMIT_CPP = true;
        } else if (data[i] == "-build") {
            args.BUILD_EXE = true;
        } else if (data[i] == "-r") {
            args.RUN_INTERPRET = true;
        } else if (data[i] == "-h") {
            args.SHOW_HELP = true;
        } else if (data[i] == "-noanalize") {
            args.NO_DEEP_ANALYZE = true;
        } else if (data[i] == "-types") {
            args.USE_TYPE_ANALYZER = true;
        } else if (data[i] == "-deep" || data[i] == "--strict") {
            args.STRICT_DEEP = true;
        } else if (data[i] == "-bench" || data[i] == "-time") {
            args.BENCHMARK = true;
        } else if (data[i] == "-benchruns") {
            if (i == data.size() - 1) outError("waited the run count next the '-benchruns' flag");
            try { args.BENCH_RUNS = std::stoi(data[i + 1].to_utf8()); }
            catch (...) { outError("invalid run count for '-benchruns'"); }
            if (args.BENCH_RUNS < 1) args.BENCH_RUNS = 1;
            i++;
        } else if (data[i] == "-O1" || data[i] == "-O2" || data[i] == "-O3") {
            args.OPT_LEVEL = data[i];
        }
    }
    return args;
}

void outCompilerArgs(const CompilerArgs& args) noexcept {
    std:: cout << Style("FilePath: ", TerminalStyle::GREEN) << args.FILE_PATH << std:: endl;
    std:: cout << Style("Debug: ", TerminalStyle::GREEN) << args.DEBUG_MODE << std:: endl;
    std:: cout << Style("EmitTokens: ", TerminalStyle::GREEN) << args.SAVE_TOKENS_FROM_FILE << std:: endl;
    std:: cout << Style("EmitPreprocessed: ", TerminalStyle::GREEN) << args.SAVE_PREPROCESSED_FILE << std:: endl;
    std:: cout << Style("OutputFile: ", TerminalStyle::GREEN) << args.OUT_FILE_NAME << std:: endl << std:: endl;
}

void printHelp() noexcept {
    std::cout << Style("Lumen Compiler (lumenc)", TerminalStyle::BRIGHT_GREEN) << "\n";
    std::cout << Style("Usage: ", TerminalStyle::DIM_WHITE) << "lumenc.exe [flags] -f <file.lm>\n\n";
    std::cout << Style("Flags:\n", TerminalStyle::BRIGHT_CYAN);

    auto flag = [](const String& f, const String& desc) {
        std::cout << "  " << Style(f, TerminalStyle::BRIGHT_YELLOW)
                  << Style("  " + desc, TerminalStyle::DIM_WHITE) << "\n";
    };

    flag("-f <path>",       "Input .lm source file (required)");
    flag("-o <path>",       "Output file path (default: out.cpp / out.exe)");
    flag("-build",          "Compile C++ to executable via g++ (requires -f)");
    flag("-c",              "Emit generated C++ code to file (out.cpp or -o path)");
    flag("-r",              "Run interpreter (echo/outln output)");
    flag("-d",              "Show full compiler pipeline diagnostics (timing, stages)");
    flag("-t",              "Save lexer tokens to tokens.txt");
    flag("-p",              "Save preprocessed tokens to pp.lm");
    flag("-tree",           "Print AST tree to stdout");
    flag("-O1 / -O2 / -O3", "Optimization level for g++");
    flag("-noanalize",      "Skip deep analysis stage during compilation");
    flag("-types",          "Use type-only analysis (no code execution) instead of the interpreter");
    flag("-deep/--strict",  "Use the full interpreter during normal builds (default: type-only analysis)");
    flag("-bench",          "Measure execution time: interpreter vs native build (implies -build, -O2)");
    flag("-benchruns <n>",  "Native run count for -bench, keeps the best (default 3)");
    flag("-h",              "Show this help message");

    std::cout << "\n" << Style("Examples:\n", TerminalStyle::BRIGHT_CYAN);
    std::cout << "  lumenc.exe -f main.lm -r\n";
    std::cout << "  lumenc.exe -f main.lm -build -o app.exe\n";
    std::cout << "  lumenc.exe -f main.lm -c -o generated.cpp\n";
    std::cout << "  lumenc.exe -f main.lm -d\n";
}
