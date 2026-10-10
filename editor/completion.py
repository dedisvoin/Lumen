"""
Автодополнение для .lm (LumenLang) — по образцу редактора TwistLang.

Список подсказок — собственный QListWidget поверх редактора (нативный
autocompletion QScintilla не используется): он не умеет красить подпись
другим цветом, а у нас слово яркое, описание — приглушённое (как в
VS Code). Enter/Tab вставляют слово, для сниппетов — раскрывают шаблон
с плейсхолдерами (Tab/Shift+Tab — навигация, индикатор 5).
"""

import os
import re
from typing import Dict, List, Optional, Tuple

from PyQt6 import sip

from PyQt6.QtCore import Qt, QTimer, QRect, QSize, QPoint
from PyQt6.QtGui import (QColor, QFont, QFontMetrics, QIcon, QPainter)
from PyQt6.QtWidgets import (QApplication, QFrame, QGraphicsDropShadowEffect,
                             QListWidget, QListWidgetItem, QStyle,
                             QStyledItemDelegate, QStyleOptionViewItem,
                             QVBoxLayout)
from PyQt6.Qsci import QsciScintilla

from editor.constants import UI_FONT_SIZE
from editor.font_settings import ui_font_family
from editor.settings_store import app_settings
from editor.lexers.lumen.lumen_lang import LumenLangLexer
from editor.lexers.asm.asm_lexer import AsmLexer
from editor.lexers.llvm.llvm_lexer import LLVMLexer
from editor.theme_utils import strengthen_border
from editor.scrollbar_qss import scrollbar_qss
from editor.lsp import get_lsp_manager, pos_to_lsp, lsp_to_pos
from editor.lsp.protocol import _plain_doc
from editor.text_utils import rgb_int
from editor.scintilla_constants import (
    SCI_COLOURISE,
    SCI_GETCURRENTPOS,
    SCI_GETLINEENDPOSITION,
    SCI_GETMARGINWIDTHN,
    SCI_GETSTYLEAT,
    SCI_LINEFROMPOSITION,
    SCI_POINTXFROMPOSITION,
    SCI_POINTYFROMPOSITION,
    SCI_POSITIONFROMLINE,
    SCI_REPLACESEL,
    SCI_SETCURRENTPOS,
    SCI_SETSEL,
    SCI_TEXTHEIGHT,
)
from editor.snippets import (
    SNIPPETS, SNIPPET_KIND_ID, SnippetSession, snippet_preview,
)

# Стили, внутри которых подсказки не показываются (комментарии, строки,
# escape-последовательности)
_NON_CODE_STYLES = {
    LumenLangLexer.STYLE_COMMENT,
    LumenLangLexer.STYLE_COMMENT_DOC,
    LumenLangLexer.STYLE_COMMENT_QUESTION,
    LumenLangLexer.STYLE_STRING,
    LumenLangLexer.STYLE_ESCAPE,
}

# Категории подсказок: (id иконки, буква, подпись в списке, атрибут лексера).
# Атрибут None означает, что слова этой категории собираются динамически
# из текста файла (см. _DYNAMIC_PATTERNS).
_COMPLETION_GROUPS = (
    (1, "K", "keyword", "keywords"),
    (2, "M", "modifier", "modifiers"),
    (3, "T", "type", "types"),
    (4, "L", "literal", "literals"),
    (5, "D", "directive", "directives"),
    (6, "S", "special word", "special_keywords"),
    (7, "C", "struct", None),
    (8, "#", "macro", None),
    (9, "V", "variable", None),
    (10, "F", "function", None),
    (11, "S", "snippet", None),
    (12, "N", "namespace", None),
)

# Цвета иконок категорий по умолчанию (Catppuccin) — используются,
# если в выбранной теме нет соответствующего стиля
_KIND_COLORS = {
    1: "#cba6f7",  # ключевое слово — mauve
    2: "#fab387",  # модификатор   — peach
    3: "#89dceb",  # тип           — teal
    4: "#a6e3a1",  # литерал       — green
    5: "#f38ba8",  # директива     — red
    6: "#89b4fa",  # спец. слово   — blue
    7: "#b4befe",  # структура     — lavender
    8: "#f9e2af",  # макрос        — yellow
    9: "#94e2d5",  # переменная    — mint
    10: "#f5c2e7",  # функция      — rose
    11: "#f9e2af",  # сниппет      — yellow
    12: "#74c7ec",  # namespace    — sapphire
}

_KIND_LABELS = {kind_id: label for kind_id, _letter, label, _attr
                in _COMPLETION_GROUPS}

# Языки, для которых работает кастомный список автодополнения
# (слова из лексера, без LSP): LumenLang, asm и LLVM
_CUSTOM_COMPLETION_LANGUAGES = frozenset({"lumenlang", "asm", "llvm"})

# Какая категория иконки какому стилю темы соответствует
_KIND_THEME_KEYS = {
    1: "keyword",       # ключевое слово
    2: "modifier",      # модификатор
    3: "type",          # тип
    4: "literal",       # литерал
    5: "directive",     # директива
    6: "special",       # спец. слово
    7: "class_name",    # структура
    8: "define_macro",  # макрос
    9: "variable",      # переменная
    10: "function_name",  # функция
    11: "keyword",        # сниппет
    12: "namespace",      # namespace
}

# Иконка категории из editor_icons/symbols (перекрашивается цветом темы)
_KIND_SYMBOL_ICONS = {
    1: "keyword",        # ключевое слово
    2: "modifier",       # модификатор
    3: "class_or_type",  # тип
    4: "constant",       # литерал
    5: "macros",         # директива
    6: "keyword",        # спец. слово
    7: "class_or_type",  # структура
    8: "macros",         # макрос
    9: "variable",       # переменная
    10: "function",      # функция
    11: "snippet",       # сниппет
    12: "namespace",     # namespace
}

# Символы, из которых состоит слово LumenLang (совпадает с wordCharacters
# лексера). По ним решаем, нужно ли после нажатия клавиши проверять стиль
# и закрывать список в комментариях/строках.
_WORD_CHARS = set(
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789_#"
)

# Директива подключения началась и каретка внутри её кавычек: показываем
# файлы/папки по пути, а не слова языка.
_INCLUDE_RE = re.compile(rb'^\s*#(?:include|cppinclude)\s*"')

# Категории иконок для элементов файлового дополнения в #include
_FOLDER_KIND_ID = 12  # папка (иконка namespace, сапфир)
_FILE_KIND_ID = 9     # файл (иконка variable, мята)

# Динамические идентификаторы: имя -> категория. Учитываются только до //.
_DYNAMIC_PATTERNS = (
    (r"\blet\s+([a-zA-Z_][a-zA-Z0-9_]*)", 9),          # переменная
    (r"\bnamespace\s+([a-zA-Z_][a-zA-Z0-9_]*)", 12),    # namespace
    (r"\bfn\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*\(", 10),      # функция
    (r"(?<!\w)#macro\s+([a-zA-Z_][a-zA-Z0-9_]*)", 8),    # макрос
    (r"(?<!\w)#define\s+([a-zA-Z_][a-zA-Z0-9_]*)", 8),   # макрос
    (r"\bstruct\s+([a-zA-Z_][a-zA-Z0-9_]*)", 7),        # структура
)

# Специальные описания директив (показываются в списке справа от слова)
_DIRECTIVE_DESCRIPTIONS = {
    #"#define": "макроопределение: #define NAME = value",
    #"#macro": "макрофункция: #macro NAME(...)",
}

# Задержка пересборки слов после изменения текста (динамические слова)
_REBUILD_DELAY_MS = 250

# --- LSP-дополнения (C/C++ через clangd) ---

# Символы-триггеры запроса дополнений: доступ к членам объекта
_LSP_TRIGGER_CHARS = set(".:>")
_LSP_DEBOUNCE_MS = 180

