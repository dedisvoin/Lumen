"""
Tasks (таксы) — именованные последовательности команд для терминала.

Alt+T — список тасков, Ctrl+Alt+T — создать таск (имя + команды),
клик выполняет выбранный в ближайшем терминале.
"""

from PyQt6.QtCore import QSettings, Qt, QSize, QTimer
from PyQt6.QtCore import QRectF, QEvent
from PyQt6.QtGui import QColor, QPainter
from PyQt6.QtWidgets import (
    QDialog, QFrame, QHBoxLayout, QLabel, QLineEdit,
    QPlainTextEdit, QPushButton, QScrollArea, QSizePolicy, QToolButton,
    QVBoxLayout, QWidget, QApplication,
)

import json
import os
import threading
import time
from pathlib import Path

from qframelesswindow import FramelessDialog, StandardTitleBar

from editor.theme_utils import ThemeColors, qss_color
from editor.scrollbar_qss import scrollbar_qss
from editor.themed_dialogs import style_titlebar, warning as themed_warning
from editor.settings_widgets import ArrowComboBox
from editor.resources import editor_path
from editor.widgets.file_icons import load_ui_icon
from editor.widgets.terminal import TerminalWidget


# Таймаут ожидания завершения одного шага таска (сек): долгая сборка
# или интерактивная программа не должны держать очередь вечно, но и
# слишком малый таймаут сломает последовательность (rm раньше clang).
_TASK_STEP_TIMEOUT_S = 300

# Доступные типы таска (показываются в списке перед названием)
TASK_TYPES = ["C++/C", "Python", "Command", "Other"]
DEFAULT_TASK_TYPE = "Other"


class TaskStore:
    """Хранение тасков во внешнем JSON-файле editor/tasks.json."""

    _FILE = editor_path("tasks.json")
    _LAST_FILE = editor_path("tasks_last.json")  # последний запущенный таск
    _LEGACY_KEY = "tasks"  # QSettings, для переноса старых данных
    _OLD_FILE = Path.home() / ".lumen" / "tasks.json"  # прежняя папка

    @classmethod
    def _parse(cls, data) -> list:
        tasks = []
        for item in data or []:
            if isinstance(item, dict) and item.get("name"):
                ttype = str(item.get("type", ""))
                if ttype not in TASK_TYPES:
                    ttype = DEFAULT_TASK_TYPE
                tasks.append({
                    "name": str(item["name"]),
                    "commands": str(item.get("commands", "")),
                    "type": ttype,
                })
        return tasks

    @classmethod
    def load(cls) -> list:
        """Таски из editor/tasks.json. Legacy-хранилища переносятся только
        если файла ещё нет — существующий (даже с ошибкой JSON) файл
        никогда не затирается старыми данными."""
        try:
            raw = cls._FILE.read_text(encoding="utf-8")
            if not raw.strip():
                return []
            return cls._parse(json.loads(raw))
        except (OSError, ValueError):
            pass
        if cls._FILE.exists():
            return []
        # Перенос из прежнего файла ~/.lumen/tasks.json при первом запуске
        try:
            raw = cls._OLD_FILE.read_text(encoding="utf-8")
            tasks = cls._parse(json.loads(raw))
            if tasks:
                cls.save(tasks)
                return tasks
        except (OSError, ValueError):
            pass
        # Перенос из старого хранилища QSettings при первом запуске
        legacy = QSettings().value(cls._LEGACY_KEY, "")
        try:
            tasks = cls._parse(json.loads(legacy))
        except (TypeError, ValueError):
            return []
        if tasks:
            cls.save(tasks)
        return tasks

    @classmethod
    def save(cls, tasks: list):
        try:
            cls._FILE.parent.mkdir(parents=True, exist_ok=True)
            cls._FILE.write_text(
                json.dumps(tasks, ensure_ascii=False, indent=2),
                encoding="utf-8")
        except OSError:
            pass

    @classmethod
    def save_last(cls, data: dict):
        """Сохраняет последний запущенный таск (для звёздочки в списке и
        Alt+Shift+T). QSettings не используем: без organizationName/
        applicationName он молча теряет данные (no-op)."""
        try:
            cls._LAST_FILE.parent.mkdir(parents=True, exist_ok=True)
            cls._LAST_FILE.write_text(
                json.dumps(data, ensure_ascii=False, indent=2),
                encoding="utf-8")
        except OSError:
            pass

    @classmethod
    def load_last(cls):
        """Последний запущенный таск (переживает перезапуск) или None."""
        try:
            raw = json.loads(cls._LAST_FILE.read_text(encoding="utf-8"))
            if not isinstance(raw, dict) or not raw.get("name"):
                return None
            return cls._parse([raw])[0]
        except (OSError, ValueError, IndexError):
            return None


