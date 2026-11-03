"""
Общие Qt-хелперы: поиск виджетов по цепочке родителей.

Раньше одинаковый проход «поднимаемся по parent(), пока не найдём X»
дублировался в code_editor.py, tab_bar.py и terminal.py.
"""

from typing import Callable, Optional, TypeVar

from PyQt6.QtWidgets import QTabWidget, QWidget

W = TypeVar("W", bound=QWidget)


def find_ancestor(widget: QWidget,
                  predicate: Callable[[QWidget], bool]) -> Optional[QWidget]:
    """Поднимается по цепочке родителей и возвращает первый, для которого
    predicate(parent) истинен (или None)."""
    parent = widget.parent()
    while parent is not None:
        if predicate(parent):
            return parent
        parent = parent.parent()
    return None


def find_tab_widget(widget: QWidget) -> Optional[QTabWidget]:
    """Ищет QTabWidget, в котором живёт виджет.

    QTabWidget переносит добавляемый виджет во внутренний QStackedWidget,
    поэтому parent() — это стек, а не сам таб-виджет; поднимаемся наверх.
    """
    return find_ancestor(widget, lambda w: isinstance(w, QTabWidget))


def sync_tab_data(editor, include_content: bool = True) -> None:
    """Синхронизирует tabData вкладки с состоянием редактора.

    Единая точка для drag&drop, Save As и переименований — раньше один
    и тот же проход копировался в editor_pane/main/tab_bar и расходился.
    include_content=False для горячих путей (Ctrl+S): полный текст буфера
    здесь не нужен, свежий снимок снимается при старте драга.
    """
    tab_widget = find_tab_widget(editor)
    if tab_widget is None:
        return
    tab_bar = tab_widget.tabBar()
    for i in range(tab_widget.count()):
        w = tab_widget.widget(i)
        if getattr(w, "editor", None) is editor:
            data = tab_bar.tabData(i)
            if data is not None:
                data["file_path"] = editor.file_path
                data["language_id"] = editor.language_id
                if include_content:
                    data["content"] = editor.text()
                tab_bar.setTabData(i, data)
            return
