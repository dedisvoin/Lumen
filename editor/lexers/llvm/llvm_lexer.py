"""
LLVMLexer — кастомный QsciLexerCustom для LLVM IR (.ll).

Написан с нуля по образцу editor/lexers/asm/asm_lexer.py: собственный
токенизатор в styleText() (комментарии ;, строки, числа/hex/флоаты,
локальные %имя, глобальные @имя, метаданные !имя/!N, типы iN/float/...,
инструкции, директивы/ linkage/атрибуты, метки базовых блоков) и фолдинг
по меткам. Тема применяется через set_theme().

Не использует встроенные лексеры.
"""

from typing import List, Optional, Set, Tuple

from PyQt6.QtGui import QFont, QColor
from PyQt6.Qsci import QsciLexerCustom

from editor.scintilla_constants import (
    SCI_LINEFROMPOSITION,
    SCI_POSITIONFROMLINE,
    SCI_SETFOLDLEVEL,
)
from editor.constants import CODE_FONT_SIZE
from editor.font_settings import code_font_family


# Scintilla-константы для фолдинга
_FOLDLEVELBASE = 0x400
_FOLDLEVELHEADERFLAG = 0x2000


# ----------------------------------------------------------------------
# Наборы токенов LLVM IR
# ----------------------------------------------------------------------

# Именованные типы
_TYPES = {
    "void", "float", "double", "label", "ptr", "half", "bfloat", "x86_fp80",
    "fp128", "ppc_fp128", "x86_mmx", "ppc_vsx", "token", "metadata",
    "opaque", "array", "struct", "vector", "packed_struct",
}

# Инструкции и условные коды
_INSTRUCTIONS = {
    "ret", "br", "switch", "indirectbr", "invoke", "resume", "catchswitch",
    "catchret", "cleanupret", "unreachable", "call", "callbr", "tail",
    "musttail", "notail", "cleanup", "catch", "land", "phi", "select",
    "freeze", "fneg", "add", "fadd", "sub", "fsub", "mul", "fmul", "udiv",
    "sdiv", "fdiv", "urem", "srem", "frem", "shl", "lshr", "ashr", "and",
    "or", "xor", "extractelement", "insertelement", "shufflevector",
    "extractvalue", "insertvalue", "alloca", "load", "store", "fence",
    "cmpxchg", "atomicrmw", "getelementptr", "trunc", "zext", "sext",
    "fptrunc", "fpext", "uitofp", "sitofp", "fptoui", "fptosi", "inttoptr",
    "ptrtoint", "bitcast", "addrspacecast", "icmp", "fcmp", "va_arg",
    # условия icmp
    "eq", "ne", "slt", "sgt", "sle", "ult", "ugt", "ule", "oge", "olt",
    "ole", "one", "ord", "uno", "ueq", "une", "oeq", "ogt",
    # условия fcmp
    "olt", "oge", "ule", "une",
    # атомик-операции
    "xchg", "nand", "max", "min", "umax", "umin",
}

# Директивы верхнего уровня, linkage, атрибуты, квалификаторы
_DIRECTIVES = {
    "define", "declare", "target", "triple", "datalayout", "module",
    "source_filename", "attributes", "globals", "metadata", "comdat",
    "section", "align", "unnamed_addr", "local_unnamed_addr", "dso_local",
    "dso_preemptable", "thread_local", "global", "constant", "external",
    "internal", "private", "weak", "common", "linkonce",
    "available_externally", "appending", "dllimport", "dllexport",
    "linkonce_odr", "weak_odr", "external_odr", "hidden", "protected",
    "default", "exact", "inbounds", "nsw", "nuw", "volatile", "atomic",
    "unordered", "monotonic", "acquire", "release", "acq_rel", "seq_cst",
    "syncscope", "singlethread", "to", "addrspace", "personality",
    "prefix", "prologue", "any", "exactmatch", "largest", "noduplicates",
    "samesize", "ccc", "fastcc", "coldcc", "cc", "cold", "noreturn",
    "nounwind", "readonly", "readnone", "optsize", "alwaysinline",
    "inlinehint", "noinline", "optnone", "ssp", "sspreq", "sspstrong",
    "naked", "nobuiltin", "noimplicitfloat", "noduplicate", "nonlazybind",
    "builtin", "convergent", "speculatable", "willreturn", "gc",
    "returns_twice", "writeonly", "argmemonly", "uwtable", "safe_stack",
    "shadowcallstack", "vscale_range", "alignstack", "allocsize",
    "swifterror", "swiftself", "byval", "byref", "inalloca", "preallocated",
    "sret", "nest", "noalias", "nocapture", "nonnull", "dereferenceable",
    "dereferenceable_or_null", "elementtype", "distinct", "opaque", "block",
    "inteldialect", "poison", "noundef", "deplibs", "except", "filter",
}

