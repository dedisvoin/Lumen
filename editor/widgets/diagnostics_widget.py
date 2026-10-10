"""
DiagnosticsWidget — вкладка со списком диагностик всех открытых редакторов.

Колонки: тип (ошибка/предупреждение), текст сообщения, файл:строка и
LSP-сервер, который вернул диагностику. Двойной клик — переход к редактору
и позиции с вспышкой токена. Список обновляется по таймеру, пока вкладка
видима (диагностики приходят асинхронно от серверов).
"""

import os
from typing import List, Tuple

from PyQt6.QtCore import Qt, QTimer, QRectF, QSize, pyqtSignal
from PyQt6.QtGui import QColor, QFont, QFontMetrics, QPainter
from PyQt6.QtWidgets import (
    QAbstractItemView, QApplication, QHeaderView, QHBoxLayout, QLabel,
    QTableWidget, QTableWidgetItem, QToolButton, QVBoxLayout, QWidget,
)

from editor.font_settings import code_font_family
from editor.lsp.manager import get_lsp_manager
from editor.scrollbar_qss import scrollbar_qss
from editor.theme_utils import ThemeColors
from editor.widgets.animated_hover import HoverFade
from editor.widgets.file_icons import load_ui_icon

_REFRESH_MS = 800          # период автообновления, пока вкладка видима
_EDITOR_POS_ROLE = Qt.ItemDataRole.UserRole  # (editor, pos) в строке


class _WarnToggleButton(QToolButton):
    """Кнопка-тогл варнингов: ховер-подсветка плавная (как у кнопки дерева)."""

    _HOVER_INSET = 0
    _HOVER_RADIUS = 8

    def __init__(self, parent=None):
        super().__init__(parent)
        self._hover_color = QColor(255, 255, 255, 20)
        self._fade = HoverFade(self)
        self.setAttribute(Qt.WidgetAttribute.WA_Hover)

    def set_hover_color(self, color: QColor):
        """Цвет подсветки при наведении — из темы."""
        self._hover_color = QColor(color)
        self.update()

    def enterEvent(self, event):
        self._fade.fade_in()
        super().enterEvent(event)

    def leaveEvent(self, event):
        self._fade.fade_out()
        super().leaveEvent(event)

    def paintEvent(self, event):
        c = QColor(self._hover_color)
        c.setAlpha(round(c.alpha() * self._fade.progress()))
        if c.alpha() > 0:
            p = QPainter(self)
            p.setRenderHint(QPainter.RenderHint.Antialiasing)
            rect = QRectF(self.rect()).adjusted(
                self._HOVER_INSET, self._HOVER_INSET,
                -self._HOVER_INSET, -self._HOVER_INSET,
            )
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(c)
            p.drawRoundedRect(rect, self._HOVER_RADIUS, self._HOVER_RADIUS)
            p.end()
        super().paintEvent(event)


