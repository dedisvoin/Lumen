"""
LspEditorBridge — вся LSP-обвязка одного редактора (для любого языка).

Здесь живут диагностика (маргин, подчёркивания, счётчики), семантическая
подсветка, hover-тултип, переход к определению, Ctrl-подчёркивание токена
и inlay-подсказки. Язык и доступные фичи определяются LspServerConfig.
"""

from typing import Optional, Tuple

import logging
import os
import re
import threading

_log = logging.getLogger("lsp_bridge")

from PyQt6.QtCore import QObject, QTimer, Qt, QEvent, QPointF, pyqtSignal
from PyQt6.QtGui import QCursor, QColor, QFontMetrics
from PyQt6.QtWidgets import QToolTip
from PyQt6.Qsci import QsciScintilla

from editor.lsp.manager import get_lsp_manager
from editor.lsp.protocol import (
    path_to_uri, uri_to_path, pos_to_lsp, lsp_to_pos,
    canonical_uri, apply_text_edits,
)
from editor.text_utils import NUMBER_PATTERN, rgb_int
from editor.semantic_lexer import _SEM_TOKEN_MAP
from editor.theme_utils import ThemeColors
from editor.scintilla_constants import (
    SCI_INDICSETSTYLE, SCI_INDICSETFORE, SCI_SETINDICATORCURRENT,
    SCI_INDICATORFILLRANGE, SCI_INDICATORCLEARRANGE, SCI_INDICSETUNDER,
    SCI_INDICSETALPHA,
    SCI_GETXOFFSET, SCI_GETFIRSTVISIBLELINE, SCI_GETMARGINWIDTHN,
    SCI_POSITIONFROMLINE, SCI_LINEFROMPOSITION, SCI_GETCURRENTPOS,
    SCI_GOTOPOS, SCI_SCROLLCARET, SCI_GETLENGTH,
    SCI_TEXTWIDTH, SCI_GETCHARAT, SCI_GETLINEENDPOSITION,
    SCI_TEXTHEIGHT, SCI_COLOURISE, SCI_GETSTYLEAT,
    SCI_POINTXFROMPOSITION, SCI_POINTYFROMPOSITION,
    SCI_SETCURSOR, SC_CURSORNORMAL, SC_CURSORHAND,
    SCI_SETCARETSTYLE,
    SCI_INSERTTEXT, SCI_DELETERANGE,
)
from editor.widgets.qt_utils import find_ancestor
from editor.widgets.hover_popup import HoverPopup
from editor.widgets.signature_popup import SignaturePopup
from editor.widgets.error_lens import ErrorLensOverlay
from editor.themed_dialogs import input_text

LSP_MARGIN = 2           # маргин 2: клик по нему — текст диагностики
LSP_MARGIN_WIDTH = 16
DEF_INDICATOR = 8        # индикатор-подчёркивание цели перехода
INDIC_STRAIGHTBOX = 8
_DEF_FLASH_MS = 900      # сколько мигает подчёркивание после перехода
_CHANGE_DELAY_MS = 400
_HOVER_DELAY_MS = 450
_HOVER_HIDE_DELAY_MS = 1000  # плашка исчезает не мгновенно, а через секунду
_SEMANTIC_DELAY_MS = 450    # семантические токены после правки — с дебаунсом: lumen-lsp
                            # не тригерится на каждое нажатие, а анализирует один раз за
                            # паузу в наборе (после ухода didChange, 400 мс) — иначе токены
                            # посчитаются по тексту, уже дошедшему до сервера
_SEMANTIC_RETRY_MS = 1200  # пустой ответ токенов — анализ ещё идёт, повторим
_SEMANTIC_MAX_RETRIES = 8  # потолок повторов (~10 с от открытия файла)
_INLAY_DELAY_MS = 650      # inlay-подсказки запрашиваем после didChange
_SIG_DELAY_MS = 250        # задержка запроса сигнатуры после '(' / ','
_LISP_CHECK_DELAY_MS = 1500  # пауза ввода до автопроверки .lisp через SBCL



