"""
DraggableTabBar — QTabBar с поддержкой перетаскивания вкладок между панелями
и кастомными закрывающими кнопками (X), которые темизируются.

Содержимое вкладки рисуется вручную, в порядке: кружок несохранённых
изменений (рисуется только если файл изменён), название вкладки и иконка
языка из files_icons (у консолей — иконка из editor_icons).
"""

import json

from PyQt6.QtCore import Qt, QPoint, QByteArray, QMimeData, QRect, QRectF, QSize
from PyQt6.QtGui import QDrag, QPixmap, QRegion, QPainter, QColor, QPen, QIcon, QCursor, QGuiApplication
from PyQt6.QtWidgets import (
    QTabBar, QApplication, QTabWidget,
    QStyle, QStyleOptionTab, QStyleOptionTabBarBase,
)

from editor.widgets.code_editor import CodeEditor
from editor.widgets.terminal import TerminalWidget
from editor.widgets.diagnostics_widget import DiagnosticsWidget
from editor.widgets.symbols_widget import SymbolsWidget
from editor.widgets.file_icons import file_icon_for_path, load_ui_icon
from editor.widgets.qt_utils import find_ancestor, sync_tab_data


# ---- геометрия содержимого вкладки: кружок · название · иконка ----
_TAB_LEFT_PAD = 10     # от левого края таба до кружка
_TAB_DOT = 6           # диаметр кружка сохранённости
_TAB_DOT_GAP = 6       # кружок → название
_TAB_ICON_GAP = 6      # название → иконка
_TAB_ICON_SIZE = 14    # размер иконки языка/консоли
_TAB_RIGHT_PAD = 22    # справа: место под кнопку закрытия + отступ


