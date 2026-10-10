"""
CodeEditor — редактор кода на базе QScintilla.
"""

import os
import re
from typing import List, Optional, Tuple

from PyQt6.QtCore import (Qt, QEvent, QPoint, QPointF, QTimer,
                        QVariantAnimation, QEasingCurve)
from PyQt6.QtGui import QFont, QFontMetrics, QColor, QMouseEvent, QPalette
from PyQt6.QtWidgets import QTabWidget, QFrame, QWidget, QMessageBox
from PyQt6.Qsci import QsciScintilla, QsciLexer

from editor.constants import (
    CODE_FONT_SIZE, DEFAULT_TAB_WIDTH, CARET_WIDTH, MARGIN_PADDING,
    STYLE_RANGE, STYLE_DEFAULT, MAX_LEXER_STYLES, FOLD_ICON_SIZE,
    SCROLLBAR_HIDE_DELAY_MS, SCROLLBAR_FADE_MS,
)
from editor.font_settings import code_font_family
from editor.scintilla_constants import (
    SCI_ASSIGNCMDKEY, SCI_SETHSCROLLBAR, SCI_SETVSCROLLBAR,
    SCI_STYLESETBACK, SCI_STYLESETFORE, SCI_TEXTHEIGHT, SCI_GETSTYLEAT,
    SCI_LINEFROMPOSITION, SCI_POSITIONFROMLINE, SCI_GETLINEENDPOSITION,
    SCI_POINTYFROMPOSITION,
    SCI_COLOURISE,
    SCI_GETFOLDLEVEL, SCI_FOLDLINE, SCI_FOLDALL,
    SC_FOLDACTION_CONTRACT, SC_FOLDLEVELBASE,
    SC_FOLDLEVELHEADERFLAG, SC_FOLDLEVELNUMBERMASK,
    SCI_WORDLEFT, SCI_WORDRIGHT, SCI_WORDLEFTEXTEND, SCI_WORDRIGHTEXTEND,
    SCI_INDICSETSTYLE, INDIC_HIDDEN, SCI_STYLESETUNDERLINE,
    SCI_SETMARGINMASKN,
)
from editor.semantic_lexer import SemanticLexer
from editor.lexers.lumen.lumen_lang import LumenLangLexer
from editor.settings_store import app_settings
from editor.theme_utils import ThemeColors

# Ступени градации альфы бегунка: 192 значений достаточно, а каждый
# лишний setStyleSheet — переключение стиля полосы на Windows.
_SB_ALPHA_STEPS = 12
from editor.text_utils import rgb_int
from editor.widgets.qt_utils import (
    find_ancestor,
    find_tab_widget as _find_tab_widget,
)
from editor.scrollbar_qss import scrollbar_qss
from editor.theme_manager import ThemeManager
from editor.lexer_manager import LexerManager
from editor.completion import CompletionEngine
from editor.fs_watcher import SharedFsWatcher
from editor.lsp.bridge import LspEditorBridge
from editor.lsp.manager import get_lsp_manager


# Автозакрытие скобок: открывающая -> закрывающая пара
_BRACKET_PAIRS = {"(": ")", "[": "]", "{": "}"}

# Комментарий строки для стандартных лексеров QScintilla (Ctrl+/);
# кастомные лексеры отдают префикс сами (LINE_COMMENT / line_comment)
_LINE_COMMENT_BY_LANG = {
    "python": "#", "ruby": "#", "bash": "#", "yaml": "#",
    "lua": "--", "sql": "--", "lisp": ";",
    "rust": "//",
}
_NO_LINE_COMMENT_LANGS = {"xml", "html", "markdown", "json"}


def _is_line_commented(line: str, prefix: str) -> bool:
    """Строка закомментирована: префикс с пробелом или ровно префикс."""
    return line.startswith(prefix + " ") or line == prefix


def _strip_line_comment(line: str, prefix: str) -> str:
    """Снимает префикс комментария (с пробелом, если он был)."""
    if line.startswith(prefix + " "):
        return line[len(prefix) + 1:]
    if line.startswith(prefix):
        return line[len(prefix):]
    return line


def _remap_mouse_event(event, local_pos: QPointF) -> QMouseEvent:
    """Копия события мыши с другой локальной позицией (для inlay-сдвига)."""
    return QMouseEvent(event.type(), local_pos, event.globalPosition(),
                       event.button(), event.buttons(), event.modifiers())