class DiagnosticsWidget(QWidget):
    """Список диагностик (E/W) всех открытых редакторов с навигацией."""

    warn_visibility_changed = pyqtSignal(bool)

    def __init__(self, tile_manager, theme_manager):
        super().__init__()
        # Без WA_StyledBackground QSS-фон обычного QWidget не рисуется
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self._tm = tile_manager
        self._theme_manager = theme_manager
        self._theme_name = ""
        self._fg = "#cdd6f4"
        self._muted = "#6c7086"
        self._accent = "#56b6c2"
        self._error = "#f38ba8"
        self._warning = "#f9e2af"
        self._warn_on_color = QColor(self._warning)
        self._warn_off_color = QColor(self._muted)
        self._show_warnings = True

        self._counts = QLabel()
        self._counts.setFont(QFont(code_font_family(), 10))

        # Тогл показа варнингов: справа сверху, как кнопка дерева (ховер)
        self._warn_btn = _WarnToggleButton(self)
        self._warn_btn.setCheckable(True)
        self._warn_btn.setChecked(True)
        self._warn_btn.setIconSize(QSize(16, 16))
        # +4px к ховеру: кнопка (и подсветка) на 4px больше авторазмера 19x19
        self._warn_btn.setFixedSize(QSize(23, 23))
        self._warn_btn.setToolTip("Show Warnings")
        self._warn_btn.toggled.connect(self._on_warn_toggled)
        self._warn_btn.setStyleSheet(
            "QToolButton { border: none; background: transparent; }"
        )
        self._table = QTableWidget(0, 4)
        self._table.horizontalHeader().setVisible(False)
        self._table.setEditTriggers(
            QAbstractItemView.EditTrigger.NoEditTriggers)
        self._table.setSelectionMode(
            QAbstractItemView.SelectionMode.NoSelection)
        self._table.setShowGrid(False)
        self._table.setFont(QFont(code_font_family(), 10))
        # Фиксированная высота строки: плотнее списка без автоподбора
        self._table.verticalHeader().setDefaultSectionSize(
            QFontMetrics(self._table.font()).height() + 4
        )
        self._table.verticalHeader().setVisible(False)
        self._table.horizontalHeader().setSectionResizeMode(
            1, QHeaderView.ResizeMode.Stretch)
        self._table.setColumnWidth(0, 46)
        self._table.setColumnWidth(2, 170)
        self._table.setColumnWidth(3, 110)
        self._table.cellDoubleClicked.connect(self._on_activated)

        header = QHBoxLayout()
        header.addWidget(self._counts)
        header.addStretch(1)
        header.addWidget(self._warn_btn)

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
        self._error = tc.error or "#f38ba8"
        self._warning = tc.warning or "#f9e2af"
        self.setStyleSheet(f"""
            DiagnosticsWidget {{
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
        # Кнопка варнингов: активна — цветом варнинга, выключена — приглушённой
        hover = QColor(self._fg)
        hover.setAlpha(20)
        self._warn_btn.set_hover_color(hover)
        self._warn_on_color = QColor(self._warning)
        self._warn_off_color = QColor(self._muted)
        self._update_warn_icon()
        self.refresh()

    def _accent_color(self) -> str:
        """Акцентный цвет (имя функции) из стилей темы, иначе fg."""
        theme = self._theme_manager.themes.get(self._theme_name, {})
        st = theme.get("styles", {}).get("function_name") or {}
        return st.get("color") or self._fg

    # --- данные ---

    def _all_panes(self):
        """Все панели всех окон приложения (главное + отдельные окна
        перетащенных вкладок), чтобы диагностики из любого окна попадали
        в единый список.

        Иначе редактор, перенесённый в отдельное окно (собственный
        TileManager), исчезал бы из поля зрения списка диагностик главного
        окна, и его ошибки переставали отображаться.
        """
        panes = []
        for tl in QApplication.topLevelWidgets():
            tm = getattr(tl, "tile_manager", None)
            if tm is None:
                continue
            try:
                panes.extend(tm.panes())
            except RuntimeError:
                continue
        return panes

    def refresh(self):
        """Пересобирает таблицу из диагностик всех открытых редакторов."""
        rows: List[Tuple] = []
        for pane in self._all_panes():
            for editor in pane.get_all_editors():
                file_path = editor.file_path or ""
                server = self._server_name(editor)
                for line, start, end, severity, message in editor.lsp_diags():
                    rows.append((severity, message, file_path,
                                 line, start, editor, server))
        if not self._show_warnings:
            rows = [r for r in rows if r[0] <= 1]  # severity 1 и ниже — ошибки
        errs = sum(1 for r in rows if r[0] <= 1)
        warns = len(rows) - errs

        self._table.setUpdatesEnabled(False)
        self._table.setRowCount(len(rows))
        for i, (severity, message, file_path, line, start,
                editor, server) in enumerate(rows):
            is_err = severity <= 1
            color = self._error if is_err else self._warning
            tag = "E" if is_err else "W"
            name = os.path.basename(file_path) if file_path else "Untitled"
            loc = f"{name}:{line + 1}"
            tip = f"{file_path}:{line + 1}" if file_path else loc
            values = (
                (tag, color, True),
                (message, self._fg, False),
                (loc, self._muted, False),
                (server, self._accent, False),
            )
            for col, (text, fg, bold) in enumerate(values):
                item = QTableWidgetItem(text)
                item.setForeground(QColor(fg))
                if col == 0 or col == 3:
                    # Тип E/W и имя LSP — по центру колонки
                    item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                if bold:
                    f = item.font()
                    f.setBold(True)
                    item.setFont(f)
                if col == 1:
                    # Полный текст ошибки (в ячейке может обрезаться)
                    item.setToolTip(message)
                if col == 2:
                    item.setToolTip(tip)
                if col == 0:
                    item.setData(_EDITOR_POS_ROLE, (editor, start))
                self._table.setItem(i, col, item)
        self._table.setUpdatesEnabled(True)
        self._set_counts(errs, warns)

    def set_show_warnings(self, checked: bool):
        """Задаёт состояние тогла извне (панель сохраняет его между вкладками)."""
        self._show_warnings = checked
        self._warn_btn.setChecked(checked)

    def _on_warn_toggled(self, checked: bool):
        """Тогл варнингов: обновить иконку, список и сообщить панели."""
        self._show_warnings = checked
        self._update_warn_icon()
        self.refresh()
        self.warn_visibility_changed.emit(checked)

    def _update_warn_icon(self):
        """Иконка кнопки: активна — цветом варнинга, выключена — приглушённой."""
        if self._warn_btn.isChecked():
            color, alpha = self._warn_on_color, 255
        else:
            color, alpha = self._warn_off_color, 120
        self._warn_btn.setIcon(load_ui_icon("warning-bold", color, alpha))

    def _server_name(self, editor) -> str:
        """Имя LSP-сервера языка редактора (basedpyright, clangd...)."""
        cfg = get_lsp_manager().config_for(editor.language_id)
        if cfg is not None and cfg.server_name:
            return cfg.server_name
        return editor.language_id or ""

    def _set_counts(self, errs: int, warns: int):
        if errs == 0 and warns == 0:
            self._counts.setText(
                f'<span style="color:{self._muted};">'
                f'No problems in workspace</span>'
            )
            return
        self._counts.setText(
            f'<span style="color:{self._fg};">Errors: </span>'
            f'<span style="color:{self._error};font-weight:bold;">{errs}</span>'
            f'<span style="color:{self._fg};">&nbsp;&nbsp;Warnings: </span>'
            f'<span style="color:{self._warning};font-weight:bold;">{warns}</span>'
        )

    # --- навигация ---

    def _on_activated(self, row: int, col: int):
        """Двойной клик: переход к редактору и позиции диагностики."""
        item = self._table.item(row, 0)
        if item is None:
            return
        data = item.data(_EDITOR_POS_ROLE)
        if not data:
            return
        editor, pos = data
        try:
            editor.objectName()
        except RuntimeError:
            return  # редактор уже уничтожен
        for pane in self._tm.panes():
            if pane.activate_editor(editor):
                try:
                    editor.lsp_reveal(pos)
                except RuntimeError:
                    pass
                break

    # --- видимость: автообновление ---

    def showEvent(self, event):
        super().showEvent(event)
        self.refresh()
        self._timer.start()

    def hideEvent(self, event):
        super().hideEvent(event)
        self._timer.stop()
