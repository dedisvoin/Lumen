"""
LspServer — универсальный клиент любого LSP-сервера (JSON-RPC 2.0 по stdio).

Один процесс сервера на (конфиг, корень воркспейса). Умеет открывать
документы, слать инкрементальные didChange (diff по префиксу/суффиксу),
запросы с очередью до готовности initialize и разбор семантической легенды.
"""

import json
import logging
import os
import subprocess
import threading
from pathlib import Path
from typing import Callable, Dict, List, Optional

_log = logging.getLogger(__name__)

from PyQt6.QtCore import QObject, pyqtSignal

from editor.lsp.config import LspServerConfig
from editor.lsp.protocol import encode, incremental_changes

# Семантические типы/модификаторы, которые умеет красить клиент.
# LSP требует объявить их в client capabilities (textDocument.semanticTokens)
# — старые серверы пропускали, новые (rust-analyzer ≥ 1.9x) строго
# валидируют и падают на initialize, если tokenTypes отсутствует.
try:
    from editor.semantic_lexer import _SEM_TOKEN_MAP
except Exception:  # автономный импорт без полного стека PyQt/Qsci
    _SEM_TOKEN_MAP = {}

_CLIENT_TOKEN_TYPES = sorted(set(_SEM_TOKEN_MAP) | {
    "const", "static", "trait", "field", "bool", "selfKeyword",
    "selfTypeKeyword", "lifetime", "formatSpecifier", "attribute",
    "associatedType", "unknown",
})
_CLIENT_TOKEN_MODIFIERS = [
    "declaration", "definition", "readonly", "static", "deprecated",
    "abstract", "async", "modification", "documentation",
    "defaultLibrary", "macro", "borrowed", "consuming", "mutable",
    "unsafe", "public", "global",
]