class CodeEditor(QsciScintilla):
    """Редактор кода на QScintilla с поддержкой тем и лексеров."""

    def __init__(self, lexer_manager: LexerManager, theme_manager: ThemeManager,
                 parent=None):
        super().__init__(parent)
        self.lexer_manager = lexer_manager
        self.theme_manager = theme_manager
        self.file_path: Optional[str] = None
        self.language_id: Optional[str] = None
        self._current_lexer: Optional[QsciLexer] = None
        self._margin_font: QFont = QFont(code_font_family(), 11)

        self.setUtf8(True)
        self.setEolMode(QsciScintilla.EolMode.EolUnix)
        self.setIndentationsUseTabs(False)
        self.setTabWidth(DEFAULT_TAB_WIDTH)
        self.setIndentationGuides(True)
        self.setWhitespaceVisibility(
            QsciScintilla.WhitespaceVisibility.WsVisible
        )
        # Размер точек-пробелов в px — 2
        self.setWhitespaceSize(2)
        self.setAutoIndent(True)
        self.setCaretLineVisible(False)
        self.setCaretWidth(CARET_WIDTH)
        self.SendScintilla(self.SCI_SETCARETSTYLE, self.CARETSTYLE_BLOCK)
        # Подсветка парных скобок, когда курсор стоит вплотную к скобке
        # (цвета задаются в теме: theme_manager.apply_theme). StrictBraceMatch
        # не матчит скобки внутри комментариев/строк: матч идёт только для
        # символов в стиле скобок (braceStyle лексера).
        self.setBraceMatching(QsciScintilla.BraceMatch.StrictBraceMatch)

        self._current_theme: Optional[str] = None

        # --- Bookmarks ---
        # В этой сборке QScintilla символьные маркеры НЕ рисуются в маргине 0
        # (он зарезервирован под номера строк), поэтому реальный bookmark-
        # маргин — индекс 1 (первый рабочий symbol-маргин). Чтобы закладка была
        # левее номеров строк, номера ставим в маргин 2, fold — в маргин 3,
        # а маргин 0 оставляем пустым.
        self._BOOKMARK_MARGIN = 1
        self._BOOKMARK_MARKER = 8          # твёрдая закладка
        self._BOOKMARK_GHOST_MARKER = 9    # блеклый призрак при наведении
        # Размер иконки закладки = _font_size * этот коэффициент (пропорционально
        # шрифту). Центрирование обеспечивает сам Scintilla: маркер в ячейке
        # строки центрируется как (cellHeight - pixmapHeight)/2, а в маргине как
        # (marginWidth - pixmapWidth)/2, поэтому при квадратном pixmap и ширине
        # маргина, зависящей от иконки, центр всегда точный при любом зуме.
        self._BOOKMARK_ICON_SCALE = 1.4
        self._bookmarks = {}  # marker handle -> текст заметки
        self._ghost_handle = -1            # handle призрака (или -1)
        self._ghost_line = -1              # строка, где сейчас призрак
        self._bookmark_tooltip = None      # кастомная плашка-подсказка (или None)
        self._bookmark_tooltip_scroll_connected = False
        self._bookmark_fg = QColor(255, 255, 255)
        self._bookmark_bg = QColor(128, 128, 128)  # для затухания призрака
        self.setMarginWidth(0, 0)  # маргин 0 не используется
        self.setMarginType(self._BOOKMARK_MARGIN,
                           QsciScintilla.MarginType.SymbolMargin)
        self.setMarginSensitivity(self._BOOKMARK_MARGIN, True)
        self.setMarginWidth(self._BOOKMARK_MARGIN, 0)  # ширина в apply_theme
        self.SendScintilla(SCI_SETMARGINMASKN, self._BOOKMARK_MARGIN,
                           (1 << self._BOOKMARK_MARKER)
                           | (1 << self._BOOKMARK_GHOST_MARKER))
        self.marginClicked.connect(self._on_bookmark_margin_clicked)
        try:
            self.marginRightClicked.connect(
                self._on_bookmark_margin_right_clicked)
        except AttributeError:
            pass

        # Номера строк (margin 2) — правее закладок
        self.setMarginType(2, QsciScintilla.MarginType.NumberMargin)
        self.setMarginLineNumbers(2, True)
        # Fold-маргин (margin 3)
        self.setFolding(QsciScintilla.FoldStyle.CircledTreeFoldStyle, 3)
        self.setMarginWidth(3, FOLD_ICON_SIZE)  # фиксируем ширину fold-маржина под иконки

        # QSS контекстного меню (правая кнопка) — попапы не наследуют
        # стиль главного окна, собираем из темы в apply_theme
        self._menu_qss = ""

        self.SendScintilla(QsciScintilla.SCI_SETMULTIPLESELECTION, True)
        self.SendScintilla(QsciScintilla.SCI_SETMULTIPASTE, 1)
        self.SendScintilla(QsciScintilla.SCI_SETADDITIONALSELECTIONTYPING, True)

        # Force QScrollBar instead of Scintilla native scrollbars
        self.SendScintilla(SCI_SETVSCROLLBAR, 0)
        self.SendScintilla(SCI_SETHSCROLLBAR, 0)
        self.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)

        # Автоскрытие полос при простое. Политика остаётся AsNeeded —
        # скрытие делается прозрачным QSS, иначе вьюпорт сужается и
        # текст переверстывается (reflow) при каждомhide.
        self._sb_shown = True
        self._sb_on_scrollbar = False
        self._sb_bg = ""
        self._sb_fg = QColor(128, 128, 128)
        self._sb_alpha = 1.0
        self._sb_applied = 1.0
        self._sb_anim = QVariantAnimation(self)
        self._sb_anim.setStartValue(1.0)
        self._sb_anim.setEndValue(1.0)
        self._sb_anim.setDuration(SCROLLBAR_FADE_MS)
        self._sb_anim.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._sb_anim.valueChanged.connect(self._on_sb_alpha)
        self._sb_hide_timer = QTimer(self)
        self._sb_hide_timer.setSingleShot(True)
        self._sb_hide_timer.setInterval(
            app_settings().get_int("editor.scrollbar_hide_delay",
                                   SCROLLBAR_HIDE_DELAY_MS))
        self._sb_hide_timer.timeout.connect(self._hide_scrollbars)
        self._sb_hide_timer.start()
        for _bar in (self.verticalScrollBar(), self.horizontalScrollBar()):
            if _bar is not None:
                _bar.installEventFilter(self)
                _bar.valueChanged.connect(self._scrollbar_activity)

        # Отключаем встроенные шорткаты Scintilla для зума
        # (SCI_CTRL = 2 в старших 16 битах команды)
        self.SendScintilla(SCI_ASSIGNCMDKEY, (2 << 16) | ord('-'), 0)
        self.SendScintilla(SCI_ASSIGNCMDKEY, (2 << 16) | ord('='), 0)
        self.SendScintilla(SCI_ASSIGNCMDKEY, (2 << 16) | ord('0'), 0)

        # Scintilla по умолчанию ставит INDIC_PLAIN на индикаторах 0-9 —
        # они рисуют серый underline под всем текстом. Отключаем все,
        # underline задаётся шрифтом лексера, а используемые индикаторы
        # (find bar, snippets, LSP go-to) настраиваются явно.
        for _ind in range(32):
            self.SendScintilla(SCI_INDICSETSTYLE, _ind, INDIC_HIDDEN)

        self._font_size = CODE_FONT_SIZE

        self._default_font = QFont(code_font_family(), CODE_FONT_SIZE)
        self._default_font.setFixedPitch(True)
        self.setFont(self._default_font)
        self._margin_font = QFont(code_font_family(), CODE_FONT_SIZE)
        self._margin_font.setFixedPitch(True)
        self.setMarginsFont(self._margin_font)
        self.update_margin_width()

        self.textChanged.connect(self._on_text_changed)
        self.modificationChanged.connect(self._on_modification_changed)

        # Автодополнение (подсказки для .lm файлов, LSP-дополнения для C/C++)
        self._completion = CompletionEngine(self)

        # --- LSP (clangd): диагностика, hover, переход к определению ---
        # Вся clangd-механика вынесена в отдельный контроллер
        self._lsp_bridge = LspEditorBridge(self)
        self.viewport().setMouseTracking(True)
        self.viewport().installEventFilter(self)

        # Слежение за файлом на диске: внешние изменения (другой редактор,
        # git, сборка) перечитываются во вкладку автоматически
        self._fs_watcher = SharedFsWatcher(self)
        self._fs_watcher.fileChanged.connect(self._on_file_changed_externally)

        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

        # Убираем внутреннюю рамку QScintilla (QFrame),
        # чтобы не было двойной обводки с QTabWidget::pane
        self.setFrameShape(QFrame.Shape.NoFrame)

    # --- focus / keyboard ---

    def keyPressEvent(self, event):
        self._scrollbar_activity()
        # Хендлеры по очереди; первый вернувший True съедает событие
        if self._handle_word_move(event):
            return
        if self._handle_completion_popup(event):
            return
        if self._handle_snippet_keys(event):
            return
        if self._handle_zoom_block(event):
            return
        if self._handle_comment_toggle(event):
            return
        if self._handle_typing_auto_close(event):
            return
        super().keyPressEvent(event)
        # После изменения текста обновляем подсказку автодополнения
        if self._completion.should_refresh(event):
            self._completion.after_edit()

    def _handle_word_move(self, event) -> bool:
        """Alt+Left/Alt+Right — переход по словам (Ctrl+Left/Right отданы
        переключению вкладок). Alt+Shift+Left/Right — с выделением."""
        key = event.key()
        if key not in (Qt.Key.Key_Left, Qt.Key.Key_Right):
            return False
        mods = event.modifiers()
        if not (mods & Qt.KeyboardModifier.AltModifier):
            return False
        extra = mods & ~(Qt.KeyboardModifier.AltModifier
                         | Qt.KeyboardModifier.ShiftModifier)
        if extra:
            return False
        forward = key == Qt.Key.Key_Right
        if mods & Qt.KeyboardModifier.ShiftModifier:
            cmd = SCI_WORDRIGHTEXTEND if forward else SCI_WORDLEFTEXTEND
        else:
            cmd = SCI_WORDRIGHT if forward else SCI_WORDLEFT
        self.SendScintilla(cmd)
        return True

    def _handle_completion_popup(self, event) -> bool:
        """LSP-обработка и клавиши открытого списка автодополнения."""
        # Всегда первым: LSP-слой слушает нажатия независимо от попапа
        if self._lsp_bridge.on_key_press(event):
            return True
        if not ((self._completion.enabled or self._completion.lsp_enabled)
                and self._completion.popup_visible()):
            return False
        key = event.key()
        if key == Qt.Key.Key_Down:
            self._completion.popup_next(1)
            return True
        if key == Qt.Key.Key_Up:
            self._completion.popup_next(-1)
            return True
        if key in (Qt.Key.Key_Return, Qt.Key.Key_Enter,
                   Qt.Key.Key_Tab):
            self._completion.accept_popup()
            return True
        if key == Qt.Key.Key_Escape:
            self._completion.hide_popup()
            return True
        return False

    def _handle_snippet_keys(self, event) -> bool:
        """Esc завершает сеанс сниппета, Tab/Shift+Tab ходят по
        плейсхолдерам (если список автодополнения закрыт)."""
        if not (self._completion.enabled or self._completion.lsp_enabled):
            return False
        key = event.key()
        if key == Qt.Key.Key_Escape:
            self._completion.clear_placeholders()
            return False  # Esc не съедаем — пусть уходит в базовый класс
        if (key in (Qt.Key.Key_Tab, Qt.Key.Key_Backtab)
                and not self._completion.popup_visible()
                and self._completion.handle_tab(key == Qt.Key.Key_Backtab)):
            return True
        return False

    def _handle_zoom_block(self, event) -> bool:
        """Блокируем встроенный зум Scintilla через клавиатуру."""
        if (event.modifiers() & Qt.KeyboardModifier.ControlModifier
                and event.key() in (Qt.Key.Key_Minus, Qt.Key.Key_Equal,
                                    Qt.Key.Key_0)):
            event.ignore()
            return True
        return False

    def _handle_comment_toggle(self, event) -> bool:
        """Ctrl+/ — закомментировать/раскомментировать строки выделения.

        На русской раскладке клавиша '/' отдаёт Key_Period — ловим обе.
        """
        if (event.key() in (Qt.Key.Key_Slash, Qt.Key.Key_Period)
                and event.modifiers() & Qt.KeyboardModifier.ControlModifier
                and not event.modifiers() & Qt.KeyboardModifier.AltModifier
                and not event.modifiers() & Qt.KeyboardModifier.MetaModifier):
            self._toggle_line_comments()
            return True
        return False

    def _line_comment_prefix(self) -> Optional[str]:
        """Префикс строкового комментария для текущего лексера (None — нет)."""
        lexer = self._current_lexer
        if isinstance(lexer, LumenLangLexer):
            return LumenLangLexer.LINE_COMMENT
        if isinstance(lexer, SemanticLexer):
            return lexer.line_comment or "//"
        lang = self.language_id or ""
        if lang in _NO_LINE_COMMENT_LANGS:
            return None
        return _LINE_COMMENT_BY_LANG.get(lang, "//")

    def _toggle_line_comments(self):
        """Ctrl+/: комментирование/раскомментирование строк выделения.

        Без выделения работает по строке под курсором. Если все строки
        уже закомментированы — префикс снимается, иначе добавляется.
        """
        prefix = self._line_comment_prefix()
        if not prefix:
            return
        line_from, idx_from, line_to, idx_to = self.getSelection()
        has_sel = self.hasSelectedText()
        if not has_sel:
            line_from = line_to = self.getCursorPosition()[0]
            idx_from = self.getCursorPosition()[1]
        elif idx_to == 0 and line_to > line_from:
            # Выделение заканчивается в начале последней строки (после
            # нормализации QScintilla) — эта строка фактически не выбрана
            line_to -= 1
        # text(line)/lineLength возвращают строку вместе с переводом \n
        lines = [self.text(i).rstrip("\n")
                 for i in range(line_from, line_to + 1)]
        # Раскомментируем только если все строки диапазона уже закомментированы
        if all(_is_line_commented(l, prefix) for l in lines):
            new_lines = [_strip_line_comment(l, prefix) for l in lines]
        else:
            # Комментарий с пробелом: "// ..."; пустая строка — просто префикс
            new_lines = [prefix + " " + l if l else prefix for l in lines]
        delta = len(new_lines[0]) - len(lines[0])
        # Заменяем блок строк целиком (индексы QScintilla — байтовые)
        last_end = len(lines[-1].encode("utf-8"))
        self.setSelection(line_from, 0, line_to, last_end)
        self.replaceSelectedText("\n".join(new_lines))
        self._completion.after_edit()
        if has_sel:
            # Восстанавливаем выделение на изменённый блок
            new_last = len(new_lines[-1].encode("utf-8"))
            self.setSelection(line_from, 0,
                              line_from + len(new_lines) - 1, new_last)
        else:
            # Курсор остаётся на той же строке, сдвиг на префикс
            max_idx = len(new_lines[0].encode("utf-8"))
            self.setCursorPosition(line_from, min(idx_from + delta, max_idx))

    def _handle_typing_auto_close(self, event) -> bool:
        """Автозакрытие скобок/кавычек, блок-комментария, backspace пары.

        Чистый Ctrl/Alt/Meta пропускаем (на Ctrl+Alt жмутся AltGr-символы
        вроде [ ] { } на русской раскладке — их разрешаем).
        """
        mods = event.modifiers()
        plain_ctrl = (mods & Qt.KeyboardModifier.ControlModifier
                      and not mods & Qt.KeyboardModifier.AltModifier)
        plain_alt = (mods & Qt.KeyboardModifier.AltModifier
                     and not mods & Qt.KeyboardModifier.ControlModifier)
        if plain_ctrl or plain_alt or mods & Qt.KeyboardModifier.MetaModifier:
            return False
        text = event.text()
        if len(text) == 1:
            if text in ('"', "'"):
                # автозакрытие кавычек: перешагивание пары, вставка
                # пары; внутри строки/комментария — одиночный символ
                if self._skip_closing_bracket(text):
                    self._completion.after_edit()
                    return True
                if self._in_string_or_comment():
                    self.insert(text)
                    self._completion.after_edit()
                    return True
                self._insert_quote_pair(text)
                self._completion.after_edit()
                return True
            if text in _BRACKET_PAIRS:
                self._insert_bracket_pair(text)
                self._completion.after_edit()
                return True
            if text == '*' and self._is_after_slash():
                self._insert_block_comment_start()
                self._completion.after_edit()
                return True
            if (text in _BRACKET_PAIRS.values()
                    and self._skip_closing_bracket(text)):
                return True
        if (event.key() == Qt.Key.Key_Backspace
                and self._delete_empty_pair()):
            self._completion.after_edit()
            return True
        return False

    def keyReleaseEvent(self, event):
        super().keyReleaseEvent(event)
        self._lsp_bridge.on_key_release(event)

    # ---- автозакрытие скобок ----

    def _insert_bracket_pair(self, opening: str):
        """Вставляет открывающую скобку и её пару, курсор — внутри."""
        closing = _BRACKET_PAIRS[opening]
        if self.hasSelectedText():
            line_from, idx_from, line_to, idx_to = self.getSelection()
            sel = self.selectedText()
            self.replaceSelectedText(opening + sel + closing)
            # Выделяем обёрнутый текст (сдвиг на вставленную скобку)
            self.setSelection(line_from, idx_from + 1, line_to, idx_to + 1)
        else:
            # insert() не двигает курсор — позицию запоминаем до вставки
            line, idx = self.getCursorPosition()
            self.insert(opening + closing)
            self.setCursorPosition(line, idx + 1)

    def _is_after_slash(self) -> bool:
        """Предыдущий символ — '/' и мы не внутри строки/комментария."""
        line, idx = self.getCursorPosition()
        if idx <= 0:
            return False
        text_line = self.text(line)
        if idx > len(text_line) or text_line[idx - 1] != '/':
            return False
        return not self._in_string_or_comment()

    def _in_string_or_comment(self) -> bool:
        """Курсор внутри строки/символа/комментария (по стилю лексера)."""
        line, idx = self.getCursorPosition()
        if idx <= 0:
            return False
        text_line = self.text(line)
        if idx > len(text_line):
            return False
        lexer = self._current_lexer
        if lexer is None:
            return False
        # Scintilla стилизует строки лениво (по отрисовке): после setText/
        # вставки стили могут быть устаревшими — форсируем перестилизацию
        # до позиции курсора (хвост строки на стиль под курсором не влияет)
        line_start = self.SendScintilla(SCI_POSITIONFROMLINE, line)
        pos = self.positionFromLineIndex(line, idx)
        if pos > line_start:
            self.SendScintilla(SCI_COLOURISE, line_start, pos)
        style = self.SendScintilla(SCI_GETSTYLEAT, pos - 1)
        desc = lexer.description(style).lower()
        return ("string" in desc or "comment" in desc
                or "character" in desc)

    def _insert_quote_pair(self, quote: str):
        """Автозакрытие кавычки: пара ''/"" с курсором внутри.

        Внутри строки/комментария пару не вставляем — иначе печать
        кавычки внутри строкового литерала сломает его.
        """
        if self.hasSelectedText():
            line_from, idx_from, line_to, idx_to = self.getSelection()
            sel = self.selectedText()
            self.replaceSelectedText(quote + sel + quote)
            # Выделяем обёрнутый текст (сдвиг на вставленную кавычку)
            self.setSelection(line_from, idx_from + 1, line_to, idx_to + 1)
        else:
            # insert() не двигает курсор — позицию запоминаем до вставки
            line, idx = self.getCursorPosition()
            self.insert(quote + quote)
            self.setCursorPosition(line, idx + 1)

    def _insert_block_comment_start(self):
        """После набранного '/' вставляет '*' + закрывающую '*/'."""
        line, idx = self.getCursorPosition()
        self.insert('**/')  # '*' (завершает /*) и сразу */
        self.setCursorPosition(line, idx + 1)

    def _skip_closing_bracket(self, closing: str) -> bool:
        """Если под курсором уже стоит такая же закрывающая — перешагиваем."""
        line, idx = self.getCursorPosition()
        text_line = self.text(line)
        if idx < len(text_line) and text_line[idx] == closing:
            self.setCursorPosition(line, idx + 1)
            return True
        return False

    def _delete_empty_pair(self) -> bool:
        """Backspace между пустой парой () [] {} — удаляет обе скобки."""
        line, idx = self.getCursorPosition()
        if idx <= 0:
            return False
        text_line = self.text(line)
        if idx >= len(text_line):
            return False
        opening = text_line[idx - 1]
        if ((opening in _BRACKET_PAIRS
                and _BRACKET_PAIRS[opening] == text_line[idx])
                or (opening in ('"', "'") and opening == text_line[idx])):
            self.setSelection(line, idx - 1, line, idx + 1)
            self.removeSelectedText()
            return True
        return False

    def focusInEvent(self, event):
        super().focusInEvent(event)
        parent = find_ancestor(self, lambda w: hasattr(w, "tile_manager"))
        if parent is not None:
            parent.tile_manager.active_editor = self

    def focusOutEvent(self, event):
        # Уход фокуса закрывает кастомный список автодополнения
        self._completion.hide_popup()
        self._lsp_bridge.on_focus_out()
        super().focusOutEvent(event)

    # --- margins ---

    def update_margin_width(self):
        lines = str(self.lines()) if self.lines() > 0 else "0"
        digits = len(lines) + MARGIN_PADDING
        fm = QFontMetrics(self._margin_font)
        pw = fm.horizontalAdvance("0" * digits)
        self.setMarginWidth(2, pw)

    def set_show_fold_margin(self, show: bool):
        """Показывать/прятать fold-маргин (иконки сворачивания).

        Только ширина: сама сворачиваемость (fold levels) сохраняется,
        так что переключение в Apply не пересобирает маркеры и не трогает
        уже свёрнутые блоки.
        """
        self.setMarginWidth(3, FOLD_ICON_SIZE if show else 0)

    def apply_fold_on_open(self):
        """Сворачивание при открытии файла (настройка editor.fold_on_open).

        Уровни фолдинга считает лексер во время стилизации (styleText), а
        она ленивая — форсируем SCI_COLOURISE, чтобы уровни были готовы,
        и только потом сворачиваем. Для plain-файлов без фолдинга уровней
        нет — цикл тихо выходит.
        """
        from editor.settings_store import app_settings
        mode = app_settings().get_str("editor.fold_on_open", "off")
        if mode == "off":
            return
        self.SendScintilla(SCI_COLOURISE, 0, -1)
        if mode == "all":
            self.SendScintilla(SCI_FOLDALL, SC_FOLDACTION_CONTRACT, 1)
        else:  # level1 — сворачиваем вложенные блоки, верхний уровень виден
            self._fold_to_level(1)

    def _fold_to_level(self, level: int):
        """Сворачивает блоки глубже level (0 = все, 1 = вложенные)."""
        base = SC_FOLDLEVELBASE
        header = SC_FOLDLEVELHEADERFLAG
        mask = SC_FOLDLEVELNUMBERMASK
        try:
            for line in range(self.lines()):
                lvl = self.SendScintilla(SCI_GETFOLDLEVEL, line)
                if lvl & header and (lvl & mask) - base >= level:
                    self.SendScintilla(SCI_FOLDLINE, line,
                                       SC_FOLDACTION_CONTRACT)
        except RuntimeError:
            pass  # редактор уничтожен

    # --- zoom через изменение размера шрифта ---

    def _apply_font_size(self):
        """Применяет _font_size ко всем шрифтам редактора."""
        self._lsp_bridge.hide_popup()
        size = max(self._font_size, 10)
        font = QFont(code_font_family(), size)
        font.setFixedPitch(True)
        self.setFont(font)
        if self._current_lexer:
            underline_set = getattr(self._current_lexer,
                                    "_underline_styles", set())
            for sid in range(128):
                if self._current_lexer.description(sid):
                    cur = self._current_lexer.font(sid)
                    f = QFont(code_font_family(), size)
                    f.setFixedPitch(True)
                    f.setBold(cur.bold())
                    f.setItalic(cur.italic())

                    f.setUnderline(cur.underline())
                    self._current_lexer.setFont(f, sid)

        self._margin_font = QFont(font)
        self._margin_font.setFixedPitch(True)
        self.setMarginsFont(self._margin_font)
        self.update_margin_width()
        self._rebuild_fold_icons()
        # Шрифт списка автодополнения тоже следует за зумом
        if self._current_theme:
            self._completion.apply_theme(self._current_theme)
        # ErrorLens-аннотации следуют за размером шрифта
        self._lsp_bridge.on_font_size_changed()
        # Иконка закладки жёстко привязана к размеру шрифта — пересобираем
        # её под новый размер при любом зуме.
        self._setup_bookmark_marker(self._bookmark_fg)

    def _rebuild_fold_icons(self):
        """Пересобирает иконки фолдинга под текущую тему и высоту строки."""
        if not self._current_theme:
            return
        tc = ThemeColors.from_theme_manager(
            self.theme_manager, self._current_theme
        )
        # SCI_TEXTHEIGHT: высота строки в пикселях
        line_height = self.SendScintilla(SCI_TEXTHEIGHT, 0)
        self.theme_manager.apply_fold_icons(
            self, tc.editor_bg_color, tc.fg_color, line_height
        )

    def zoomIn(self, *args):
        self._font_size += 1
        self._apply_font_size()

    def zoomOut(self, *args):
        self._font_size = max(self._font_size - 1, 10)
        self._apply_font_size()

    def zoomTo(self, zoom_level: int):
        self._font_size = max(CODE_FONT_SIZE + zoom_level, 10)
        self._apply_font_size()

    def wheelEvent(self, event):
        self._scrollbar_activity()
        # Ctrl+колесо — зум, иначе стандартная прокрутка
        if event.modifiers() & Qt.KeyboardModifier.ControlModifier:
            delta = event.angleDelta().y()
            if delta > 0:
                self.zoomIn()
            elif delta < 0:
                self.zoomOut()
        else:
            super().wheelEvent(event)

    def paintEvent(self, event):
        # ErrorLens: рисуем диагностики поверх кода в той же поверхности.
        # pre_paint прячет каретку Scintilla, если она лежит в сдвигаемой
        # inlay-области — иначе она рисуется на старом месте поверх оверлея.
        try:
            self._lsp_bridge.pre_paint()
        except RuntimeError:
            pass
        super().paintEvent(event)
        try:
            self._lsp_bridge.paint_error_lens()
        except RuntimeError:
            pass

    def _on_text_changed(self):
        self._lsp_bridge.on_text_changed()
        self.update_margin_width()
        # Пересборка динамических слов автодополнения (let/fn/struct/...)
        self._completion.schedule_rebuild()
        # Хлебные крошки: дерево символов устарело
        bar = getattr(self, "_breadcrumbs_bar", None)
        if bar is not None:
            bar.on_text_changed()

    def find_tab_widget(self) -> Optional[QTabWidget]:
        """Ищет QTabWidget, в котором живёт редактор (см. qt_utils)."""
        return _find_tab_widget(self)

    def _on_modification_changed(self, modified: bool):
        """Кружок несохранённых изменений рисует таб-бар.

        Место под кружок зарезервировано всегда, поэтому ширина таба не
        меняется — достаточно просто перерисовать таб-бар, чтобы кружок
        появился/исчез.
        """
        tab_widget = self.find_tab_widget()
        if tab_widget is not None:
            tab_bar = tab_widget.tabBar()
            if tab_bar is not None:
                tab_bar.update()

    # --- language / lexer ---

    def set_language(self, language_id: Optional[str]):
        self.language_id = language_id
        self._completion.set_language(language_id)
        if self._current_lexer:
            # setEditor есть только у кастомных лексеров (QsciLexerCustom)
            if hasattr(self._current_lexer, "setEditor"):
                self._current_lexer.setEditor(None)
            self._current_lexer.deleteLater()
            self._current_lexer = None

        if language_id is None:
            self.setLexer(None)
            self._completion.on_lexer_set(None)
            self._lsp_bridge.set_language(language_id)
            return

        # LSP-языки с семантическими токенами (cpp — clangd, python —
        # basedpyright и т.д.): стили даёт сервер, стандартный лексер выключен
        config = get_lsp_manager().config_for(language_id)
        if config is not None and config.semantic_tokens:
            lexer = SemanticLexer(
                self,
                keywords=config.keywords,
                type_keywords=config.type_keywords,
                line_comment=config.line_comment,
                block_comments=config.block_comments,
                triple_quotes=config.triple_quotes,
                language=language_id,
            )
            self._current_lexer = lexer
            self.setLexer(lexer)
            self._completion.on_lexer_set(lexer)
            # Шрифты один раз под _font_size; apply_theme (ниже или при
            # добавлении вкладки) красит только цвета — без повторных шрифтов
            self._apply_font_size()
            if self._current_theme:
                self.apply_theme(self._current_theme)
            self._lsp_bridge.set_language(language_id)
            return

        lexer = self.lexer_manager.get_lexer(language_id, self)
        self._current_lexer = lexer
        self.setLexer(lexer)
        self._completion.on_lexer_set(lexer)
        self._apply_font_size()
        self._lsp_bridge.set_language(language_id)

    # --- file I/O ---

    def _watch_file(self, path: Optional[str]):
        """Подписывает watcher на файл редактора (и только на него)."""
        watched = self._fs_watcher.files()
        if watched and watched != [path]:
            self._fs_watcher.removePaths(watched)
        if path and os.path.isfile(path):
            self._fs_watcher.addPath(path)

    def _on_file_changed_externally(self, path: str):
        """Файл изменился на диске — перечитываем вкладку.

        Несохранённые правки не затираем: если пользователь редактировал
        буфер, внешнее изменение игнорируется (сохранит своё при save).
        """
        if not self.file_path or path != self.file_path:
            return
        if not os.path.isfile(path):
            return
        if self.isModified():
            return
        try:
            with open(path, "r", encoding="utf-8") as f:
                new_text = f.read()
        except OSError:
            return
        if new_text == self.text():
            return
        # Сравниваем по логическому содержанию (LF-форма): своя же запись
        # через save_file кладёт на диск нормализованные переводы строк,
        # буфер же может держать смесь \n и \r\n — перечитывать файл при
        # этом не нужно (иначе сбросятся курсор и undo после каждого Save).
        if new_text == self.text().replace("\r\n", "\n").replace("\r", "\n"):
            return
        # Сохраняем позицию курсора и прокрутку
        cur_line, cur_idx = self.getCursorPosition()
        sb_value = self.verticalScrollBar().value()
        self.setText(new_text)
        self.setModified(False)
        self.setCursorPosition(cur_line, cur_idx)
        self.verticalScrollBar().setValue(sb_value)

    def load_file(self, path: str):
        self.file_path = path
        self._watch_file(path)
        try:
            with open(path, "r", encoding="utf-8") as f:
                self.setText(f.read())
        except Exception as e:
            self.setText(f"# Error loading file: {e}")

        self.setModified(False)
        lang_id = self.lexer_manager.get_language_id(path)
        self.set_language(lang_id)
        self.apply_fold_on_open()

    def _normalized_text(self) -> str:
        """Текст буфера с консистентными переводами строк (по EOL-режиму).

        QScintilla хранит загруженный текст с теми переводами, что были
        в файле, а Enter вставляет разделители по eolMode(). Итог — смесь
        \n и \r\n в буфере. Писать её в текстовом режиме нельзя: Python
        на Windows переведёт каждый \n в \r\n, и уже существующее \r\n
        станет \r\r\n (лишние пустые строки). Нормализуем всё к одному
        EOL и пишем с newline="" (без перевода).
        """
        text = self.text()
        if app_settings().get_bool("editor.trim_trailing_whitespace", False):
            text = re.sub(r"[ \t]+(?=\r\n|\n|\r|$)", "", text)
        if self.eolMode() == QsciScintilla.EolMode.EolWindows:
            return text.replace("\r\n", "\n").replace("\r", "\n")\
                       .replace("\n", "\r\n")
        return text.replace("\r\n", "\n").replace("\r", "\n")

    def save_file(self, path: Optional[str] = None) -> bool:
        old_path = self.file_path
        if path:
            self.file_path = path
        if not self.file_path:
            return False
        # Сохраняем BOM, если он присутствовал у файла (иначе пишем UTF-8
        # без BOM, как и раньше).
        bom = b""
        try:
            with open(self.file_path, "rb") as fb:
                if fb.read(3) == b"\xef\xbb\xbf":
                    bom = b"\xef\xbb\xbf"
        except OSError:
            pass
        text = self._normalized_text()
        if bom and text.startswith("\ufeff"):
            text = text[1:]
        try:
            with open(self.file_path, "wb") as f:
                if bom:
                    f.write(bom)
                f.write(text.encode("utf-8"))
        except OSError as e:
            # Ошибку показываем пользователю — иначе сбой записи (нет прав,
            # кончилось место) молча возвращал False, и файл «сохранён».
            QMessageBox.critical(self, "Save error",
                                 f"Failed to save file:\n{e}")
            return False
        self.setModified(False)
        # Файл мог быть пересоздан — переподписываем watcher
        self._watch_file(self.file_path)
        # Перерегистрируем документ в LSP только при реальной смене пути
        # (Save As / первое сохранение), а не при обычном Ctrl+S — иначе
        # clangd пересоздаёт сессию документа на каждое сохранение.
        if path is not None and path != old_path:
            self._lsp_bridge.detach()
            self._lsp_bridge.attach()
        # didSave: серверные проверки по сохранению (cargo check у
        # rust-analyzer приносит варнинги unused/dead_code).
        self._lsp_bridge.notify_save()
        return True

    # --- LSP (clangd) ---
    # Вся clangd-механика (диагностика, семантика, hover, переход к
    # определению, Ctrl-подчёркивание) вынесена в LspEditorBridge; здесь —
    # публичный интерфейс для остальных модулей.

    @property
    def current_lexer(self):
        return self._current_lexer

    @property
    def current_theme(self):
        return self._current_theme

    @property
    def lsp_attached(self) -> bool:
        return self._lsp_bridge.attached

    @property
    def lsp_error_count(self) -> int:
        return self._lsp_bridge.error_count

    @property
    def lsp_warning_count(self) -> int:
        return self._lsp_bridge.warning_count

    def lsp_diagnostics(self, uri: str, diagnostics: list):
        """Диагностика clangd (вызывается из LspManager)."""
        self._lsp_bridge.lsp_diagnostics(uri, diagnostics)

    def lsp_diags(self) -> List[Tuple[int, int, int, int, str]]:
        """Диагностики (line, start, end, severity, message) для списка.

        Внутренний список копируется: внешний код может хранить снимок,
        пока LSP присылает новые данные.
        """
        return list(self._lsp_bridge._diags)

    def lsp_reveal(self, pos: int):
        """Переход к позиции с вспышкой токена (из списка диагностик)."""
        self._lsp_bridge.reveal_position(pos)

    def lsp_go_to_definition(self):
        """F12: определение символа под курсором (через clangd)."""
        self._lsp_bridge.goto_definition()

    def lsp_rename(self):
        """F2: переименование символа под курсором (через LSP)."""
        self._lsp_bridge.rename_symbol()

    def lsp_load_file(self):
        """Common Lisp: загрузить файл в бэкенд cl-lsp (Ctrl+Shift+L)."""
        self._lsp_bridge.lisp_load_file()

    def lsp_check_lisp(self):
        """Common Lisp: проверить файл через SBCL (Ctrl+Shift+L).

        Диагностика появляется в маргине/подчёркиваниях/ErrorLens,
        как и серверные publishDiagnostics.
        """
        self._lsp_bridge.lisp_check_file()

    # --- события: Ctrl+клик -> определение, движение мыши -> hover ---

    def mousePressEvent(self, event):
        if (event.button() == Qt.MouseButton.LeftButton
                and event.modifiers() & Qt.KeyboardModifier.ControlModifier):
            # Ctrl+клик: сдвиг inlay компенсирует _pos_from_viewport —
            # ремап события дал бы двойное вычитание (переход на токен
            # перед кликнутым)
            if self._lsp_bridge.on_mouse_press(event):
                event.accept()
                return
            super().mousePressEvent(event)
            return
        adj = self._lsp_bridge.adjust_click_for_inlay(event)
        if adj is not None:
            event = _remap_mouse_event(event, adj)
        if self._lsp_bridge.on_mouse_press(event):
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseDoubleClickEvent(self, event):
        adj = self._lsp_bridge.adjust_click_for_inlay(event)
        if adj is not None:
            event = _remap_mouse_event(event, adj)
        super().mouseDoubleClickEvent(event)

    def mouseReleaseEvent(self, event):
        # release без ремапа после ремапленного press Scintilla считает drag-ом
        # и выделяет текст в сдвинутой inlay-области
        adj = self._lsp_bridge.adjust_click_for_inlay(event)
        if adj is not None:
            event = _remap_mouse_event(event, adj)
        super().mouseReleaseEvent(event)

