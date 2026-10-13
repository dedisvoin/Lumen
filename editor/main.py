"""
Главный модуль — MainWindow + точка входа.
"""


import sys
import os
import ctypes
from pathlib import Path
from typing import Optional

# Корень проекта (Lumen/) — чтобы импорты editor.* работали из любой точки
_project_root = Path(__file__).resolve().parent.parent
if str(_project_root) not in sys.path:
    sys.path.insert(0, str(_project_root))

from PyQt6.QtCore import (
    Qt, QTimer, QRectF, QSize, QObject, QEvent, QSettings, QPoint, QByteArray,
)
from PyQt6.QtGui import (
    QFont, QColor, QGuiApplication, QAction,
    QIcon, QPainter, QPen, QCursor, QPalette, QPixmap,
)
from PyQt6.QtSvg import QSvgRenderer

from PyQt6.QtWidgets import (
    QApplication, QWidget, QVBoxLayout, QMenuBar,
    QLabel, QMenu,
    QHBoxLayout, QSplitter, QToolButton,
)

from qframelesswindow import FramelessWindow, StandardTitleBar

from editor.constants import (
    UI_FONT_SIZE, MENU_FONT_SIZE, TITLE_FONT_SIZE,
    FALLBACK_BG, FALLBACK_FG,
    DEFAULT_THEME,
)
from editor.font_settings import ui_font_family
from editor.resources import editor_path
from editor.theme_utils import ThemeColors, apply_narrow_caption_buttons
from editor.theme_manager import ThemeManager
from editor.lexer_manager import LexerManager
from editor.tile_manager import TileManager
from editor.spotlight import SpotlightController
from editor.widgets.animated_hover import HoverFade, install_titlebar_button_hover_fade
from editor.widgets.code_editor import CodeEditor
from editor.widgets.file_tree import FileTreeWidget
from editor.widgets.terminal import TerminalWidget
from editor.widgets.file_icons import load_ui_icon
from editor.widgets.qt_utils import sync_tab_data
from editor.lsp import get_lsp_manager
from editor.tasks import TasksController, TaskListPopup
from editor.file_finder import FileFindPopup
from editor.bookmark_finder import BookmarkFindPopup, collect_bookmarks
from editor.detached_window import all_detached_windows
from editor.menu_builder import MenuBuilder
from editor import themed_dialogs
from editor.session import save_session, load_session
from editor.settings_dialog import open_settings
from editor.settings_store import app_settings
from editor.settings_apply import (
    apply_settings as _apply_settings,
    iter_editors,
)



def status_lsp_text(editor) -> str:
    """Метка статус-бара: basedpyright(python), clangd(.cpp) или просто расширение.

    Без LSP — только расширение файла; с LSP — имя сервера из конфига
    (server_name) и в скобках язык (python) либо расширение файла.
    """
    if not getattr(editor, "lsp_attached", False):
        return os.path.splitext(getattr(editor, "file_path", "") or "")[1]
    cfg = get_lsp_manager().config_for(editor.language_id)
    name = "lsp"
    if cfg is not None:
        name = (cfg.server_name
                or os.path.splitext(os.path.basename(cfg.command[0]))[0])
    ext = os.path.splitext(getattr(editor, "file_path", "") or "")[1]
    inner = (getattr(editor, "language_id", "")
             if getattr(editor, "language_id", "") == "python" else ext)
    return f"{name}({inner})" if inner else name


class _LspSpinner(QWidget):
    """Крутящийся индикатор в статус-баре: виден, пока LSP-сервер стартует.

    Рисуется дуга (спиннер с «хвостом»), вращается по таймеру, когда активен.
    """

    _SIDE = 13
    _SPEED = 45     # градусов за тик
    _TICK_MS = 70

    def __init__(self, parent=None):
        super().__init__(parent)
        self._angle = 0.0
        self._color = QColor(180, 180, 180)
        self._active = False
        self.setFixedSize(self._SIDE, self._SIDE)
        self.hide()
        self._timer = QTimer(self)
        self._timer.setInterval(self._TICK_MS)
        self._timer.timeout.connect(self._tick)

    def set_color(self, color: QColor):
        """Цвет спиннера — из темы (цвет текста статус-бара)."""
        self._color = QColor(color)
        self.update()

    def set_active(self, active: bool):
        active = bool(active)
        if active == self._active:
            return
        self._active = active
        self.setVisible(active)
        if active:
            self._timer.start()
        else:
            self._timer.stop()

    @property
    def active(self) -> bool:
        return self._active

    def _tick(self):
        self._angle = (self._angle + self._SPEED) % 360.0
        self.update()

    def paintEvent(self, event):
        if not self._active:
            return
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(1.0, 1.0, self.width() - 2.0, self.height() - 2.0)
        base_color = QColor(self._color)
        base_color.setAlpha(45)
        base_pen = QPen(base_color)
        base_pen.setWidthF(1.4)
        p.setPen(base_pen)
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawEllipse(rect)
        arc_pen = QPen(self._color)
        arc_pen.setWidthF(1.9)
        arc_pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        p.setPen(arc_pen)
        p.drawArc(rect, int(-self._angle * 16), int(270 * 16))
        p.end()


class _StatusBarButtonSizer(QObject):
    """Кнопки-квадраты со стороной = высоте бара: прижаты к левому нижнему
    углу статус-бара и торчат на 1px над его верхним краем. Раскладываются
    в ряд слева направо в порядке передачи."""

    def __init__(self, statusbar: QWidget, buttons):
        super().__init__(statusbar)
        self._buttons = list(buttons)
        statusbar.installEventFilter(self)
        self._sync(statusbar.height())

    def eventFilter(self, obj, event):
        if obj is self.parent() and event.type() == QEvent.Type.Resize:
            self._sync(self.parent().height())
        return super().eventFilter(obj, event)

    def _sync(self, bar_height: int):
        if bar_height <= 0:
            return
        side = bar_height
        x = 0
        for btn in self._buttons:
            btn.setFixedSize(side, side)
            btn.move(x, 0)  # прижаты к левому верхнему/нижнему краю
            x += side


