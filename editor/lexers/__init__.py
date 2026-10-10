"""
editor/lexers — поддержка языков.

Каждый язык описан в своей папке editor/lexers/<lang>/ (лексер + LSP),
а editor/lexers/registry.py собирает их в реестры для ядра редактора
(LexerManager, lsp.manager/bridge). Ядру достаточно строки language_id.
"""

from editor.lexers.registry import (  # noqa: F401
    lsp_config, register_lsp_server, set_lsp_enabled, set_feature_flags,
    set_user_override, language_for_ext, lexer_spec, cpp_standard_options,
)
from editor.lexers.language_def import LanguageDef  # noqa: F401
from editor.lexers.lsp_types import LspServerConfig  # noqa: F401
