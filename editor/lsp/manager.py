"""
LspManager — реестр LSP-сессий и документов для всех языков.

Одна сессия (процесс сервера) на (конфиг сервера, корень воркспейса).
Корень ищется по root_markers конфига (compile_commands.json, .git, ...).
Редакторы регистрируются по языку; все запросы (completion/hover/
semanticTokens/inlay/definition) идут через единый интерфейс сессии.
"""

import os
import time
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

from PyQt6.QtCore import QObject
from PyQt6 import sip

from editor.lsp.config import LspServerConfig, lsp_config
from editor.lsp.protocol import (
    hover_text, normalize_completion, normalize_locations, path_to_uri,
    canonical_uri, normalize_symbols, symbols_to_tokens, symbol_names,
    normalize_workspace_edit, normalize_symbol_tree, normalize_signature_help,
)
from editor.lsp.server import LspServer


class LspManager(QObject):
    """Единая точка входа LSP: сессии по корням, документы, запросы."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.workspace_root: Optional[str] = None
        self._sessions: Dict[Tuple[str, str], LspServer] = {}
        self._docs: Dict[str, Tuple[object, LspServer, LspServerConfig]] = {}
        self._by_editor: Dict[int, str] = {}
        self._available: Dict[str, bool] = {}
        # Ошибки запуска LSP: language_id -> текст (для иконки-предупреждения
        # в статус-баре). Очищаются при успешном старте/готовности сервера.
        self._errors: Dict[str, str] = {}
        # Моменты падений сессий (для лимита рестартов: key -> timestamps)
        self._crash_times: Dict[Tuple[str, str], List[float]] = {}

    # --- конфигурация ---

    def config_for(self, language_id: str) -> Optional[LspServerConfig]:
        """Конфиг LSP-сервера для языка (None — язык без LSP)."""
        return lsp_config(language_id)

    def server_available(self, language_id: str) -> bool:
        """Есть ли исполняемый бинарь сервера для языка.

        Успех кэшируется, неудача — нет: если сервер установили или добавили
        в PATH уже после старта редактора, повторная проверка находит его
        без перезапуска LSP.
        """
        config = lsp_config(language_id)
        if config is None:
            return False
        if language_id in self._available:
            return self._available[language_id]
        resolved = (config.resolve_command(self.workspace_root or "")
                    is not None)
        if resolved:
            self._available[language_id] = True
            self._clear_error(language_id)
        else:
            self._available.pop(language_id, None)
            exe = config.command[0] if config.command else "?"
            self._set_error(
                language_id,
                f"{config.server_name or exe} not found ({exe}). "
                f"Install the server or set its path in Settings → LSP.",
            )
        return resolved

    def error_for(self, language_id: str) -> Optional[str]:
        """Ошибка запуска LSP-сервера языка (для статус-бара) или None."""
        return self._errors.get(language_id)

    def _set_error(self, language_id: str, message: str):
        self._errors[language_id] = message

    def _clear_error(self, language_id: str):
        self._errors.pop(language_id, None)

    def set_workspace_root(self, path: Optional[str]):
        self.workspace_root = path
        # Корень влияет на resolve_command ({workspace}) — кэш бинарей
        # мог устареть (раньше сервер_available кэшировал навсегда)
        self._available.clear()

    # --- сессии ---

    def _root_for(self, config: LspServerConfig, file_path: str) -> str:
        p = Path(file_path).resolve()
        if self.workspace_root:
            wr = Path(self.workspace_root).resolve()
            try:
                p.relative_to(wr)
                return str(wr)
            except ValueError:
                pass
        if config.root_markers:
            for d in (p.parent, *p.parents):
                if any((d / marker).is_file() for marker in config.root_markers):
                    return str(d)
        return str(p.parent)

    def _session_for(self, config: LspServerConfig,
                     root: str) -> Optional[LspServer]:
        key = (config.language_id, root)
        session = self._sessions.get(key)
        if session is None:
            try:
                session = LspServer(config, root, self)
            except Exception as exc:
                self._set_error(config.language_id, str(exc))
                return None
            session.diagnostics.connect(self._on_diagnostics)
            session.process_exited.connect(
                lambda: self._on_server_exited(session))
            session.ready_changed.connect(
                lambda: self._clear_error(config.language_id))
            self._sessions[key] = session
        return session

    # --- отказоустойчивость: падение процесса и перезапуск ---

    _RESTART_WINDOW_S = 60.0   # окно подсчёта падений
    _RESTART_MAX_ATTEMPTS = 3  # больше падений в окне — не рестартим

    def _prettify_error(self, language_id: str, message: str) -> str:
        """Переводит сырые stderr-сообщения серверов в понятные подсказки."""
        low = message.lower()
        if language_id == "rust" and "unknown binary" in low:
            return ("rust-analyzer is not installed for the active Rust "
                    "toolchain (rustup proxy reports «Unknown binary»). "
                    "Run: rustup component add rust-analyzer")
        return message

    def _on_server_exited(self, session: LspServer):
        """LSP-процесс упал: отвязываем документы и перезапускаем сессию."""
        key = next((k for k, s in self._sessions.items() if s is session),
                   None)
        if key is None:
            return  # уже убрана (shutdown_all) — не трогаем
        language_id, _root = key
        tail = session.stderr_text().strip()
        name = (session.config.server_name
                or os.path.basename(session.config.command[0]))
        msg = f"{name} exited unexpectedly"
        if tail:
            msg += ":\n" + tail
        self._set_error(language_id, self._prettify_error(language_id, msg))
        self._sessions.pop(key)
        survivors = []
        for uri, (editor, s, _cfg) in list(self._docs.items()):
            if s is not session:
                continue
            self._docs.pop(uri, None)
            self._by_editor.pop(id(editor), None)
            survivors.append((uri, editor))
        self._try_restart(key, survivors)

    def _try_restart(self, key: Tuple[str, str],
                     docs: List[Tuple[str, object]]):
        """Пересоздаёт упавшую сессию и переоткрывает её документы.

        Лимит падений в окне _RESTART_WINDOW_S: если сервер падает на
        старте, не плодим процессы в цикле — LSP-фичи просто отключаются,
        а в статус-баре остаётся иконка-предупреждение с причиной.
        """
        now = time.monotonic()
        crashes = self._crash_times.setdefault(key, [])
        crashes.append(now)
        cutoff = now - self._RESTART_WINDOW_S
        self._crash_times[key] = [t for t in crashes if t > cutoff]
        if len(self._crash_times[key]) > self._RESTART_MAX_ATTEMPTS:
            return  # ошибка уже записана в _on_server_exited

        language_id, root = key
        config = lsp_config(language_id)
        if config is None:
            return
        try:
            session = LspServer(config, root, self)
        except Exception as exc:
            self._set_error(language_id, str(exc))
            return
        session.diagnostics.connect(self._on_diagnostics)
        session.process_exited.connect(
            lambda: self._on_server_exited(session))
        session.ready_changed.connect(
            lambda: self._clear_error(language_id))
        self._sessions[key] = session
        for uri, editor in docs:
            if sip.isdeleted(editor):
                continue
            try:
                self._docs[uri] = (editor, session, config)
                self._by_editor[id(editor)] = uri
                session.open_document(uri, editor.text())
            except RuntimeError:
                pass  # редактор мог быть уничтожен между проверкой и вызовом

    # --- регистрация редакторов ---

    def register_editor(self, editor) -> Optional[LspServer]:
        config = lsp_config(editor.language_id)
        if config is None or not self.server_available(editor.language_id):
            return None
        path = editor.file_path
        if not path:
            return None
        session = self._session_for(config, self._root_for(config, path))
        if session is None:
            return None
        uri = path_to_uri(path)
        self._docs[uri] = (editor, session, config)
        self._by_editor[id(editor)] = uri
        session.open_document(uri, editor.text())
        return session

    def unregister_editor(self, editor):
        uri = self._by_editor.pop(id(editor), None)
        if uri is None:
            return
        entry = self._docs.pop(uri, None)
        if entry is not None:
            entry[1].did_close(uri)

    def shutdown_unused_sessions(self):
        """Завершает сессии без открытых документов (панели пересобраны
        через restore) — иначе живые LSP-процессы копятся в _sessions."""
        used = {entry[1] for entry in self._docs.values()}
        for key, session in list(self._sessions.items()):
            if session not in used:
                session.shutdown()
                self._sessions.pop(key, None)

    def notify_change(self, editor):
        """Синхронизация буфера с сервером (инкрементальный didChange)."""
        uri = self._by_editor.get(id(editor))
        if not uri:
            return
        entry = self._docs.get(uri)
        if entry is not None:
            entry[1].did_change(uri, editor.text())

    def notify_save(self, editor):
        """Уведомление о сохранении (textDocument/didSave): запускает
        серверные проверки по сохранению (cargo check у rust-analyzer)."""
        uri = self._by_editor.get(id(editor))
        if not uri:
            return
        entry = self._docs.get(uri)
        if entry is not None:
            entry[1].did_save(uri, editor.text())

    def _on_diagnostics(self, uri: str, diagnostics: list):
        # uri приходит в формате сервера (pyright: file:///c%3A/...) —
        # приводим к виду наших ключей документов И к формату bridge._uri,
        # иначе lsp_diagnostics отбросит чужой uri
        canonical = canonical_uri(uri)
        entry = self._docs.get(canonical)
        if entry is None:
            return
        editor = entry[0]
        if editor is None or sip.isdeleted(editor):
            return
        try:
            editor.lsp_diagnostics(canonical, diagnostics)
        except RuntimeError:
            pass

    # --- запросы ---

    def _entry(self, editor) -> Optional[Tuple[str, LspServer]]:
        uri = self._by_editor.get(id(editor))
        if not uri:
            return None
        entry = self._docs.get(uri)
        if entry is None:
            return None
        return uri, entry[1]

    def server_ready(self, editor) -> Optional[bool]:
        """Состояние LSP-сессии редактора для статус-бара:
        None — сессии нет, True — initialize завершён,
        False — сервер ещё стартует (крутится спиннер)."""
        entry = self._entry(editor)
        if entry is None:
            return None
        return bool(entry[1].ready)

    def request_completion(self, editor, position: dict, callback):
        entry = self._entry(editor)
        if entry is None:
            callback(None)
            return
        uri, session = entry
        session.request(
            "textDocument/completion",
            {"textDocument": {"uri": uri},
             "position": position,
             "context": {"triggerKind": 1}},
            lambda result: callback(normalize_completion(result)),
        )

    def request_hover(self, editor, position: dict, callback):
        entry = self._entry(editor)
        if entry is None:
            callback(None)
            return
        uri, session = entry
        session.request(
            "textDocument/hover",
            {"textDocument": {"uri": uri}, "position": position},
            lambda result: callback(hover_text(result)),
        )

    def request_resolve(self, editor, prefix_pos: dict, prefix_len: int,
                        label: str, callback):
        """completionItem/resolve: lumen-lsp «дописывает» слово в буфере до
        полного label (по data) и считает hover — документация как при
        наведении мыши на целиком набранное слово."""
        entry = self._entry(editor)
        if entry is None:
            callback(None)
            return
        uri, session = entry
        item = {
            "label": label,
            "data": {"uri": uri, "repStart": prefix_pos,
                     "prefixLen": prefix_len},
        }
        session.request(
            "completionItem/resolve", item,
            lambda result: callback(
                result if isinstance(result, dict) else None),
        )

    def request_signature_help(self, editor, position: dict, callback):
        """textDocument/signatureHelp: сигнатуры вызова под позицией каретки."""
        entry = self._entry(editor)
        if entry is None:
            callback(None)
            return
        uri, session = entry
        session.request(
            "textDocument/signatureHelp",
            {"textDocument": {"uri": uri}, "position": position},
            lambda result: callback(normalize_signature_help(result)),
        )

    def request_semantic_tokens(self, editor, callback):
        """Семантические токены сервера; индексы легенды -> имена типов.

        Серверы без semanticTokens подменяем documentSymbol:
        определения функций/классов превращаем в те же токены — подсветка
        def/class работает без семантической легенды.
        """
        entry = self._entry(editor)
        if entry is None:
            callback([])
            return
        uri, session = entry
        if not session.ready:
            # Запрос ушёл до готовности initialize: после неё повторим —
            # иначе semanticTokens/full уйдёт серверу, который его не заявил,
            # и подсветка не появится до следующей правки текста
            session.after_ready(
                lambda: self.request_semantic_tokens(editor, callback))
            return
        legend = session.token_types
        if session.ready and not legend:
            if session.document_symbols:
                self.request_document_symbols(
                    editor, lambda syms: callback(symbols_to_tokens(syms),
                                                  symbol_names(syms)))
                return
            # Сервер не заявил semanticTokensProvider в initialize —
            # токенов не будет, не дёргаем его.
            callback([])
            return

        def on_result(result):
            data = result.get("data") if isinstance(result, dict) else result or []
            if legend:
                mapped = []
                for i in range(0, len(data) - 4, 5):
                    t = data[i + 3]
                    name = legend[t] if 0 <= t < len(legend) else None
                    if name is None:
                        continue
                    mapped.extend((data[i], data[i + 1], data[i + 2],
                                   name, data[i + 4]))
                data = mapped
            callback(data)

        session.request(
            "textDocument/semanticTokens/full",
            {"textDocument": {"uri": uri}},
            on_result,
        )

    def request_document_symbols(self, editor, callback):
        """Символы документа: (name, kind, line, character) — для подсветки
        определений (функции/классы/методы) серверами без semanticTokens.
        Строки текста нужны normalize_symbols: сервер отдаёт начало
        определения вместо позиции имени."""
        entry = self._entry(editor)
        if entry is None:
            callback([])
            return
        uri, session = entry
        try:
            text = editor.text()
        except Exception:
            text = ""
        lines = text.split("\n") if isinstance(text, str) else None
        session.request(
            "textDocument/documentSymbol",
            {"textDocument": {"uri": uri}},
            lambda result: callback(normalize_symbols(result, lines)),
        )

    def request_symbol_tree(self, editor, callback):
        """Дерево documentSymbol для breadcrumbs (файл -> класс -> метод).

        Поддерживаются оба формата LSP: DocumentSymbol[] с children и
        SymbolInformation[] с containerName (дерево по имени контейнера).
        """
        entry = self._entry(editor)
        if entry is None:
            callback([])
            return
        uri, session = entry
        try:
            text = editor.text()
        except Exception:
            text = ""
        lines = text.split("\n") if isinstance(text, str) else None
        session.request(
            "textDocument/documentSymbol",
            {"textDocument": {"uri": uri}},
            lambda result: callback(normalize_symbol_tree(result, lines)),
        )

    def request_execute_command(self, editor, command: str, arguments: list,
                                callback=None):
        """workspace/executeCommand — специфичные команды сервера.

        Например, cl-lsp.eval-last-expression: выполняет форму перед
        позицией в бэкенде cl-lsp (SBCL/micros) — после этого сервер
        знает символы файла и отвечает на definition/references/hover.
        """
        entry = self._entry(editor)
        if entry is None:
            return
        entry[1].request(
            "workspace/executeCommand",
            {"command": command, "arguments": arguments},
            callback or (lambda result: None),
        )

    def request_inlay_hints(self, editor, callback):
        entry = self._entry(editor)
        if entry is None:
            callback([])
            return
        uri, session = entry
        try:
            text = editor.text()
        except Exception:
            text = ""
        lines = text.split("\n")
        # Диапазон обязателен: basedpyright падает без range
        # («Cannot read properties of undefined (reading 'start')»).
        rng = {"start": {"line": 0, "character": 0},
               "end": {"line": max(len(lines) - 1, 0),
                       "character": len(lines[-1])}}
        session.request(
            "textDocument/inlayHint",
            {"textDocument": {"uri": uri}, "range": rng},
            lambda result: callback(result if isinstance(result, list) else []),
        )

    def request_definition(self, editor, position: dict, callback):
        entry = self._entry(editor)
        if entry is None:
            callback(None)
            return
        uri, session = entry
        session.request(
            "textDocument/definition",
            {"textDocument": {"uri": uri}, "position": position},
            lambda result: callback(normalize_locations(result)),
        )

    def request_rename(self, editor, position: dict, new_name: str, callback):
        """textDocument/rename: сервер вернёт WorkspaceEdit со всеми
        вхождениями символа; callback получает [(uri, [(start, end, text)])]."""
        entry = self._entry(editor)
        if entry is None:
            callback(None)
            return
        uri, session = entry
        session.request(
            "textDocument/rename",
            {"textDocument": {"uri": uri},
             "position": position,
             "newName": new_name},
            lambda result: callback(normalize_workspace_edit(result)),
        )

    # --- завершение ---

    def shutdown_all(self):
        for session in self._sessions.values():
            session.shutdown()
        self._sessions.clear()
        self._docs.clear()
        self._by_editor.clear()
        self._crash_times.clear()
        self._errors.clear()


_lsp_manager: Optional[LspManager] = None


def get_lsp_manager() -> LspManager:
    """Ленивый синглтон: создаётся после старта QApplication."""
    global _lsp_manager
    if _lsp_manager is None:
        _lsp_manager = LspManager()
    return _lsp_manager