class _StatusBarSeparator(QWidget):
    """Вертикальный разделитель статус-бара между кнопками-переключателями
    и счётчиками ошибок/предупреждений. Цвет задаётся из темы в
    apply_theme_to_all (QSS тут бесполезен: до показа окна цвет первого
    apply «запекается» при первой полировке виджета)."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._color = QColor(0, 0, 0)
        self.setFixedWidth(2)

    def set_color(self, color: QColor):
        self._color = QColor(color)
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing, False)
        x = self.width() // 2
        p.fillRect(x, 0, 1, self.height(), self._color)
        p.end()


class _StatusBarToggleButton(QToolButton):
    """Кнопка статус-бара: при наведении подсвечивается не всей площадью,
    а меньшей закруглённой областью (отступ от краёв кнопки).

    Подсветка рисуется вручную в paintEvent с анимацией прозрачности
    (HoverFade), поэтому плавно проявляется и плавно гаснет — в отличие
    от мгновенного переключения нативного QSS `:hover`.
    """

    _HOVER_INSET = 3
    _HOVER_RADIUS = 7

    def __init__(self, parent=None):
        super().__init__(parent)
        self._hover_color = QColor(255, 255, 255, 20)
        self._fade = HoverFade(self)
        self.setAttribute(Qt.WidgetAttribute.WA_Hover)
        self.setStyleSheet(
            "QToolButton { border: none; background: transparent; }"
        )

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
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        c = QColor(self._hover_color)
        c.setAlpha(round(c.alpha() * self._fade.progress()))
        if c.alpha() > 0:
            rect = QRectF(self.rect()).adjusted(
                self._HOVER_INSET, self._HOVER_INSET,
                -self._HOVER_INSET, -self._HOVER_INSET,
            )
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(c)
            p.drawRoundedRect(rect, self._HOVER_RADIUS, self._HOVER_RADIUS)
        p.end()
        super().paintEvent(event)


class _TitleBarIconButton(QToolButton):
    """Кнопка тайтл-бара (настройки): иконка + квадратная подсветка hover/pressed.

    Цвета подсветки приходят из темы (apply_theme_to_all), как у системных
    кнопок сворачивания/закрытия. Hover-подсветка плавно проявляется/гаснет
    через HoverFade; pressed — без анимации (мгновенный отклик на клик).
    """

    _HOVER_INSET = 0
    _HOVER_RADIUS = 0

    def __init__(self, parent=None):
        super().__init__(parent)
        self._pressed = False
        self._hover_color = QColor(255, 255, 255, 30)
        self._pressed_color = QColor(255, 255, 255, 60)
        self._fade = HoverFade(self)
        self.setAttribute(Qt.WidgetAttribute.WA_Hover)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setStyleSheet(
            "QToolButton { border: none; background: transparent; }"
        )
        self.setFixedSize(40, 30)

    def set_hover_color(self, color: QColor):
        self._hover_color = QColor(color)
        self.update()

    def set_pressed_color(self, color: QColor):
        self._pressed_color = QColor(color)
        self.update()

    def enterEvent(self, event):
        self._fade.fade_in()
        super().enterEvent(event)

    def leaveEvent(self, event):
        self._fade.fade_out()
        super().leaveEvent(event)

    def mousePressEvent(self, event):
        self._pressed = True
        self.update()
        super().mousePressEvent(event)

    def mouseReleaseEvent(self, event):
        self._pressed = False
        self.update()
        super().mouseReleaseEvent(event)

    def paintEvent(self, event):
        if self._pressed or self._fade.progress() > 0:
            p = QPainter(self)
            p.setRenderHint(QPainter.RenderHint.Antialiasing)
            rect = QRectF(self.rect()).adjusted(
                self._HOVER_INSET, self._HOVER_INSET,
                -self._HOVER_INSET, -self._HOVER_INSET,
            )
            if self._pressed:
                col = QColor(self._pressed_color)
            else:
                col = QColor(self._hover_color)
                col.setAlpha(round(col.alpha() * self._fade.progress()))
            if col.alpha() > 0:
                p.setPen(Qt.PenStyle.NoPen)
                p.setBrush(col)
                p.drawRoundedRect(rect, self._HOVER_RADIUS, self._HOVER_RADIUS)
            p.end()
        super().paintEvent(event)


# ----------------------------------------------------------------------
# DWM-скругление попапов QMenu
# ----------------------------------------------------------------------
class _MenuDwmRoundedFilter(QObject):
    """Скругляет окно попапа QMenu через DWM (DWMWCP_ROUND).

    QSS рисует попапу скруглённый фон, но само окно остаётся квадратным,
    и в нижнем правом углу виден «уголок». Атрибут DWM скругляет окно
    целиком, и уголок исчезает (проверено пиксельно).
    """

    def eventFilter(self, obj, event):
        if event.type() == QEvent.Type.Show and isinstance(
                obj, (QMenu, TaskListPopup, FileFindPopup)):
            try:
                hwnd = int(obj.winId())
                dwmapi = ctypes.windll.dwmapi
                v = ctypes.c_int(2)
                dwmapi.DwmSetWindowAttribute(
                    hwnd, 33, ctypes.byref(v), ctypes.sizeof(v)
                )
            except Exception:
                pass
        return False


# ----------------------------------------------------------------------
# Логотип приложения
# ----------------------------------------------------------------------
def app_icon(color=None, sizes=(16, 24, 32, 48, 64, 128, 256)) -> QIcon:
    """Иконка приложения из editor_icons/new_logo.svg.

    Рисуем через QSvgRenderer, а не отдаём путь в QIcon: svg-формат
    QIcon читает imageformat-плагином qsvg, которого в onefile-сборке
    Nuitka нет, и иконка молча становится пустой. Модуль PyQt6.QtSvg в
    сборку попадает — на нём уже работают иконки дерева и файлов.

    color=None — исходные цвета svg. В логотипе обводка чёрная (#000),
    поэтому для тёмного тайтл-бара её надо перекрашивать: передаём сюда
    цвет текста темы и получаем тот же значок, но читаемый на фоне.
    """
    data = editor_path("editor_icons", "new_logo.svg").read_bytes()
    tint = QColor(color) if color is not None else QColor()
    icon = QIcon()
    for size in sizes:
        pixmap = QPixmap(size, size)
        pixmap.fill(Qt.GlobalColor.transparent)
        painter = QPainter(pixmap)
        QSvgRenderer(QByteArray(data)).render(painter)
        painter.end()
        if tint.isValid():
            # Красим только непрозрачные пиксели (SourceIn), иначе заливка
            # залила бы весь квадрат.
            fill = QPixmap(size, size)
            fill.fill(tint)
            painter.begin(pixmap)
            painter.setCompositionMode(
                QPainter.CompositionMode.CompositionMode_SourceIn
            )
            painter.drawPixmap(0, 0, fill)
            painter.end()
        icon.addPixmap(pixmap)
    return icon


# ----------------------------------------------------------------------
# MainWindow
# ----------------------------------------------------------------------
class MainWindow(FramelessWindow):
    def _refresh_titlebar_icon(self, color=None):
        """Ставит логотип в тайтл-бар вместо текста названия окна.

        color — цвет текста темы: обводка логотипа чёрная, на тёмном
        фоне тайтл-бара без перекраски значок не читается. Вызывается
        при создании окна и при каждой смене темы.
        """
        label = getattr(self.titleBar, "iconLabel", None)
        if label is None:
            return
        label.setFixedSize(20, 20)
        label.setPixmap(app_icon(color, sizes=(20, 40)).pixmap(20, 20))
        label.show()

    def __init__(self):
        super().__init__()

        self.setObjectName("mainWindow")
        self.setWindowTitle("SolarIDE")

        # Replace default TitleBar with StandardTitleBar
        standard_tb = StandardTitleBar(self)
        # В тайтл-баре — логотип вместо текста названия (см. app_icon).
        standard_tb.titleLabel.hide()
        # Стандартный titlebar сам подставляет иконку окна (чёрный контур
        # поверх тёмного фона — нечитаемо), поэтому отписываемся от его
        # реакции на windowIconChanged и рисуем иконку сами.
        try:
            self.windowIconChanged.disconnect(standard_tb.setIcon)
        except (TypeError, RuntimeError):
            pass
        self.setTitleBar(standard_tb)
        apply_narrow_caption_buttons(standard_tb)

        # DWM rounded corners (Windows 11+)
        self._enable_dwm_rounded_corners()

        self.resize(1280, 800)

        # Центрирование окна по центру экрана
        screen_center = QGuiApplication.primaryScreen().availableGeometry().center()
        frame_center = self.rect().center()
        self.move(screen_center - frame_center)


        self.themes_dir = str(editor_path("themes"))
        self.theme_manager = ThemeManager(self.themes_dir)

        self.lexer_manager = LexerManager(self.theme_manager)

        # Main layout with top margin for the overlaid title bar
        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(0, self.titleBar.height(), 0, 0)
        main_layout.setSpacing(0)

        # Menu bar — inline inside the title bar
        self._menubar = QMenuBar(self.titleBar)
        self._menubar.setStyleSheet(
            "QMenuBar { background: transparent; border: none; }"
        )
        self.titleBar.hBoxLayout.insertWidget(
            3, self._menubar, 0, Qt.AlignmentFlag.AlignLeft
        )
        # Убираем стартовый отступ 10px и ставим свой: логотип не должен
        # прилипать к краю окна
        if self.titleBar.hBoxLayout.count() > 0:
            item = self.titleBar.hBoxLayout.itemAt(0)
            if item and item.spacerItem():
                self.titleBar.hBoxLayout.removeItem(item)
        self.titleBar.hBoxLayout.insertSpacing(0, 9)
        self._refresh_titlebar_icon()

        # Кнопка настроек — справа, перед системными кнопками окна
        self._settings_btn = _TitleBarIconButton(self.titleBar)
        self._settings_btn.setToolTip("Settings")
        self._settings_btn.setIconSize(QSize(16, 16))
        tb_lay = self.titleBar.hBoxLayout
        tb_lay.insertWidget(
            max(tb_lay.indexOf(self.titleBar.minBtn), 0),
            self._settings_btn, 0, Qt.AlignmentFlag.AlignVCenter,
        )
        self._settings_btn.clicked.connect(self._open_settings)

        # Editor area
        central_editor = QWidget()
        self.tile_manager = TileManager(
            central_editor, self.lexer_manager, self.theme_manager
        )

        # Зенитный спотлайт: приглушение неактивных панелей (см. spotlight.py)
        self.spotlight = SpotlightController(
            self.tile_manager, self.theme_manager, parent=self)
        try:
            _store = app_settings()
            self.spotlight.set_enabled(
                _store.get_bool("view.spotlight", False))
            self.spotlight.set_alphas(
                _store.get_int("view.spotlight_dark_alpha", 230),
                _store.get_int("view.spotlight_light_alpha", 190))
            self.spotlight.set_skip(
                _store.get_bool("view.spotlight_skip_terminal", False),
                _store.get_bool("view.spotlight_skip_diagnostics", False))
        except Exception:
            pass

        # LSP: clangd для C/C++ файлов
        self.lsp_manager = get_lsp_manager()

        # File tree (sidebar) + editor area
        self.file_tree = FileTreeWidget(self.theme_manager)
        self.file_tree.open_file_requested.connect(self._open_path)
        self.file_tree.folder_opened.connect(self._on_folder_opened)
        self.file_tree.file_renamed.connect(self._on_file_renamed)

        self._root_splitter = QSplitter(Qt.Orientation.Horizontal)
        self._root_splitter.setHandleWidth(0)
        self._root_splitter.setChildrenCollapsible(False)
        # Живой ресайз: контент перерисовывается при перетаскивании
        self._root_splitter.setOpaqueResize(True)
        self._root_splitter.addWidget(self.file_tree)
        self._root_splitter.addWidget(central_editor)
        self._root_splitter.setStretchFactor(0, 0)
        self._root_splitter.setStretchFactor(1, 1)
        self._root_splitter.setSizes([250, 1030])

        main_layout.addWidget(self._root_splitter, 1)

        # Status bar (QWidget+QHBoxLayout вместо QStatusBar —
        # у QStatusBar нет способа убрать его разделители «|»)
        self._statusbar = QWidget()
        main_layout.addWidget(self._statusbar)

        self.current_theme = DEFAULT_THEME
        self._dark_mode_active: Optional[bool] = None

        # Недавние папки (из QSettings)
        self._recent_folders = self._load_recent_folders()

        # Таски: создание/правка/запуск — отдельный контроллер
        self._tasks = TasksController(self)
        # Меню тайтл-бара строит отдельный модуль
        self._menu_builder = MenuBuilder(self)
        self._menu_builder.build()
        # DWM-скругление попапов QMenu (меню-бар, контекстные меню)
        self._menu_dwm_filter = _MenuDwmRoundedFilter(self)
        QApplication.instance().installEventFilter(self._menu_dwm_filter)
        self._setup_statusbar()

        # Global font
        font = QFont("Consolas", 10)
        font.setStyleHint(QFont.StyleHint.Monospace)
        self.setFont(font)

        self._status_timer = QTimer(self)
        self._status_timer.timeout.connect(self._update_status)
        self._status_timer.start(200)
        # Кэш статус-бара: не трогаем UI, пока (редактор, позиция, LSP)
        # не изменились — иначе поллинг каждые 200 мс перерисовывает бар
        self._status_cache = None

        # Автосохранение: таймер по настройкам + сохранение при потере
        # фокуса окна (переключился на другое приложение)
        self._autosave_timer = QTimer(self)
        self._autosave_timer.timeout.connect(self._autosave_all)
        QApplication.instance().applicationStateChanged.connect(
            self._on_application_state_changed
        )

        # Ctrl+Left/Ctrl+Right — переключение вкладок (везде, кроме модальных
        # окон и меню). Фильтр приложения видит клавиши раньше фокус-виджета,
        # поэтому в редакторе они съедаются здесь; переход по словам перенесён
        # на Alt+Left/Right (code_editor._handle_word_move). QShortcut не
        # подходит: он перехватывал клавиши и у редактора.
        QApplication.instance().installEventFilter(self)

        # Ctrl+Tab / Ctrl+Shift+Tab — цикл по вкладкам активной панели
        # (у QTabWidget нет встроенного шортката). QAction из реестра
        # настраиваемых шорткатов корректно матчит Shift+Tab
        # (Key_Backtab нормализуется в Ctrl+Shift+Tab).
        self._cycle_tab_action = self._make_shortcut(
            "view.cycle_tab", self._cycle_tab)
        self._cycle_tab_prev_action = self._make_shortcut(
            "view.cycle_tab_prev",
            lambda: self._cycle_tab(previous=True))

        # F12 — переход к определению через clangd
        self._goto_definition_action = self._make_shortcut(
            "view.goto_definition", self._goto_definition)
        # F2 — переименование символа через LSP
        self._rename_symbol_action = self._make_shortcut(
            "view.rename_symbol", self._rename_symbol)
        # Ctrl+Shift+L — Common Lisp: проверить файл через SBCL
        self._check_lisp_action = self._make_shortcut(
            "view.check_lisp", self._check_lisp)

        self.apply_theme_to_all(self.current_theme)
        self._restore_session()

        # Настройки из editor/settings.json применяются после первого показа:
        # редакторы уже восстановлены, LSP-сессии перезапускаются под
        # пользовательскими путями/аргументами.
        QTimer.singleShot(0, self.apply_settings_from_store)

    def _make_shortcut(self, shortcut_id: str, slot):
        """QAction-шорткат из реестра editor/shortcuts.py.

        Регистрируется в окне (addAction), чтобы был активен, и помечается
        _shortcut_id — apply_shortcuts(settings_apply.py) переприменяет
        его после изменения на странице Shortcuts.
        """
        action = QAction(self)
        from editor.shortcuts import bind_shortcut
        bind_shortcut(action, shortcut_id)
        action.triggered.connect(slot)
        self.addAction(action)
        return action

    def apply_settings_from_store(self):
        """Применяет сохранённые настройки (после восстановления сессии)."""
        try:
            _apply_settings(self)
        except Exception:
            pass

    # --- автосохранение ---

    def configure_autosave(self):
        """Включает/выключает таймер автосохранения по настройкам."""
        store = app_settings()
        if store.get_bool("autosave.enabled", False):
            interval = store.get_int("autosave.interval", 30)
            self._autosave_timer.start(max(1, interval) * 1000)
        else:
            self._autosave_timer.stop()

    def _autosave_all(self):
        """Сохраняет на диск все изменённые файлы с путём (Untitled пропуск)."""
        for editor in iter_editors(self):
            try:
                if editor.file_path and editor.isModified():
                    editor.save_file()
            except Exception:
                continue

    def _on_application_state_changed(self, state):
        """Фокус ушёл от окна — сохраняем, если включено в настройках."""
        store = app_settings()
        if (state == Qt.ApplicationState.ApplicationInactive
                and store.get_bool("autosave.enabled", False)
                and store.get_bool("autosave.on_focus_lost", False)):
            self._autosave_all()

    def _restore_session(self):
        """Восстанавливает окно, папку и дерево панелей из сессии."""
        data = load_session()
        if data is None:
            return
        # Тему из сессии запоминаем, но применяем ниже — после restore
        # редакторов, чтобы палитра легла один раз на созданные вкладки.
        theme = data.get("theme")
        if theme in self.theme_manager.themes:
            self.current_theme = theme
        folder = data.get("folder")
        if folder and os.path.isdir(folder):
            # Корень воркспейса до создания редакторов — clangd-сессии
            # сразу группируются по нему
            self.file_tree.set_root_path(folder)
            self.lsp_manager.set_workspace_root(folder)
        self.tile_manager.restore(data.get("tree"))
        self.apply_theme_to_all(self.current_theme)

        geo = data.get("geometry") or {}
        if geo.get("maximized"):
            self.showMaximized()
        else:
            w, h = geo.get("w") or 1280, geo.get("h") or 800
            self.resize(w, h)
            x, y = geo.get("x"), geo.get("y")
            if x is not None and y is not None:
                self.move(x, y)

        sizes = data.get("root_sizes")
        if sizes and len(sizes) == self._root_splitter.count():
            sizes = [int(s) for s in sizes]
            self._root_splitter.setSizes(sizes)
            self._root_splitter.setProperty("session_sizes", sizes)
        if not data.get("tree_visible", True):
            self._toggle_file_tree(False)

        act = data.get("active")
        editor = None
        if act:
            panes = self.tile_manager._panes
            if 0 <= act.get("pane", 0) < len(panes):
                pane = panes[act["pane"]]
                tab = act.get("tab", 0)
                if 0 <= tab < pane.tabs.count():
                    pane.tabs.setCurrentIndex(tab)
                    editor = pane.current_editor()
        if editor is None:
            editor = self._get_current_editor()
        if editor is not None:
            self.tile_manager.active_editor = editor
            QTimer.singleShot(0, editor.setFocus)

    def showEvent(self, event):
        super().showEvent(event)
        if not getattr(self, "_session_sizes_applied", False):
            self._session_sizes_applied = True
            self._apply_session_sizes()

    def _apply_session_sizes(self):
        """Размеры сплиттеров применяются после показа окна: до показа Qt
        не знает реальных размеров виджетов и может сбросить сохранённые."""

        def walk(w):
            if isinstance(w, QSplitter):
                sizes = w.property("session_sizes")
                if sizes and len(sizes) == w.count():
                    w.setSizes([int(s) for s in sizes])
                for i in range(w.count()):
                    walk(w.widget(i))

        walk(self.tile_manager._main_splitter)
        sizes = self._root_splitter.property("session_sizes")
        if sizes and len(sizes) == self._root_splitter.count():
            self._root_splitter.setSizes([int(s) for s in sizes])

    def _enable_dwm_rounded_corners(self):
        """DWM: скруглённые углы окна (Windows 11+)."""
        self._dwm_set_attribute(33, 2)  # DWMWCP_ROUND

    def _dwm_set_attribute(self, attribute: int, value: int):
        """Устанавливает атрибут окна через dwmapi (сбой молча игнорируем)."""
        try:
            hwnd = int(self.winId())
            dwmapi = ctypes.windll.dwmapi
            c_value = ctypes.c_int(value)
            dwmapi.DwmSetWindowAttribute(
                hwnd, attribute, ctypes.byref(c_value), ctypes.sizeof(c_value)
            )
        except Exception:
            pass

    # ---- status bar ----

    def _setup_statusbar(self):
        self._statusbar.setObjectName("statusBar")
        # Кнопка показа/скрытия дерева — квадрат со стороной = высоте бара;
        # вне layout, прижата к левому краю (позиционирует _StatusBarButtonSizer)
        self._tree_toggle_btn = _StatusBarToggleButton(self._statusbar)
        self._tree_toggle_btn.setCheckable(True)
        self._tree_toggle_btn.setChecked(True)
        self._tree_toggle_btn.setToolTip("Show/Hide File Tree")
        self._tree_toggle_btn.setIconSize(QSize(16, 16))
        self._tree_toggle_btn.toggled.connect(self._toggle_file_tree)

        # Кнопка показа/скрытия списка диагностик — так же, как кнопка дерева,
        # рядом с ней (иконка diagostics.svg в цветах темы)
        self._diag_toggle_btn = _StatusBarToggleButton(self._statusbar)
        self._diag_toggle_btn.setCheckable(True)
        self._diag_toggle_btn.setChecked(False)
        self._diag_toggle_btn.setToolTip("Show/Hide Diagnostics")
        self._diag_toggle_btn.setIconSize(QSize(16, 16))
        self._diag_toggle_btn.toggled.connect(self._on_diag_toggle)

        # Кнопка показа/скрытия списка символов активного файла — как кнопка
        # диагностик (иконка symbols/function.svg в цветах темы)
        self._sym_toggle_btn = _StatusBarToggleButton(self._statusbar)
        self._sym_toggle_btn.setCheckable(True)
        self._sym_toggle_btn.setChecked(False)
        self._sym_toggle_btn.setToolTip("Show/Hide Symbols")
        self._sym_toggle_btn.setIconSize(QSize(16, 16))
        self._sym_toggle_btn.toggled.connect(self._on_sym_toggle)
        _StatusBarButtonSizer(
            self._statusbar,
            [self._tree_toggle_btn, self._diag_toggle_btn,
             self._sym_toggle_btn],
        )

        self.status_pos = QLabel("Ln 1, Col 1")
        self.status_lsp = QLabel("")
        self.status_python = QLabel("")
        # Версия Python видна только пока активен таб с питоновским кодом
        self.status_python.hide()
        self.status_lsp_spinner = _LspSpinner(self._statusbar)
        # Предупреждение о проблеме с запуском LSP-сервера (иконка + тултип
        # с причиной); показывается рядом с именем сервера.
        self.status_lsp_warn = QLabel(self._statusbar)
        self.status_lsp_warn.setFixedSize(16, 16)
        self.status_lsp_warn.hide()
        self._status_lsp_warn_text = ""

        # Счётчики ошибок/предупреждений — слева, сразу после кнопок: сначала
        # иконка error-bold.svg, затем число ошибок, затем warning-bold.svg и
        # число предупреждений. (В layout до addStretch — прижаты к левому
        # краю после квадратных кнопок-переключателей.)
        self.status_err_icon = QLabel(self._statusbar)
        self.status_err_icon.setFixedSize(14, 14)
        self.status_err_count = QLabel(self._statusbar)
        self.status_err_count.setToolTip("Errors in active file")
        self.status_warn_icon = QLabel(self._statusbar)
        self.status_warn_icon.setFixedSize(14, 14)
        self.status_warn_count = QLabel(self._statusbar)
        self.status_warn_count.setToolTip("Warnings in active file")

        status_layout = QHBoxLayout(self._statusbar)
        # Слева резервируем место под квадратные кнопки-переключатели (дерево,
        # диагностики, символы — 3 шт. × высота бара 30px = 90px). Разделитель
        # идёт за кнопками с отступом в 3px.
        status_layout.setContentsMargins(90 + 3, 0, 8, 0)
        status_layout.setSpacing(16)
        # Вертикальный разделитель — сразу после кнопок, перед счётчиками.
        # Вложенный layout: слева отступ 3px (margins), справа — 3px (spacing).
        self.status_sep = _StatusBarSeparator(self._statusbar)
        err_layout = QHBoxLayout()
        err_layout.setContentsMargins(0, 0, 0, 0)
        err_layout.setSpacing(4)
        err_layout.addWidget(self.status_err_icon, 0,
                             Qt.AlignmentFlag.AlignVCenter)
        err_layout.addWidget(self.status_err_count, 0,
                             Qt.AlignmentFlag.AlignVCenter)
        sep_layout = QHBoxLayout()
        sep_layout.setContentsMargins(0, 0, 0, 0)
        sep_layout.setSpacing(10)
        sep_layout.addWidget(self.status_sep, 0)
        sep_layout.addLayout(err_layout)
        warn_layout = QHBoxLayout()
        warn_layout.setContentsMargins(0, 0, 0, 0)
        warn_layout.setSpacing(4)
        warn_layout.addWidget(self.status_warn_icon, 0,
                              Qt.AlignmentFlag.AlignVCenter)
        warn_layout.addWidget(self.status_warn_count, 0,
                              Qt.AlignmentFlag.AlignVCenter)
        sep_layout.addLayout(warn_layout)
        status_layout.addLayout(sep_layout)
        status_layout.addStretch(1)  # прижимает блок к правому краю
        status_layout.addWidget(self.status_lsp_spinner, 0,
                                Qt.AlignmentFlag.AlignVCenter)
        status_layout.addWidget(self.status_lsp, 0, Qt.AlignmentFlag.AlignVCenter)
        status_layout.addWidget(self.status_lsp_warn, 0,
                                Qt.AlignmentFlag.AlignVCenter)
        status_layout.addWidget(self.status_pos, 0, Qt.AlignmentFlag.AlignVCenter)
        status_layout.addWidget(self.status_python, 0,
                                Qt.AlignmentFlag.AlignVCenter)

        # Высоту бара фиксируем: кнопка-квадрат занимает её целиком
        self._statusbar.setFixedHeight(30)

        self.update_python_status()

    def update_python_status(self):
        """Статус-бар: «Python 3.13.7» для выбранного интерпретатора."""
        from editor.python_env import selected_python
        try:
            info = selected_python()
            version = info.get("version") or ""
            self.status_python.setText(
                f"Python {version}" if version else "")
            self.status_python.setToolTip(info.get("path", ""))
        except Exception:
            self.status_python.setText("")

    def _lsp_starting(self, editor) -> bool:
        """LSP-сервер текущего редактора ещё стартует (спиннер в статус-баре)."""
        if not getattr(editor, "lsp_attached", False):
            return False
        try:
            return get_lsp_manager().server_ready(editor) is False
        except Exception:
            return False

    def _update_status(self):
        editor = self._get_current_editor()
        line = col = 1
        lsp = ""
        if editor:
            try:
                line, col = editor.getCursorPosition()
            except RuntimeError:
                pass
            if getattr(editor, "lsp_attached", False):
                lsp = status_lsp_text(editor)
            else:
                lsp = os.path.splitext(editor.file_path or "")[1]
        # Предупреждение о проблеме запуска LSP-сервера языка редактора
        # (проверяем вне кэша: ошибка появляется/исчезает асинхронно)
        self._update_lsp_warning(editor)
        # Спиннер обновляем вне кэша: готовность сервера меняется асинхронно
        if editor is not None:
            self.status_lsp_spinner.set_active(self._lsp_starting(editor))
        else:
            self.status_lsp_spinner.set_active(False)
        # Состояние кнопки диагностик тоже не входит в кэш: вкладка может
        # открыться/закрыться мимо кнопки (Ctrl+M, крестик, смена панели)
        self._sync_diagnostics_btn()
        # Состояние кнопки символов — аналогично (Ctrl+L, крестик, сессия)
        self._sync_symbols_btn()
        # Версию Python показываем только на питоновском табе
        py_visible = bool(
            editor and getattr(editor, "language_id", None) == "python")
        # Счётчики ошибок/предупреждений активного файла
        errs = warns = 0
        if editor is not None:
            try:
                errs = editor.lsp_error_count or 0
                warns = editor.lsp_warning_count or 0
            except RuntimeError:
                pass
        key = (id(editor), line, col, lsp, py_visible, errs, warns)
        if key == self._status_cache:
            return
        self._status_cache = key
        self.status_pos.setText(f"Ln {line+1}, Col {col+1}")
        self.status_lsp.setText(lsp)
        self.status_python.setVisible(py_visible)
        # Счётчики показываем только если есть проблемы: блок ошибок — при
        # ошибках, блок предупреждений — при предупреждениях. Нулевые блоки
        # скрыты, чтобы справа от разделителя не висели «0 0».
        self.status_err_count.setText(str(errs))
        self.status_warn_count.setText(str(warns))
        self._set_block_visible((self.status_err_icon, self.status_err_count),
                                errs > 0)
        self._set_block_visible((self.status_warn_icon, self.status_warn_count),
                                warns > 0)

    @staticmethod
    def _set_block_visible(widgets, visible: bool):
        """Показывает/прячет блок счётчика (иконка + число) целиком."""
        for w in widgets:
            if w.isVisible() != visible:
                w.setVisible(visible)

    def _update_lsp_warning(self, editor):
        """Показывает/прячет иконку-предупреждение с ошибкой запуска LSP."""
        error = ""
        if editor is not None:
            language_id = getattr(editor, "language_id", "")
            if language_id:
                try:
                    error = get_lsp_manager().error_for(language_id) or ""
                    # Ошибка бинаря может устареть: сервер установили/добавили
                    # в PATH уже после старта. Повторная проверка доступности
                    # сбросит ошибку, и предупреждение пропадёт само.
                    if error:
                        get_lsp_manager().server_available(language_id)
                        error = get_lsp_manager().error_for(language_id) or ""
                except Exception:
                    error = ""
        if not error:
            if self.status_lsp_warn.isVisible():
                self.status_lsp_warn.hide()
            return
        # Тултип обновляем только при смене текста ошибки (status timer идёт
        # каждые 200 мс, а setToolTip во время показа сбрасывает тултип)
        if error != self._status_lsp_warn_text:
            self._status_lsp_warn_text = error
            self.status_lsp_warn.setToolTip(error)
        if not self.status_lsp_warn.isVisible():
            self.status_lsp_warn.show()

    # ---- current editor ----

    def _get_current_editor(self) -> Optional[CodeEditor]:
        # Проверяем, не удалён ли active_editor
        ae = self.tile_manager.active_editor
        if ae is not None:
            try:
                ae.objectName()
                return ae
            except RuntimeError:
                self.tile_manager.active_editor = None

        pane = self.tile_manager.get_active_pane()
        if pane:
            current = pane.current_editor()
            if current is not None:
                return current
        return None

    # ---- actions ----

    def _new_file(self):
        pane = self.tile_manager.get_active_pane()
        if pane:
            pane.add_editor()

    def _toggle_diagnostics_tab(self):
        """Ctrl+M: вкладка списка диагностик (повтор — закрыть)."""
        pane = self.tile_manager.get_active_pane()
        if pane:
            pane.toggle_diagnostics_tab()
        # Иконка/состояние кнопки в статус-баре обновляем сразу (как кнопка
        # дерева при Ctrl+B), а не ждём следующий тик таймера (200 мс)
        self._sync_diagnostics_btn()

    def _on_diag_toggle(self, checked: bool):
        """Кнопка диагностик в статус-баре: открыть/закрыть список.

        Состояние кнопки отражает наличие вкладки в ЛЮБОЙ панели (не только
        в активной): при открытии не создаём дубль, а показываем
        существующую вкладку."""
        if checked:
            self._open_diagnostics()
        else:
            self._close_all_diagnostics()
        # Иконка: ярче при открытой вкладке, темнее при закрытой
        self._diag_toggle_btn.setIcon(self._diag_icon(checked))

    def _any_diagnostics_open(self) -> bool:
        """Открыта ли вкладка диагностик хотя бы в одной панели."""
        return any(p.has_diagnostics_tab()
                   for p in self.tile_manager.panes())

    def _open_diagnostics(self):
        """Открыть список диагностик: если вкладка уже есть в какой-то панели —
        просто показать её (без дублей), иначе создать в активной панели."""
        for pane in self.tile_manager.panes():
            idx = pane._find_diagnostics_tab()
            if idx is not None:
                pane.tabs.setCurrentWidget(pane.tabs.widget(idx))
                return
        pane = self.tile_manager.get_active_pane()
        if pane:
            pane.add_diagnostics_tab()

    def _close_all_diagnostics(self):
        """Закрыть вкладки диагностик во всех панелях."""
        for pane in list(self.tile_manager.panes()):
            try:
                while True:
                    idx = pane._find_diagnostics_tab()
                    if idx is None:
                        break
                    pane._close_tab(idx)
            except RuntimeError:
                # Панель удалена при закрытии последней вкладки
                break

    def _sync_diagnostics_btn(self):
        """Держим кнопку диагностик в синхроне с реальной вкладкой: таб может
        открыться/закрыться мимо кнопки (Ctrl+M, крестик, сессия, смена
        панели). Смотрим по всем панелям, а не только по активной."""
        active = self._any_diagnostics_open()
        btn = self._diag_toggle_btn
        if btn.isChecked() != active:
            # blockSignals: синхронизация не должна снова открывать/закрывать
            btn.blockSignals(True)
            btn.setChecked(active)
            btn.blockSignals(False)
            btn.setIcon(self._diag_icon(active))

    def _diag_icon(self, active: bool) -> QIcon:
        """Иконка кнопки диагностик: яркая при открытой вкладке, приглушённая —
        при закрытой."""
        alpha = 255 if active else 120
        return load_ui_icon("diagostics",
                            QColor(self._diag_icon_color), alpha)

    def _toggle_symbols_tab(self):
        """Ctrl+L: вкладка списка символов активного файла (повтор — закрыть)."""
        pane = self.tile_manager.get_active_pane()
        if pane:
            pane.toggle_symbols_tab()
        # Иконка/состояние кнопки в статус-баре обновляем сразу (как кнопка
        # диагностик при Ctrl+M), а не ждём следующий тик таймера (200 мс)
        self._sync_symbols_btn()

    def _on_sym_toggle(self, checked: bool):
        """Кнопка символов в статус-баре: открыть/закрыть список.

        Состояние отражает наличие вкладки в ЛЮБОЙ панели (как диагностики):
        при открытии не создаём дубль, а показываем существующую вкладку."""
        if checked:
            self._open_symbols()
        else:
            self._close_all_symbols()
        self._sym_toggle_btn.setIcon(self._sym_icon(checked))

    def _any_symbols_open(self) -> bool:
        """Открыта ли вкладка символов хотя бы в одной панели."""
        return any(p.has_symbols_tab()
                   for p in self.tile_manager.panes())

    def _open_symbols(self):
        """Открыть список символов: если вкладка уже есть в какой-то панели —
        просто показать её (без дублей), иначе создать в активной панели."""
        for pane in self.tile_manager.panes():
            idx = pane._find_symbols_tab()
            if idx is not None:
                pane.tabs.setCurrentWidget(pane.tabs.widget(idx))
                return
        pane = self.tile_manager.get_active_pane()
        if pane:
            pane.add_symbols_tab()

    def _close_all_symbols(self):
        """Закрыть вкладки символов во всех панелях."""
        for pane in list(self.tile_manager.panes()):
            try:
                while True:
                    idx = pane._find_symbols_tab()
                    if idx is None:
                        break
                    pane._close_tab(idx)
            except RuntimeError:
                # Панель удалена при закрытии последней вкладки
                break

    def _sync_symbols_btn(self):
        """Держим кнопку символов в синхроне с реальной вкладкой: таб может
        открыться/закрыться мимо кнопки (Ctrl+L, крестик, сессия)."""
        active = self._any_symbols_open()
        btn = self._sym_toggle_btn
        if btn.isChecked() != active:
            btn.blockSignals(True)
            btn.setChecked(active)
            btn.blockSignals(False)
            btn.setIcon(self._sym_icon(active))

    def _sym_icon(self, active: bool) -> QIcon:
        """Иконка кнопки символов: яркая при открытой вкладке, приглушённая —
        при закрытой."""
        alpha = 255 if active else 120
        return load_ui_icon("Symbols",
                            QColor(self._sym_icon_color), alpha)

    def _open_file(self):
        paths = themed_dialogs.open_file_names(
            self, "Open File", "",
            "All Supported (*.c *.cpp *.cxx *.cc *.h *.hpp *.cs "
            "*.py *.pyw *.java *.lm *.js *.jsx *.mjs *.json *.xml *.html "
            "*.htm *.svg *.yaml *.yml *.sql *.md *.markdown *.sh "
            "*.bash *.zsh *.lua *.rb *.mylang *.txt);;All Files (*)",
            self.theme_manager, self.current_theme,
        )
        if paths:
            pane = self.tile_manager.get_active_pane()
            for p in paths:
                pane.add_editor(file_path=p)

    def _open_folder(self):
        self.file_tree.choose_folder()

    # ---- recent folders ----

    MAX_RECENT_FOLDERS = 10

    def _load_recent_folders(self) -> list:
        settings = QSettings()
        value = settings.value("recentFolders", [])
        if isinstance(value, str):
            value = [value]
        return [p for p in value if os.path.isdir(p)]

    def _save_recent_folders(self):
        QSettings().setValue("recentFolders", self._recent_folders)

    def _on_folder_opened(self, path: str):
        """Папка открыта (диалогом или из Recent) — поднимаем её в начало."""
        if path in self._recent_folders:
            self._recent_folders.remove(path)
        self._recent_folders.insert(0, path)
        del self._recent_folders[self.MAX_RECENT_FOLDERS:]
        self._save_recent_folders()
        self._menu_builder.populate_recent(
            self._recent_folders, self._open_recent_folder
        )
        # Корень воркспейса для clangd (база для compile_commands.json)
        self.lsp_manager.set_workspace_root(path)

    def _open_recent_folder(self, path: str):
        self.file_tree.set_root_path(path)

    def _open_path(self, path: str):
        if not path:
            return
        pane = self.tile_manager.get_active_pane()
        if pane:
            pane.add_editor(file_path=path)

    def _open_file_finder(self):
        """Ctrl+Shift+F: поиск файлов в открытой папке."""
        root = self.file_tree.root_path
        if not root:
            return
        popup = FileFindPopup(
            root, self.theme_manager, self.current_theme,
            self._open_finder_file, self,
        )
        popup.adjustSize()
        geo = self.geometry()
        popup.move(geo.center().x() - popup.width() // 2, geo.top() + 100)
        popup.show()
        self._file_finder_popup = popup  # держим ссылку, пока попап открыт

    def _open_finder_file(self, path: str):
        """Открывает выбранный в поиске файл и передаёт ему фокус."""
        self._open_path(path)
        editor = self._get_current_editor()
        if editor is not None:
            editor.setFocus()

    def _open_bookmark_finder(self):
        """Ctrl+Shift+B: поиск по всем закладкам проекта."""
        entries = collect_bookmarks()
        popup = BookmarkFindPopup(
            entries, self.theme_manager, self.current_theme,
            self._open_bookmark, self,
        )
        popup.adjustSize()
        geo = self.geometry()
        popup.move(geo.center().x() - popup.width() // 2, geo.top() + 100)
        popup.show()
        self._bookmark_finder_popup = popup  # держим ссылку, пока попап открыт

    def _open_bookmark(self, path: str, line: int):
        """Переходит к закладке: открывает файл (если закрыт) и прыгает
        на строку с показом плашки-пояснения."""
        self._open_path(path)
        editor = self._get_current_editor()
        if editor is None:
            return
        editor.setCursorPosition(line, 0)
        try:
            editor.ensureLineVisible(line)
        except Exception:
            pass
        editor.setFocus()
        note = ""
        try:
            note = editor._bookmark_note_at_line(line) or ""
        except Exception:
            note = ""
        if note and hasattr(editor, "_show_bookmark_tooltip"):
            try:
                editor._show_bookmark_tooltip(note, line)
            except Exception:
                pass

    def _on_file_renamed(self, old_path: str, new_path: str):
        """Файл/папка переименованы — перенацеливаем вкладки на новый путь."""
        is_dir = os.path.isdir(new_path)
        for pane in self.tile_manager._panes:
            pane.retarget_tabs_with_path(old_path, new_path, is_dir)

    def _toggle_file_tree(self, visible: bool):
        self.file_tree.setVisible(visible)
        # Держим в синхроне пункт меню и кнопку в статус-баре
        self._menu_builder.toggle_tree_action.setChecked(visible)
        self._tree_toggle_btn.setChecked(visible)
        # Иконка: ярче при открытом дереве, темнее при закрытом
        self._tree_toggle_btn.setIcon(self._tree_icon(visible))
        if visible:
            self._root_splitter.setSizes(
                [250, max(400, self._root_splitter.width() - 250)]
            )

    def _tree_icon(self, active: bool) -> QIcon:
        """Иконка кнопки дерева: яркая при открытом дереве, приглушённая —
        при закрытом."""
        alpha = 255 if active else 120
        return load_ui_icon("path_tree", QColor(self._tree_icon_color), alpha)

    def _save_file(self):
        editor = self._get_current_editor()
        if editor:
            if editor.file_path:
                if editor.save_file():
                    self._update_tab_title(editor)
            else:
                self._save_as()

    def _save_as(self):
        editor = self._get_current_editor()
        if not editor:
            return
        path = themed_dialogs.save_file_name(
            self, "Save As", "", self.theme_manager, self.current_theme,
        )
        if path:
            if editor.save_file(path):
                # Лексер пересоздаём только если язык файла реально изменился
                # (пересоздание сбрасывает внутреннее состояние лексера:
                # фолдинг, собранные struct/define). Позицию курсора и
                # прокрутку сохраняем на случай смены лексера.
                new_lang = editor.lexer_manager.get_language_id(path)
                if new_lang != editor.language_id:
                    cur_line, cur_idx = editor.getCursorPosition()
                    sb_value = editor.verticalScrollBar().value()
                    try:
                        editor.set_language(new_lang)
                    except Exception:
                        pass
                    editor.setCursorPosition(cur_line, cur_idx)
                    editor.verticalScrollBar().setValue(sb_value)
                editor.apply_theme(self.current_theme)
                self._update_tab_title(editor)

    @staticmethod
    def _update_tab_title(editor: CodeEditor):
        tab_widget = editor.find_tab_widget()
        if tab_widget is not None:
            # Редактор лежит в контейнере вкладки (_EditorHost) — ищем его
            for i in range(tab_widget.count()):
                w = tab_widget.widget(i)
                if getattr(w, "editor", None) is editor:
                    tab_widget.setTabText(
                        i, os.path.basename(editor.file_path)
                    )
                    break
            # tabData (drag&drop) обновляем без копии буфера: свежий
            # контент снимут при старте драга (tab_bar.mouseMoveEvent)
            sync_tab_data(editor, include_content=False)

    def _goto_definition(self):
        """F12: определение символа под курсором (через clangd)."""
        editor = self._get_current_editor()
        if editor is not None:
            editor.lsp_go_to_definition()

    def _rename_symbol(self):
        """F2: переименование символа под курсором (через LSP)."""
        editor = self._get_current_editor()
        if editor is not None:
            editor.lsp_rename()

    def _check_lisp(self):
        """Ctrl+Shift+L: Common Lisp — проверить файл через SBCL."""
        editor = self._get_current_editor()
        if editor is not None:
            editor.lsp_check_lisp()

    def _cycle_tab(self, previous: bool = False):
        """Ctrl+Tab — следующая вкладка активной панели, Ctrl+Shift+Tab —
        предыдущая (с заворотом через край)."""
        pane = self.tile_manager.get_active_pane()
        if not pane or pane.tabs.count() <= 1:
            return
        idx = pane.tabs.currentIndex()
        step = -1 if previous else 1
        pane.tabs.setCurrentIndex((idx + step) % pane.tabs.count())

    def eventFilter(self, obj, event):
        """App-level Ctrl+Left/Ctrl+Right: переключение вкладок везде, кроме
        модальных окон и открытых меню (фильтр установлен на QApplication
        в __init__). В редакторе Ctrl+Left/Right съедаются здесь — переход по
        словам перенесён на Alt+Left/Right (см. code_editor._handle_word_move)."""
        if (event.type() == QEvent.Type.KeyPress
                and self._is_tab_switch_keys(event)
                and QApplication.activeModalWidget() is None
                and QApplication.activePopupWidget() is None):
            self._cycle_tab(previous=(event.key() == Qt.Key.Key_Left))
            return True
        return super().eventFilter(obj, event)

    @staticmethod
    def _is_tab_switch_keys(event) -> bool:
        """Чистый Ctrl+Left/Ctrl+Right (без Shift/Alt и пр.)."""
        return (event.key() in (Qt.Key.Key_Left, Qt.Key.Key_Right)
                and event.modifiers() == Qt.KeyboardModifier.ControlModifier)

    def _close_current_pane(self):
        """Закрывает активную панель; несохранённые файлы — через диалог.

        Раньше панель закрывалась молча и несохранённые изменения терялись.
        """
        pane = self.tile_manager.get_active_pane()
        if not pane or len(self.tile_manager._panes) <= 1:
            return
        dirty = [ed for ed in pane.get_all_editors()
                 if ed.isModified() or not ed.file_path]
        if dirty:
            names = "\n".join(
                f"• {ed.file_path or 'Untitled'}" for ed in dirty)
            reply = themed_dialogs.question(
                self, "Close Pane",
                f"Unsaved changes in:\n{names}\n\nSave them?",
                themed_dialogs.StandardButton.Yes
                | themed_dialogs.StandardButton.No
                | themed_dialogs.StandardButton.Cancel,
                themed_dialogs.StandardButton.Cancel,
                self.theme_manager, self.current_theme,
            )
            if reply == themed_dialogs.StandardButton.Cancel:
                return
            if reply == themed_dialogs.StandardButton.Yes:
                for ed in dirty:
                    if not ed.save_file():
                        return  # не смогли сохранить — панель не трогаем
        self.tile_manager.close_pane(pane)

    # ---- терминал (вкладки-консоли в панелях редакторов) ----

    def _new_terminal(self):
        """Добавляет вкладку консоли в первую панель редакторов (Ctrl+`)."""
        if not self.tile_manager._panes:
            return
        terminal = self.tile_manager._panes[0].add_terminal()
        terminal.focus_input()

    def _shutdown_terminals(self):
        """Останавливает оболочки терминалов (при закрытии окна)."""
        for pane in self.tile_manager._panes:
            for i in range(pane.tabs.count()):
                widget = pane.tabs.widget(i)
                if isinstance(widget, TerminalWidget):
                    widget.shutdown()

    def closeEvent(self, event):
        save_session(self)
        # Попап поиска файлов: закрываем до выхода, чтобы фоновое
        # сканирование остановилось и не уронило процесс при разрушении
        popup = getattr(self, "_file_finder_popup", None)
        if popup is not None:
            try:
                popup.close()
            except RuntimeError:
                pass  # попап уже удалён
        self._shutdown_terminals()
        # Окна-деточки (перетащенные табы) закрываем до выхода: у каждого
        # свой TileManager и свои терминалы
        for win in all_detached_windows():
            try:
                win.close()
            except RuntimeError:
                pass
        self.lsp_manager.shutdown_all()
        super().closeEvent(event)

    def _zoom_in(self):
        editor = self._get_current_editor()
        if editor:
            editor.zoomIn()

    def _zoom_out(self):
        editor = self._get_current_editor()
        if editor:
            editor.zoomOut()

    def _zoom_reset(self):
        editor = self._get_current_editor()
        if editor:
            editor.zoomTo(0)

    # ---- поиск по файлу ----

    def _find_in_file(self):
        pane = self.tile_manager.get_active_pane()
        if pane:
            pane.toggle_find_bar()

    def _find_next_in_file(self):
        pane = self.tile_manager.get_active_pane()
        if pane:
            pane.find_next()

    def _find_prev_in_file(self):
        pane = self.tile_manager.get_active_pane()
        if pane:
            pane.find_previous()

    def _show_about(self):
        themed_dialogs.about(
            self, "About LumenIDE",
            "LumenIDE\n\n"
            "A tile-based code editor built with PyQt6 and QScintilla,\n"
            "with built-in support for the Lumen language (.lm).\n\n"
            "Supports multiple languages, split panes and custom themes.",
            self.theme_manager, self.current_theme,
        )

    def _open_settings(self):
        """Открывает панель настроек (кнопка шестерёнки в тайтл-баре)."""
        open_settings(self)

    # ---- theme application ----

    def apply_theme_to_all(self, theme_name: str):
        self.current_theme = theme_name
        # Тему, выбранную в меню или панели настроек, сразу запоминаем в
        # settings.json: иначе при старте apply_settings перезапишет её
        # сохранённым themes.current (в сессии она лежит только на время
        # закрытия и теряется, если themes.current остался от прошлого раза).
        try:
            app_settings().set("themes.current", theme_name)
            app_settings().save()
        except Exception:
            pass
        self.tile_manager.apply_theme_to_all(theme_name)
        self.spotlight.set_theme(theme_name)
        self._menu_builder.update_theme_checks(theme_name)

        theme_data = self.theme_manager.themes.get(theme_name, {})
        colors = theme_data.get("colors", {})
        tc = ThemeColors.from_theme_manager(self.theme_manager, theme_name)
        bg, fg, border_color = tc.bg, tc.fg, tc.border

        # Полоска-разделитель при перетаскивании — QRubberBand, рисуется
        # цветами WindowText/Base из палитры сплиттера (CE_RubberBand).
        accent = QColor(tc.selection) if tc.selection else QColor(fg)
        pal = self._root_splitter.palette()
        pal.setColor(QPalette.ColorRole.WindowText, accent)
        pal.setColor(QPalette.ColorRole.Base, accent)
        self._root_splitter.setPalette(pal)

        # Полупрозрачные оверлеи hover/pressed строим из цвета текста темы,
        # а не из белого: на светлом фоне белые оверлеи невидимы.
        fg_color = QColor(fg)  # для separator/hover rgba в QSS
        menu_hover = (f"rgba({fg_color.red()},{fg_color.green()},"
                      f"{fg_color.blue()},38)")    # ≈ 0.15
        menu_pressed = (f"rgba({fg_color.red()},{fg_color.green()},"
                        f"{fg_color.blue()},26)")   # ≈ 0.10
        menu_item_hover = (f"rgba({fg_color.red()},{fg_color.green()},"
                           f"{fg_color.blue()},30)")  # ≈ 0.12

        # DWM dark/light mode for native scrollbars (Windows 11).
        # Переключаем атрибут только при реальной смене режима — вызов
        # DwmSetWindowAttribute заставляет Windows перерисовать системные
        # элементы окна (скроллбары и т.п.).
        bg_color = QColor(bg)
        lum = (
            bg_color.red() * 299
            + bg_color.green() * 587
            + bg_color.blue() * 114
        ) / 1000
        dark_mode = lum < 128
        if dark_mode != self._dark_mode_active:
            self._dark_mode_active = dark_mode
            self._dwm_set_attribute(20, 1 if dark_mode else 0)

        # Style the library's title bar
        tb = self.titleBar
        title_bg = tc.title_bg or bg
        tb.setStyleSheet(f"""
            StandardTitleBar {{
                background-color: {title_bg};
            }}
            StandardTitleBar QMenuBar {{
                background: transparent;
                border: none;
                padding: 0;
                font-size: {MENU_FONT_SIZE}pt;
            }}
            StandardTitleBar QMenuBar::item {{
                color: {fg};
                padding: 7px 10px 5px 10px;
                background: transparent;
                font-size: {MENU_FONT_SIZE}pt;
            }}
            StandardTitleBar QMenuBar::item:selected {{
                background-color: {menu_hover};
            }}
            StandardTitleBar QMenuBar::item:pressed {{
                background-color: {menu_pressed};
            }}
        """)

        # Title label
        tb.titleLabel.setStyleSheet(f"""
            QLabel {{
                color: {fg};
                font-family: '{ui_font_family()}';
                font-size: {TITLE_FONT_SIZE}pt;
                background: transparent;
                border: none;
                padding-left: 7px;
            }}
        """)

        # Логотип в тайтл-баре — в цвете текста темы
        self._refresh_titlebar_icon(fg)

        # Button colors
        tb.minBtn.setNormalColor(QColor(fg))
        tb.maxBtn.setNormalColor(QColor(fg))
        tb.closeBtn.setNormalColor(QColor(fg))

        tb.minBtn.setHoverColor(QColor(fg))
        tb.maxBtn.setHoverColor(QColor(fg))
        tb.closeBtn.setHoverColor(QColor(fg))

        # В библиотеке pressed-цвет иконок по умолчанию чёрный —
        # при зажатии кнопки сворачивания/минимизации иконка чернела
        tb.minBtn.setPressedColor(QColor(fg))
        tb.maxBtn.setPressedColor(QColor(fg))

        hover_bg = QColor(fg)
        hover_bg.setAlpha(30)
        tb.minBtn.setHoverBackgroundColor(hover_bg)
        tb.maxBtn.setHoverBackgroundColor(hover_bg)
        tb.closeBtn.setHoverBackgroundColor(QColor(232, 17, 35))

        # Global stylesheet
        menu_bg = colors.get("menu_background",
                       colors.get("background", FALLBACK_BG))
        menu_fg = colors.get("menu_foreground",
                       colors.get("foreground", FALLBACK_FG))
        # Неактивные пункты меню — блёклый полупрозрачный текст
        menu_fg_color = QColor(menu_fg)
        menu_fg_disabled = (f"rgba({menu_fg_color.red()},"
                            f"{menu_fg_color.green()},"
                            f"{menu_fg_color.blue()},120)")

        # QSS попапов QMenu. Попапы — отдельные top-level окна и НЕ наследуют
        # stylesheet главного окна, поэтому стиль задаём каждому меню напрямую.
        menu_qss = f"""
            QMenu {{
                background-color: {menu_bg};
                border: 0.5px solid {border_color};
                border-radius: 8px;
                padding: 4px;
            }}
            QMenu::item {{
                color: {menu_fg};
                padding: 4px 24px 4px 12px;
                border-radius: 6px;
            }}
            QMenu::item:disabled {{
                color: {menu_fg_disabled};
            }}
            QMenu::item:selected {{
                background-color: {menu_item_hover};
            }}
            QMenu::separator {{
                height: 1px;
                background: rgba({fg_color.red()},{fg_color.green()},{fg_color.blue()},48);
                margin: 4px 8px;
            }}
        """
        self._menu_qss = menu_qss
        self._menu_builder.set_menu_qss(menu_qss)
        self.file_tree.set_menu_qss(menu_qss)

        # Фон главного окна — палитрой вместо глобального QSS: setStyleSheet
        # на окне заставлял Qt переполировать ВСЁ дерево виджетов при каждой
        # смене темы (лавина перерисовок, FPS просаживается на секунды).
        # Меню, таб-бары, панели и статус-бар красятся локально — каждый
        # своим QSS/палитрой, без каскада на соседей.
        pal = self.palette()
        pal.setColor(QPalette.ColorRole.Window, QColor(bg))
        self.setPalette(pal)

        # Стандартные диалоги (QMessageBox/QInputDialog/QFileDialog)
        # стилизуются при создании в themed_dialogs.install_titlebar —
        # НЕ через QApplication.setStyleSheet: смена app-QSS заставляет Qt
        # переполировать ВСЕ виджеты приложения (~900 мс на переключение
        # темы), а QSS диалогов нужен только самому диалогу при открытии.

        # Шрифты больше не задаются глобальным QSS окна (это заставляло Qt
        # переполировывать все виджеты и пересоздавать шрифты при каждой
        # смене темы) — они заданы один раз через app.setFont в main(),
        # поэтому зум терминалов сбрасываться не может.

        # Tab bar styling: перенесено в TileManager.apply_theme_to_all —
        # тема к панелям (редакторы + обвязка) применяется одним проходом.

        # File tree styling
        self.file_tree.apply_theme(theme_name)

        # Статус-бар: фон и цвет текста — ТОЛЬКО палитра. QSS на виджете до
        # показа окна запекает цвет ПЕРВОГО apply (Qt полишит виджет один раз),
        # и второй setStyleSheet/setPalette его не переписывают — стартовая
        # заливка оставалась от дефолтной темы (Monokai) при сессии с другой
        # темой. Палитра переживает и двойной pre-show apply, и смену темы
        # в рантайме. autoFill нужен: у голого QWidget без QSS фон не рисуется.
        self._statusbar.setAutoFillBackground(True)
        sb_pal = self._statusbar.palette()
        sb_pal.setColor(QPalette.ColorRole.Window, QColor(bg))
        sb_pal.setColor(QPalette.ColorRole.WindowText, QColor(fg))
        self._statusbar.setPalette(sb_pal)
        for lbl in (self.status_pos, self.status_lsp, self.status_python,
                    self.status_err_count, self.status_warn_count):
            lp = lbl.palette()
            lp.setColor(QPalette.ColorRole.WindowText, QColor(fg))
            lbl.setPalette(lp)
        self.status_lsp_spinner.set_color(QColor(fg))
        # Вертикальный разделитель статус-бара в цвете границ темы
        self.status_sep.set_color(QColor(border_color))
        # Иконка предупреждения LSP в цветах темы
        self.status_lsp_warn.setPixmap(
            load_ui_icon("warning-bold", QColor(fg)).pixmap(16, 16))
        # Иконки счётчиков ошибок/предупреждений в цветах темы
        self.status_err_icon.setPixmap(
            load_ui_icon(
                "error-bold",
                tc.error_color or QColor(fg)).pixmap(14, 14))
        self.status_warn_icon.setPixmap(
            load_ui_icon(
                "warning-bold",
                tc.warning_color or QColor(fg)).pixmap(14, 14))

        #  нопка показа/скрытия дерева в статус-баре
        self._tree_icon_color = QColor(fg)
        tree_hover = QColor(fg)
        tree_hover.setAlpha(20)
        self._tree_toggle_btn.set_hover_color(tree_hover)
        self._tree_toggle_btn.setIcon(
            self._tree_icon(self._tree_toggle_btn.isChecked())
        )

        # Кнопка показа/скрытия диагностик в статус-баре (как кнопка дерева)
        self._diag_icon_color = QColor(fg)
        self._diag_toggle_btn.set_hover_color(tree_hover)
        self._diag_toggle_btn.setIcon(
            self._diag_icon(self._diag_toggle_btn.isChecked())
        )

        # Кнопка показа/скрытия списка символов в статус-баре (как кнопка
        # диагностик)
        self._sym_icon_color = QColor(fg)
        self._sym_toggle_btn.set_hover_color(tree_hover)
        self._sym_toggle_btn.setIcon(
            self._sym_icon(self._sym_toggle_btn.isChecked())
        )

        # Кнопка настроек в тайтл-баре: иконка и подсветки в цветах темы
        settings_hover = QColor(fg)
        settings_hover.setAlpha(30)
        settings_press = QColor(fg)
        settings_press.setAlpha(60)
        self._settings_btn.set_hover_color(settings_hover)
        self._settings_btn.set_pressed_color(settings_press)
        self._settings_btn.setIcon(load_ui_icon("settings", QColor(fg)))

        # Окна-деточки (перетащенные табы) перекрашиваем той же темой
        for win in all_detached_windows():
            try:
                win.apply_theme(theme_name)
            except RuntimeError:
                pass


# ----------------------------------------------------------------------
# Точка входа
# ----------------------------------------------------------------------
def main():
    app = QApplication(sys.argv)
    # Без этого атрибута вложенные виджеты FramelessDialog (QLineEdit и др.)
    # получают нативные окна-сиблинги: после закрытия диалога у главного
    # окна залипает курсор-каретка и ломается resize за края (issue #50/#62
    # qframelesswindow).
    app.setAttribute(
        Qt.ApplicationAttribute.AA_DontCreateNativeWidgetSiblings
    )
    # Иконка приложения (окно, панель задач, alt-tab).
    app.setWindowIcon(app_icon())
    # app.setApplicationName("LumenID")
    # app.setOrganizationName("Lumen Coorperativ")

    # Шрифт интерфейса задаётся один раз и наследуется всеми виджетами.
    # Раньше он жил в глобальном QSS окна (QWidget { font-family; font-size })
    # и при каждой смене темы Qt переполировывал ВСЕ виджеты и пересоздавал
    # шрифты — первые секунды после смены темы каждое взаимодействие
    # дёргало эту работу (просадка FPS), пока полировка не завершится.
    app.setFont(QFont(ui_font_family(), UI_FONT_SIZE))

    # Плавный hover кнопок тайтл-бара (min/max/close) библиотеки
    # qframelesswindow — патчим класс до создания окон.
    install_titlebar_button_hover_fade()

    window = MainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
