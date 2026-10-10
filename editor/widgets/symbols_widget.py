"""
SymbolsWidget — вкладка со списком символов (функции, переменные, структуры,
enums, namespace'ы) активного редактора.

Список строится из LSP textDocument/documentSymbol (дерево DocumentSymbol[],
см. normalize_symbol_tree); расскрывается в плоский список с отступами по
глубине. Одиночный клик — переход к редактору и началу символа (вспышка
токена, как в списке диагностик). Обновляется по таймеру, пока вкладка видима
(сервер пересобирает дерево после каждого didChange).
"""

import html
import os

from PyQt6.QtCore import Qt, QEvent, QPoint, QSize, QTimer
from PyQt6.QtGui import (
    QAbstractTextDocumentLayout, QColor, QFont, QFontMetrics, QPalette,
    QPainter, QTextDocument, QTextOption,
)
from PyQt6.QtWidgets import (
    QAbstractItemView, QApplication, QHeaderView, QHBoxLayout, QLabel,
    QStyledItemDelegate, QStyle, QStyleOptionViewItem, QTableWidget,
    QTableWidgetItem, QVBoxLayout, QWidget,
)

from editor.font_settings import code_font_family
from editor.lsp import get_lsp_manager
from editor.lsp.protocol import lsp_to_pos, uri_to_path
from editor.scrollbar_qss import scrollbar_qss
from editor.theme_utils import ThemeColors
from editor.widgets.animated_hover import mix_colors

try:
    from editor.widgets.hover_popup import lumen_token_spans
except Exception:
    lumen_token_spans = None  # без Qsci — ячейки обычным текстом

_REFRESH_MS = 800          # период автообновления, пока вкладка видима
_SYMBOL_NAV_ROLE = Qt.ItemDataRole.UserRole  # (editor, byte_pos, start, file_uri)
_PLAIN_ROLE = Qt.ItemDataRole.UserRole + 1  # текст без разметки (для элизии)
_DEPTH_ROLE = Qt.ItemDataRole.UserRole + 2  # глубина вложенности
  
_CELL_PAD = 6             # горизонтальный отступ текста в ячейке, px

# Стиль токена Lumen -> (ключ стиля темы, запасной цвет); None-цвет — из темы
_SPAN_STYLE = {
    "kw": ("keyword", "#c678dd"),
    "mod": ("modifier", "#c678dd"),
    "type": ("type", "#61afef"),
    "ns": ("class_name", "#61afef"),
    "lit": ("literal", "#94e2d5"),
    "dir": ("directive", "#f392a8"),
    "obj": ("variable", None),
    "var": ("variable", None),
    "func": ("function_name", None),
    "num": ("number", "#d19a66"),
    "str": ("string", "#98c379"),
    "com": ("comment", None),
    "punct": ("punct", None),
}


