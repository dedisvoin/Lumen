"""
AsmLexer — кастомный QsciLexerCustom для ассемблерных файлов (.asm/.s/...).

Написан с нуля по образцу editor/lexers/lumen/lumen_lang.py: собственный
токенизатор в styleText() (комментарии ; // /* */ и # в начале строки,
строки, числа в разных системах счисления, регистры, инструкции, директивы,
метки, операторы) и фолдинг по меткам. Тема применяется через set_theme().

Не использует встроенный QsciLexerAsm.
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


# Scintilla-константы для фолдинга (числовые — надёжнее атрибутов класса)
_FOLDLEVELBASE = 0x400        # SC_FOLDLEVELBASE
_FOLDLEVELHEADERFLAG = 0x2000 # SC_FOLDLEVELHEADERFLAG


# ----------------------------------------------------------------------
# Наборы токенов
# ----------------------------------------------------------------------

def _gen_registers() -> Set[str]:
    regs = set()
    for base in ("a", "b", "c", "d"):
        regs.update((f"r{base}x", f"e{base}x", f"{base}x", f"{base}h", f"{base}l"))
    for i in range(8, 16):
        regs.update((f"r{i}", f"r{i}d", f"r{i}w", f"r{i}b"))
    regs.update(("rsi", "esi", "si", "sil", "rdi", "edi", "di", "dil",
                 "rbp", "ebp", "bp", "bpl", "rsp", "esp", "sp", "spl",
                 "rip", "eip", "ip"))
    regs.update(("cs", "ds", "ss", "es", "fs", "gs"))
    regs.update(("rflags", "eflags", "flags"))
    regs.update(f"st{i}" for i in range(8))
    regs.update(f"mm{i}" for i in range(8))
    for kind in ("xmm", "ymm", "zmm"):
        for i in range(16):
            regs.add(f"{kind}{i}")
    regs.update(f"cr{i}" for i in range(16))
    regs.update(f"dr{i}" for i in range(16))
    regs.update(("tr0", "tr1", "tr2", "tr3", "tr4", "tr5", "tr6", "tr7",
                 "gdtr", "idtr", "ldtr", "mxcsr", "xcr0", "msr", "tsc"))
    return regs


_REGISTERS = _gen_registers()

_INSTRUCTIONS = {
    "mov", "movsx", "movsxd", "movzx", "movs", "movsb", "movsw", "movsd",
    "movsq", "movss", "movups", "movupd", "movaps", "movapd", "movd", "movq",
    "movdqu", "movdqa", "movbe", "xchg", "lea", "xadd", "cmpxchg",
    "cmpxchg8b", "cmpxchg16b", "bswap", "xlat", "xlatb", "cld", "std",
    "lahf", "sahf",
    "add", "sub", "mul", "imul", "div", "idiv", "inc", "dec", "neg", "adc",
    "sbb", "sal", "sar", "shl", "shr", "rol", "ror", "rcl", "rcr", "shld",
    "shrd", "cbw", "cwd", "cwde", "cdqe", "cqo", "bsf", "bsr", "bt", "bts",
    "btr", "btc", "cmp", "test", "comisd", "comiss", "ucomisd", "ucomiss",
    "fist", "fistp",
    "and", "or", "xor", "not",
    "lock", "rep", "repe", "repz", "repne", "repnz", "rex", "rexb", "rexw",
    "rexwx", "rexwb",
    "call", "ret", "retn", "retf", "enter", "leave", "hlt", "nop", "pause",
    "syscall", "sysenter", "sysexit", "sysret", "ud2", "xbegin", "xend",
    "xabort", "bound", "int", "into", "int3",
    "jmp", "push", "pop", "pushad", "popad", "pushf", "popf", "pushfd",
    "popfd", "pushdq", "popcnt",
    "clc", "stc", "cmc", "cli", "sti",
    "cmps", "cmpsb", "cmpsw", "cmpsd", "cmpsq", "scas", "scasb", "scasw",
    "scasd", "lods", "lodsb", "lodsw", "lodsd", "stos", "stosb", "stosw",
    "stosd", "ins", "outs",
    "addps", "subps", "mulps", "divps", "sqrtps", "maxps", "minps",
    "addpd", "subpd", "mulpd", "divpd", "sqrtpd", "addss", "subss", "mulss",
    "divss", "sqrtss", "cvtss2sd", "cvtsd2ss", "cvtsi2ss", "cvtsi2sd",
    "cvtss2si", "cvtsd2si", "unpcklps", "unpckhps", "shufps", "andps",
    "orps", "xorps", "andpd", "orpd", "xorpd", "pshufd", "pshufb",
    "packssdw", "paddb", "paddw", "paddd", "psubb", "psubw", "psubd",
    "pmullw", "pmaddwd", "pand", "por", "pxor", "pcmpgtb", "pcmpgtw",
    "pcmpgtd",
    "cpuid", "rdtsc", "rdtscp", "rdmsr", "wrmsr", "invlpg", "wbinvd",
    "sfence", "lfence", "mfence", "prefetch", "prefetchnta", "prefetcht0",
    "prefetcht1", "prefetcht2",
}

_COND = ("o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np",
         "l", "ge", "le", "g", "c", "nc", "z", "nz", "pe", "po", "nge",
         "ng", "nle", "na", "nb", "cxz", "ecxz", "rcxz")
for _c in _COND:
    _INSTRUCTIONS.add("j" + _c)
    _INSTRUCTIONS.add("cmov" + _c)
    _INSTRUCTIONS.add("set" + _c)
for _c in ("", "e", "ne", "z", "nz"):
    _INSTRUCTIONS.add("loop" + _c)

_SPECIAL = {
    "fadd", "fmul", "fsub", "fsubr", "fdiv", "fdivr", "fld", "fst", "fstp",
    "fcomp", "fcom", "fcompp", "fcomi", "fcomip", "fucom", "fucomp",
    "fucompp", "fucomi", "fucomip", "fxch", "fchs", "fabs", "fsqrt", "fsin",
    "fcos", "ftan", "fld1", "fldz", "fldpi", "fild", "fistp", "ffree",
    "finit", "fninit", "fwait", "fnop", "fprem", "fprem1", "frndint",
    "fscale", "f2xm1", "fyl2x", "fyl2xp1", "fxtract", "fdecstp", "fincstp",
    "fxam", "fpatan", "fnstsw", "fstsw", "fnstcw", "fstcw", "fldcw",
    "fstenv", "fldenv", "fsave", "frstor", "fbld", "fbstp", "fclex",
}

_DIRECTIVES = {
    ".text", ".data", ".bss", ".rodata", ".section", "section", ".code",
    ".const", ".stack", ".rdata",
    ".globl", ".global", ".extern", ".public", ".private", "global",
    "extern", "public",
    ".macro", ".endmacro", ".macros", "macro", "endmacro", "endm",
    ".struct", ".ends", "struct", "ends", "struc", "endstruc", "istruc",
    ".proc", ".endproc", "proc", "endp", ".typedef", ".enum", ".endenum",
    ".if", ".else", ".elseif", ".endif", ".ifdef", ".ifndef", ".ifnotdef",
    ".ifb", ".ifnb", ".ifidn", ".ifidni", ".ifstr", ".repeat", ".until",
    ".while", ".endw", ".for", ".exitm", "if", "else", "endif", "ifdef",
    "ifndef", "repeat", "until", "while", "endw",
    ".byte", ".word", ".dword", ".qword", ".tbyte", ".oword", ".yword",
    ".ascii", ".asciz", ".string", "db", "dw", "dd", "dq", "dt", "df",
    ".equ", ".equiv", "equ", ".set", "set",
    ".align", ".balign", ".p2align", ".org", "org", "align", ".fill",
    ".space", ".skip", ".zero", ".incbin", "incbin",
    "bits", "use16", "use32", "use64", ".bits", ".code16", ".code32",
    ".code64", ".model", ".assume", ".list", ".nolist", ".radix", "radix",
    ".local", "local", ".comm", "comm", ".common", ".file", ".line",
    ".type", ".size", ".ident", ".cfi_startproc", ".cfi_endproc",
    ".cfi_def_cfa", ".cfi_offset", ".cfi_register", ".cfi_remember_state",
    ".cfi_restore_state",     ".cfi_adjust_cfa_offset", ".attribute",
}


class AsmLexer(QsciLexerCustom):
    """Custom lexer for assembly (.asm/.s/...)."""

    STYLE_DEFAULT = 0
    STYLE_COMMENT = 1
    STYLE_COMMENTBLOCK = 2
    STYLE_NUMBER = 3
    STYLE_STRING = 4
    STYLE_OPERATOR = 5
    STYLE_REGISTER = 6
    STYLE_INSTRUCTION = 7
    STYLE_DIRECTIVE = 8
    STYLE_LABEL = 9
    STYLE_SPECIAL = 10

    _STYLE_THEME_KEYS = {
        STYLE_DEFAULT: ("default", None),
        STYLE_COMMENT: ("comment", QColor(108, 112, 134)),
        STYLE_COMMENTBLOCK: ("comment", QColor(108, 112, 134)),
        STYLE_NUMBER: ("number", QColor(250, 179, 135)),
        STYLE_STRING: ("string", QColor(166, 227, 161)),
        STYLE_OPERATOR: ("operator", QColor(137, 180, 250)),
        STYLE_REGISTER: ("type", QColor(137, 220, 235)),
        STYLE_INSTRUCTION: ("keyword", QColor(203, 166, 247)),
        STYLE_DIRECTIVE: ("directive", QColor(243, 139, 168)),
        STYLE_LABEL: ("function_name", QColor(249, 226, 175)),
        STYLE_SPECIAL: ("special", QColor(249, 226, 175)),
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
        self.special_keywords = _SPECIAL | _REGISTERS
        self.directives = _DIRECTIVES
        self.types = _REGISTERS
        self.modifiers = set()
        self.literals = _REGISTERS
        self.setup_styles()

    def language(self) -> str:
        return "asm"

    def wordCharacters(self) -> str:
        return "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.@$?#"

    def description(self, style: int) -> str:
        descriptions = {
            self.STYLE_DEFAULT: "Default",
            self.STYLE_COMMENT: "Comment",
            self.STYLE_COMMENTBLOCK: "Block Comment",
            self.STYLE_NUMBER: "Number",
            self.STYLE_STRING: "String",
            self.STYLE_OPERATOR: "Operator",
            self.STYLE_REGISTER: "Register",
            self.STYLE_INSTRUCTION: "Instruction",
            self.STYLE_DIRECTIVE: "Directive",
            self.STYLE_LABEL: "Label",
            self.STYLE_SPECIAL: "FPU Instruction",
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
        self.setFont(italic_font, self.STYLE_COMMENTBLOCK)

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

            # --- Комментарии // и /* */ ---
            if ch == "/" and pos + ch_len < end:
                next_ch, next_len = self._get_char_at(
                    text_bytes, pos + ch_len, total)
                if next_ch == "/":
                    j = pos
                    while j < end:
                        c, l = self._get_char_at(text_bytes, j, total)
                        if c == "\n":
                            break
                        j += l
                    self.setStyling(j - pos, self.STYLE_COMMENT)
                    pos = j
                    continue
                if next_ch == "*":
                    close = text_bytes.find(b"*/", pos + ch_len + next_len)
                    if close == -1:
                        self.setStyling(end - pos, self.STYLE_COMMENTBLOCK)
                        pos = end
                        continue
                    self.setStyling(close + 2 - pos, self.STYLE_COMMENTBLOCK)
                    pos = close + 2
                    continue

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

            # --- Комментарий # только в начале строки (GAS) ---
            if ch == "#":
                line = editor.SendScintilla(SCI_LINEFROMPOSITION, pos)
                line_start = editor.SendScintilla(SCI_POSITIONFROMLINE, line)
                if text_bytes[line_start:pos].strip() == b"":
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

            # --- Числа ---
            num = self._try_number(text_bytes, pos, end, total)
            if num is not None:
                self.setStyling(num - pos, self.STYLE_NUMBER)
                pos = num
                continue

            # --- Идентификаторы / метки / ключевые слова ---
            if ch.isalpha() or ch == "_" or ch == ".":
                j = pos
                while j < end:
                    c, l = self._get_char_at(text_bytes, j, total)
                    if c is None:
                        break
                    if c.isalnum() or c in "_.@?#$":
                        j += l
                    else:
                        break
                word = text_bytes[pos:j].decode("utf-8", "ignore")

                # Метка: идентификатор + ':' (возможно с пробелами)
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

                if word.startswith(".") or word in _DIRECTIVES:
                    style = self.STYLE_DIRECTIVE
                elif word in _REGISTERS:
                    style = self.STYLE_REGISTER
                elif word in _INSTRUCTIONS:
                    style = self.STYLE_INSTRUCTION
                elif word in _SPECIAL:
                    style = self.STYLE_SPECIAL
                else:
                    style = self.STYLE_DEFAULT
                self.setStyling(j - pos, style)
                pos = j
                continue

            # --- Операторы / пунктуация ---
            if ch in "+-*/%=&|^!<>~,?.:(){}[]#$@":
                j = pos + ch_len
                if j < end:
                    next_ch, next_len = self._get_char_at(
                        text_bytes, j, total)
                    multi = (
                        (ch == ":" and next_ch == ":") or
                        (ch == "-" and next_ch == ">") or
                        (ch == "<" and next_ch == "=") or
                        (ch == ">" and next_ch == "=") or
                        (ch == "=" and next_ch == "=") or
                        (ch == "!" and next_ch == "=") or
                        (ch == "<" and next_ch == "<") or
                        (ch == ">" and next_ch == ">") or
                        (ch == "+" and next_ch == "+") or
                        (ch == "-" and next_ch == "-")
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
        """Возвращает позицию конца числа или None, если токен не число."""
        ch, _ = AsmLexer._get_char_at(text_bytes, pos, total)
        if ch is None:
            return None

        # $hex (MASM/TASM)
        if ch == "$":
            j = pos + 1
            c, l = AsmLexer._get_char_at(text_bytes, j, total)
            if c is not None and c in "0123456789abcdefABCDEF":
                while j < end:
                    c2, l2 = AsmLexer._get_char_at(text_bytes, j, total)
                    if c2 is not None and c2 in "0123456789abcdefABCDEF":
                        j += l2
                    else:
                        break
                return j
            return None

        # 0x / 0b / 0o / 0q
        if ch == "0" and pos + 1 < end:
            nxt, nl = AsmLexer._get_char_at(text_bytes, pos + 1, total)
            if nxt in ("x", "X"):
                j = pos + 1 + nl
                while j < end:
                    c, l = AsmLexer._get_char_at(text_bytes, j, total)
                    if c is not None and (c.isdigit() or c in "abcdefABCDEF"):
                        j += l
                    else:
                        break
                return j
            if nxt in ("b", "B"):
                j = pos + 1 + nl
                while j < end:
                    c, l = AsmLexer._get_char_at(text_bytes, j, total)
                    if c is not None and c in "01":
                        j += l
                    else:
                        break
                return j
            if nxt in ("o", "O", "q", "Q"):
                j = pos + 1 + nl
                while j < end:
                    c, l = AsmLexer._get_char_at(text_bytes, j, total)
                    if c is not None and c in "01234567":
                        j += l
                    else:
                        break
                return j

        # Десятичное / шестнадцатеричное с буквами / суффикс h/b/d/o/q
        if ch.isdigit() or (ch == "." and pos + 1 < end and
                            AsmLexer._get_char_at(text_bytes, pos + 1, total)[0].isdigit()):
            j = pos
            seen_dot = False
            while j < end:
                c, l = AsmLexer._get_char_at(text_bytes, j, total)
                if c is None:
                    break
                if c.isdigit() or c in "abcdefABCDEF":
                    j += l
                elif c == "." and not seen_dot:
                    seen_dot = True
                    j += l
                else:
                    break
            if j < end:
                c, l = AsmLexer._get_char_at(text_bytes, j, total)
                if c in "hHbBdDoOqQ":
                    j += l
            return j

        return None

    # ------------------------------------------------------------------
    # Инкрементальный кэш (SCN_MODIFIED) + фолдинг по меткам
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
        """Считает { ( [ и ) } ] в строке, игнорируя строки и комментарии."""
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
            if ch == "/" and i + 1 < n and line[i + 1] == "/":
                break
            if ch == "/" and i + 1 < n and line[i + 1] == "*":
                end = line.find("*/", i + 2)
                i = n if end == -1 else end + 2
                continue
            if ch == ";" or (ch == "#" and line[:i].strip() == ""):
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

