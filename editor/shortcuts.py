"""
Настраиваемые горячие клавиши.

Единый реестр шорткатов: id -> строка QKeySequence по умолчанию. Меню
(menu_builder.py), окна-деточки (detached_window.py) и действия в main.py
создают QAction с атрибутом _shortcut_id и значением shortcut_key(id) —
шорткат берётся из настроек (ключ shortcuts.<id>), а не зашит строкой.

apply_shortcuts(window) (settings_apply.py) переприменяет все шорткаты
окна после Apply/OK в панели настроек (страница Shortcuts).
"""

from PyQt6.QtGui import QKeySequence

# id -> шорткат по умолчанию (строка QKeySequence)
SHORTCUT_DEFAULTS: dict = {
    "file.new": "Ctrl+N",
    "file.open": "Ctrl+O",
    "file.save": "Ctrl+S",
    "file.close_pane": "Ctrl+W",
    "view.zoom_reset": "Ctrl+0",
    "view.find": "Ctrl+F",
    "view.find_next": "F3",
    "view.find_previous": "Shift+F3",
    "view.find_file": "Ctrl+Shift+F",
    "bookmarks.find": "Ctrl+Shift+B",
    "view.diagnostics": "Ctrl+M",
    "view.symbols": "Ctrl+Q",
    "view.toggle_tree": "Ctrl+B",
    "view.new_terminal": "Ctrl+`",
    "view.cycle_tab": "Ctrl+Tab",
    "view.cycle_tab_prev": "Ctrl+Shift+Tab",
    "view.goto_definition": "F12",
    "view.rename_symbol": "F2",
    "view.check_lisp": "Ctrl+Shift+L",
    "run.task_create": "Ctrl+Alt+T",
    "run.task_run": "Alt+Shift+T",
    "run.task_last": "Alt+T",
}

# Строки страницы Shortcuts в панели настроек (порядок = порядок в списке)
SHORTCUT_ITEMS: list = [
    {"id": "file.new", "label": "New File"},
    {"id": "file.open", "label": "Open File"},
    {"id": "file.save", "label": "Save File"},
    {"id": "file.close_pane", "label": "Close Pane"},
    {"id": "view.zoom_reset", "label": "Reset Zoom"},
    {"id": "view.find", "label": "Find in File"},
    {"id": "view.find_next", "label": "Find Next"},
    {"id": "view.find_previous", "label": "Find Previous"},
    {"id": "view.find_file", "label": "Find File in Folder"},
    {"id": "bookmarks.find", "label": "Go to Bookmark"},
    {"id": "view.diagnostics", "label": "Diagnostics"},
    {"id": "view.symbols", "label": "Symbols"},
    {"id": "view.toggle_tree", "label": "Toggle File Tree"},
    {"id": "view.new_terminal", "label": "New Terminal"},
    {"id": "view.cycle_tab", "label": "Cycle Tabs"},
    {"id": "view.cycle_tab_prev", "label": "Cycle Tabs Backwards"},
    {"id": "view.goto_definition", "label": "Go to Definition"},
    {"id": "view.rename_symbol", "label": "Rename Symbol"},
    {"id": "view.check_lisp", "label": "Check Lisp File (SBCL)"},
    {"id": "run.task_create", "label": "New Task"},
    {"id": "run.task_run", "label": "Run Task"},
    {"id": "run.task_last", "label": "Rerun Last Task"},
]


def shortcut_key(shortcut_id: str) -> str:
    """Текущее значение шортката: из настроек или дефолт.

    Ленивый импорт app_settings: settings_store импортирует этот модуль
    на верхнем уровне (для DEFAULTS), цикл бы не развязался.
    """
    default = SHORTCUT_DEFAULTS[shortcut_id]
    try:
        from editor.settings_store import app_settings
        value = app_settings().get_str(f"shortcuts.{shortcut_id}", default)
        return value if isinstance(value, str) else default
    except Exception:
        return default


def parse_shortcut(text: str) -> QKeySequence:
    """Безопасный разбор строки шортката: пусто/мусор -> пустой QKeySequence."""
    if not text:
        return QKeySequence()
    if isinstance(text, str):
        text = text.strip()
    if not text:
        return QKeySequence()
    try:
        seq = QKeySequence(text)
    except Exception:
        return QKeySequence()
    return seq if not seq.isEmpty() else QKeySequence()


def bind_shortcut(action, shortcut_id: str):
    """Вешает на QAction шорткат из настроек и помечает его id.

    action.setShortcut(QKeySequence()) отключает шорткат (пустое значение
    в настройках) — см. ShortcutControl.
    """
    action._shortcut_id = shortcut_id
    action.setShortcut(parse_shortcut(shortcut_key(shortcut_id)))