# LSP CompletionItemKind -> категория иконки (_COMPLETION_GROUPS)
_LSP_KIND_MAP = {
    1: 9,    # Text        -> переменная
    2: 10,   # Method      -> функция
    3: 10,   # Function
    4: 10,   # Constructor
    5: 9,    # Field
    6: 9,    # Variable
    7: 7,    # Class       -> структура
    8: 7,    # Interface
    9: 12,   # Module      -> namespace
    10: 9,   # Property
    11: 9,   # Unit
    12: 9,   # Value
    13: 7,   # Enum
    14: 1,   # Keyword     -> ключевое слово
    15: 11,  # Snippet
    16: 9,   # Color
    17: 9,   # File
    18: 9,   # Reference
    19: 9,   # Folder
    20: 9,   # EnumMember
    21: 4,   # Constant    -> литерал
    22: 7,   # Struct
    23: 9,   # Event
    24: 1,   # Operator
    25: 3,   # TypeParameter -> тип
    1000: 8,  # Lumen #define -> макрос
    1001: 8,  # Lumen #macro  -> макрос
    1002: 5,  # Lumen #pragma/#include/#define -> директива
    1003: 2,  # Lumen модификатор (const/static/...) -> модификатор
}

# Ключевые слова-типы C++: clangd шлёт их как Keyword, но в списке они
# должны быть помечены как тип (T), а не ключевое слово (K)
_LSP_TYPE_KEYWORDS = frozenset((
    "auto", "bool", "char", "char8_t", "char16_t", "char32_t",
    "decltype", "double", "float", "int", "long", "short",
    "signed", "unsigned", "void", "wchar_t",
))

# Плейсхолдеры сниппетов clangd: ${1:arg} -> arg, $0 -> пусто
_LSP_SNIPPET_RE = re.compile(r"\$\{\d+:([^}]*)\}|\$\{\d+\}|\$\d")


def _strip_snippet(text: str) -> str:
    return _LSP_SNIPPET_RE.sub(lambda m: m.group(1) or "", text)


def _parse_import(new_text: str):
    """Из newText доп. правки (строка импорта) -> (модуль, символ).

    basedpyright шлёт доп. правку вида «from os import path» или
    «import os», а в label/detail — «autoImport». Чтобы показать в списке
    понятное название модуля, парсим его отсюда.
    """
    t = (new_text or "").strip()
    m = re.match(r"^from\s+([\w.]+)\s+import\s+(.+)$", t)
    if m:
        mod = m.group(1)
        sym = m.group(2).split(",")[0].strip().split(" as ")[0].strip()
        return mod, sym
    m = re.match(r"^import\s+([\w.]+)", t)
    if m:
        mod = m.group(1)
        return mod, mod.rsplit(".", 1)[-1]
    return "", ""


def _edit_start(e):
    """Ключ сортировки доп. правки по позиции (строка, столбец)."""
    r = (e.get("range") or {}).get("start") or {}
    return (r.get("line", 0), r.get("character", 0))


def _is_import_stmt(text: str) -> bool:
    """Доп. правка — строка импорта (Python import/from, C++ #include)?"""
    t = (text or "").lstrip()
    return bool(re.match(r"^(import\s|from\s|#\s*include\b)", t))


def _file_top_for_import(editor) -> int:
    """Байтовая позиция для вставки импорта в самое начало файла.

    Соблюдаем PEP: она должна быть после shebang (#!) и строки кодировки
    (PEP 263), иначе они сломаются; во всех остальных случаях — байт 0.
    """
    try:
        full = editor.text()
    except Exception:
        return 0
    if not full:
        return 0
    lines = full.split("\n")
    ln = 0
    if lines[0].startswith("#!"):
        ln = 1
    if ln < len(lines) and re.match(r"^[ \t]*#.*coding[:=]", lines[ln]):
        ln += 1
    off = 0
    for i in range(ln):
        off += len(lines[i].encode("utf-8")) + 1  # + '\n'
    return off


def _dim_hex(fg_hex: str, bg_hex: str) -> str:
    """Приглушённый цвет описания: текст, смешанный с фоном (60/40)."""
    fg, bg = QColor(fg_hex), QColor(bg_hex)
    mix = lambda c1, c2: int(c1 * 0.6 + c2 * 0.4)
    return QColor(mix(fg.red(), bg.red()), mix(fg.green(), bg.green()),
                  mix(fg.blue(), bg.blue())).name()


class _CompletionDelegate(QStyledItemDelegate):
    """Рисует строку: иконка + яркое слово + тёмное описание справа."""

    def __init__(self, popup):
        super().__init__(popup)
        self.popup = popup

    def paint(self, painter, option, index):
        popup = self.popup
        selected = bool(option.state & QStyle.StateFlag.State_Selected)
        # Правая граница контента = правый край строки. item.rect во всех
        # случаях уже равен viewport за вычетом полосы прокрутки, поэтому
        # компенсировать её ширину не нужно: раньше при СКРЫТОЙ полосе
        # прибавляли SCROLLBAR_WIDTH, и правый текст уезжал за край
        # видимой области на всю ширину полосы.
        sb = popup.list.verticalScrollBar()
        sb_visible = sb.isVisible()
        right = option.rect.right()
        painter.save()
        if selected:
            # Выделенная строка — скруглённая плашка. Со скроллбаром —
            # отступ 4px справа (не даёт углам упереться в полосу); без
            # него плашка идёт заподлицо с краем строки, но в пределах
            # viewport, чтобы скругление справа не срезалось клипом.
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(popup.sel_color)
            painter.drawRoundedRect(
                option.rect.adjusted(0, 0, -(4 if sb_visible else 0), 0),
                6, 6)
        else:
            painter.fillRect(option.rect, popup.bg_color)
        icon = index.data(Qt.ItemDataRole.DecorationRole)
        x = option.rect.left() + 8
        if isinstance(icon, QIcon) and not icon.isNull():
            sz = popup.icon_size
            icon.paint(painter, QRect(
                x, option.rect.top() + (option.rect.height() - sz) // 2,
                sz, sz))
            x += sz + 8
        font = popup.font()
        painter.setFont(font)
        fm = QFontMetrics(font)
        word = index.data(Qt.ItemDataRole.UserRole)
        desc = index.data(Qt.ItemDataRole.UserRole + 1)
        word_w = fm.horizontalAdvance(word) + 4
        painter.setPen(popup.word_color)
        painter.drawText(QRect(x, option.rect.top(), word_w,
                               option.rect.height()),
                         Qt.AlignmentFlag.AlignVCenter
                         | Qt.AlignmentFlag.AlignLeft, word)
        if desc:
            gap = x + word_w + 12
            avail = max(right - gap - 8, 0)
            if avail > 0:
                elided = fm.elidedText(desc, Qt.TextElideMode.ElideRight,
                                       avail)
                painter.setPen(popup.desc_color)
                painter.drawText(QRect(gap, option.rect.top(), avail,
                                       option.rect.height()),
                                 Qt.AlignmentFlag.AlignVCenter
                                 | Qt.AlignmentFlag.AlignRight, elided)
        painter.restore()

    def sizeHint(self, option, index):
        width = self.popup.width() - 2 * self.popup._SHADOW_MARGIN
        return QSize(width, self.popup.row_height)