class _SymbolDelegate(QStyledItemDelegate):
    """Рисует текст ячейки как HTML: QTableWidget в Qt6 сам HTML не рендерит.

    В ячейке хранится plain-текст подписи (+ глубина); здесь его обрезаем
    многоточием по ширине колонки (без переноса), затем красим токены
    цветными span'ами через QTextDocument. Простые колонки (номер строки)
    рисуются обычным текстом с элизией."""

    def __init__(self, widget, parent=None):
        super().__init__(parent)
        self._widget = widget
        self.fg = "#cdd6f4"

    def _elided_plain(self, opt: QStyleOptionViewItem, index) -> str:
        plain = index.data(_PLAIN_ROLE)
        if plain is None:
            return ""
        avail = max(10, opt.rect.width() - _CELL_PAD)
        return QFontMetrics(opt.font).elidedText(
            plain, Qt.TextElideMode.ElideRight, avail)

    def paint(self, painter: QPainter, option: QStyleOptionViewItem, index):
        opt = QStyleOptionViewItem(option)
        self.initStyleOption(opt, index)
        painter.save()
        painter.setClipRect(opt.rect)
        if ((opt.state & QStyle.StateFlag.State_MouseOver)
                or index.row() == self._widget._hover_row):
            painter.fillRect(opt.rect, self._widget._hover_bg)
        plain = index.data(_PLAIN_ROLE)
        if plain is None:
            # колонка без подписей (номер строки) — рисуем напрямую
            t = index.data(Qt.ItemDataRole.DisplayRole) or ""
            fm = QFontMetrics(opt.font)
            elided = fm.elidedText(
                t, Qt.TextElideMode.ElideRight, opt.rect.width() - _CELL_PAD)
            painter.setPen(QColor(self.fg))
            y = opt.rect.top() + (opt.rect.height() - fm.height()) / 2 \
                + fm.ascent()
            painter.drawText(
                int(opt.rect.left() + _CELL_PAD), int(y), elided)
            painter.restore()
            return
        depth = int(index.data(_DEPTH_ROLE) or 0)
        html = self._widget._detail_html(
            self._elided_plain(opt, index), depth)
        doc = QTextDocument()
        doc.setDocumentMargin(0)
        doc.setDefaultFont(opt.font)
        tw = QTextOption()
        tw.setWrapMode(QTextOption.WrapMode.NoWrap)
        doc.setDefaultTextOption(tw)
        doc.setTextWidth(opt.rect.width())
        doc.setHtml(html)
        y = opt.rect.top() + (opt.rect.height() - doc.size().height()) / 2
        painter.translate(
            QPoint(int(opt.rect.left() + _CELL_PAD), int(max(y, 0))))
        ctx = QAbstractTextDocumentLayout.PaintContext()
        ctx.palette.setColor(QPalette.ColorRole.Text, QColor(self.fg))
        doc.documentLayout().draw(painter, ctx)
        painter.restore()

    def sizeHint(self, option, index):
        fm = QFontMetrics(option.font)
        return QSize(fm.horizontalAdvance(index.data(
            Qt.ItemDataRole.DisplayRole) or "") + 12, fm.height() + 4)