class TasksController:
    """Операции с тасками: создание, правка, удаление, запуск в терминале.

    Вынесено из MainWindow: контроллер ходит в окно за сервисами
    (theme_manager, tile_manager, активный редактор), не раздувая окно.
    """

    def __init__(self, window):
        self.window = window
        self._last = self._load_last()
        # Очередь таска: QTimer в главном потоке гоняет шаги по очереди
        self._task_timer = None
        self._task_terminal = None
        self._task_steps = []
        self._task_idx = 0
        self._wait_check = None
        self._wait_started = 0.0
        self._task_thread = None
        # Папка активного файла таска — для резолва относительных OPEN: путей
        self._task_base_dir = None
        # Нужен ли терминал (есть ли шаги term); если нет — новую вкладку
        # терминала не открываем
        self._needs_terminal = False
        # Прогресс, показанный на вкладке терминала (для подавления лишних
        # перерисовок таб-бара: setTabText только при реальной смене)
        self._task_progress_shown = None

    def create(self):
        """Ctrl+Alt+T: диалог создания таска (имя + команды)."""
        w = self.window
        dlg = TaskDialog(w.theme_manager, w.current_theme, w)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        data = dlg.task_data()
        if not data["name"] or not data["commands"]:
            return
        tasks = TaskStore.load()
        tasks.append(data)
        TaskStore.save(tasks)

    def run_list(self):
        """Alt+T: попап тасков; клик по имени — запуск, кнопки справа —
        правка (rewrite) и удаление (delete)."""
        w = self.window
        tasks = TaskStore.load()
        last = self._load_last() or {}
        popup = TaskListPopup(
            w.theme_manager, w.current_theme, tasks,
            self.run, self.edit, self.delete, w,
            last_name=last.get("name"),
        )
        geo = w.geometry()
        popup.adjustSize()
        popup.move(geo.center().x() - popup.width() // 2, geo.top() + 100)
        popup.show()
        self._popup = popup  # держим ссылку, пока попап открыт

    def edit(self, data: dict):
        """Открывает диалог редактирования таска."""
        w = self.window
        dlg = TaskDialog(w.theme_manager, w.current_theme, w)
        dlg.set_data(data["name"], data["commands"], data.get("type", DEFAULT_TASK_TYPE))
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        new_data = dlg.task_data()
        if not new_data["name"] or not new_data["commands"]:
            return
        tasks = TaskStore.load()
        for t in tasks:
            if t["name"] == data["name"]:
                t.update(new_data)
                break
        TaskStore.save(tasks)

    def delete(self, data: dict):
        """Удаляет таск по имени."""
        tasks = TaskStore.load()
        tasks = [t for t in tasks if t["name"] != data["name"]]
        TaskStore.save(tasks)

    def run(self, data: dict):
        """Выполняет команды таска строго по очереди.

        Обычная команда пишется в ближайший терминал, и очередь ждёт
        появления нового промпта (команда завершилась); команда с
        префиксом CMD: (регистр не важен) выполняется на фоне через
        os.system() — её поток блокируется до завершения, поэтому
        следующая команда стартует только после неё.

        @AT_FILE — путь файла из активной вкладки,
        @AT_FILE_NAME — его имя без расширения (main.cpp -> main).
        """
        w = self.window
        editor = w._get_current_editor()
        file_path = editor.file_path if editor else None
        if "@AT_FILE" in data["commands"] and not file_path:
            themed_warning(
                w, "Run Task",
                "No file open to substitute @AT_FILE / @AT_FILE_NAME.",
                w.theme_manager, w.current_theme,
            )
            return
        self._cancel_task()
        self._last = data
        TaskStore.save_last(data)
        self._task_base_dir = os.path.dirname(file_path) if file_path else None
        self._task_steps = []
        for line in data["commands"].splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            command = self._substitute(line, file_path)
            if line.upper().startswith("OPEN:"):
                # OPEN: path — открыть файл в новой вкладке (без запуска
                # команды в терминале)
                self._task_steps.append(("open", command[5:].strip()))
            elif line.upper().startswith("SLEEP:"):
                # SLEEP: 0.5 — пауза между шагами (в секундах)
                self._task_steps.append(("sleep", command[6:].strip()))
            elif line.upper().startswith("CMD:"):
                self._task_steps.append(("bg", command[4:].strip()))
            else:
                self._task_steps.append(("term", command))
        # Терминал нужен только для обычных команд (term). Для тасков из
        # одних CMD:/OPEN:/SLEEP: новая вкладка терминала не открывается.
        needs_term = any(k == "term" for k, _ in self._task_steps)
        self._needs_terminal = needs_term
        terminal = None
        if needs_term:
            terminal = self.nearest_terminal()
            if terminal is None:
                return
            # Вывод таска должен быть виден: сбрасываем прокрутку вниз, даже
            # если пользователь был проскроллен вверх по scrollback.
            terminal.scroll_to_bottom()
            if self._task_steps and self._task_steps[0][0] == "term" \
                    and terminal.last_prompt_block() == -1:
                # Оболочка ещё не вывела первый промпт — ждём её готовности,
                # иначе первый промпт будет принят за завершение команды
                self._task_steps.insert(0, ("ready", None))
        self._task_terminal = terminal
        self._task_idx = 0
        self._wait_check = None
        if self._task_timer is None:
            self._task_timer = QTimer(self.window)
            self._task_timer.setInterval(80)
            self._task_timer.timeout.connect(self._task_tick)
        self._task_timer.start()
        self._update_task_tab()

    def _update_task_tab(self):
        """Обновляет прогресс таска на вкладке терминала ([1/3] • running).

        Меняет текст вкладки только при смене (шаг/всего), чтобы таймер
        (80 мс) не перерисовывал таб-бар вхолостую.
        """
        terminal = self._task_terminal
        if terminal is None:
            return
        # Номер шага = число уже отправленных команд (idx). idx=0 — задача
        # только запущена ([1/всего]); idx=total — идёт последний шаг ([n/n]).
        # Снимает бейдж _cancel_task при завершении/отмене очереди.
        if self._task_steps:
            progress = (max(self._task_idx, 1), len(self._task_steps))
        else:
            progress = None
        if progress == self._task_progress_shown:
            return
        self._task_progress_shown = progress
        try:
            if progress is None:
                terminal.clear_task_progress()
            else:
                terminal.set_task_progress(*progress)
        except RuntimeError:
            pass  # вкладка терминала уже закрыта

    def _cancel_task(self):
        """Останавливает текущую очередь таска (повторный запуск)."""
        if self._task_timer is not None:
            self._task_timer.stop()
        terminal = self._task_terminal
        if terminal is not None:
            try:
                terminal.clear_task_progress()
            except RuntimeError:
                pass
        self._task_steps = []
        self._wait_check = None
        self._task_thread = None
        self._task_terminal = None
        self._task_progress_shown = None

    def _task_tick(self):
        """Тик очереди: ждёт завершения текущего шага, затем следующий.

        Все проверки лёгкие и в главном потоке; долгие ожидания
        (os.system в фоновом потоке, работа программы в терминале) не
        блокируют UI — тикаем раз в 80 мс до готовности или таймаута.
        """
        terminal = self._task_terminal
        if terminal is None:
            if self._needs_terminal:
                # Терминал закрылся посреди таска, требующего его — стоп
                self._cancel_task()
                return
            # Иначе терминал и не требовался (CMD:/OPEN:/SLEEP:) — идём дальше
        self._update_task_tab()
        if self._wait_check is not None:
            if self._wait_check():
                self._wait_check = None
            elif time.monotonic() - self._wait_started > _TASK_STEP_TIMEOUT_S:
                self._wait_check = None  # таймаут — идём дальше
            else:
                return
        if self._task_idx >= len(self._task_steps):
            self._cancel_task()
            try:
                terminal.focus_input()
            except (RuntimeError, AttributeError):
                pass  # вкладка терминала уже закрыта
            return
        kind, command = self._task_steps[self._task_idx]
        self._task_idx += 1
        if kind == "open":
            # OPEN: — открываем файл в новой вкладке и сразу идём дальше,
            # ожидание завершения не нужно
            self._open_in_new_tab(command)
            return
        if kind == "sleep":
            # SLEEP: 0.5 — пауза в секундах перед следующим шагом. Ждём
            # через тот же механизм _wait_check, что и обычные шаги.
            try:
                secs = float(command)
            except (ValueError, TypeError):
                secs = 0.0
            if secs > 0:
                self._wait_started = time.monotonic()
                self._wait_check = \
                    lambda: time.monotonic() - self._wait_started >= secs
            return
        if kind == "ready":
            # Ждём первый промпт оболочки (терминал подключился)
            marker = terminal.last_prompt_block()
            self._wait_check = lambda: (
                terminal.has_new_prompt(marker) or not terminal.is_running()
            )
            self._wait_started = time.monotonic()
            return
        if kind == "bg":
            # os.system блокирует поток до завершения команды — завершение
            # шага определяем по выходу из потока (daemon, чтобы не
            # держать приложение при закрытии)
            self._task_thread = None
            if command:
                thread = threading.Thread(
                    target=os.system, args=(command,), daemon=True
                )
                self._task_thread = thread
                thread.start()
            self._wait_check = lambda: (
                self._task_thread is None
                or not self._task_thread.is_alive()
            )
            self._wait_started = time.monotonic()
            return
        terminal._write(command + "\r")
        marker = terminal.last_prompt_block()
        self._wait_check = lambda: (
            terminal.has_new_prompt(marker) or not terminal.is_running()
        )
        self._wait_started = time.monotonic()

    @staticmethod
    def _substitute(line: str, file_path) -> str:
        """Заменяет @AT_FILE_NAME и @AT_FILE на путь активного файла.

        Сначала длинный вариант (@AT_FILE_NAME — подстрока @AT_FILE).
        """
        if not file_path:
            return line
        stem = os.path.splitext(os.path.basename(file_path))[0]
        return line.replace("@AT_FILE_NAME", stem).replace("@AT_FILE", file_path)

    def _open_in_new_tab(self, path: str):
        """OPEN: — открывает файл в новой вкладке активной панели окна.

        Относительные пути резолвятся относительно папки активного файла
        таска (если такой есть и файл там действительно лежит).
        """
        if not path:
            return
        if not os.path.isabs(path) and self._task_base_dir:
            cand = os.path.join(self._task_base_dir, path)
            if os.path.isfile(cand):
                path = cand
        w = self.window
        tile = w.tile_manager
        pane = tile.get_active_pane()
        if pane is None and tile._panes:
            pane = tile._panes[0]
        if pane is not None:
            pane.add_editor(file_path=path)

    def run_last(self):
        """Alt+Shift+T: повторный запуск последнего таска, иначе список."""
        if self._last:
            self.run(self._last)
        else:
            self.run_list()

    @staticmethod
    def _load_last():
        """Последний запущенный таск (переживает перезапуск приложения)."""
        return TaskStore.load_last()

    def nearest_terminal(self):
        """Возвращает ближайший открытый терминал.

        Ищем сначала в текущем окне (как раньше), затем во всех остальных
        окнах приложения — и главном, и detached sub-окнах
        (DetachedWindow со своим TileManager), чтобы таск запускался в уже
        открытой консоли, а не только в панелях текущего окна.
        """
        windows = self._all_windows()
        term = self._find_terminal_in(self.window)
        if term is not None:
            return term
        for w in windows:
            if w is self.window:
                continue
            term = self._find_terminal_in(w)
            if term is not None:
                return term
        # Нигде нет терминала — создаём в текущем окне
        return self._create_terminal_in(self.window)

    @staticmethod
    def _all_windows():
        """Все живые окна с TileManager (главное + detached sub-окна)."""
        wins = []
        for w in QApplication.topLevelWidgets():
            tile = getattr(w, "tile_manager", None)
            if tile is not None:
                wins.append(w)
        return wins

    def _find_terminal_in(self, w):
        """Ближайший терминал в конкретном окне или None (без создания)."""
        tile = w.tile_manager
        pane = tile.get_active_pane()
        if pane is not None:
            if isinstance(pane.tabs.currentWidget(), TerminalWidget):
                return pane.tabs.currentWidget()
            for i in range(pane.tabs.count()):
                widget = pane.tabs.widget(i)
                if isinstance(widget, TerminalWidget):
                    pane.tabs.setCurrentWidget(widget)
                    return widget
        for p in tile._panes:
            for i in range(p.tabs.count()):
                widget = p.tabs.widget(i)
                if isinstance(widget, TerminalWidget):
                    p.tabs.setCurrentWidget(widget)
                    return widget
        return None

    def _create_terminal_in(self, w):
        """Создаёт новый терминал в окне (активная панель -> первая)."""
        tile = w.tile_manager
        pane = tile.get_active_pane()
        if pane is not None:
            return pane.add_terminal()
        if tile._panes:
            return tile._panes[0].add_terminal()
        return None


class TaskDialog(FramelessDialog):
    """Создание/редактирование таска: имя + команды (по одной на строку)."""

    def __init__(self, theme_manager, theme_name: str, parent=None):
        super().__init__(parent)
        self.setWindowTitle("New Task")
        self.setModal(True)
        self.setMinimumWidth(440)

        # Тайтл-бар как у главного окна (qframelesswindow)
        tb = StandardTitleBar(self)
        tb.titleLabel.setText("New Task")
        tb.maxBtn.hide()
        if tb.hBoxLayout.count() > 0:
            item = tb.hBoxLayout.itemAt(0)
            if item and item.spacerItem():
                tb.hBoxLayout.removeItem(item)
        if hasattr(tb, "iconLabel"):
            tb.iconLabel.hide()
        self.setTitleBar(tb)

        layout = QVBoxLayout(self)
        layout.setSpacing(8)
        layout.setContentsMargins(16, self.titleBar.height() + 8, 16, 16)

        layout.addWidget(QLabel("Task name"))
        self._name = QLineEdit()
        self._name.setPlaceholderText("e.g. Build & Run")
        layout.addWidget(self._name)

        layout.addWidget(QLabel("Type"))
        self._type = ArrowComboBox()
        self._type.addItems(TASK_TYPES)
        self._type.setCurrentText(DEFAULT_TASK_TYPE)
        self._type.setMinimumWidth(160)
        layout.addWidget(self._type)

        layout.addWidget(QLabel("Commands (one per line)"))
        self._commands = QPlainTextEdit()
        self._commands.setPlaceholderText(
            "python build.py\n"
            "test.exe --verbose\n"
            "OPEN: path/to/file.cpp — открыть файл в новой вкладке\n"
            "SLEEP: 0.5 — пауза между шагами (в секундах)\n"
            "CMD: rm -rf build — фоновая команда (ждём завершения)\n"
            "@AT_FILE — путь активного файла, @AT_FILE_NAME — его имя без расширения"
        )
        layout.addWidget(self._commands)

        btns = QHBoxLayout()
        btns.addStretch(1)
        cancel = QPushButton("Cancel")
        cancel.clicked.connect(self.reject)
        ok = QPushButton("Save")
        ok.setDefault(True)
        ok.clicked.connect(self.accept)
        btns.addWidget(cancel)
        btns.addWidget(ok)
        layout.addLayout(btns)

        self._apply_theme(theme_manager, theme_name)
        self._name.setFocus()

    def _apply_theme(self, theme_manager, theme_name: str):
        tc = ThemeColors.from_theme_manager(theme_manager, theme_name)
        bg = tc.editor_bg_color.name()
        fg = tc.fg
        border = tc.border
        muted = tc.muted
        acc = QColor(tc.selection or tc.fg)
        self._type.set_arrow_color(QColor(fg))
        hover = QColor(fg)
        hover.setAlpha(30)
        sel = QColor(acc)
        sel.setAlpha(45)

        # Тайтл-бар — общая стилизация (тот же код, что у стандартных
        # диалогов в themed_dialogs.style_titlebar)
        style_titlebar(self.titleBar, tc)

        # Тело диалога
        self.setStyleSheet(f"""
            QDialog {{ background-color: {bg}; }}
            QLabel {{ color: {muted}; }}
            QLineEdit, QPlainTextEdit, QComboBox {{
                background-color: {bg};
                color: {fg};
                border: 0.5px solid {border};
                border-radius: 4px;
                padding: 4px 8px;
            }}
            QComboBox::drop-down {{
                subcontrol-origin: border;
                subcontrol-position: top right;
                width: 20px;
                border-top: 0.5px solid {border};
                border-bottom: 0.5px solid {border};
                border-left: 0.5px solid {border};
                border-top-right-radius: 3px;
                border-bottom-right-radius: 3px;
                background-color: {qss_color(hover)};
            }}
            QComboBox::down-arrow {{
                width: 0;
                height: 0;
            }}
            QLineEdit:focus, QPlainTextEdit:focus, QComboBox:focus {{
                border-color: {acc.name()};
            }}
            QComboBox QAbstractItemView {{
                background-color: {bg};
                color: {fg};
                border: 0.5px solid {border};
                selection-background-color: {qss_color(sel)};
                outline: 0;
            }}
            QComboBox QAbstractItemView::item {{
                padding: 1px 8px;
                border: none;
                background-color: transparent;
                color: {fg};
            }}
            QComboBox QAbstractItemView::item:hover {{
                background-color: {qss_color(hover)};
            }}
            QComboBox QAbstractItemView::item:selected {{
                background-color: {qss_color(sel)};
                color: {fg};
            }}
            QPushButton {{
                color: {fg};
                background-color: {bg};
                border: 0.5px solid {border};
                border-radius: 4px;
                padding: 4px 14px;
            }}
            QPushButton:hover, QPushButton:default {{
                border-color: {acc.name()};
            }}
        """)

    def set_data(self, name: str, commands: str, ttype: str = DEFAULT_TASK_TYPE):
        """Предзаполнение полей (для редактирования)."""
        self._name.setText(name)
        self._commands.setPlainText(commands)
        if ttype in TASK_TYPES:
            self._type.setCurrentText(ttype)
        else:
            self._type.setCurrentText(DEFAULT_TASK_TYPE)

    def task_data(self) -> dict:
        return {
            "name": self._name.text().strip(),
            "commands": self._commands.toPlainText().strip(),
            "type": self._type.currentText(),
        }


class _LeftToolButton(QToolButton):
    """QToolButton с текстом, прижатым к левому краю.

    QStyleSheetStyle перехватывает отрисовку текста у QToolButton и
    игнорирует и QSS `text-align`, и textAlignment в paintEvent,
    поэтому рисуем всё вручную через QPainter.
    """

    def __init__(self, parent=None):
        super().__init__(parent)
        self._fg = QColor(255, 255, 255)
        self._hover = QColor(255, 255, 255, 30)
        self._sel = QColor(255, 255, 255, 60)
        self._hovered = False
        self._selected = False
        self.setAttribute(Qt.WidgetAttribute.WA_Hover, True)
        self.setAutoRaise(True)
        self.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextOnly)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        # Высота кнопки задаёт высоту строки и область hover-подсветки
        self.setMinimumHeight(27)

    def set_colors(self, fg: QColor, hover: QColor, sel: QColor):
        self._fg = fg
        self._hover = hover
        self._sel = sel
        self.update()

    def set_hovered(self, hovered: bool):
        """Явное управление hover-состоянием (enter/leave и сброс попапом)."""
        if hovered != self._hovered:
            self._hovered = hovered
            self.update()

    def set_selected(self, selected: bool):
        """Подсветка выбранного стрелками таска."""
        if selected != self._selected:
            self._selected = selected
            self.update()

    def enterEvent(self, event):
        self.set_hovered(True)
        super().enterEvent(event)

    def leaveEvent(self, event):
        self.set_hovered(False)
        super().leaveEvent(event)

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(self.rect()).adjusted(1, 1, -1, -1)
        if self._selected:
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(self._sel)
            p.drawRoundedRect(r, 4, 4)
        elif self._hovered:
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(self._hover)
            p.drawRoundedRect(r, 4, 4)
        text_rect = self.rect().adjusted(8, 6, -8, -6)
        elided = self.fontMetrics().elidedText(
            self.text(), Qt.TextElideMode.ElideRight, text_rect.width())
        p.setPen(self._fg)
        p.drawText(text_rect, Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, elided)
        p.end()