class CompletionPopup(QFrame):
    """Кастомный список автодополнения поверх редактора.

    Виджет-ребёнок редактора (не отдельное окно): увеличен на прозрачный
    запас (_SHADOW_MARGIN) вокруг карточки, а QGraphicsDropShadowEffect на
    карточке рисует тень — та же, что у ховер-попапа — в этот запас.
    Карточка рисует фон, рамку и скругление честно через QSS.
    """

    _MARGIN = 4  # отступ содержимого от рамки (зона скругления)
    _SHADOW_MARGIN = 40  # прозрачный запас вокруг карточки под тень

    def __init__(self, engine):
        super().__init__(engine.editor)
        self.engine = engine
        self.bg_color = QColor("#1e1e2e")
        self.sel_color = QColor("#45475a")
        self.border_color = QColor("#45475a")
        self.word_color = QColor("#cdd6f4")
        self.desc_color = QColor("#6c7086")
        self.row_height = 20
        self.icon_size = 16
        self._max_rows = 12
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint
                            | Qt.WindowType.NoDropShadowWindowHint)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setFrameShape(QFrame.Shape.NoFrame)

        self.list = QListWidget(self)
        self.list.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.list.setFrameShape(QFrame.Shape.NoFrame)
        self.list.setHorizontalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.list.setVerticalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self.list.setItemDelegate(_CompletionDelegate(self))
        self.list.setUniformItemSizes(True)
        self.list.viewport().setAutoFillBackground(False)
        self.list.itemClicked.connect(self._on_item_clicked)
        # Смена выделения (стрелки/клик) — обновить плашку документации
        self.list.currentRowChanged.connect(self.engine._update_doc_panel)

        # Карточка — видимая часть виджета: рисует фон/рамку/скругление,
        # вокруг неё — прозрачный запас виджета, куда свешивается тень.
        self._card = QFrame(self)
        self._card.setObjectName("completionCard")
        self._card.setAttribute(
            Qt.WidgetAttribute.WA_StyledBackground, True)
        card_layout = QVBoxLayout(self._card)
        card_layout.setContentsMargins(self._MARGIN, self._MARGIN,
                                       self._MARGIN, self._MARGIN)
        card_layout.setSpacing(0)
        card_layout.addWidget(self.list)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(self._SHADOW_MARGIN, self._SHADOW_MARGIN,
                                  self._SHADOW_MARGIN, self._SHADOW_MARGIN)
        layout.setSpacing(0)
        layout.addWidget(self._card)
        # Тень как у ховер-попапа: крупнее, чем у плашки закладки
        shadow = QGraphicsDropShadowEffect(self._card)
        shadow.setBlurRadius(100)
        shadow.setOffset(-5, 6)
        shadow.setColor(QColor(0, 0, 0, 90))
        self._card.setGraphicsEffect(shadow)

        self.hide()

    def _on_item_clicked(self, _item):
        self.engine.accept_popup()

    # Прокси к списку: движок и делегат работают с попапом как раньше
    def currentItem(self):
        return self.list.currentItem()

    def currentRow(self):
        return self.list.currentRow()

    def row(self, item):
        return self.list.row(item)

    def count(self):
        return self.list.count()

    def setCurrentRow(self, row):
        self.list.setCurrentRow(row)

    def scrollToItem(self, item):
        self.list.scrollToItem(item)

    def _in_card(self, pos) -> bool:
        """Позиция внутри карточки (в координатах виджета)."""
        return self._card.geometry().contains(pos.toPoint())

    # Прозрачный запас вокруг карточки не должен ловить мышь: события в зоне
    # тени игнорируются и уходят родителю (редактору), как если бы попапа
    # над ним не было.
    def mousePressEvent(self, event):
        if not self._in_card(event.position()):
            event.ignore()
            return
        super().mousePressEvent(event)

    def mouseReleaseEvent(self, event):
        if not self._in_card(event.position()):
            event.ignore()
            return
        super().mouseReleaseEvent(event)

    def mouseDoubleClickEvent(self, event):
        if not self._in_card(event.position()):
            event.ignore()
            return
        super().mouseDoubleClickEvent(event)

    def mouseMoveEvent(self, event):
        if not self._in_card(event.position()):
            event.ignore()
            return
        super().mouseMoveEvent(event)

    def wheelEvent(self, event):
        if not self._in_card(event.position()):
            event.ignore()
            return
        super().wheelEvent(event)

    def set_entries(self, entries, docs=None):
        """entries: [(слово, категория, описание)], docs — опциональный
        список документации по каждому элементу — заполняет список."""
        self.list.clear()
        fm = QFontMetrics(self.font())
        max_text_w = 0
        for i, (word, kind_id, desc) in enumerate(entries):
            icon = self.engine.kind_icons.get(kind_id) or QIcon()
            item = QListWidgetItem(icon, word)
            item.setData(Qt.ItemDataRole.UserRole, word)
            item.setData(Qt.ItemDataRole.UserRole + 1, desc)
            item.setData(Qt.ItemDataRole.UserRole + 2, kind_id)
            item.setData(Qt.ItemDataRole.UserRole + 3,
                         (docs[i] if docs else None) or "")
            self.list.addItem(item)
            w = (fm.horizontalAdvance(word)
                 + (fm.horizontalAdvance(desc) if desc else 0))
            max_text_w = max(max_text_w, w)
        # Ширина по содержимому: длинные типы не обрезаем до 700px,
        # но и за край экрана не выходим
        screen = self.screen() or QApplication.primaryScreen()
        max_w = (screen.availableGeometry().width() - 16
                 if screen is not None else 700)
        width = min(max(180, max_text_w + self.icon_size + 56),
                    max_w, 700)
        rows = min(len(entries), self._max_rows)
        # До 7 элементов список целиком помещается — скроллбар не нужен
        self.list.setVerticalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff
            if len(entries) <= 7
            else Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        # Строка на всю ширину списка, иначе делегат нарисует её
        # только по своему sizeHint и выделение не дотянется до края
        for i in range(self.list.count()):
            self.list.item(i).setSizeHint(QSize(width, self.row_height))
        # Окно целиком = карточка (список + поля под рамку/скругление +
        # рамка 1px сверху и снизу) + прозрачный запас под тень с каждой
        # стороны; позиция попапа считается по размеру карточки
        # (см. _position_popup). Без +2 viewport на 2px меньше контента,
        # и выбор последнего элемента прокручивает список
        self.setFixedSize(
            width + 2 * self._MARGIN + 2 * self._SHADOW_MARGIN,
            rows * self.row_height + 2 * self._MARGIN + 2
            + 2 * self._SHADOW_MARGIN)
        self.setCurrentRow(0)
        if self.count():
            self.scrollToItem(self.currentItem())

    def move_next(self, delta):
        row = max(0, min(self.currentRow() + delta, self.count() - 1))
        self.setCurrentRow(row)
        if self.count():
            self.scrollToItem(self.currentItem())