# --- Автоскрытие полос прокрутки ---

    def _apply_editor_palette(self, bg: QColor, fg: QColor) -> None:
        """Окрасить фон самого виджета, а не только текстовой области.

        setPaper() красит лишь область текста. Область полос прокрутки
        находится вне неё, и при прозрачном QSS за полосой проступала
        серая палитра Qt (#efefef) — светлая полоса на тёмной теме.
        """
        pal = self.palette()
        pal.setColor(QPalette.ColorRole.Window, bg)
        pal.setColor(QPalette.ColorRole.Base, bg)
        pal.setColor(QPalette.ColorRole.WindowText, fg)
        pal.setColor(QPalette.ColorRole.Text, fg)
        self.setPalette(pal)
        self.setAutoFillBackground(True)

    def _scrollbar_auto_hide(self) -> bool:
        return app_settings().get_bool("editor.scrollbar_auto_hide", True)

    def _scrollbar_bars(self):
        return (self.verticalScrollBar(), self.horizontalScrollBar())

    def _sb_render(self, force: bool = False) -> None:
        """Перерисовать полосы с текущей альфой бегунка.

        Ширина полосы не меняется — меняется только прозрачность
        бегунка, поэтому текст не переверстывается.

        Каждый setStyleSheet на Windows переключает полосу между
        QStyleSheetStyle и нативным стилем, а нативная полоса мигает
        светлым. Поэтому альфа квантуется, и QSS ставится только при
        реальном изменении шага. Палитра самой полосы всегда равна
        фону редактора — тогда даже в момент переключения стиля
        подмена фоном невозможна.
        """
        if not self._sb_bg:
            return
        step = round(self._sb_alpha * _SB_ALPHA_STEPS) / _SB_ALPHA_STEPS
        if not force and step == self._sb_applied:
            return
        self._sb_applied = step
        qss = scrollbar_qss(self._sb_bg, self._sb_fg, alpha=step)
        for bar in self._scrollbar_bars():
            if bar is None:
                continue
            pal = bar.palette()
            pal.setColor(QPalette.ColorRole.Window, QColor(self._sb_bg))
            pal.setColor(QPalette.ColorRole.Base, QColor(self._sb_bg))
            bar.setPalette(pal)
            bar.setAutoFillBackground(True)
            bar.setStyleSheet(qss)

    def _on_sb_alpha(self, value) -> None:
        self._sb_alpha = float(value)
        self._sb_render()

    def _fade_to(self, target: float) -> None:
        """Плавно довести бегунок до target (1.0 — виден, 0.0 — скрыт)."""
        if not app_settings().get_bool("editor.scrollbar_fade", True):
            self._sb_anim.stop()
            self._sb_alpha = target
            self._sb_render(force=True)
            return
        if abs(self._sb_alpha - target) < 0.001:
            return
        self._sb_anim.stop()
        self._sb_anim.setStartValue(self._sb_alpha)
        self._sb_anim.setEndValue(target)
        self._sb_anim.start()

    def _show_scrollbars(self) -> None:
        if not self._sb_shown:
            self._sb_shown = True
            self._fade_to(1.0)

    def _hide_scrollbars(self) -> None:
        if not self._scrollbar_auto_hide():
            return
        if self._sb_on_scrollbar:
            return
        if self._sb_shown:
            self._sb_shown = False
            self._fade_to(0.0)

    def _scrollbar_activity(self) -> None:
        """Любое действие пользователя показывает полосы и откладывает hide."""
        if not self._scrollbar_auto_hide():
            # Настройку выключили, пока полосы были скрыты — вернуть их.
            self._show_scrollbars()
            return
        self._show_scrollbars()
        self._sb_hide_timer.start()

    def eventFilter(self, obj, event):
          if obj in self._scrollbar_bars():
              et = event.type()
              if et == QEvent.Type.Enter:
                  self._sb_on_scrollbar = True
                  self._show_scrollbars()
              elif et == QEvent.Type.Leave:
                  self._sb_on_scrollbar = False
                  self._scrollbar_activity()
              return False
          if obj is self.viewport():
              self._lsp_bridge.on_viewport_event(event)
              et = event.type()
              if et == QEvent.Type.MouseMove:
                  self._scrollbar_activity()
                  self._update_bookmark_tooltip(event)
              elif et == QEvent.Type.Leave:
                  self._clear_ghost()
              elif (et == QEvent.Type.MouseButtonPress
                    and event.button() == Qt.MouseButton.MiddleButton):
                  if self._maybe_delete_bookmark_at_event(event):
                      event.accept()
                      return True
          return super().eventFilter(obj, event)

    def _build_menu_qss(self, tc: ThemeColors) -> str:
        """QSS контекстного меню редактора в цветах темы."""
        fg = QColor(tc.fg)
        return f"""
            QMenu {{
                background-color: {tc.bg};
                border: 0.5px solid {tc.border};
                border-radius: 8px;
                padding: 4px;
            }}
            QMenu::item {{
                color: {tc.fg};
                padding: 4px 24px 4px 12px;
                border-radius: 6px;
            }}
            QMenu::item:disabled {{
                color: rgba({fg.red()},{fg.green()},{fg.blue()},120);
            }}
            QMenu::item:selected {{
                background-color: rgba({fg.red()},{fg.green()},{fg.blue()},30);
            }}
            QMenu::separator {{
                height: 1px;
                background: rgba({fg.red()},{fg.green()},{fg.blue()},48);
                margin: 4px 8px;
            }}
        """

    def contextMenuEvent(self, event):
        """Правая кнопка: стандартное меню QScintilla в цветах темы."""
        menu = self.createStandardContextMenu()
        if self._menu_qss:
            menu.setStyleSheet(self._menu_qss)
        menu.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        menu.exec(event.globalPos())

    # --- bookmarks ---

    def _faded_color(self, fg):
        """Цвет призрака-закладки: тот же оттенок, но затемнён к фону."""
        bg = self._bookmark_bg
        t = 0.62
        return QColor(
            int(fg.red() * (1 - t) + bg.red() * t),
            int(fg.green() * (1 - t) + bg.green() * t),
            int(fg.blue() * (1 - t) + bg.blue() * t),
        )

    def _setup_bookmark_marker(self, fg_color: QColor):
        """(Пере)определяет иконки закладки (твёрдую и блеклый призрак)."""
        from editor.widgets.file_icons import render_bookmark
        self._bookmark_fg = QColor(fg_color)
        # Рендерим ровно в нужный размер: load_ui_icon строит QIcon только из
        # фиксированных _ICON_SIZES (макс. 48px) и при зуме-in выше 48px
        # перестаёт расти. render_bookmark рисует SVG в точный размер, поэтому
        # иконка масштабируется вместе со шрифтом без потолка.
        size = int(round(self._font_size * self._BOOKMARK_ICON_SCALE))
        # Высота маркера = высоте строки, чтобы иконка центрировалась по
        # вертикали в строке (глиф центрируется внутри render_bookmark).
        lh = self.textHeight(0)
        solid = render_bookmark(QColor(fg_color), size, lh)
        faded = render_bookmark(self._faded_color(fg_color), size, lh)

        # Квадратный pixmap: Scintilla сам центрирует маркер в ячейке строки
        # ((cellHeight - pixmapHeight)/2) и в маргине ((marginWidth - pixmapWidth)/2),
        # поэтому вертикальное центрирование точное. Небольшой отступ слева
        # «зашит» в сам SVG (глиф сдвинут вправо внутри бокса), поэтому он
        # масштабируется пропорционально и не требует композиции pixmap.
        self.markerDefine(solid, self._BOOKMARK_MARKER)
        self.markerDefine(faded, self._BOOKMARK_GHOST_MARKER)
        self.SendScintilla(SCI_SETMARGINMASKN, self._BOOKMARK_MARGIN,
                           (1 << self._BOOKMARK_MARKER)
                           | (1 << self._BOOKMARK_GHOST_MARKER))
        # Ширина маргина = ширине иконки, чтобы горизонтальное центрирование
        # совпадало с геометрией pixmap (отступ слева задаётся в SVG).
        self.setMarginWidth(self._BOOKMARK_MARGIN, size)

    def _clear_ghost(self):
        """Убирает блеклый призрак наведения, если он показан."""
        if self._ghost_handle >= 0:
            self.markerDeleteHandle(self._ghost_handle)
            self._ghost_handle = -1
        self._ghost_line = -1
        self._hide_bookmark_tooltip()

    def _update_ghost(self, line: int):
        """Показывает блеклый призрак на строке под курсором (или скрывает)."""
        if line == self._ghost_line:
            return
        self._clear_ghost()
        # На строке с уже существующей закладкой призрак не нужен
        if self._bookmark_note_at_line(line) is not None:
            self._ghost_line = line
            return
        h = self.markerAdd(line, self._BOOKMARK_GHOST_MARKER)
        if h >= 0:
            self._ghost_handle = h
            self._ghost_line = line

    def _prune_bookmarks(self):
        """Удаляет из словаря закладки, чьи маркеры уехали (строка удалена)."""
        for handle in list(self._bookmarks.keys()):
            if self.markerLine(handle) < 0:
                self._bookmarks.pop(handle, None)

    def _bookmark_note_at_line(self, line: int):
        self._prune_bookmarks()
        for handle, note in self._bookmarks.items():
            if self.markerLine(handle) == line:
                return note
        return None

    def _on_bookmark_margin_clicked(self, margin, line, modifiers):
        if margin != self._BOOKMARK_MARGIN:
            return
        self._clear_ghost()
        self._open_bookmark_input(line)

    def _on_bookmark_margin_right_clicked(self, margin, line, modifiers):
        if margin != self._BOOKMARK_MARGIN:
            return
        # Правый клик по маргину закладки больше не удаляет её: удаление —
        # по средней кнопке (см. _maybe_delete_bookmark_at_event). Просто
        # сбрасываем призрак-подсветку.
        self._clear_ghost()

    def _open_bookmark_input(self, line: int):
        """Открывает панель ввода пояснения рядом с иконкой закладки на строке."""
        old = getattr(self, "_bookmark_popup", None)
        if old is not None:
            try:
                old.close()
            except Exception:
                pass
        from editor.bookmark_input import BookmarkInputPopup
        from PyQt6.QtCore import QPoint
        popup = BookmarkInputPopup(
            self, line, self.theme_manager, self._current_theme,
            self._create_bookmark, self)
        popup.adjustSize()
        top_left = self.mapToGlobal(QPoint(0, 0))
        # Вертикаль: центрируем попап по строке, на которой кликнули иконку.
        # SCI_POINTYFROMPOSITION даёт y строки относительно верха текстовой
        # области (верхних margin'ов нет, поэтому совпадает с верхом виджета).
        pos = self.SendScintilla(SCI_POSITIONFROMLINE, line)
        line_y = self.SendScintilla(SCI_POINTYFROMPOSITION, 0, pos)
        # Горизонталь: сразу справа от иконки закладки в маргине.
        icon_x = top_left.x() + self.marginWidth(self._BOOKMARK_MARGIN) + 4
        gx = icon_x - popup._shadow_margin
        gy = top_left.y() + line_y + self.textHeight(0) // 2 - popup.height() // 2
        # Не вылезаем за нижний край редактора.
        max_gy = top_left.y() + self.height() - popup.height() - 4
        if gy > max_gy:
            gy = max_gy
        popup.move(gx, gy)
        popup.show()
        self._bookmark_popup = popup

    def _create_bookmark(self, text: str, line: int):
        """Создаёт/заменяет закладку на строке с введённым пояснением."""
        self._prune_bookmarks()
        for handle, _ in list(self._bookmarks.items()):
            if self.markerLine(handle) == line:
                self.markerDeleteHandle(handle)
                self._bookmarks.pop(handle, None)
                break
        handle = self.markerAdd(line, self._BOOKMARK_MARKER)
        if handle >= 0:
            self._bookmarks[handle] = text
        self._clear_ghost()

    def get_bookmarks(self) -> list:
        """Сериализуемая форма закладок для сохранения в сессии.

        Возвращает список {"line": int, "note": str} (line — 0-based).
        """
        self._prune_bookmarks()
        out = []
        for handle, note in self._bookmarks.items():
            line = self.markerLine(handle)
            if line >= 0:
                out.append({"line": line, "note": note})
        return out

    def restore_bookmarks(self, items: list):
        """Восстанавливает закладки из сессии (по номеру строки).

        Если документ изменился и строка вышла за пределы — пропускаем.
        """
        if not items:
            return
        n = self.lines()
        for it in items:
            try:
                line = int(it.get("line", -1))
                note = it.get("note", "") or ""
            except (TypeError, ValueError):
                continue
            if 0 <= line < n:
                self._create_bookmark(note, line)

    def _line_from_point(self, x: int, y: int) -> int:
        """Документная строка (0-based) под курсором.

        Точный Scintilla-API (SCI_POSITIONFROMPOINT) в этой сборке QScintilla
        вызывает падение процесса, поэтому считаем по высоте строки. Для
        верхнего края (первая видимая строка прижата к y=0) формула точна.
        """
        first = self.firstVisibleLine()
        lh = self.textHeight(0)
        if lh <= 0:
            return first
        return first + max(0, y) // lh

    def _line_from_y(self, y: int) -> int:
        return self._line_from_point(0, y)

    def _maybe_delete_bookmark_at_event(self, event) -> bool:
        """Удаление закладки по средней кнопке в bookmark-маргине.

        Возвращает True, если закладка была удалена (событие нужно
        поглотить, чтобы Scintilla не обработала средний клик по-своему).
        """
        pos = event.pos()
        x0 = sum(self.marginWidth(m) for m in range(self._BOOKMARK_MARGIN))
        x1 = x0 + self.marginWidth(self._BOOKMARK_MARGIN)
        if not (x0 <= pos.x() < x1):
            return False
        line = self._line_from_point(pos.x(), pos.y())
        self._prune_bookmarks()
        for handle, _ in list(self._bookmarks.items()):
            if self.markerLine(handle) == line:
                self.markerDeleteHandle(handle)
                self._bookmarks.pop(handle, None)
                self._clear_ghost()
                return True
        return False

    def _update_bookmark_tooltip(self, event):
        """Показывает кастомную плашку с пояснением и блеклый призрак
        при наведении на bookmark-маргин (вместо встроенного QToolTip)."""
        pos = event.pos()
        # bookmark-маргин — самый левый (индекс 0), его x-диапазон [0, width)
        x1 = self.marginWidth(self._BOOKMARK_MARGIN)
        if not (0 <= pos.x() < x1):
            self._clear_ghost()  # прячет и призрак, и плашку
            return
        line = self._line_from_point(pos.x(), pos.y())
        self._update_ghost(line)
        note = self._bookmark_note_at_line(line)
        if note:
            self._show_bookmark_tooltip(note, line)
        else:
            self._hide_bookmark_tooltip()

    def _show_bookmark_tooltip(self, text: str, line: int):
        """Показывает/обновляет кастомную плашку рядом с иконкой закладки."""
        tip = getattr(self, "_bookmark_tooltip", None)
        # Создаём заново, если ещё нет или сменилась тема (чтобы цвета
        # оставались актуальными). Пересоздание только при смене темы, а не
        # на каждом mouseMove — поэтому без мерцания.
        if tip is None or getattr(tip, "_theme_name", None) != self._current_theme:
            from editor.bookmark_tooltip import BookmarkTooltip
            tip = BookmarkTooltip(
                self.theme_manager, self._current_theme, self)
            self._bookmark_tooltip = tip
        tip.set_note(text)
        # Геометрия могла измениться (другая длина текста) — пересчитываем
        # синхронно до чтения height()/позиционирования, иначе при быстром
        # перемещении между закладками плашка не успевает вырасти и текст
        # обрезается.
        tip.adjustSize()
        # Позиция рядом с иконкой, как у виджета ввода: справа от bookmark-
        # маргина, по вертикали — по центру строки.
        top_left = self.mapToGlobal(QPoint(0, 0))
        lpos = self.SendScintilla(SCI_POSITIONFROMLINE, line)
        line_y = self.SendScintilla(SCI_POINTYFROMPOSITION, 0, lpos)
        lh = self.textHeight(0)
        icon_x = top_left.x() + self.marginWidth(self._BOOKMARK_MARGIN) + 4
        gy = top_left.y() + line_y + lh // 2 - tip.height() // 2
        max_gy = top_left.y() + self.height() - tip.height() - 4
        if gy > max_gy:
            gy = max_gy
        # Окно шире карточки на _shadow_margin с каждой стороны — сдвигаем
        # окно влево, чтобы сама карточка оказалась у иконки (как раньше).
        tip.move(icon_x - tip._shadow_margin, gy)
        tip.show()

    def _hide_bookmark_tooltip(self, *_):
        tip = getattr(self, "_bookmark_tooltip", None)
        if tip is not None:
            try:
                tip.hide()
            except Exception:
                pass

    # --- theme ---

    def apply_theme(self, theme_name: str):
        self._current_theme = theme_name
        self.theme_manager.apply_theme(self, theme_name, self._current_lexer)
        # Список автодополнения красим в цвета темы
        self._completion.apply_theme(theme_name)
        self._lsp_bridge.apply_theme(theme_name)
        # Хлебные крошки — цветами той же темы
        bar = getattr(self, "_breadcrumbs_bar", None)
        if bar is not None:
            bar.apply_theme(theme_name)

        # Шрифт маргина — из _font_size (зум редактора), а не из лексера:
        # defaultFont(0) кастомных лексеров несёт размер из темы (10pt),
        # а не шрифт кода, и на старте номера строк становились мелкими.
        self._margin_font = QFont(code_font_family(), self._font_size)
        self._margin_font.setFixedPitch(True)
        self.setMarginsFont(self._margin_font)

        # Scrollbar styling directly on each QScrollBar: фон — затемнённый
        # цвет редактора (темнее тайтл-бара и табов)
        tc = ThemeColors.from_theme_manager(self.theme_manager, theme_name)
        editor_bg_color = tc.editor_bg_color
        self._bookmark_bg = QColor(editor_bg_color)  # для затухания призрака
        editor_bg = editor_bg_color.name()
        self._menu_qss = self._build_menu_qss(tc)

        # Индикатор цели перехода и hover-попап красит мост LSP
        self.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        # Палитра для последующих перерисовок анимации: тема могла
        # смениться, пока бегунок был в промежуточном состоянии.
        self._sb_bg = editor_bg
        self._sb_fg = QColor(tc.fg_color)
        vsb = self.verticalScrollBar()
        if vsb:
            if not self._bookmark_tooltip_scroll_connected:
                vsb.valueChanged.connect(self._hide_bookmark_tooltip)
                self._bookmark_tooltip_scroll_connected = True
        self._sb_render(force=True)

        # Scrollbar corner (bottom-right square) — цвет под фон редактора
        corner = QWidget()
        corner.setStyleSheet(f"background: {editor_bg};")
        self.setCornerWidget(corner)

        # Force background update — use darker bg for editing area
        self.setPaper(editor_bg_color)
        self._apply_editor_palette(editor_bg_color, QColor(tc.fg_color))

        # Точки-пробелы: смесь фона редактора и текста темы — видны
        # и на светлой (темнеют), и на тёмной теме (светлеют)
        self.setWhitespaceForegroundColor(tc.whitespace_color())

        # Make the line number margin and fold margin use the darker bg too
        self.setMarginsBackgroundColor(editor_bg_color)
        self.setFoldMarginColors(editor_bg_color, editor_bg_color)

        if self._current_lexer:
            # Update the lexer's default paper and all defined style backgrounds
            self._current_lexer.setDefaultPaper(editor_bg_color)
            for style_id in range(MAX_LEXER_STYLES):
                if self._current_lexer.description(style_id):
                    self._current_lexer.setPaper(editor_bg_color, style_id)
        else:
            # Without a lexer, use SendScintilla to force the default style
            # SCI_STYLESETBACK: задать фон стиля
            editor_bg_int = rgb_int(editor_bg_color)
            for style_id in range(STYLE_RANGE):
                self.SendScintilla(SCI_STYLESETBACK, style_id, editor_bg_int)
            self.SendScintilla(SCI_STYLESETBACK, STYLE_DEFAULT, editor_bg_int)

        # Шрифты НЕ переприменяем: размер живёт в _font_size (зум), лексеры
        # его сохраняют (setup_styles/set_theme) — тема это только палитра.
        # Пересобираем лишь иконки фолдинга под новые цвета.
        self._rebuild_fold_icons()

        # Иконка закладки — тем же цветом fg
        self._setup_bookmark_marker(tc.fg_color)

        # Горизонтальная линия под свёрнутым блоком и линии фолда в тексте
        # рисуются цветом STYLE_DEFAULT (в этой сборке QScintilla нет
        # element colours). Текстовые стили получают явные цвета, поэтому
        # перекраска STYLE_DEFAULT трогает только линию.
        if self._current_lexer:
            fold_line = self.theme_manager.themes\
                .get(self._current_theme, {})\
                .get("colors", {}).get("folding_foreground")
            if fold_line:
                c = QColor(fold_line)
                fold_rgb = rgb_int(c)
                self.SendScintilla(SCI_STYLESETFORE, STYLE_DEFAULT, fold_rgb)

        self.update()
