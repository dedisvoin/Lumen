"""
LumenLangLexer — кастомный лексер для .lm файлов (LumenLang/TwistLang).
Подсветка ключевых слов, типов, директив, макросов, строк, комментариев,
функций, операторов и фолдинг блоков.

Производительность: define-макросы, struct-имена и состояние блочных
комментариев /* */ кэшируются построчно и обновляются инкрементально из
SCN_MODIFIED; styleText не сканирует документ целиком на каждый paint,
фолдинг пересчитывается только когда изменились скобки.
"""

import re
from typing import List, Optional, Set, Tuple

from PyQt6.QtGui import QFont, QColor
from PyQt6.Qsci import QsciLexerCustom

from editor.scintilla_constants import (
    SCI_GETFOLDLEVEL,
    SCI_LINEFROMPOSITION,
    SCI_POSITIONFROMLINE,
    SCI_SETFOLDLEVEL,
)
from editor.constants import CODE_FONT_SIZE
from editor.font_settings import code_font_family


# Scintilla-константы для фолдинга (числовые — надёжнее атрибутов класса)
_FOLDLEVELBASE = 0x400        # SC_FOLDLEVELBASE = 1024
_FOLDLEVELHEADERFLAG = 0x2000 # SC_FOLDLEVELHEADERFLAG
_FOLDLEVELNUMBERMASK = 0x0FFF # SC_FOLDLEVELNUMBERMASK

# Биты SCN_MODIFIED (SC_MOD_INSERTTEXT / SC_MOD_DELETETEXT)
SC_MOD_INSERTTEXT = 0x1
SC_MOD_DELETETEXT = 0x2

# Правки крупнее этого объёма (вставка файла, setText) дешевле пересобрать целиком
_FULL_REBUILD_THRESHOLD = 8192


def _extract_define_macros(line: str) -> Set[str]:
    """Имена #define-макросов, объявленных на строке."""
    stripped = line.strip()
    if not stripped.startswith('#define'):
        return set()
    parts = stripped.split()
    if len(parts) < 2:
        return set()
    name = parts[1].rstrip('=:;,(){}[] \t')
    if name and name[0].isalpha():
        return {name}
    return set()


def _extract_macro_functions(line: str) -> Set[str]:
    """Имена #macro-функций, объявленных на строке."""
    return set(re.findall(r'^\s*#macro\s+([a-zA-Z_][a-zA-Z0-9_]*)', line))


def _extract_structs(line: str) -> Set[str]:
    """Имена struct, объявленных на строке."""
    return set(re.findall(r'\bstruct\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*(?:\:|\{)', line))


