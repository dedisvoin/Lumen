"""
SignaturePopup — всплывающая подсказка сигнатуры при наборе '('.

Показывает label активной сигнатуры (активный параметр подсвечен),
документацию под ней и счётчик перегрузок. Позиционируется у каретки,
форматируется под тему редактора.
"""

import html
import re
from typing import List, Optional

from PyQt6.QtCore import QPoint, QRectF, Qt
from PyQt6.QtGui import (
    QColor, QFont, QFontMetrics, QPainter, QPainterPath, QPen,
)
from PyQt6.QtWidgets import (
    QApplication, QFrame, QLabel, QScrollArea, QVBoxLayout, QWidget,
)
from PyQt6.Qsci import (
    QsciLexerBash, QsciLexerCPP, QsciLexerJava, QsciLexerJavaScript,
    QsciLexerLua, QsciLexerPython, QsciLexerRuby, QsciLexerSQL,
    QsciLexerXML, QsciScintilla,
)

try:
    from PyQt6 import sip
except Exception:
    sip = None

_MD_CODE_RE = re.compile(r"`([^`\n]+)`")
_MD_BOLD_RE = re.compile(r"\*\*([^*]+)\*\*")
_MD_ITALIC_RE = re.compile(r"(?<!\*)\*([^*\n]+)\*(?!\*)")
_MD_LINK_RE = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")
_MD_HEADING_RE = re.compile(r"^(#{1,6})\s+(.*)$")
_MD_RULE_RE = re.compile(r"^[-*_]{3,}\s*$")
_MD_BULLET_RE = re.compile(r"^(\s*)[-*+]\s+(.*)$")
_MD_NUMBER_RE = re.compile(r"^(\s*)\d{1,3}[.)]\s+(.*)$")

# --- Python lexer for doc code blocks ---
try:
    from Solar.lexers.python.python_def import _PY_KEYWORDS, _PY_TYPES
except Exception:
    _PY_KEYWORDS = ("False", "None", "True", "and", "as", "assert", "async",
                    "await", "break", "class", "continue", "def", "del", "elif",
                    "else", "except", "finally", "for", "from", "global", "if",
                    "import", "in", "is", "lambda", "nonlocal", "not", "or",
                    "pass", "raise", "return", "try", "while", "with", "yield",
                    "match", "case")
    _PY_TYPES = ("bool", "bytearray", "bytes", "complex", "dict", "float",
                 "frozenset", "int", "list", "memoryview", "object", "range",
                 "set", "slice", "str", "tuple", "type", "NoneType", "Any",
                 "Optional", "Union", "Callable", "Iterable", "Iterator",
                 "Sequence", "Mapping", "List", "Dict", "Set", "Tuple",
                 "Self", "Literal", "TypeVar", "Protocol")


class _PythonDocLexer(QsciLexerPython):
    """QsciLexerPython с явными keywords/types для блоков кода в доках."""

    def keywords(self, set_):
        if set_ == 1:
            return " ".join(_PY_KEYWORDS)
        if set_ == 2:
            # QsciLexerPython uses set 2 for highlighted identifiers (builtins/types).
            # Include True/False/None here too for robust highlighting across QScintilla builds.
            extras = ("True", "False", "None")
            return " ".join(dict.fromkeys([*extras, *_PY_TYPES]))
        return super().keywords(set_)


from Solar.constants import CODE_FONT_SIZE, UI_FONT_SIZE
from Solar.font_settings import code_font_family
from Solar.scintilla_constants import (
    SCI_GETCURRENTPOS, SCI_POINTXFROMPOSITION, SCI_POINTYFROMPOSITION,
    SCI_TEXTHEIGHT,
)
from Solar.scrollbar_qss import scrollbar_qss
from Solar.theme_utils import ThemeColors
from Solar.widgets.error_lens import ErrorLensOverlay

_MAX_WIDTH = 720          # максимальная ширина, px
_MAX_DOC_HEIGHT = 260     # максимальная высота блока документации, px
_PAD_X, _PAD_Y = 12, 8    # внутренние отступы
_OFFSET_Y = 6             # отступ попапа от строки каретки, px
_SIG_FONT_MAX = 12        # макс. размер шрифта сигнатуры, pt


