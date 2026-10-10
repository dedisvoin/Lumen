"""
Агрегатор языков: собирает все LanguageDef из editor/lexers/<lang>/ в
реестры для ядра редактора.

Ядро (LexerManager, lsp.manager/bridge) не знает про конкретные языки —
оно ходит сюда за language_id -> (лексер, LSP-конфиг). Все пер-языковые
детали спрятаны в папках editor/lexers/<lang>/.

Публичное API (lsp_config, register_lsp_server, set_*, cpp_standard_options)
зеркалит старый editor/lsp/config.py, чтобы остальной код не поменялся;
editor/lsp/config.py теперь просто переэкспортирует отсюда.
"""

import dataclasses
from typing import Dict, List, Optional, Tuple

from editor.lexers.cpp.cpp_def import cpp_def
from editor.lexers.python.python_def import python_def
from editor.lexers.lisp.lisp_def import lisp_def
from editor.lexers.lumen.lumen_def import lumen_def
from editor.lexers.rust.rust_def import rust_def
from editor.lexers.asm.asm_def import asm_def
from editor.lexers.llvm.llvm_def import llvm_def
from editor.lexers.builtins import builtin_defs
from editor.lexers.lsp_types import LspServerConfig
from editor.lexers.cpp.cpp_def import cpp_standard_options


# --- сборка реестров из LanguageDef ---

_DEFS = ([cpp_def(), python_def(), lisp_def(), lumen_def(), rust_def(),
          asm_def(), llvm_def()]
         + builtin_defs())

# расширение -> language_id
EXT_MAP: Dict[str, str] = {}
for _d in _DEFS:
    for _ext in _d.extensions:
        EXT_MAP[_ext] = _d.id

# language_id -> ("builtin"|"custom", класс лексера)
LEXER_SPECS: Dict[str, Tuple[str, type]] = {}
for _d in _DEFS:
    if _d.builtin_lexer is not None:
        LEXER_SPECS[_d.id] = ("builtin", _d.builtin_lexer)
    if _d.custom_lexer is not None:
        LEXER_SPECS[_d.id] = ("custom", _d.custom_lexer)

# language_id -> LspServerConfig (без учёта override/флагов)
_BUILTIN: Dict[str, LspServerConfig] = {
    _d.id: _d.lsp for _d in _DEFS if _d.lsp is not None
}


# --- публичный API (зеркалит старый editor/lsp/config.py) ---

# Пользовательские серверы (register_lsp_server), перекрывают встроенные
_CUSTOM: Dict[str, LspServerConfig] = {}

# Глобальный выключатель LSP (панель настроек: lsp.enabled)
_LSP_ENABLED = True

# Переопределения из панели настроек: language_id -> dict(command, initial_options)
_USER_OVERRIDES: Dict[str, Dict] = {}

# Флаги фич из панели настроек: language_id -> {error_lens, hover, inlay_hints}
_FEATURE_FLAGS: Dict[str, Dict[str, bool]] = {}


def register_lsp_server(language_id: str, config: LspServerConfig):
    """Регистрирует LSP-сервер для языка (перекрывает встроенный)."""
    _CUSTOM[language_id] = config


def set_lsp_enabled(enabled: bool):
    """Панель настроек: lsp.enabled — выключает все LSP-серверы."""
    global _LSP_ENABLED
    _LSP_ENABLED = bool(enabled)


def set_feature_flags(language_id: str, flags: Dict[str, bool]):
    """Пер-языковые тумблеры фич: error_lens / hover / inlay_hints."""
    _FEATURE_FLAGS.setdefault(language_id, {}).update(flags)


def set_user_override(language_id: str, command: Optional[List[str]] = None,
                      initial_options: Optional[dict] = None):
    """Переопределение команды/опций сервера из панели настроек.

    None в обоих аргументах снимает переопределение. Команда заменяет
    первый элемент (путь бинаря) и дополняется новыми аргументами —
    значения уже собраны в settings_apply.
    """
    if command is None and initial_options is None:
        _USER_OVERRIDES.pop(language_id, None)
    else:
        _USER_OVERRIDES[language_id] = {
            "command": command,
            "initial_options": initial_options,
        }


def lsp_config(language_id: str,
              ignore_enabled: bool = False) -> Optional[LspServerConfig]:
    """Конфиг сервера для языка или None, если LSP не подключён.

    Учитывает глобальный выключатель и пользовательские переопределения
    (путь/аргументы из панели настроек) — они мержатся в копию конфига,
    исходные (встроенные/кастомные) не меняются.

    ignore_enabled=True нужен, чтобы прочитать базовый конфиг при
    выключенном LSP (например, для пересборки override-опций в
    settings_apply, которые применяются после повторного включения).
    """
    if not _LSP_ENABLED and not ignore_enabled:
        return None
    cfg = _CUSTOM.get(language_id) or _BUILTIN.get(language_id)
    if cfg is None:
        return None
    override = _USER_OVERRIDES.get(language_id)
    if override:
        command = override.get("command") or cfg.command
        options = override.get("initial_options")
        if options:
            merged = {**cfg.initial_options, **options}
            cfg = dataclasses.replace(cfg, command=command,
                                      initial_options=merged)
        else:
            cfg = dataclasses.replace(cfg, command=command)
    flags = _FEATURE_FLAGS.get(language_id)
    if flags:
        cfg = dataclasses.replace(
            cfg,
            error_lens=bool(flags.get("error_lens", cfg.error_lens)),
            hover=bool(flags.get("hover", cfg.hover)),
            inlay_hints=bool(flags.get("inlay_hints", cfg.inlay_hints)),
        )
    return cfg


# --- помощники для LexerManager ---

def language_for_ext(ext: str) -> Optional[str]:
    """language_id по расширению файла (без точки уже приведён к .ext)."""
    return EXT_MAP.get(ext)


def lexer_spec(language_id: str):
    """('builtin'|'custom', класс лексера) для language_id или None."""
    return LEXER_SPECS.get(language_id)
