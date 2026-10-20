"""
SemanticLexer — лексер-заглушка для языков с LSP-семантикой.

Стили даёт LSP-сервер через семантические токены (editor/lsp); недостающие
серверные категории (keyword/number/string/comment) дорисовывает лёгкий
байтовый лексер, параметризуемый под язык (ключевые слова, комментарии,
строки). QScintilla не применяет SCI_SETSTYLING без лексера, поэтому нужен
QsciLexerCustom.
"""

import logging
import re
from typing import Dict, List, Optional, Tuple

_log = logging.getLogger("semantic_lexer")

from PyQt6.QtGui import QColor, QFont
from PyQt6.Qsci import QsciLexerCustom

from editor.constants import CODE_FONT_SIZE, FALLBACK_FG
from editor.font_settings import code_font_family
from editor.settings_store import app_settings
from editor.scintilla_constants import (
    SCI_GETFOLDLEVEL, SCI_GETLINEENDPOSITION, SCI_GETSTYLEAT,
    SCI_LINEFROMPOSITION, SCI_POSITIONFROMLINE, SCI_SETFOLDLEVEL,
)
from editor.text_utils import CPP_KEYWORDS, CPP_TYPES, NUMBER_PATTERN

# Scintilla-константы фолдинга (числовые — как в lumen_lang.py)
_FOLDLEVELBASE = 0x400        # SC_FOLDLEVELBASE = 1024
_FOLDLEVELHEADERFLAG = 0x2000 # SC_FOLDLEVELHEADERFLAG
_FOLDLEVELNUMBERMASK = 0x0FFF # SC_FOLDLEVELNUMBERMASK

_STYLE_KEYWORD = 1
_STYLE_MODIFIER = 2
_STYLE_TYPE = 3
_STYLE_CLASS = 4
_STYLE_FUNCTION = 5
_STYLE_VARIABLE = 6
_STYLE_LITERAL = 7
_STYLE_MACRO = 8
_STYLE_COMMENT = 9
_STYLE_STRING = 10
_STYLE_NUMBER = 11
_STYLE_OPERATOR = 12
_STYLE_NAMESPACE = 13
_STYLE_SELF = 14
_STYLE_DUNDER = 15
_STYLE_COMMENT_DOC = 16      # //! комментарий-указание
_STYLE_COMMENT_QUESTION = 17 # //? комментарий-вопрос/warning
_STYLE_TYPE_PARAM = 18       # дженерик-параметр (typeParameter)
_STYLE_MACRO_FUNCTION = 19   # имя #macro-функции (macroFunction)

# Языки, где __method__ — настоящий dunder (не C++-интринсик вроде __func__).
_DUNDER_LANGUAGES = frozenset({"python", "lumenlang"})

# tokenType clangd -> (id стиля, ключ стиля темы, bold по умолч., italic по умолч.)
_SEM_TOKEN_MAP = {
    "keyword":       (_STYLE_KEYWORD, "keyword", False, False),
    "modifier":      (_STYLE_MODIFIER, "modifier", False, True),
    "type":          (_STYLE_TYPE, "type", False, False),
    "typeParameter": (_STYLE_TYPE_PARAM, "type_parameter", False, True),
    "typeAlias":     (_STYLE_TYPE, "type", False, False),
    "primitive":     (_STYLE_TYPE, "type", False, False),
    "concept":       (_STYLE_TYPE, "type", False, False),
    "class":         (_STYLE_CLASS, "class_name", True, False),
    "struct":        (_STYLE_CLASS, "class_name", True, False),
    "enum":          (_STYLE_CLASS, "class_name", True, False),
    "interface":     (_STYLE_CLASS, "class_name", True, False),
    "namespace":     (_STYLE_NAMESPACE, "namespace", False, False),
    "function":      (_STYLE_FUNCTION, "function_name", False, False),
    "method":        (_STYLE_FUNCTION, "function_name", False, False),
    "variable":      (_STYLE_VARIABLE, "variable", False, False),
    "parameter":     (_STYLE_VARIABLE, "variable", False, False),
    "selfParameter": (_STYLE_SELF, "self", False, False),
    "property":      (_STYLE_VARIABLE, "variable", False, False),
    "member":        (_STYLE_VARIABLE, "variable", False, False),
    "event":         (_STYLE_VARIABLE, "variable", False, False),
    "enumMember":    (_STYLE_LITERAL, "literal", False, False),
    "macro":         (_STYLE_MACRO, "define_macro", False, False),
    "macroFunction": (_STYLE_MACRO_FUNCTION, "macro_function", False, False),
    "decorator":     (_STYLE_MACRO, "directive", False, False),
    "comment":       (_STYLE_COMMENT, "comment", False, True),
    "string":        (_STYLE_STRING, "string", False, False),
    "regexp":        (_STYLE_STRING, "string", False, False),
    "number":        (_STYLE_NUMBER, "number", False, False),
    "operator":      (_STYLE_OPERATOR, "operator", False, False),
    "bracket":       (_STYLE_OPERATOR, "operator", False, False),
}