class DraggableTabBar(QTabBar):
    """TabBar с перетаскиванием вкладок, кастомными X-кнопками закрытия
    и содержимым вкладки: кружок сохранённости + название + иконка языка."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._drag_start_pos = QPoint()
        self._drag_tab_index = -1
        self._close_hovered = -1
        self._hovered_tab = -1
        self._theme_bg = "#1b1d2e"
        self._theme_fg = "#cdd6f4"
        self._theme_border = "#6c7086"
        # Стрелки прокрутки не показываем. При нехватке места табы
        # сжимаются, а текст усекается многоточием, но остаются доступны
        # все табы (полный текст виден, пока места хватает).
        self.setUsesScrollButtons(False)
        self.setElideMode(Qt.TextElideMode.ElideRight)

        # Открепление таба: окно открывается только когда таб отпущен за
        # пределами исходного окна. Окно-источник запоминаем до drag.exec()
        self._drag_source_window = None

    def minimumSizeHint(self) -> QSize:
        """Табы не должны мешать уменьшать панель.

        По умолчанию QTabBar без кнопок прокрутки возвращает в качестве
        минимума ширину всех табов — из-за этого сплиттер не даёт сузить
        область. Отдаём нулевую ширину (высоту оставляем обычной).
        """
        return QSize(0, super().sizeHint().height())

    # ---- тема ----

    def set_theme_colors(self, bg: str, fg: str, border: str):
        """Сохраняет цвета темы для отрисовки содержимого и X-кнопок."""
        self._theme_bg = bg
        self._theme_fg = fg
        self._theme_border = border
        self.update()

    # ---- содержимое вкладки ----

    def _tab_widget(self, index: int):
        """Виджет вкладки (CodeEditor / TerminalWidget) или None."""
        parent = self.parent()
        if isinstance(parent, QTabWidget):
            return parent.widget(index)
        return None

    def _tab_editor(self, index: int):
        """Редактор вкладки, даже если она обёрнута в _EditorHost."""
        widget = self._tab_widget(index)
        return getattr(widget, "editor", None) or (
            widget if isinstance(widget, CodeEditor) else None)

    def _tab_icon(self, index: int, fg: QColor) -> QIcon:
        """Иконка вкладки: язык файла, консоль, диагностики или символы."""
        widget = self._tab_widget(index)
        if isinstance(widget, TerminalWidget):
            return load_ui_icon("console", fg)
        if isinstance(widget, DiagnosticsWidget):
            return load_ui_icon("diagostics", fg)
        if isinstance(widget, SymbolsWidget):
            return load_ui_icon("Symbols", fg)
        editor = self._tab_editor(index)
        if editor is not None:
            return file_icon_for_path(editor.file_path, fg)
        return QIcon()

    def _close_button_rect(self, index: int) -> QRect:
        """Возвращает прямоугольник кнопки закрытия для вкладки index."""
        rect = self.tabRect(index)
        size = 14
        x = rect.right() - size - 4
        y = rect.top() + (rect.height() - size) // 2
        return QRect(x, y, size, size)

    # ---- события мыши ----

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            tab = self.tabAt(event.pos())
            if tab >= 0:
                close_rect = self._close_button_rect(tab)
                if close_rect.contains(event.pos()):
                    # Закрываем вкладку — эмитим сигнал QTabBar
                    self.tabCloseRequested.emit(tab)
                    return
            self._drag_start_pos = event.pos()
            self._drag_tab_index = tab
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event):
        left_down = bool(event.buttons() & Qt.MouseButton.LeftButton)

        if not left_down:
            # Отслеживаем hover на табах и кнопках закрытия (без драга)
            tab = self.tabAt(event.pos())
            if tab != self._hovered_tab:
                self._hovered_tab = tab
                self.update()
            hovered_close = -1
            if tab >= 0 and self._close_button_rect(tab).contains(event.pos()):
                hovered_close = tab
            if hovered_close != self._close_hovered:
                self._close_hovered = hovered_close
                self.update()
            return

        if (event.pos() - self._drag_start_pos).manhattanLength() < QApplication.startDragDistance():
            return
        if self._drag_tab_index < 0:
            return

        tab_text = self.tabText(self._drag_tab_index)
        tab_data = self.tabData(self._drag_tab_index)
        if not tab_data:
            return
        # Диагностики — эфемерная вкладка, но переносим живой виджет целиком
        # (как терминалы), см. EditorPane._move_or_copy_tab.
        # Приёмник дропа находит по этому индексу живой контейнер вкладки,
        # чтобы перенести его целиком (панель поиска, курсор, undo-история)
        tab_data["drag_index"] = self._drag_tab_index

        # tabData может устареть (например, после Save As), поэтому контент
        # и путь берём прямо из живого редактора в момент начала драга
        tab_widget = self.parent()
        widget = None
        if isinstance(tab_widget, QTabWidget):
            widget = tab_widget.widget(self._drag_tab_index)
            editor = getattr(widget, "editor", None) or (
                widget if isinstance(widget, CodeEditor) else None)
            if editor is not None:
                sync_tab_data(editor)
            elif isinstance(widget, TerminalWidget):
                # Переносим живой терминал: по id приёмник найдёт виджет
                # в реестре и заберёт его целиком вместе с сессией ConPTY
                tab_data["terminal_id"] = widget.terminal_id
            elif isinstance(widget, DiagnosticsWidget):
                # Эфемерная вкладка: метка виджета, чтобы приёмник мог
                # проверить, что за время драга вкладку не закрыли/
                # пересоздали (см. EditorPane._take_diagnostics_widget)
                tab_data["widget_id"] = id(widget)
            elif isinstance(widget, SymbolsWidget):
                # Вкладка символов — такая же эфемерная, переносится живьём
                tab_data["widget_id"] = id(widget)
        tab_data["title"] = tab_text

        mime = QMimeData()
        mime.setData("application/x-editor-tab",
                      QByteArray(json.dumps(tab_data).encode("utf-8")))
        mime.setText(tab_text)

        if isinstance(tab_widget, QTabWidget):
            tab_count = tab_widget.count()
            mime.setData("application/x-tab-count",
                          QByteArray(str(tab_count).encode("utf-8")))

        drag = QDrag(self)
        drag.setMimeData(mime)

        rect = self.tabRect(self._drag_tab_index)
        pixmap = QPixmap(rect.size())
        self.render(pixmap, QPoint(), QRegion(rect))
        drag.setPixmap(pixmap)
        drag.setHotSpot(event.pos() - rect.topLeft())

        # На Windows Qt рисует поверх пиксмапы драга ещё и курсор действия.
        # Вне валидных таргетов (рабочий стол, чужое окно) это «перечёркнутый
        # круг» — будто перетаскивание невозможно. Подменяем прозрачным:
        # таб всё равно открепится в новое окно, поэтому запрет неверен.
        drag.setDragCursor(QPixmap(1, 1), Qt.DropAction.IgnoreAction)

        # Окно-деточка открывается только при отпускании таба: во время
        # драга ничего не создаём (на Windows создание окна посреди
        # OLE-драга может отменить сам драг)
        self._drag_source_window = self.window()
        self._terminal_handed_off = False

        result = drag.exec(Qt.DropAction.MoveAction | Qt.DropAction.CopyAction)

        source_win = self._drag_source_window
        self._drag_source_window = None

        still_here = (isinstance(tab_widget, QTabWidget)
                      and widget is not None
                      and tab_widget.indexOf(widget) >= 0)

        if result == Qt.DropAction.MoveAction:
            if isinstance(tab_widget, QTabWidget) and tab_widget.count() == 0:
                # Вкладку отцепили живьём (take_live_host) и панель опустела —
                # закрываем пустую панель/окно или создаём Untitled.
                self._handle_empty_source(tab_widget)

            # Если вкладку забрали живым терминалом — исходный таб-бар
            # уже помечен take_terminal_from() и закрывать здесь нечего.
            # Иначе контент уже скопирован в новый таб: закрываем исходный
            # без диалога «Сохранить изменения?».
            handed_off = self._terminal_handed_off
            self._terminal_handed_off = False
            if not handed_off and still_here:
                self._suppress_prompt = True
                try:
                    self.tabCloseRequested.emit(self._drag_tab_index)
                finally:
                    self._suppress_prompt = False
        elif still_here and source_win is not None:
            # Таб никто не принял (рабочий стол, чужое окно). Если отпустили
            # за пределами исходного окна — открываем деточку и переносим
            # туда таб.
            drop_pos = QCursor.pos()
            if not source_win.geometry().adjusted(
                    -60, -60, -60, -60).contains(drop_pos):
                win = self._create_detach_window(source_win, drop_pos)
                if win is not None:
                    win.tile_manager._panes[0]._move_or_copy_tab(
                        tab_data, source=self)
                    self._terminal_handed_off = False
                    if (isinstance(tab_widget, QTabWidget)
                            and tab_widget.count() == 0):
                        # Исходная панель опустела — закрыть панель/окно
                        # или создать Untitled
                        self._handle_empty_source(tab_widget)

        self._drag_tab_index = -1

    def _handle_empty_source(self, tab_widget):
        """Разбирает опустевшую после переноса исходную панель.

        Локальный импорт — чтобы избежать циклического импорта (панель
        импортирует tab_bar).
        """
        from editor.widgets.editor_pane import EditorPane
        pane = find_ancestor(
            tab_widget, lambda w: isinstance(w, EditorPane)
        )
        if pane is not None:
            pane.on_tab_removed()

    # ---- окно-деточка (открепление таба за пределы окна) ----

    def _create_detach_window(self, source, cursor: QPoint):
        theme_manager = getattr(source, "theme_manager", None)
        lexer_manager = getattr(source, "lexer_manager", None)
        theme_name = getattr(source, "current_theme", None)
        if theme_manager is None or lexer_manager is None:
            return None
        from editor.detached_window import DetachedWindow
        win = DetachedWindow(theme_manager, lexer_manager, theme_name,
                             title=source.windowTitle())
        win.resize(760, 520)
        # Заготовку-Untitled из нового окна убираем: сюда должен попасть
        # именно переносимый таб. Берём первую панель нового окна
        # напрямую — get_active_pane() до показа окна вернул бы панель
        # окна-источника (фокус ещё там), и мы удалили бы не тот таб
        try:
            panes = win.tile_manager._panes
            if panes and panes[0].tabs.count() > 0:
                idx = panes[0].tabs.currentIndex()
                if idx >= 0:
                    panes[0].tabs.removeTab(idx)
        except RuntimeError:
            pass
        self._place_detach_window(win, cursor)
        win.show()
        return win

    @staticmethod
    def _place_detach_window(win, cursor: QPoint):
        size = win.size()
        x = cursor.x() - size.width() // 2
        y = cursor.y() - 40
        screen = QGuiApplication.screenAt(cursor)
        if screen is not None:
            ag = screen.availableGeometry()
            x = max(ag.left() + 20, min(x, ag.right() - size.width() - 20))
            y = max(ag.top() + 20, min(y, ag.bottom() - size.height() - 20))
        win.move(x, y)

    def mouseReleaseEvent(self, event):
        self._drag_tab_index = -1
        super().mouseReleaseEvent(event)

    def leaveEvent(self, event):
        """Сбрасываем hover при уходе мыши."""
        if self._close_hovered >= 0 or self._hovered_tab >= 0:
            self._close_hovered = -1
            self._hovered_tab = -1
            self.update()
        super().leaveEvent(event)

    # ---- размеры ----

    def tabSizeHint(self, index: int) -> QSize:
        """Ширина таба под кастомное содержимое: кружок + название + иконка.

        Место под кружок резервируется у всех табов всегда — чтобы ширина
        таба не менялась при появлении/исчезновении кружка. Сам кружок
        рисуется только при несохранённых изменениях у редакторов.
        """
        base = super().tabSizeHint(index)
        text_width = self.fontMetrics().horizontalAdvance(self.tabText(index))
        extra = (_TAB_LEFT_PAD + _TAB_DOT + _TAB_DOT_GAP
                 + _TAB_ICON_GAP + _TAB_ICON_SIZE + _TAB_RIGHT_PAD)
        return QSize(max(extra + text_width, base.width()), base.height())

    # ---- отрисовка ----

    def paintEvent(self, event):
        """Рисуем вкладки полностью сами: форма таба — из QSS темы,
        содержимое (кружок сохранённости, название, иконка) и X-кнопки
        закрытия — вручную."""
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setRenderHint(QPainter.RenderHint.TextAntialiasing)
        fg_color = QColor(self._theme_fg)
        style = self.style()

        # Линия-основание под вкладками рисуется ДО табов: в QSS-режиме
        # PE_FrameTabBarBase накрывает весь таб-бар полупрозрачной заливкой
        # и, если рисовать её поверх, осветляет верхнюю 1px-обводку табов.
        base_option = QStyleOptionTabBarBase()
        base_option.initFrom(self)
        base_option.shape = self.shape()
        base_option.documentMode = self.documentMode()
        base_option.tabBarRect = QRect(self.rect())
        base_option.selectedTabRect = self.tabRect(self.currentIndex())
        style.drawPrimitive(QStyle.PrimitiveElement.PE_FrameTabBarBase,
                            base_option, painter, self)

        for i in range(self.count()):
            rect = self.tabRect(i)
            if not rect.intersects(event.rect()):
                continue
            option = QStyleOptionTab()
            self.initStyleOption(option, i)
            style.drawControl(QStyle.ControlElement.CE_TabBarTabShape,
                              option, painter, self)
            self._paint_tab_contents(painter, i, rect, fg_color)

        # X-кнопки закрытия только на hover-табе
        for i in range(self.count()):
            if i != self._hovered_tab:
                continue

            close_rect = self._close_button_rect(i)

            # Цвет X: полупрозрачный обычно, ярче при наведении на кнопку
            alpha = 200 if self._close_hovered == i else 100
            line_color = QColor(fg_color)
            line_color.setAlpha(alpha)

            pen = QPen(line_color, 1.2)
            painter.setPen(pen)
            m = 3
            painter.drawLine(close_rect.left() + m, close_rect.top() + m,
                           close_rect.right() - m, close_rect.bottom() - m)
            painter.drawLine(close_rect.left() + m, close_rect.bottom() - m,
                           close_rect.right() - m, close_rect.top() + m)

        painter.end()

    def _paint_tab_contents(self, painter: QPainter, index: int,
                            rect: QRect, fg: QColor):
        """Кружок сохранённости → название → иконка языка/консоли."""
        fm = self.fontMetrics()
        cy = rect.center().y()
        x = rect.left() + _TAB_LEFT_PAD
        text_top = rect.top() + (rect.height() - fm.height()) // 2

        # Слот под кружок резервируется у всех табов (текст не прыгает),
        # сам кружок рисуется только у изменённых редакторов
        editor = self._tab_editor(index)
        if editor is not None and editor.isModified():
            dot_rect = QRectF(x, cy - _TAB_DOT / 2, _TAB_DOT, _TAB_DOT)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(fg)
            painter.drawEllipse(dot_rect)
        x += _TAB_DOT + _TAB_DOT_GAP

        # Название — полный текст. Элидируется только если таб реально сужен
        # (переполнение). Ширину считаем через left()+width(), а не right():
        # right() на 1px меньше, и elidedText режет текст «…» даже когда
        # таб получил полный sizeHint. Зазор до иконки — запас для текста.
        icon_left = rect.left() + rect.width() - _TAB_RIGHT_PAD - _TAB_ICON_SIZE
        text_width = icon_left - x
        if text_width > 0:
            elided = fm.elidedText(self.tabText(index),
                                   Qt.TextElideMode.ElideRight,
                                   text_width)
            text_rect = QRectF(x, text_top, text_width, fm.height())
            painter.setPen(fg)
            painter.drawText(text_rect,
                             Qt.AlignmentFlag.AlignVCenter
                             | Qt.AlignmentFlag.AlignLeft,
                             elided)

        # Иконка языка (или консоль) — справа от названия
        icon = self._tab_icon(index, fg)
        if not icon.isNull():
            icon_rect = QRect(icon_left, cy - _TAB_ICON_SIZE // 2,
                              _TAB_ICON_SIZE, _TAB_ICON_SIZE)
            icon.paint(painter, icon_rect, Qt.AlignmentFlag.AlignCenter)
