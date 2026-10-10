"""
FindBar — поиск по текущему файлу (Ctrl+F).

Встраивается в layout EditorPane над вкладками: пока скрыта, редактор
занимает всю панель; при открытии панель поиска занимает место сверху.
Целевой редактор задаётся через set_editor() при переключении вкладок.
Все совпадения подсвечиваются индикатором, текущее — выделением Scintilla.
Поиск идёт через SCI_SEARCHINTARGET (байтовые позиции UTF-8).
"""

from PyQt6 import sip
from PyQt6.QtCore import Qt, QTimer
from PyQt6.QtGui import QColor
from PyQt6.QtWidgets import (
    QHBoxLayout, QLabel, QLineEdit, QToolButton, QWidget,
)

from editor.scintilla_constants import (
    SCI_GETCURRENTPOS, SCI_GETLENGTH, SCI_GETTARGETEND, SCI_GETTARGETSTART,
    SCI_INDICATORCLEARRANGE, SCI_INDICATORFILLRANGE, SCI_INDICSETALPHA,
    SCI_INDICSETFORE, SCI_INDICSETSTYLE, SCI_INDICSETUNDER, SCI_SCROLLCARET,
    SCI_SEARCHINTARGET, SCI_SETINDICATORCURRENT, SCI_SETSEARCHFLAGS,
    SCI_SETSEL, SCI_SETTARGETEND, SCI_SETTARGETSTART,
)
from editor.theme_utils import ThemeColors
from editor.text_utils import rgb_int

# Флаги SCFIND и стиль индикатора (не SCI-команды — держим здесь)
SCFIND_WHOLEWORD = 2
SCFIND_MATCHCASE = 4
SCFIND_REGEXP = 0x00200000
INDIC_ROUNDBOX = 7
INDICATOR_ID = 1  # не 0: нулевой индикатор может занимать подсветка скобок


class _SearchEdit(QLineEdit):
    """Поле поиска: Esc — закрыть, Enter/Shift+Enter — далее/назад,
    стрелки вверх/вниз — навигация по совпадениям."""

    def __init__(self, bar):
        super().__init__(bar)
        self._bar = bar
        self.setPlaceholderText("Find")

    def keyPressEvent(self, event):
        key = event.key()
        if key == Qt.Key.Key_Escape:
            self._bar.close_bar()
            return
        if key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            if event.modifiers() & Qt.KeyboardModifier.ShiftModifier:
                self._bar.find_previous()
            else:
                self._bar.find_next()
            return
        if key == Qt.Key.Key_Up:
            self._bar.find_previous()
            return
        if key == Qt.Key.Key_Down:
            self._bar.find_next()
            return
        super().keyPressEvent(event)


