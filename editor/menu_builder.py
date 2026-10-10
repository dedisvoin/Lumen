"""
Меню тайтл-бара (File/Editor/Help) — построение вынесено из MainWindow.

Окно остаётся владельцем сервисов (редакторы, дерево, таски, терминалы);
этот модуль отвечает только за пункты меню, их шорткаты и подменю Recent/Theme.
"""

import os

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QAction
from PyQt6.QtWidgets import QMenu

from editor.shortcuts import bind_shortcut


class MenuBuilder:
    """Строит меню и хранит действия, к которым обращается окно.

    populate_recent() и populate_themes() пересобирают подменю, когда
    меняется список недавних папок или текущая тема.
    """

    def __init__(self, window):
        self.window = window
        self.recent_menu: QMenu = None
        self.theme_menu: QMenu = None
        self.toggle_tree_action: QAction = None
        self.terminal_action: QAction = None
        self.task_create_action: QAction = None
        self.task_run_action: QAction = None
        self.task_last_action: QAction = None

    def build(self):
        """Создаёт все меню и подменю (вызывается один раз после __init__)."""
        w = self.window
        menubar = w._menubar

        file_menu = menubar.addMenu("File")

        new_action = QAction("New", w)
        new_action.triggered.connect(w._new_file)
        bind_shortcut(new_action, "file.new")
        file_menu.addAction(new_action)

        open_action = QAction("Open...", w)
        open_action.triggered.connect(w._open_file)
        bind_shortcut(open_action, "file.open")
        file_menu.addAction(open_action)

        open_folder_action = QAction("Open Folder...", w)
        open_folder_action.triggered.connect(w._open_folder)
        file_menu.addAction(open_folder_action)

        file_menu.addSeparator()

        self.recent_menu = file_menu.addMenu("Recent")

        save_action = QAction("Save", w)
        save_action.triggered.connect(w._save_file)
        bind_shortcut(save_action, "file.save")
        file_menu.addAction(save_action)

        save_as_action = QAction("Save As...", w)
        save_as_action.triggered.connect(w._save_as)
        file_menu.addAction(save_as_action)

        file_menu.addSeparator()

        close_pane_action = QAction("Close Pane", w)
        close_pane_action.triggered.connect(w._close_current_pane)
        bind_shortcut(close_pane_action, "file.close_pane")
        file_menu.addAction(close_pane_action)

        file_menu.addSeparator()

        exit_action = QAction("Exit", w)
        exit_action.triggered.connect(w.close)
        file_menu.addAction(exit_action)

        view_menu = menubar.addMenu("Editor")

        zoom_in_action = QAction("Zoom In", w)
        zoom_in_action.triggered.connect(w._zoom_in)
        # Шорткаты + и - убраны, чтобы не конфликтовать с набором текста
        view_menu.addAction(zoom_in_action)

        zoom_out_action = QAction("Zoom Out", w)
        zoom_out_action.triggered.connect(w._zoom_out)
        view_menu.addAction(zoom_out_action)

        zoom_reset_action = QAction("Zoom Reset", w)
        zoom_reset_action.triggered.connect(w._zoom_reset)
        bind_shortcut(zoom_reset_action, "view.zoom_reset")
        view_menu.addAction(zoom_reset_action)

        view_menu.addSeparator()
        # Поиск по файлу: Ctrl+F — панель, F3/Shift+F3 — следующее/предыдущее
        find_action = QAction("Find", w)
        find_action.triggered.connect(w._find_in_file)
        bind_shortcut(find_action, "view.find")
        view_menu.addAction(find_action)

        find_next_action = QAction("Find Next", w)
        find_next_action.triggered.connect(w._find_next_in_file)
        bind_shortcut(find_next_action, "view.find_next")
        view_menu.addAction(find_next_action)

        find_prev_action = QAction("Find Previous", w)
        find_prev_action.triggered.connect(w._find_prev_in_file)
        bind_shortcut(find_prev_action, "view.find_previous")
        view_menu.addAction(find_prev_action)

        # Поиск файла в открытой папке: Ctrl+Shift+F
        find_file_action = QAction("Find File in Folder...", w)
        find_file_action.triggered.connect(w._open_file_finder)
        bind_shortcut(find_file_action, "view.find_file")
        view_menu.addAction(find_file_action)
        w.addAction(find_file_action)

        # Поиск по закладкам проекта: Ctrl+Shift+B
        bookmark_action = QAction("Go to Bookmark...", w)
        bookmark_action.triggered.connect(w._open_bookmark_finder)
        bind_shortcut(bookmark_action, "bookmarks.find")
        view_menu.addAction(bookmark_action)
        w.addAction(bookmark_action)

        view_menu.addSeparator()
        # Список диагностик (ошибки/предупреждения) — как Problems в VS Code
        diagnostics_action = QAction("Diagnostics", w)
        diagnostics_action.triggered.connect(w._toggle_diagnostics_tab)
        bind_shortcut(diagnostics_action, "view.diagnostics")
        view_menu.addAction(diagnostics_action)

        # Список символов активного файла — как Outline в VS Code
        symbols_action = QAction("Symbols", w)
        symbols_action.triggered.connect(w._toggle_symbols_tab)
        bind_shortcut(symbols_action, "view.symbols")
        view_menu.addAction(symbols_action)

        view_menu.addSeparator()
        # Переключение дерева — кнопкой в статус-баре; пункт меню из
        # тайтл-бара убран, но шорткат Ctrl+B сохранён.
        self.toggle_tree_action = QAction("Toggle File Tree", w)
        self.toggle_tree_action.setCheckable(True)
        self.toggle_tree_action.setChecked(True)
        bind_shortcut(self.toggle_tree_action, "view.toggle_tree")
        self.toggle_tree_action.triggered.connect(w._toggle_file_tree)
        # В PyQt6 QAction(widget) не добавляется в actions() виджета сам —
        # регистрируем явно, чтобы шорткат Ctrl+B работал без пункта меню
        w.addAction(self.toggle_tree_action)

        view_menu.addSeparator()
        self.theme_menu = view_menu.addMenu("Theme")

        view_menu.addSeparator()


        view_menu.addSeparator()
        # Терминал: новая вкладка консоли в первой панели (Ctrl+`)
        self.terminal_action = QAction("New Terminal", w)
        bind_shortcut(self.terminal_action, "view.new_terminal")
        self.terminal_action.triggered.connect(w._new_terminal)
        view_menu.addAction(self.terminal_action)
        # Регистрируем в окне, чтобы шорткат Ctrl+` работал (см. toggle_tree_action)
        w.addAction(self.terminal_action)

        # Таски в отдельном меню Run: Ctrl+Alt+T — создать,
        # Alt+Shift+T — список, Alt+T — повторить последний
        run_menu = menubar.addMenu("Run")
        self.task_create_action = QAction("New Task...", w)
        bind_shortcut(self.task_create_action, "run.task_create")
        self.task_create_action.triggered.connect(w._tasks.create)
        run_menu.addAction(self.task_create_action)
        w.addAction(self.task_create_action)

        self.task_run_action = QAction("Run Task...", w)
        bind_shortcut(self.task_run_action, "run.task_run")
        self.task_run_action.triggered.connect(w._tasks.run_list)
        run_menu.addAction(self.task_run_action)
        w.addAction(self.task_run_action)

        self.task_last_action = QAction("Rerun Last Task", w)
        bind_shortcut(self.task_last_action, "run.task_last")
        self.task_last_action.triggered.connect(w._tasks.run_last)
        run_menu.addAction(self.task_last_action)
        w.addAction(self.task_last_action)

        help_menu = menubar.addMenu("Help")
        about_action = QAction("About", w)
        about_action.triggered.connect(w._show_about)
        help_menu.addAction(about_action)

        # Подменю наполняются текущими данными окна
        self.populate_recent(w._recent_folders, w._open_recent_folder)
        self.populate_themes(w.apply_theme_to_all)

        # Windows: без прозрачного фона окно меню заливает углы непрозрачным
        # фоном, и нижний правый угол (запас под тень) остаётся квадратным
        for menu in self._iter_menus(menubar):
            menu.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)

    @staticmethod
    def _iter_menus(root):
        """Все QMenu и подменю, начиная с root (меню-бар или QMenu)."""
        for action in root.actions():
            menu = action.menu()
            if menu is not None:
                yield menu
                yield from MenuBuilder._iter_menus(menu)

    def set_menu_qss(self, qss: str):
        """Задаёт QSS попапам QMenu.

        Попапы — отдельные top-level окна и не наследуют stylesheet
        главного окна, поэтому стиль (фон/рамка/скругление) применяется
        каждому меню и подменю напрямую.
        """
        for menu in self._iter_menus(self.window._menubar):
            menu.setStyleSheet(qss)

    def populate_recent(self, folders, on_open):
        """Пересобирает подменю Recent: путь после \\t — справа от названия."""
        self.recent_menu.clear()
        if not folders:
            empty_action = QAction("(No recent folders)", self.window)
            empty_action.setEnabled(False)
            self.recent_menu.addAction(empty_action)
            return
        for path in folders:
            # "Название" слева, путь — справа (Qt выравнивает текст после \t)
            name = os.path.basename(path.rstrip("/\\")) or path
            action = QAction(f"{name}\t{path}", self.window)
            action.triggered.connect(
                lambda checked, p=path: on_open(p)
            )
            self.recent_menu.addAction(action)

    def populate_themes(self, on_apply):
        """Пересобирает подменю Theme из списка тем окна."""
        self.theme_menu.clear()
        for name in self.window.theme_manager.get_theme_names():
            action = QAction(name, self.window)
            action.setCheckable(True)
            action.triggered.connect(
                lambda checked, n=name: on_apply(n)
            )
            self.theme_menu.addAction(action)
        self.update_theme_checks(self.window.current_theme)

    def update_theme_checks(self, current: str):
        """Отмечает галочкой текущую тему в подменю Theme."""
        for action in self.theme_menu.actions():
            action.setChecked(action.text() == current)
