"""
Поиск по всем закладкам проекта.

Ctrl+Shift+B — попап, аналогичный поиску файлов (file_finder.py): строка
ввода сверху и список закладок снизу. При вводе список динамически
фильтруется; стрелки перемещают выбор, Enter переходит к закладке и
закрывает панель.

Источник закладок — открытые редакторы всех окон (живые заметки) плюс
сохранённые закладки закрытых файлов (bookmark_store), без дубликатов.
"""

import os
from typing import List, Tuple

from PyQt6.QtCore import QEvent, QRectF, Qt, QTimer, pyqtSignal
from PyQt6.QtGui import QColor, QIcon, QPainter
from PyQt6.QtWidgets import (
    QFrame, QHBoxLayout, QLabel, QLineEdit, QScrollArea, QSizePolicy,
    QToolButton, QVBoxLayout, QWidget,
)

from editor.scrollbar_qss import scrollbar_qss
from editor.theme_utils import ThemeColors
from editor.widgets.file_icons import file_icon_for_path, load_ui_icon

# (абсолютный путь, имя файла, строка 0-based, заметка)
BookmarkInfo = Tuple[str, str, int, str]


def collect_bookmarks() -> List[BookmarkInfo]:
    """Все закладки проекта: из открытых редакторов и из хранилища.

    Дедуп по (нормализованный путь, строка), чтобы закрытый файл не
    дублировался с открытым.
    """
    from PyQt6.QtWidgets import QApplication

    entries = {}
    seen = set()

    # Открытые редакторы всех окон (живые заметки)
    for tl in QApplication.topLevelWidgets():
        tm = getattr(tl, "tile_manager", None)
        if tm is None:
            continue
        try:
            panes = tm.panes()
        except RuntimeError:
            continue
        for pane in panes:
            try:
                editors = pane.get_all_editors()
            except RuntimeError:
                continue
            for editor in editors:
                path = editor.file_path
                if not path:
                    continue
                try:
                    bms = editor.get_bookmarks()
                except Exception:
                    continue
                for bm in bms:
                    line = int(bm.get("line", 0) or 0)
                    note = bm.get("note", "") or ""
                    key = (os.path.normcase(os.path.abspath(path)), line)
                    if key in seen:
                        continue
                    seen.add(key)
                    entries[key] = (path, os.path.basename(path), line, note)

    # Сохранённые закладки закрытых файлов
    try:
        from editor.bookmark_store import all_bookmarks
        store = all_bookmarks()
    except Exception:
        store = {}
    for raw_path, items in store.items():
        for bm in items:
            try:
                line = int(bm.get("line", 0) or 0)
                note = bm.get("note", "") or ""
            except (TypeError, ValueError):
                continue
            key = (os.path.normcase(os.path.abspath(raw_path)), line)
            if key in seen:
                continue
            seen.add(key)
            entries[key] = (raw_path, os.path.basename(raw_path), line, note)

    return list(entries.values())