class SignaturePopup(QFrame):
    """Плашка сигнатуры функции: label с подсветкой активного параметра."""

    def __init__(self, editor):
        super().__init__(editor)
        self.editor = editor
        self.setObjectName("SignaturePopup")
        self.setWindowFlags(
            Qt.WindowType.ToolTip
            | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowDoesNotAcceptFocus
        )
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setMaximumWidth(_MAX_WIDTH)

        self._tc: Optional[ThemeColors] = None
        self._param_hl: Optional[str] = None
        self._signatures: List[dict] = []
        self._active = 0
        self._param = 0

        self._label = QLabel()
        self._label.setTextFormat(Qt.TextFormat.RichText)
        self._label.setWordWrap(False)
        self._label.setTextInteractionFlags(Qt.TextInteractionFlag.NoTextInteraction)
        self._label.setContentsMargins(_PAD_X, _PAD_Y, _PAD_X, _PAD_Y)

        self._sep = QFrame()
        self._sep.setFrameShape(QFrame.Shape.NoFrame)
        self._sep.setFixedHeight(1)
        self._sep.hide()

        self._doc_scroll = QScrollArea()
        self._doc_scroll.setWidgetResizable(True)
        self._doc_scroll.setFrameShape(QFrame.Shape.NoFrame)
        self._doc_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._doc_scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self._doc_scroll.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._doc_scroll.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)

        self._doc_wrap = QWidget()
        self._doc_wrap.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self._doc_layout = QVBoxLayout(self._doc_wrap)
        self._doc_layout.setContentsMargins(_PAD_X, 6, _PAD_X, _PAD_Y)
        self._doc_layout.setSpacing(4)
        self._doc_layout.addStretch(1)
        self._doc_scroll.setWidget(self._doc_wrap)
        self._doc_code_views = []

        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(0)
        lay.addWidget(self._label)
        lay.addWidget(self._sep)
        lay.addWidget(self._doc_scroll)
        self._doc_scroll.hide()

    # --- Тема ---

    def paintEvent(self, event):
        """Фон-плашка с рамкой (QSS не перекрашивает фон из-за прозрачности)."""
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        if self._tc is None:
            bg, border = QColor("#1e1e2e"), QColor("#45475a")
        else:
            bg = self._tc.shade(105)
            border = QColor(self._tc.border)
        path = QPainterPath()
        path.addRoundedRect(
            QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6
        )
        p.fillPath(path, bg)
        p.setPen(QPen(border, 1))
        p.drawPath(path)

    def apply_theme(self, theme_name: str):
        """Применить палитру под тему редактора."""
        self._tc = ThemeColors.from_theme_manager(
            self.editor.theme_manager, theme_name
        )
        self._param_hl = self._param_highlight_bg(self._tc)

        sig_font = self._signature_font()
        doc_font = QFont(self.font())
        doc_font.setPointSize(UI_FONT_SIZE)

        self._label.setFont(sig_font)
        self.setFont(doc_font)
        self._doc_wrap.setFont(doc_font)
        for i in range(self._doc_layout.count()):
            w = self._doc_layout.itemAt(i).widget()
            if isinstance(w, QLabel):
                w.setFont(doc_font)
        self._label.setStyleSheet(
            f"background: transparent; color: {self._tc.fg};"
        )
        self._doc_scroll.setStyleSheet("background: transparent; border: none;")
        self._doc_scroll.viewport().setStyleSheet("background: transparent;")
        self._doc_wrap.setStyleSheet("background: transparent;")
        sep_color = QColor(self._tc.border)
        sep_color.setAlpha(140 if self._tc.dark else 110)
        self._sep.setStyleSheet(
            f"background-color: {sep_color.name(QColor.NameFormat.HexArgb)};"
            "border: none;"
            f"margin-left: {_PAD_X}px; margin-right: {_PAD_X}px;"
        )
        self._paint()

    def _doc_code_bg(self) -> str:
        tc = self._tc
        return tc.shade(105).name() if tc else "#313244"

    def _inline_md(self, escaped: str) -> str:
        """Минимальный markdown->HTML для doc-блоков (code/bold/italic/link)."""
        code_bg = self._doc_code_bg()
        fg = self._tc.fg if self._tc else "#cdd6f4"
        link = self._tc.accent_color.name() if self._tc and self._tc.accent else fg
        muted = self._tc.muted if self._tc else "#6c7086"
        placeholders: List[str] = []

        def _stash(fragment: str) -> str:
            placeholders.append(fragment)
            return f"\x00{len(placeholders) - 1}\x00"

        def _code(m):
            return _stash(
                f"<span style='font-family:{code_font_family()};"
                f"background-color:{code_bg};color:{fg};'>"
                f"&nbsp;{html.escape(m.group(1))}&nbsp;</span>"
            )

        def _link(m):
            return _stash(
                f"<span style='color:{link};text-decoration:underline;'>"
                f"{html.escape(m.group(1))}</span>"
            )

        text = _MD_CODE_RE.sub(_code, escaped)
        text = _MD_LINK_RE.sub(_link, text)
        text = _MD_BOLD_RE.sub(r"<b>\1</b>", text)
        text = _MD_ITALIC_RE.sub(r"<i>\1</i>", text)
        text = re.sub(r"\x00(\d+)\x00", lambda m: placeholders[int(m.group(1))], text)
        # fallback muted color is applied at container level
        return text

    def _clear_doc(self):
        while self._doc_layout.count() > 1:  # keep trailing stretch
            item = self._doc_layout.takeAt(0)
            w = item.widget()
            if w is None:
                continue
            w.setParent(None)
            w.deleteLater()
        self._doc_code_views.clear()

    def _make_doc_label(self, html_text: str) -> QLabel:
        lbl = QLabel()
        lbl.setWordWrap(True)
        lbl.setTextFormat(Qt.TextFormat.RichText)
        lbl.setTextInteractionFlags(Qt.TextInteractionFlag.NoTextInteraction)
        lbl.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignLeft)
        lbl.setStyleSheet(f"background: transparent; color: {self._tc.muted if self._tc else '#6c7086'};")
        lbl.setText(html_text)
        return lbl

    def _lexer_for_lang(self, lang: str):
        key = (lang or "").strip().lower()
        mapping = {
            "python": QsciLexerPython, "py": QsciLexerPython,
            "c": QsciLexerCPP, "cpp": QsciLexerCPP, "c++": QsciLexerCPP,
            "h": QsciLexerCPP, "hpp": QsciLexerCPP,
            "js": QsciLexerJavaScript, "javascript": QsciLexerJavaScript,
            "json": QsciLexerJavaScript,
            "java": QsciLexerJava,
            "xml": QsciLexerXML, "html": QsciLexerXML,
            "sql": QsciLexerSQL,
            "bash": QsciLexerBash, "sh": QsciLexerBash, "shell": QsciLexerBash,
            "lua": QsciLexerLua,
            "ruby": QsciLexerRuby, "rb": QsciLexerRuby,
        }
        if key in mapping:
            return mapping[key]
        try:
            from Solar.lexers.registry import lexer_spec
            spec = lexer_spec(key)
            if spec is not None:
                if key == "python":
                    return _PythonDocLexer
                return spec[1]
        except Exception:
            pass
        lid = getattr(self.editor, "language_id", "") or ""
        try:
            from Solar.lexers.registry import lexer_spec
            spec = lexer_spec(lid)
            if spec is not None:
                if lid == "python":
                    return _PythonDocLexer
                return spec[1]
        except Exception:
            pass
        return _PythonDocLexer if (key == "python" or lid == "python") else QsciLexerCPP

    def _make_doc_code(self, code: str, lang: str = "") -> QsciScintilla:
        view = QsciScintilla(self._doc_wrap)
        view.setUtf8(True)
        view.setReadOnly(True)
        view.setFrameShape(QFrame.Shape.NoFrame)
        view.setWrapMode(QsciScintilla.WrapMode.WrapNone)
        view.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        view.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        view.setCaretWidth(0)
        view.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        view.setContextMenuPolicy(Qt.ContextMenuPolicy.NoContextMenu)
        for i in range(5):
            view.setMarginWidth(i, 0)
        try:
            view.SendScintilla(view.SCI_SETHSCROLLBAR, 1)
            view.SendScintilla(view.SCI_SETVSCROLLBAR, 1)
        except Exception:
            pass
        lexer_cls = self._lexer_for_lang(lang)
        theme_name = getattr(self.editor, "_current_theme", None) or self.editor.theme_manager.current_theme
        theme_data = self.editor.theme_manager.themes.get(theme_name, {})
        try:
            from PyQt6.Qsci import QsciLexerCustom
            is_custom = isinstance(lexer_cls, type) and issubclass(lexer_cls, QsciLexerCustom)
        except Exception:
            is_custom = False
        try:
            lexer = lexer_cls(view, theme_data) if is_custom else lexer_cls(view)
        except TypeError:
            lexer = lexer_cls(view)
        try:
            lexer.setProperty("fold", "1")
        except Exception:
            pass
        view.setLexer(lexer)
        try:
            self.editor.theme_manager.apply_theme(view, theme_name, lexer)
        except Exception:
            pass
        font = self._signature_font()
        for sid in range(128):
            try:
                if lexer.description(sid):
                    lexer.setFont(font, sid)
            except Exception:
                pass
        try:
            bg = QColor(self._doc_code_bg())
            view.setPaper(bg)
            lexer.setDefaultPaper(bg)
            for sid in range(128):
                if lexer.description(sid):
                    lexer.setPaper(bg, sid)
            sb_fg = self._tc.fg_color if self._tc else QColor("#cdd6f4")
            qss = scrollbar_qss(bg.name(), sb_fg)
            view.verticalScrollBar().setStyleSheet(qss)
            view.horizontalScrollBar().setStyleSheet(qss)
        except Exception:
            pass
        code_text = code.rstrip("\n")
        view.setText(code_text)
        try:
            if (lang or "").strip().lower() in ("python", "py", "") and (">>> " in code_text or "... " in code_text):
                self._apply_doctest_prompts(view, code_text)
            else:
                view.SendScintilla(view.SCI_COLOURISE, 0, -1)
        except Exception:
            pass
        try:
            view.zoomTo(0)
        except Exception:
            pass
        try:
            view.SendScintilla(getattr(view, "SCI_SETSCROLLWIDTHTRACKING", 2516), 1)
            sw = self._code_natural_width(view)
            view.SendScintilla(getattr(view, "SCI_SETSCROLLWIDTH", 2274), int(sw))
        except Exception:
            pass
        view.installEventFilter(self)
        self._doc_code_views.append(view)
        self._update_code_view_size(view)
        from PyQt6.QtCore import QTimer
        QTimer.singleShot(0, lambda v=view: (self._update_code_view_size(v) if v in self._doc_code_views else None))
        return view


    def _apply_doctest_prompts(self, view: QsciScintilla, code: str):
        """Подсветка doctest: >>> / ... как промпт, выражение - как Python."""
        lines = code.splitlines()
        prompt_spans = []  # (byte_start, byte_len)
        has_prompt = False
        shadow_lines = []
        off = 0
        for idx, line in enumerate(lines):
            m = re.match(r'^(\s*)(>>> |\.\.\. )', line)
            if m:
                has_prompt = True
                pstart = len(m.group(1).encode('utf-8'))
                plen = len(m.group(2).encode('utf-8'))
                prompt_spans.append((off + pstart, plen))
                shadow_lines.append(line[:m.end()].replace('>', ' ').replace('.', ' ') + line[m.end():])
            else:
                shadow_lines.append(line)
            off += len(line.encode('utf-8')) + (1 if idx < len(lines) - 1 else 0)
        if not has_prompt:
            return
        shadow = "\n".join(shadow_lines)
        try:
            view.setText(shadow)
            view.SendScintilla(view.SCI_COLOURISE, 0, -1)
            code_bytes = shadow.encode('utf-8')
            styles = [view.SendScintilla(view.SCI_GETSTYLEAT, i) for i in range(len(code_bytes))]
            view.setText(code)
            # apply captured styles in runs
            if styles:
                view.SendScintilla(view.SCI_STARTSTYLING, 0, 0)
                i = 0
                n = len(styles)
                while i < n:
                    st = styles[i]
                    j = i + 1
                    while j < n and styles[j] == st:
                        j += 1
                    view.SendScintilla(view.SCI_SETSTYLING, j - i, st)
                    i = j
            # overlay prompt style
            prompt_style = getattr(QsciLexerPython, 'Comment', 1)
            for bstart, blen in prompt_spans:
                view.SendScintilla(view.SCI_STARTSTYLING, bstart, 0)
                view.SendScintilla(view.SCI_SETSTYLING, blen, prompt_style)
        except Exception:
            try:
                view.setText(code)
                view.SendScintilla(view.SCI_COLOURISE, 0, -1)
            except Exception:
                pass
    def _code_natural_width(self, view: QsciScintilla) -> int:
        text = view.text().splitlines() or [""]
        font = view.font()
        try:
            lexer = view.lexer()
            if lexer is not None:
                font = lexer.font(0) or font
        except Exception:
            pass
        fm = QFontMetrics(font)
        max_px = max((fm.horizontalAdvance(ln.expandtabs(4)) for ln in text), default=0)
        try:
            for ln in text:
                b = ln.expandtabs(4).encode("utf-8", errors="replace")
                if b:
                    w = int(view.SendScintilla(view.SCI_TEXTWIDTH, 0, b))
                    if w > 0:
                        max_px = max(max_px, w)
        except Exception:
            pass
        try:
            sw = int(view.SendScintilla(getattr(view, "SCI_GETSCROLLWIDTH", 2274)))
            if sw > 0:
                max_px = max(max_px, sw)
        except Exception:
            pass
        return max(220, max_px + 52)

    def _update_code_view_size(self, view: QsciScintilla):
        lines = max(1, view.lines())
        fm = QFontMetrics(view.font())
        line_h = view.SendScintilla(view.SCI_TEXTHEIGHT, 0) or fm.height() or 16
        max_w = max(120, self.maximumWidth() - 2 * _PAD_X)
        natural_w = self._code_natural_width(view)
        sb_w = view.style().pixelMetric(view.style().PixelMetric.PM_ScrollBarExtent) or 14
        sb_h = sb_w

        show_v = lines > 11
        needed_w = natural_w + (sb_w if show_v else 0)
        block_w = min(needed_w, max_w)
        show_h = block_w > 500 and needed_w > block_w

        width = block_w if show_h else min(needed_w, max_w)

        visible_lines = min(lines, 11) if show_v else lines
        h = max(line_h + 10, visible_lines * line_h + 8)
        if show_h:
            h += sb_h
        try:
            view.SendScintilla(getattr(view, "SCI_SETSCROLLWIDTH", 2274), int(max(1, natural_w)))
        except Exception:
            pass

        try:
            view.setVerticalScrollBarPolicy(
                Qt.ScrollBarPolicy.ScrollBarAsNeeded if show_v else Qt.ScrollBarPolicy.ScrollBarAlwaysOff
            )
            view.setHorizontalScrollBarPolicy(
                Qt.ScrollBarPolicy.ScrollBarAsNeeded if show_h else Qt.ScrollBarPolicy.ScrollBarAlwaysOff
            )
            view.SendScintilla(view.SCI_SETVSCROLLBAR, 1 if show_v else 0)
            view.SendScintilla(view.SCI_SETHSCROLLBAR, 1 if show_h else 0)
        except Exception:
            pass

        view.setFixedHeight(h)
        view.setFixedWidth(max(120, min(width, max_w)))

    def eventFilter(self, obj, event):
        # Prevent Ctrl+wheel zoom in doc code blocks
        try:
            from PyQt6.QtCore import QEvent
            if event.type() == QEvent.Type.Wheel and obj in self._doc_code_views:
                if event.modifiers() & Qt.KeyboardModifier.ControlModifier:
                    return True
        except Exception:
            pass
        return super().eventFilter(obj, event)

    def _render_doc_blocks(self, doc: str) -> List[tuple]:
        """Markdown doc -> blocks: ('html', html) or ('code', lang, code)."""
        if not doc:
            return []
        lines = doc.replace("\r\n", "\n").replace("\r", "\n").split("\n")
        blocks = []
        para: List[str] = []
        in_code = False
        code_lines: List[str] = []
        code_lang = ""
        tc = self._tc
        fg = tc.fg if tc else "#cdd6f4"
        muted = tc.muted if tc else "#6c7086"
        border = tc.border if tc else "#45475a"

        def flush_para():
            if para:
                txt = "<br>".join(self._inline_md(html.escape(p)) for p in para)
                blocks.append(("html", f"<div style='color:{muted};'>{txt}</div>"))
                para.clear()

        def flush_code():
            nonlocal code_lang
            src = "\n".join(code_lines).rstrip("\n")
            blocks.append(("code", code_lang, src))
            code_lines.clear()
            code_lang = ""

        doctest_re = re.compile(r'^\s*(>>>|\.\.\.)\s?')
        i = 0
        while i < len(lines):
            raw = lines[i]
            stripped = raw.strip()
            if stripped.startswith("```"):
                if in_code:
                    flush_code()
                    in_code = False
                else:
                    flush_para()
                    in_code = True
                    code_lang = stripped[3:].strip()
                i += 1
                continue
            if in_code:
                code_lines.append(raw)
                i += 1
                continue
            if not stripped:
                flush_para()
                i += 1
                continue
            # doctest block in plain text: >>> / ...
            if doctest_re.match(raw):
                flush_para()
                dt = [raw.rstrip()]
                i += 1
                while i < len(lines):
                    nxt = lines[i]
                    if nxt.strip().startswith("```"):
                        break
                    if doctest_re.match(nxt):
                        dt.append(nxt.rstrip())
                        i += 1
                        continue
                    # include output lines until blank line or next markdown structure
                    nstr = nxt.strip()
                    if not nstr:
                        break
                    if _MD_HEADING_RE.match(nstr) or _MD_RULE_RE.match(nstr) or _MD_BULLET_RE.match(nxt) or _MD_NUMBER_RE.match(nxt):
                        break
                    dt.append(nxt.rstrip())
                    i += 1
                    # stop after output line unless next is doctest/continuation
                    if i < len(lines) and not doctest_re.match(lines[i]) and lines[i].strip():
                        # keep consuming consecutive output lines
                        continue
                    break
                blocks.append(("code", "python", "\n".join(dt)))
                continue
            m = _MD_HEADING_RE.match(stripped)
            if m:
                flush_para()
                body = self._inline_md(html.escape(m.group(2).strip()))
                blocks.append(("html", f"<div style='color:{fg};font-weight:600;'>{body}</div>"))
                i += 1
                continue
            if _MD_RULE_RE.match(stripped):
                flush_para()
                blocks.append(("html", f"<div style='color:{muted};border-top:1px solid {border};margin:4px 0;'></div>"))
                i += 1
                continue
            mb = _MD_BULLET_RE.match(raw)
            mn = _MD_NUMBER_RE.match(raw)
            if mb or mn:
                flush_para()
                indent = len((mb or mn).group(1)) // 2
                body = self._inline_md(html.escape((mb or mn).group(2).strip()))
                bullet = "•" if mb else "-"
                blocks.append(("html", f"<div style='margin-left:{10 + indent * 12}px;color:{muted};'><span style='color:{muted};'>{bullet}</span> {body}</div>"))
                i += 1
                continue
            para.append(raw.rstrip())
            i += 1

        if in_code:
            flush_code()
        flush_para()
        return blocks

    def _signature_font(self) -> QFont:
        """Моноширинный шрифт подписи в духе hover-попапа."""
        size = self.editor.font().pointSize() or CODE_FONT_SIZE
        return QFont(code_font_family(), max(min(size, _SIG_FONT_MAX), 9))

    @staticmethod
    def _blend(a: QColor, b: QColor, t: float) -> QColor:
        t = max(0.0, min(1.0, t))
        return QColor(
            int(a.red() + (b.red() - a.red()) * t),
            int(a.green() + (b.green() - a.green()) * t),
            int(a.blue() + (b.blue() - a.blue()) * t),
        )

    def _param_highlight_bg(self, tc: ThemeColors) -> str:
        """Мягкий фон активного параметра (без резкой плашки selection)."""
        base = tc.shade(105)
        if tc.accent:
            src = tc.accent_color
            mix = 0.22 if tc.dark else 0.16
        elif tc.selection:
            src = QColor(tc.selection)
            mix = 0.30 if tc.dark else 0.22
        else:
            src = QColor(tc.muted)
            mix = 0.22
        return self._blend(base, src, mix).name()

    # --- Содержимое ---

    def set_signatures(self, signatures: List[dict], active: int, param: int):
        """Обновляет содержимое: активная сигнатура + подсветка параметра."""
        self._signatures = signatures or []
        self._active = (max(0, min(active, len(self._signatures) - 1))
                        if self._signatures else 0)
        self._param = param
        self._paint()

    def _paint(self):
        """Перерисовывает label/doc под текущую сигнатуру."""
        tc = self._tc
        if tc is None:
            return
        sigs = self._signatures or []
        sig = sigs[self._active] if sigs else {}
        label = sig.get("label") or ""
        params = sig.get("params") or []
        active_param = self._param if 0 <= self._param < len(params) else -1

        html_label = self._render_label(label, params, active_param, tc)
        if len(sigs) > 1:
            html_label += (
                f"<span style='color:{tc.muted}'>&nbsp;&nbsp;"
                f"{self._active + 1}/{len(sigs)}</span>"
            )
        self._label.setText(html_label)

        param_doc = ""
        if 0 <= active_param < len(params):
            param_doc = params[active_param][3] if len(params[active_param]) > 3 else ""

        doc = sig.get("doc") or ""
        blocks = []
        if param_doc:
            param_name = params[active_param][0] or "parameter"
            blocks.append(("html", f"<div style='color:{tc.fg};font-weight:600;'>{html.escape(str(param_name))}</div>"))
            blocks.extend(self._render_doc_blocks(param_doc))
        if doc:
            if blocks:
                blocks.append(("html", "<div style='height:4px;'></div>"))
            blocks.extend(self._render_doc_blocks(doc))

        self._clear_doc()
        if blocks:
            for b in blocks:
                if b[0] == "html":
                    self._doc_layout.insertWidget(self._doc_layout.count() - 1, self._make_doc_label(b[1]))
                elif b[0] == "code":
                    self._doc_layout.insertWidget(self._doc_layout.count() - 1, self._make_doc_code(b[2], b[1]))
            self._label.setContentsMargins(_PAD_X, _PAD_Y, _PAD_X, _PAD_Y)
            self._sep.show()
            self._doc_scroll.show()
        else:
            self._sep.hide()
            self._doc_scroll.hide()
            self._doc_scroll.setMaximumHeight(0)
            # одна строка: нижний отступ сохраняем
            self._label.setContentsMargins(_PAD_X, _PAD_Y, _PAD_X, _PAD_Y)
        self.adjustSize()

    def _render_label(self, label: str, params, param_idx: int, tc) -> str:
        """label сигнатуры в HTML: читаемый текст и мягкая подсветка параметра."""
        esc_label = html.escape(label)
        fg = tc.fg
        if not params:
            return f"<span style='color:{fg}'>{esc_label}</span>"

        hl = self._param_hl or self._param_highlight_bg(tc)
        parts = []
        last = 0
        for i, pinfo in sorted(enumerate(params), key=lambda p: p[1][1]):
            if not isinstance(pinfo, (list, tuple)) or len(pinfo) < 3:
                continue
            _name, s, e = pinfo[0], pinfo[1], pinfo[2]
            try:
                s, e = int(s), int(e)
            except (TypeError, ValueError):
                continue
            if s < 0 or e < s or s >= len(label):
                continue
            e = min(e, len(label))
            if s > last:
                parts.append(
                    f"<span style='color:{fg}'>{html.escape(label[last:s])}</span>"
                )
            seg = html.escape(label[s:e])
            if i == param_idx:
                parts.append(
                    f"<span style='color:{fg};background-color:{hl};"
                    f"font-weight:600'>{seg}</span>"
                )
            else:
                parts.append(f"<span style='color:{fg}'>{seg}</span>")
            last = max(last, e)
        if last < len(label):
            parts.append(
                f"<span style='color:{fg}'>{html.escape(label[last:])}</span>"
            )
        return "".join(parts) if parts else f"<span style='color:{fg}'>{esc_label}</span>"

    # --- Показ ---

    def _available_screen(self, global_pt: QPoint):
        screen = QApplication.screenAt(global_pt)
        if screen is None:
            screen = self.screen() or QApplication.primaryScreen()
        return screen.availableGeometry() if screen is not None else None

    def _fit_to_screen(self, anchor_top: int, anchor_bottom: int):
        """Подогнать размер/позицию под экран: скролл doc, клампинг."""
        screen_geo = self._available_screen(
            self.mapToGlobal(self.rect().topLeft())
        )
        if screen_geo is None:
            return

        max_w = max(200, min(_MAX_WIDTH, screen_geo.width() - 8))
        self.setMaximumWidth(max_w)
        self._label.setMaximumWidth(max_w)
        for view in self._doc_code_views:
            try:
                self._update_code_view_size(view)
            except Exception:
                pass

        max_popup_h = max(80, screen_geo.height() - 8)
        natural_doc_h = self._doc_wrap.sizeHint().height() + 2
        doc_cap = min(_MAX_DOC_HEIGHT, max(60, max_popup_h - 80))
        if self._doc_scroll.isVisible():
            self._doc_scroll.setMaximumHeight(min(natural_doc_h, doc_cap))
        else:
            self._doc_scroll.setMaximumHeight(0)

        self.adjustSize()
        if self.height() > max_popup_h and self._doc_scroll.isVisible():
            over = self.height() - max_popup_h
            cur = self._doc_scroll.maximumHeight()
            self._doc_scroll.setMaximumHeight(max(60, cur - over))
            self.adjustSize()

        gy = anchor_top - self.height() - _OFFSET_Y
        if gy < screen_geo.top():
            gy = anchor_bottom + _OFFSET_Y
        if gy + self.height() > screen_geo.bottom():
            gy = max(screen_geo.top(), screen_geo.bottom() - self.height())

        gx = self.x()
        gx = min(max(gx, screen_geo.left()), screen_geo.right() - self.width())
        self.move(gx, gy)

    def show_at_caret(self, editor):
        """Показать над/под кареткой, с учётом границ экрана и overflow."""
        try:
            pos = editor.SendScintilla(SCI_GETCURRENTPOS)
            x = editor.SendScintilla(SCI_POINTXFROMPOSITION, 0, pos)
            y = editor.SendScintilla(SCI_POINTYFROMPOSITION, 0, pos)
            margin = ErrorLensOverlay.margin_x(editor)
            line_h = editor.SendScintilla(SCI_TEXTHEIGHT, 0)
        except RuntimeError:
            return
        pt = editor.mapToGlobal(QPoint(int(x) + margin, int(y)))
        screen_geo = self._available_screen(pt)
        if screen_geo is None:
            return

        max_w = max(200, min(_MAX_WIDTH, screen_geo.width() - 8))
        self.setMaximumWidth(max_w)
        self._label.setMaximumWidth(max_w)
        for view in self._doc_code_views:
            try:
                self._update_code_view_size(view)
            except Exception:
                pass

        gx = min(max(pt.x(), screen_geo.left()), screen_geo.right() - 200)
        self.move(gx, pt.y() - 40)
        self.adjustSize()
        self._fit_to_screen(pt.y(), pt.y() + line_h)
        self.show()
        self.raise_()
