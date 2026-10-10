"""
Поиск файлов в открытой папке.

Ctrl+Shift+F — попап со строкой ввода сверху и списком файлов снизу:
при вводе список динамически фильтруется; стрелки перемещают выбор,
Enter открывает выбранный файл и закрывает панель. Сканирование папки
идёт в фоновом потоке, фильтрация — в главном.
"""

import os
from typing import List, Tuple

from PyQt6.QtCore import QEvent, QRectF, Qt, QThread, QTimer, pyqtSignal
from PyQt6.QtGui import QColor, QIcon, QPainter
from PyQt6.QtWidgets import (
    QFrame, QHBoxLayout, QLabel, QLineEdit, QScrollArea, QSizePolicy,
    QToolButton, QVBoxLayout, QWidget,
)

from editor.scrollbar_qss import scrollbar_qss
from editor.theme_utils import ThemeColors
from editor.widgets.file_icons import file_icon_for_path
from editor.widgets.file_tree_ops import SKIP_DIRS

# (абсолютный путь, имя файла, относительный путь)
FileInfo = Tuple[str, str, str]


class _ScanThread(QThread):
    """Собирает все файлы папки в фоновом потоке (без блокировки UI).

    Живёт сам по себе (без родителя): если попап закроется раньше, чем
    сканирование закончится, попап просит поток остановиться и ждёт его
    в closeEvent. finished->deleteLater НЕ используем: удалённый объект
    потока ещё висит в self._scan_thread, и обращение к нему из Qt-колбэка
    роняет процесс без traceback.
    """

    done = pyqtSignal(list)

    def __init__(self, root: str):
        super().__init__()
        self._root = root

    def run(self):
        files = []
        for dirpath, dirnames, filenames in os.walk(self._root):
            if self.isInterruptionRequested():
                return
            dirnames[:] = sorted(
                d for d in dirnames
                if not d.startswith(".") and d not in SKIP_DIRS
            )
            for name in filenames:
                if self.isInterruptionRequested():
                    return
                if name.startswith("."):
                    continue
                files.append((os.path.join(dirpath, name), name))
        self.done.emit(files)


class _FileRow(QToolButton):
    """Строка файла: иконка расширения + имя слева, путь справа.

    QStyleSheetStyle перехватывает отрисовку текста QToolButton, поэтому
    рисуем всё вручную через QPainter (как строки списка тасков).
    """

    def __init__(self, parent=None):
        super().__init__(parent)
        self._icon = QIcon()
        self._path = ""
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

    def set_data(self, icon: QIcon, name: str, path: str,
                 fg: QColor, muted: QColor, hover: QColor, sel: QColor):
        self._icon = icon
        self.setText(name)
        self._path = path
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
        # Путь справа: обрезаем слева, чтобы был виден ближний каталог
        max_path_w = min(fm.horizontalAdvance(self._path),
                         int(self.width() * 0.45))
        path_rect = self.rect().adjusted(0, 0, -8, 0)
        path_rect.setLeft(self.width() - 8 - max_path_w)
        elided_path = fm.elidedText(
            self._path, Qt.TextElideMode.ElideLeft, max_path_w)
        p.setPen(self._muted)
        p.drawText(path_rect,
                   Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
                   elided_path)

        # Иконка расширения слева
        icon_size = 16
        icon_y = (self.height() - icon_size) // 2
        self._icon.paint(p, 8, icon_y, icon_size, icon_size)

        # Имя между иконкой и путём
        name_rect = self.rect().adjusted(8 + icon_size + 6, 0,
                                         -(max_path_w + 16), 0)
        elided_name = fm.elidedText(
            self.text(), Qt.TextElideMode.ElideRight, name_rect.width())
        p.setPen(self._fg)
        p.drawText(name_rect,
                   Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                   elided_name)
        p.end()


