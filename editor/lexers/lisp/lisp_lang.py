"""
LispLexer — кастомный лексер для Lisp (.lisp/.lsp/.cl/.el/.scm/.ss/.rkt).

Подсветка специальных операторов, builtin-форм, строк с экранированием,
чисел (в т.ч. #x/#o/#b), character-литералов #\\a, keywords :foo,
package-символов pkg::sym, строковых ; и блочных #| ... |# комментариев,
quote/quasiquote/unquote и фолдинг по круглым скобкам.

Производительность по образцу LumenLangLexer: текст кэшируется построчно,
состояние блочного комментария #| |# пересчитывается инкрементально из
SCN_MODIFIED, фолдинг пересчитывается только когда изменились скобки.
"""

from typing import List, Optional, Tuple

from PyQt6.QtGui import QColor, QFont
from PyQt6.Qsci import QsciLexerCustom

from editor.scintilla_constants import (
    SCI_GETFOLDLEVEL,
    SCI_LINEFROMPOSITION,
    SCI_POSITIONFROMLINE,
    SCI_SETFOLDLEVEL,
)
from editor.constants import CODE_FONT_SIZE
from editor.font_settings import code_font_family


# Scintilla-константы для фолдинга
_FOLDLEVELBASE = 0x400        # SC_FOLDLEVELBASE = 1024
_FOLDLEVELHEADERFLAG = 0x2000 # SC_FOLDLEVELHEADERFLAG
_FOLDLEVELNUMBERMASK = 0x0FFF # SC_FOLDLEVELNUMBERMASK

# Биты SCN_MODIFIED (SC_MOD_INSERTTEXT / SC_MOD_DELETETEXT)
SC_MOD_INSERTTEXT = 0x1
SC_MOD_DELETETEXT = 0x2

# Правки крупнее этого объёма дешевле пересобрать целиком
_FULL_REBUILD_THRESHOLD = 8192

_WHITESPACE = " \t\r\n"
_SYMBOL_TERMINATORS = set('()[]{}"\'`,;')
# Двойной quote: " и ' в _SYMBOL_TERMINATORS (для строк и quote-оператора)

_KEYWORDS = {
    'defun', 'defmacro', 'defgeneric', 'defmethod', 'defclass', 'defstruct',
    'defpackage', 'in-package', 'defvar', 'defparameter', 'defconstant',
    'lambda', 'let', 'let*', 'flet', 'labels', 'macrolet', 'symbol-macrolet',
    'if', 'cond', 'case', 'typecase', 'ecase', 'etypecase', 'when', 'unless',
    'and', 'or', 'not', 'quote', 'quasiquote', 'unquote', 'unquote-splicing',
    'function', 'setq', 'setf', 'psetq', 'psetf', 'progn', 'prog1', 'prog2',
    'loop', 'do', 'do*', 'dolist', 'dotimes', 'return', 'return-from', 'block',
    'catch', 'throw', 'unwind-protect', 'multiple-value-bind',
    'multiple-value-call', 'multiple-value-prog1', 'multiple-value-list',
    'multiple-value-setq', 'eval-when', 'locally', 'the', 'typep', 'declare',
    'handler-case', 'handler-bind', 'ignore-errors', 'assert', 'check-type',
    'go', 'tagbody', 'incf', 'decf', 'push', 'pop', 'pushnew', 'car', 'cdr',
    'cons', 'list', 'append', 'reverse', 'mapcar', 'mapc', 'maplist', 'apply',
    'funcall', 'reduce', 'remove', 'find', 'member', 'assoc', 'format',
    'print', 'prin1', 'princ', 'terpri', 'fresh-line', 'read', 'read-line',
    'load', 'compile', 'compile-file', 'eval', 'error', 'warn', 'signal',
    'make-instance', 'slot-value', 'with-slots', 'with-open-file',
    'with-open-stream', 'with-input-from-string', 'with-output-to-string',
    'values', 'nth-value', 'floor', 'ceiling', 'truncate', 'round',
}

_LITERALS = {
    'nil', 't', 'true', 'false',
    '&optional', '&rest', '&key', '&body', '&aux', '&allow-other-keys',
    '&whole', '&environment',
}

# Имена определений: следующий символ — имя (функции/класса/структуры)
_DEFN_KEYWORDS = {
    'defun', 'defmacro', 'defgeneric', 'defmethod', 'defstruct', 'defclass',
}