class LspEditorBridge(QObject):
    """Контроллер LSP-функций одного редактора (по конфигу языка)."""

    _lisp_done = pyqtSignal(int, list)  # (req, items) фоновой SBCL-проверки
    document_synced = pyqtSignal()      # буфер синхронизирован с сервером (didChange ушёл)

    def __init__(self, editor):
        super().__init__(editor)
        self.editor = editor
        self.attached = False
        self.error_count = 0
        self.warning_count = 0

        self._session = None             # LspServer
        self._config = None              # LspServerConfig
        self._uri: Optional[str] = None
        self._diags = []                 # (line, start, end, severity, message)
        self._line_msgs = {}             # line -> [(severity, message)]
        self._hover_pos = 0
        self._theme_name: Optional[str] = None
        self._hover_popup: Optional[HoverPopup] = None
        self._flash_editor = None
        self._flash_range = (0, 0)
        self._ctrl_range: Optional[Tuple[int, int]] = None
        self._hover_lit_range: Optional[Tuple[int, int]] = None
        self._semantic_active = False
        self._semantic_data = []
        self._semantic_prev_len = 0
        self._semantic_retry = 0   # повторы пустых ответов (открытие файла)
        self._semantic_req_edit_count = 0  # правок на момент отправки запроса
        self._inlay_hints = []   # [(байтовая позиция, текст подсказки)]
        self._inlay_prev_len = 0  # длина документа на момент свежих хинтов
        self._inlay_req = 0      # инвалидация устаревших ответов
        self._hover_req = 0      # инвалидация устаревших hover-ответов
        self._sig_popup: Optional[SignaturePopup] = None
        self._sig_req = 0        # инвалидация устаревших ответов сигнатуры
        self._sig_signatures = []
        self._sig_active = 0
        self._sig_param = 0
        self._sig_open_pos = -1   # позиция открывающей скобки вызова
        # ErrorLens: inline-сообщения диагностик в конце строки кода
        self._overlay = ErrorLensOverlay(editor)
        self._last_caret_style = self._overlay._caret_style  # кэш: избегаем SCI_SETCARETSTYLE на каждый paint

        ed = self.editor
        # Маргин диагностики совмещён с маргином номеров строк (индекс 2):
        # клик по нему — тултип с текстом ошибки. Тип/ширину не меняем,
        # чтобы не сломать номера строк; только делаем кликабельным.
        ed.setMarginSensitivity(LSP_MARGIN, True)
        ed.marginClicked.connect(self.on_margin_clicked)
        # Индикатор-подчёркивание цели перехода к определению
        ed.SendScintilla(SCI_INDICSETSTYLE, DEF_INDICATOR, INDIC_STRAIGHTBOX)
        ed.SendScintilla(SCI_INDICSETUNDER, DEF_INDICATOR, 1)
        ed.SendScintilla(SCI_INDICSETALPHA, DEF_INDICATOR, 30)
        ed.SendScintilla(SCI_INDICSETFORE, DEF_INDICATOR, 0xB4BEFE)

        # Отложенная отправка правок в сервер
        self._change_timer = QTimer(self)
        self._change_timer.setSingleShot(True)
        self._change_timer.timeout.connect(self._notify_change)
        # Hover-тултип (signature/тип под курсором)
        self._hover_timer = QTimer(self)
        self._hover_timer.setSingleShot(True)
        self._hover_timer.timeout.connect(self._hover_request)
        # Прячет плашку с задержкой: мышь ушла далеко, но если вернулась
        # на токен/плашку в течение секунды — плашка остаётся
        self._hover_hide_timer = QTimer(self)
        self._hover_hide_timer.setSingleShot(True)
        self._hover_hide_timer.setInterval(_HOVER_HIDE_DELAY_MS)
        self._hover_hide_timer.timeout.connect(self._hover_hide_now)
        # Семантическая подсветка: запрос токенов после правок
        self._semantic_timer = QTimer(self)
        self._semantic_timer.setSingleShot(True)
        self._semantic_timer.timeout.connect(self._semantic_request)
        # Inlay-подсказки типов (auto a: int = 10; как в VS Code)
        self._inlay_timer = QTimer(self)
        self._inlay_timer.setSingleShot(True)
        self._inlay_timer.timeout.connect(self._inlay_request)
        # Мигание подчёркивания цели перехода
        self._def_flash_timer = QTimer(self)
        self._def_flash_timer.setSingleShot(True)
        self._def_flash_timer.timeout.connect(self.clear_definition_flash)
        # Сигнатура вызова: запрос после '(' / ',' и при движении каретки
        self._sig_timer = QTimer(self)
        self._sig_timer.setSingleShot(True)
        self._sig_timer.timeout.connect(self._sig_request)
        # Автопроверка Common Lisp: SBCL после открытия и паузы в наборе
        self._lisp_check_timer = QTimer(self)
        self._lisp_check_timer.setSingleShot(True)
        self._lisp_check_timer.timeout.connect(self._lisp_check_auto)
        self._lisp_check_req = 0  # инвалидация устаревших результатов
        self._lisp_done.connect(self._apply_lisp_check)
        try:
            ed.cursorPositionChanged.connect(self._on_cursor_moved)
        except RuntimeError:
            pass  # редактор мог быть уничтожен

        ed.destroyed.connect(self.detach)

    # --- подключение / отключение ---

    def attach(self):
        """Подключает редактор к LSP-серверу его языка (если настроен)."""
        ed = self.editor
        manager = get_lsp_manager()
        config = manager.config_for(ed.language_id)
        if config is None or not ed.file_path:
            self.detach()
            return
        # Автопроверка .lisp через SBCL не зависит от cl-lsp — планируем её
        # и при отсутствии LSP-сервера (инлайн-ошибки даёт именно она)
        self._schedule_lisp_check()
        if not manager.server_available(ed.language_id):
            self.attached = False
            return
        self._config = config
        self._session = manager.register_editor(ed)
        self._uri = path_to_uri(ed.file_path)
        self.attached = self._session is not None
        if self.attached:
            self._inlay_prev_len = self.editor.SendScintilla(SCI_GETLENGTH)
            if self._semantic_active:
                self._semantic_retry = 0
                self._semantic_timer.start(_SEMANTIC_DELAY_MS)
            if self._config.inlay_hints:
                self._inlay_timer.start(_INLAY_DELAY_MS)

    def detach(self):
        """Отключает редактор от LSP (смена языка, закрытие вкладки)."""
        self.hide_popup()
        self._clear_ctrl_underline()
        self._clear_hover_literal()
        self._lisp_check_timer.stop()
        self._lisp_check_req += 1
        try:
            self.clear_definition_flash()
        except RuntimeError:
            pass
        if self.attached and self._session is not None:
            try:
                get_lsp_manager().unregister_editor(self.editor)
            except RuntimeError:
                pass
        self._session = None
        self._uri = None
        self.attached = False
        self._clear_diagnostics()
        self._inlay_req += 1
        self._hover_req += 1
        self._sig_req += 1
        self._hide_sig_popup()
        try:
            self._overlay.set_inlay_hints([])
        except RuntimeError:
            pass

    def set_language(self, language_id):
        """Смена языка: включает/выключает семантику и переподключает LSP."""
        self.hide_popup()
        self._sig_req += 1
        self._hide_sig_popup()
        config = get_lsp_manager().config_for(language_id)
        self._semantic_active = bool(config and config.semantic_tokens)
        # Длина документа на момент свежих токенов (база для delta правок)
        self._semantic_prev_len = self.editor.SendScintilla(SCI_GETLENGTH)
        if self._semantic_active:
            self._semantic_data = []
        self.attach()

    def _notify_change(self):
        if self.attached and self._session is not None:
            get_lsp_manager().notify_change(self.editor)
            # Буфер теперь актуален на сервере — можно запрашивать данные,
            # зависящие от текста (documentSymbol для хлебных крошек и т.п.),
            # иначе сервер вернёт позиции по устаревшему документу
            self.document_synced.emit()

    def notify_save(self):
        """Файл сохранён: шлём didSave, чтобы сервер запустил проверки
        по сохранению (rust-analyzer checkOnSave — cargo check, приносящий
        варнинги unused/dead_code)."""
        if self.attached and self._session is not None:
            get_lsp_manager().notify_save(self.editor)

    # --- Common Lisp: загрузка файла в бэкенд cl-lsp ---

    def lisp_load_file(self) -> bool:
        """Common Lisp: компилирует и загружает текущий файл в бэкенд cl-lsp.

        cl-lsp отвечает на definition/references/hover из отдельного SBCL
        (micros/Swank), где известны только загруженные символы — без
        загрузки файла сервер возвращает пустые ответы для символов
        пользователя. Форма `(load #P"...")` временно добавляется в конец
        буфера и выполняется командой cl-lsp.eval-last-expression, после
        чего буфер возвращается к исходному тексту.
        """
        ed = self.editor
        if not self.attached or self._session is None:
            return False
        if getattr(ed, "language_id", None) != "lisp":
            return False
        path = ed.file_path
        if not path:
            return False
        lisp_path = path.replace("\\", "/").replace('"', '\\"')
        form = f'(load #P"{lisp_path}")'
        try:
            ed.objectName()
            length = ed.SendScintilla(SCI_GETLENGTH)
            ed.SendScintilla(SCI_INSERTTEXT, length, form.encode("utf-8"))
        except RuntimeError:
            return False
        # didChange с добавленной формой отправляем сразу (не дожидаясь
        # дебаунс-таймера on_text_changed): executeCommand ниже должен
        # увидеть форму в документе сервера
        get_lsp_manager().notify_change(self.editor)
        form_end = length + len(form)
        get_lsp_manager().request_execute_command(
            ed, "cl-lsp.eval-last-expression",
            [{"textDocument": {"uri": self._uri},
              "position": pos_to_lsp(ed, form_end)}])
        ed.SendScintilla(SCI_DELETERANGE, length, len(form))
        get_lsp_manager().notify_change(self.editor)
        return True

    def lisp_check_file(self) -> bool:
        """Common Lisp: проверка текущего буфера через SBCL (Ctrl+Shift+L).

        cl-lsp не шлёт publishDiagnostics, поэтому проверяем файл сами:
        буфер пишется во временный файл и eval-ится по top-level формам
        (sbcl_checker.check_text), результат отдаётся в общий рендер
        диагностик (маргин, подчёркивания, ErrorLens, счётчики) — тот же,
        что и для publishDiagnostics. Работает независимо от подключения
        cl-lsp. Буфер может отличаться от файла на диске — проверяется
        то, что видит пользователь.
        """
        ed = self.editor
        if getattr(ed, "language_id", None) != "lisp":
            return False
        path = ed.file_path
        if not path:
            return False
        # Ручная проверка отменяет незавершённую автопроверку — результат
        # не должен прийти позже и перезатереть свежую диагностику
        self._lisp_check_req += 1
        self._lisp_check_timer.stop()
        from editor.lsp.sbcl_checker import check_text
        items = check_text(ed.text(), path)
        self._apply_diagnostics([self._lisp_item_to_diag(*it) for it in items])
        return True

    @staticmethod
    def _lisp_item_to_diag(line, col, severity, message) -> dict:
        """(line, col, severity, message) sbcl_checker -> LSP-диагностика."""
        return {
            "severity": 1 if severity == "error" else 2,
            "message": message,
            "range": {
                "start": {"line": line - 1, "character": col},
                "end": {"line": line - 1, "character": col},
            },
        }

    def _schedule_lisp_check(self):
        """Запускает таймер автопроверки .lisp через SBCL (язык + путь)."""
        ed = self.editor
        if getattr(ed, "language_id", None) == "lisp" and ed.file_path:
            self._lisp_check_timer.start(_LISP_CHECK_DELAY_MS)

    def _lisp_check_auto(self):
        """Автопроверка .lisp через SBCL (открытие файла + пауза в наборе).

        Проверка в фоне: SBCL стартует секунды, держать UI в блокировке
        на каждый ввод нельзя. Снимок текста берётся на момент запуска,
        результат применяется в главном потоке по сигналу _lisp_done.
        """
        ed = self.editor
        if getattr(ed, "language_id", None) != "lisp":
            return
        path = ed.file_path
        if not path:
            return
        req = self._lisp_check_req + 1
        self._lisp_check_req = req
        text = ed.text()
        thread = threading.Thread(
            target=self._lisp_check_worker, args=(path, text, req), daemon=True)
        thread.start()

    def _lisp_check_worker(self, path, text, req):
        from editor.lsp.sbcl_checker import check_text
        items = check_text(text, path)
        try:
            self._lisp_done.emit(req, items)
        except RuntimeError:
            pass  # редактор/бридж уничтожены во время проверки

    def _apply_lisp_check(self, req, items):
        """Результат фоновой SBCL-проверки -> общий рендер диагностик."""
        if req != self._lisp_check_req:
            return  # текст менялся после запуска — результат устарел
        ed = self.editor
        try:
            ed.objectName()
        except RuntimeError:
            return
        self._apply_diagnostics([self._lisp_item_to_diag(*it) for it in items])

    # --- события виджета (делегируются из CodeEditor) ---

    def on_key_press(self, event) -> bool:
        self._hover_timer.stop()
        self.hide_popup()
        # Сигнатура видна: стрелки переключают перегрузку, каретку не двигаем
        if (event.key() in (Qt.Key.Key_Up, Qt.Key.Key_Down)
                and self._sig_popup is not None
                and self._sig_popup.isVisible()):
            self._sig_cycle(1 if event.key() == Qt.Key.Key_Down else -1)
            return True
        # Зажали Ctrl с мышью над редактором — подчёркиваем токен
        if event.key() == Qt.Key.Key_Control:
            self._update_ctrl_underline_from_mouse()
        return False

    def on_key_release(self, event):
        # Отпустили Ctrl — снимаем подчёркивание токена
        if event.key() == Qt.Key.Key_Control:
            self._clear_ctrl_underline()
            # Мышь над редактором — показываем hover-подсветку литерала
            ed = self.editor
            if self.attached and ed.viewport().underMouse():
                view_pos = ed.viewport().mapFromGlobal(QCursor.pos())
                if not self._mouse_in_margin(view_pos):
                    self._set_hover_literal(self._pos_from_viewport(view_pos))

    def on_focus_out(self):
        self._clear_ctrl_underline()
        self._clear_hover_literal()
        self._sig_req += 1
        self._hide_sig_popup()

    def on_text_changed(self):
        self._hover_timer.stop()
        self.clear_definition_flash()
        self.hide_popup()
        self._sig_req += 1   # ответ сигнатуры устарел; каретка уточнит сама
        # Отложенная синхронизация буфера с сервером (инкрементальная)
        if self.attached:
            self._change_timer.start(_CHANGE_DELAY_MS)
        # Common Lisp: автопроверка SBCL после паузы в наборе — инлайн-
        # ошибки даёт именно она, cl-lsp publishDiagnostics не шлёт
        if (self.editor.language_id == "lisp"
                and self.editor.file_path is not None):
            self._lisp_check_req += 1  # результат незавершённой проверки устарел
            self._lisp_check_timer.start(_LISP_CHECK_DELAY_MS)
        # Семантическая подсветка пересчитается после анализа сервера;
        # старые токены пока сдвигаем по месту правки
        if self._semantic_active:
            lexer = self.editor.current_lexer
            if lexer is not None and hasattr(lexer, "add_edit"):
                new_len = self.editor.SendScintilla(SCI_GETLENGTH)
                delta = new_len - self._semantic_prev_len
                self._semantic_prev_len = new_len
                caret = self.editor.SendScintilla(SCI_GETCURRENTPOS)
                edit_pos = caret - max(delta, 0)
                lexer.set_spans_stale(True)
                lexer.add_edit(edit_pos, delta)
                if _log.isEnabledFor(logging.DEBUG):
                    _log.debug("on_text_changed: edit_pos=%d delta=%d "
                               "caret=%d new_len=%d", edit_pos, delta,
                               caret, new_len)
            self._semantic_retry = 0
            self._semantic_timer.start(_SEMANTIC_DELAY_MS)
        # Inlay-подсказки: сдвигаем по месту правки — не исчезают при
        # наборе, свежие придут после анализа сервера
        self._inlay_req += 1
        try:
            new_len = self.editor.SendScintilla(SCI_GETLENGTH)
            delta = new_len - self._inlay_prev_len
            self._inlay_prev_len = new_len
            caret = self.editor.SendScintilla(SCI_GETCURRENTPOS)
            self._overlay.shift_inlay_hints(caret - max(delta, 0), delta)
        except RuntimeError:
            pass
        if self.attached and self._config and self._config.inlay_hints:
            self._inlay_timer.start(_INLAY_DELAY_MS)

    def on_mouse_press(self, event) -> bool:
        """Ctrl+клик — переход к определению. True — событие обработано."""
        self._hover_timer.stop()
        self.hide_popup()
        self._clear_ctrl_underline()
        self._clear_hover_literal()
        if (event.button() == Qt.MouseButton.LeftButton
                and event.modifiers() & Qt.KeyboardModifier.ControlModifier
                and self.attached):
            self.goto_definition_at(self._pos_from_viewport(event.position()))
            return True
        return False

    def adjust_click_for_inlay(self, event):
        """Позиция клика с x, скомпенсированным для строк со сдвигом inlay.

        Возвращает QPointF, если сдвиг есть (строку визуально сдвинули
        подсказки), иначе None — событие можно передавать Scintilla как есть.
        """
        ed = self.editor
        try:
            if not self._overlay._inlay_hints:
                return None
            pos = event.position()
            line_h = ed.SendScintilla(SCI_TEXTHEIGHT, 0)
            if line_h <= 0:
                return None
            first = ed.SendScintilla(SCI_GETFIRSTVISIBLELINE)
            line = first + int(pos.y() / line_h)
            x = int(pos.x())
            adj = self._overlay.adjust_click_x(line, x)
            if adj == x:
                return None
            return QPointF(float(adj), pos.y())
        except RuntimeError:
            return None

    def on_viewport_event(self, event):
        """MouseMove/Leave/Wheel по viewport: hover-таймер и Ctrl-подчёркивание."""
        etype = event.type()
        if etype == QEvent.Type.MouseMove and not event.buttons():
            view_pos = event.position()
            # Мышь над маргинами (закладки/номера строк) — токен под ней
            # не подсвечиваем: _pos_from_viewport для x в маргине даёт начало
            # строки, и подчёркивался бы первый токен, хотя курсор не на нём
            if self._mouse_in_margin(view_pos):
                self._hover_timer.stop()
                self._clear_ctrl_underline()
                self._clear_hover_literal()
                popup = self._hover_popup
                if popup is not None and popup.isVisible():
                    self._schedule_hide()
                return
            pos = self._pos_from_viewport(view_pos)
            ctrl = (event.modifiers()
                    & Qt.KeyboardModifier.ControlModifier)
            if ctrl and self.attached:
                # Ctrl+наведение — только подчёркивание токена,
                # hover-попап при этом не показываем
                self._hover_timer.stop()
                popup = self._hover_popup
                if popup is not None and popup.isVisible():
                    self._schedule_hide()
                self._clear_hover_literal()
                self._set_ctrl_underline(pos)
                return
            self._clear_ctrl_underline()
            # Подсветка токена — визуальная фича, LSP не нужен:
            # работает и в файлах без сервера (.lm и т.п.)
            self._set_hover_literal(pos)
            # Мышь ушла с токена (пробел/конец строки/пустая зона) —
            # отменяем запрос, запущенный, пока мышь была на токене:
            # иначе он сработает на позицию конца документа, и сервер
            # ответит «токеном рядом» (pyright: hover последнего вызова)
            if self._hover_lit_range is None:
                self._hover_timer.stop()
            if not self.attached:
                return
            popup = self._hover_popup
            if not (pos == self._hover_pos
                    and popup is not None and popup.isVisible()):
                if popup is not None and popup.isVisible():
                    # Попап прячем не сразу: пока мышь «идёт» на плашку
                    # прокрутить её, не исчезаем
                    if popup.near_cursor():
                        self._cancel_hide()
                    else:
                        self._schedule_hide()
                self._hover_pos = pos
                # Мышь ушла на новый токен — ответы прежних запросов
                # больше не применяем (см. _on_hover)
                self._hover_req += 1
                # LSP-запрос — только когда под мышью реально есть токен:
                # на пробеле, в конце строки и в пустой зоне сервер
                # отвечает «токеном рядом» (pyright на конце строки с
                # вызовом возвращает hover функции) — фантомный попап
                if self._hover_lit_range is not None and (
                        self._config and self._config.hover):
                    self._hover_timer.start(_HOVER_DELAY_MS)
            elif popup is not None and popup.isVisible():
                # Тот же токен — попап не прячем,
                # снимаем отложенное скрытие, если оно висит
                if popup.under_mouse() or popup.near_cursor():
                    self._cancel_hide()
        elif etype in (QEvent.Type.Leave, QEvent.Type.Wheel):
            self._hover_timer.stop()
            self._clear_ctrl_underline()
            self._clear_hover_literal()
            popup = self._hover_popup
            if popup is not None and popup.isVisible():
                if popup.under_mouse():
                    self._cancel_hide()
                else:
                    # Уходят с плашки не мгновенно: интервал даёт шанс
                    # вернуться (прокрутить колёсиком и т.п.)
                    self._schedule_hide()

    # --- диагностика ---

    def lsp_diagnostics(self, uri: str, diagnostics: list):
        """Диагностика сервера: маркеры в маргине, подчёркивание, счётчики."""
        if uri != self._uri:
            return
        self._apply_diagnostics(diagnostics)

    def _apply_diagnostics(self, diagnostics: list):
        """Применяет список LSP-диагностик к редактору (общий рендер).

        Используется и серверными publishDiagnostics, и локальной
        SBCL-проверкой Common Lisp (lisp_check_file) — позиции,
        маргин, ErrorLens одни и те же.
        """
        self._clear_diagnostics()
        ed = self.editor
        doc_len = ed.SendScintilla(SCI_GETLENGTH)
        errs = warns = 0
        line_msgs: dict = {}
        ranges = []
        for diag in diagnostics:
            severity = diag.get("severity", 1)
            rng = diag.get("range", {})
            start = lsp_to_pos(ed, rng.get("start", {"line": 0, "character": 0}))
            end = lsp_to_pos(ed, rng.get("end", rng.get("start", {"line": 0, "character": 0})))
            end = min(max(end, start), doc_len)
            if start >= doc_len:
                continue
            line = ed.SendScintilla(SCI_LINEFROMPOSITION, start)
            message = diag.get("message", "")
            if severity <= 1:
                errs += 1
            else:
                warns += 1
            line_msgs.setdefault(line, []).append((severity, message))
            ranges.append((line, start, end, severity, message))
        self._diags = ranges
        self._line_msgs = line_msgs
        self.error_count = errs
        self.warning_count = warns
        # Подчёркивание позиций ошибок рисуем всегда — это отдельная фича
        # от ErrorLens (инлайн-текст в конце строки).
        self._overlay.set_diag_ranges(ranges)
        # ErrorLens выключен в настройках — инлайн-сообщения не показываем,
        # счётчики в статус-баре и подчёркивания остаются.
        if not (self._config and self._config.error_lens):
            return
        self._overlay.set_messages(line_msgs)

    def _clear_diagnostics(self):
        try:
            self._overlay.clear()
        except RuntimeError:
            pass  # редактор уничтожен (destroyed-сигнал)
        self._diags = []
        self._line_msgs = {}
        self.error_count = 0
        self.warning_count = 0

    def on_margin_clicked(self, margin: int, line: int, modifiers=None):
        """Клик по маргину диагностики — текст ошибки/предупреждения."""
        if margin != LSP_MARGIN:
            return
        msgs = self._line_msgs.get(line)
        if not msgs:
            return
        text = "\n".join(
            f"{'Error' if s <= 1 else 'Warning'}: {m}" for s, m in msgs
        )
        QToolTip.showText(QCursor.pos(), text, self.editor)

    # --- семантическая подсветка (токены сервера вместо лексера) ---

    def _semantic_request(self):
        """Запрос семантических токенов у сервера (с дебаунсом).

        Запрос идёт с _SEMANTIC_DELAY_MS после последней правки, поэтому
        буфер должен быть синхронизирован с сервером (didChange): иначе
        токены посчитаются по старому тексту и не совпадут с текущими
        позициями в редакторе.
        """
        if not self.attached or not self._semantic_active:
            return
        if self._change_timer.isActive():
            # didChange ещё отложен (его дебаунс) — отправляем сейчас,
            # иначе ответ придёт по устаревшему буферу
            self._change_timer.stop()
            self._notify_change()
        # Снимок накопленных правок на момент запроса: ответ уже посчитан по
        # этому тексту, поэтому такие правки при применении сдвигать НЕ нужно.
        lexer = self.editor.current_lexer
        self._semantic_req_edit_count = (
            len(lexer._pending_edits) if (lexer is not None
                                          and hasattr(lexer, "_pending_edits"))
            else 0)
        get_lsp_manager().request_semantic_tokens(self.editor,
                                                  self._semantic_on_tokens)

    def _semantic_on_tokens(self, data, names=None):
        """Относительные токены сервера -> span'ы стилей лексера.

        names (documentSymbol-путь) — имена символов для подсветки вхождений
        (использование класса/функции), не только определений."""
        if not self._semantic_active:
            return
        if not data:
            # Пустой ответ — сервер ещё анализирует (первое открытие файла,
            # холодный старт): повторим запрос, иначе подсветка не появится
            # до следующей правки текста
            if self.attached and self._semantic_retry < _SEMANTIC_MAX_RETRIES:
                self._semantic_retry += 1
                self._semantic_timer.start(_SEMANTIC_RETRY_MS)
            return
        self._semantic_retry = 0
        ed = self.editor
        try:
            ed.objectName()
        except RuntimeError:
            return
        spans = []
        line = start = 0
        for i in range(0, len(data) - 4, 5):
            delta_line = data[i]
            delta_start = data[i + 1]
            length = data[i + 2]
            mapping = _SEM_TOKEN_MAP.get(data[i + 3])
            line += delta_line
            start = delta_start if delta_line else start + delta_start
            if mapping is None:
                continue
            s = lsp_to_pos(ed, {"line": line, "character": start})
            e = lsp_to_pos(ed, {"line": line, "character": start + length})
            if e <= s:
                continue
            spans.append((s, e - s, mapping[0]))
        self._semantic_data = spans
        # DEBUG: log raw server data and resulting spans
        if _log.isEnabledFor(logging.DEBUG):
            _log.debug("semantic_on_tokens: raw_data=%s", data[:60])
            _log.debug("semantic_on_tokens: spans=%s", spans[:30])
        lexer = ed.current_lexer
        if lexer is not None and hasattr(lexer, "set_spans"):
            if names is not None and hasattr(lexer, "set_symbol_names"):
                lexer.set_symbol_names(names)
            self._semantic_prev_len = ed.SendScintilla(SCI_GETLENGTH)
            # Diagnostic: log what clangd sent and how it maps to spans
            if _log.isEnabledFor(logging.DEBUG):
                first20 = spans[:20] if spans else []
                _log.debug("semantic tokens: %d raw data -> %d spans, "
                           "first20=%s", len(data), len(spans), first20)
            # Fresh-токены уже посчитаны по тексту, отправленному didChange
            # перед запросом: правки, накопленные ДО отправки запроса, учтены
            # сервером, и вторично сдвигать их нельзя (иначе при вводе в начале
            # файла подсветка «плывёт»/остаётся на старых позициях). Для
            # stale-сдвига оставляем только правки, пришедшие ПОСЛЕ запроса.
            if hasattr(lexer, "_pending_edits"):
                base = min(len(lexer._pending_edits),
                           self._semantic_req_edit_count)
                del lexer._pending_edits[:base]
                if _log.isEnabledFor(logging.DEBUG):
                    _log.debug("semantic_on_tokens: dropped %d pre-request "
                               "edits, kept %d", base,
                               len(lexer._pending_edits))
            lexer.set_spans_keep_stale(spans)
            # Diagnostic: check # style BEFORE and AFTER SCI_COLOURISE
            if _log.isEnabledFor(logging.DEBUG):
                _pre_hash = []
                for _li in range(min(5, ed.SendScintilla(SCI_LINEFROMPOSITION,
                              ed.SendScintilla(SCI_GETLENGTH)) + 1)):
                    _ls = ed.SendScintilla(SCI_POSITIONFROMLINE, _li)
                    _ln = ed.SendScintilla(SCI_GETLINEENDPOSITION, _li)
                    if _ln <= _ls:
                        continue
                    _raw = ed.text(_li)
                    _stripped = _raw.lstrip()
                    if _stripped and _stripped[0] == "#":
                        _s = ed.SendScintilla(SCI_GETSTYLEAT, _ls)
                        _pre_hash.append("L%d@%d:style=%d" % (
                            _li, _ls, _s))
                if _pre_hash:
                    _log.debug("COLOURISE-BEFORE #-lines: %s", _pre_hash)
            ed.SendScintilla(SCI_COLOURISE, 0, -1)
            # Now that SCI_COLOURISE has applied the spans via styleText(),
            # clear stale state. Any edits that arrived between set_spans_keep_stale
            # and SCI_COLOURISE were handled by stale-shift in _collect().
            lexer.clear_stale()
            if _log.isEnabledFor(logging.DEBUG):
                _post_hash = []
                for _li in range(min(5, ed.SendScintilla(SCI_LINEFROMPOSITION,
                              ed.SendScintilla(SCI_GETLENGTH)) + 1)):
                    _ls = ed.SendScintilla(SCI_POSITIONFROMLINE, _li)
                    _ln = ed.SendScintilla(SCI_GETLINEENDPOSITION, _li)
                    if _ln <= _ls:
                        continue
                    _raw = ed.text(_li)
                    _stripped = _raw.lstrip()
                    if _stripped and _stripped[0] == "#":
                        _s = ed.SendScintilla(SCI_GETSTYLEAT, _ls)
                        _post_hash.append("L%d@%d:style=%d" % (
                            _li, _ls, _s))
                if _post_hash:
                    _log.debug("COLOURISE-AFTER #-lines: %s", _post_hash)
            ed.update()

    # --- inlay-подсказки типов (auto a: int = 10;) ---

    def _inlay_request(self):
        """Запрос textDocument/inlayHint у сервера."""
        if not self.attached or not (self._config and self._config.inlay_hints):
            return
        req_id = self._inlay_req + 1
        self._inlay_req = req_id
        get_lsp_manager().request_inlay_hints(
            self.editor,
            lambda hints: self._on_inlay_hints(req_id, hints),
        )

    def _on_inlay_hints(self, req_id, hints):
        """Ответ сервера: (позиция, текст) для отрисовки в ErrorLensOverlay."""
        if req_id != self._inlay_req:
            return  # документ менялся — ответ устарел
        ed = self.editor
        try:
            ed.objectName()
        except RuntimeError:
            return
        doc_len = ed.SendScintilla(SCI_GETLENGTH)
        out = []
        for hint in hints or []:
            if not isinstance(hint, dict):
                continue
            label = self._inlay_label(hint.get("label", ""))
            if not label:
                continue
            # clangd присылает параметры как "name:" без пробела —
            # добавляем его, чтобы было P(name: "ivan"), как в VS Code
            if hint.get("kind") == 2 and not label.endswith(" "):
                label += " "
            pos = lsp_to_pos(ed, hint.get("position", {}))
            if pos < 0 or pos > doc_len:
                continue
            out.append((pos, label))
        self._inlay_hints = out
        self._overlay.set_inlay_hints(out)

    @staticmethod
    def _inlay_label(label) -> str:
        """label может быть строкой или списком InlayHintLabelPart."""
        if isinstance(label, str):
            return label
        if isinstance(label, list):
            parts = []
            for part in label:
                if isinstance(part, dict):
                    parts.append(part.get("value", ""))
                elif isinstance(part, str):
                    parts.append(part)
            return "".join(parts)
        return ""

    # --- переход к определению ---

    def goto_definition(self):
        """F12: определение символа под курсором."""
        if not self.attached:
            return
        pos = self.editor.SendScintilla(SCI_GETCURRENTPOS)
        get_lsp_manager().request_definition(
            self.editor, pos_to_lsp(self.editor, pos), self._on_definition
        )

    def goto_definition_at(self, pos: int):
        if not self.attached:
            return
        get_lsp_manager().request_definition(
            self.editor, pos_to_lsp(self.editor, pos), self._on_definition
        )

    def _on_definition(self, locations):
        if not locations:
            return
        ed = self.editor
        try:
            ed.objectName()
        except RuntimeError:
            return
        uri, start = locations[0]
        path = uri_to_path(uri)
        if not os.path.isabs(path):
            path = os.path.abspath(path)
        window = find_ancestor(ed, lambda w: hasattr(w, "tile_manager"))
        if window is None:
            return
        pane = window.tile_manager.get_active_pane()
        if pane is None:
            return
        if os.path.normcase(os.path.abspath(path)) == \
                os.path.normcase(os.path.abspath(ed.file_path)):
            target = ed
        else:
            target = pane.add_editor(file_path=path)
        if target is None:
            return
        pos = lsp_to_pos(target, start)
        target.SendScintilla(SCI_GOTOPOS, min(pos, target.length()))
        target.SendScintilla(SCI_SCROLLCARET)
        self._flash_definition(target, pos)
        self._clear_ctrl_underline()
        self._clear_hover_literal()
        target.setFocus()

    def _flash_definition(self, editor, pos):
        """Короткое подчёркивание токена, куда перешли (как в VS Code)."""
        self.clear_definition_flash()
        length = editor.SendScintilla(SCI_GETLENGTH)
        if pos >= length:
            return
        start = pos
        while start > 0:
            ch = chr(editor.SendScintilla(SCI_GETCHARAT, start - 1))
            if not (ch.isalnum() or ch == "_"):
                break
            start -= 1
        end = pos
        while end < length:
            ch = chr(editor.SendScintilla(SCI_GETCHARAT, end))
            if not (ch.isalnum() or ch == "_"):
                break
            end += 1
        if end <= start:
            return
        editor.SendScintilla(SCI_SETINDICATORCURRENT, DEF_INDICATOR)
        editor.SendScintilla(SCI_INDICATORFILLRANGE, start, end - start)
        self._flash_editor = editor
        self._flash_range = (start, end)
        self._def_flash_timer.start(_DEF_FLASH_MS)

    def clear_definition_flash(self):
        """Снимает подчёркивание цели перехода."""
        self._def_flash_timer.stop()
        editor = self._flash_editor
        if editor is not None:
            try:
                s, e = self._flash_range
                editor.SendScintilla(SCI_SETINDICATORCURRENT, DEF_INDICATOR)
                editor.SendScintilla(SCI_INDICATORCLEARRANGE, s, e - s)
            except RuntimeError:
                pass
        self._flash_editor = None
        self._flash_range = (0, 0)

    def reveal_position(self, pos: int):
        """Переход к позиции с короткой вспышкой токена (список диагностик)."""
        ed = self.editor
        pos = min(pos, ed.SendScintilla(SCI_GETLENGTH))
        ed.SendScintilla(SCI_GOTOPOS, pos)
        ed.SendScintilla(SCI_SCROLLCARET)
        self._flash_definition(ed, pos)
        ed.setFocus()

    # --- переименование символа (F2) ---

    def rename_symbol(self):
        """F2: переименование символа под курсором через textDocument/rename."""
        if not self.attached:
            return
        if not getattr(self._session, "rename_supported", False):
            return  # сервер не заявил renameProvider в initialize
        ed = self.editor
        pos = ed.SendScintilla(SCI_GETCURRENTPOS)
        word = self._word_at(pos)
        if not word:
            return
        window = find_ancestor(ed, lambda w: hasattr(w, "theme_manager"))
        if window is None:
            return
        new_name, ok = input_text(window, "Rename Symbol", "New name:",
                                  window.theme_manager, window.current_theme,
                                  text=word)
        if not ok or not new_name or new_name == word:
            return
        get_lsp_manager().request_rename(
            ed, pos_to_lsp(ed, pos), new_name, self._on_rename)

    def _on_rename(self, edits):
        """Ответ сервера: применяем WorkspaceEdit ко всем файлам."""
        if not edits:
            return
        self._apply_workspace_edit(edits)

    def _apply_workspace_edit(self, edits):
        """Применяет [(uri, [(start, end, new_text)])] к открытым редакторам
        и файлам на диске; итог печатается в консоль."""
        window = find_ancestor(self.editor, lambda w: hasattr(w, "tile_manager"))
        if window is None:
            return
        open_editors = {}
        for pane in window.tile_manager.panes():
            for e in pane.get_all_editors():
                if e.file_path:
                    open_editors[os.path.normcase(os.path.abspath(e.file_path))] = e
        tab_changed = disk_changed = skipped = 0
        for uri, items in edits:
            path = uri_to_path(canonical_uri(uri))
            if not path:
                continue
            target = open_editors.get(
                os.path.normcase(os.path.abspath(path)))
            if target is not None:
                if self._apply_edits_to_editor(target, items):
                    tab_changed += 1
                continue
            try:
                with open(path, "r", encoding="utf-8", newline="") as f:
                    text = f.read()
            except (OSError, UnicodeDecodeError):
                skipped += 1
                continue
            new_text = apply_text_edits(text, items)
            if new_text == text:
                continue
            try:
                with open(path, "w", encoding="utf-8", newline="") as f:
                    f.write(new_text)
            except OSError:
                skipped += 1
                continue
            disk_changed += 1
        print(f"[rename] files changed: {tab_changed} (tab)"
              f" + {disk_changed} (disk), skipped: {skipped}")

    def _apply_edits_to_editor(self, target, items) -> bool:
        """Правки в открытом редакторе: один undo-шаг, правки от конца."""
        try:
            target.objectName()
        except RuntimeError:
            return False
        applied = False
        target.beginUndoAction()
        try:
            ordered = sorted(items,
                             key=lambda i: (i[0].get("line", 0),
                                            i[0].get("character", 0)),
                             reverse=True)
            for start, end, new_text in ordered:
                s = lsp_to_pos(target, start)
                e = lsp_to_pos(target, end)
                if s < 0 or e < s:
                    continue
                line_s = target.SendScintilla(SCI_LINEFROMPOSITION, s)
                col_s = s - target.SendScintilla(SCI_POSITIONFROMLINE, line_s)
                line_e = target.SendScintilla(SCI_LINEFROMPOSITION, e)
                col_e = e - target.SendScintilla(SCI_POSITIONFROMLINE, line_e)
                target.setSelection(line_s, col_s, line_e, col_e)
                target.replaceSelectedText(new_text)
                applied = True
        finally:
            target.endUndoAction()
        return applied

    def _word_at(self, pos: int) -> str:
        """Слово (буквы/цифры/_) вокруг байтовой позиции, иначе пустая строка."""
        ed = self.editor
        length = ed.SendScintilla(SCI_GETLENGTH)
        if pos < 0 or pos > length:
            return ""
        start = pos
        while start > 0:
            ch = chr(ed.SendScintilla(SCI_GETCHARAT, start - 1))
            if not (ch.isalnum() or ch == "_"):
                break
            start -= 1
        end = pos
        while end < length:
            ch = chr(ed.SendScintilla(SCI_GETCHARAT, end))
            if not (ch.isalnum() or ch == "_"):
                break
            end += 1
        return ed.text(start, end)

    # --- Ctrl-подчёркивание токена ---

    def _ctrl_token_range(self, pos: int) -> Optional[Tuple[int, int]]:
        """Границы токена (слова) вокруг позиции."""
        ed = self.editor
        try:
            length = ed.SendScintilla(SCI_GETLENGTH)
        except RuntimeError:
            return None
        if pos >= length:
            return None
        # позиция на пробеле/пунктуации — слово искать нечего
        raw = ed.SendScintilla(SCI_GETCHARAT, pos)
        if raw < 0 or raw > 0x10FFFF:
            return None
        if not (chr(raw).isalnum() or chr(raw) == "_"):
            return None
        start = pos
        while start > 0:
            raw = ed.SendScintilla(SCI_GETCHARAT, start - 1)
            # Во время уничтожения виджета Scintilla может вернуть мусор
            if raw < 0 or raw > 0x10FFFF:
                break
            ch = chr(raw)
            if not (ch.isalnum() or ch == "_"):
                break
            start -= 1
        end = pos
        while end < length:
            raw = ed.SendScintilla(SCI_GETCHARAT, end)
            if raw < 0 or raw > 0x10FFFF:
                break
            ch = chr(raw)
            if not (ch.isalnum() or ch == "_"):
                break
            end += 1
        if end <= start:
            return None
        return start, end

    def _set_ctrl_underline(self, pos: int):
        """Обводит токен рамкой, пока зажат Ctrl.

        Рисование — в ErrorLensOverlay (со сдвигом inlay-подсказок),
        Scintilla-индикатор не используем: он рисует на несдвинутых
        позициях и не совпадает с видимым словом.
        """
        rng = self._ctrl_token_range(pos)
        if rng is None:
            self._clear_ctrl_underline()
            return
        if rng == self._ctrl_range:
            return
        self._clear_ctrl_underline()
        s, e = rng
        # Цвет подчёркивания — как у токена под курсором (число, строка…)
        color: QColor = None
        lexer = self.editor.current_lexer
        if lexer is not None:
            style = self.editor.SendScintilla(SCI_GETSTYLEAT, s)
            c = lexer.color(style)
            if c.isValid():
                color = c
        self._ctrl_range = rng
        try:
            self._overlay.set_ctrl_range(s, e, color)
        except RuntimeError:
            ...
        # Ctrl+токен = переход к определению — показываем «кликающую» руку
        self.editor.SendScintilla(SCI_SETCURSOR, SC_CURSORHAND)

    def _clear_ctrl_underline(self):
        """Снимает рамку токена под Ctrl."""
        if self._ctrl_range is None:
            return
        self._ctrl_range = None
        try:
            self._overlay.set_ctrl_range(None)
            self.editor.SendScintilla(SCI_SETCURSOR, SC_CURSORNORMAL)
        except RuntimeError:
            pass

    # --- hover-подсветка литерала (число/строка под мышью) ---

    # Число как в SemanticLexer (0x/0b/float, суффиксы f/u/l)
    _NUMBER_RE = re.compile(NUMBER_PATTERN)

    def _literal_token_range(self, pos: int) -> Optional[Tuple[int, int]]:
        """Границы токена-литерала (число/строка/символ) вокруг позиции.

        Определяем по содержимому строки текста, а не по стилю лексера:
        ID стилей у лексеров разные, а свежие правки могут быть ещё не
        стилизованы (Scintilla красит лениво). Строковые индексы переводим
        в байтовые — Scintilla работает с байтами.
        """
        ed = self.editor
        try:
            length = ed.SendScintilla(SCI_GETLENGTH)
            if pos >= length:
                return None
            line = ed.SendScintilla(SCI_LINEFROMPOSITION, pos)
            line_start = ed.SendScintilla(SCI_POSITIONFROMLINE, line)
            text = ed.text(line)
        except RuntimeError:
            return None
        # PyQt6 добавляет EOL к editor.text(i) — позициям он не нужен
        if text.endswith("\r\n"):
            text = text[:-2]
        elif text.endswith("\n"):
            text = text[:-1]
        # Scintilla отдаёт байтовые позиции, text — Python-строка:
        # кириллица занимает 2 байта, поэтому байтовый столбец переводим
        # в символьный, иначе hover на строках с UTF-8 не находит литерал
        byte_col = pos - line_start
        if byte_col < 0:
            return None
        col = len(text.encode("utf-8")[:byte_col].decode("utf-8", errors="ignore"))
        if col >= len(text):
            return None
        ch = text[col]
        if ch in "\"'":
            start, end = self._quote_literal_bounds(text, col, ch)
        else:
            # число: границы слова вокруг позиции, но только если позиция
            # ВНУТРИ числа (иначе hover на ';' после числа тянет само число)
            start = col
            while start > 0 and (text[start - 1].isalnum()
                                 or text[start - 1] in "._'"):
                start -= 1
            end = col
            while end < len(text) and (text[end].isalnum()
                                       or text[end] in "._'"):
                end += 1
            if not (start <= col < end
                    and self._NUMBER_RE.fullmatch(text[start:end])):
                qb = self._quote_containing(text, col)
                if qb is None:
                    return None
                start, end = qb
        bs = line_start + len(text[:start].encode("utf-8"))
        be = line_start + len(text[:end].encode("utf-8"))
        return (bs, be) if be > bs else None

    @staticmethod
    def _escaped_quote(text, i) -> bool:
        """Кавычка на позиции i экранирована (нечётное число слэшей перед ней)."""
        slashes = 0
        j = i
        while j > 0 and text[j - 1] == "\\":
            slashes += 1
            j -= 1
        return slashes % 2 == 1

    @staticmethod
    def _quote_end(text, i, quote) -> Optional[int]:
        """Индекс сразу после закрывающей кавычки (None — не закрыта)."""
        while i < len(text):
            c = text[i]
            if c == "\\":
                i += 2
                continue
            if c == quote:
                return i + 1
            i += 1
        return None

    def _quote_literal_bounds(self, text, col, quote):
        """Границы строки/символа, когда курсор стоит на кавычке."""
        # Чётное число неэкранированных кавычек ДО col — эта кавычка
        # открывающая, иначе (нечётное) — закрывающая, ищем открывающую левее
        opens = sum(1 for i in range(col)
                    if text[i] == quote and not self._escaped_quote(text, i))
        start = col
        if opens % 2 == 1:
            while start > 0:
                if (text[start - 1] == quote
                        and not self._escaped_quote(text, start - 1)):
                    start -= 1
                    break
                start -= 1
        end = self._quote_end(text, start + 1, quote)
        return start, (end if end is not None else len(text))

    def _quote_containing(self, text, col):
        """Границы строки/символа, ВНУТРИ которой находится позиция.

        Последняя кавычка до col может быть ЗАКРЫВАЮЩЕЙ (позиция после
        строки) — её пропускаем, иначе hover на ';' после "..." тянет
        закрывающую кавычку и хвост строки.
        """
        for quote in ('"', "'"):
            q = text.rfind(quote, 0, col + 1)
            if q < 0 or self._escaped_quote(text, q):
                continue
            # нечётное число неэкранированных кавычек до q — q закрывает
            # строку, позиция после неё, а не внутри
            opens = sum(1 for i in range(q)
                        if text[i] == quote and not self._escaped_quote(text, i))
            if opens % 2 == 1:
                continue
            end = self._quote_end(text, q + 1, quote)
            if end is None:
                end = len(text)  # незакрытая до конца строки
            if end > col:
                return q, end
        return None

    def _punct_range(self, pos: int) -> Optional[Tuple[int, int]]:
        """Один символ пунктуации/оператора под позицией (не пробел)."""
        ed = self.editor
        try:
            length = ed.SendScintilla(SCI_GETLENGTH)
        except RuntimeError:
            return None
        if pos >= length:
            return None
        raw = ed.SendScintilla(SCI_GETCHARAT, pos)
        if raw < 0 or raw > 0x10FFFF:
            return None
        ch = chr(raw)
        if ch.isspace():
            return None
        return (pos, pos + 1)

    def _set_hover_literal(self, pos: int):
        """Слабое выделение токена под мышью (как Ctrl, но прозрачнее).

        Литералы — по содержимому строки, слова и пунктуация — по символам;
        LSP не нужен: подсветка работает и в файлах без сервера (.lm и т.п.).
        """
        rng = self._literal_token_range(pos)
        if rng is None:
            rng = self._ctrl_token_range(pos)
        if rng is None:
            rng = self._punct_range(pos)
        if rng is None:
            self._clear_hover_literal()
            return
        if rng == self._hover_lit_range:
            return
        self._clear_hover_literal()
        s, e = rng
        color: QColor = None
        lexer = self.editor.current_lexer
        if lexer is not None:
            c = lexer.color(self.editor.SendScintilla(SCI_GETSTYLEAT, s))
            if c.isValid():
                color = c
        self._hover_lit_range = rng
        try:
            self._overlay.set_hover_range(s, e, color)
        except RuntimeError:
            ...

    def _clear_hover_literal(self):
        """Снимает hover-подсветку токена."""
        if self._hover_lit_range is None:
            return
        self._hover_lit_range = None
        try:
            self._overlay.set_hover_range(None)
        except RuntimeError:
            pass

    def _mouse_in_margin(self, view_pos) -> bool:
        """True, если точка viewport-а попадает в область левых маргинов."""
        ed = self.editor
        x = view_pos.x()
        for i in range(5):
            x -= ed.SendScintilla(SCI_GETMARGINWIDTHN, i)
            if x < 0:
                return True
        return False

    def _update_ctrl_underline_from_mouse(self):
        """Мышь над редактором — подчеркнуть токен под ней (Ctrl зажат)."""
        ed = self.editor
        if not self.attached or not ed.viewport().underMouse():
            return
        view_pos = ed.viewport().mapFromGlobal(QCursor.pos())
        if self._mouse_in_margin(view_pos):
            self._clear_ctrl_underline()
            return
        self._set_ctrl_underline(self._pos_from_viewport(view_pos))

    # --- hover (тип/сигнатура под курсором) ---

    def _hover_request(self):
        if not self.attached:
            return
        # Hover-тултипы выключены в настройках (настраивается на LSP)
        if not (self._config and self._config.hover):
            return
        # Токен под мышью мог исчезнуть (мышь ушла в пустую зону, файл
        # изменился) — запрос по «пустой» позиции не нужен
        if self._hover_lit_range is None:
            return
        req_id = self._hover_req + 1
        self._hover_req = req_id
        get_lsp_manager().request_hover(
            self.editor, pos_to_lsp(self.editor, self._hover_pos),
            lambda text: self._on_hover(req_id, text),
        )

    def _on_hover(self, req_id, text):
        if req_id != self._hover_req:
            return  # мышь уехала/документ менялся — ответ устарел
        # Пока зажат Ctrl — попап не показываем (это переход к определению)
        if self._ctrl_range is not None:
            self.hide_popup()
            return
        if not text:
            # Сервер не отвечает на числовые/символьные литералы C/C++ —
            # определяем тип токена сами
            text = self._literal_hover_fallback()
        if not text:
            self.hide_popup()
            return
        ed = self.editor
        try:
            ed.objectName()
        except RuntimeError:
            return
        popup = self._get_hover_popup()
        if popup is None:
            return
        self._cancel_hide()
        popup.set_content(text)
        popup.show_at(QCursor.pos())

    def _get_hover_popup(self) -> Optional[HoverPopup]:
        """Кастомный тултип; создаётся лениво при первом показе."""
        if self._hover_popup is None:
            self._hover_popup = HoverPopup(self.editor, self)
            self._hover_popup.apply_theme(
                self.editor.current_theme or self.editor.theme_manager.current_theme
            )
        return self._hover_popup

    def _schedule_hide(self):
        """Отложенное скрытие плашки: сработает, только если мышь в течение
        секунды не вернулась на токен плашку."""
        popup = self._hover_popup
        if popup is None or not popup.isVisible():
            return
        # Курсор над плашкой — не прячем: плашка остаётся, сколько бы
        # курсор на ней ни висел
        if popup.under_mouse():
            return
        if not self._hover_hide_timer.isActive():
            self._hover_hide_timer.start()

    def _cancel_hide(self):
        self._hover_hide_timer.stop()

    def _hover_hide_now(self):
        popup = self._hover_popup
        if popup is None:
            return
        try:
            if not popup.isVisible():
                return
            # Курсор всё ещё над плашкой (прокрутка колесом) — оставляем её
            if popup.under_mouse():
                return
        except RuntimeError:
            return  # попап уничтожен вместе с редактором
        self.hide_popup()

    def hide_popup(self):
        if self._hover_popup is not None:
            try:
                self._hover_popup.hide()
            except RuntimeError:
                pass  # попап уничтожен вместе с редактором

    # --- сигнатура вызова (signatureHelp) ---

    def _on_cursor_moved(self):
        """Каретка сдвинулась: если внутри вызова — переспрашиваем сигнатуру."""
        try:
            ed = self.editor
            ed.objectName()
        except RuntimeError:
            return
        if not (self.attached and self._session is not None):
            return
        open_pos = self._active_in_call(ed.SendScintilla(SCI_GETCURRENTPOS))
        if open_pos is None:
            self._sig_req += 1
            self._hide_sig_popup()
            return
        self._sig_open_pos = open_pos
        self._sig_timer.start(_SIG_DELAY_MS)

    def _sig_request(self):
        """Запрос сигнатуры после дебаунса (позиция — открывающая скобка)."""
        if not (self.attached and self._session is not None):
            return
        ed = self.editor
        try:
            ed.objectName()
        except RuntimeError:
            return
        open_pos = self._sig_open_pos
        if open_pos < 0 or open_pos >= ed.SendScintilla(SCI_GETLENGTH):
            return
        req_id = self._sig_req + 1
        self._sig_req = req_id
        # Позиция — каретка (внутри вызова), не скобка: по LSP-спеке сервер
        # сам определяет активный параметр по позиции курсора
        pos = ed.SendScintilla(SCI_GETCURRENTPOS)
        get_lsp_manager().request_signature_help(
            ed, pos_to_lsp(ed, pos),
            lambda result: self._sig_on_response(req_id, result),
        )

    def _sig_on_response(self, req_id, result):
        if req_id != self._sig_req:
            return  # каретка уехала/текст менялся — ответ устарел
        self._hide_sig_popup()
        if not result or not result.get("signatures"):
            return
        ed = self.editor
        try:
            ed.objectName()
        except RuntimeError:
            return
        popup = self._get_sig_popup()
        if popup is None:
            return
        sigs = result["signatures"]
        active = min(max(0, result.get("active") or 0), len(sigs) - 1)
        param = result.get("param") or 0
        self._sig_signatures = sigs
        self._sig_active = active
        self._sig_param = param
        popup.set_signatures(sigs, active, param)
        popup.show_at_caret(ed)

    def _sig_cycle(self, delta):
        """Стрелки вверх/вниз: следующая перегрузка сигнатуры."""
        if not self._sig_signatures:
            return
        n = len(self._sig_signatures)
        self._sig_active = (self._sig_active + delta) % n
        popup = self._sig_popup
        if popup is None:
            return
        try:
            popup.set_signatures(self._sig_signatures, self._sig_active,
                                 self._sig_param)
            popup.show_at_caret(self.editor)
        except RuntimeError:
            pass

    def _get_sig_popup(self) -> Optional[SignaturePopup]:
        """Попап сигнатуры; создаётся лениво при первом показе."""
        if self._sig_popup is None:
            self._sig_popup = SignaturePopup(self.editor)
            self._sig_popup.apply_theme(
                self.editor.current_theme or self.editor.theme_manager.current_theme
            )
        return self._sig_popup

    def _hide_sig_popup(self):
        if self._sig_popup is not None:
            try:
                self._sig_popup.hide()
            except RuntimeError:
                pass  # попап уничтожен вместе с редактором

    _CALL_STOP_WORDS = frozenset((
        "def class struct enum union interface fn func function sub macro "
        "if for while switch catch with lambda match case type typeof "
        "sizeof alignof"
    ).split())

    def _active_in_call(self, pos):
        """Открывающая скобка вызова, внутри которой pos (или None).

        Сканируем строку назад: ')' увеличивает глубину, '(' на нулевой
        глубине — кандидат вызова (перед ним имя/']'/')'), ')' внутри пары
        не считается. '{'/'}' и ключевые слова-определения обрывают поиск.
        """
        ed = self.editor
        line = ed.SendScintilla(SCI_LINEFROMPOSITION, pos)
        line_start = ed.SendScintilla(SCI_POSITIONFROMLINE, line)
        text = ed.text(line)
        col = min(pos - line_start, len(text))
        depth = 0
        for i in range(col - 1, -1, -1):
            ch = text[i]
            if ch == ')':
                depth += 1
            elif ch == '(':
                if depth:
                    depth -= 1
                    continue
                if not self._is_call_open(text, i):
                    return None
                return line_start + i
            elif ch in '{}':
                return None
        return None

    def _is_call_open(self, text, open_col):
        """Перед '(' — имя (не ключевое слово определения), ']' или ')'."""
        i = open_col - 1
        while i >= 0 and text[i] == ' ':
            i -= 1
        if i < 0:
            return False
        if text[i] in ')]':
            return True
        end = i + 1
        while i >= 0 and (text[i].isalnum() or text[i] == '_'):
            i -= 1
        name = text[i + 1:end]
        if not name or not (name[0].isalpha() or name[0] == '_'):
            return False
        if name in self._CALL_STOP_WORDS:
            return False  # if (x), match (x) — синтаксис, не вызов
        j = i
        while j >= 0 and text[j] == ' ':
            j -= 1
        k = j
        while k >= 0 and (text[k].isalnum() or text[k] == '_'):
            k -= 1
        if text[k + 1:j + 1] in self._CALL_STOP_WORDS:
            return False
        return True

    def _literal_hover_fallback(self) -> Optional[str]:
        """Тип числового/символьного литерала под курсором (C/C++-фолбэк)."""
        ed = self.editor
        if not self._config or self._config.language_id != "cpp":
            return None
        pos = self._hover_pos
        if pos >= ed.SendScintilla(SCI_GETLENGTH):
            return None
        line = ed.SendScintilla(SCI_LINEFROMPOSITION, pos)
        line_start = ed.SendScintilla(SCI_POSITIONFROMLINE, line)
        # Пустое место правее текста — подсказок не показываем
        if pos >= ed.SendScintilla(SCI_GETLINEENDPOSITION, line):
            return None
        text = ed.text(line)
        # Позиция могла устареть после правок — зажимаем в границы строки
        col = min(pos - line_start, len(text))
        start = col
        while start > 0 and (text[start - 1].isalnum()
                             or text[start - 1] in "._'"):
            start -= 1
        end = col
        while end < len(text) and (text[end].isalnum()
                                   or text[end] in "._'"):
            end += 1
        token = text[start:end]
        is_char = token.startswith("'") and len(token) >= 3
        if not token or (not any(c.isdigit() for c in token) and not is_char):
            return None
        if token.startswith(("0x", "0X", "0b", "0B")):
            typ = "int"
            if token[-1] in "uU":
                typ = "unsigned int"
            elif token[-1] in "lL":
                typ = "long"
            return f"integer-literal\n\nType: {typ}"
        if token.startswith("'") and token.endswith("'") and len(token) >= 3:
            return "character-literal\n\nType: char"
        if any(c in token for c in ".eEfF"):
            return ("floating-literal\n\nType: float"
                    if token[-1] in "fF"
                    else "floating-literal\n\nType: double")
        low = token.lower()
        if low.endswith("ull") or low.endswith("llu"):
            typ = "unsigned long long"
        elif low.endswith("ll"):
            typ = "long long"
        elif low.endswith("ul") or low.endswith("lu"):
            typ = "unsigned long"
        elif low.endswith("u"):
            typ = "unsigned int"
        elif low.endswith("l"):
            typ = "long"
        else:
            typ = "int"
        return f"integer-literal\n\nType: {typ}"

    # --- тема ---

    def apply_theme(self, theme_name: str):
        """Цвет индикатора цели перехода и тема hover-попапа."""
        # Дефолт (по токенам красится в _set_ctrl_underline)
        flash = QColor("#B4BEFE")
        self.editor.SendScintilla(SCI_INDICSETFORE, DEF_INDICATOR, rgb_int(flash))
        if self._hover_popup is not None:
            self._hover_popup.apply_theme(theme_name)
        if self._sig_popup is not None:
            self._sig_popup.apply_theme(theme_name)
        self._theme_name = theme_name
        tc = ThemeColors.from_theme_manager(self.editor.theme_manager, theme_name)
        # ErrorLens и подчёркивание диагностик — цветами темы (если заданы)
        self._overlay.apply_theme(tc.dark, tc.inlay_hint_color,
                                  tc.error_color, tc.warning_color)

    def on_font_size_changed(self):
        self.hide_popup()
        self._overlay.update()

    def paint_error_lens(self):
        """Вызывается из CodeEditor.paintEvent после super().paintEvent."""
        try:
            self._overlay.paint()
        except RuntimeError:
            pass

    def pre_paint(self):
        """Прячет каретку Scintilla на строках со сдвигающими inlay-подсказками.

        Должен вызываться ДО super().paintEvent: иначе Scintilla нарисует
        каретку на старом (несдвинутом) месте поверх нашего оверлея.
        """
        try:
            ed = self.editor
            ed.objectName()   # редактор мог быть уничтожен
        except RuntimeError:
            return
        style = 0 if self._overlay.should_hide_caret() else self._overlay._caret_style
        if style != self._last_caret_style:
            # Вызов инвалидирует область каретки -> repaint-петля, если слать его на каждый paint
            ed.SendScintilla(SCI_SETCARETSTYLE, style)
            self._last_caret_style = style

    # --- вспомогательное ---

    def _pos_from_viewport(self, view_pos) -> int:
        """Точка viewport-а -> байтовая позиция документа.

        Позицию ищем бинарным поиском по x-координатам символов Scintilla:
        так учитываются реальная ширина символов, табы, пробелы, zoom и DPI.
        """
        ed = self.editor
        line_h = ed.SendScintilla(SCI_TEXTHEIGHT, 0)
        if line_h <= 0:
            line_h = 1
        length = ed.SendScintilla(SCI_GETLENGTH)
        # Пустая зона ниже последней строки документа: токенов там нет.
        # Границу считаем по y-координатам Scintilla, а не формулой
        # first + y/line_h: верх строк не кратен line_h (верхний отступ
        # viewport), и формула на нижней половине строки уводит на строку
        # ниже — hover «прыгал» на чужой токен
        if length > 0:
            last_line = ed.SendScintilla(SCI_LINEFROMPOSITION, length - 1)
            last_top = ed.SendScintilla(SCI_POSITIONFROMLINE, last_line)
            if view_pos.y() >= (ed.SendScintilla(
                    SCI_POINTYFROMPOSITION, 0, last_top) + line_h):
                return length
        # Строка под мышью: y считаем от верха первой видимой строки — так
        # учитываются верхний отступ viewport и частично обрезанная (при
        # прокрутке) первая строка, а не только первая полностью видимая
        first = ed.SendScintilla(SCI_GETFIRSTVISIBLELINE)
        first_top = ed.SendScintilla(SCI_POINTYFROMPOSITION, 0,
                                     ed.SendScintilla(SCI_POSITIONFROMLINE, first))
        dy = view_pos.y() - first_top
        line = first + (int(dy / line_h) if dy >= 0 else 0)
        if length > 0:
            line = min(line, last_line)
        # Сдвинутые inlay-подсказки: компенсируем сдвиг до «несдвинутого» x
        x = self._overlay.adjust_click_x(line, int(view_pos.x()))
        line_start = ed.SendScintilla(SCI_POSITIONFROMLINE, line)
        line_end = ed.SendScintilla(SCI_GETLINEENDPOSITION, line)
        try:
            # последняя позиция, чьё начало символа левее/на x (POINTX
            # учитывает маргины, скролл, zoom и реальную ширину символа)
            lo, hi = line_start, line_end
            best = line_start
            while lo <= hi:
                mid = (lo + hi) // 2
                if ed.SendScintilla(SCI_POINTXFROMPOSITION, 0, mid) <= x:
                    best = mid
                    lo = mid + 1
                else:
                    hi = mid - 1
            return best
        except Exception:
            pass  # POINTX недоступен — грубый расчёт по ширине символа
        x += ed.SendScintilla(SCI_GETXOFFSET)
        for i in range(5):
            x -= ed.SendScintilla(SCI_GETMARGINWIDTHN, i)
        cw = ed.SendScintilla(SCI_TEXTWIDTH, 0, b"0")
        if cw <= 0:
            cw = QFontMetrics(ed.font()).horizontalAdvance("0")
        if cw <= 0:
            return line_start
        col = max(int(x / cw), 0)
        # символьный столбец -> байтовый сдвиг (UTF-8): иначе на строках
        # с многобайтовыми символами до курсора позиция «уезжает» вправо
        col_bytes = len(ed.text(line).encode("utf-8")[:col]
                        .decode("utf-8", errors="ignore").encode("utf-8"))
        # правее текста строки позиция не должна «проваливаться» в строку ниже
        return min(line_start + col_bytes, line_end)