class LspServer(QObject):
    """Один процесс LSP-сервера: транспорт, документы, запросы."""

    message = pyqtSignal(dict)          # любое входящее сообщение (главный поток)
    diagnostics = pyqtSignal(str, list)  # uri, [diagnostic, ...]
    process_exited = pyqtSignal()
    ready_changed = pyqtSignal()        # initialize завершён (очистка ошибок)

    def __init__(self, config: LspServerConfig, root_dir: str, parent=None):
        super().__init__(parent)
        self.config = config
        self.root_dir = root_dir
        self.ready = False
        self.token_types: List[str] = []  # легенда semanticTokens сервера
        self.document_symbols = False     # textDocument/documentSymbol
        self.rename_supported = False     # textDocument/rename
        self.pull_diagnostics_supported = False  # textDocument/diagnostic (LSP 3.17)

        args = config.resolve_command(root_dir)
        if args is None:
            raise FileNotFoundError(f"LSP binary not found: {config.command[0]}")
        creationflags = 0
        if os.name == "nt":
            creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        try:
            self._proc = subprocess.Popen(
                args,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                creationflags=creationflags,
            )
        except OSError as exc:
            raise OSError(f"Failed to start {config.server_name or args[0]}: {exc}")

        # Хвост stderr сервера: если процесс упал/не стартует — по нему
        # показываем причину в статус-баре (иконка предупреждения).
        self._stderr_lines: List[str] = []
        self._err_lock = threading.Lock()
        self._err_reader = threading.Thread(target=self._err_loop, daemon=True)
        self._err_reader.start()

        self._next_id = 1  # 0 занят под initialize
        self._pending: Dict[int, Callable[[Optional[dict]], None]] = {}
        self._versions: Dict[str, int] = {}
        self._doc_text: Dict[str, str] = {}
        self._opened: List[str] = []
        self._deferred: List[tuple] = []
        self._deferred_reqs: List[tuple] = []
        self._after_ready: List[Callable[[], None]] = []  # колбэки после initialize

        self.message.connect(self._on_message)
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

        caps = {
            "textDocument": {
                "completion": {
                    "completionItem": {"snippetSupport": False},
                },
                "hover": {"contentFormat": ["plaintext", "markdown"]},
                "definition": {},
                "rename": {},
            }
        }
        if config.pull_diagnostics:
            # LSP 3.17 pull-диагностика: rust-analyzer (в pull-режиме) пишет
            # workspace/diagnostic/refresh вместо publishDiagnostics и ждёт,
            # что клиент сам запросит textDocument/diagnostic. Это лишает
            # клиента варнингов (unused/dead_code) — они приходят только по
            # publishDiagnostics, поэтому rust выключает pull через
            # pull_diagnostics=False.
            caps["textDocument"]["diagnostic"] = {"dynamicRegistration": False}
        if config.semantic_tokens:
            caps["textDocument"]["semanticTokens"] = {
                "requests": {"full": True, "range": False},
                "formats": ["relative"],
                "tokenTypes": _CLIENT_TOKEN_TYPES,
                "tokenModifiers": _CLIENT_TOKEN_MODIFIERS,
            }
        if config.inlay_hints:
            caps["textDocument"]["inlayHint"] = {"dynamicRegistration": False}
        # Дозапись специфичных для сервера возможностей
        for section, data in config.capabilities.items():
            caps.setdefault("textDocument", {}).setdefault(section, {}).update(data)
        self._send({
            "jsonrpc": "2.0",
            "id": 0,
            "method": "initialize",
            "params": {
                "processId": None,
                "rootUri": Path(root_dir).as_uri(),
                "capabilities": caps,
                "initializationOptions": config.initial_options,
            },
        })

    # --- чтение stdout (поток) ---

    def _err_loop(self):
        """Читает stderr сервера и держит хвост последних строк.

        Пайп непрерывно дренируется, чтобы буфер канала не заблокировал
        сервер; сами строки пригодятся, если процесс упадёт при старте.
        """
        stream = self._proc.stderr
        try:
            for line in stream:
                if not line:
                    break
                text = line.decode("utf-8", errors="replace").rstrip("\r\n")
                if not text:
                    continue
                with self._err_lock:
                    self._stderr_lines.append(text)
                    del self._stderr_lines[:-30]
        except Exception as exc:
            _log.debug("stderr read loop ended: %s", exc)

    def stderr_text(self) -> str:
        """Последние строки stderr процесса (для сообщения об ошибке)."""
        with self._err_lock:
            return "\n".join(self._stderr_lines)

    def _read_loop(self):
        stream = self._proc.stdout
        content_length = None
        try:
            while True:
                line = stream.readline()
                if not line:
                    break
                if line in (b"\r\n", b"\n"):
                    if content_length is None:
                        continue
                    body = stream.read(content_length)
                    content_length = None
                    try:
                        msg = json.loads(body.decode("utf-8"))
                    except Exception as exc:
                        _log.warning("LSP: failed to parse JSON-RPC body "
                                     "(%s): %r", exc, body[:200])
                        continue
                    self.message.emit(msg)
                else:
                    try:
                        key, _, value = line.decode("ascii").partition(":")
                        if key.strip().lower() == "content-length":
                            content_length = int(value.strip())
                    except Exception as exc:
                        _log.warning("LSP: bad header line (%s): %r",
                                     exc, line[:80])
                        continue
        finally:
            self.process_exited.emit()

    # --- приём сообщений (главный поток; сигнал message — queued) ---

    def _on_message(self, msg: dict):
        if "method" in msg:
            method = msg["method"]
            if method == "textDocument/publishDiagnostics":
                self.diagnostics.emit(
                    msg["params"]["uri"],
                    msg["params"].get("diagnostics", []),
                )
                return
            req_id = msg.get("id")
            if req_id is not None:
                # Входящий запрос от сервера — без ответа сервер повиснет.
                # workspace/configuration отдаём нашими settings, остальное
                # гасим пустым результатом.
                if method == "workspace/configuration":
                    self._handle_config_request(req_id, msg.get("params") or {})
                else:
                    self._send({"jsonrpc": "2.0", "id": req_id, "result": {}})
                return
            # workspace/diagnostic/refresh — состояние диагностik изменилось,
            # pull-клиент обязан сам запросить актуальные
            if method == "workspace/diagnostic/refresh":
                for uri in list(self._opened):
                    self.pull_diagnostics(uri)
            return
        req_id = msg.get("id")
        # Ответ на initialize (послан в конструкторе до готовности менеджера)
        if req_id == 0:
            result = msg.get("result") or {}
            caps = result.get("capabilities") or {}
            # LSP: легенда живёт в semanticTokensProvider.legend.tokenTypes
            legend = (caps.get("semanticTokensProvider") or {}).get("legend") or {}
            self.token_types = legend.get("tokenTypes") or []
            self.document_symbols = bool(caps.get("documentSymbolProvider"))
            self.rename_supported = bool(caps.get("renameProvider"))
            self.pull_diagnostics_supported = bool(caps.get("diagnosticProvider"))
            # В push-режиме (config.pull_diagnostics=False) сервер после
            # didChange не шлёт refresh/publish — актуальную семантику
            # добываем повторными textDocument/diagnostic (см. did_change);
            # варнинги cargo-check приходят по publishDiagnostics (flycheck).
            self.ready = True
            self.ready_changed.emit()
            self.notify("initialized", {})
            # basedpyright читает настройки (pythonPath, analysis) только из
            # workspace/didChangeConfiguration, initializationOptions — нет
            if self.config.initial_options:
                self.notify("workspace/didChangeConfiguration",
                            {"settings": self.config.initial_options})
            self._flush_deferred()
            if self._after_ready:
                cbs, self._after_ready = self._after_ready, []
                for cb in cbs:
                    cb()
            return
        cb = self._pending.pop(req_id, None)
        if cb is None:
            return
        cb(msg.get("result") if "error" not in msg else None)

    def after_ready(self, callback: Callable[[], None]):
        """Выполнить callback после готовности initialize (сразу, если готов)."""
        if self.ready:
            callback()
        else:
            self._after_ready.append(callback)

    def _flush_deferred(self):
        for method, params in self._deferred:
            self.notify(method, params)
        self._deferred.clear()
        for method, params, callback in self._deferred_reqs:
            self._send_request(method, params, callback)
        self._deferred_reqs.clear()

    # --- отправка ---

    def _send(self, msg: dict):
        try:
            self._proc.stdin.write(encode(msg))
            self._proc.stdin.flush()
        except Exception:
            pass

    def request(self, method: str, params, callback):
        if not self.ready:
            self._deferred_reqs.append((method, params, callback))
            return
        self._send_request(method, params, callback)

    def _send_request(self, method: str, params, callback):
        req_id = self._next_id
        self._next_id += 1
        self._pending[req_id] = callback
        self._send({"jsonrpc": "2.0", "id": req_id,
                    "method": method, "params": params})

    def notify(self, method: str, params):
        self._send({"jsonrpc": "2.0", "method": method, "params": params})

    def _queue(self, method: str, params: dict):
        if self.ready:
            self.notify(method, params)
        else:
            self._deferred.append((method, params))

    # --- документы ---

    def _bump_version(self, uri: str) -> int:
        self._versions[uri] = self._versions.get(uri, 0) + 1
        return self._versions[uri]

    def open_document(self, uri: str, text: str):
        if uri in self._opened:
            self.did_change(uri, text)
            return
        self._opened.append(uri)
        self._doc_text[uri] = text
        self._queue("textDocument/didOpen", {
            "textDocument": {
                "uri": uri,
                "languageId": self.config.language_id,
                "version": self._bump_version(uri),
                "text": text,
            },
        })
        self.pull_diagnostics(uri)

    def did_change(self, uri: str, text: str):
        """Инкрементальная синхронизация: только изменённый диапазон."""
        if uri not in self._opened:
            self.open_document(uri, text)
            return
        old = self._doc_text.get(uri, "")
        changes = incremental_changes(old, text)
        if changes is None:
            changes = [{"text": text}]  # фолбэк: полная пересинхронизация
        self._doc_text[uri] = text
        self._queue("textDocument/didChange", {
            "textDocument": {"uri": uri,
                             "version": self._bump_version(uri)},
            "contentChanges": changes,
        })
        self.pull_diagnostics(uri)

    def did_save(self, uri: str, text: Optional[str] = None):
        """Уведомление о сохранении файла. Включает серверную проверку,
        запускаемую по сохранению (rust-analyzer checkOnSave — cargo check,
        который приносит варнинги unused/dead_code и т.п.)."""
        if uri not in self._opened:
            return
        params = {"textDocument": {"uri": uri}}
        if text is not None:
            params["text"] = text
        self._queue("textDocument/didSave", params)

    def did_close(self, uri: str):
        if uri not in self._opened:
            return
        self._opened.remove(uri)
        self._doc_text.pop(uri, None)
        if self.ready:
            self.notify("textDocument/didClose",
                        {"textDocument": {"uri": uri}})

    # --- pull-диагностика (LSP 3.17) ---
    # Большинство серверов шлёт publishDiagnostics (push); rust-analyzer при
    # объявленном pull-капабилити переходит на workspace/diagnostic/refresh +
    # textDocument/diagnostic и НЕ приносит варнинги cargo-check. Поэтому rust
    # работает в push-режиме, а живые ошибки при наборе добираем pull-ами.

    def pull_diagnostics(self, uri: str):
        """Запросить актуальные диагностики документа (pull-модель)."""
        if not self.pull_diagnostics_supported:
            return
        self.request(
            "textDocument/diagnostic",
            {"textDocument": {"uri": uri}},
            lambda result: self._on_pull_diagnostics(uri, result),
        )

    def _on_pull_diagnostics(self, uri: str, result):
        if not isinstance(result, dict):
            return
        if result.get("kind") in (None, "full"):
            items = list(result.get("items") or [])
            if not items and not self.config.pull_diagnostics:
                # Push-режим (rust): пустой pull — гонка анализа (сервер на
                # didChange сбрасывает семантику и пересчитывает её сам), а
                # не факт очистки. Не стираем видимые ошибки: их убирает
                # только авторитетный publishDiagnostics из flycheck
                # (open/didSave при сохранении на диск). Иначе ошибка
                # «мигает» во время набора.
                return
            self.diagnostics.emit(uri, items)
        for rel_uri, report in (result.get("relatedDocuments") or {}).items():
            if isinstance(report, dict) and report.get("kind") in (None, "full"):
                items = list(report.get("items") or [])
                if not items and not self.config.pull_diagnostics:
                    continue
                self.diagnostics.emit(rel_uri, items)

    def _handle_config_request(self, req_id: int, params: dict):
        """workspace/configuration: сервер спрашивает настройки по секциям.

        initial_options хранит их с префиксом секции ({"rust-analyzer": …}),
        а тут сервер ждёт тело секции без имени — отдаём его как есть.
        """
        opts = self.config.initial_options or {}
        values = []
        for item in params.get("items") or []:
            section = (item.get("section") or "").strip()
            value = opts.get(section) if section else opts
            if isinstance(value, dict):
                values.append(value)
            else:
                values.append({})
        self._send({"jsonrpc": "2.0", "id": req_id, "result": values})

    def shutdown(self):
        try:
            self.request("shutdown", None, lambda _: None)
            self.notify("exit", None)
        except Exception as exc:
            _log.debug("LSP shutdown request failed: %s", exc)
        try:
            self._proc.terminate()
        except Exception as exc:
            _log.debug("LSP terminate failed: %s", exc)
        try:
            self._proc.wait(timeout=2)
        except Exception as exc:
            _log.debug("LSP wait failed: %s", exc)
