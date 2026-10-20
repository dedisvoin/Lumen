"""
HoverPopup — кастомный тултип в стиле темы редактора.

Поддерживает упрощённый markdown из hover LSP-серверов (clangd, pyright):
заголовки (#/##), разделители (---), списки (-/1.), параграфы, инлайн-код
(``), жирный (**), курсив (*), ссылки [text](url) и код-фенсы (```) с
настоящей подсветкой синтаксиса через лексеры Qsci и цвета темы редактора.
Lumen-код (```lumen) красится своей regex-подсветкой: ключевые слова языка
(let/ret/echo/...) больше не падают в fallback QsciLexerCPP.
Сигнатуры вроде `int sum(int a, int b)` раскрашиваются по токенам:
типы, ключевые слова, имена функций, переменные, строки и комментарии,
пунктуация — приглушённым цветом. Многострочные сигнатуры pyright
(«(function) def print(» с параметрами) рендерятся заголовком + строки
с раскраской и сохранёнными отступами.
"""

import html
import re
from typing import Dict, List, Optional, Tuple

from PyQt6.QtCore import (
    QEasingCurve, Qt, QPoint, QPointF, QRectF, QVariantAnimation,
)
from PyQt6.QtGui import (
    QFont, QFontMetrics, QColor, QCursor, QPainter, QPainterPath, QPen,
    QPixmap,
)
from PyQt6.QtWidgets import (
    QFrame, QLabel, QVBoxLayout, QApplication, QSizePolicy, QScrollArea,
    QWidget, QGraphicsDropShadowEffect,
)
from PyQt6.Qsci import (
    QsciScintilla, QsciLexer, QsciLexerCPP, QsciLexerPython,
    QsciLexerJava, QsciLexerJavaScript, QsciLexerXML, QsciLexerSQL,
    QsciLexerBash, QsciLexerLua, QsciLexerRuby,
)
from PyQt6 import sip

from editor.constants import CODE_FONT_SIZE
from editor.font_settings import code_font_family
from editor.scintilla_constants import (
    SCI_COLOURISE, SCI_TEXTHEIGHT, SCI_TEXTWIDTH,
)
from editor.constants import SCROLLBAR_WIDTH
from editor.theme_utils import ThemeColors
from editor.text_utils import CPP_KEYWORDS, CPP_TYPES, NUMBER_PATTERN

_FADE_IN_MS = 150          # длительность появления плашки
_FADE_OUT_MS = 120         # длительность исчезания плашки
_WIPE_MS = 130             # перетекание: старый кадр растворяется в новый
_MOVE_MS = 180             # плавное перемещение плашки к новому токену
_SHADOW_MARGIN = 40        # прозрачный запас вокруг карточки под тень
_MAX_WIDTH = 900          # максимальная ширина попапа, px
_MIN_WIDTH = 260          # минимальная ширина попапа, px (короткие hover)
_MAX_HEIGHT = 480         # максимальная высота попапа, px (дальше — прокрутка)
_SB_GAP = 6               # зазор между контентом и полосой прокрутки (внутри попапа)
_SB_PAD_RIGHT = 5         # отступ бегунка от правого края плашки
_SB_PAD_V = 6             # отступ бегунка от верха/низа плашки
_SB_THUMB_W = 5           # ширина рисуемого бегунка попапа, px
_CODE_MAX_WIDTH = 860     # максимальная ширина блока кода, px
_PAD_X, _PAD_Y = 12, 8    # внутренние отступы
_OFFSET_X, _OFFSET_Y = 25, 24  # смещение от курсора мыши
_POPUP_MAX_SIZE = 12      # макс. размер шрифта попапа, pt (иначе наследует 20+ от редактора)

# Классы токенов заголовка hover («function sum», «class Foo», литералы,
# формат pyright: «(function) foo(...)», «(module) os», «(type alias) Alias»)
_KIND_GROUP = {
    "function": "func", "method": "func", "macro": "func",
    "constructor": "func", "lambda": "func", "property": "func",
    "builtin": "func", "overload": "func", "fn": "func",
    "class": "cls", "struct": "cls", "enum": "cls", "interface": "cls",
    "union": "cls", "concept": "cls", "alias": "cls", "typedef": "cls",
    "type alias": "cls", "type": "cls",
    "namespace": "ns", "module": "ns", "package": "ns",
    "variable": "var", "parameter": "var", "field": "var",
    "instance": "var", "self": "var",
}
_KIND_WORDS = set(_KIND_GROUP) | {
    "integer-literal", "character-literal", "floating-literal",
    "string-literal", "boolean-literal", "nullptr-literal",
    "user-defined-literal",
}

# Ключевые слова C/Python-сигнатур (модификаторы/структуры/поток)
_KEYWORDS = set(CPP_KEYWORDS) | {
    # python
    "def", "lambda", "import", "from", "as", "pass", "raise",
    "yield", "assert", "global", "nonlocal", "del", "with", "await",
    "async", "except", "finally", "elif", "not", "and", "or", "in",
    "is", "None", "True", "False",
}
# Базовые типы — в сигнатурах красятся цветом типа (не keyword)
_BASE_TYPES = set(CPP_TYPES) | {
    "size_t", "ptrdiff_t", "decltype",
    # python (lowercase типы)
    "str", "bytes", "bytearray", "list", "dict", "set", "tuple",
    "frozenset", "object", "range",
}

_HEADING_RE = re.compile(r"^(#{1,6})\s+(.*)$")
_RULE_RE = re.compile(r"^[-*_]{3,}\s*$")
_BULLET_RE = re.compile(r"^(\s*)([-*+])\s+(.*)$")
_NUMBERED_RE = re.compile(r"^(\s*)(\d{1,3})[.)]\s+(.*)$")
_SECTION_RE = re.compile(
    r"^(Parameters|Param|Arguments|Returns|Return|Exceptions|Throws|"
    r"Note|Notes|See also|Example|Examples|Warning|Important|Requires|"
    r"Precondition|Postcondition|Template|Template parameters|"
    r"Type parameters|Since|Deprecated|Bugs|Todo|Author|Authors)"
    r"\s*:?\s*(.*)$"
)
_INLINE_CODE_RE = re.compile(r"`([^`\n]+)`")
SENTINEL = "\x01"  # плейсхолдер внутри спрятанных фрагментов
_SENTINEL_RE = re.compile(r"\x00(\d+)\x00")
_BOLD_RE = re.compile(r"\*\*([^*]+)\*\*")
_ITALIC_RE = re.compile(r"\*([^*]+)\*")
_LINK_RE = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")
# pyright-заголовок: «(function) foo(a: int) -> bool»
_KIND_HEADER_RE = re.compile(r"^\(([^)]+)\)\s*(.*)$")

# Токены C-подобных строк: числа/строки/комментарии/операторы/идентификаторы
_SIG_TOKEN_RE = re.compile(
    r"(" + NUMBER_PATTERN
    + r"|'[^'\\]*(?:\\.[^'\\]*)*'|\"[^\"\\]*(?:\\.[^\"\\]*)*\""
    + r"|//[^\n]*|::|->|<=|>=|==|!=|&&|\|\||<<|>>"
    + r"|[A-Za-z_]\w*|\s+|[^\s])"
)

# Слова языка Lumen — те же наборы, что в editor/lexers/lumen/lumen_lang.py.
# Lumen-код в фенсах (```lumen) красится своей regex-подсветкой, а не
# fallback-лексером QsciLexerCPP.
_LUMEN_KEYWORDS = {
    "if", "else", "for", "while", "let", "in", "and", "or", "echo",
    "ret", "assert", "lambda", "do", "is", "as", "ptr",
    "struct", "enum", "namespace", "fn", "continue", "break",
    "new", "del", "typeof", "sizeof", "out", "outln", "input", "exit",
    "append", "reserve", "typeis", "mod", "eval",
    "__LINE__", "__FILE__", "__LUMEN__",
}
_LUMEN_MODIFIERS = {
    "const", "ref", "static", "global", "final", "private", "shadow", "strict",
}
_LUMEN_TYPES = {
    "str", "char", "bool", "Type", "Namespace", "Func", "Lambda",
    "auto", "Null", "any", "i8", "i16", "i32", "i64", "f16", "f32", "f64",
    "Array", "Tuple",
}
_LUMEN_LITERALS = {"true", "false", "null", "self", "this"}
_LUMEN_DIRECTIVES = {
    "define", "macro", "include", "ifdef", "ifndef", "elifdef", "elif",
    "else", "endif", "undef",
}

# Токены Lumen-кода: то же, что C-строки, плюс директивы `#define` и т.п.
# (все альтернативы — в одной группе, чтобы findall вернул строки-токены)
_LUMEN_TOKEN_RE = re.compile(
    r"("
    + r"#[a-zA-Z_][a-zA-Z0-9_]*|"
    + NUMBER_PATTERN
    + r"|'[^'\\]*(?:\\.[^'\\]*)*'|\"[^\"\\]*(?:\\.[^\"\\]*)*\""
    + r"|//[^\n]*|::|->|<=|>=|==|!=|&&|\|\||<<|>>"
    + r"|[A-Za-z_]\w*|\s+|[^\s]"
    + r")"
)

def _is_generic_type_decl(content: str) -> bool:
    """`struct[T] Vec2`, `struct[T: Type] Vec2`, `enum[T] E` — заголовок
    генерик-структуры/enum: ключевое слово и сразу за ним (за блоком
    `[...]`) имя. Внутри скобок есть пробелы и ограничения (`T: Type`,
    `U: Type = i64`), поэтому разбивка на токены по первому пробелу
    режет `struct[T:` — считаем скобки вручную."""
    m = re.match(r"^(struct|enum)\b", content)
    if not m:
        return False
    pos = m.end()
    if pos < len(content) and content[pos] == "[":
        depth = 1
        pos += 1
        while pos < len(content) and depth:
            if content[pos] == "[":
                depth += 1
            elif content[pos] == "]":
                depth -= 1
            pos += 1
        if depth:
            return False  # скобки не закрылись — это не заголовок
    rest = content[pos:].lstrip()
    return bool(rest and (rest[0].isalpha() or rest[0] == "_"))