# Точные совпадения: арифметика/сравнение, которые не являются словами
_OPERATOR_SYMBOLS = {
    '+', '-', '*', '/', '=', '/=', '<', '<=', '>', '>=', '1+', '1-',
    'mod', 'rem', 'expt', 'sqrt', 'abs', 'min', 'max', 'logand', 'logior',
}

_RADIX_PREFIXES = ('x', 'X', 'o', 'O', 'b', 'B', 'c', 'C', 'r', 'R')


class LispLexer(QsciLexerCustom):
    """Custom lexer for Common Lisp / Scheme-like syntax with paren folding."""

    STYLE_DEFAULT = 0
    STYLE_COMMENT = 1
    STYLE_STRING = 2
    STYLE_ESCAPE = 3
    STYLE_NUMBER = 4
    STYLE_CHARACTER = 5
    STYLE_KEYWORD = 6
    STYLE_FUNCTION = 7
    STYLE_VARIABLE = 8
    STYLE_LITERAL = 9
    STYLE_KEYWORD_ARG = 10
    STYLE_PACKAGE = 11
    STYLE_OPERATOR = 12
    STYLE_SPECIAL = 13
    STYLE_COMMENT_DOC = 14

    LINE_COMMENT = ";"  # комментарий строки (Ctrl+/)

    # Стиль → (ключ темы, цвет по умолчанию)
    _STYLE_THEME_KEYS = {
        STYLE_DEFAULT: ("default", None),
        STYLE_COMMENT: ("comment", QColor(108, 112, 134)),
        STYLE_STRING: ("string", QColor(166, 227, 161)),
        STYLE_ESCAPE: ("escape", QColor(249, 226, 175)),
        STYLE_NUMBER: ("number", QColor(250, 179, 135)),
        STYLE_CHARACTER: ("number", QColor(250, 179, 135)),
        STYLE_KEYWORD: ("keyword", QColor(203, 166, 247)),
        STYLE_FUNCTION: ("function_name", QColor(249, 226, 175)),
        STYLE_VARIABLE: ("variable", QColor(148, 226, 213)),
        STYLE_LITERAL: ("literal", QColor(148, 226, 213)),
        STYLE_KEYWORD_ARG: ("special", QColor(249, 226, 175)),
        STYLE_PACKAGE: ("class_name", QColor(137, 220, 235)),
        STYLE_OPERATOR: ("operator", QColor(137, 180, 250)),
        STYLE_SPECIAL: ("special", QColor(243, 139, 168)),
        STYLE_COMMENT_DOC: ("comment_doc", QColor(148, 226, 213)),
    }

    _NUM_STYLES = 15

    def __init__(self, parent=None, theme_data: Optional[dict] = None):
        super().__init__(parent)
        self._theme_data = theme_data or {}

        # Кэш документа (обновляется инкрементально из SCN_MODIFIED)
        self._text: str = ""
        self._text_bytes: bytes = b""
        self._lines: List[str] = []
        # Состояние #| |# на начало строки: True — внутри блочного комментария
        self._block_state: List[bool] = []
        self._dirty: bool = True
        self._hooked_editor = None
        self._folding_dirty: bool = False
        self._folding_line: int = 0

        self.setup_styles()

    # ------------------------------------------------------------------
    # Язык
    # ------------------------------------------------------------------

    def language(self) -> str:
        return "Lisp"

    def wordCharacters(self) -> str:
        return ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "0123456789_-+*/<>=?!$%&^~:.@#")

    def description(self, style: int) -> str:
        descriptions = {
            self.STYLE_DEFAULT: "Default",
            self.STYLE_COMMENT: "Comment",
            self.STYLE_STRING: "String",
            self.STYLE_ESCAPE: "Escape sequence",
            self.STYLE_NUMBER: "Number",
            self.STYLE_CHARACTER: "Character literal",
            self.STYLE_KEYWORD: "Keyword",
            self.STYLE_FUNCTION: "Function",
            self.STYLE_VARIABLE: "Variable",
            self.STYLE_LITERAL: "Literal",
            self.STYLE_KEYWORD_ARG: "Keyword argument",
            self.STYLE_PACKAGE: "Package",
            self.STYLE_OPERATOR: "Operator",
            self.STYLE_SPECIAL: "Special",
            self.STYLE_COMMENT_DOC: "Documentation Comment",
        }
        return descriptions.get(style, "")

    # ------------------------------------------------------------------
    # Темизация — интерфейс для ThemeManager
    # ------------------------------------------------------------------

    def set_theme(self, theme_data: dict):
        """Вызывается ThemeManager для применения цветовой темы."""
        self._theme_data = theme_data
        self.setup_styles()

    def setup_styles(self):
        theme = self._theme_data
        colors = theme.get("colors", {})
        styles = theme.get("styles", {})

        bg = QColor(colors.get("background", "#1b1d2e"))
        fg = QColor(colors.get("foreground", "#cdd6f4"))

        self.setDefaultPaper(bg)
        self.setDefaultColor(fg)

        size = (getattr(self.editor(), "_font_size", None)
                or styles.get("default", {}).get("size")
                or CODE_FONT_SIZE)
        base_font = QFont(code_font_family(), size)
        base_font.setFixedPitch(True)

        for style_id in range(self._NUM_STYLES):
            self.setFont(base_font, style_id)
            self.setPaper(bg, style_id)

        for style_id, (key, fallback) in self._STYLE_THEME_KEYS.items():
            color = fg if fallback is None else fallback
            self.setColor(self._style_color(styles, key, color), style_id)

        italic_font = QFont(base_font)
        italic_font.setItalic(True)
        self.setFont(italic_font, self.STYLE_COMMENT)
        self.setFont(italic_font, self.STYLE_COMMENT_DOC)

        bold_font = QFont(base_font)
        bold_font.setBold(True)
        self.setFont(bold_font, self.STYLE_KEYWORD)

    @staticmethod
    def _style_color(styles: dict, key: str, fallback: QColor) -> QColor:
        data = styles.get(key)
        if data and "color" in data:
            return QColor(data["color"])
        return fallback

    # ------------------------------------------------------------------
    # Основной токенизатор
    # ------------------------------------------------------------------

    def styleText(self, start: int, end: int):
        editor = self.editor()
        if not editor:
            return
        self._ensure_hooked(editor)
        if self._dirty:
            self._refresh_cache(editor)

        text_bytes = self._text_bytes
        total_bytes = len(text_bytes)
        if total_bytes == 0:
            return

        start = max(0, start)
        end = min(total_bytes, end)

        # Блочный комментарий #|, открытый до start — стилизуем с его начала.
        line = editor.SendScintilla(SCI_LINEFROMPOSITION, start)
        in_block = (self._block_state[line]
                    if line < len(self._block_state) else False)
        if in_block:
            start = editor.SendScintilla(SCI_POSITIONFROMLINE, line)
        elif line < len(self._block_state):
            line_start = editor.SendScintilla(SCI_POSITIONFROMLINE, line)
            rel = self._find_open_block_in_segment(text_bytes[line_start:start])
            if rel >= 0:
                start = line_start + rel

        if start >= end:
            return

        self.startStyling(start)

        pos = start
        # Мы внутри #| |#, открытого раньше start — остаёмся в режиме комментария.
        in_block_here = in_block
        # Последний «значимый» символ перед pos: используется для определения
        # функции — первого элемента списка (symbol сразу после открытой '(').
        last_sig = self._prev_significant(text_bytes, start)
        # Следующий символ — имя определяемой функции (после defun/defmacro…)
        next_is_defn = False

        while pos < end:
            if in_block_here:
                close = text_bytes.find(b'|#', pos, end)
                if close == -1:
                    self.setStyling(end - pos, self.STYLE_COMMENT)
                    pos = end
                    break
                close += 2
                self.setStyling(close - pos, self.STYLE_COMMENT)
                pos = close
                in_block_here = False
                continue

            ch, ch_len = self._get_char_at(text_bytes, pos, total_bytes)
            if ch_len == 0:
                break

            # --- Пробелы ---
            if ch in _WHITESPACE:
                self.setStyling(ch_len, self.STYLE_DEFAULT)
                pos += ch_len
                continue

            # --- Комментарии (; и ;; ;;...) ---
            if ch == ';':
                next_is_defn = False
                comment_style = self.STYLE_COMMENT
                if pos + ch_len < end:
                    next_ch, _ = self._get_char_at(text_bytes, pos + ch_len,
                                                   total_bytes)
                    if next_ch == ';':
                        comment_style = self.STYLE_COMMENT_DOC
                j = pos
                while j < end:
                    c, l = self._get_char_at(text_bytes, j, total_bytes)
                    if c == '\n':
                        j += l
                        break
                    j += l
                self.setStyling(j - pos, comment_style)
                pos = j
                continue

            # --- Строки ---
            if ch == '"':
                next_is_defn = False
                last_sig = '"'
                j = pos + ch_len
                self.setStyling(ch_len, self.STYLE_STRING)
                while j < end:
                    c, l = self._get_char_at(text_bytes, j, total_bytes)
                    if c is None:
                        break
                    if c == '\\':
                        esc_len = l
                        j += l
                        if j < end:
                            _, next_l = self._get_char_at(text_bytes, j,
                                                          total_bytes)
                            esc_len += next_l
                            j += next_l
                        self.setStyling(esc_len, self.STYLE_ESCAPE)
                    elif c == '"':
                        self.setStyling(l, self.STYLE_STRING)
                        j += l
                        break
                    else:
                        self.setStyling(l, self.STYLE_STRING)
                        j += l
                pos = j
                continue

            # --- Quote / quasiquote / unquote ---
            if ch == "'":
                next_is_defn = False
                self.setStyling(ch_len, self.STYLE_SPECIAL)
                last_sig = "'"
                pos += ch_len
                continue
            if ch == '`':
                next_is_defn = False
                self.setStyling(ch_len, self.STYLE_SPECIAL)
                last_sig = '`'
                pos += ch_len
                continue
            if ch == ',':
                next_is_defn = False
                j = pos + ch_len
                if j < end:
                    next_ch, next_len = self._get_char_at(text_bytes, j,
                                                          total_bytes)
                    if next_ch == '@':
                        j += next_len
                self.setStyling(j - pos, self.STYLE_SPECIAL)
                last_sig = ','
                pos = j
                continue

            # --- Скобки ---
            if ch in '()[]{}':
                next_is_defn = False
                self.setStyling(ch_len, self.STYLE_OPERATOR)
                last_sig = ch
                pos += ch_len
                continue

            # --- Reader-макросы #... ---
            if ch == '#':
                next_is_defn = False
                last_sig = '#'
                pos = self._style_reader_macro(text_bytes, pos, end,
                                               total_bytes)
                continue

            # --- Числа ---
            if ch.isdigit() or (ch in '+-.' and pos + ch_len < end and
                                self._get_char_at(text_bytes, pos + ch_len,
                                                  total_bytes)[0].isdigit()):
                next_is_defn = False
                j = pos
                while j < end:
                    c, l = self._get_char_at(text_bytes, j, total_bytes)
                    if not self._is_symbol_char(c):
                        break
                    j += l
                self.setStyling(j - pos, self.STYLE_NUMBER)
                last_sig = text_bytes[j - 1:j].decode('latin-1')
                pos = j
                continue

            # --- Keywords :foo и ::bar ---
            if ch == ':':
                next_is_defn = False
                j = pos
                while j < end:
                    c, l = self._get_char_at(text_bytes, j, total_bytes)
                    if not self._is_symbol_char(c):
                        break
                    j += l
                self.setStyling(j - pos, self.STYLE_KEYWORD_ARG)
                last_sig = ':'
                pos = j
                continue

            # --- Символы ---
            j = pos
            while j < end:
                c, l = self._get_char_at(text_bytes, j, total_bytes)
                if not self._is_symbol_char(c):
                    break
                j += l
            if j == pos:
                # Неизвестный символ — просто пропускаем
                self.setStyling(ch_len, self.STYLE_DEFAULT)
                last_sig = ch
                pos += ch_len
                continue

            word = text_bytes[pos:j].decode('utf-8', errors='replace')
            if next_is_defn:
                style = self.STYLE_FUNCTION
                next_is_defn = False
            elif word in _KEYWORDS:
                style = self.STYLE_KEYWORD
                next_is_defn = word in _DEFN_KEYWORDS
            elif word in _LITERALS:
                style = self.STYLE_LITERAL
            elif word in _OPERATOR_SYMBOLS:
                style = self.STYLE_OPERATOR
            elif word.startswith(':'):
                style = self.STYLE_KEYWORD_ARG
            elif '::' in word or (':' in word and word[0] != ':'):
                style = self.STYLE_PACKAGE
            elif last_sig == '(':
                style = self.STYLE_FUNCTION
            else:
                style = self.STYLE_VARIABLE

            self.setStyling(j - pos, style)
            last_sig = word[-1]
            pos = j

        self.update_folding_levels(start, end)

    def _style_reader_macro(self, text_bytes: bytes, pos: int, end: int,
                            total_bytes: int) -> int:
        """Стилизует reader-макрос с позиции '#', возвращает новый pos."""
        ch_len = 1
        if pos + ch_len >= end:
            self.setStyling(ch_len, self.STYLE_SPECIAL)
            return pos + ch_len

        next_ch, next_len = self._get_char_at(text_bytes, pos + ch_len,
                                              total_bytes)

        if next_ch == '\\':
            # Character literal #\a, #\Space
            j = pos + ch_len + next_len
            while j < end:
                c, l = self._get_char_at(text_bytes, j, total_bytes)
                if c in _WHITESPACE or c in '()"\'`,;':
                    break
                j += l
            self.setStyling(j - pos, self.STYLE_CHARACTER)
            return j

        if next_ch == '|':
            # Блочный комментарий #| ... |#
            close = text_bytes.find(b'|#', pos + ch_len + next_len, total_bytes)
            if close == -1:
                close = total_bytes
            else:
                close += 2
            self.setStyling(close - pos, self.STYLE_COMMENT)
            return close

        if next_ch == "'":
            # Функциональный ссылочный оператор #'
            self.setStyling(ch_len + next_len, self.STYLE_SPECIAL)
            return pos + ch_len + next_len

        if next_ch == '(':
            # Вектор #(...) — скобки как оператор
            self.setStyling(ch_len + next_len, self.STYLE_OPERATOR)
            return pos + ch_len + next_len

        if next_ch == ':':
            # Uninterned keyword #:foo
            j = pos + ch_len + next_len
            while j < end:
                c, l = self._get_char_at(text_bytes, j, total_bytes)
                if not self._is_symbol_char(c):
                    break
                j += l
            self.setStyling(j - pos, self.STYLE_KEYWORD_ARG)
            return j

        if (next_ch in _RADIX_PREFIXES and pos + ch_len + next_len < end and
                self._get_char_at(text_bytes, pos + ch_len + next_len,
                                  total_bytes)[0].isalnum()):
            # Числа с основанием #x1F, #b101, #r36...
            j = pos + ch_len + next_len
            while j < end:
                c, l = self._get_char_at(text_bytes, j, total_bytes)
                if not self._is_symbol_char(c):
                    break
                j += l
            self.setStyling(j - pos, self.STYLE_NUMBER)
            return j

        # Прочие: #. #= ## #< #+ #- #0#...
        self.setStyling(ch_len, self.STYLE_SPECIAL)
        return pos + ch_len

    # ------------------------------------------------------------------
    # Фолдинг
    # ------------------------------------------------------------------

    def update_folding_levels(self, start: int, end: int):
        """Уровни фолдинга по круглым скобкам.

        Запускается только при изменении скобок (флаг из _on_modified);
        строки берутся из кэша.
        """
        if not self._folding_dirty:
            return
        self._folding_dirty = False
        editor = self.editor()
        if editor is None:
            return

        total_lines = len(self._lines)
        if total_lines == 0:
            return
        first = min(self._folding_line, total_lines - 1)
        end_line = editor.SendScintilla(SCI_LINEFROMPOSITION, end)

        current_level = _FOLDLEVELBASE
        if first > 0:
            prev_level = editor.SendScintilla(SCI_GETFOLDLEVEL, first - 1)
            current_level = prev_level & _FOLDLEVELNUMBERMASK
            prev_opens, prev_closes = self._count_parens_safely(
                self._lines[first - 1])
            current_level += prev_opens - prev_closes
            if current_level < _FOLDLEVELBASE:
                current_level = _FOLDLEVELBASE

        for line in range(first, total_lines):
            open_count, close_count = self._count_parens_safely(self._lines[line])
            net_change = open_count - close_count

            is_header = _FOLDLEVELHEADERFLAG if net_change > 0 else 0
            new_fold_level = current_level | is_header

            if line > end_line and net_change == 0:
                old_fold_level = editor.SendScintilla(SCI_GETFOLDLEVEL, line)
                if ((old_fold_level & _FOLDLEVELNUMBERMASK) ==
                        (new_fold_level & _FOLDLEVELNUMBERMASK)
                        and bool(old_fold_level & _FOLDLEVELHEADERFLAG) ==
                        bool(new_fold_level & _FOLDLEVELHEADERFLAG)):
                    break

            editor.SendScintilla(SCI_SETFOLDLEVEL, line, new_fold_level)

            current_level += net_change
            if current_level < _FOLDLEVELBASE:
                current_level = _FOLDLEVELBASE

    def _count_parens_safely(self, text: str) -> Tuple[int, int]:
        """Считает ( и ), игнорируя строки, ; и #|...|# комментарии."""
        open_count = 0
        close_count = 0
        in_string = False
        in_block = False
        i = 0
        n = len(text)

        while i < n:
            c = text[i]
            if in_block:
                if text.startswith('|#', i):
                    in_block = False
                    i += 2
                else:
                    i += 1
                continue
            if in_string:
                if c == '\\':
                    i += 2
                    continue
                if c == '"':
                    in_string = False
                i += 1
                continue
            if c == ';':
                break
            if c == '"':
                in_string = True
                i += 1
                continue
            if c == '#' and text.startswith('#|', i):
                in_block = True
                i += 2
                continue
            if c == '(':
                open_count += 1
            elif c == ')':
                close_count += 1
            i += 1

        return open_count, close_count

    def _mark_folding(self, changed_text, line: int):
        """Скобки изменились — фолдинг пересчитается при следующем styleText."""
        # SCN_MODIFIED отдаёт text как bytes (перебор даёт int) — сравниваем с bytes
        if isinstance(changed_text, bytes):
            if not any(c in b'()' for c in changed_text):
                return
        elif not any(c in '()' for c in changed_text):
            return
        if self._folding_dirty:
            self._folding_line = min(self._folding_line, line)
        else:
            self._folding_dirty = True
            self._folding_line = line

    # ------------------------------------------------------------------
    # Инкрементальный кэш (SCN_MODIFIED): состояние #| |#
    # ------------------------------------------------------------------

    def _ensure_hooked(self, editor):
        """Подключает SCN_MODIFIED один раз на редактор; кэш сбрасывается."""
        if editor is self._hooked_editor:
            return
        old = self._hooked_editor
        if old is not None:
            try:
                old.SCN_MODIFIED.disconnect(self._on_modified)
            except Exception:
                pass
        editor.SCN_MODIFIED.connect(self._on_modified)
        self._hooked_editor = editor
        self._dirty = True

    def _refresh_cache(self, editor):
        """Обновляет кэш текста: 1 полный проход на правку."""
        text = editor.text()
        self._text = text
        self._text_bytes = text.encode('utf-8')
        self._lines = text.split('\n')
        if len(self._block_state) != len(self._lines) + 1:
            self._rebuild_all()
        self._dirty = False

    def _on_modified(self, pos, mod_type, text, length, lines_added, line, *_):
        """SCN_MODIFIED: инкрементально обновляет построчные кэши."""
        if not (mod_type & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)):
            return
        try:
            editor = self.editor()
        except RuntimeError:
            return  # лексер отвязан от редактора (deleteLater при смене языка)
        try:
            # Qsci всегда отдаёт line=0 — строку правки считаем по позиции (байты)
            line = editor.SendScintilla(SCI_LINEFROMPOSITION, pos)
            if length > _FULL_REBUILD_THRESHOLD:
                self._rebuild_from_editor(editor)
                return
            self._splice_line_lists(line, lines_added)
            n_changed = 1 + max(lines_added, 0)
            self._recompute_block_states(editor, line, n_changed)
            if mod_type & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT) and text:
                self._mark_folding(text, line)
        except Exception:
            # Ошибка внутри обработчика сигнала не должна ронять процесс:
            # сбрасываем кэш, следующий styleText пересоберёт всё заново.
            self._block_state = []
            self._dirty = True
        self._dirty = True

    def _rebuild_from_editor(self, editor):
        """Пересборка всех кэшей за один проход (крупные правки)."""
        text = editor.text()
        self._text = text
        self._text_bytes = text.encode('utf-8')
        self._lines = text.split('\n')
        self._rebuild_all()
        self._dirty = True

    def _rebuild_all(self):
        """Полная пересборка построчного кэша состояния #| |#."""
        block_state = [False]
        in_block = False
        for line_text in self._lines:
            in_block = self._line_state_after(in_block, line_text)
            block_state.append(in_block)
        self._block_state = block_state
        self._folding_dirty = True
        self._folding_line = 0

    def _splice_line_lists(self, line: int, lines_added: int):
        """Выравнивает кэш состояния под новое число строк документа."""
        if lines_added > 0:
            self._block_state[line + 1:line + 1] = [False] * lines_added
        elif lines_added < 0:
            # Строка `line` (объединённая) сохраняется — её стартовое
            # состояние валидно; удаляются только съеденные строки.
            hi = line + 1 - lines_added
            del self._block_state[line + 1:hi]

    def _recompute_block_states(self, editor, start_line: int, n_changed: int):
        """Пересчитывает состояние #| |# для изменившихся строк и далее
        до стабилизации."""
        bs = self._block_state
        n = len(bs) - 1  # индекс последней строки
        end_phase1 = min(start_line + n_changed, n)
        for i in range(start_line, end_phase1):
            bs[i + 1] = self._line_state_after(bs[i], editor.text(i))
        i = end_phase1
        while i < n:
            new_state = self._line_state_after(bs[i], editor.text(i))
            if new_state == bs[i + 1]:
                break
            bs[i + 1] = new_state
            i += 1

    def _line_state_after(self, in_block: bool, line_text: str) -> bool:
        """Состояние #| |# в конце строки."""
        i, n = 0, len(line_text)
        while i < n:
            c = line_text[i]
            if in_block:
                if line_text.startswith('|#', i):
                    in_block = False
                    i += 2
                else:
                    i += 1
                continue
            if c == ';':
                break
            if c == '"':
                i += 1
                while i < n:
                    ch = line_text[i]
                    if ch == '\\':
                        i += 2
                        continue
                    if ch == '"':
                        i += 1
                        break
                    i += 1
                continue
            if c == '#' and line_text.startswith('#|', i):
                in_block = True
                i += 2
                continue
            i += 1
        return in_block

    # ------------------------------------------------------------------
    # Утилиты для работы с UTF-8
    # ------------------------------------------------------------------

    @staticmethod
    def _get_char_at(text_bytes: bytes, pos: int,
                     total_bytes: int) -> Tuple[Optional[str], int]:
        """Извлекает один UTF-8 символ из байтовой строки по позиции."""
        if pos >= total_bytes:
            return (None, 0)

        b = text_bytes[pos]
        if b & 0x80 == 0:
            length = 1
        elif b & 0xE0 == 0xC0:
            length = 2
        elif b & 0xF0 == 0xE0:
            length = 3
        elif b & 0xF8 == 0xF0:
            length = 4
        else:
            length = 1

        if pos + length > total_bytes:
            length = 1

        try:
            ch = text_bytes[pos:pos + length].decode('utf-8')
        except UnicodeDecodeError:
            ch = '\ufffd'

        return (ch, length)

    @staticmethod
    def _is_symbol_char(ch: str) -> bool:
        """Символ, допустимый внутри Lisp-символа (не терминатор)."""
        if ch in _SYMBOL_TERMINATORS:
            return False
        if ch in _WHITESPACE:
            return False
        return True

    def _prev_significant(self, text_bytes: bytes, pos: int) -> Optional[str]:
        """Ближайший не-пробельный символ перед pos (для head-of-list)."""
        i = pos
        while i > 0:
            i -= 1
            b = text_bytes[i]
            if b < 128 and chr(b) in _WHITESPACE:
                continue
            try:
                return text_bytes[i:i + 1].decode('utf-8')
            except UnicodeDecodeError:
                return None
        return None

    @staticmethod
    def _find_open_block_in_segment(seg: bytes) -> int:
        """Позиция незакрытого #| в отрезке одной строки, иначе -1."""
        in_string = False
        i, n = 0, len(seg)
        while i < n:
            c = seg[i]
            if in_string:
                if c == 92:  # backslash
                    i += 2
                    continue
                if c == 34:  # "
                    in_string = False
                i += 1
                continue
            if c == 59:  # ;
                return -1
            if c == 34:  # "
                in_string = True
                i += 1
                continue
            if c == 35 and i + 1 < n and seg[i + 1] == 124:  # #|
                close = seg.find(b'|#', i + 2)
                if close == -1:
                    return i
                i = close + 2
                continue
            i += 1
        return -1