class LumenLangLexer(QsciLexerCustom):
    """
    Custom lexer for LumenLang (.lm) syntax highlighting.
    Поддерживает директивы #define, #macro, #include, #ifdef/#ifndef,
    макросы, структуры, namespace, и фолдинг по {[(.
    """

    STYLE_DEFAULT = 0
    STYLE_KEYWORD = 1
    STYLE_TYPE = 2
    STYLE_COMMENT = 3
    STYLE_STRING = 4
    STYLE_NUMBER = 5
    STYLE_OPERATOR = 6
    STYLE_FUNCTION = 7
    STYLE_MODIFIER = 8
    STYLE_DIRECTIVE = 9
    STYLE_LITERAL = 10
    STYLE_NAMESPACE_ID = 11
    STYLE_SPECIAL = 12
    STYLE_OBJECT = 13
    STYLE_ESCAPE = 14
    STYLE_DEFINE_MACRO = 15
    STYLE_COMMENT_DOC = 16       # //! комментарии
    STYLE_COMMENT_QUESTION = 17  # //? комментарии
    STYLE_DUNDER_METHOD = 18     # __method__
    STYLE_MACRO_FUNCTION = 19    # имя #macro-функции

    LINE_COMMENT = "//"  # комментарий строки (Ctrl+/)

    # Единый маппинг: стиль → (ключ темы, цвет по умолчанию).
    # fallback=None означает «использовать цвет текста темы» (STYLE_DEFAULT).
    _STYLE_THEME_KEYS = {
        STYLE_DEFAULT: ("default", None),
        STYLE_KEYWORD: ("keyword", QColor(203, 166, 247)),
        STYLE_TYPE: ("type", QColor(137, 220, 235)),
        STYLE_COMMENT: ("comment", QColor(108, 112, 134)),
        STYLE_STRING: ("string", QColor(166, 227, 161)),
        STYLE_NUMBER: ("number", QColor(250, 179, 135)),
        STYLE_OPERATOR: ("operator", QColor(137, 180, 250)),
        STYLE_FUNCTION: ("function_name", QColor(249, 226, 175)),
        STYLE_MODIFIER: ("modifier", QColor(203, 166, 247)),
        STYLE_DIRECTIVE: ("directive", QColor(243, 139, 168)),
        STYLE_LITERAL: ("literal", QColor(148, 226, 213)),
        STYLE_NAMESPACE_ID: ("class_name", QColor(137, 220, 235)),
        STYLE_SPECIAL: ("special", QColor(249, 226, 175)),
        STYLE_OBJECT: ("variable", QColor(148, 226, 213)),
        STYLE_ESCAPE: ("escape", QColor(249, 226, 175)),
        STYLE_DEFINE_MACRO: ("define_macro", QColor(243, 139, 168)),
        STYLE_COMMENT_DOC: ("comment_doc", QColor(255, 80, 80)),
        STYLE_COMMENT_QUESTION: ("comment_question", QColor(255, 200, 80)),
        STYLE_DUNDER_METHOD: ("dunder_method", QColor(203, 166, 247)),
        STYLE_MACRO_FUNCTION: ("macro_function", QColor(255, 121, 198)),
    }

    def __init__(self, parent=None, theme_data: Optional[dict] = None):
        super().__init__(parent)
        self._theme_data = theme_data or {}

        self.keywords = {
            'if', 'else', 'for', 'while', 'let', 'in', 'and', 'or', 'echo',
            'ret', 'assert', 'lambda', 'do', "is", "as", "ptr",
            'struct', 'enum', 'namespace', 'fn', 'continue', 'break', '__LINE__', '__FILE__', '__LUMEN__',
            'new', 'del', 'typeof', 'sizeof', 'out', 'outln',
            'input', 'exit', 'append', 'reserve', 'typeis', 'mod', 'eval',
        }
        self.modifiers = {'const', 'static', 'global', 'final', 'private', 'shadow', 'strict', 'ref'}
        self.types = {'str', 'char', 'bool', 'Type', 'Namespace',
                      'Func', 'Lambda', 'auto', 'Null', "any",
                      'i8', 'i16', 'i32', 'i64', 'f16', 'f32', 'f64',
                      'Array', 'Tuple'}
        self.literals = {'true', 'false', 'null', 'self', 'this'}
        self.directives = {'#define', '#macro', '#ifdef', '#elifdef', '#undef',
                           '#else', '#ifndef', '#include', '#cppinclude', '#pragma'}
        # Операторы-ключевые слова (new/del/typeof/...) красим как обычные
        # ключевые слова: в компайлере это KEYWORD-токены, как `let`/`if`.
        self.special_keywords: Set[str] = set()

        self.define_macros: Set[str] = set()
        self.macro_functions: Set[str] = set()
        self.struct_names: Set[str] = set()

        # Инкрементальный кэш документа: обновляется из SCN_MODIFIED,
        # чтобы styleText не делал полных сканов на каждый paint
        self._text: str = ""
        self._text_bytes: bytes = b""
        self._lines: List[str] = []
        self._line_macros: List[Set[str]] = []          # #define-имена на строку
        self._line_macro_fns: List[Set[str]] = []       # #macro-имена на строку
        self._line_structs: List[Set[str]] = []         # struct-имена на строку
        self._block_state: List[Tuple[bool, int]] = []  # (in_block, open_rel) на начало строки
        self._dirty: bool = True                        # текст изменился — обновить кэш
        self._hooked_editor = None
        self._folding_dirty: bool = False
        self._folding_line: int = 0

        self.setup_styles()

    # ------------------------------------------------------------------
    # Язык
    # ------------------------------------------------------------------

    def language(self) -> str:
        return "LumenLang"

    def wordCharacters(self) -> str:
        return "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_#"

    def description(self, style: int) -> str:
        descriptions = {
            self.STYLE_DEFAULT: "Default",
            self.STYLE_KEYWORD: "Keyword",
            self.STYLE_TYPE: "Type",
            self.STYLE_COMMENT: "Comment",
            self.STYLE_STRING: "String",
            self.STYLE_NUMBER: "Number",
            self.STYLE_OPERATOR: "Operator",
            self.STYLE_FUNCTION: "Function",
            self.STYLE_MODIFIER: "Modifier",
            self.STYLE_DIRECTIVE: "Directive",
            self.STYLE_LITERAL: "Literal",
            self.STYLE_NAMESPACE_ID: "Namespace",
            self.STYLE_SPECIAL: "Special",
            self.STYLE_OBJECT: "Object",
            self.STYLE_ESCAPE: "Escape sequence",
            self.STYLE_DEFINE_MACRO: "Define Macro",
            self.STYLE_COMMENT_DOC: "Documentation Comment",
            self.STYLE_COMMENT_QUESTION: "Question Comment",
            self.STYLE_DUNDER_METHOD: "Dunder Method",
            self.STYLE_MACRO_FUNCTION: "Macro Function",
        }
        return descriptions.get(style, "")

    def braceStyle(self) -> int:
        """Стиль скобок для StrictBraceMatch (operator): скобки в коде —
        operator, в комментариях — comment, поэтому ::не матчатся."""
        return self.STYLE_OPERATOR

    # ------------------------------------------------------------------
    # Темизация — интерфейс для ThemeManager
    # ------------------------------------------------------------------

    def set_theme(self, theme_data: dict):
        """Вызывается ThemeManager для применения цветовой темы."""
        self._theme_data = theme_data
        self.setup_styles()

    # ------------------------------------------------------------------
    # Настройка стилей
    # ------------------------------------------------------------------

    def setup_styles(self):
        theme = self._theme_data
        colors = theme.get("colors", {})
        styles = theme.get("styles", {})

        bg = QColor(colors.get("background", "#1b1d2e"))
        fg = QColor(colors.get("foreground", "#cdd6f4"))

        self.setDefaultPaper(bg)
        self.setDefaultColor(fg)

        # Размер шрифта — источник истины CodeEditor._font_size (зум).
        # Тема даёт только палитру: смена темы не должна пересоздавать
        # шрифты (прыжок размера + полный reflow документа на редакторе).
        size = (getattr(self.editor(), "_font_size", None)
                or styles.get("default", {}).get("size")
                or CODE_FONT_SIZE)
        base_font = QFont(code_font_family(), size)
        base_font.setFixedPitch(True)

        # Фон по умолчанию для всех стилей
        for style_id in range(20):
            self.setFont(base_font, style_id)
            self.setPaper(bg, style_id)

        # Цвета для каждого стиля из темы — единый маппинг _STYLE_THEME_KEYS
        for style_id, (key, fallback) in self._STYLE_THEME_KEYS.items():
            color = fg if fallback is None else fallback
            self.setColor(self._style_color(styles, key, color), style_id)

        # Специальные шрифты
        italic_font = QFont(base_font)
        italic_font.setItalic(True)
        self.setFont(italic_font, self.STYLE_COMMENT)
        self.setFont(italic_font, self.STYLE_COMMENT_DOC)
        self.setFont(italic_font, self.STYLE_COMMENT_QUESTION)

        bold_font = QFont(base_font)
        bold_font.setBold(True)
        self.setFont(bold_font, self.STYLE_KEYWORD)
        self.setFont(bold_font, self.STYLE_MACRO_FUNCTION)

        # Подчёркивание директив/макросов — из настройки
        # editor.underline_preprocessor (по умолчанию вкл; в C/C++ это же
        # поведение даёт #include/#define).  Запоминаем стили в
        # _underline_styles, чтобы CodeEditor._apply_font_size (зум) и смена
        # темы корректно переприменяли/сбрасывали underline.
        from editor.settings_store import app_settings
        underline_on = app_settings().get_bool("editor.underline_preprocessor", True)
        self._underline_styles = set()
        for style_id in (self.STYLE_MODIFIER, self.STYLE_DEFINE_MACRO):
            f = QFont(base_font)
            if style_id == self.STYLE_MODIFIER:
                f.setItalic(True)
            if underline_on:
                self._underline_styles.add(style_id)
                f.setUnderline(True)
            self.setFont(f, style_id)

        ed = self.editor()
        if ed is not None:
            for sid in self._STYLE_THEME_KEYS:
                ed.SendScintilla(2059, sid, 1 if sid in self._underline_styles else 0)

    @staticmethod
    def _style_color(styles: dict, key: str, fallback: QColor) -> QColor:
        """Достаёт цвет из стиля темы, если есть."""
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

        # Блочный комментарий /* */, открытый до start — стилизуем с его начала.
        # Состояние /* */ на начало каждой строки поддерживается в _on_modified.
        line = editor.SendScintilla(SCI_LINEFROMPOSITION, start)
        in_block, open_rel = (
            self._block_state[line] if line < len(self._block_state) else (False, -1)
        )
        if in_block:
            if open_rel < 0:
                open_rel = 0
            start = editor.SendScintilla(SCI_POSITIONFROMLINE, line) + open_rel
        elif open_rel < 0:
            # /* мог открыться в той же строке между её началом и start
            line_start = editor.SendScintilla(SCI_POSITIONFROMLINE, line)
            rel = self._find_open_block_in_segment(text_bytes[line_start:start])
            if rel >= 0:
                start = line_start + rel

        if start >= end:
            return

        self.startStyling(start)

        pos = start
        expecting_namespace = False

        while pos < end:
            ch, ch_len = self._get_char_at(text_bytes, pos, total_bytes)
            if ch_len == 0:
                break

            # --- Комментарии (//, //!, //?) ---
            if ch == '/' and pos + ch_len < end:
                next_ch, next_len = self._get_char_at(text_bytes, pos + ch_len, total_bytes)
                if next_ch == '/':
                    expecting_namespace = False
                    j = pos
                    comment_style = self.STYLE_COMMENT

                    # Третий символ: ! или ?
                    if j + ch_len + next_len < end:
                        third_ch, third_len = self._get_char_at(
                            text_bytes, j + ch_len + next_len, total_bytes
                        )
                        if third_ch == '!':
                            comment_style = self.STYLE_COMMENT_DOC
                            j += third_len
                        elif third_ch == '?':
                            comment_style = self.STYLE_COMMENT_QUESTION
                            j += third_len

                    while j < end:
                        c, l = self._get_char_at(text_bytes, j, total_bytes)
                        if c == '\n':
                            j += l
                            break
                        j += l
                    self.setStyling(j - pos, comment_style)
                    pos = j
                    continue

                if next_ch == '*':
                    # Блочный комментарий /* ... */
                    expecting_namespace = False
                    close = text_bytes.find(b'*/', pos + ch_len + next_len,
                                            total_bytes)
                    if close == -1:
                        close = total_bytes
                        end = total_bytes  # не закрыт — стилизуем до конца
                    else:
                        close += 2
                    self.setStyling(close - pos, self.STYLE_COMMENT)
                    pos = close
                    continue

            # --- Строки ---
            if ch in ('"', "'"):
                quote_char = ch
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
                            _, next_l = self._get_char_at(text_bytes, j, total_bytes)
                            esc_len += next_l
                            j += next_l
                        self.setStyling(esc_len, self.STYLE_ESCAPE)

                    elif c == quote_char:
                        self.setStyling(l, self.STYLE_STRING)
                        j += l
                        break
                    else:
                        self.setStyling(l, self.STYLE_STRING)
                        j += l

                pos = j
                continue

            # --- Числа ---
            if ch.isdigit() or (ch == '.' and pos + ch_len < end and
                                self._get_char_at(text_bytes, pos + ch_len, total_bytes)[0].isdigit()):
                expecting_namespace = False
                j = pos
                has_dot = False
                while j < end:
                    c, l = self._get_char_at(text_bytes, j, total_bytes)
                    if not (c.isdigit() or (c == '.' and not has_dot)):
                        break
                    if c == '.':
                        has_dot = True
                    j += l
                self.setStyling(j - pos, self.STYLE_NUMBER)
                pos = j
                continue

            # --- Указатели ***auto, **auto, *auto ---
            if ch == '*' and pos + ch_len < end:
                star_start = pos
                star_count = 0
                temp_pos = pos

                while temp_pos < end:
                    c, l = self._get_char_at(text_bytes, temp_pos, total_bytes)
                    if c == '*':
                        star_count += 1
                        temp_pos += l
                    else:
                        break

                if star_count > 0:
                    c, l = self._get_char_at(text_bytes, temp_pos, total_bytes)
                    if c == 'a':
                        j = temp_pos
                        while j < end:
                            c, l = self._get_char_at(text_bytes, j, total_bytes)
                            if not (c.isalnum() or c == '_'):
                                break
                            j += l
                        word = text_bytes[temp_pos:j].decode('utf-8')
                        if word == 'auto':
                            self.setStyling(j - pos, self.STYLE_TYPE)
                            pos = j
                            continue

                expecting_namespace = False
                self.setStyling(ch_len, self.STYLE_OPERATOR)
                pos += ch_len
                continue

            # --- Идентификаторы / слова ---
            if ch.isalpha() or ch == '_' or ch == '#':
                j = pos
                while j < end:
                    c, l = self._get_char_at(text_bytes, j, total_bytes)
                    if not (c.isalnum() or c == '_' or c == '#'):
                        break
                    j += l

                word = text_bytes[pos:j].decode('utf-8')
                style = self.STYLE_DEFAULT

                if word == "namespace" or word == "struct" or word == "enum":
                    expecting_namespace = True
                    style = self.STYLE_KEYWORD
                elif expecting_namespace:
                    style = self.STYLE_NAMESPACE_ID
                    expecting_namespace = False
                else:
                    if word in self.macro_functions:
                        style = self.STYLE_MACRO_FUNCTION
                    elif word in self.define_macros:
                        style = self.STYLE_DEFINE_MACRO
                    elif word in self.directives:
                        style = self.STYLE_DIRECTIVE
                    elif word.startswith('__') and word.endswith('__') and len(word) > 4:
                        style = self.STYLE_DUNDER_METHOD
                    elif word in self.keywords:
                        style = self.STYLE_KEYWORD
                    elif word in self.modifiers:
                        style = self.STYLE_MODIFIER
                    elif word in self.types:
                        style = self.STYLE_TYPE
                    elif word in self.struct_names:
                        style = self.STYLE_NAMESPACE_ID
                    elif word in self.literals:
                        style = self.STYLE_LITERAL
                    elif j + 2 <= end and text_bytes[j:j+2] == b'::':
                        style = self.STYLE_NAMESPACE_ID
                    else:
                        nch, npos = self._peek_nonws(text_bytes, j, total_bytes)
                        if nch == '(':
                            style = self.STYLE_FUNCTION
                        elif nch == '[' and self._is_generic_call(text_bytes, npos, total_bytes):
                            style = self.STYLE_FUNCTION
                        elif nch == '.':
                            style = self.STYLE_OBJECT

                self.setStyling(j - pos, style)
                pos = j
                continue

            # --- Операторы ---
            if ch in '+-*/%$=&@|^!<>~,?.:;(){}[]':
                expecting_namespace = False
                j = pos + ch_len

                if j < end:
                    next_ch, next_len = self._get_char_at(text_bytes, j, total_bytes)
                    multi_op = (ch == ':' and next_ch == ':') or \
                               (ch == '-' and next_ch == '>') or \
                               (ch == '<' and next_ch == '=') or \
                               (ch == '>' and next_ch == '=') or \
                               (ch == '=' and next_ch == '=') or \
                               (ch == '!' and next_ch == '=') or \
                               (ch == '<' and next_ch == '<') or \
                               (ch == '>' and next_ch == '>')
                    if multi_op:
                        j += next_len
                self.setStyling(j - pos, self.STYLE_OPERATOR)
                pos = j
                continue

            # --- Всё остальное ---
            self.setStyling(ch_len, self.STYLE_DEFAULT)
            pos += ch_len

        self.update_folding_levels(start, end)

    # ------------------------------------------------------------------
    # Поддержка фолдинга
    # ------------------------------------------------------------------

    # ------------------------------------------------------------------
    # Инкрементальный кэш (SCN_MODIFIED): макросы, struct, состояние /* */
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
        """Обновляет кэш текста: 1 полный проход на правку (C-скорость)."""
        text = editor.text()
        self._text = text
        self._text_bytes = text.encode('utf-8')
        self._lines = text.split('\n')
        if len(self._line_macros) != len(self._lines):
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
        # Qsci всегда отдаёт line=0 — строку правки считаем по позиции (байты)
        line = editor.SendScintilla(SCI_LINEFROMPOSITION, pos)
        if length > _FULL_REBUILD_THRESHOLD:
            # Крупная правка (вставка файла, setText) — пересобрать всё
            self._rebuild_from_editor(editor)
            return
        self._splice_line_lists(line, lines_added)
        n_changed = 1 + max(lines_added, 0)
        self._rescan_affected_lines(editor, line, n_changed)
        self._recompute_block_states(editor, line, n_changed)
        # Вариант 2: при любой правке текста помечаем фолдинг «грязным» и
        # пересчитываем файл целиком (как при открытии). База SC_FOLDLEVELBASE,
        # поэтому двойной учёт скобок предыдущей строки не играет роли и уровни
        # всегда корректны. Аргумент text из SCN_MODIFIED для одиночных вставок
        # часто пуст, поэтому не полагаемся на него.
        self._folding_dirty = True
        self._folding_line = 0
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
        """Полная пересборка построчных кэшей из self._lines."""
        macros: Set[str] = set()
        macro_fns: Set[str] = set()
        structs: Set[str] = set()
        line_macros = []
        line_macro_fns = []
        line_structs = []
        block_state = [(False, -1)]
        in_block, open_rel = False, -1
        for line_text in self._lines:
            m = _extract_define_macros(line_text)
            mf = _extract_macro_functions(line_text)
            s = _extract_structs(line_text)
            line_macros.append(m)
            line_macro_fns.append(mf)
            line_structs.append(s)
            macros.update(m)
            macro_fns.update(mf)
            structs.update(s)
            in_block, open_rel = self._line_state_after(in_block, open_rel, line_text)
            block_state.append((in_block, open_rel))
        self.define_macros = macros
        self.macro_functions = macro_fns
        self.struct_names = structs
        self._line_macros = line_macros
        self._line_macro_fns = line_macro_fns
        self._line_structs = line_structs
        self._block_state = block_state
        self._folding_dirty = True  # первый прогон фолдинга после пересборки
        self._folding_line = 0

    def _splice_line_lists(self, line: int, lines_added: int):
        """Выравнивает построчные кэши под новое число строк документа."""
        if lines_added > 0:
            p = (False, -1)
            self._block_state[line + 1:line + 1] = [p] * lines_added
            self._line_macros[line + 1:line + 1] = [set()] * lines_added
            self._line_macro_fns[line + 1:line + 1] = [set()] * lines_added
            self._line_structs[line + 1:line + 1] = [set()] * lines_added
        elif lines_added < 0:
            # Строка `line` (объединённая) сохраняется — её стартовое
            # состояние валидно; удаляются только полностью съеденные строки
            hi = line + 1 - lines_added
            for i in range(line + 1, hi):
                if self._line_macros[i]:
                    self.define_macros.difference_update(self._line_macros[i])
                if self._line_macro_fns[i]:
                    self.macro_functions.difference_update(self._line_macro_fns[i])
                if self._line_structs[i]:
                    self.struct_names.difference_update(self._line_structs[i])
            del self._block_state[line + 1:hi]
            del self._line_macros[line + 1:hi]
            del self._line_macro_fns[line + 1:hi]
            del self._line_structs[line + 1:hi]

    def _rescan_affected_lines(self, editor, start_line: int, count: int):
        """Пересканирует изменившиеся строки: #define-макросы и struct-имена."""
        for i in range(start_line, min(start_line + count, len(self._line_macros))):
            old_m = self._line_macros[i]
            old_mf = self._line_macro_fns[i]
            old_s = self._line_structs[i]
            if old_m:
                self.define_macros.difference_update(old_m)
            if old_mf:
                self.macro_functions.difference_update(old_mf)
            if old_s:
                self.struct_names.difference_update(old_s)
            line_text = editor.text(i)
            new_m = _extract_define_macros(line_text)
            new_mf = _extract_macro_functions(line_text)
            new_s = _extract_structs(line_text)
            self._line_macros[i] = new_m
            self._line_macro_fns[i] = new_mf
            self._line_structs[i] = new_s
            if new_m:
                self.define_macros.update(new_m)
            if new_mf:
                self.macro_functions.update(new_mf)
            if new_s:
                self.struct_names.update(new_s)

    def _recompute_block_states(self, editor, start_line: int, n_changed: int):
        """Пересчитывает состояние /* */ для изменившихся строк и далее
        до стабилизации (фолдинг-аналог: строки с неизменным текстом
        сохраняют состояние, поэтому цикл быстро сходится)."""
        bs = self._block_state
        n = len(bs) - 1  # индекс последней строки
        # Фаза 1: строки, чей текст гарантированно изменился
        end_phase1 = min(start_line + n_changed, n)
        for i in range(start_line, end_phase1):
            in_b, open_r = bs[i]
            bs[i + 1] = self._line_state_after(in_b, open_r, editor.text(i))
        # Фаза 2: пока новое состояние отличается от кэшированного
        i = end_phase1
        while i < n:
            in_b, open_r = bs[i]
            new_state = self._line_state_after(in_b, open_r, editor.text(i))
            if new_state == bs[i + 1]:
                break
            bs[i + 1] = new_state
            i += 1

    def _mark_folding(self, changed_text, line: int):
        """Скобки изменились — фолдинг пересчитается при следующем styleText."""
        if not any(c in b'{}()[]' for c in changed_text):
            return
        if self._folding_dirty:
            self._folding_line = min(self._folding_line, line)
        else:
            self._folding_dirty = True
            self._folding_line = line

    def _line_state_after(self, in_block: bool, open_rel: int, line_text: str,
                          base: int = 0):
        """Состояние /* */ в конце строки; open_rel — смещение /* в строке."""
        if in_block:
            idx = line_text.find('*/', base)
            if idx == -1:
                return (True, open_rel)
            return self._line_state_after(False, -1, line_text, idx + 2)
        i, n = base, len(line_text)
        while i < n:
            c = line_text[i]
            if c == '/' and i + 1 < n:
                nxt = line_text[i + 1]
                if nxt == '/':
                    break
                if nxt == '*':
                    close = line_text.find('*/', i + 2)
                    if close == -1:
                        return (True, i)
                    i = close + 2  # закрылся в этой же строке
                    continue
                i += 2
                continue
            if c in ('"', "'"):
                q = c
                i += 1
                while i < n:
                    ch = line_text[i]
                    if ch == '\\':
                        i += 2
                        continue
                    if ch == q:
                        i += 1
                        break
                    i += 1
                continue
            i += 1
        return (False, -1)

    def _count_braces_safely(self, text: str):
        """
        Подсчитывает {[( и )]} игнорируя строки, // и /* */ комментарии.
        Возвращает (open_count, close_count).
        """
        open_count = 0
        close_count = 0
        in_string = None
        i = 0
        n = len(text)

        while i < n:
            ch = text[i]
            if in_string:
                if ch == '\\' and i + 1 < n:
                    i += 2
                    continue
                elif ch == in_string:
                    in_string = None
                i += 1
                continue

            if ch in ('"', "'"):
                in_string = ch
                i += 1
            elif ch == '/' and i + 1 < n:
                if text[i + 1] == '/':
                    break  # остаток строки — комментарий
                if text[i + 1] == '*':
                    # Блочный комментарий: пропускаем до */
                    end_c = text.find('*/', i + 2)
                    i = n if end_c == -1 else end_c + 2
                    continue
            elif ch in ('{', '(', '['):
                open_count += 1
            elif ch in ('}', ')', ']'):
                close_count += 1
            i += 1

        return open_count, close_count

    def update_folding_levels(self, start: int, end: int):
        """Уровни фолдинга как в C++ — по фигурным скобкам.

        Запускается только при изменении скобок (флаг из _on_modified);
        строки берутся из кэша, без editor.text(line) на каждую строку.
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

        # Текущий уровень вложенности (базовый уровень Scintilla = 1024)
        current_level = _FOLDLEVELBASE
        if first > 0:
            # Продолжаем с предыдущей строки: уровень + её изменение скобок.
            # Если не применить net_change, строка после закрывающей } получит
            # завышенный уровень и фолд растянется до конца файла.
            prev_level = editor.SendScintilla(SCI_GETFOLDLEVEL, first - 1)
            current_level = prev_level & _FOLDLEVELNUMBERMASK
            prev_opens, prev_closes = self._count_braces_safely(self._lines[first - 1])
            current_level += prev_opens - prev_closes
            if current_level < _FOLDLEVELBASE:
                current_level = _FOLDLEVELBASE

        for line in range(first, total_lines):
            open_count, close_count = self._count_braces_safely(self._lines[line])
            net_change = open_count - close_count

            # Заголовок (фолд-точка) ставится, если строка открывает блок
            is_header = _FOLDLEVELHEADERFLAG if net_change > 0 else 0
            new_fold_level = current_level | is_header

            # Оптимизация: дальше ничего не меняется — прерываем цикл
            if line > end_line and net_change == 0:
                old_fold_level = editor.SendScintilla(SCI_GETFOLDLEVEL, line)
                if (
                    (old_fold_level & _FOLDLEVELNUMBERMASK) == (new_fold_level & _FOLDLEVELNUMBERMASK)
                    and bool(old_fold_level & _FOLDLEVELHEADERFLAG) == bool(new_fold_level & _FOLDLEVELHEADERFLAG)
                ):
                    break

            editor.SendScintilla(SCI_SETFOLDLEVEL, line, new_fold_level)

            current_level += net_change
            if current_level < _FOLDLEVELBASE:
                current_level = _FOLDLEVELBASE

    # ------------------------------------------------------------------
    # Утилиты для работы с UTF-8
    # ------------------------------------------------------------------

    def _peek_nonws(self, text_bytes: bytes, pos: int, total_bytes: int):
        """Первый не-пробельный символ, начиная с pos (в пределах end).

        Возвращает (символ, позиция). За именем функции (определением или
        вызовом) идёт '(' или '[' (генерик-вызов/генерик-определение),
        возможно через пробелы.
        """
        p = pos
        while p < total_bytes:
            c, l = self._get_char_at(text_bytes, p, total_bytes)
            if c is None:
                break
            if c.isspace():
                p += l
                continue
            return c, p
        return None, -1

    def _is_generic_call(self, text_bytes: bytes, pos: int, total_bytes: int):
        """pos указывает на '['. True, если сразу после парной ']' идёт '('.

        Различает генерик-вызов/определение `foo[...](...)` от индексации
        `arr[index]` (где после ']' вызова нет). Учитывает вложенные
        скобки и строковые литералы внутри [...].
        """
        depth = 0
        p = pos
        while p < total_bytes:
            c, l = self._get_char_at(text_bytes, p, total_bytes)
            if c is None:
                return False
            if c in ('"', "'"):
                q = c
                p += l
                while p < total_bytes:
                    sc, sl = self._get_char_at(text_bytes, p, total_bytes)
                    if sc is None:
                        return False
                    if sc == '\\':
                        p += 2 * sl
                        continue
                    p += sl
                    if sc == q:
                        break
                continue
            if c in ('[', '('):
                depth += 1
            elif c in (']', ')'):
                depth -= 1
                if depth == 0:
                    q = p + l
                    while q < total_bytes:
                        nc, nl = self._get_char_at(text_bytes, q, total_bytes)
                        if nc is None:
                            return False
                        if nc.isspace():
                            q += nl
                            continue
                        return nc == '('
            p += l
        return False

    @staticmethod
    def _get_char_at(text_bytes: bytes, pos: int, total_bytes: int) -> Tuple[Optional[str], int]:
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
    def _find_open_block_in_segment(seg: bytes) -> int:
        """Позиция незакрытого /* в отрезке одной строки, иначе -1."""
        in_string = None
        i, n = 0, len(seg)
        while i < n:
            c = seg[i]
            if in_string:
                if c == 92:
                    i += 2
                    continue
                if c == in_string:
                    in_string = None
                i += 1
                continue
            if c == 47 and i + 1 < n:
                nxt = seg[i + 1]
                if nxt == 47:
                    return -1  # // — остаток строки не код
                if nxt == 42:
                    close = seg.find(b'*/', i + 2)
                    if close == -1:
                        return i
                    i = close + 2
                    continue
            if c in (34, 39):
                in_string = c
            i += 1
        return -1
