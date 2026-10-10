"""
Единый LSP-слой: любой язык подключается к любому stdio LSP-серверу
через LspServerConfig + register_lsp_server().

Публичный API:
  get_lsp_manager()        — синглтон LspManager
  register_lsp_server()    — подключить новый язык к LSP-серверу
  LspServerConfig          — конфиг сервера (команда, фичи, триггеры)
  path_to_uri / uri_to_path / pos_to_lsp / lsp_to_pos — конвертации
"""

from editor.lsp.config import LspServerConfig, register_lsp_server
from editor.lsp.manager import LspManager, get_lsp_manager
from editor.lsp.protocol import (
    path_to_uri, uri_to_path, pos_to_lsp, lsp_to_pos,
)

__all__ = [
    "LspServerConfig", "register_lsp_server",
    "LspManager", "get_lsp_manager",
    "path_to_uri", "uri_to_path", "pos_to_lsp", "lsp_to_pos",
]