class TaskListPopup(QFrame):
    """Список тасков: клик по имени — запуск, кнопки справа — правка/удаление."""

    def __init__(self, theme_manager, theme_name: str, tasks: list,
                 on_run, on_edit, on_delete, parent=None, last_name=None):
        super().__init__(parent, Qt.WindowType.Popup | Qt.WindowType.FramelessWindowHint | Qt.WindowType.NoDropShadowWindowHint)
        self._on_run, self._on_edit, self._on_delete = on_run, on_edit, on_delete
        self._tasks = list(tasks)
        self._last_name = last_name
        self._run_buttons = []
        self._rows = []
        self._current = -1
        self.setFixedWidth(500)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self._apply_theme(theme_manager, theme_name)

        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(6, 6, 6, 6)
        self._layout.setSpacing(2)

        self._scroll = QScrollArea(self)
        self._scroll.setWidgetResizable(True)
        self._scroll.setFixedHeight(250)
        self._scroll.setFrameShape(QFrame.Shape.NoFrame)
        self._scroll.setHorizontalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._scroll.setVerticalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self._list_widget = QWidget()
        self._list_widget.setAutoFillBackground(False)
        self._list_layout = QVBoxLayout(self._list_widget)
        self._list_layout.setContentsMargins(0, 0, 0, 0)
        self._list_layout.setSpacing(2)
        self._scroll.setWidget(self._list_widget)
        self._scroll.viewport().setAutoFillBackground(False)
        self._scroll.verticalScrollBar().installEventFilter(self)
        self._layout.addWidget(self._scroll)

        if not tasks:
            empty = QLabel("(no tasks)")
            empty.setStyleSheet(f"color: {self._muted}; padding: 6px;")
            self._list_layout.addWidget(empty)
        else:
            for t in tasks:
                self._list_layout.addWidget(self._make_row(t))
        self._list_layout.addStretch(1)

    def _rebuild(self):
        """Перестраивает строки списка после удаления таска."""
        while self._list_layout.count():
            item = self._list_layout.takeAt(0)
            w = item.widget()
            if w:
                w.hide()
                w.deleteLater()
        self._run_buttons = []
        self._rows = []
        if not self._tasks:
            self._current = -1
            empty = QLabel("(no tasks)")
            empty.setStyleSheet(f"color: {self._muted}; padding: 6px;")
            self._list_layout.addWidget(empty)
        else:
            for t in self._tasks:
                self._list_layout.addWidget(self._make_row(t))
            self._update_selection(min(max(self._current, 0), len(self._tasks) - 1))
        self._list_layout.addStretch(1)
        self.setFocus()

    def _apply_theme(self, theme_manager, theme_name: str):
        tc = ThemeColors.from_theme_manager(theme_manager, theme_name)
        self._bg = tc.editor_bg_color.name()
        self._fg = tc.fg
        self._muted = tc.muted
        self._border = tc.border
        hover = QColor(tc.fg)
        hover.setAlpha(30)
        self._hover = hover.name(QColor.NameFormat.HexArgb)
        sel = QColor(tc.fg)
        sel.setAlpha(60)
        self._sel = sel.name(QColor.NameFormat.HexArgb)
        self.setStyleSheet(f"""
            TaskListPopup {{
                background-color: {self._bg};
                border: 0.5px solid {self._border};
                border-radius: 6px;
            }}
            QToolButton#taskIcon {{
                background: transparent;
                border: none;
                border-radius: 4px;
                padding: 4px;
            }}
            QToolButton#taskIcon:hover {{
                background-color: {self._hover};
            }}
            QScrollArea {{
                background: transparent;
            }}
            QScrollArea > QWidget > QWidget {{
                background-color: transparent;
            }}
        """ + scrollbar_qss(self._bg, QColor(self._fg)))

    def eventFilter(self, obj, event):
        # После ручной прокрутки мышью возвращаем фокус попапу,
        # иначе стрелки остаются у QScrollBar и не навигируют по списку
        if (obj is self._scroll.verticalScrollBar()
                and event.type() == QEvent.Type.MouseButtonRelease):
            self.setFocus()
        return super().eventFilter(obj, event)

    def showEvent(self, event):
        super().showEvent(event)
        self.setFocus()
        # Первый таск выбран по умолчанию — Enter сразу запускает
        if self._run_buttons:
            self._update_selection(0)

    def _update_selection(self, index: int):
        """Выбор таска стрелками (с зацикливанием)."""
        if not self._run_buttons:
            return
        self._current = index % len(self._run_buttons)
        for i, b in enumerate(self._run_buttons):
            b.set_selected(i == self._current)
        if self._current < len(self._rows):
            self._scroll.ensureWidgetVisible(self._rows[self._current], 0, 0)

    def keyPressEvent(self, event):
        key = event.key()
        if key == Qt.Key.Key_Down:
            self._update_selection(self._current + 1)
            event.accept()
        elif key == Qt.Key.Key_Up:
            self._update_selection(self._current - 1)
            event.accept()
        elif key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            if 0 <= self._current < len(self._tasks):
                self._pick(self._tasks[self._current])
            event.accept()
        elif key == Qt.Key.Key_Escape:
            self.close()
            event.accept()
        else:
            super().keyPressEvent(event)

    def _clear_hover(self):
        """Снимает hover со всех кнопок (мышь ушла из попапа)."""
        for b in self.findChildren(_LeftToolButton):
            b.set_hovered(False)

    def leaveEvent(self, event):
        # У Popup-окон дети могут не получить Leave при выходе за окно
        self._clear_hover()
        super().leaveEvent(event)

    def mouseMoveEvent(self, event):
        # Страховка: пересчитываем hover по фактической позиции курсора,
        # чтобы подсветка не залипала при переходе на пустые области.
        # Наведённая строка становится и выбранной (для Enter).
        pos = event.position().toPoint()
        hovered_idx = -1
        for i, b in enumerate(self._run_buttons):
            inside = b.rect().contains(b.mapFrom(self, pos))
            b.set_hovered(inside)
            if inside and hovered_idx < 0:
                hovered_idx = i
        if hovered_idx >= 0:
            self._update_selection(hovered_idx)
        super().mouseMoveEvent(event)

    def _make_row(self, task: dict):
        row = QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(2)

        run = _LeftToolButton(self)
        run.setObjectName("taskRun")
        run.setText(task["name"])
        run.setToolTip(task["commands"])
        run.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        run.set_colors(QColor(self._fg), QColor(self._hover), QColor(self._sel))
        run.clicked.connect(lambda _=False, d=task: self._pick(d))

        # Бейдж типа таска — отображается в строке перед названием
        ttype = task.get("type", "")
        if ttype:
            badge = QLabel(self)
            badge.setObjectName("taskType")
            badge.setText(ttype)
            badge.setAlignment(Qt.AlignmentFlag.AlignCenter)
            badge.setFixedHeight(22)
            badge.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
            badge.setStyleSheet(
                f"color: {self._muted}; border: 0.5px solid {self._border};"
                f"border-radius: 4px; padding: 0 8px; font-size: 11px;")
            row.addWidget(badge)

        row.addWidget(run, 1)
        self._run_buttons.append(run)

        # Звёздочка после текста у последнего запущенного таска
        # (Alt+Shift+T — его повтор)
        if self._last_name and task["name"] == self._last_name:
            star = QLabel(self)
            star.setPixmap(load_ui_icon("star", QColor(self._fg))
                           .pixmap(QSize(14, 14)))
            star.setToolTip("Last used task")
            star.setFixedSize(22, 22)
            star.setAlignment(Qt.AlignmentFlag.AlignCenter)
            row.addWidget(star)

        edit = QToolButton(self)
        edit.setObjectName("taskIcon")
        edit.setIcon(load_ui_icon("rewrite", QColor(self._fg)))
        edit.setIconSize(QSize(14, 14))
        edit.setToolTip("Edit...")
        edit.setAutoRaise(True)
        edit.clicked.connect(lambda _=False, d=task: self._edit(d))
        row.addWidget(edit)

        delete = QToolButton(self)
        delete.setObjectName("taskIcon")
        delete.setIcon(load_ui_icon("delete", QColor(self._fg)))
        delete.setIconSize(QSize(14, 14))
        delete.setToolTip("Delete")
        delete.setAutoRaise(True)
        delete.clicked.connect(lambda _=False, d=task: self._delete(d))
        row.addWidget(delete)

        container = QFrame(self)
        container.setSizePolicy(
            QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed)
        container.setLayout(row)
        self._rows.append(container)
        return container

    def _pick(self, task: dict):
        self.close()
        self._on_run(task)

    def _edit(self, task: dict):
        self.close()
        self._on_edit(task)

    def _delete(self, task: dict):
        """Удаляет таск и обновляет список, не закрывая попап."""
        self._on_delete(task)
        self._tasks = [t for t in self._tasks if t is not task]
        self._rebuild()