_SEM_STYLE_NAMES = {
    0: "Default",
    _STYLE_KEYWORD: "Keyword",
    _STYLE_MODIFIER: "Modifier",
    _STYLE_TYPE: "Type",
    _STYLE_CLASS: "Class",
    _STYLE_FUNCTION: "Function",
    _STYLE_VARIABLE: "Variable",
    _STYLE_LITERAL: "Literal",
    _STYLE_MACRO: "Macro",
    _STYLE_COMMENT: "Comment",
    _STYLE_STRING: "String",
    _STYLE_NUMBER: "Number",
    _STYLE_OPERATOR: "Operator",
    _STYLE_NAMESPACE: "Namespace",
    _STYLE_SELF: "Self",
    _STYLE_DUNDER: "DunderMethod",
    _STYLE_COMMENT_DOC: "DocComment",
    _STYLE_COMMENT_QUESTION: "QuestionComment",
    _STYLE_TYPE_PARAM: "TypeParameter",
    _STYLE_MACRO_FUNCTION: "MacroFunction",
}

# --- лексический fallback (то, чего LSP-сервер не шлёт) ---
# Списки по умолчанию — C++ (исторический дефолт), переопределяются в __init__.

_TYPE_KWS = CPP_TYPES
_KEYWORDS = CPP_KEYWORDS

_LB = rb"(?<![A-Za-z0-9_])"
_RB = rb"(?![A-Za-z0-9_])"

_OP_CHARS = b"=+-*/%<>!&|^~?:.,;()[]{}"


def _is_ident(b: int) -> bool:
    """ASCII-буква/цифра/_ — граница слова при поиске вхождений имён.
    (кириллица и прочие многобайтовые байты — не идентификатор, для
    русских имён граница считается корректно)"""
    return (b == 0x5F or 0x30 <= b <= 0x39
            or 0x41 <= b <= 0x5A or 0x61 <= b <= 0x7A)


def _is_dunder(name: bytes) -> bool:
    """`__method__` — ровно две черты с обоих концов и непустая середина.

    Работает на bytes: путь вызова — styleText, там декодировать каждое
    имя на каждый keystroke нельзя.
    """
    return len(name) >= 5 and name[:2] == b"__" and name[-2:] == b"__"

# Приоритет при пересечении span'ов: строка/комментарий > LSP > лексер
_PRIO_SEMANTIC = 3
_PRIO_LEX = 1
_PRIO_LEX_OVERRIDE = 5  # выше LSP — директивы и комментарии не перекрываются


