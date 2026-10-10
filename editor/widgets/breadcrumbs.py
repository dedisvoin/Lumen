"""
BreadcrumbsBar — «хлебные крошки» над редактором: файл › класс › метод.

Путь символа под кареткой берётся из LSP textDocument/documentSymbol
(дерево, см. normalize_symbol_tree: DocumentSymbol[] и SymbolInformation[]).
Клик по сегменту — переход к началу символа. Бар скрывается, если файл
вне LSP или сервер не отдаёт символы.
"""

import os

from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtGui import QColor, QFont, QFontMetrics, QPainter
from PyQt6.QtWidgets import QWidget

from editor.constants import UI_FONT_SIZE
from editor.lsp import get_lsp_manager
from editor.lsp.protocol import symbol_path_at
from editor.scintilla_constants import (
    SCI_GOTOPOS, SCI_GETCURRENTPOS, SCI_SCROLLCARET,
)
from editor.theme_utils import ThemeColors

_BAR_HEIGHT = 24     # высота полоски, px
_SEP = "  ›  "       # разделитель сегментов
_LEFT_PAD = 8


class BreadcrumbsBar(QWidget):
    """Полоска пути символа под кареткой (файл › класс › метод)."""

    def __init__(self, editor, parent=None):
        super().__init__(parent)
        self._editor = editor
        self._tree: list = []
        self._tree_req = 0            # номер последнего запроса дерева
        self._synced_connected = False
        self._path: list = []        # [(name, kind, start, end)] под кареткой
        self._segments: list = []    # [(x0, x1, name, start)] для кликов
        self._file = ""
        self._has_lsp = False        # LSP подключён (бар виден даже без дерева)
        self._dark = True
        self._fg = QColor("#cdd6f4")
        self._muted = QColor("#7f849c")
        self._bg = QColor("#1e1e2e")
        self.setFixedHeight(_BAR_HEIGHT)
        self.setVisible(False)

        try:
            editor.cursorPositionChanged.connect(self._on_cursor_moved)
        except RuntimeError:
            pass
        # Дерево запрашиваем сразу после синхронизации буфера с сервером
        # (сигнал document_synced моста), а не немедленно: иначе documentSymbol
        # уходит ДО didChange и clangd отдаёт позиции по старому документу
        self._connect_synced()
        self._fetch_tree()

    def _connect_synced(self):
        """Подписаться на document_synced моста (один раз)."""
        if self._synced_connected:
            return
        bridge = getattr(self._editor, "_lsp_bridge", None)
        if bridge is not None:
            try:
                bridge.document_synced.connect(self._fetch_tree)
                self._synced_connected = True
            except (TypeError, RuntimeError):
                pass

    # --- события редактора ---

    def on_text_changed(self):
        """Правка текста: дерево устарело, ждём синхронизации буфера с LSP.

        Сам запрос documentSymbol уходит не здесь, а по сигналу
        document_synced моста — ровно после того, как didChange дойдёт до
        сервера. Иначе clangd вернул бы позиции по старому документу
        (видно, когда вставляешь пустые строки: каретка на новой строке,
        а крошки всё ещё «внутри» функции с прежней строки).
        """
        self._tree = []
        self._path = []
        self._segments = []
        self.update()

    def _on_cursor_moved(self, *args):
        self._refresh_path()

    # --- LSP ---

    def _fetch_tree(self):
        ed = self._editor
        fp = getattr(ed, "file_path", None) or ""
        self._file = os.path.basename(fp) if fp else ""
        bridge = getattr(ed, "_lsp_bridge", None)
        self._has_lsp = bool(fp) and bridge is not None and bridge.attached
        if not self._has_lsp:
            self._tree = []
            self._refresh_path()
            return
        self._tree_req += 1
        rid = self._tree_req
        try:
            get_lsp_manager().request_symbol_tree(
                ed, lambda t: self._on_tree(rid, t))
        except Exception:
            # запрос не ушёл — если это последний, сбрасываем дерево
            if rid == self._tree_req:
                self._tree = []
                self._refresh_path()

    def _on_tree(self, rid, tree):
        # Игнорируем устаревший ответ: если пришёл не от последнего запроса,
        # он отдаст дерево со старыми позициями (хлебные крошки покажут
        # «внутри main()» на пустых строках после вставки выше функции).
        if rid != self._tree_req:
            return
        self._tree = tree or []
        self._refresh_path()

    def _refresh_path(self):
        ed = self._editor
        try:
            pos = ed.SendScintilla(SCI_GETCURRENTPOS)
        except RuntimeError:
            return
        # _has_lsp кэшируется в _fetch_tree, но пересчитываем из живого
        # состояния моста, чтобы бар сам «вылечивался» после кратковременного
        # отключения LSP (иначе оставался скрытым до следующей правки текста)
        fp = getattr(ed, "file_path", None) or ""
        self._file = os.path.basename(fp) if fp else ""
        bridge = getattr(ed, "_lsp_bridge", None)
        was_lsp = self._has_lsp
        self._has_lsp = bool(fp) and bridge is not None and bridge.attached
        self._path = symbol_path_at(self._tree, ed, pos) if self._tree else []
        self._segments = []
        self.update()
        # Бар виден, если файл открыт и LSP подключён (даже без символов);
        # без LSP (lua и т.п.) — скрыт, путь при печати показывать нечего
        self.setVisible(bool(self._file) and self._has_lsp)
        # LSP только что подключился (ранее был отключён) — дерево могло
        # устареть/опустеть: подписываемся на document_synced и запрашиваем
        if self._has_lsp and not was_lsp:
            self._connect_synced()
            self._fetch_tree()

    # --- тема ---

    def apply_theme(self, theme_name: str):
        try:
            tc = ThemeColors.from_theme_manager(self._editor.theme_manager,
                                                theme_name)
        except Exception:
            return
        self._dark = tc.dark
        self._fg = QColor(tc.fg)
        self._muted = QColor(tc.muted)
        self._bg = tc.editor_bg_color
        self.update()

    # --- отрисовка ---

    def _font(self) -> QFont:
        f = QFont(self.font())
        f.setPointSize(UI_FONT_SIZE)
        return f

    def paintEvent(self, event):
        p = QPainter(self)
        p.fillRect(self.rect(), self._bg)
        # Путь пуст — пустая полоска: имя файла не печатаем, но виджет
        # остаётся, чтобы раскладка не прыгала при печати
        if not self._path:
            p.end()
            return
        fm = QFontMetrics(self._font())
        p.setFont(self._font())
        # Сегменты: файл (без перехода) + путь символов под кареткой
        items = [(self._file, None)] + [(name, s)
                                        for name, _k, s, _e in self._path]
        if not items:
            p.end()
            return
        x = _LEFT_PAD
        max_x = self.width() - _LEFT_PAD
        # Не влезает: показываем хвост пути, спереди многоточие
        if (x + fm.horizontalAdvance(
                _SEP.join(n for n, _ in items))) > max_x:
            kept = []
            w = x + fm.horizontalAdvance("…" + _SEP)
            for n, s in reversed(items):
                w += fm.horizontalAdvance(n)
                if w > max_x:
                    break
                kept.append((n, s))
                w += fm.horizontalAdvance(_SEP)
            items = [("…", None)] + list(reversed(kept))
        self._segments = []
        y = int(self.height() / 2 + fm.ascent() / 2 - 1)
        for i, (name, start) in enumerate(items):
            if not name:
                continue
            # Все сегменты приглушённые — без яркого выделения последнего
            p.setPen(QColor(self._muted))
            p.drawText(int(x), y, name)
            self._segments.append((x, x + fm.horizontalAdvance(name),
                                   name, start))
            x += fm.horizontalAdvance(name)
            if i < len(items) - 1:
                # Разделитель между сегментами — приглушённым цветом
                p.setPen(QColor(self._muted))
                p.drawText(int(x), y, _SEP)
                x += fm.horizontalAdvance(_SEP)
        p.end()

    def mousePressEvent(self, event):
        if event.button() != Qt.MouseButton.LeftButton:
            return
        x = event.position().x()
        for x0, x1, _name, start in self._segments:
            if x0 <= x <= x1 and start is not None:
                self._goto(start)
                return

    def _goto(self, start: int):
        ed = self._editor
        try:
            ed.SendScintilla(SCI_GOTOPOS, start)
            ed.SendScintilla(SCI_SCROLLCARET)
            ed.setFocus()
        except RuntimeError:
            pass