def lumen_token_spans(text: str) -> List[Tuple[str, str]]:
    """Токены Lumen-кода -> [(стиль, текст)] для цветного рендера.

    Вынесено из HoverPopup._lumen_spans, чтобы переиспользовать подсветку
    в других местах (список символов): regex-эвристика без Qsci-лексера —
    ключевые слова/модификаторы/типы/литералы/спецоператоры/директивы по
    словарям языка; имя namespace (после `namespace`/`struct` или перед
    `::`) — цветом namespace; имя перед `(` — функция; после `.` — объект.
    """
    tokens = _LUMEN_TOKEN_RE.findall(text)
    n = len(tokens)
    if not n:
        return []
    styles = ["fg"] * n
    # числа/строки/комментарии/директивы — по виду токена, пунктуация — muted
    for i, tok in enumerate(tokens):
        if tok.startswith("//"):
            styles[i] = "com"
        elif tok[:1] in "\"'":
            styles[i] = "str"
        elif tok.startswith("#"):
            styles[i] = "dir" if tok[1:] in _LUMEN_DIRECTIVES else "punct"
        elif (tok[0].isdigit()
              or (tok[0] == "." and len(tok) > 1 and tok[1].isdigit())):
            styles[i] = "num"
        elif not (tok[0].isalpha() or tok[0] == "_"):
            # скобки, `:`, `=`, `*`, `->`, `::` — приглушённым
            styles[i] = "punct"
    expecting_ns = False
    expecting_var = False
    i = 0
    while i < n:
        tok = tokens[i]
        if not tok or not tok.strip():
            i += 1
            continue  # пробелы не влияют на ожидание имени namespace
        if styles[i] != "fg":
            if expecting_ns and tok == "[":
                # `struct[T] Name` / `enum[T] E`: генерик-параметры
                # между ключевым словом и именем не сбивают ожидание
                depth = 1
                j = i + 1
                while j < n:
                    if tokens[j] == "[":
                        depth += 1
                    elif tokens[j] == "]":
                        depth -= 1
                        if depth == 0:
                            break
                    j += 1
                i = j
            else:
                expecting_ns = False
            i += 1
            continue
        nxt = next((tokens[j] for j in range(i + 1, n)
                    if tokens[j].strip()), None)
        prv = next((tokens[j] for j in range(i - 1, -1, -1)
                    if tokens[j].strip()), None)
        if expecting_var:
            # имя переменной сразу после `let` / `ref let` — цветом переменной
            expecting_var = False
            if styles[i] == "fg":
                styles[i] = "var"
                i += 1
                continue
        if tok in ("namespace", "struct", "enum"):
            expecting_ns = True
            styles[i] = "kw"
        elif expecting_ns:
            expecting_ns = False
            styles[i] = "ns"
        elif tok in _LUMEN_KEYWORDS:
            styles[i] = "kw"
            expecting_var = tok == "let"
        elif tok in _LUMEN_MODIFIERS:
            styles[i] = "mod"
        elif tok in _LUMEN_TYPES:
            styles[i] = "type"
        elif tok in _LUMEN_LITERALS:
            styles[i] = "lit"
        elif nxt == "::":
            styles[i] = "ns"
        elif prv == "::":
            styles[i] = "func" if nxt == "(" else "ns"
        elif nxt == "(":
            styles[i] = "func"
        elif nxt == "[":
            # генерик-функция: `name[T](...)` — имя функции по цвету;
            # индекс `arr[i]` без `(` после парной ']' остаётся переменной
            depth = 0
            j = i + 1
            while j < n:
                t = tokens[j]
                if t == "[":
                    depth += 1
                elif t == "]":
                    depth -= 1
                    if depth == 0:
                        after = next((tokens[k] for k in range(j + 1, n)
                                      if tokens[k].strip()), None)
                        if after == "(":
                            styles[i] = "func"
                        break
                elif depth == 0:
                    break
                j += 1
        elif nxt == ".":
            styles[i] = "obj"
        else:
            styles[i] = "fg"
        i += 1
    return [(s, t) for s, t in zip(styles, tokens)]