# Константы-литералы
_LITERALS = {
    "true", "false", "null", "undef", "zeroinitializer", "none", "c",
}


def _is_type(word: str) -> bool:
    """iN (целочисленный тип) или именованный тип."""
    if word in _TYPES:
        return True
    if len(word) >= 2 and word[0] == "i" and word[1:].isdigit():
        return True
    if len(word) >= 3 and word.startswith("vscale") and word[6:].isdigit():
        return True
    return False


class LLVMLexer(QsciLexerCustom):
    """Custom lexer for LLVM IR (.ll)."""

    STYLE_DEFAULT = 0
    STYLE_COMMENT = 1
    STYLE_NUMBER = 2
    STYLE_STRING = 3
    STYLE_OPERATOR = 4
    STYLE_LOCAL = 5       # %имя (локальные значения)
    STYLE_GLOBAL = 6      # @имя (глобальные)
    STYLE_METADATA = 7    # !имя / !N / !{ }
    STYLE_TYPE = 8
    STYLE_INSTRUCTION = 9
    STYLE_DIRECTIVE = 10
    STYLE_LABEL = 11
    STYLE_LITERAL = 12

    _STYLE_THEME_KEYS = {
        STYLE_DEFAULT: ("default", None),
        STYLE_COMMENT: ("comment", QColor(108, 112, 134)),
        STYLE_NUMBER: ("number", QColor(250, 179, 135)),
        STYLE_STRING: ("string", QColor(166, 227, 161)),
        STYLE_OPERATOR: ("operator", QColor(137, 180, 250)),
        STYLE_LOCAL: ("variable", QColor(148, 226, 213)),
        STYLE_GLOBAL: ("namespace_id", QColor(137, 220, 235)),
        STYLE_METADATA: ("special", QColor(249, 226, 175)),
        STYLE_TYPE: ("type", QColor(137, 220, 235)),
        STYLE_INSTRUCTION: ("keyword", QColor(203, 166, 247)),
        STYLE_DIRECTIVE: ("directive", QColor(243, 139, 168)),
        STYLE_LABEL: ("function_name", QColor(249, 226, 175)),
        STYLE_LITERAL: ("literal", QColor(148, 226, 213)),
    }

    LINE_COMMENT = ";"

    def __init__(self, parent=None, theme_data: Optional[dict] = None):
        super().__init__(parent)
        self._theme_data = theme_data or {}
        self._text: str = ""
        self._text_bytes: bytes = b""
        self._lines: List[str] = []
        self._dirty: bool = True
        self._hooked_editor = None
        # Слова для автодополнения («как в Lumen»): атрибуты читает
        # CompletionEngine._collect_static_words по _COMPLETION_GROUPS
        self.keywords = _INSTRUCTIONS
        self.types = _TYPES
        self.directives = _DIRECTIVES
        self.literals = _LITERALS
        self.special_keywords = set()
        self.modifiers = set()
        self.setup_styles()

    def language(self) -> str:
        return "llvm"

    def wordCharacters(self) -> str:
        return "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.%@!#$"

    def description(self, style: int) -> str:
        descriptions = {
            self.STYLE_DEFAULT: "Default",
            self.STYLE_COMMENT: "Comment",
            self.STYLE_NUMBER: "Number",
            self.STYLE_STRING: "String",
            self.STYLE_OPERATOR: "Operator",
            self.STYLE_LOCAL: "Local Value",
            self.STYLE_GLOBAL: "Global Value",
            self.STYLE_METADATA: "Metadata",
            self.STYLE_TYPE: "Type",
            self.STYLE_INSTRUCTION: "Instruction",
            self.STYLE_DIRECTIVE: "Directive",
            self.STYLE_LABEL: "Label",
            self.STYLE_LITERAL: "Literal",
        }
        return descriptions.get(style, "")

    def set_theme(self, theme_data: dict):
        self._theme_data = theme_data
        self.setup_styles()

    def setup_styles(self):
        theme = self._theme_data
        colors = theme.get("colors", {})
        styles = theme.get("styles", {})

        bg = QColor(colors.get("background", "#1e1e2e"))
        fg = QColor(colors.get("foreground", "#dcdcdc"))

        self.setDefaultPaper(bg)
        self.setDefaultColor(fg)

        ed = self.editor()
        size = (getattr(ed, "_font_size", None) if ed is not None else None)
        size = size or styles.get("default", {}).get("size") or CODE_FONT_SIZE
        base_font = QFont(code_font_family(), size)
        base_font.setFixedPitch(True)

        for style_id in range(len(self._STYLE_THEME_KEYS)):
            self.setFont(base_font, style_id)
            self.setPaper(bg, style_id)

        for style_id, (key, fallback) in self._STYLE_THEME_KEYS.items():
            color = fg if fallback is None else fallback
            self.setColor(self._style_color(styles, key, color), style_id)

        italic_font = QFont(base_font)
        italic_font.setItalic(True)
        self.setFont(italic_font, self.STYLE_COMMENT)

        bold_font = QFont(base_font)
        bold_font.setBold(True)
        self.setFont(bold_font, self.STYLE_INSTRUCTION)
        self.setFont(bold_font, self.STYLE_DIRECTIVE)

    @staticmethod
    def _style_color(styles: dict, key: str, fallback: QColor) -> QColor:
        data = styles.get(key)
        if data and "color" in data:
            return QColor(data["color"])
        return fallback

    @staticmethod
    def _get_char_at(text_bytes: bytes, pos: int, total: int) -> Tuple[Optional[str], int]:
        if pos >= total:
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
        if pos + length > total:
            length = 1
        try:
            ch = text_bytes[pos:pos + length].decode("utf-8")
        except UnicodeDecodeError:
            ch = "�"
        return (ch, length)

    def styleText(self, start: int, end: int):
        editor = self.editor()
        if not editor:
            return
        self._ensure_hooked(editor)
        if self._dirty:
            self._refresh_cache(editor)

        text_bytes = self._text_bytes
        total = len(text_bytes)
        if total == 0:
            return

        start = max(0, start)
        end = min(total, end)
        if start >= end:
            return

        self.startStyling(start)
        pos = start

        while pos < end:
            ch, ch_len = self._get_char_at(text_bytes, pos, total)
            if ch_len == 0:
                break

            # --- Комментарий ; (до конца строки) ---
            if ch == ";":
                j = pos
                while j < end:
                    c, l = self._get_char_at(text_bytes, j, total)
                    if c == "\n":
                        break
                    j += l
                self.setStyling(j - pos, self.STYLE_COMMENT)
                pos = j
                continue

            # --- Строки ---
            if ch in ('"', "'"):
                quote = ch
                j = pos + ch_len
                self.setStyling(ch_len, self.STYLE_STRING)
                while j < end:
                    c, l = self._get_char_at(text_bytes, j, total)
                    if c is None:
                        break
                    if c == "\\":
                        self.setStyling(l, self.STYLE_STRING)
                        j += l
                        if j < end:
                            _, nl = self._get_char_at(text_bytes, j, total)
                            self.setStyling(nl, self.STYLE_STRING)
                            j += nl
                        continue
                    if c == quote:
                        self.setStyling(l, self.STYLE_STRING)
                        j += l
                        break
                    self.setStyling(l, self.STYLE_STRING)
                    j += l
                pos = j
                continue

            # --- Префиксные значения: %локаль, @глобаль, !метаданные, #атрибуты ---
            if ch in ("%", "@", "!", "#"):
                prefix = ch
                j = pos + ch_len
                # !{ ... } — блок метаданных
                c0, _ = self._get_char_at(text_bytes, j, total)
                if prefix == "!" and c0 == "{":
                    depth = 1
                    j += 1
                    while j < end:
                        c, l = self._get_char_at(text_bytes, j, total)
                        if c == "{":
                            depth += 1
                        elif c == "}":
                            depth -= 1
                            j += l
                            if depth == 0:
                                break
                        else:
                            j += l
                    self.setStyling(j - pos, self.STYLE_METADATA)
                    pos = j
                    continue
                allowed = "_." if prefix in ("%", "@") else "_."
                while j < end:
                    c, l = self._get_char_at(text_bytes, j, total)
                    if c is None:
                        break
                    if c.isalnum() or c in allowed:
                        j += l
                    else:
                        break
                style = {
                    "%": self.STYLE_LOCAL,
                    "@": self.STYLE_GLOBAL,
                    "!": self.STYLE_METADATA,
                    "#": self.STYLE_METADATA,
                }[prefix]
                self.setStyling(j - pos, style)
                pos = j
                continue

            # --- Числа ---
            num = self._try_number(text_bytes, pos, end, total)
            if num is not None:
                self.setStyling(num - pos, self.STYLE_NUMBER)
                pos = num
                continue

            # --- Идентификаторы / метки / ключевые слова ---
            if ch.isalpha() or ch == "_":
                j = pos
                while j < end:
                    c, l = self._get_char_at(text_bytes, j, total)
                    if c is None:
                        break
                    if c.isalnum() or c in "_.@#":
                        j += l
                    else:
                        break
                word = text_bytes[pos:j].decode("utf-8", "ignore")

                # Метка базового блока: имя + ':'
                k = j
                while k < end:
                    c, l = self._get_char_at(text_bytes, k, total)
                    if c in (" ", "\t"):
                        k += l
                    else:
                        break
                if k < end and text_bytes[k:k + 1] == b":":
                    j = k + 1
                    self.setStyling(j - pos, self.STYLE_LABEL)
                    pos = j
                    continue

                if _is_type(word):
                    style = self.STYLE_TYPE
                elif word in _INSTRUCTIONS:
                    style = self.STYLE_INSTRUCTION
                elif word in _DIRECTIVES:
                    style = self.STYLE_DIRECTIVE
                elif word in _LITERALS:
                    style = self.STYLE_LITERAL
                else:
                    style = self.STYLE_DEFAULT
                self.setStyling(j - pos, style)
                pos = j
                continue

            # --- Операторы / пунктуация ---
            if ch in "=<>[](){},*:+-":
                j = pos + ch_len
                if j < end:
                    next_ch, next_len = self._get_char_at(
                        text_bytes, j, total)
                    multi = (
                        (ch == "<" and next_ch == "<") or
                        (ch == ">" and next_ch == ">") or
                        (ch == "=" and next_ch == "=") or
                        (ch == "<" and next_ch == "=") or
                        (ch == ">" and next_ch == "=")
                    )
                    if multi:
                        j += next_len
                self.setStyling(j - pos, self.STYLE_OPERATOR)
                pos = j
                continue

            # --- Всё остальное ---
            self.setStyling(ch_len, self.STYLE_DEFAULT)
            pos += ch_len

        self.update_folding_levels(start, end)

    @staticmethod
    def _try_number(text_bytes: bytes, pos: int, end: int, total: int):
        """Возвращает позицию конца числа или None."""
        ch, _ = LLVMLexer._get_char_at(text_bytes, pos, total)
        if ch is None:
            return None

        # 0x / 0b (hex-флоаты: 0x...pN)
        if ch == "0" and pos + 1 < end:
            nxt, nl = LLVMLexer._get_char_at(text_bytes, pos + 1, total)
            if nxt in ("x", "X"):
                j = pos + 1 + nl
                while j < end:
                    c, l = LLVMLexer._get_char_at(text_bytes, j, total)
                    if c is not None and (c.isdigit() or c in "abcdefABCDEF"):
                        j += l
                    elif c in ("p", "P"):
                        j += l
                        if j < end:
                            s, sl = LLVMLexer._get_char_at(text_bytes, j, total)
                            if s in "+-":
                                j += sl
                        while j < end:
                            c2, l2 = LLVMLexer._get_char_at(text_bytes, j, total)
                            if c2 is not None and c2.isdigit():
                                j += l2
                            else:
                                break
                    else:
                        break
                return j
            if nxt in ("b", "B"):
                j = pos + 1 + nl
                while j < end:
                    c, l = LLVMLexer._get_char_at(text_bytes, j, total)
                    if c is not None and c in "01":
                        j += l
                    else:
                        break
                return j

        # Десятичное / флоат с экспонентой
        if ch.isdigit() or (ch == "." and pos + 1 < end and
                            LLVMLexer._get_char_at(text_bytes, pos + 1, total)[0].isdigit()):
            j = pos
            seen_dot = False
            seen_exp = False
            while j < end:
                c, l = LLVMLexer._get_char_at(text_bytes, j, total)
                if c is None:
                    break
                if c.isdigit():
                    j += l
                elif c == "." and not seen_dot:
                    seen_dot = True
                    j += l
                elif c in "eE" and not seen_exp:
                    seen_exp = True
                    j += l
                    if j < end:
                        s, sl = LLVMLexer._get_char_at(text_bytes, j, total)
                        if s in "+-":
                            j += sl
                else:
                    break
            return j

        return None

    # ------------------------------------------------------------------
    # Инкрементальный кэш (SCN_MODIFIED) + фолдинг по меткам блоков
    # ------------------------------------------------------------------

    def _ensure_hooked(self, editor):
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

    def _on_modified(self, *args):
        self._dirty = True

    def _refresh_cache(self, editor):
        text = editor.text()
        self._text = text
        self._text_bytes = text.encode("utf-8")
        self._lines = text.split("\n")
        self._dirty = False

    def _count_braces(self, line: str):
        """Считает { ( [ и ) } ] в строке, игнорируя строки и ;-комментарии."""
        open_c = close_c = 0
        i = 0
        n = len(line)
        in_str = None
        while i < n:
            ch = line[i]
            if in_str:
                if ch == "\\":
                    i += 2
                    continue
                if ch == in_str:
                    in_str = None
                i += 1
                continue
            if ch in ('"', "'"):
                in_str = ch
                i += 1
                continue
            if ch == ";":
                break
            if ch in "{(":
                open_c += 1
            elif ch in "})":
                close_c += 1
            elif ch == "[":
                open_c += 1
            elif ch == "]":
                close_c += 1
            i += 1
        return open_c, close_c

    def update_folding_levels(self, start: int, end: int):
        """Фолдинг только по скобкам { ( [ и ] ) }.

        Заголовком становится строка, открывающая больше скобок, чем закрывает.
        Пересчёт идёт только при изменении текста (_dirty).
        """
        if not self._dirty:
            return
        editor = self.editor()
        if editor is None:
            return
        level = _FOLDLEVELBASE
        for line in range(len(self._lines)):
            text = self._lines[line]
            open_c, close_c = self._count_braces(text)
            net = open_c - close_c
            is_header = _FOLDLEVELHEADERFLAG if net > 0 else 0
            editor.SendScintilla(SCI_SETFOLDLEVEL, line, level | is_header)
            level += net
            if level < _FOLDLEVELBASE:
                level = _FOLDLEVELBASE