class FileFindPopup(QFrame):
    """Поиск файлов в открытой папке (панель как у списка тасков)."""

    # Последний запрос: попап создаётся заново при каждом открытии,
    # поэтому текст запроса живёт на классе (переживает закрытие попапа).
    last_query = ""

    def __init__(self, root: str, theme_manager, theme_name: str,
                 on_open, parent=None):
        super().__init__(
            parent,
            Qt.WindowType.Popup | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.NoDropShadowWindowHint,
        )
        self._root = root
        self._on_open = on_open
        self._all: List[FileInfo] = []
        self._shown: List[FileInfo] = []
        self._rows: List[_FileRow] = []
        self._current = -1
        self._query = ""
        self._scan_done = False
        self._filter_timer = None
        self.setFixedWidth(560)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self._apply_theme(theme_manager, theme_name)

        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(8, 8, 8, 8)
        self._layout.setSpacing(6)

        self._search = QLineEdit()
        self._search.setPlaceholderText("Search files in folder...")
        if FileFindPopup.last_query:
            self._search.setText(FileFindPopup.last_query)
        self._search.textChanged.connect(self._schedule_filter)
        self._search.installEventFilter(self)

        self._counter = QLabel()
        self._counter.setAlignment(
            Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        self._counter.setStyleSheet(
            f"color: {self._muted}; padding: 0 4px 0 8px;")
        self._counter.setText("(scanning...)")

        self._search_row = QWidget()
        search_row_layout = QHBoxLayout(self._search_row)
        search_row_layout.setContentsMargins(0, 0, 0, 0)
        search_row_layout.setSpacing(4)
        search_row_layout.addWidget(self._search, 1)
        search_row_layout.addWidget(self._counter)
        self._layout.addWidget(self._search_row)

        # Разделитель под строкой поиска
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

        self._show_hint("(scanning...)")

        self._scan_thread = _ScanThread(root)
        self._scan_thread.done.connect(self._on_scan_done)
        self._scan_thread.start()

    def _stop_scan(self):
        """Останавливает фоновое сканирование и ждёт поток.

        Сканирование проверяет запрос прерывания в каждой папке и быстро
        останавливается, поэтому ожидание короткое. Обращение к потоку
        безопасно, даже если он уже завершился (объект не удаляем).
        """
        thread = getattr(self, "_scan_thread", None)
        if thread is not None and thread.isRunning():
            thread.requestInterruption()
            thread.wait(3000)

    def closeEvent(self, event):
        self._stop_scan()
        super().closeEvent(event)

    def hideEvent(self, event):
        # Popup-окно скрывается и по клику мимо него (без closeEvent) —
        # останавливаем сканирование и здесь, чтобы поток не остался
        # жить после закрытия попапа. Заодно запоминаем запрос, чтобы
        # восстановить его при следующем открытии.
        FileFindPopup.last_query = self._search.text().strip()
        self._stop_scan()
        super().hideEvent(event)

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
            FileFindPopup {{
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

    # ---- сканирование ----

    def _on_scan_done(self, files: list):
        self._scan_done = True
        self._all = []
        for abs_path, name in files:
            rel_path = os.path.relpath(abs_path, self._root)
            self._all.append((abs_path, name, rel_path))
        self._all.sort(key=lambda f: (f[1].lower(), f[2].lower()))
        self._apply_filter()

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
                f for f in self._all
                if q in f[1].lower() or q in f[2].lower()
            ]
        else:
            self._shown = list(self._all)
        self._rebuild(self._shown)

    def _counter_text(self, n: int) -> str:
        if not self._scan_done:
            return "(scanning...)"
        if not self._all:
            return "(no files)"
        if self._query:
            return f"{n} of {len(self._all)} files"
        word = "file" if n == 1 else "files"
        return f"{n} {word}"

    def _rebuild(self, shown: List[FileInfo]):
        while self._list_layout.count():
            item = self._list_layout.takeAt(0)
            w = item.widget()
            if w:
                w.hide()
                w.deleteLater()
        self._rows = []
        self._current = -1
        if not shown:
            if not self._scan_done:
                text = "(scanning...)"
            elif not self._all:
                text = "(no files)"
            else:
                text = "(no matches)"
            self._show_hint(text)
        else:
            for idx, info in enumerate(shown):
                self._list_layout.addWidget(self._make_row(idx, info))
            self._update_selection(0)
        self._counter.setText(self._counter_text(len(shown)))
        self._list_layout.addStretch(1)

    def _show_hint(self, text: str):
        hint = QLabel(text)
        hint.setStyleSheet(f"color: {self._muted}; padding: 6px;")
        self._list_layout.addWidget(hint)

    def _make_row(self, idx: int, info: FileInfo) -> _FileRow:
        row = _FileRow(self)
        icon = file_icon_for_path(info[0], QColor(self._fg))
        row.set_data(icon, info[1], info[2],
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

    def _pick(self, info: FileInfo):
        self.close()
        self._on_open(info[0])

    # ---- события ----

    def showEvent(self, event):
        super().showEvent(event)
        self._search.setFocus()
        # Выделяем весь восстановленный запрос: ввод сразу заменяет его
        self._search.selectAll()

    def eventFilter(self, obj, event):
        # self._scroll создаётся уже после строки поиска, поэтому к нему
        # обращаемся только если он есть: иначе событие (например Polish)
        # от addWidget попадёт в переопределённый фильтр раньше времени и
        # PyQt уронит процесс без traceback
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