class _BookmarkRow(QToolButton):
    """Строка закладки: иконка + имя файла и строка слева, заметка справа."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._icon = QIcon()
        self._name = ""
        self._line = 0
        self._note = ""
        self._fg = QColor(255, 255, 255)
        self._muted = QColor(200, 200, 200)
        self._hover = QColor(255, 255, 255, 30)
        self._sel = QColor(255, 255, 255, 60)
        self._hovered = False
        self._selected = False
        self.setAttribute(Qt.WidgetAttribute.WA_Hover, True)
        self.setAutoRaise(True)
        self.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextOnly)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setMinimumHeight(27)
        self.setSizePolicy(
            QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)

    def set_data(self, icon: QIcon, name: str, line: int, note: str,
                 fg: QColor, muted: QColor, hover: QColor, sel: QColor):
        self._icon = icon
        self._name = name
        self._line = line
        self._note = note
        self._fg = fg
        self._muted = muted
        self._hover = hover
        self._sel = sel
        self.update()

    def set_hovered(self, hovered: bool):
        if hovered != self._hovered:
            self._hovered = hovered
            self.update()

    def set_selected(self, selected: bool):
        if selected != self._selected:
            self._selected = selected
            self.update()

    def enterEvent(self, event):
        self.set_hovered(True)
        super().enterEvent(event)

    def leaveEvent(self, event):
        self.set_hovered(False)
        super().leaveEvent(event)

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(self.rect()).adjusted(1, 1, -1, -1)
        if self._selected:
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(self._sel)
            p.drawRoundedRect(r, 4, 4)
        elif self._hovered:
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(self._hover)
            p.drawRoundedRect(r, 4, 4)

        fm = self.fontMetrics()
        # Иконка расширения слева
        icon_size = 16
        icon_y = (self.height() - icon_size) // 2
        self._icon.paint(p, 8, icon_y, icon_size, icon_size)

        # Имя + «:строка» (без текста заметки справа)
        name = f"{self._name} :{self._line + 1}"
        name_rect = self.rect().adjusted(8 + icon_size + 6, 0, -12, 0)
        elided_name = fm.elidedText(
            name, Qt.TextElideMode.ElideRight, name_rect.width())
        p.setPen(self._fg)
        p.drawText(name_rect,
                   Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                   elided_name)
        p.end()


class BookmarkFindPopup(QFrame):
    """Поиск по всем закладкам проекта (панель как у поиска файлов)."""

    last_query = ""

    def __init__(self, entries: List[BookmarkInfo], theme_manager,
                 theme_name: str, on_pick, parent=None):
        super().__init__(
            parent,
            Qt.WindowType.Popup | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.NoDropShadowWindowHint,
        )
        self._on_pick = on_pick
        self._all: List[BookmarkInfo] = list(entries)
        self._shown: List[BookmarkInfo] = []
        self._rows: List[_BookmarkRow] = []
        self._current = -1
        self._query = ""
        self._filter_timer = None
        self.setFixedWidth(560)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self._apply_theme(theme_manager, theme_name)

        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(8, 8, 8, 8)
        self._layout.setSpacing(6)

        self._search = QLineEdit()
        self._search.setPlaceholderText("Search bookmarks...")
        if BookmarkFindPopup.last_query:
            self._search.setText(BookmarkFindPopup.last_query)
        self._search.textChanged.connect(self._schedule_filter)
        self._search.installEventFilter(self)

        self._counter = QLabel()
        self._counter.setAlignment(
            Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        self._counter.setStyleSheet(
            f"color: {self._muted}; padding: 0 4px 0 8px;")
        self._counter.setText("(no bookmarks)")

        self._search_row = QWidget()
        search_row_layout = QHBoxLayout(self._search_row)
        search_row_layout.setContentsMargins(0, 0, 0, 0)
        search_row_layout.setSpacing(4)
        search_row_layout.addWidget(self._search, 1)
        search_row_layout.addWidget(self._counter)
        self._layout.addWidget(self._search_row)

        self._separator = QFrame(self)
        self._separator.setObjectName("finderSeparator")
        self._separator.setFixedHeight(1)
        self._layout.addWidget(self._separator)

        self._scroll = QScrollArea(self)
        self._scroll.setWidgetResizable(True)
        self._scroll.setFixedHeight(280)
        self._scroll.setFrameShape(QFrame.Shape.NoFrame)
        self._scroll.setHorizontalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._scroll.setVerticalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self._list_widget = QWidget()
        self._list_widget.setAutoFillBackground(False)
        self._list_layout = QVBoxLayout(self._list_widget)
        self._list_layout.setContentsMargins(0, 0, 0, 0)
        self._list_layout.setSpacing(2)
        self._scroll.setWidget(self._list_widget)
        self._scroll.viewport().setAutoFillBackground(False)
        self._scroll.verticalScrollBar().installEventFilter(self)
        self._layout.addWidget(self._scroll)

        self._apply_filter()

    # ---- тема ----

    def _apply_theme(self, theme_manager, theme_name: str):
        tc = ThemeColors.from_theme_manager(theme_manager, theme_name)
        self._bg = tc.editor_bg_color.name()
        self._fg = tc.fg
        self._muted = tc.muted
        self._border = tc.border
        hover = QColor(tc.fg)
        hover.setAlpha(30)
        self._hover = hover
        sel = QColor(tc.fg)
        sel.setAlpha(60)
        self._sel = sel
        self.setStyleSheet(f"""
            BookmarkFindPopup {{
                background-color: {self._bg};
                border: 0.5px solid {self._border};
                border-radius: 6px;
            }}
            QLineEdit {{
                background-color: transparent;
                color: {self._fg};
                border: none;
                padding: 5px 4px;
            }}
            QFrame#finderSeparator {{
                background-color: {self._border};
            }}
            QScrollArea {{
                background: transparent;
            }}
            QScrollArea > QWidget > QWidget {{
                background-color: transparent;
            }}
        """ + scrollbar_qss(self._bg, QColor(self._fg)))

    # ---- фильтрация ----

    def _schedule_filter(self, *_):
        if self._filter_timer is None:
            self._filter_timer = QTimer(self)
            self._filter_timer.setSingleShot(True)
            self._filter_timer.timeout.connect(self._apply_filter)
        self._filter_timer.start(100)

    def _apply_filter(self):
        q = self._search.text().strip().lower()
        self._query = q
        if q:
            self._shown = [
                b for b in self._all
                if (q in b[1].lower()
                    or q in b[3].lower()
                    or q in b[0].lower()
                    or q in str(b[2] + 1)
                    or q in str(b[2]))
            ]
        else:
            self._shown = list(self._all)
        self._rebuild(self._shown)

    def _counter_text(self, n: int) -> str:
        if not self._all:
            return "(no bookmarks)"
        if self._query:
            return f"{n} of {len(self._all)} bookmarks"
        word = "bookmark" if n == 1 else "bookmarks"
        return f"{n} {word}"

    def _rebuild(self, shown: List[BookmarkInfo]):
        while self._list_layout.count():
            item = self._list_layout.takeAt(0)
            w = item.widget()
            if w:
                w.hide()
                w.deleteLater()
        self._rows = []
        self._current = -1
        if not shown:
            if not self._all:
                text = "(no bookmarks)"
            else:
                text = "(no matches)"
            hint = QLabel(text)
            hint.setStyleSheet(f"color: {self._muted}; padding: 6px;")
            self._list_layout.addWidget(hint)
        else:
            for idx, info in enumerate(shown):
                self._list_layout.addWidget(self._make_row(idx, info))
            self._update_selection(0)
        self._counter.setText(self._counter_text(len(shown)))
        self._list_layout.addStretch(1)

    def _make_row(self, idx: int, info: BookmarkInfo) -> _BookmarkRow:
        row = _BookmarkRow(self)
        icon = load_ui_icon("bookmark_for_list", QColor(self._fg))
        row.set_data(icon, info[1], info[2], info[3],
                     QColor(self._fg), QColor(self._muted),
                     QColor(self._hover), QColor(self._sel))
        row.clicked.connect(lambda _=False, i=idx: self._pick_index(i))
        self._rows.append(row)
        return row

    # ---- выбор ----

    def _update_selection(self, index: int):
        if not self._rows:
            self._current = -1
            return
        self._current = index % len(self._rows)
        for i, row in enumerate(self._rows):
            row.set_selected(i == self._current)
        self._scroll.ensureWidgetVisible(self._rows[self._current], 0, 0)

    def _move(self, delta: int):
        self._update_selection(self._current + delta)

    def _pick_current(self):
        if 0 <= self._current < len(self._shown):
            self._pick(self._shown[self._current])

    def _pick_index(self, idx: int):
        if 0 <= idx < len(self._shown):
            self._pick(self._shown[idx])

    def _pick(self, info: BookmarkInfo):
        self.close()
        self._on_pick(info[0], info[2])

    # ---- события ----

    def showEvent(self, event):
        super().showEvent(event)
        self._search.setFocus()
        self._search.selectAll()

    def eventFilter(self, obj, event):
        scroll = getattr(self, "_scroll", None)
        if obj is self._search and event.type() == QEvent.Type.KeyPress:
            key = event.key()
            if key == Qt.Key.Key_Down:
                self._move(1)
                event.accept()
                return True
            if key == Qt.Key.Key_Up:
                self._move(-1)
                event.accept()
                return True
            if key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
                self._pick_current()
                event.accept()
                return True
            if key == Qt.Key.Key_Escape:
                self.close()
                event.accept()
                return True
        if (scroll is not None and obj is scroll.verticalScrollBar()
                and event.type() == QEvent.Type.MouseButtonRelease):
            self._search.setFocus()
        return super().eventFilter(obj, event)

    def keyPressEvent(self, event):
        key = event.key()
        if key == Qt.Key.Key_Down:
            self._move(1)
            event.accept()
        elif key == Qt.Key.Key_Up:
            self._move(-1)
            event.accept()
        elif key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            self._pick_current()
            event.accept()
        elif key == Qt.Key.Key_Escape:
            self.close()
            event.accept()
        else:
            super().keyPressEvent(event)

    def mouseMoveEvent(self, event):
        pos = event.position().toPoint()
        hovered_idx = -1
        for i, row in enumerate(self._rows):
            inside = row.rect().contains(row.mapFrom(self, pos))
            row.set_hovered(inside)
            if inside and hovered_idx < 0:
                hovered_idx = i
        if hovered_idx >= 0:
            self._update_selection(hovered_idx)
        super().mouseMoveEvent(event)

    def leaveEvent(self, event):
        for row in self._rows:
            row.set_hovered(False)
        super().leaveEvent(event)