class _WipeOverlay(QWidget):
    """Прозрачная заглушка поверх плашки: рисует старый кадр попапа.

    При быстрой смене подсвеченного слова контент попапа заменяется под
    заглушкой, а эта заглушка с прежним кадром растворяется — старый текст
    «перетекает» в новый, а не мигает."""
    def __init__(self, parent=None):
        super().__init__(parent)
        self._pix = None
        self._alpha = 0.0
        self.setAttribute(
            Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAttribute(
            Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)

    def set_pixmap(self, pix: QPixmap):
        self._pix = pix
        self.update()

    def set_alpha(self, alpha: float):
        self._alpha = alpha
        self.update()

    def paintEvent(self, event):
        if self._pix is None:
            return
        p = QPainter(self)
        p.setOpacity(self._alpha)
        # Кадр старого попапа рисуется 1:1 от левого верхнего угла —
        # новый контент, вылезающий за старый размер, проявляется сам.
        p.drawPixmap(QPointF(0, 0), self._pix)
        p.end()


class _PopupCard(QFrame):
    """Карточка содержимого попапа: рисует фон, рамку и бегунок прокрутки.

    Рисование живёт здесь, а не на окне попапа, потому что вокруг карточки
    окно оставляет прозрачный запас (_SHADOW_MARGIN), на который свешивается
    QGraphicsDropShadowEffect (как у плашки закладки и строки ввода) — тень
    не обрезается границами frameless-окна."""
    def __init__(self, popup):
        super().__init__(popup)
        self._popup = popup
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        popup = self._popup
        if popup._tc is None:
            bg, border = QColor("#1e1e2e"), QColor("#45475a")
        else:
            bg = popup._tc.shade(105)
            border = QColor(popup._tc.border)
        path = QPainterPath()
        path.addRoundedRect(
            QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6
        )
        p.fillPath(path, bg)
        p.setPen(QPen(border, 1))
        p.drawPath(path)
        popup._draw_thumb(p, self)
        p.end()


class HoverPopup(QFrame):
    """Плашка с подсказкой: фон/границы темы, код — с подсветкой."""

    def __init__(self, editor, bridge=None):
        super().__init__(editor)
        self.editor = editor
        self._bridge = bridge
        self.setObjectName("HoverPopup")
        self.setWindowFlags(
            Qt.WindowType.ToolTip
            | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowDoesNotAcceptFocus
        )
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        # Прозрачный запас вокруг карточки под тень (см. _PopupCard)
        self._shadow_margin = _SHADOW_MARGIN

        # Плавное появление/исчезание: анимируем непрозрачность окна.
        # Скрытие откладывается до конца fade-out, поэтому выглядит плавно.
        self._hide_pending = False
        self._opacity = QVariantAnimation(self)
        self._opacity.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._opacity.valueChanged.connect(self._on_opacity)
        self._opacity.finished.connect(self._on_opacity_done)

        # Перетекание старого контента в новый (быстрая смена слова под
        # курсором): снимок предыдущего кадра + растворяющаяся заглушка.
        self._last_content: Optional[str] = None
        self._wipe_pix: Optional[QPixmap] = None
        self._wipe: Optional[_WipeOverlay] = None
        self._wipe_anim = QVariantAnimation(self)
        self._wipe_anim.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._wipe_anim.valueChanged.connect(self._on_wipe)
        self._wipe_anim.finished.connect(self._on_wipe_done)

        # Плавное переезд плашки к новому токену при смене слова
        self._pos_anim = QVariantAnimation(self)
        self._pos_anim.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._pos_anim.valueChanged.connect(self._on_pos)

        self._theme_name: Optional[str] = None
        self._tc: Optional[ThemeColors] = None
        self._styles: dict = {}

        # Содержимое — в прокрутке: длинный hover (справка по функции,
        # много параметров) не растягивается на весь экран, а получает
        # максимальную высоту и прокручивается колесом мыши над плашкой.
        # Карточка контента лежит внутри окна с прозрачным запасом под тень
        # (_SHADOW_MARGIN), иначе QGraphicsDropShadowEffect обрезался бы
        # границами frameless-окна.
        self._root = QVBoxLayout(self)
        self._root.setContentsMargins(
             _SHADOW_MARGIN, _SHADOW_MARGIN,
             _SHADOW_MARGIN, _SHADOW_MARGIN)
        self._root.setSpacing(0)
        self._wrap = QFrame(self)
        self._wrap.setAttribute(
            Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self._layout = QVBoxLayout(self._wrap)
        self._layout.setContentsMargins(_PAD_X, _PAD_Y, _PAD_X, _PAD_Y)
        self._layout.setSpacing(6)
        self._scroll = QScrollArea(self)
        self._scroll.setWidgetResizable(False)
        self._scroll.setFrameShape(QFrame.Shape.NoFrame)
        self._scroll.setHorizontalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._scroll.setVerticalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self._scroll.setWidget(self._wrap)
        # Перерисовка попапа при прокрутке — бегунок следует за контентом
        self._scroll.verticalScrollBar().valueChanged.connect(
            lambda _v: self.update())
        self._card = _PopupCard(self)
        card_layout = QVBoxLayout(self._card)
        card_layout.setContentsMargins(0, 0, 0, 0)
        card_layout.setSpacing(0)
        card_layout.addWidget(self._scroll)
        self._root.addWidget(self._card)

        # Тень как у плашки закладки и строки ввода, но крупнее:
        # больший blur/смещение требуют и большего запаса (_SHADOW_MARGIN)
        shadow = QGraphicsDropShadowEffect(self._card)
        shadow.setBlurRadius(100)
        shadow.setOffset(-5, 6)

        shadow.setColor(QColor(0, 0, 0, 90))
        self._card.setGraphicsEffect(shadow)

        # Короткий hover (литерал, тип) не должен схлопываться в узкую
        # полоску — layout держит как минимум _MIN_WIDTH
        self.setMinimumWidth(_MIN_WIDTH)
        self.setMaximumWidth(_MAX_WIDTH)
        self._wrap.setMinimumWidth(_MIN_WIDTH)
        # Кодовые вью по классу лексера: переиспользуются между показами
        self._code_views: Dict[type, QsciScintilla] = {}
        self._code_lexers: Dict[type, QsciLexer] = {}

    # ------------------------------------------------------------------
    # тема и цвета
    # ------------------------------------------------------------------

    def apply_theme(self, theme_name: str):
        """Перекрашивает попап (и все кодовые вью) в цвета темы."""
        self._theme_name = theme_name
        self._tc = ThemeColors.from_theme_manager(
            self.editor.theme_manager, theme_name
        )
        theme = self.editor.theme_manager.themes.get(theme_name, {})
        self._styles = theme.get("styles", {})

        self.setStyleSheet("")  # фон/рамку/бегунок рисует карточка попапа
        self._card.setStyleSheet("background: transparent; border: none;")
        self._scroll.setStyleSheet("background: transparent; border: none;")
        # Нативный скроллбар невидим: он не красится поверх вьюпорта в
        # нестандартной позиции. Бегунок с отступом рисует попап сам.
        invisible_sb = (
            "QScrollBar:vertical { width: 0px; background: transparent;"
            " border: none; }"
            " QScrollBar:horizontal { height: 0px; background: transparent;"
            " border: none; }"
        )
        self._scroll.verticalScrollBar().setStyleSheet(invisible_sb)
        self._scroll.horizontalScrollBar().setStyleSheet(invisible_sb)
        for lexer_cls, view in self._code_views.items():
            lexer = self._code_lexers.get(lexer_cls)
            if lexer is None or sip.isdeleted(view):
                continue
            try:
                self.editor.theme_manager.apply_theme(
                    view, theme_name, lexer
                )
                self._apply_code_font(lexer)
                self._paint_code_bg(lexer, view)
            except RuntimeError:
                pass  # вью могло быть уничтожено вместе с редактором

    def _apply_code_font(self, lexer):
        """Шрифт блока кода — тот же, что и у текста попапа."""
        font = self._popup_font()
        for sid in range(128):
            if lexer.description(sid):
                lexer.setFont(font, sid)

    def _paint_code_bg(self, lexer, view):
        """Единый фон блока кода (чуть контрастнее фона попапа)."""
        bg = self._tc.shade(105)
        view.setPaper(bg)
        lexer.setDefaultPaper(bg)
        for sid in range(128):
            if lexer.description(sid):
                lexer.setPaper(bg, sid)

    def paintEvent(self, event):
        # Фон/рамку/бегунок рисует _PopupCard (self._card): окно прозрачно,
        # чтобы тень вокруг карточки не обрезалась и не закрашивалась.
        pass

    def _apply_hit_region(self):
        """Делает зону за пределами карточки «прозрачной» для мыши.

        Перехватываем WM_NCHITTEST (Win32) и возвращаем HTNOWHERE для всех
        точек вне карточки: Windows маршрутизирует мышь в этой области окну
        снизу (редактору), поэтому слово под тенью наводится нормально.

        SetWindowRgn/QWidget.setMask тут не подходят: на этом Qt регион
        обрезает и отрисовку — тень под карточкой пропадает. Субкласс
        WndProc не трогает отрисовку, он влияет только на hit-test."""
        try:
            import ctypes
            from ctypes import wintypes
        except Exception:
            return
        try:
            hwnd = int(self.winId())
        except Exception:
            return
        try:
            if not hwnd:
                return
            if getattr(self, "_hit_cb", None) is not None:
                if self._hit_hwnd == hwnd:
                    return
            user32 = ctypes.windll.user32
            comctl32 = ctypes.windll.comctl32

            class POINT(ctypes.Structure):
                _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

            user32.ScreenToClient.argtypes = [
                wintypes.HWND, ctypes.POINTER(POINT)]
            user32.ScreenToClient.restype = wintypes.BOOL
            DEFSUBCLASSPROC = ctypes.WINFUNCTYPE(
                ctypes.c_ssize_t, wintypes.HWND, wintypes.UINT,
                ctypes.c_ssize_t, ctypes.c_ssize_t,
                ctypes.c_ssize_t, ctypes.c_ssize_t)

            comctl32.SetWindowSubclass.argtypes = [
                ctypes.c_void_p, ctypes.c_void_p,
                ctypes.c_ssize_t, ctypes.c_ssize_t]
            comctl32.SetWindowSubclass.restype = wintypes.BOOL
            comctl32.DefSubclassProc.argtypes = [
                ctypes.c_void_p, wintypes.UINT,
                ctypes.c_ssize_t, ctypes.c_ssize_t]
            comctl32.DefSubclassProc.restype = ctypes.c_ssize_t

            def subclass(h, msg, wp, lp, u_id, ref_data):
                try:
                    if msg == 0x0084:  # WM_NCHITTEST
                        popup = self
                        pt = POINT()
                        pt.x = lp & 0xFFFF
                        pt.y = (lp >> 16) & 0xFFFF
                        if pt.x & 0x8000:
                            pt.x -= 0x10000
                        if pt.y & 0x8000:
                            pt.y -= 0x10000
                        if user32.ScreenToClient(
                                h, ctypes.pointer(pt)):
                            dpr = popup.devicePixelRatioF() or 1.0
                            m = round(popup._shadow_margin * dpr) if hasattr(
                                popup, "_shadow_margin") else 0
                            cw = round(popup.width() * dpr)
                            ch = round(popup.height() * dpr)
                            inside = (m <= pt.x < cw - m
                                      and m <= pt.y < ch - m)
                            if not inside:
                                # HTTRANSPARENT — Windows повторяет hit-тест с
                                # окна ниже в z-order (редактора): мышь в поле
                                # тени/запаса уходит под плашку. HTNOWHERE (0)
                                # тут не годится: события просто глохнут.
                                return -1  # HTTRANSPARENT
                except Exception:
                    pass  # ctypes/платформенный сбой в callback — не роняем попап
                return comctl32.DefSubclassProc(h, msg, wp, lp)

            cb = DEFSUBCLASSPROC(subclass)
            self._hit_cb = cb
            self._hit_hwnd = hwnd
            comctl32.SetWindowSubclass(
                ctypes.c_void_p(hwnd), cb, 1, 0)
        except Exception:
            pass  # не-Windows или сбой платформы — без hit-региона обходимся

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._apply_hit_region()

    def _draw_thumb(self, p: QPainter, card: QFrame):
        """Тонкий бегунок справа: рисуется, когда есть что прокручивать.

        Фон плашки и бегунок рисует сама карточка поверх прозрачной
        скролл-области, а узкая полоса нативного скроллбара спрятана —
        так бегунок гарантированно виден с отступом от правого края."""
        content_h = self._wrap.height()
        vp = self._scroll.viewport()
        slot_h = vp.height() if vp is not None else card.height()
        if content_h <= slot_h or content_h <= 0 or slot_h <= 0:
            return
        sb = self._scroll.verticalScrollBar()
        # Трек с отступами сверху/снизу: бегунок не касается краёв плашки
        top = _SB_PAD_V
        track_h = slot_h - 2 * top
        if track_h <= 0:
            return
        thumb_h = max(24, int(track_h * slot_h / content_h))
        thumb_h = min(thumb_h, track_h)
        max_y = track_h - thumb_h
        y = top + (0 if sb.maximum() <= 0 else int(
            sb.value() * max_y / sb.maximum()))
        x = card.width() - _SB_THUMB_W - _SB_PAD_RIGHT
        fg = QColor(self._tc.fg_color) if self._tc is not None \
            else QColor("#cdd6f4")
        fg.setAlpha(110)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(fg)
        p.drawRoundedRect(
            QRectF(x, y, _SB_THUMB_W, thumb_h), 2, 2)

    def _style_color(self, key: str, fallback: str) -> str:
        st = self._styles.get(key) or {}
        return st.get("color") or fallback

    def _fg(self) -> str:
        return self._tc.fg if self._tc else "#cdd6f4"

    def _muted(self) -> str:
        if self._tc:
            return self._tc.muted
        return "#6c7086"

    def _keyword_color(self) -> str:
        return self._style_color("keyword", "#c678dd")

    def _type_color(self) -> str:
        return self._style_color("type", "#61afef")

    def _func_color(self) -> str:
        return self._style_color("function_name", "#56b6c2")

    def _namespace_color(self) -> str:
        return self._style_color("namespace", self._keyword_color())

    def _comment_color(self) -> str:
        return self._style_color("comment", self._muted())

    def _variable_color(self) -> str:
        return self._style_color("variable", self._fg())

    def _number_color(self) -> str:
        return self._style_color("number", "#d19a66")

    def _string_color(self) -> str:
        return self._style_color("string", "#98c379")

    def _class_color(self) -> str:
        return self._style_color("class_name", self._type_color())

    def _self_color(self) -> str:
        """Цвет self/cls в python-сигнатурах (есть во всех темах)."""
        return self._style_color("self", self._func_color())

    def _literal_color(self) -> str:
        """Цвет литералов Lumen (true/false/null/self/this)."""
        return self._style_color("literal", "#94e2d5")

    def _modifier_color(self) -> str:
        """Цвет модификаторов Lumen (const/static/global/...)."""
        return self._style_color("modifier", self._keyword_color())

    def _directive_color(self) -> str:
        """Цвет директив препроцессора (#define/#include/...)."""
        return self._style_color("directive", "#f392a8")

    def _code_bg_name(self) -> str:
        """Подложка инлайн-кода — чуть контрастнее фона попапа."""
        if self._tc is None:
            return "#313244"
        return self._tc.shade(105).name()

    def _popup_font(self) -> QFont:
        size = self.editor.font().pointSize() or CODE_FONT_SIZE
        return QFont(code_font_family(), max(min(size, _POPUP_MAX_SIZE), 9))

    # ------------------------------------------------------------------
    # контент
    # ------------------------------------------------------------------

    def set_content(self, text: str):
        """Разбирает текст hover на заголовки, списки, код и параграфы."""
        # Плашка видима и текст меняется — запоминаем текущий кадр для
        # плавного перетекания в новый контент (см. _start_wipe).
        if self.isVisible() and text != self._last_content:
            self._capture_wipe()
        self._last_content = text
        while self._layout.count():
            item = self._layout.takeAt(0)
            w = item.widget()
            if w is None:
                continue
            if isinstance(w, QsciScintilla):
                w.hide()  # кодовые вью переиспользуются между показами
            else:
                w.setParent(None)
                w.deleteLater()

        for kind, *payload in self._parse(text):
            if kind == "code":
                self._add_code_block(payload[0], payload[1])
            else:
                self._add_text_block(payload[0])

        # Точные размеры вычисляет show_at: здесь только сбрасываем геометрию,
        # чтобы контент предыдущего показа не влиял на размер
        self._wrap.adjustSize()

    @staticmethod
    def _parse(text: str) -> List[Tuple[str, object]]:
        """Разбивает текст на ("text", блок), ("code", код, язык)."""
        items: List[Tuple[str, object]] = []
        buf: List[str] = []
        code: Optional[List[str]] = None
        lang = ""
        for raw in text.split("\n"):
            stripped = raw.strip()
            if stripped.startswith("```"):
                if code is not None:
                    items.append(("code", "\n".join(code), lang))
                    code = None
                else:
                    if buf:
                        items.append(("text", "\n".join(buf)))
                        buf = []
                    lang = stripped[3:].strip()
                    code = []
            elif code is not None:
                code.append(raw)
            else:
                buf.append(raw)
        if code is not None:
            items.append(("code", "\n".join(code), lang))
        elif buf:
            items.append(("text", "\n".join(buf)))
        return items

    def _add_text_block(self, text: str):
        """Разбирает text-блок на заголовки, списки, разделители, параграфы."""
        first_para = True
        for kind, *payload in self._split_elements(text):
            if kind == "heading":
                self._layout.addWidget(self._make_heading(*payload))
            elif kind == "rule":
                self._layout.addWidget(self._make_rule())
            elif kind == "list":
                self._layout.addWidget(self._make_list(payload[0]))
            else:
                self._layout.addWidget(self._make_label(payload[0], first_para))
                first_para = False

    def _split_elements(self, text: str) -> List[Tuple[str, object]]:
        """Строки text-блока -> элементы: heading/rule/list/para.

        Идущие подряд строки склеиваются в параграф или список; пустая
        строка закрывает текущий элемент, #/--- начинают свои.
        """
        elements: List[Tuple[str, object]] = []
        buf: List[str] = []
        items: List[str] = []
        for raw in text.split("\n"):
            stripped = raw.strip()
            if not stripped:
                if buf:
                    elements.append(("para", "\n".join(buf)))
                    buf = []
                elif items:
                    elements.append(("list", items))
                    items = []
                continue
            m = _HEADING_RE.match(stripped)
            if m:
                if buf:
                    elements.append(("para", "\n".join(buf)))
                    buf = []
                if items:
                    elements.append(("list", items))
                    items = []
                elements.append(
                    ("heading", min(len(m.group(1)), 3), m.group(2).strip())
                )
                continue
            if _RULE_RE.match(stripped):
                if buf:
                    elements.append(("para", "\n".join(buf)))
                    buf = []
                if items:
                    elements.append(("list", items))
                    items = []
                elements.append(("rule",))
                continue
            b = _BULLET_RE.match(raw)
            n = _NUMBERED_RE.match(raw)
            if b or n:
                if buf:
                    elements.append(("para", "\n".join(buf)))
                    buf = []
                items.append((b or n).group(3))
                continue
            buf.append(raw)
        if items:
            elements.append(("list", items))
        elif buf:
            elements.append(("para", "\n".join(buf)))
        return elements

    # --- заголовки, разделители, списки ---

    def _make_label_base(self) -> QLabel:
        label = QLabel(self)
        label.setFont(self._popup_font())
        label.setWordWrap(True)
        label.setMaximumWidth(_MAX_WIDTH - 2 * _PAD_X)
        # Если layout растянул виджет выше контента (перенос строки),
        # текст прижат к верху — иначе пустая полоса сверху из-за
        # вертикального центрирования по умолчанию
        label.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop)
        # Высота строго по тексту: layout не растягивает параграф,
        # попап не получает пустых полос сверху/снизу
        label.setSizePolicy(
            QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed
        )
        label.setTextFormat(Qt.TextFormat.RichText)
        label.setStyleSheet(
            f"background: transparent; color: {self._fg()};"
        )
        return label

    def _make_heading(self, level: int, text: str) -> QLabel:
        """Заголовок markdown: цвет + полоса-маркер слева (как в GitHub)."""
        label = self._make_label_base()
        color = {
            1: self._keyword_color(), 2: self._type_color(),
            3: self._func_color(),
        }.get(level, self._fg())
        body = self._inline_format(html.escape(text))
        label.setText(
            f'<span style="border-left: 3px solid {color}; color: {color};'
            f' font-weight: bold;">&nbsp;&nbsp;{body}</span>'
        )
        return label

    def _make_rule(self) -> QFrame:
        """Горизонтальный разделитель в цвете границ темы."""
        line = QFrame(self)
        line.setFrameShape(QFrame.Shape.NoFrame)
        line.setFixedHeight(1)
        border = self._tc.border if self._tc else "#45475a"
        line.setStyleSheet(f"border-top: 0.5px solid {border};")
        return line

    def _make_list(self, items: List[str]) -> QLabel:
        """Список: «- имя: описание» — имя цветом переменной, двоеточие muted."""
        label = self._make_label_base()
        rows = []
        for item in items:
            item = item.strip()
            marker = f'<span style="color:{self._muted()};">&nbsp;&nbsp;•&nbsp;</span>'
            if ":" in item:
                name, _, desc = item.partition(":")
                rows.append(
                    marker
                    + f'<span style="color:{self._variable_color()};">{html.escape(name)}</span>'
                    + f'<span style="color:{self._muted()};">:</span>'
                    + f' <span style="color:{self._fg()};">'
                    + f'{self._inline_format(html.escape(desc.strip()))}</span>'
                )
            else:
                rows.append(
                    marker
                    + f'<span style="color:{self._fg()};">'
                    + f'{self._inline_format(html.escape(item))}</span>'
                )
        label.setText("<br>".join(rows))
        return label

    # --- параграфы ---

    def _make_label(self, para: str, first: bool = False) -> QLabel:
        label = self._make_label_base()
        label.setText(self._render_paragraph(para, first))
        return label

    def _render_paragraph(self, para: str, first: bool = False) -> str:
        lines = para.split("\n")
        single = len(lines) == 1
        out = []
        i = 0
        while i < len(lines):
            raw = lines[i]
            line = raw.strip()
            if not line:
                i += 1
                continue
            indented = raw[:1] in (" ", "\t") and raw != line
            if first and i == 0 and self._is_kind_line(line):
                # многострочная сигнатура pyright: «(function) def print(»
                # + строки параметров — заголовок и раскраска по токенам
                end = self._sig_block_end(lines)
                if end > 1:
                    out.append(self._render_kind_block(lines[:end]))
                    i = end
                    continue
                # заголовок вида «function sum» или «(function) foo(...)»
                line = self._render_header(line)
            elif indented:
                # описание параметра (с отступом) — приглушённым
                line = self._render_param_desc(line)
            elif i + 1 < len(lines) and lines[i + 1][:1] in (" ", "\t"):
                # одиночная строка перед описанием — имя параметра
                line = self._render_param_name(line)
            elif self._is_lumen_doc() and self._match_lumen_decl(line):
                # Lumen-заголовок hover (переменная/функция/namespace/тип) —
                # LSP шлёт строкой без фенса: раскраска Lumen-токенизатором.
                # Каждая сигнатура перегрузки в многострочном параграфе
                # (`**fn a(x) ...**\n**fn a(y) ...**`) красится одинаково,
                # а не только первая строка.
                line = self._lumen_decl_html(line)
            elif single and "(" in line and self._looks_like_sig(line):
                # однострочная сигнатура без фенса — раскраска по токенам
                line = self._sig_html(line)
            else:
                line = self._render_line(line)
            out.append(line)
            i += 1
        return "<br>".join(out)

    def _sig_block_end(self, lines: List[str]) -> int:
        """Длина многострочной сигнатуры pyright, начиная с kind-заголовка.

        Продолжения — строки параметров с отступом и закрывающая «)»;
        первая же строка без отступа (описание, новый абзац) обрывает.
        """
        j = 1
        while j < len(lines):
            s = lines[j].strip()
            if not s:
                break
            if lines[j][:1] in (" ", "\t") or s.startswith(")"):
                j += 1
            else:
                break
        return j

    def _is_kind_line(self, line: str) -> bool:
        """Строка — kind-заголовок: «function sum» или «(function) …»."""
        if _KIND_HEADER_RE.match(line.strip()):
            return True
        word = line.strip().split(None, 1)[0] if line.strip() else ""
        return word in _KIND_WORDS

    def _render_param_name(self, line: str) -> str:
        """Имя параметра перед его описанием — цветом переменной."""
        return (
            f'<span style="color:{self._variable_color()};font-weight:bold;">'
            f"{html.escape(line)}</span>"
        )

    def _render_param_desc(self, line: str) -> str:
        """Описание параметра (строка с отступом) — приглушённым, с отступом."""
        return (
            f'&nbsp;&nbsp;<span style="color:{self._muted()};">'
            f"{self._inline_format(html.escape(line))}</span>"
        )

    def _render_header(self, line: str) -> str:
        """Заголовок hover: «function sum» или pyright «(function) sum(...)»."""
        kind_html = self._render_kind_header(line)
        if kind_html is not None:
            return kind_html
        line = line.strip()
        word, _, rest = line.partition(" ")
        if word not in _KIND_WORDS:
            return f'<span style="color:{self._muted()};">{html.escape(line)}</span>'
        group = _KIND_GROUP.get(word, "kw")
        color = {
            "kw": self._keyword_color(),
            "ns": self._namespace_color(),
            "cls": self._class_color(),
            "func": self._func_color(),
            "var": self._variable_color(),
        }[group]
        parts = [
            f'<span style="color:{color};font-weight:bold;">{html.escape(word)}</span>'
        ]
        if rest:
            parts.append(
                f' <span style="color:{color};">'
                f'{self._inline_format(html.escape(rest))}</span>'
            )
        return "".join(parts)

    def _render_kind_header(self, line: str) -> Optional[str]:
        """Заголовок pyright «(function) foo(a: int) -> bool» -> цветной HTML.

        Кинд в скобках — keyword-цветом, имя — цветом группы (функция/класс/
        модуль), сигнатура — раскраской по токенам. «(function) def print(» —
        def keyword-цветом. None — не kind-заголовок.
        """
        m = _KIND_HEADER_RE.match(line.strip())
        if not m:
            return None
        kind = m.group(1).strip()
        group = _KIND_GROUP.get(kind)
        if group is None:
            return None
        color = {
            "kw": self._keyword_color(),
            "ns": self._namespace_color(),
            "cls": self._class_color(),
            "func": self._func_color(),
            "var": self._variable_color(),
        }[group]
        parts = [
            f'<span style="color:{self._keyword_color()};">({html.escape(kind)})</span>'
        ]
        rest = m.group(2).strip()
        if rest:
            name, sep, sig = rest.partition("(")
            name = name.strip()
            if name:
                if not sep and ":" in name and "::" not in name:
                    # «(variable) x: int» — имя переменной и тип по токенам
                    vname, _, vtype = name.partition(":")
                    parts.append(" " + self._kind_name_html(vname.strip(), color))
                    parts.append(
                        f'<span style="color:{self._muted()};">:</span>'
                        + self._sig_html(vtype.strip())
                    )
                else:
                    parts.append(" " + self._kind_name_html(name, color))
            if sep:
                parts.append(
                    f'<span style="color:{self._muted()};">(</span>'
                    + self._sig_html(sig)
                )
        return "".join(parts)

    def _kind_name_html(self, name: str, color: str) -> str:
        """Имя в kind-заголовке: «def print» — def keyword-цветом, print — цветом группы."""
        words = name.split()
        if words and words[0] in ("def", "class", "struct", "enum",
                                  "union", "namespace"):
            out = [
                f'<span style="color:{self._keyword_color()};">{html.escape(words[0])}</span>'
            ]
            if len(words) > 1:
                out.append(
                    f' <span style="color:{color};font-weight:bold;">'
                    f'{html.escape(" ".join(words[1:]))}</span>'
                )
            return "".join(out)
        return (
            f'<span style="color:{color};font-weight:bold;">'
            f"{html.escape(name)}</span>"
        )

    def _looks_like_sig(self, line: str) -> bool:
        """Строка похожа на сигнатуру кода (тип/модификатор/:: в начале).

        Отличает код от обычного описания: «Adds two numbers (a, b)» —
        текст, а «int sum(int a, int b)» или python «foo(a: int) -> bool» —
        сигнатура. Кириллица и прочая не-ASCII проза (русские док-строки)
        сигнатурой не считаются: скобки в них не красятся.
        """
        head = line.lstrip()
        if not head or head.startswith(("//", "Type:", "Returns:", "→")):
            return False
        first = head.split(None, 1)[0]
        word = first.rstrip(":*&")
        if not word.isascii():
            return False  # не-ASCII (кириллица) — проза, не код
        if (word in _BASE_TYPES or word in _KEYWORDS
                or "::" in head):
            return True
        # Тип с заглавной («MyStruct make(int)»): сигнатурой считаем только если
        # открывающая скобка «прикреплена» к идентификатору без пробела.
        # Иначе «Adds two numbers (a, b)» — проза.
        if head[0].isupper():
            for i, ch in enumerate(head):
                if ch == "(" and i > 0:
                    prev = head[i - 1]
                    if prev.isalnum() or prev == "_":
                        return True
            return False
        # python-сигнатура без модификаторов: `foo(a: int) -> bool`
        return "(" in head and (":" in head or "->" in head)

    def _render_line(self, line: str) -> str:
        if line.lstrip().startswith("//"):
            return (
                f'<span style="color:{self._comment_color()};">'
                f"{html.escape(line)}</span>"
            )
        m = _SECTION_RE.match(line.strip())
        if m:
            # «Parameters:», «Returns: int» — акцентная подпись секции
            label = m.group(1)
            rest = m.group(2).strip()
            head = (
                f'<span style="color:{self._func_color()};font-weight:bold;">'
                f"{html.escape(label)}</span>"
            )
            if not rest:
                return head + ":"
            if rest in _BASE_TYPES or rest[0].isupper():
                value = f'<span style="color:{self._type_color()};">{html.escape(rest)}</span>'
            else:
                value = self._inline_format(html.escape(rest))
            return head + f": {value}"
        # «→ Тип» и «Type: X» — подпись приглушённая, значение как код
        for marker, label_style in (("→", "arrow"), ("Type:", "type-label")):
            if marker in line:
                left, _, right = line.partition(marker)
                parts = [html.escape(left) or ""]
                if right.strip():
                    sep = "→" if marker == "→" else "Type:"
                    parts.append(
                        f' <span style="color:{self._muted()};">{sep}</span> '
                        f"<span style=\"color:{self._type_color()};\">"
                        f"{html.escape(right.strip())}</span>"
                    )
                return "".join(parts)
        if line.endswith(":"):
            return (
                f'<span style="color:{self._func_color()};font-weight:bold;">'
                f"{html.escape(line)}</span>"
            )
        return self._inline_format(html.escape(line))

    def _inline_format(self, escaped: str) -> str:
        """Инлайн-разметка: `код` — бейдж с подложкой, **жирный**, *курсив*.

        Код и ссылки прячем в плейсхолдеры до обработки **/*: иначе разметка
        внутри них (`` `a **b**` ``) красится как обычный жирный текст.
        """
        code_span = (
            f'<span style="color:{self._fg()};font-family:{code_font_family()};'
            f'background-color:{self._code_bg_name()};">&nbsp;{SENTINEL}&nbsp;</span>'
        )
        link_span = (
            f'<span style="color:{self._func_color()};'
            f'text-decoration: underline;">{SENTINEL}</span>'
        )
        hidden: List[str] = []

        def _stash(fragment: str) -> str:
            hidden.append(fragment)
            return f"\x00{len(hidden) - 1}\x00"

        escaped = _INLINE_CODE_RE.sub(
            lambda m: _stash(code_span.replace(SENTINEL, m.group(1))), escaped)
        escaped = _LINK_RE.sub(
            lambda m: _stash(link_span.replace(SENTINEL, m.group(1))), escaped)
        escaped = _BOLD_RE.sub(r"<b>\1</b>", escaped)
        escaped = _ITALIC_RE.sub(r"<i>\1</i>", escaped)
        # Восстановление многошаговое: вложенные ссылки/код создают токены
        # внутри уже спрятанных фрагментов (например, `код` внутри [ссылки]),
        # поэтому один проход их не раскрывает
        while True:
            next_esc = _SENTINEL_RE.sub(
                lambda m: hidden[int(m.group(1))], escaped)
            if next_esc == escaped:
                break
            escaped = next_esc
        return escaped

    # --- раскраска сигнатур по токенам ---

    def _sig_html(self, text: str) -> str:
        """C/Python-подобная строка (сигнатура/фрагмент кода) -> цветной HTML."""
        colors = {
            "kw": self._keyword_color(),
            "type": self._type_color(),
            "func": self._func_color(),
            "var": self._variable_color(),
            "self": self._self_color(),
            "ns": self._namespace_color(),
            "num": self._number_color(),
            "str": self._string_color(),
            "com": self._comment_color(),
            "punct": self._muted(),
        }
        out = []
        for style, tok in self._sig_spans(text):
            if style == "fg":
                out.append(html.escape(tok))
            else:
                out.append(
                    f'<span style="color:{colors[style]};">{html.escape(tok)}</span>'
                )
        return "".join(out)

    def _sig_spans(self, text: str) -> List[Tuple[str, str]]:
        """Токены C/Python-строки -> [(стиль, текст)] для цветного рендера.

        Эвристика без внешних зависимостей: базовые типы и `_t`-суффиксы —
        типы, имя перед `(` — функция, «тип перед именем» — тип, после `::` —
        тип (до `::` — namespace), внутри `<>` — типы. Python: `a: int` —
        имя параметра до `:`, тип после; `self/cls` — отдельным цветом;
        `-> T` (trailing return, перед стрелкой `)`) — тип; C: `p->field`
        (перед стрелкой имя) — поле. `std::pair<int, int> v` даёт
        ns/type/type/var, `Callable[[int], str]` — типы в скобках.
        Пунктуация (скобки, `:`, `=`, `|`, `*`, `->`) — приглушённым цветом.
        """
        tokens = _SIG_TOKEN_RE.findall(text)
        n = len(tokens)
        if not n:
            return []
        styles = ["fg"] * n
        # числа/строки/комментарии — по виду токена, пунктуация — muted
        for i, tok in enumerate(tokens):
            if tok.startswith("//"):
                styles[i] = "com"
            elif tok[:1] in "\"'":
                styles[i] = "str"
            elif (tok[0].isdigit()
                  or (tok[0] == "." and len(tok) > 1 and tok[1].isdigit())):
                styles[i] = "num"
            elif not (tok[0].isalpha() or tok[0] == "_"):
                # скобки, `:`, `=`, `->`, `|`, `*` — приглушённым
                styles[i] = "punct"
        # глубина шаблонных скобок <> на каждом токене
        depths = [0] * n
        d = 0
        for i, tok in enumerate(tokens):
            depths[i] = d
            if tok in ("<", "<<"):
                d += 1
            elif tok in (">", ">>"):
                d = max(d - 1, 0)
        # «->» после «)» — trailing return (тип), иначе C-поле p->field
        arrow_type = set()
        for i, tok in enumerate(tokens):
            if tok != "->":
                continue
            j = i - 1
            while j >= 0 and not tokens[j].strip():
                j -= 1
            if j >= 0 and tokens[j] == ")":
                arrow_type.add(i)
        for i, tok in enumerate(tokens):
            if styles[i] != "fg" or not (tok[0].isalpha() or tok[0] == "_"):
                continue
            nxt = next((tokens[j] for j in range(i + 1, n)
                        if tokens[j].strip()), None)
            prv = next((tokens[j] for j in range(i - 1, -1, -1)
                        if tokens[j].strip()), None)
            if tok in ("self", "cls"):
                styles[i] = "self"
            elif tok in _KEYWORDS:
                styles[i] = "kw"
            elif tok in _BASE_TYPES or tok.endswith("_t"):
                styles[i] = "type"
            elif nxt == "(":
                styles[i] = "func"
            elif prv == "::":
                styles[i] = "type"
            elif prv == ".":
                styles[i] = "var"
            elif prv == "->":
                # trailing return: `f() -> int` (перед стрелкой «)»)
                j = i - 1
                while j >= 0 and not tokens[j].strip():
                    j -= 1
                styles[i] = "type" if j in arrow_type else "var"
            elif nxt == ":":
                styles[i] = "var"
            elif nxt == "::":
                styles[i] = "ns"
            elif prv == ":":
                # тип аннотации: после `:` — тип, даже если дальше `[`
                # (Literal[False], SupportsWrite[str])
                styles[i] = "type"
            elif depths[i] == 0 and nxt in (",", ")", "=", ";", "[", None):
                styles[i] = "var"
            elif tok[0].isupper():
                styles[i] = "type"
            elif prv == "<":
                styles[i] = "type"
            elif prv in ("(", ",", "{", ";", "[") or prv is None:
                styles[i] = "type"
            elif prv in _KEYWORDS:
                styles[i] = "type"
            elif nxt in ("*", "&", "[", "("):
                styles[i] = "type"
            else:
                styles[i] = "var"
        return [(s, t) for s, t in zip(styles, tokens)]

    # --- блок кода ---

    def _lexer_for_lang(self, lang: str):
        """Класс лексера для языка код-фенса (пустой — язык редактора)."""
        key = lang.strip().lower()
        mapping = {
            "cpp": QsciLexerCPP, "c": QsciLexerCPP, "c++": QsciLexerCPP,
            "cc": QsciLexerCPP, "cxx": QsciLexerCPP, "h": QsciLexerCPP,
            "hpp": QsciLexerCPP, "hxx": QsciLexerCPP,
            "python": QsciLexerPython, "py": QsciLexerPython,
            "java": QsciLexerJava,
            "javascript": QsciLexerJavaScript, "js": QsciLexerJavaScript,
            "xml": QsciLexerXML, "html": QsciLexerXML,
            "sql": QsciLexerSQL,
            "bash": QsciLexerBash, "sh": QsciLexerBash,
            "shell": QsciLexerBash,
            "lua": QsciLexerLua,
            "ruby": QsciLexerRuby, "rb": QsciLexerRuby,
        }
        if not key:
            lid = getattr(self.editor, "language_id", "")
            return QsciLexerPython if lid == "python" else QsciLexerCPP
        return mapping.get(key, QsciLexerCPP)

    def _ensure_code_view(self, lang: str) -> QsciScintilla:
        """Вью для кода на языке фенса; переиспользуются между показами."""
        lexer_cls = self._lexer_for_lang(lang)
        view = self._code_views.get(lexer_cls)
        if view is not None and not sip.isdeleted(view):
            return view
        view = QsciScintilla(self)
        view.setUtf8(True)
        view.setReadOnly(True)
        view.setFrameShape(QFrame.Shape.NoFrame)
        view.setHorizontalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff
        )
        view.setVerticalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff
        )
        view.setCaretWidth(0)
        view.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        view.setContextMenuPolicy(Qt.ContextMenuPolicy.NoContextMenu)
        for i in range(5):
            view.setMarginWidth(i, 0)
        lexer = lexer_cls(view)
        lexer.setProperty("fold", "1")
        view.setLexer(lexer)
        self._code_views[lexer_cls] = view
        self._code_lexers[lexer_cls] = lexer
        if self._theme_name:
            self.editor.theme_manager.apply_theme(
                view, self._theme_name, lexer
            )
            self._apply_code_font(lexer)
            self._paint_code_bg(lexer, view)
        return view

    def _looks_like_kind_block(self, lines: List[str]) -> bool:
        """Блок кода начинается с kind-заголовка «(function) …»."""
        m = _KIND_HEADER_RE.match(lines[0].strip())
        return bool(m and m.group(1).strip() in _KIND_GROUP)

    def _render_kind_block(self, lines: List[str]) -> str:
        """pyright-блок: «(function) def print( ... ) -> None» — заголовок + сигнатура.

        Первая строка — kind-заголовок, остальные — строки сигнатуры:
        отступ сохраняем, текст раскрашиваем по токенам.
        """
        # kind может быть не из _KIND_GROUP (clangd: «(static member)» и т.п.) —
        # _render_kind_header вернёт None, тогда рисуем обычным заголовком
        parts = [self._render_kind_header(lines[0]) or self._render_header(lines[0])]
        for raw in lines[1:]:
            if not raw.strip():
                continue
            parts.append(self._indent_sig_line(raw))
        return "<br>".join(parts)

    def _indent_sig_line(self, raw: str) -> str:
        """Строка сигнатуры: ведущий отступ — &nbsp;, остальное — по токенам."""
        stripped = raw.lstrip()
        pad = len(raw) - len(stripped)
        return "&nbsp;" * min(pad, 16) + self._sig_html(stripped)

    @staticmethod
    def _is_lumen_lang(lang: str, language_id: str = "") -> bool:
        """Фенс — Lumen-код: явный `````lumen`/`lm` или язык редактируемого файла."""
        key = lang.strip().lower()
        if key in ("lumen", "lm"):
            return True
        if key:
            return False
        return language_id == "lumenlang"

    def _is_lumen_doc(self) -> bool:
        """Редактируемый документ — Lumen (.lm): hover на переменные/функции
        приходит строками без код-фенса, красим их Lumen-подсветкой."""
        return getattr(self.editor, "language_id", "") == "lumenlang"

    def _match_lumen_decl(self, line: str) -> bool:
        """Однострочный Lumen-«заголовок» hover: `let/const let/strict let`,
        `fn`, `namespace`, `type`, `struct`, `enum`, `generic param` — с
        markdown-жирностью `**...**` или без.

        Переменные/функции/namespace/типы LSP шлёт именно такими строками
        (напр. `**let a: *(i64)**`), а не код-фенсом.
        """
        content = line.strip()
        if content.startswith("**") and content.endswith("**") \
                and len(content) > 4:
            content = content[2:-2]
        if not content:
            return False
        head = content.split(None, 1)[0]
        # заголовок hover для #define / #macro (директивы, а не ключевое слово)
        if head == "#define" or head == "#macro":
            return True
        if not (content[0].isalpha() or content[0] == "_"):
            return False
        words = content.split(None, 1)
        if not words:
            return False
        head = words[0]
        if head in ("let", "fn", "namespace", "struct", "enum", "type", "generic"):
            return True
        # генерик-заголовок: `struct[T] Name`, `struct[T: Type] Name` —
        # имя стоит после блока `[...]` (в `fn name[T]` имя идёт раньше)
        if _is_generic_type_decl(content):
            return True
        # одиночный модификатор (титул доки `**strict**`, `**const**`) и
        # связка `strict let …` — цвет модификатора
        if words[0] in _LUMEN_MODIFIERS:
            return True
        # одиночное ключевое слово/спецоператор/тип/литерал — титул доки
        # (`**new**`, `**typeof**`, `**echo**`, `**auto**`, `**in**`, ...):
        # красим Lumen-подсветкой, а не обычным жирным текстом
        if len(content.split()) == 1 and words[0] in (
                _LUMEN_KEYWORDS | _LUMEN_TYPES
                | _LUMEN_LITERALS):
            return True
        return False

    def _lumen_decl_html(self, line: str) -> str:
        """Lumen-заголовок hover -> HTML: снимаем `**…**`, раскрашиваем
        токены (`let` — keyword, тип — type и т.д.), жирность сохраняем."""
        content = line.strip()
        bold = False
        if content.startswith("**") and content.endswith("**") \
                and len(content) > 4:
            content = content[2:-2]
            bold = True
        if content == "type" or content.startswith("type "):
            # `type Foo` — слово type не ключ языка; красим его keyword-цветом,
            # имя типа раскрашивает токенизатор
            head, _, rest = content.partition(" ")
            body = (
                f'<span style="color:{self._keyword_color()};">'
                f"{html.escape(head)}</span>"
                + ((" " + self._lumen_sig_html(rest)) if rest else "")
            )
        elif content == "generic param" or content.startswith("generic param "):
            # `generic param A: Type` — дженерик-параметр: `generic` keyword-цветом,
            # имя параметра цветом класса, `: Type` раскрашивает токенизатор
            _, _, header = content.partition("generic param ")
            name, sep, ty = header.partition(":")
            name = name.strip()
            kw = (f'<span style="color:{self._keyword_color()};">'
                  f"generic</span>")
            name_span = (
                f'<span style="color:{self._class_color()};">'
                f"{html.escape(name)}</span>"
            )
            body = kw + " param " + name_span
            if sep:
                ty_rest = ":" + ty if ty else ":"
                body += " " + self._lumen_sig_html(ty_rest)
        else:
            body = self._lumen_sig_html(content)
        return f"<b>{body}</b>" if bold else body

    def _lumen_spans(self, text: str) -> List[Tuple[str, str]]:
        """Токены Lumen-кода -> [(стиль, текст)] для цветного рендера.

        Обёртка над module-level lumen_token_spans (переиспользуется
        списком символов во вкладке Outline).
        """
        return lumen_token_spans(text)

    def _lumen_colors(self) -> Dict[str, str]:
        """Цвета стилей Lumen-токенов (та же тема редактора)."""
        return {
            "kw": self._keyword_color(),
            "mod": self._modifier_color(),
            "type": self._type_color(),
            "ns": self._class_color(),
            "lit": self._literal_color(),
            "dir": self._directive_color(),
            "obj": self._variable_color(),
            "var": self._variable_color(),
            "func": self._func_color(),
            "num": self._number_color(),
            "str": self._string_color(),
            "com": self._comment_color(),
            "punct": self._muted(),
        }

    def _lumen_sig_html(self, text: str) -> str:
        """Однострочный Lumen-текст (строка hover) -> цветной HTML.

        Применяется к «заголовкам» переменных/функций/namespace/types, которые
        LSP присылает без код-фенса (напр. `let a: *(i64)`), чтобы они
        красились той же regex-подсветкой, что и примеры кода в фенсах.
        """
        colors = self._lumen_colors()
        out = []
        for style, tok in self._lumen_spans(text):
            esc = html.escape(tok).replace(" ", "&nbsp;")
            if style == "fg":
                out.append(esc)
            else:
                out.append(
                    f'<span style="color:{colors[style]};">{esc}</span>'
                )
        return "".join(out)

    def _lumen_code_html(self, lines: List[str]) -> str:
        """Lumen-код -> HTML: токены цветные, отступы сохранены (&nbsp;)."""
        colors = self._lumen_colors()
        rows = []
        for raw in lines:
            stripped = raw.lstrip(" \t")
            lead = len(raw) - len(stripped)
            pad = sum((4 if c == "\t" else 1) for c in raw[:lead])
            parts = ["&nbsp;" * min(pad, 64)]
            for style, tok in self._lumen_spans(stripped):
                esc = html.escape(tok).replace(" ", "&nbsp;")
                if style == "fg":
                    parts.append(esc)
                else:
                    parts.append(
                        f'<span style="color:{colors[style]};">{esc}</span>'
                    )
            rows.append("".join(parts) or "&nbsp;")
        return "<br>".join(rows)

    def _add_lumen_code_block(self, lines: List[str]):
        """Фенс ```lumen — QLabel с моноширинным шрифтом и regex-подсветкой.

        В отличие от Qsci-вью (fallback QsciLexerCPP) здесь Lumen-слова
        (let/ret/echo/...) раскрашиваются своим токенизатором на regex,
        цвета — из той же темы редактора.
        """
        # Общий ведущий отступ (как в _add_code_block) убираем
        nonspace = [l for l in lines if l.strip()]
        if nonspace:
            indent = min(len(l) - len(l.lstrip(" ")) for l in nonspace)
            if indent:
                lines = [l[indent:] for l in lines]
        label = QLabel(self)
        label.setFont(self._popup_font())
        label.setWordWrap(False)
        label.setTextFormat(Qt.TextFormat.RichText)
        label.setTextInteractionFlags(Qt.TextInteractionFlag.NoTextInteraction)
        label.setAlignment(
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop
        )
        label.setStyleSheet(
            f"background: {self._code_bg_name()}; color: {self._fg()};"
        )
        label.setText(self._lumen_code_html(lines))
        fm = QFontMetrics(label.font())
        cw = fm.horizontalAdvance("0") or 8
        max_len = max((len(l) for l in lines), default=0)
        width = min(max_len * cw + 14, _CODE_MAX_WIDTH)
        line_h = fm.height() or (fm.ascent() + fm.descent())
        label.setFixedSize(int(width), len(lines) * line_h + 8)
        label.show()
        self._layout.addWidget(label)

    def _add_code_block(self, code: str, lang: str = ""):
        lines = code.strip("\n").split("\n") or [""]
        # pyright шлёт kind-заголовок («(function) foo(a: int) -> bool»)
        # в фенсе — одной строкой или многострочной сигнатурой; красим его
        # как заголовок, а не показываем кодом
        if self._looks_like_kind_block(lines):
            label = self._make_label_base()
            label.setText(self._render_kind_block(lines))
            self._layout.addWidget(label)
            return
        # Lumen-код — кастомная regex-подсветка вместо QsciLexerCPP
        self_lang = getattr(self.editor, "language_id", "")
        if self._is_lumen_lang(lang, self_lang):
            self._add_lumen_code_block(lines)
            return
        view = self._ensure_code_view(lang)
        # clangd цитирует определение с отступами исходника (структура
        # в namespace/классе) — общий ведущий отступ убираем, иначе
        # каждая строка кода начинается с «ступеньки»
        nonspace = [l for l in lines if l.strip()]
        if nonspace:
            indent = min(len(l) - len(l.lstrip(" ")) for l in nonspace)
            if indent:
                lines = [l[indent:] for l in lines]
        code = "\n".join(lines)
        view.setText(code)
        view.SendScintilla(SCI_COLOURISE, 0, -1)
        fm = QFontMetrics(view.font())
        cw = view.SendScintilla(SCI_TEXTWIDTH, 0, b"0")
        if not cw:
            cw = fm.horizontalAdvance("0")
        max_len = max(len(l) for l in lines)
        width = min(max_len * cw + 12, _CODE_MAX_WIDTH)
        line_h = view.SendScintilla(SCI_TEXTHEIGHT, 0) or fm.height()
        view.setFixedSize(int(width), len(lines) * line_h + 8)
        view.show()
        self._layout.addWidget(view)

    # ------------------------------------------------------------------
    # показ
    # ------------------------------------------------------------------

    def _content_height(self, inner_w: int) -> int:
        """Высота контента при ширине inner_w: суммарные высоты меток
        (с переносами wordWrap) и кодовых вью + отступы и промежутки."""
        margins = self._layout.contentsMargins()
        total = margins.top() + margins.bottom()
        spacing = self._layout.spacing()
        first = True
        for i in range(self._layout.count()):
            item = self._layout.itemAt(i)
            widget = item.widget() if item is not None else None
            if widget is None:
                continue
            if not first:
                total += spacing
            first = False
            h = widget.heightForWidth(inner_w)
            if h is None or h <= 0:
                # Кодовые вью сами фиксируют размер (setFixedSize в
                # _add_code_block); heightForWidth у QsciScintilla — -1, а
                # sizeHint — огромный дефолт, не отражающий реальной высоты
                if (isinstance(widget, QsciScintilla) and widget.height() > 0):
                    h = widget.height()
                elif (widget.sizePolicy().verticalPolicy()
                      == QSizePolicy.Policy.Fixed and widget.height() > 0):
                    # Lumen-лейбл (без wordWrap) тоже фиксирует размер
                    h = widget.height()
                else:
                    h = widget.sizeHint().height()
            total += h
        return total

    def _max_height(self) -> int:
        """Максимальная высота попапа: фиксированная, но не больше 60%
        рабочей высоты экрана (на маленьких экранах со скроллбаром)."""
        screen = self.editor.screen() or QApplication.primaryScreen()
        if screen is not None:
            avail_h = screen.availableGeometry().height()
            if avail_h < _MAX_HEIGHT * 2:
                return max(200, int(avail_h * 0.6))
        return _MAX_HEIGHT

    def show_at(self, global_pos: QPoint):
        """Показывает попап рядом с курсором, не выходя за экран.

        Длинный контент (справка по функции) обрезается по высоте:
        содержимое прокручивается колесом мыши над плашкой."""
        screen = self.editor.screen() or QApplication.primaryScreen()
        avail = screen.availableGeometry() if screen is not None else None
        avail_w = avail.width() - 16 if avail is not None else _MAX_WIDTH
        max_w = min(_MAX_WIDTH, avail_w)
        max_h = self._max_height()

        # Природная ширина контента — по самим виджетам: QLayout.sizeHint
        # кэшируется предыдущим показом и на повторном hover возвращает
        # прошлую (малую) ширину — плашка сужается и контент обрезается.
        # sizeHint меток от этого не зависит (wordWrap-лейблы дают ширину
        # самой длинной строки, фикс-метки кода — полный размер текста).
        margins = self._layout.contentsMargins()
        content_w = 0
        for i in range(self._layout.count()):
            item = self._layout.itemAt(i)
            widget = item.widget() if item is not None else None
            if widget is None:
                continue
            if isinstance(widget, QsciScintilla):
                # у кодовых вью зафиксирован размер (setFixedSize) —
                # это и есть ширина; sizeHint у Qsci — огромный дефолт
                hint = widget.width() or widget.sizeHint().width()
            else:
                hint = widget.sizeHint().width()
            if hint > content_w:
                content_w = hint
        nat_w = content_w + margins.left() + margins.right()

        inner_w = max(
            _MIN_WIDTH - 2 * _PAD_X, min(nat_w, max_w) - 2 * _PAD_X
        )
        # Высота контента: считаем сами — QLayout.heightForWidth после
        # первого показа видимого попапа может вернуть -1
        h0 = self._content_height(inner_w)

        # Полоса прокрутки внутри попапа: контент не сужается, а плашка
        # расширяется — правый отступ = зазор + место под саму полосу +
        # отступ скроллбара от правого края плашки (_SB_PAD_RIGHT).
        pad_right = _PAD_X + _SB_GAP + _SB_PAD_RIGHT if h0 > max_h else _PAD_X
        self._layout.setContentsMargins(_PAD_X, _PAD_Y, pad_right, _PAD_Y)

        wrap_w = inner_w + _PAD_X + pad_right
        wrap_h = h0
        self._wrap.resize(wrap_w, wrap_h)

        # Скроллбар нужен, только если контент выше лимита
        sb_used = SCROLLBAR_WIDTH if h0 > max_h else 0
        win_w = wrap_w + sb_used
        win_h = min(wrap_h, max_h)
        # Окно шире карточки на _SHADOW_MARGIN с каждой стороны — тень
        # рисуется в этот прозрачный запас и не обрезается окном
        self.resize(win_w + self._shadow_margin * 2,
                    win_h + self._shadow_margin * 2)

        # Позиционируется карточка, а не окно: прозрачный запас (_SHADOW_MARGIN)
        # под тень — невидимый, поэтому сдвиг окна на него не должен
        # отодвигать контент от курсора. Окно ставим на (card - margin),
        # и карточка встаёт ровно в cursor + offset, а запас вокруг — только
        # место под тень.
        x = global_pos.x() + _OFFSET_X
        y = global_pos.y() + _OFFSET_Y
        if avail is not None:
            if x + win_w > avail.right():
                x = max(avail.left(), avail.right() - win_w)
            if y + win_h > avail.bottom():
                # не помещается снизу — показываем над строкой ввода
                y = global_pos.y() - win_h - 12
                if y < avail.top():
                    y = avail.top()
        self._move_to(x - self._shadow_margin, y - self._shadow_margin)
        self._show_with_fade()

    def show_next_to(self, global_pos: QPoint):
        """Показывает плашку ровно в указанной глобальной точке (без
        смещения от курсора) — для доков, привязанных к списку
        автодополнения. Расчёт размеров, скроллбара и кламп к экрану —
        те же, что в show_at."""
        screen = self.editor.screen() or QApplication.primaryScreen()
        avail = screen.availableGeometry() if screen is not None else None
        avail_w = avail.width() - 16 if avail is not None else _MAX_WIDTH
        max_w = min(_MAX_WIDTH, avail_w)
        max_h = self._max_height()

        # Природная ширина контента — по самим виджетам (см. show_at)
        margins = self._layout.contentsMargins()
        content_w = 0
        for i in range(self._layout.count()):
            item = self._layout.itemAt(i)
            widget = item.widget() if item is not None else None
            if widget is None:
                continue
            if isinstance(widget, QsciScintilla):
                hint = widget.width() or widget.sizeHint().width()
            else:
                hint = widget.sizeHint().width()
            if hint > content_w:
                content_w = hint
        nat_w = content_w + margins.left() + margins.right()

        inner_w = max(
            _MIN_WIDTH - 2 * _PAD_X, min(nat_w, max_w) - 2 * _PAD_X
        )
        h0 = self._content_height(inner_w)

        pad_right = _PAD_X + _SB_GAP + _SB_PAD_RIGHT if h0 > max_h else _PAD_X
        self._layout.setContentsMargins(_PAD_X, _PAD_Y, pad_right, _PAD_Y)

        wrap_w = inner_w + _PAD_X + pad_right
        wrap_h = h0
        self._wrap.resize(wrap_w, wrap_h)

        sb_used = SCROLLBAR_WIDTH if h0 > max_h else 0
        win_w = wrap_w + sb_used
        win_h = min(wrap_h, max_h)
        self.resize(win_w + self._shadow_margin * 2,
                    win_h + self._shadow_margin * 2)

        x = global_pos.x()
        y = global_pos.y()
        if avail is not None:
            if x + win_w > avail.right():
                x = max(avail.left(), avail.right() - win_w)
            if y + win_h > avail.bottom():
                # не помещается снизу — показываем над анкером
                y = global_pos.y() - win_h - 12
                if y < avail.top():
                    y = avail.top()
        self._move_to(x - self._shadow_margin, y - self._shadow_margin)
        self._show_with_fade()

    # --- плавное появление/исчезание ---

    def _show_with_fade(self):
        """Показывает плашку: из непрозрачности 0 постепенно проявляет.

        Если плашка уже видима (обновление контента или разворот fade-out
        при возврате курсора) — анимация идёт с текущей непрозрачности."""
        self._hide_pending = False
        if not self.isVisible():
            try:
                self.setWindowOpacity(0.0)
            except RuntimeError:
                pass
        self.show()
        self.raise_()
        self._apply_hit_region()
        self._animate_to(1.0, _FADE_IN_MS)
        # Смена контента на видимой плашке — старый кадр растворяется в новый
        if self._wipe_pix is not None:
            self._start_wipe()

    # --- перетекание контента ---

    def _capture_wipe(self):
        """Снимок текущего вида плашки (старый контент под курсором)."""
        try:
            self._wipe_pix = self.grab()
        except RuntimeError:
            self._wipe_pix = None

    def _start_wipe(self):
        """Накрывает новый контент снимком старого и растворяет его."""
        if self._wipe_pix is None or not self.isVisible():
            return
        wipe = self._get_wipe_overlay()
        wipe.set_pixmap(self._wipe_pix)
        wipe.set_alpha(1.0)
        wipe.show()
        wipe.raise_()
        self._wipe_anim.stop()
        self._wipe_anim.setDuration(_WIPE_MS)
        self._wipe_anim.setStartValue(1.0)
        self._wipe_anim.setEndValue(0.0)
        self._wipe_anim.start()

    def _get_wipe_overlay(self) -> _WipeOverlay:
        if self._wipe is None:
            self._wipe = _WipeOverlay(self)
        self._wipe.setGeometry(self.rect())
        return self._wipe

    def _on_wipe(self, value):
        wipe = self._wipe
        if wipe is not None:
            wipe.set_alpha(float(value))

    def _on_wipe_done(self):
        wipe = self._wipe
        if wipe is not None:
            wipe.hide()
        self._wipe_pix = None

    # --- плавное перемещение ---

    def _move_to(self, x: int, y: int):
        """Ставит плашку в (x, y); если она уже видима — плавно переезжает."""
        try:
            target = QPoint(x, y)
            if not self.isVisible() or self.pos() == target:
                self.move(target)
                return
            self._pos_anim.stop()
            self._pos_anim.setDuration(_MOVE_MS)
            self._pos_anim.setStartValue(self.pos())
            self._pos_anim.setEndValue(target)
            self._pos_anim.start()
        except RuntimeError:
            pass

    def _on_pos(self, value):
        try:
            self.move(QPoint(value))
        except RuntimeError:
            pass

    def hide(self):
        """Плавно гасит плашку; реальное скрытие — по завершении анимации."""
        if not self.isVisible():
            super().hide()
            return
        self._hide_pending = True
        self._animate_to(0.0, _FADE_OUT_MS)

    def _animate_to(self, target: float, duration: int):
        try:
            start = self.windowOpacity()
        except RuntimeError:
            return
        if target > 0.5 and not self.isVisible():
            start = 0.0
        self._opacity.stop()
        self._opacity.setDuration(duration)
        self._opacity.setStartValue(start)
        self._opacity.setEndValue(target)
        self._opacity.start()

    def _on_opacity(self, value):
        try:
            self.setWindowOpacity(float(value))
        except RuntimeError:
            pass

    def _on_opacity_done(self):
        if self._hide_pending:
            self._hide_pending = False
            # Плашка скрылась — снимки перетекания больше не нужны,
            # иначе при следующем показе всплыл бы устаревший кадр
            self._wipe_anim.stop()
            self._pos_anim.stop()
            self._wipe_pix = None
            if self._wipe is not None:
                self._wipe.hide()
            try:
                super().hide()
            except RuntimeError:
                pass

    def enterEvent(self, event):
        """Курсор вошёл в плашку — отменяем отложенное скрытие."""
        if self._hide_pending:
            # Разворачиваем уже начавшийся fade-out
            self._hide_pending = False
            self._animate_to(1.0, _FADE_IN_MS)
        if self._bridge is not None:
            self._bridge._cancel_hide()

    def leaveEvent(self, event):
        """Курсор покинул плашку — запускаем отложенное скрытие."""
        if self._bridge is not None:
            self._bridge._schedule_hide()

    def near_cursor(self, margin: int = 20) -> bool:
        """Курсор рядом с попапом (включая «коридор» для перехода с токена).

        Нужно, чтобы попап не прятался, пока мышь идёт с слова на плашку,
        чтобы прокрутить её колесом."""
        if not self.isVisible():
            return False
        return self.geometry().adjusted(
            -margin, -margin, margin, margin
        ).contains(self.mapFromGlobal(QCursor.pos()))

    def under_mouse(self) -> bool:
        """Курсор находится над попапом (чтобы не прятать его)."""
        if not self.isVisible():
            return False
        return self.geometry().contains(self.mapFromGlobal(QCursor.pos()))