class FindBar(QWidget):
    """Панель поиска в файле: ввод, счётчик, кнопки, опции.

    Живёт в контейнере вкладки (_EditorHost) над редактором и
    встроена в его layout, а не является оверлеем.
    """

    def __init__(self, editor, parent=None):
        super().__init__(parent)
        self._editor = editor
        self._theme_manager = editor.theme_manager
        self._matches = []       # [(byte_pos, byte_len), ...]
        self._current = -1
        self._theme = None
        self.setObjectName("findBar")
        self.setFixedHeight(38)
        # Без WA_StyledBackground обычный QWidget игнорирует background-color
        # из QSS (фон рисуют только нативные виджеты)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self._setup_indicators()
        # Пересчёт при изменении текста файла (ввод, перезагрузка с диска)
        editor.textChanged.connect(self._schedule_search)

        self._search = _SearchEdit(self)
        self._search.setMinimumWidth(120)  # растягивается на всю свободную ширину
        self._search.setClearButtonEnabled(True)

        self._count = QLabel(self)

        self._btn_prev = QToolButton(self)
        self._btn_prev.setText("↑")
        self._btn_prev.setToolTip("Previous (Shift+Enter)")
        self._btn_next = QToolButton(self)
        self._btn_next.setText("↓")
        self._btn_next.setToolTip("Next (Enter)")
        self._btn_close = QToolButton(self)
        self._btn_close.setText("×")
        self._btn_close.setToolTip("Close (Esc)")

        self._btn_case = QToolButton(self)
        self._btn_case.setText("Aa")
        self._btn_case.setCheckable(True)
        self._btn_case.setToolTip("Match Case")
        self._btn_word = QToolButton(self)
        self._btn_word.setText("ab")
        self._btn_word.setCheckable(True)
        self._btn_word.setToolTip("Whole Word")
        self._btn_regex = QToolButton(self)
        self._btn_regex.setText(".*")
        self._btn_regex.setCheckable(True)
        self._btn_regex.setToolTip("Regex")

        # Компактные квадратные кнопки — иначе QToolButton тянется по
        # тексту и съедает ширину поля поиска
        for btn in (self._btn_prev, self._btn_next, self._btn_close,
                    self._btn_case, self._btn_word, self._btn_regex):
            btn.setFixedSize(26, 26)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 4, 8, 4)
        layout.setSpacing(4)
        layout.addWidget(self._search, 1)  # stretch: поле заполняет область справа
        layout.addWidget(self._count)
        layout.addSpacing(6)
        layout.addWidget(self._btn_case)
        layout.addWidget(self._btn_word)
        layout.addWidget(self._btn_regex)
        layout.addSpacing(6)
        layout.addWidget(self._btn_prev)
        layout.addWidget(self._btn_next)
        layout.addWidget(self._btn_close)

        self._btn_prev.clicked.connect(self.find_previous)
        self._btn_next.clicked.connect(self.find_next)
        self._btn_close.clicked.connect(self.close_bar)
        for btn in (self._btn_case, self._btn_word, self._btn_regex):
            btn.toggled.connect(self._schedule_search)

        # Пересчёт при изменении запроса
        self._search.textChanged.connect(self._schedule_search)

        # Дебаунс: ищем после паузы ввода, а не на каждый символ
        self._debounce = QTimer(self)
        self._debounce.setSingleShot(True)
        self._debounce.timeout.connect(self._run_search)

    def _setup_indicators(self):
        """Настраивает индикатор подсветки на редакторе."""
        self._editor.SendScintilla(SCI_INDICSETSTYLE, INDICATOR_ID, INDIC_ROUNDBOX)
        self._editor.SendScintilla(SCI_INDICSETUNDER, INDICATOR_ID, 1)
        self._editor.SendScintilla(SCI_INDICSETALPHA, INDICATOR_ID, 90)
        self._editor.SendScintilla(SCI_SETINDICATORCURRENT, INDICATOR_ID)

    # --- показать / скрыть ---

    def open(self):
        """Открывает панель, подставляя выделенный текст (если есть)."""
        if self._editor.hasSelectedText():
            self._search.setText(self._editor.selectedText())
        self._search.selectAll()
        self.show()
        self._search.setFocus()
        self._schedule_search()

    def close_bar(self):
        self.hide()
        if self._editor is not None and not sip.isdeleted(self._editor):
            self._editor.setFocus()

    def showEvent(self, event):
        super().showEvent(event)
        # После возврата на вкладку (или повторного открытия) — пересчёт
        self._schedule_search()

    def hideEvent(self, event):
        super().hideEvent(event)
        # При закрытии вкладки/окна редактор может быть уже уничтожен,
        # а hideEvent панели ещё срабатывает — не трогаем мёртвый объект
        if self._editor is not None and not sip.isdeleted(self._editor):
            self._clear_highlights()

    # --- поиск ---

    def _flags(self) -> int:
        flags = 0
        if self._btn_case.isChecked():
            flags |= SCFIND_MATCHCASE
        if self._btn_word.isChecked():
            flags |= SCFIND_WHOLEWORD
        if self._btn_regex.isChecked():
            flags |= SCFIND_REGEXP
        return flags

    def _schedule_search(self, *_):
        if self.isVisible() and self._editor is not None:
            self._debounce.start(120)

    def _clear_highlights(self):
        if self._editor is None or sip.isdeleted(self._editor):
            return
        self._editor.SendScintilla(SCI_SETINDICATORCURRENT, INDICATOR_ID)
        for pos, length in self._matches:
            self._editor.SendScintilla(SCI_INDICATORCLEARRANGE, pos, length)

    def _run_search(self):
        if not self.isVisible() or self._editor is None:
            return
        editor = self._editor
        query = self._search.text()
        self._clear_highlights()
        self._matches = []
        self._current = -1
        if not query:
            self._count.setText("")
            return
        try:
            qbytes = query.encode("utf-8")
        except UnicodeEncodeError:
            self._count.setText("")
            return
        qlen = len(qbytes)
        editor.SendScintilla(SCI_SETINDICATORCURRENT, INDICATOR_ID)
        editor.SendScintilla(SCI_SETSEARCHFLAGS, self._flags())
        doc_len = editor.SendScintilla(SCI_GETLENGTH)
        pos = 0
        while pos < doc_len:
            editor.SendScintilla(SCI_SETTARGETSTART, pos)
            editor.SendScintilla(SCI_SETTARGETEND, doc_len)
            found = editor.SendScintilla(SCI_SEARCHINTARGET, qlen, qbytes)
            if found < 0:
                break
            # Диапазон совпадения берём из таргета: для regex его длина
            # отличается от длины паттерна
            start = editor.SendScintilla(SCI_GETTARGETSTART)
            mlen = editor.SendScintilla(SCI_GETTARGETEND) - start
            self._matches.append((start, mlen))
            editor.SendScintilla(SCI_INDICATORFILLRANGE, start, mlen)
            pos = start + max(mlen, 1)  # защита от пустых regex-совпадений
        cursor = editor.SendScintilla(SCI_GETCURRENTPOS)
        self._current = next(
            (i for i, (p, l) in enumerate(self._matches) if p + l > cursor),
            0,
        ) if self._matches else -1
        self._update_label()

    def _update_label(self):
        n = len(self._matches)
        if n == 0:
            self._count.setText("no matches" if self._search.text() else "")
        else:
            self._count.setText(f"{self._current + 1}/{n}")

    # --- навигация ---

    def find_next(self):
        if not self._matches:
            self._run_search()
        if not self._matches:
            return
        self._current = (self._current + 1) % len(self._matches)
        self._goto_current()

    def find_previous(self):
        if not self._matches:
            self._run_search()
        if not self._matches:
            return
        if self._current < 0:
            self._current = 0
        self._current = (self._current - 1) % len(self._matches)
        self._goto_current()

    def _goto_current(self):
        pos, length = self._matches[self._current]
        editor = self._editor
        editor.SendScintilla(SCI_SETSEL, pos, pos + length)
        editor.SendScintilla(SCI_SCROLLCARET)
        self._search.setFocus()
        self._update_label()

    # --- тема ---

    def apply_theme(self, theme_name: str):
        self._theme = theme_name
        if self._editor is None or sip.isdeleted(self._editor):
            return
        manager = self._theme_manager or self._editor.theme_manager
        tc = ThemeColors.from_theme_manager(manager, theme_name)
        bg = tc.editor_bg_color.name()  # тот же фон, что у редактора
        fg = tc.fg
        border = tc.border
        muted = tc.muted
        input_bg = tc.editor_bg_color.name()
        accent = tc.selection or fg
        fg_c = QColor(fg)
        hover = f"rgba({fg_c.red()},{fg_c.green()},{fg_c.blue()},32)"
        checked = f"rgba({fg_c.red()},{fg_c.green()},{fg_c.blue()},70)"

        acc = QColor(accent)
        fore_int = rgb_int(acc)
        self._editor.SendScintilla(SCI_INDICSETFORE, INDICATOR_ID, fore_int)

        self.setStyleSheet(f"""
            QWidget#findBar {{
                background-color: {bg};
            }}
            QLineEdit {{
                background-color: {input_bg};
                color: {fg};
                border: 0.5px solid {border};
                border-radius: 4px;
                padding: 2px 6px;
            }}
            QLineEdit:focus {{ border-color: {accent}; }}
            QToolButton {{
                color: {fg};
                background: transparent;
                border: none;
                border-radius: 4px;
                padding: 2px 0;
            }}
            QToolButton:hover {{ background-color: {hover}; }}
            QToolButton:pressed {{ background-color: {hover}; }}
            QToolButton:checked {{ background-color: {checked}; }}
            QLabel {{ color: {muted}; background: transparent; }}
        """)