class CompletionEngine:
    """Автодополнение для редакторов LumenLang (.lm)."""

    def __init__(self, editor):
        self.editor = editor
        self.enabled = False
        self._icons_registered = False
        self._static_words: List[Tuple[str, int]] = []
        self._dynamic_words: Dict[str, int] = {}
        self.kind_icons: Dict[int, QIcon] = {}
        self.popup = CompletionPopup(self)


        # Сеанс сниппета: плейсхолдеры и навигация по Tab (см. snippets.py)
        self._snippet = SnippetSession()

        # Пересборка динамических слов после изменения текста (let/fn/...)
        self._rebuild_timer = QTimer(self.editor)
        self._rebuild_timer.setSingleShot(True)
        self._rebuild_timer.timeout.connect(self._rebuild_from_text)

        # Сдвиг позиций плейсхолдеров при вводе/удалении текста
        editor.SCN_MODIFIED.connect(self._on_modified)
        # Курсор ушёл из плейсхолдера (клик/стрелки) — сеанс завершён
        editor.cursorPositionChanged.connect(self._on_cursor_moved)
        # Любая правка текста / прокрутка закрывает кастомный список
        editor.textChanged.connect(self._on_text_changed)
        editor.verticalScrollBar().valueChanged.connect(self.hide_popup)
        editor.horizontalScrollBar().valueChanged.connect(self.hide_popup)

        # LSP-дополнения (сервер по конфигу языка): запрос с дебаунсом
        self.lsp_enabled = False
        self._lsp_trigger_chars = set(_LSP_TRIGGER_CHARS)
        self._lsp_seq = 0
        self._lsp_inserts: Dict[int, str] = {}
        self._lsp_text_edits: Dict[int, dict] = {}
        self._lsp_adds: Dict[int, list] = {}
        self._lsp_timer = QTimer(self.editor)
        self._lsp_timer.setSingleShot(True)
        self._lsp_timer.timeout.connect(self._lsp_request)

        # Плашка документации выбранного элемента. Свой экземпляр HoverPopup
        # (не трогаем курсовый ховер bridge'а), создаётся лениво. Источник
        # доков — как у ховера: documentation из completion, при его
        # отсутствии — textDocument/hover (с кэшем и защитой от устаревших)
        self._doc_popup = None
        self._doc_last_text: Optional[str] = None
        self._doc_cache: Dict[str, str] = {}
        self._doc_hover_seq = 0
        self._doc_req_ctx = (0, 0)  # (каретка, длина префикса) на момент запуска
        self._doc_hover_timer = QTimer(self.editor)
        self._doc_hover_timer.setSingleShot(True)
        self._doc_hover_timer.timeout.connect(self._doc_request_hover)

    # ------------------------------------------------------------------
    # Включение / выключение
    # ------------------------------------------------------------------

    def set_language(self, language_id):
        """Кастомный список: LumenLang (.lm) из лексера, LSP-языки — из сервера."""
        config = get_lsp_manager().config_for(language_id)
        self.lsp_enabled = bool(config) and get_lsp_manager().server_available(
            language_id)
        self._lsp_trigger_chars = (
            set(config.trigger_chars) if config else set(_LSP_TRIGGER_CHARS)
        )
        self.enabled = language_id in _CUSTOM_COMPLETION_LANGUAGES
        # LumenLang: при доступном LSP-сервере символы даёт языковой сервер
        # (вся программа анализируется — переменные/функции/неймспейсы),
        # а не набор регулярок по тексту. Без сервера — кастомный список.
        if language_id == "lumenlang":
            self.enabled = not self.lsp_enabled
        if not (self.enabled or self.lsp_enabled):
            self._rebuild_timer.stop()
            self.hide_popup()
            self.editor.setAutoCompletionSource(
                QsciScintilla.AutoCompletionSource.AcsNone
            )
        if self.lsp_enabled and not self._icons_registered:
            self._register_icons()

    def on_lexer_set(self, lexer):
        """Вызывается после setLexer() — инициализирует автодополнение."""
        if self.enabled and isinstance(lexer, (LumenLangLexer, AsmLexer,
                                               LLVMLexer)):
            self._setup_apis(lexer)
            self.schedule_rebuild()
        else:
            self.hide_popup()
            self.editor.setAutoCompletionSource(
                QsciScintilla.AutoCompletionSource.AcsNone
            )

    def schedule_rebuild(self):
        """Запланировать пересборку API (после изменения текста)."""
        if self.enabled:
            self._rebuild_timer.start(_REBUILD_DELAY_MS)

    def _setup_apis(self, lexer: LumenLangLexer):
        """Иконки, статические слова и настройки кастомного автокомплита."""
        editor = self.editor
        if not self._icons_registered:
            self._register_icons()

        # Нативный список QScintilla не используется — список рисуем сами
        editor.setAutoCompletionSource(
            QsciScintilla.AutoCompletionSource.AcsNone
        )
        editor.setAutoCompletionThreshold(1)

        # Индикатор подсветки плейсхолдеров сниппета
        self._snippet.setup_indicator(editor)

        self._static_words = self._collect_static_words(lexer)
        if getattr(editor, "_current_theme", None):
            self.apply_theme(editor._current_theme)

    def _register_icons(self, styles: dict = None):
        """Иконки категорий: SVG из editor_icons/symbols в цветах темы."""
        # Импорт в рантайме: editor.widgets -> code_editor -> completion
        # (циклический импорт на этапе загрузки модуля)
        from editor.widgets.file_icons import load_symbol_icon
        for kind_id, _letter, _label, _attr in _COMPLETION_GROUPS:
            color = QColor(_KIND_COLORS[kind_id])
            if styles:
                data = styles.get(_KIND_THEME_KEYS[kind_id])
                if data and data.get("color"):
                    color = QColor(data["color"])
            self.kind_icons[kind_id] = load_symbol_icon(
                _KIND_SYMBOL_ICONS[kind_id], color
            )
        self._icons_registered = True

    def apply_theme(self, theme_name: str):
        """Окрашивает кастомный список автодополнения в цвета темы."""
        editor = self.editor
        theme = editor.theme_manager.themes.get(theme_name, {})
        colors = theme.get("colors", {})
        # Фон списка — как фон редактора; выделение — как в редакторе
        bg_hex = (colors.get("background")
                  or colors.get("caret_line") or "#1e1e2e")
        fg_hex = colors.get("foreground") or "#cdd6f4"
        sel_hex = colors.get("selection") or "#45475a"
        bg, fg = QColor(bg_hex), QColor(fg_hex)
        sel = QColor(sel_hex)
        border = QColor(strengthen_border(
            bg_hex, colors.get("border") or sel_hex, fg_hex
        ))
        # Описание (подпись справа) — цвет комментария темы; если его
        # нет — приглушённый текст (смесь с фоном), как в VS Code
        styles = theme.get("styles", {})
        comment = styles.get("comment") or {}
        desc_hex = comment.get("color") or _dim_hex(fg_hex, bg_hex)

        popup = self.popup
        popup.bg_color = bg
        popup.sel_color = sel
        popup.border_color = border
        popup.word_color = fg
        popup.desc_color = QColor(desc_hex)
        # Шрифт списка — фиксированный UI-шрифт, не зависит от зума редактора
        popup.setFont(QFont(ui_font_family(), UI_FONT_SIZE))
        fm = QFontMetrics(popup.font())
        popup.row_height = max(fm.height() + 6, 20)
        popup.icon_size = UI_FONT_SIZE * 2
        popup.setStyleSheet(
            "CompletionPopup { background: transparent; }"
            "QFrame#completionCard {"
            f"  background-color: {bg.name()};"
            f"  border: 0.5px solid {border.name()};"
            "  border-radius: 8px;"
            "}"
            "QListWidget {"
            "  background: transparent;"
            "  border: none;"
            "}"
            "QListWidget::item {"
            "  padding: 2px 8px;"
            "  margin: 0px 4px;"
            "  border-radius: 4px;"
            "}"
            "QListWidget::item:selected {"
            "  background-color: rgba(255, 255, 255, 0.05);"
            "}"
            + scrollbar_qss(bg.name(), fg)
        )
        # Иконки категорий — цветами стилей темы (как подсветка кода)
        if self.enabled or self.lsp_enabled:
            self._register_icons(styles)

        # Цвет индикатора плейсхолдеров — как выделение в редакторе
        fore_int = rgb_int(sel)
        self._snippet.set_indicator_color(self.editor, fore_int)

        # Плашка документации — те же цвета темы, что у ховера
        if self._doc_popup is not None:
            self._doc_popup.apply_theme(theme_name)

    def _collect_static_words(self, lexer) -> List[Tuple[str, int]]:
        """(слово, категория) из словарей лексера.

        Слова-сниппеты тоже включаем: в списке они идут и как сниппет,
        и как ключевое слово/директива (см. _candidates).
        """
        words: List[Tuple[str, int]] = []
        for kind_id, _letter, _label, attr in _COMPLETION_GROUPS:
            if attr is None:
                continue
            for word in getattr(lexer, attr, ()):
                words.append((word, kind_id))
        return words

    # ------------------------------------------------------------------
    # Кастомный список: кандидаты и показ
    # ------------------------------------------------------------------

    @staticmethod
    def _label(word: str, kind_id: int) -> str:
        """Подпись слова в списке (справа от слова)."""
        return _DIRECTIVE_DESCRIPTIONS.get(word) or _KIND_LABELS[kind_id]

    # ------------------------------------------------------------------
    # Файловое дополнение в #include "…" / #cppinclude "…"
    # ------------------------------------------------------------------

    def _include_context(self) -> Optional[Tuple[str, int]]:
        """(частичный путь, начало вставки в байтах), если курсор внутри
        кавычек #include "…" — иначе None.

        Закрывающая кавычка уже напечатана — контекст не действует.
        """
        editor = self.editor
        pos = editor.SendScintilla(SCI_GETCURRENTPOS)
        line = editor.SendScintilla(SCI_LINEFROMPOSITION, pos)
        line_start = editor.SendScintilla(SCI_POSITIONFROMLINE, line)
        col = pos - line_start
        line_bytes = editor.text(line).encode("utf-8")
        head = line_bytes[:col]
        if not _INCLUDE_RE.match(head):
            return None
        # Ровно одна кавычка (открывающая): закрывающая уже напечатана —
        # контекст файлового дополнения не действует.
        if head.count(b'"') != 1:
            return None
        q = head.find(b'"')
        prefix = head[q + 1:]
        # Вставке отдаём байтовую позицию начала последнего сегмента пути
        # (после последней "/" или "\"): уже набранная директория
        # "std/…" останется на месте, а не затрется целиком.
        seg = max(prefix.rfind(b"/"), prefix.rfind(b"\\")) + 1
        return prefix.decode("utf-8", "replace"), line_start + q + 1 + seg

    @staticmethod
    def _is_lumen_root(d: str) -> bool:
        """Корень проекта Lumen: папка со std/*.lm или бинарём lumenc."""
        try:
            std = os.path.join(d, "std")
            if os.path.isdir(std):
                for name in os.listdir(std):
                    if name.lower().endswith(".lm"):
                        return True
            return (os.path.isfile(os.path.join(d, "lumenc.exe"))
                    or os.path.isfile(os.path.join(d, "lumenc")))
        except OSError:
            return False

    def _include_roots(self) -> List[str]:
        """Корни, относительно которых ищутся пути в #include.

        Пути lumenc резолвит относительно рабочей папки (корня проекта),
        поэтому ищем папку файла, а от неё вверх — ближайший корень Lumen
        (там живут std/*.lm). Запасной вариант — текущая директория.
        """
        roots: List[str] = []
        seen = set()

        def add(d: str):
            d = os.path.normcase(os.path.abspath(d))
            if d not in seen:
                seen.add(d)
                roots.append(d)

        file_path = getattr(self.editor, "file_path", None) or ""
        if file_path:
            add(os.path.dirname(file_path))
            cur = os.path.abspath(os.path.dirname(file_path))
            while True:
                parent = os.path.dirname(cur)
                if parent == cur:
                    break
                cur = parent
                if self._is_lumen_root(cur):
                    add(cur)
                    break
        else:
            add(os.getcwd())
        return roots or [os.path.abspath(os.getcwd())]

    def _include_candidates(self, prefix: str) -> List[Tuple[str, int, str]]:
        """(имя с завершающим "/" для папок, категория, подпись) по части
        пути: папки первыми, дальше файлы; скрытые — только если префикс
        уже начинается с точки."""
        raw = prefix.replace("\\", "/")
        if raw.endswith("/"):
            # Финальный "/" — листаем содержимое этой папки, фильтр пустой.
            dir_part = raw.rstrip("/")
            name_part = ""
        else:
            dir_part, name_part = os.path.split(raw.rstrip("/"))
        entries: Dict[Tuple[bool, str], Tuple[str, int, str]] = {}
        for root in self._include_roots():
            scan = os.path.join(root, dir_part) if dir_part else root
            if not os.path.isdir(scan):
                continue
            try:
                names = os.listdir(scan)
            except OSError:
                continue
            if name_part and not name_part.startswith("."):
                names = [n for n in names if not n.startswith(".")]
            low = name_part.lower()
            for n in names:
                if not n.lower().startswith(low):
                    continue
                is_dir = os.path.isdir(os.path.join(scan, n))
                key = (is_dir, n.lower())
                if key in entries:
                    continue
                low_n = n.lower()
                if is_dir:
                    desc = "folder"
                elif low_n.endswith(".lm"):
                    desc = "module"
                elif low_n.endswith(".cpp"):
                    desc = "cpp-module"
                else:
                    desc = "файл"
                entries[key] = (n + ("/" if is_dir else ""),
                                _FOLDER_KIND_ID if is_dir else _FILE_KIND_ID,
                                desc)
                if len(entries) >= 256:
                    break
            if len(entries) >= 256:
                break
        out = list(entries.values())
        out.sort(key=lambda t: (t[1] != _FOLDER_KIND_ID, t[0].lower()))
        return out

    def _word_start(self, pos: int) -> Optional[int]:
        """Начало слова LumenLang слева от позиции (None, если не в слове)."""
        editor = self.editor
        line = editor.SendScintilla(SCI_LINEFROMPOSITION, pos)
        line_start = editor.SendScintilla(SCI_POSITIONFROMLINE, line)
        col = pos - line_start
        line_bytes = editor.text(line).encode("utf-8")
        b = col
        while b > 0 and chr(line_bytes[b - 1]) in _WORD_CHARS:
            b -= 1
        if b == col:
            return None
        return line_start + b

    def _current_prefix(self) -> str:
        """Слово под курсором (префикс для фильтрации списка)."""
        editor = self.editor
        pos = editor.SendScintilla(SCI_GETCURRENTPOS)
        start = self._word_start(pos)
        if start is None:
            return ""
        line = editor.SendScintilla(SCI_LINEFROMPOSITION, pos)
        line_start = editor.SendScintilla(SCI_POSITIONFROMLINE, line)
        line_bytes = editor.text(line).encode("utf-8")
        return line_bytes[start - line_start:pos - line_start].decode(
            "utf-8", "replace")

    def _candidates(self, prefix: str) -> List[Tuple[str, int, str]]:
        """(слово, категория, описание) по префиксу: статика, сниппеты,
        динамика."""
        prefix_l = prefix.lower()
        out: List[Tuple[str, int, str]] = []
        seen = set()
        for word, kind_id in self._static_words:
            if word.lower().startswith(prefix_l) and word not in seen:
                seen.add(word)
                out.append((word, kind_id, self._label(word, kind_id)))
        for word in SNIPPETS:
            # Дедупликацию по слову не делаем: слово может быть и ключевым
            # словом (например namespace) — нужны оба варианта: ключевое
            # слово и сниппет с шаблоном
            if word.lower().startswith(prefix_l):
                seen.add(word)
                out.append((word, SNIPPET_KIND_ID,
                            snippet_preview(SNIPPETS[word])))
        for word, kind_id in self._dynamic_words.items():
            if word.lower().startswith(prefix_l) and word not in seen:
                seen.add(word)
                out.append((word, kind_id, self._label(word, kind_id)))
        return out

    def _position_popup(self):
        """Ставит список под строкой с курсором (в координатах редактора),
        не выходя за края экрана.

        Виджет расширен на прозрачный запас под тень; позиционируется
        карточка, а не виджет: сдвиг на запас не должен отодвигать список
        от каретки — карточка встаёт ровно под строкой с курсором.
        К экрану прижимаем по размеру карточки.

        Координаты берём у самого Scintilla (SCI_POINTXFROMPOSITION/
        SCI_POINTYFROMPOSITION): они учитывают зум, маргины, горизон-
        тальный скролл и перенос строк. Ручной расчёт по ширине символа
        (QFontMetrics(editor.font())) съезжал при зуме — zoomIn/Out меняет
        отображаемый шрифт, но не editor.font().
        """
        editor = self.editor
        popup = self.popup
        pos = editor.SendScintilla(SCI_GETCURRENTPOS)
        # wParam=0 обязателен: без него SendScintilla в PyQt6 падает
        x = editor.SendScintilla(SCI_POINTXFROMPOSITION, 0, pos)
        y = editor.SendScintilla(SCI_POINTYFROMPOSITION, 0, pos)
        margin_w = sum(editor.SendScintilla(SCI_GETMARGINWIDTHN, i)
                       for i in range(5))
        line_h = editor.SendScintilla(SCI_TEXTHEIGHT, 0)
        # Виджет расширен на прозрачный запас под тень; позиционируется
        # карточка, а не виджет: ставим на (карточка - запас), и карточка
        # встаёт ровно под строкой с курсором.
        card_w = popup.width() - 2 * popup._SHADOW_MARGIN
        card_h = popup.height() - 2 * popup._SHADOW_MARGIN
        # Глобальная позиция карточки (под строкой с курсором)
        tl = editor.mapToGlobal(QPoint(x, y + line_h))
        screen = editor.screen() or QApplication.primaryScreen()
        if screen is not None:
            avail = screen.availableGeometry()
            if tl.x() + card_w > avail.right():
                tl.setX(max(avail.left(), avail.right() - card_w))
            if tl.y() + card_h > avail.bottom():
                # Не помещается снизу — показываем над строкой ввода
                tl.setY(editor.mapToGlobal(QPoint(x + margin_w, y)).y()
                        - card_h)
                if tl.y() < avail.top():
                    tl.setY(avail.top())
        card_pos = editor.mapFromGlobal(tl)
        popup.move(card_pos.x() - popup._SHADOW_MARGIN,
                   card_pos.y() - popup._SHADOW_MARGIN)

    def popup_visible(self) -> bool:
        return self.popup.isVisible()

    def hide_popup(self, *_):
        self.popup.hide()
        self._hide_doc_popup()

    def _on_text_changed(self):
        """При вводе список не мерцает: остаётся видимым и следует за
        курсором. Прячем только когда слово-контекст разрушено."""
        if not self.popup.isVisible():
            return
        editor = self.editor
        if editor is None or sip.isdeleted(editor):
            return
        # Каретка в момент textChanged ещё на старой позиции —
        # проверяем отложенно, когда Scintilla обновила курсор
        QTimer.singleShot(0, self._on_text_changed_deferred)

    def _on_text_changed_deferred(self):
        if not self.popup.isVisible():
            return
        editor = self.editor
        if editor is None or sip.isdeleted(editor):
            return
        if self._current_prefix() or self._include_context() is not None:
            self._position_popup()
            self._update_doc_panel()
            return
        line, col = editor.getCursorPosition()
        line_text = editor.text(line)
        if (col > 0 and col <= len(line_text)
                and line_text[col - 1] in self._lsp_trigger_chars):
            self._position_popup()
            self._update_doc_panel()
        else:
            self.hide_popup()

    def popup_next(self, delta: int):
        self.popup.move_next(delta)

    def accept_popup(self):
        """Enter/Tab/клик: вставить выбранное слово (сниппет — раскрыть)."""
        popup = self.popup
        item = popup.currentItem()
        popup.hide()
        self._hide_doc_popup()
        if item is None:
            return
        if (self.lsp_enabled and not self.enabled
                and self._include_context() is None):
            self._lsp_accept(item)
            return
        word = item.data(Qt.ItemDataRole.UserRole)
        kind_id = item.data(Qt.ItemDataRole.UserRole + 2)
        self._insert_word(word, kind_id)

    # ------------------------------------------------------------------
    # Плашка документации выбранного элемента (как ховер при наведении)
    # ------------------------------------------------------------------

    def _get_doc_popup(self):
        """Док-попап: свой экземпляр HoverPopup, чтобы не конфликтовать
        с ховером от наведения мыши. Создаётся лениво (asm/llvm без доков
        не создают лишних окна)."""
        if self._doc_popup is None:
            from editor.widgets.hover_popup import HoverPopup
            self._doc_popup = HoverPopup(self.editor, None)
            theme = (getattr(self.editor, "_current_theme", None)
                     or self.editor.theme_manager.current_theme)
            self._doc_popup.apply_theme(theme)
        return self._doc_popup

    def _update_doc_panel(self, *_):
        """Документация выбранного элемента — ховер-попапом справа от списка.

        Док берём из самих элементов completion; если сервер его не
        приложил — запрашиваем textDocument/hover (как обычный ховер).
        """
        if self.editor is None or sip.isdeleted(self.editor):
            return
        popup = self.popup
        item = popup.currentItem()
        if item is None or not popup.isVisible():
            self._hide_doc_popup()
            return
        if self._include_context() is not None:
            self._doc_show("")
            return
        text = item.data(Qt.ItemDataRole.UserRole + 3) or ""
        if not text and self.lsp_enabled and not self.enabled:
            word = item.data(Qt.ItemDataRole.UserRole)
            if word:
                cached = self._doc_cache.get(word)
                if cached is not None:
                    text = cached
                else:
                    self._hide_doc_popup()
                    # Запоминаем слово-префикс (i) и его диапазон в буфере:
                    # lumen-lsp в resolve «дописывает» его до полного label
                    # и считает hover, как будто слово набрано целиком.
                    # Пустой префикс (курсор сразу после триггера: `Time::`,
                    # `x.`) — диапазон нулевой длины на месте каретки, иначе
                    # доки для списка после `::` вообще не запрашивались.
                    editor = self.editor
                    caret = editor.SendScintilla(SCI_GETCURRENTPOS)
                    ws = self._word_start(caret)
                    if ws is None:
                        ws = caret
                    self._doc_req_ctx = (caret, caret - ws)
                    self._doc_hover_timer.start(_LSP_DEBOUNCE_MS)
                    return  # пока летит ответ, старую доку показывать нельзя
        self._doc_show(text)

    def _doc_show(self, text):
        popup = self.popup
        if not text or not popup.isVisible():
            self._hide_doc_popup()
            return
        doc_popup = self._get_doc_popup()
        if text != self._doc_last_text:
            doc_popup.set_content(text)
            self._doc_last_text = text
        # Якорь — правее карточки и на её верхнем уровне: y=0 дал бы верх виджета,
        # а тот на _SHADOW_MARGIN(тень) выше карточки — ховер повисал бы.
        doc_popup.show_next_to(
            popup.mapToGlobal(QPoint(
                popup.width() - popup._SHADOW_MARGIN + 8,
                popup._SHADOW_MARGIN))
        )

    def _hide_doc_popup(self):
        self._doc_last_text = None
        self._doc_hover_timer.stop()
        if self._doc_popup is not None:
            self._doc_popup.hide()

    def _doc_request_hover(self):
        """Доков нет в ответе completion — просим их у сервера.

        Для lumen-lsp — completionItem/resolve (там нет documentation в
        элементах, а hover по каретке видит только префикс); для остальных
        серверов — textDocument/hover как обычный ховер."""
        editor = self.editor
        if editor is None or sip.isdeleted(editor):
            return
        if not (self.lsp_enabled and not self.enabled):
            return
        self._doc_hover_timer.stop()
        seq = self._doc_hover_seq + 1
        self._doc_hover_seq = seq
        popup = self.popup
        item = popup.currentItem() if popup.isVisible() else None
        label = (item.data(Qt.ItemDataRole.UserRole) if item is not None
                 else "") or ""
        if not label:
            self._update_doc_panel()
            return
        caret, prefix_len = self._doc_req_ctx or (
            editor.SendScintilla(SCI_GETCURRENTPOS), 0)
        start = max(caret - prefix_len, 0)
        lang = getattr(editor, "language_id", "") or ""
        lsp_pos = pos_to_lsp(editor, start)
        if lang == "lumenlang":
            get_lsp_manager().request_resolve(
                editor, lsp_pos, prefix_len, label,
                lambda resolved: self._doc_on_hover(
                    seq,
                    _plain_doc(resolved.get("documentation"))
                        if resolved is not None
                        else ""))
        else:
            get_lsp_manager().request_hover(
                editor, lsp_pos,
                lambda text: self._doc_on_hover(seq, text))

    def _doc_on_hover(self, seq, text):
        if seq != self._doc_hover_seq:
            return  # ответ устарел — пользователь уже выбрал другой элемент
        popup = self.popup
        item = popup.currentItem() if popup.isVisible() else None
        if item is not None:
            word = item.data(Qt.ItemDataRole.UserRole)
            if word:
                # Кэш хранит и пустой ответ, чтобы не дёргать сервер повторно
                self._doc_cache[word] = (text or "").strip()
        self._update_doc_panel()

    # ------------------------------------------------------------------
    # LSP-дополнения (clangd): запрос и вставка
    # ------------------------------------------------------------------

    def _lsp_request(self):
        """Запрашивает дополнения у clangd на текущей позиции курсора."""
        editor = self.editor
        if editor is None or sip.isdeleted(editor):
            return
        if not self.lsp_enabled:
            return
        if self._include_context() is not None:
            return  # файловое дополнение #include ведёт клиентский список
        pos = editor.SendScintilla(SCI_GETCURRENTPOS)
        # Без префикса показываем список только после триггер-символов
        # (доступ к членам через . : >) — иначе clangd вернёт всё подряд
        prefix = self._current_prefix()
        if not prefix:
            line, col = editor.getCursorPosition()
            line_text = editor.text(line)
            if (col == 0 or col > len(line_text)
                    or line_text[col - 1] not in self._lsp_trigger_chars):
                self.hide_popup()
                return
        seq = self._lsp_seq + 1
        self._lsp_seq = seq
        # Синхронизируем буфер с clangd: без этого запрос completion (180 мс)
        # уходит раньше отложенного didChange (400 мс), и clangd отвечает по
        # старому тексту — после медленного набора «a.» поля структуры не видны
        get_lsp_manager().notify_change(editor)
        get_lsp_manager().request_completion(
            editor, pos_to_lsp(editor, pos),
            lambda items: self._lsp_on_result(seq, pos, prefix, items),
        )

    def _lsp_on_result(self, seq: int, req_pos: int, prefix: str, items):
        """Ответ clangd: показывает список (или прячет, если устарел)."""
        if seq != self._lsp_seq or not self.lsp_enabled:
            return
        if self._include_context() is not None:
            return  # ответ устарел: каретка ушла в #include "…"
        editor = self.editor
        if editor is None or sip.isdeleted(editor):
            return
        lang = getattr(editor, "language_id", "") or ""
        items = items or []
        # Курсор ушёл далеко от места запроса — ответ устарел;
        # список не прячем (он обновится свежим запросом)
        if abs(editor.SendScintilla(SCI_GETCURRENTPOS) - req_pos) > 3:
            return
        # Пока летел ответ, слово-контекст могло разрушиться (пробел,
        # скобка) — показывать старый список нельзя
        if not self._current_prefix():
            line, col = editor.getCursorPosition()
            line_text = editor.text(line)
            if (col == 0 or col > len(line_text)
                    or line_text[col - 1] not in self._lsp_trigger_chars):
                self.hide_popup()
                return
        prefix_l = prefix.lower()
        rows: List[Tuple[str, int, str, str, dict, list, str]] = []
        docs: List[str] = []
        for item in items:
            # clangd в detailed-режиме шлёт label с ведущим пробелом
            label = (item.get("label") or "").lstrip()
            if not label:
                continue
            kind = _LSP_KIND_MAP.get(item.get("kind", 1), 9)
            if label.lower() in _LSP_TYPE_KEYWORDS:
                kind = 3  # int/char/void — это тип, а не ключевое слово
            doc = _plain_doc(item.get("documentation"))
            te = item.get("textEdit") or {}
            insert = _strip_snippet(
                te.get("newText") or item.get("insertText") or label
            )
            adds = item.get("additionalTextEdits") or []
            # Автоимпорт (доп. правка со строкой import/include): уважаем
            # настройку editor.auto_import.<язык> — если выключено, элемент
            # скрываем, чтобы import не добавлялся при приёме
            if adds and any(_is_import_stmt(e.get("newText") or "")
                           for e in adds):
                if not app_settings().get_bool(
                        "editor.auto_import." + lang, True):
                    continue
            detail = item.get("detail") or ""
            # basedpyright для auto-import пишет «autoImport» в label/detail,
            # а реальный импорт несёт в additionalTextEdits — показываем
            # вместо «autoImport» понятное название модуля
            if adds:
                mod, sym = _parse_import(adds[0].get("newText", ""))
                dl = detail.strip().lower()
                if mod and (dl in ("autoimport", "auto-import")
                            or not detail.strip()):
                    detail = mod
                if mod and label.strip().lower() in ("autoimport",
                                                      "auto-import"):
                    label = sym or label
            rows.append((label, kind, detail, insert, te, adds, doc))
        if not rows:
            self.hide_popup()
            return
        # Слова, начинающиеся с префикса, — первыми (иначе нужное слово
        # может оказаться ниже видимой области списка)
        if prefix_l:
            rows.sort(key=lambda r: (not r[0].lower().startswith(prefix_l),
                                     r[0].lower()))
        # Индексы строк попапа == индексы insert/textEdit (после сортировки)
        self._lsp_inserts = {}
        self._lsp_text_edits = {}
        self._lsp_adds = {}
        entries = []
        for i, (label, kind, detail, insert, te, adds, doc) in enumerate(rows):
            self._lsp_inserts[i] = insert
            if "range" in te:
                self._lsp_text_edits[i] = te
            if adds:
                self._lsp_adds[i] = adds
            docs.append(doc)
            entries.append((label, kind, detail))
        popup = self.popup
        popup.set_entries(entries, docs)
        self._position_popup()
        popup.show()
        popup.raise_()
        self._update_doc_panel()

    def _lsp_accept(self, item):
        """Вставляет выбранный элемент: по textEdit clangd или заменой префикса."""
        editor = self.editor
        if editor is None or sip.isdeleted(editor):
            return
        row = self.popup.row(item)
        insert = self._lsp_inserts.get(row)
        if not insert:
            return
        te = self._lsp_text_edits.get(row)
        if te:
            start = lsp_to_pos(editor, te["range"]["start"])
            end = lsp_to_pos(editor, te["range"]["end"])
            cur = editor.SendScintilla(SCI_GETCURRENTPOS)
            # clangd шлёт нулевой range (start==end==курсор):
            # заменяем слово под курсором, иначе будет stastatic_cast
            if start == end:
                start = self._word_start(cur)
                if start is None:
                    start = cur
                end = cur
            # range clangd зафиксирован на момент запроса; если после него
            # допечатали символы — расширяем замену до курсора, иначе
            # останется хвост (cout -> coutt)
            elif cur > end:
                end = cur
            editor.SendScintilla(SCI_SETSEL, start, end)
        else:
            pos = editor.SendScintilla(SCI_GETCURRENTPOS)
            start = self._word_start(pos)
            if start is None:
                start = pos
            editor.SendScintilla(SCI_SETSEL, start, pos)
        editor.SendScintilla(SCI_REPLACESEL, insert.encode("utf-8"))
        # Каретка после основной вставки — именно сюда нужно её вернуть:
        # доп. правки (import сверху файла) при вставке уведут каретку наверх
        caret_after_main = editor.SendScintilla(SCI_GETCURRENTPOS)
        # Доп. правки (обычно строка import сверху файла, auto-import
        # Python): основная вставка уже применена, а доп. правки выше
        # курсора, поэтому их позиции ещё валидны. Применяем снизу вверх,
        # чтобы вставки не сдвигали ещё не применённые правки
        adds = self._lsp_adds.get(row)
        shift = 0
        if adds:
            for edit in sorted(adds, key=_edit_start, reverse=True):
                new_text = edit.get("newText") or ""
                if _is_import_stmt(new_text):
                    # Автоимпорт всегда в самое начало файла, а не туда,
                    # куда сервер решил (после docstring/блока импортов)
                    ins = _file_top_for_import(editor)
                    editor.SendScintilla(SCI_SETSEL, ins, ins)
                else:
                    rng = edit.get("range") or {}
                    s = lsp_to_pos(editor, rng.get("start") or {})
                    epos = lsp_to_pos(editor, rng.get("end") or {})
                    ins = s
                    editor.SendScintilla(SCI_SETSEL, s, epos)
                editor.SendScintilla(SCI_REPLACESEL, new_text.encode("utf-8"))
                # правка, вставленная выше каретки, сдвигает символ (и каретку)
                # вниз на длину вставленного текста
                if ins < caret_after_main:
                    shift += len(new_text.encode("utf-8"))
        # Возвращаем каретку к месту вставки символа (с учётом сдвига
        # вставленным сверху import), чтобы Tab не уводил её наверх файла.
        # SCI_SETSEL(a, a) схлопывает выделение (ставит якорь = каретке),
        # иначе SCI_SETCURRENTPOS оставил бы якорь наверху и выделил весь
        # кусок от import до места вставки
        final = caret_after_main + shift
        editor.SendScintilla(SCI_SETSEL, final, final)

    def _insert_word(self, word: str, kind_id: int):
        """Заменяет префикс под курсором на слово; сниппет раскрывается."""
        editor = self.editor
        if editor is None or sip.isdeleted(editor):
            return
        pos = editor.SendScintilla(SCI_GETCURRENTPOS)
        inc = self._include_context()
        if inc is not None:
            # Файловое дополнение: заменяем часть пути после кавычки.
            # Папка — вставляем с "/" и сразу открываем следующий уровень.
            _prefix, start = inc
            editor.SendScintilla(SCI_SETSEL, start, pos)
            editor.SendScintilla(SCI_REPLACESEL, word.encode("utf-8"))
            if kind_id == _FOLDER_KIND_ID and word.endswith("/"):
                QTimer.singleShot(0, self._update_popup)
            return
        start = self._word_start(pos)
        if start is None:
            return
        editor.SendScintilla(SCI_SETSEL, start, pos)
        editor.SendScintilla(SCI_REPLACESEL, word.encode("utf-8"))
        if kind_id == SNIPPET_KIND_ID:
            QTimer.singleShot(0, lambda: self._snippet.expand(word, editor))

    # ------------------------------------------------------------------
    # Сниппеты: раскрытие шаблона и навигация по плейсхолдерам
    # (механика — в SnippetSession, см. editor/snippets.py)
    # ------------------------------------------------------------------

    def handle_tab(self, backward: bool = False) -> bool:
        """Tab/Shift+Tab: переход к следующему/предыдущему плейсхолдеру.

        True, если клавиша обработана сниппетом (символ таба вставлять
        не нужно). После последнего плейсхолдера — финальная позиция $0.
        """
        if not self.enabled or not self._snippet.active:
            return False
        if self.popup.isVisible():
            return False  # список сам обрабатывает Tab
        return self._snippet.handle_tab(self.editor, backward)

    def clear_placeholders(self):
        """Завершает сеанс сниппета: снимает подсветку и сбрасывает позиции."""
        self._snippet.clear(self.editor)

    def _on_modified(self, pos: int, mod_type: int, text, length: int, *_):
        """Правка текста: сдвигает позиции плейсхолдеров сниппета."""
        self._snippet.on_modified(self.editor, pos, mod_type, text, length)

    def _on_cursor_moved(self, *_):
        """Курсор сдвинут (клик/стрелки) — список следует за ним и
        перефильтровывается; если курсор ушёл за пределы плейсхолдеров,
        сеанс сниппета завершён.

        QsciScintilla шлёт cursorPositionChanged отложенно (singleShot),
        поэтому прятать список здесь нельзя — он закроет только что
        показанный в after_edit попап.
        """
        if self.popup.isVisible():
            if (self.lsp_enabled and not self.enabled
                    and self._include_context() is None):
                self._lsp_timer.start(_LSP_DEBOUNCE_MS)
            else:
                self._update_popup()
        editor = self.editor
        if editor is None or sip.isdeleted(editor):
            self._snippet.clear(editor)
            return
        self._snippet.on_cursor_moved(
            editor, editor.SendScintilla(SCI_GETCURRENTPOS)
        )

    # ------------------------------------------------------------------
    # События от CodeEditor
    # ------------------------------------------------------------------

    def should_refresh(self, event) -> bool:
        """Нужно ли проверять список после этого нажатия клавиши."""
        if not (self.enabled or self.lsp_enabled):
            return False
        if event.key() in (Qt.Key.Key_Backspace, Qt.Key.Key_Delete):
            return True
        text = event.text()
        if len(text) != 1:
            return False
        # В кавычках #include "…" проверяем список на каждый символ пути
        # (/, ., \ и т.д. не входят в слово языка)
        if self._include_context() is not None:
            return True
        if self.enabled:
            return text in _WORD_CHARS
        return text in _WORD_CHARS or text in self._lsp_trigger_chars

    def after_edit(self):
        """После вставки текста обновляет кастомный список автодополнения."""
        if not (self.enabled or self.lsp_enabled):
            return
        editor = self.editor
        if editor is None or sip.isdeleted(editor):
            return
        if (self.lsp_enabled and not self.enabled
                and self._include_context() is None):
            self._lsp_timer.start(_LSP_DEBOUNCE_MS)
            return
        self._update_popup()

    def _update_popup(self):
        """Показывает список под курсором или прячет его.

        Список виден, только если курсор внутри слова и не в
        комментарии/строке.
        """
        editor = self.editor
        if editor is None or sip.isdeleted(editor):
            return
        pos = editor.SendScintilla(SCI_GETCURRENTPOS)
        self._refresh_line_styles(pos)
        inc = self._include_context()
        if inc is not None:
            # В #include "…": список файлов/папок по части пути.
            # Строка с кавычками отмечена стилем строки — проверку
            # _style_blocked пропускаем намеренно.
            prefix, _start = inc
            entries = self._include_candidates(prefix)
            if not entries:
                self.hide_popup()
                return
            popup = self.popup
            popup.set_entries(entries)
            self._position_popup()
            popup.show()
            popup.raise_()
            self._update_doc_panel()
            return
        if self._style_blocked(pos):
            self.hide_popup()
            return
        prefix = self._current_prefix()
        if not prefix:
            self.hide_popup()
            return
        entries = self._candidates(prefix)
        if not entries:
            self.hide_popup()
            return
        popup = self.popup
        popup.set_entries(entries)
        self._position_popup()
        popup.show()
        popup.raise_()
        self._update_doc_panel()

    # ------------------------------------------------------------------
    # Логика
    # ------------------------------------------------------------------

    def _rebuild_from_text(self):
        """Собирает динамические слова (let/fn/struct/...) из текущего текста."""
        if not self.enabled:
            return
        lexer = self.editor._current_lexer
        if not isinstance(lexer, LumenLangLexer):
            return
        self._dynamic_words = self._collect_identifiers_from_text(
            self.editor.text()
        )

    @staticmethod
    def _collect_identifiers_from_text(text: str) -> Dict[str, int]:
        """{имя: категория} для слов после let/namespace/fn/#define/...

        Всё, что находится в //-комментарии, не учитывается.
        """
        identifiers: Dict[str, int] = {}
        for line in text.split("\n"):
            if "//" in line:
                line = line[: line.index("//")]
            for pattern, kind_id in _DYNAMIC_PATTERNS:
                for match in re.finditer(pattern, line):
                    name = match.group(1)
                    if name:
                        identifiers[name] = kind_id
        return identifiers

    def _style_blocked(self, pos: int) -> bool:
        """True, если курсор в комментарии/строке — там не подсказываем.

        Смотрим на символ слева от курсора (только что напечатанный), а не
        на позицию курсора: сразу после ввода курсор стоит на следующем
        символе (часто это перевод строки со стилем по умолчанию).
        """
        editor = self.editor
        lexer = editor._current_lexer
        if isinstance(lexer, LumenLangLexer):
            non_code = _NON_CODE_STYLES
        elif isinstance(lexer, AsmLexer):
            non_code = {AsmLexer.STYLE_COMMENT, AsmLexer.STYLE_COMMENTBLOCK,
                        AsmLexer.STYLE_STRING}
        elif isinstance(lexer, LLVMLexer):
            non_code = {LLVMLexer.STYLE_COMMENT, LLVMLexer.STYLE_STRING}
        else:
            return False
        for check_pos in (pos - 1, pos):
            if check_pos < 0:
                continue
            style = editor.SendScintilla(SCI_GETSTYLEAT, check_pos)
            if style in non_code:
                return True
        return False

    def _refresh_line_styles(self, pos: int):
        """Перестилизовывает текущую строку, чтобы проверка стилей была свежей.

        Scintilla стилизует текст лениво (по требованию отрисовки), поэтому
        только что набранные символы ещё не имеют стилей; без этого проверка
        «курсор в комментарии/строке» видела бы устаревшие стили (0).
        """
        editor = self.editor
        if not isinstance(editor._current_lexer, (LumenLangLexer, AsmLexer,
                                                  LLVMLexer)):
            return
        line = editor.SendScintilla(SCI_LINEFROMPOSITION, pos)
        line_start = editor.SendScintilla(SCI_POSITIONFROMLINE, line)
        line_end = editor.SendScintilla(SCI_GETLINEENDPOSITION, line)
        if line_end > line_start:
            editor.SendScintilla(SCI_COLOURISE, line_start, line_end)
