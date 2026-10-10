"""
editor/lsp/config.py — фасад совместимости.

Реальные определения языков и агрегатор теперь живут в editor/lexers/*
(пер-языковые папки + registry.py). Этот модуль только переэкспортирует
публичный API, чтобы остальной код (lsp.manager/bridge/server, настройки)
не изменился.
"""

from editor.lexers.lsp_types import LspServerConfig, _interpreter_path
from editor.lexers.registry import (
    lsp_config, register_lsp_server, set_lsp_enabled, set_feature_flags,
    set_user_override, cpp_standard_options,
)

__all__ = [
    "LspServerConfig", "_interpreter_path", "lsp_config",
    "register_lsp_server", "set_lsp_enabled", "set_feature_flags",
    "set_user_override", "cpp_standard_options",
]