class SemanticLexer(QsciLexerCustom):
    """Стили: span'ы LSP-сервера + лёгкий лексер (keyword/number/string/comment).

    Параметры подстраивают fallback-лексику под язык: ключевые слова, типы,
    комментарии (строковый/блочный) и тройные кавычки Python.
    """

    def __init__(self, parent=None,
                 keywords=_KEYWORDS,
                 type_keywords=_TYPE_KWS,
                 line_comment: str = "//",
                 block_comments: bool = True,
                 triple_quotes: bool = False,
                 language: str = "SemanticC++"):
        super().__init__(parent)
        self._language = language
        self._line_comment = line_comment
        self._spans: List[Tuple[int, int, int]] = []
        self._spans_stale = False
        self._pending_edits: List[Tuple[int, int]] = []
        self._symbol_names: Dict[str, str] = {}  # имя -> имя_токена (documentSymbol)

        def alt(words):
            if not words:
                return rb"(?!)"
            return rb"(?:" + b"|".join(w.encode() for w in words) + rb")"

        self._kw_re = re.compile(_LB + alt(keywords) + _RB)
        self._type_re = re.compile(_LB + alt(type_keywords) + _RB)
        self._number_re = re.compile(
            _LB + NUMBER_PATTERN.encode() + _RB
        )
        lc = re.escape(line_comment).encode()
        self._line_comment_re = re.compile(lc + rb"[^\n]*")
        self._block_comment_re = (re.compile(rb"/\*.*?\*/", re.DOTALL)
                                  if block_comments else None)
        self._string_res = []
        if triple_quotes:
            self._string_res.append(
                re.compile(rb'"""[\s\S]*?"""|\'\'\'[\s\S]*?\'\'\''))
        self._string_res.append(re.compile(rb'"(?:\\.|[^"\\\n])*"'))
        self._char_re = re.compile(rb"'(?:\\.|[^'\\\n])*'")
        self._preproc_re = re.compile(rb"(?m)^[ \t]*#\w+")
        # #macro NAME — имя макрофункции (группа 1), для lumenlang-fallback
        self._macro_fn_re = re.compile(
            rb"(?m)^[ \t]*#macro[ \t]+([A-Za-z_][A-Za-z0-9_]*)")

    def language(self) -> str:
        return self._language

    @property
    def line_comment(self) -> str:
        """Префикс строкового комментария ("//", "#"...) для этого языка."""
        return self._line_comment

    def description(self, style: int) -> str:
        return _SEM_STYLE_NAMES.get(style, "")

    def braceStyle(self) -> int:
        """Стиль скобок для StrictBraceMatch: скобки в коде красит LSP
        как operator, в комментариях они остаются comment — поэтому пары
        внутри /* */ и // больше не подсвечиваются."""
        return _STYLE_OPERATOR

    def set_spans(self, spans: List[Tuple[int, int, int]]):
        if _log.isEnabledFor(logging.DEBUG):
            _log.debug("set_spans: %d spans, clearing stale edits=%s",
                       len(spans), self._pending_edits[:3])
        self._spans = spans
        self._spans_stale = False
        self._pending_edits = []

    def set_spans_keep_stale(self, spans: List[Tuple[int, int, int]]):
        """Store fresh spans but keep _spans_stale and _pending_edits.

        Used when fresh spans arrive from the server but haven't been applied
        to Scintilla yet (SCI_COLOURISE pending). Any edits that arrive between
        set_spans_keep_stale and the subsequent SCI_COLOURISE will be applied
        via stale-shift in _collect, so the final styling is correct.
        """
        if _log.isEnabledFor(logging.DEBUG):
            _log.debug("set_spans_keep_stale: %d spans, keeping stale=%s "
                       "edits=%s", len(spans), self._spans_stale,
                       self._pending_edits[:3])
        self._spans = spans

    def clear_stale(self):
        """Clear stale state after SCI_COLOURISE has applied spans."""
        if _log.isEnabledFor(logging.DEBUG) and (self._spans_stale or self._pending_edits):
            _log.debug("clear_stale: was stale=%s edits=%s",
                       self._spans_stale, self._pending_edits[:3])
        self._spans_stale = False
        self._pending_edits = []

    def add_edit(self, pos: int, delta: int):
        """Правка (позиция, изменение длины): старые токены после неё сдвигаем."""
        if _log.isEnabledFor(logging.DEBUG):
            _log.debug("add_edit: pos=%d delta=%d (total edits=%d)",
                       pos, delta, len(self._pending_edits) + 1)
        self._pending_edits.append((pos, delta))

    def set_spans_stale(self, stale: bool):
        """После правки координаты clangd устарели — не рисовать их."""
        self._spans_stale = stale

    def set_symbol_names(self, names: Dict[str, str]):
        """Имена символов (классы/функции/... из documentSymbol LSP): ими
        подсвечиваются и вхождения имён (использование), не только
        определения, которые приходят точными токенами в set_spans."""
        self._symbol_names = dict(names)

    def set_theme(self, theme: dict):
        """Вызывается ThemeManager.apply_theme для кастомных лексеров."""
        styles = theme.get("styles", {})
        default = styles.get("default", {})
        size = getattr(self.editor(), "_font_size", None) or default.get("size", CODE_FONT_SIZE)
        base_font = QFont(code_font_family(), size)
        base_font.setFixedPitch(True)
        self.setColor(QColor(default.get("color")))

        seen = {}
        for style_id, key, bold, italic in _SEM_TOKEN_MAP.values():
            seen.setdefault(style_id, (key, bold, italic))
        seen[_STYLE_SELF] = ("self", False, False)
        seen[_STYLE_DUNDER] = ("dunder_method", False, False)
        seen[_STYLE_COMMENT_DOC] = ("comment_doc", False, False)
        seen[_STYLE_COMMENT_QUESTION] = ("comment_question", False, True)
        seen[_STYLE_TYPE_PARAM] = ("type_parameter", False, True)
        seen[_STYLE_MACRO_FUNCTION] = ("macro_function", True, False)

        self._underline_styles = set()
        for style_id, (key, bold, italic) in seen.items():
            data = styles.get(key) or (styles.get("variable") if key in ("self", "dunder_method") else {}) or {}
            if key == "type_parameter":
                data = data or styles.get("type") or {}
            underline = bool(data.get("underline", False))
            # Настройка editor.underline_preprocessor: подчёркивать
            # директивы препроцессора (#include/#define/…) только в C/C++.
            if (style_id == _STYLE_MACRO and self._language == "cpp"
                    and not app_settings().get_bool("editor.underline_preprocessor", True)):
                underline = False
            if underline:
                self._underline_styles.add(style_id)

            self.setColor(QColor(data.get("color")), style_id)
            f = QFont(base_font)
            f.setBold(bool(data.get("bold", bold)))
            f.setItalic(bool(data.get("italic", italic)))
            f.setUnderline(underline)
            self.setFont(f, style_id)

        ed = self.editor()
        if ed is not None:
            for sid in range(128):
                if self.description(sid):
                    ed.SendScintilla(2059, sid, 1 if sid in self._underline_styles else 0)

    # --- подсветка ---

    _call_counter = 0

    def styleText(self, start: int, end: int):
        SemanticLexer._call_counter += 1
        _cid = SemanticLexer._call_counter
        editor = self.editor()
        if editor is None:
            return
        # DEBUG: log styleText entry with stale state
        if _log.isEnabledFor(logging.DEBUG):
            _log.debug("styleText#%d ENTRY start=%d end=%d stale=%s "
                       "edits=%s spans_count=%d",
                       _cid, start, end, self._spans_stale,
                       self._pending_edits[:3], len(self._spans))
        start_line = editor.SendScintilla(SCI_LINEFROMPOSITION, start)
        line_start = editor.SendScintilla(SCI_POSITIONFROMLINE, start_line)
        end_line = editor.SendScintilla(SCI_LINEFROMPOSITION, end)
        parts = []
        for i in range(start_line, end_line + 1):
            line = editor.text(i)
            if line.endswith("\r\n"):
                parts.append(line[:-2].encode("utf-8"))
                parts.append(b"\r\n")
            elif line.endswith("\n"):
                parts.append(line[:-1].encode("utf-8"))
                parts.append(b"\n")
            else:
                parts.append(line.encode("utf-8"))
        text = b"".join(parts)
        spans = self._collect(text, 0, end - line_start,
                              span_offset=line_start)
        # DEBUG: log spans being applied and stale state
        if _log.isEnabledFor(logging.DEBUG) and spans:
            _log.debug("styleText#%d(%d,%d) stale=%s edits=%s "
                       "span_offset=%d spans(first10)=%s",
                       _cid, start, end, self._spans_stale,
                       self._pending_edits[:5], line_start, spans[:10])
        pos = line_start
        self.startStyling(line_start, 0xff)
        for s, length, style in spans:
            if (style == _STYLE_VARIABLE and self._language == "python"
                    and text[s:s + length] in (b"self", b"cls")):
                style = _STYLE_SELF
            elif (style == _STYLE_FUNCTION and self._language in _DUNDER_LANGUAGES):
                if _is_dunder(text[s:s + length]):
                    style = _STYLE_DUNDER
            s += line_start
            if s > pos:
                self._style_gap(text, pos - line_start, s - line_start)
            if s < pos:
                length -= pos - s
                s = pos
            if length > 0:
                self.setStyling(length, style)
                pos = s + length
        if pos < end:
            self._style_gap(text, pos - line_start, end - line_start)
        # --- safety-net: ensure #include / #define lines keep MACRO style ---
        # Scintilla may call styleText() for a narrow range that doesn't
        # start at the beginning of a preprocessor line.  Although line_start
        # logic above usually covers this, a second pass guarantees it: scan
        # each line in the range, and if it starts with '#' outside a
        # string/comment, verify via SCI_GETSTYLEAT that the '#' is styled
        # as MACRO; if not, re-apply MACRO for the whole line.
        # Только для C/C++ — в Python '#' это комментарий, не директива.
        _restyle = []
        if self._language == "cpp":
            for li in range(start_line, end_line + 1):
                ln = editor.SendScintilla(SCI_GETLINEENDPOSITION, li)
                ls = editor.SendScintilla(SCI_POSITIONFROMLINE, li)
                if ln <= ls:
                    continue
                style_at_hash = editor.SendScintilla(SCI_GETSTYLEAT, ls)
                if style_at_hash == _STYLE_MACRO:
                    continue
                # read first non-whitespace byte of the line
                raw = editor.text(li)
                stripped = raw.lstrip()
                if not stripped or stripped[0] != "#":
                    continue
                if style_at_hash in (_STYLE_STRING, _STYLE_COMMENT):
                    continue
                _restyle.append(ls)
            if _restyle:
                _log.debug("safety-net: re-applying MACRO for %d line(s) in "
                           "styleText(%d, %d) #%d", len(_restyle), start, end,
                           _cid)
                for ls in _restyle:
                    line_bytes = editor.text(
                        editor.SendScintilla(SCI_LINEFROMPOSITION, ls)
                    ).encode("utf-8", "replace")
                    m = self._preproc_re.search(line_bytes)
                    if not m:
                        continue
                    self.startStyling(ls + m.start(), 0xff)
                    self.setStyling(m.end() - m.start(), _STYLE_MACRO)
        self.update_folding_levels(line_start, end)

        # --- post-styling diagnostics: check # line style survives ---
        if _log.isEnabledFor(logging.DEBUG):
            _hash_styles = []
            for li in range(start_line, min(end_line + 1, start_line + 5)):
                ls = editor.SendScintilla(SCI_POSITIONFROMLINE, li)
                ln = editor.SendScintilla(SCI_GETLINEENDPOSITION, li)
                if ln <= ls:
                    continue
                raw = editor.text(li)
                stripped = raw.lstrip()
                if stripped and stripped[0] == "#":
                    s_at = editor.SendScintilla(SCI_GETSTYLEAT, ls)
                    _hash_styles.append(
                        "line%d@%d:style=%d(%s)" % (
                            li, ls, s_at,
                            _SEM_STYLE_NAMES.get(s_at, "?")))
            if _hash_styles:
                _log.debug("#-lines after styleText+%d folding: %s",
                           _cid, ", ".join(_hash_styles))

    # --- фолдинг (как в LumenLangLexer — по фигурным скобкам) ---

    def _count_braces_safely(self, text: str):
        """{[( и )]} вне строк и комментариев -> (open_count, close_count)."""
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
        """Уровни фолдинга по фигурным скобкам (как в C++/Lumen)."""
        editor = self.editor()
        if not editor:
            return
        start_line = editor.SendScintilla(SCI_LINEFROMPOSITION, start)
        end_line = editor.SendScintilla(SCI_LINEFROMPOSITION, end)
        total_lines = editor.lines()
        current_level = _FOLDLEVELBASE
        if start_line > 0:
            prev_level = editor.SendScintilla(SCI_GETFOLDLEVEL, start_line - 1)
            current_level = prev_level & _FOLDLEVELNUMBERMASK
            prev_opens, prev_closes = self._count_braces_safely(
                editor.text(start_line - 1)
            )
            current_level += prev_opens - prev_closes
            if current_level < _FOLDLEVELBASE:
                current_level = _FOLDLEVELBASE
        for line in range(start_line, total_lines):
            text = editor.text(line)
            open_count, close_count = self._count_braces_safely(text)
            net_change = open_count - close_count
            is_header = _FOLDLEVELHEADERFLAG if net_change > 0 else 0
            new_fold_level = current_level | is_header
            if line > end_line and net_change == 0:
                old_fold_level = editor.SendScintilla(SCI_GETFOLDLEVEL, line)
                if ((old_fold_level & _FOLDLEVELNUMBERMASK)
                        == (new_fold_level & _FOLDLEVELNUMBERMASK)
                        and bool(old_fold_level & _FOLDLEVELHEADERFLAG)
                        == bool(new_fold_level & _FOLDLEVELHEADERFLAG)):
                    break
            editor.SendScintilla(SCI_SETFOLDLEVEL, line, new_fold_level)
            current_level += net_change
            if current_level < _FOLDLEVELBASE:
                current_level = _FOLDLEVELBASE

    def _collect(self, text: bytes, start: int, end: int,
                 span_offset: int = 0):
        """Все span'ы в [start, end): LSP + лексические, без пересечений.

        Строки собираются первыми, и комментарии внутри строк не маскируются
        ("http://" в C++-строке, "a # b" в Python-строке).
        span_offset — сдвиг абсолютных позиций LSP-токенов в координаты
        сегмента text (для styleText, стилизующего не весь документ).
        """
        seg = text[start:end]
        items = []  # (start, length, style, prio)
        string_spans = []
        comment_spans = []

        def add_string(m, style=_STYLE_STRING):
            s0 = start + m.start()
            items.append((s0, m.end() - m.start(), style, _PRIO_LEX))
            string_spans.append((s0, s0 + (m.end() - m.start())))

        for regex in self._string_res:
            for m in regex.finditer(seg):
                add_string(m)
        for m in self._char_re.finditer(seg):
            add_string(m)

        def in_string(s):
            for ss, ee in string_spans:
                if ss <= s < ee:
                    return True
            return False

        def in_comment(s):
            for cs, ce in comment_spans:
                if cs <= s < ce:
                    return True
            return False

        if self._block_comment_re is not None:
            for m in self._block_comment_re.finditer(seg):
                s0 = start + m.start()
                if not in_string(s0):
                    items.append((s0, m.end() - m.start(),
                                   _STYLE_COMMENT, _PRIO_LEX_OVERRIDE))
                    comment_spans.append((s0, s0 + (m.end() - m.start())))
        if self._line_comment_re is not None:
            pos = 0
            while True:
                m = self._line_comment_re.search(seg, pos)
                if m is None:
                    break
                s0 = start + m.start()
                if in_string(s0):
                    # маркер внутри строки — его матч «съедает» конец строки,
                    # продолжаем поиск с конца строки
                    pos = next((ee - start for ss, ee in string_spans
                                if ss <= s0 < ee), m.end())
                else:
                    # //! (указание) и //? (вопрос/warning) — отдельные стили
                    body = seg[m.start():m.end()]
                    lc = self._line_comment
                    if (lc and body.startswith(lc.encode() + b"!")
                            and self._language == "lumenlang"):
                        style = _STYLE_COMMENT_DOC
                    elif (lc and body.startswith(lc.encode() + b"?")
                            and self._language == "lumenlang"):
                        style = _STYLE_COMMENT_QUESTION
                    else:
                        style = _STYLE_COMMENT
                    items.append((s0, m.end() - m.start(), style,
                                   _PRIO_LEX_OVERRIDE))
                    comment_spans.append((s0, s0 + (m.end() - m.start())))
                    pos = m.end()
        # Препроцессорные директивы (#include/#define/…) — только для C/C++,
        # иначе в Python '# комментарий' тоже красится/подчёркивается как
        # директива, что некорректно.
        if self._language == "cpp":
            for m in self._preproc_re.finditer(seg):
                s0 = start + m.start()
                if not in_string(s0):
                    items.append((s0, m.end() - m.start(),
                                  _STYLE_MACRO, _PRIO_LEX_OVERRIDE))
        # #macro NAME: имя макрофункции — отдельный стиль (lumenlang)
        if self._language == "lumenlang":
            for m in self._macro_fn_re.finditer(seg):
                ns, ne = m.span(1)
                s0 = start + ns
                if not in_string(s0) and not in_comment(s0):
                    items.append((s0, ne - ns,
                                  _STYLE_MACRO_FUNCTION, _PRIO_LEX_OVERRIDE))
        for m in self._type_re.finditer(seg):
            items.append((start + m.start(), m.end() - m.start(),
                          _STYLE_TYPE, _PRIO_LEX))
        for m in self._kw_re.finditer(seg):
            items.append((start + m.start(), m.end() - m.start(),
                          _STYLE_KEYWORD, _PRIO_LEX))
        for m in self._number_re.finditer(seg):
            items.append((start + m.start(), m.end() - m.start(),
                          _STYLE_NUMBER, _PRIO_LEX))
        # Вхождения имён от LSP (documentSymbol): использование класса/функции
        # подсвечивается так же, как определение. Поиск подстроки без
        # регулярных выражений; строки/комментарии и границы слова фильтруются.
        if self._symbol_names:
            for name, token in sorted(self._symbol_names.items(),
                                      key=lambda kv: -len(kv[0])):
                mapping = _SEM_TOKEN_MAP.get(token)
                nbytes = name.encode("utf-8")
                if mapping is None or not nbytes:
                    continue
                style_id = mapping[0]
                if (style_id == _STYLE_FUNCTION
                        and self._language in _DUNDER_LANGUAGES
                        and _is_dunder(nbytes)):
                    style_id = _STYLE_DUNDER
                pos = 0
                while True:
                    i = seg.find(nbytes, pos)
                    if i < 0:
                        break
                    s0 = start + i
                    e0 = s0 + len(nbytes)
                    if ((s0 == 0 or not _is_ident(text[s0 - 1]))
                            and (e0 >= len(text) or not _is_ident(text[e0]))
                            and not in_string(s0) and not in_comment(s0)):
                        items.append((s0, len(nbytes), style_id,
                                      _PRIO_SEMANTIC))
                    pos = i + 1
        doc_len = len(text)
        # DEBUG: log raw LSP spans before stale-shift
        if _log.isEnabledFor(logging.DEBUG) and self._spans:
            _log.debug("_collect: start=%d end=%d stale=%s edits=%s "
                       "raw_spans(first10)=%s", start, end,
                       self._spans_stale, self._pending_edits[:3],
                       self._spans[:10])
        for s, length, style in self._spans:
            # Пока clangd анализирует — сдвигаем старые токены по правкам,
            # чтобы подсветка не «гасла» при вводе и не «плыла»
            if self._spans_stale:
                e = s + length
                for p, d in self._pending_edits:
                    if e <= p:
                        continue  # токен целиком до правки — не тронут
                    e += d
                    if s >= p:
                        s += d
                length = e - s
            if length <= 0:
                continue
            s -= span_offset  # абсолютные позиции LSP -> координаты сегмента
            e = s + length
            # Спан целиком вне текущего сегмента [start, end] — пропускаем.
            # Без этой проверки max(s, start) «втаскивает» чужой спан (напр.
            # MACRO из #define выше) в начало сегмента, и пробелы в строке
            # ввода кратковременно красятся/подчёркиваются серым.
            if e <= start or s >= end:
                continue
            if s < start:
                s = start
            length = e - s
            if s >= end or s >= doc_len:
                continue
            length = min(length, doc_len - s, end - s)
            if length > 0:
                items.append((s, length, style, _PRIO_SEMANTIC))
        # DEBUG: log final segment-relative spans
        if _log.isEnabledFor(logging.DEBUG) and items:
            _log.debug("_collect: final_items(first10)=%s",
                       [(s,l,sty) for s,l,sty,p in items[:10]])
        items.sort(key=lambda t: (t[0], -t[3]))
        spans = []
        cur_end = start
        for s, length, style, prio in items:
            if length <= 0:
                continue
            e = s + length
            if e <= cur_end:
                continue
            if s < cur_end:
                s = cur_end
                length = e - s
            spans.append((s, length, style))
            cur_end = e

        return spans

    def _style_gap(self, text: bytes, a: int, b: int):
        """Промежуток без span'ов: пунктуация -> operator, остальное -> default.

        Если промежуток содержит препроцессорную директиву (# в начале
        строки), только само имя директивы (#include / #define / ...) красится
        как MACRO — пробелы и имя файла идут как default/operator, без
        подчёркивания.  Так убирается «серый/подчёркнутый» зазор между
        директивой и строкой вроде `#include "x"`.
        """
        i = a
        while i < b:
            # Препроцессорная директива: # в начале строки -> только #\w+.
            # Только для C/C++ — в Python '#' это комментарий.
            if (self._language == "cpp" and text[i] == 0x23
                    and (i == 0 or text[i - 1] in (0x0A, 0x0D))):
                j = i + 1
                while j < b and (0x41 <= text[j] <= 0x5A
                                 or 0x61 <= text[j] <= 0x7A
                                 or 0x30 <= text[j] <= 0x39
                                 or text[j] == 0x5F):
                    j += 1
                self.setStyling(j - i, _STYLE_MACRO)
                i = j
                continue
            j = i
            while j < b and text[j] in _OP_CHARS:
                j += 1
            if j > i:
                self.setStyling(j - i, _STYLE_OPERATOR)
                i = j
                continue
            j = i
            while j < b and text[j] not in _OP_CHARS:
                j += 1
            self.setStyling(j - i, 0)
            i = j


# Обратная совместимость: старый класс C++-семантики — это SemanticLexer
SemanticCppLexer = SemanticLexer
