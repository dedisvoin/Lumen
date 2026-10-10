"""
Rust — описание языка: семантический лексер (SemanticLexer) + LSP (rust-analyzer).

Синтаксис Rust красится легким лексером SemanticLexer (ключевые слова, типы,
числа, строки, комментарии), а глубокие стили (семантические токены) и все
LSP-возможности (диагностики, goto-definition, hover, inlay-hints, rename)
поставляет rust-analyzer — так же, как clangd для C++.

Пер-языковая логика (поиск rust-analyzer, ключевые слова) вынесена сюда из
старой editor/lsp/config.py, чтобы ядро оставалось языко-независимым.
"""

import os
import shutil
from typing import List, Optional, Tuple

from editor.lexers.language_def import LanguageDef
from editor.lexers.lsp_types import LspServerConfig


# Ключевые слова Rust (fallback лексера SemanticLexer, пока нет семантики)
_RUST_KEYWORDS = (
    "as", "async", "await", "break", "const", "continue", "crate", "dyn",
    "else", "enum", "extern", "false", "fn", "for", "if", "impl", "in",
    "let", "loop", "match", "mod", "move", "mut", "pub", "ref", "return",
    "self", "Self", "static", "struct", "super", "trait", "true", "type",
    "unsafe", "use", "where", "while", "move", "union", "macro_rules",
)

# Встроенные примитивные типы Rust (fallback-подсветка типов)
_RUST_TYPES = (
    "bool", "char", "str", "u8", "u16", "u32", "u64", "u128", "usize",
    "i8", "i16", "i32", "i64", "i128", "isize", "f32", "f64",
    "String", "Vec", "Box", "Option", "Result", "Some", "None", "Ok",
    "Err", "HashMap", "VecDeque", "BTreeMap", "BTreeSet", "HashSet",
    "Rc", "Arc", "Cow", "Cell", "RefCell", "Mutex", "RwLock",
    "std", "core", "alloc", "println", "eprintln", "format",
)


def _find_rust_analyzer() -> Optional[str]:
    """Настоящий rust-analyzer (или прокси rustup), пригодный для LSP.

    Сначала ищем реальный бинарь в дистрибутивах rustup
    (~/.rustup/toolchains/<tc>/bin/rust-analyzer.exe): прокси из ~/.cargo/bin
    умеет печатать «Unknown binary … in official toolchain», если компонент
    rust-analyzer не установлен (rustup component add rust-analyzer).
    Затем — PATH, потом типичные пути установки прокси.
    """
    # 1) Реальный бинарь внутри установленных тулчейнов rustup
    rustup_home = (os.environ.get("RUSTUP_HOME")
                   or os.path.join(os.path.expanduser("~"), ".rustup"))
    tools_dir = os.path.join(rustup_home, "toolchains")
    try:
        toolchains = sorted(os.listdir(tools_dir)) if os.path.isdir(tools_dir) else []
    except OSError:
        toolchains = []
    for tc in reversed(toolchains):  # stable > nightly лексикографически
        cand = os.path.join(tools_dir, tc, "bin", "rust-analyzer.exe")
        if os.path.isfile(cand):
            return cand
    # 2) Через PATH (может оказаться прокси rustup — работает, когда
    #    компонент установлен)
    exe = shutil.which("rust-analyzer")
    if exe:
        return exe
    # 3) Типичные пути установки (~/.cargo/bin rustup создаёт прокси)
    home = os.path.expanduser("~")
    _FALLBACKS = (
        os.path.join(home, ".cargo", "bin", "rust-analyzer.exe"),
        os.path.join(home, ".cargo", "bin", "rust-analyzer"),
        r"C:\Users\Ivan\.cargo\bin\rust-analyzer.exe",
    )
    for p in _FALLBACKS:
        if p and os.path.isfile(p):
            return p
    return None


def rust_def() -> LanguageDef:
    """Описание языка Rust для агрегатора editor/lexers/registry.py."""
    return LanguageDef(
        id="rust",
        extensions=[".rs"],
        lsp=LspServerConfig(
            language_id="rust",
            server_name="rust-analyzer",
            command=[_find_rust_analyzer() or "rust-analyzer"],
            root_markers=("Cargo.toml", ".git", "rust-toolchain.toml"),
            trigger_chars={".", ":", "::", "("},
            semantic_tokens=True,
            inlay_hints=True,
            hover=True,
            # Варнинги (unused, dead_code, …) rust-analyzer шлёт только через
            # textDocument/publishDiagnostics: pull-режим (textDocument/
            # diagnostic) не включает результаты проверки. Поэтому для rust
            # pull отключаем — диагностики (ошибки и варнинги) приходят по push.
            pull_diagnostics=False,
            keywords=_RUST_KEYWORDS,
            type_keywords=_RUST_TYPES,
            line_comment="//",
            block_comments=True,
            # Быстрая работа на открытых файлах; канарейка-выключение шумных
            # диагностик dead_code от русского анализатора в один клик в
            # панели настроек (lsp.rust.check_on_save).
            initial_options={
                "rust-analyzer": {
                    "checkOnSave": {"enable": True},
                    "diagnostics": {"enable": True},
                },
            },
        ),
    )