class SymbolsWidget(QWidget):
    """Список символов активного редактора с навигацией по клику."""

    def __init__(self, tile_manager, theme_manager):
        super().__init__()
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self._tm = tile_manager
        self._theme_manager = theme_manager
        self._theme_name = ""
        self._fg = "#cdd6f4"
        self._muted = "#6c7086"
        self._accent = "#56b6c2"
        self._req = 0            # номер последнего запроса дерева символов

        self._counts = QLabel()
        self._counts.setFont(QFont(code_font_family(), 10))

        self._table = QTableWidget(0, 1)
        self._table.horizontalHeader().setVisible(False)
        self._table.setEditTriggers(
            QAbstractItemView.EditTrigger.NoEditTriggers)
        self._table.setSelectionMode(
            QAbstractItemView.SelectionMode.NoSelection)
        self._table.setShowGrid(False)
        self._table.setFont(QFont(code_font_family(), 10))
        self._table.verticalHeader().setDefaultSectionSize(
            QFontMetrics(self._table.font()).height() + 4
        )
        self._table.verticalHeader().setVisible(False)
        self._table.horizontalHeader().setSectionResizeMode(
            0, QHeaderView.ResizeMode.Stretch)
        self._hover_bg = QColor("#26303f")
        self._hover_row = -1
        self._table.setMouseTracking(True)
        self._table.viewport().setMouseTracking(True)
        self._table.viewport().installEventFilter(self)
        self._delegate = _SymbolDelegate(self, self._table)
        self._table.setItemDelegate(self._delegate)
        self._table.cellClicked.connect(self._on_activated)

        header = QHBoxLayout()
        header.addWidget(self._counts)
        header.addStretch(1)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 8)
        layout.setSpacing(6)
        layout.addLayout(header)
        layout.addWidget(self._table, 1)

        self._timer = QTimer(self)
        self._timer.setInterval(_REFRESH_MS)
        self._timer.timeout.connect(self.refresh)

    # --- тема ---

    def apply_theme(self, theme_name: str):
        """Перекрашивает список в цвета темы и пересобирает строки."""
        self._theme_name = theme_name
        tc = ThemeColors.from_theme_manager(self._theme_manager, theme_name)
        self._fg = tc.fg
        self._muted = tc.muted
        self._accent = self._accent_color()
        self._styles = self._theme_manager.themes.get(
            self._theme_name, {}).get("styles", {})
        self._hover_bg = self._hover_color(tc)
        self._delegate.fg = self._fg
        self.setStyleSheet(f"""
            SymbolsWidget {{
                background: {tc.editor_bg_color.name()};
            }}
            QTableWidget {{
                background: {tc.editor_bg_color.name()};
                color: {self._fg};
                border: none;
                border-radius: 4px;
                gridline-color: transparent;
                outline: none;
            }}
            QHeaderView::section {{
                background: {tc.bg};
                color: {self._muted};
                border: none;
                padding: 3px 6px;
                font-weight: bold;
            }}
            QTableWidget::item {{ padding: 0px 6px; }}
            QTableWidget::item:hover {{ background: transparent; }}
        """ + scrollbar_qss(tc.editor_bg_color.name(), QColor(self._fg)))
        self.refresh()

    def _hover_color(self, tc) -> QColor:
        """Цвет подсветки строки при наведении: selection темы, приглушённый
        через подмешивание к фону редактора (опак-селекшены не заливают текст)."""
        bg = tc.editor_bg_color
        sel = tc.selection
        if not sel:
            return mix_colors(bg, bg.lighter(115) if tc.dark else bg.darker(112), 1.0)
        sel_c = QColor(sel)
        if sel_c.alpha() < 255:
            return sel_c
        return mix_colors(bg, sel_c, 0.22)

    def _accent_color(self) -> str:
        """Акцентный цвет (имя функции) из стилей темы, иначе fg."""
        theme = self._theme_manager.themes.get(self._theme_name, {})
        st = theme.get("styles", {}).get("function_name") or {}
        return st.get("color") or self._fg

    def _span_color(self, style: str) -> str:
        """Цвет токена подсветки: стиль темы (как в hover), иначе запасной."""
        cfg = _SPAN_STYLE.get(style)
        if not cfg:
            return self._fg
        key, fallback = cfg
        st = self._styles.get(key) or {}
        color = st.get("color")
        if color:
            return color
        if fallback:
            return fallback
        return {"obj": self._fg, "var": self._fg,
                "func": self._accent, "com": self._muted,
                "punct": self._muted}.get(style, self._fg)

    def _detail_html(self, text: str, depth: int = 0) -> str:
        """Подпись символа -> HTML: Lumen-токены, цвета как в hover-попапе.

        Строка начинается с `<span>`: QTableWidgetItem рендерит HTML только
        если текст начинается с тега (отступы `&nbsp;` детекцию сломали бы).
        """
        pad = "&nbsp;&nbsp;" * depth
        if lumen_token_spans is None:
            return f"<span>{pad}{html.escape(text)}</span>"
        parts = ["<span>", pad]
        for style, tok in lumen_token_spans(text):
            esc = html.escape(tok).replace(" ", "&nbsp;")
            if style == "fg":
                parts.append(esc)
            else:
                parts.append(
                    f'<span style="color:{self._span_color(style)};">{esc}</span>'
                )
        parts.append("</span>")
        return "".join(parts)

    # --- данные ---

    def _active_editor(self):
        """Активный редактор приложения: сперва из активного окна, иначе
        первый редактор, у которого открыта текущая вкладка.

        Список символов следует за активным файлом, как Outline в IDE: если
        активен другой таб (терминал и т.п.), показываем символы последнего
        активного редактора (ed выставляется в refresh на каждый тик).
        """
        return self._resolve_active_editor()

    def _resolve_active_editor(self):
        for tl in QApplication.topLevelWidgets():
            tm = getattr(tl, "tile_manager", None)
            if tm is None:
                continue
            try:
                if tl.isActiveWindow():
                    pane = tm.get_active_pane()
                    if pane is not None:
                        ed = pane.current_editor()
                        if ed is not None:
                            return ed
            except RuntimeError:
                continue
        for tl in QApplication.topLevelWidgets():
            tm = getattr(tl, "tile_manager", None)
            if tm is None:
                continue
            try:
                for pane in tm.panes():
                    ed = pane.current_editor()
                    if ed is not None:
                        return ed
            except RuntimeError:
                continue
        return None

    def _flatten(self, tree: list, depth: int = 0):
        """Дерево нормированных символов -> плоский список кортежей.

        Кортеж: (name, detail, kind, start, file, depth). file — uri файла
        для символов из #include (пусто = символ в текущем файле).
        """
        out = []
        for node in tree:
            name = node.get("name", "")
            if not name:
                continue
            out.append((name, node.get("detail") or "",
                        node.get("kind", 0), node.get("start"),
                        node.get("file") or "", depth))
            out.extend(self._flatten(node.get("children") or [], depth + 1))
        return out

    def refresh(self):
        """Запрашивает дерево символов активного редактора и обновляет список."""
        editor = self._active_editor()
        if editor is None:
            self._table.setUpdatesEnabled(False)
            self._table.setRowCount(0)
            self._table.setUpdatesEnabled(True)
            self._set_counts(0, "")
            return
        fp = getattr(editor, "file_path", None) or ""
        bridge = getattr(editor, "_lsp_bridge", None)
        attached = bridge is not None and bridge.attached
        if not fp or not attached:
            self._table.setUpdatesEnabled(False)
            self._table.setRowCount(0)
            self._table.setUpdatesEnabled(True)
            self._set_counts(0, os.path.basename(fp) if fp else "Untitled")
            return
        self._req += 1
        rid = self._req
        try:
            get_lsp_manager().request_symbol_tree(
                editor, lambda t: self._on_tree(rid, editor, t))
        except Exception:
            if rid == self._req:
                self._set_counts(0, os.path.basename(fp))

    def _on_tree(self, rid, editor, tree):
        # Ответ от устаревшего запроса (редактор сменили) — отбрасываем
        if rid != self._req:
            return
        if editor is None:
            return
        try:
            editor.objectName()
        except RuntimeError:
            return  # редактор уже уничтожен
        rows = []
        for name, detail, kind, start, file, depth in self._flatten(tree or []):
            rows.append((name, detail, kind, start, file, depth))
        fp = getattr(editor, "file_path", None) or ""

        self._table.setUpdatesEnabled(False)
        self._table.setRowCount(len(rows))
        for i, (name, detail, kind, start, file, depth) in enumerate(rows):
            text = detail or name
            display = text.replace("\n", " ")
            nav = QTableWidgetItem(display)
            nav.setData(_PLAIN_ROLE, display)
            nav.setData(_DEPTH_ROLE, depth)
            nav.setToolTip(text + self._kind_suffix(name, start))
            pos = 0
            if start and not file:
                try:
                    pos = lsp_to_pos(editor, {"line": start[0],
                                              "character": start[1]})
                except Exception:
                    pos = 0
            nav.setData(_SYMBOL_NAV_ROLE, (editor, pos, start, file))
            if depth:
                f = nav.font()
                f.setBold(False)
                nav.setFont(f)
            self._table.setItem(i, 0, nav)
        self._table.setUpdatesEnabled(True)
        self._set_counts(len(rows), os.path.basename(fp) if fp else "")

    def _kind_suffix(self, name: str, start) -> str:
        if not start:
            return ""
        return f"  (line {start[0] + 1})"

    def _set_counts(self, n: int, file_name: str):
        if n == 0:
            self._counts.setText(
                f'<span style="color:{self._muted};">'
                f'No symbols{(" in " + file_name) if file_name else ""}</span>'
            )
            return
        text = f'{file_name} · {n}'
        if len(text) > 80:
            text = text[:77] + "..."
        self._counts.setText(
            f'<span style="color:{self._fg};">Symbols: </span>'
            f'<span style="color:{self._muted};font-weight:bold;">{text}</span>'
        )

    # --- навигация ---

    def eventFilter(self, obj, event):
        """Отслеживает наведение мыши на строки таблицы (viewport).

        State_MouseOver в опционах делегата недетерминирован (особенно без
        живого курсора/стилей) — ховер ведём явно по mouse-move/leave.
        """
        if obj is self._table.viewport():
            et = event.type()
            if et == QEvent.Type.MouseMove:
                row = self._table.rowAt(int(event.position().y()))
                if row != self._hover_row:
                    self._hover_row = row
                    if row >= 0:
                        self._table.viewport().setCursor(
                            Qt.CursorShape.PointingHandCursor)
                    else:
                        self._table.viewport().unsetCursor()
                    self._table.viewport().update()
            elif et == QEvent.Type.Leave:
                if self._hover_row != -1:
                    self._hover_row = -1
                    self._table.viewport().unsetCursor()
                    self._table.viewport().update()
        return super().eventFilter(obj, event)

    def _on_activated(self, row: int, col: int):
        """Клик: переход к редактору и позиции символа.

        Символ из другого файла (#include) сначала открывает этот файл
        в активной панели и переходит на его позицию.
        """
        item = self._table.item(row, 0)
        if item is None:
            return
        data = item.data(_SYMBOL_NAV_ROLE)
        if not data:
            return
        editor, pos, start, file = data
        try:
            editor.objectName()
        except RuntimeError:
            return  # редактор уже уничтожен
        target = self._open_symbol_editor(editor, file, start)
        if target is None:
            return
        if self._activate(target):
            try:
                target.lsp_reveal(pos if target is editor else
                                  self._foreign_pos(target, start))
            except RuntimeError:
                pass

    def _open_symbol_editor(self, editor, file: str, start):
        """Редактор цели символа: текущий, либо открытый файл file (uri)."""
        if not file:
            return editor
        try:
            path = uri_to_path(file)
            if not os.path.isabs(path):
                path = os.path.abspath(path)
            pane = self._active_pane()
            if pane is None:
                return None
            return pane.add_editor(file_path=path)
        except (RuntimeError, OSError):
            return None

    def _foreign_pos(self, editor, start) -> int:
        """"Позиция символа чужого файла в открытом редакторе editor."""
        if not start:
            return 0
        try:
            return lsp_to_pos(editor, {"line": start[0], "character": start[1]})
        except Exception:
            return 0

    def _active_pane(self):
        """Активная (или первая) панель любого окна приложения."""
        for tm in self._all_tile_managers():
            try:
                pane = tm.get_active_pane()
            except RuntimeError:
                continue
            if pane is not None:
                return pane
        for tm in self._all_tile_managers():
            try:
                panes = tm.panes()
            except RuntimeError:
                continue
            for pane in panes:
                if pane is not None:
                    return pane
        return None

    def _activate(self, editor) -> bool:
        """Находит панель с редактором в любом окне и делает вкладку активной."""
        panes = []
        for tm in self._all_tile_managers():
            try:
                panes.extend(tm.panes())
            except RuntimeError:
                continue
        for pane in panes:
            try:
                if pane.activate_editor(editor):
                    return True
            except RuntimeError:
                continue
        return False

    def _all_tile_managers(self):
        seen = set()
        for tl in QApplication.topLevelWidgets():
            tm = getattr(tl, "tile_manager", None)
            if tm is not None and id(tm) not in seen:
                seen.add(id(tm))
                yield tm

    # --- видимость: автообновление ---

    def showEvent(self, event):
        super().showEvent(event)
        self.refresh()
        self._timer.start()

    def hideEvent(self, event):
        super().hideEvent(event)
        self._timer.stop()