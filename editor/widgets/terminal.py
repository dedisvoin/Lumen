"""
TerminalWidget — встроенный терминал на базе псевдоконсоли Windows (ConPTY).

Вывод псевдоконсоли рендерится через VtScreen, ввод пробрасывается
напрямую, поэтому интерактивные программы (REPL python, cmd с историей
и TAB-дополнением) работают как в настоящем терминале.

Требуется pywinpty (pip install pywinpty); если пакет не установлен,
терминал выводит подсказку и остаётся пустым.

Живёт как вкладка EditorPane и умеет перетаскиваться между таб-барами.
"""

import os
import select
import subprocess
import threading
import uuid
import weakref
from typing import Optional

from PyQt6.QtCore import Qt, QTimer, QRegularExpression, pyqtSignal
from PyQt6.QtGui import QColor, QFont, QKeyEvent, QPalette
from PyQt6.QtWidgets import QApplication, QTabWidget, QVBoxLayout, QWidget

from editor.constants import (
    FALLBACK_BG, FALLBACK_FG,
)
from editor.font_settings import code_font_family
from editor.scrollbar_qss import scrollbar_qss
from editor.scrollbar_fade import ScrollBarAutoHide
from editor.theme_utils import ThemeColors, resolve_terminal_palette
from editor.widgets.qt_utils import find_tab_widget
from editor.widgets.vt_screen import VtScreen

try:
    import winpty
except ImportError:
    winpty = None

_CREATE_NO_WINDOW = 0x08000000  # не мелькать консолью у taskkill

# Промпт cmd/powershell: компилируются один раз, а не каждые 80 мс поллинга
_CMD_PROMPT_RE = QRegularExpression(r"^[A-Za-z]:[^>]*> ?$")
_PS_PROMPT_RE = QRegularExpression(r"^PS [A-Za-z]:[^>]*> ?$")

_TERMINAL_BASE_FONT_SIZE = 11  # базовый размер шрифта терминала (зум от него)
_TERMINAL_MIN_FONT_SIZE = 8
_TERMINAL_MAX_FONT_SIZE = 32

# Реестр живых TerminalWidget. При перетаскивании вкладки в MIME уходит
# только terminal_id, а приёмник забирает существующий виджет из реестра —
# так сессия ConPTY (процесс, история, cwd) не пересоздаётся.
# Ссылки слабые (weakref): если виджет уничтожился без destroyed-сигнала,
# запись протухает и не держит объект в памяти.
_TERMINAL_REGISTRY = {}


# Задержка агрегации вывода псевдоконсоли (мс): при живом выводе куски
# копятся в буфер и сбрасываются пачкой — UI не дёргается на каждый read.
_OUTPUT_BATCH_MS = 30


def find_terminal(terminal_id: Optional[str]):
    """Возвращает живой TerminalWidget по его id из MIME drag&drop."""
    if not terminal_id:
        return None
    ref = _TERMINAL_REGISTRY.get(terminal_id)
    if ref is None:
        return None
    widget = ref()
    if widget is None:
        _TERMINAL_REGISTRY.pop(terminal_id, None)  # протухшая запись
    return widget


def take_terminal_from(terminal) -> Optional[QTabWidget]:
    """Отсоединяет TerminalWidget от таб-виджета-владельца.

    Виджет не удаляется и не гасит псевдоконсоль — его сразу
    добавляет приёмник дропа. Возвращает прежний QTabWidget
    (тот может опустеть), либо None.
    """
    old = find_tab_widget(terminal)
    if old is not None:
        idx = old.indexOf(terminal)
        if idx >= 0:
            old.removeTab(idx)
            # Помечаем исходный таб-бар: вкладку уже забрали целиком,
            # источнику не нужно закрывать её после drag.exec()
            bar = old.tabBar()
            if bar is not None:
                bar._terminal_handed_off = True
            return old
    return None


class TerminalWidget(QWidget):
    """Терминальная вкладка: псевдоконсоль + окно вывода VtScreen."""

    _output_ready = pyqtSignal(str)   # вывод из фонового потока чтения
    _shell_exited = pyqtSignal(object)  # аргумент — завершившаяся псевдоконсоль

    def __init__(self, theme_manager, parent=None, cwd=None):
        super().__init__(parent)
        self.terminal_id = uuid.uuid4().hex
        _TERMINAL_REGISTRY[self.terminal_id] = weakref.ref(self)
        self.theme_manager = theme_manager
        self._cwd = cwd or os.getcwd()
        self._shell = os.environ.get("COMSPEC", "cmd.exe")
        base = os.path.basename(self._shell).lower()
        self._is_powershell = base.startswith("powershell") or base == "pwsh.exe"
        self._theme_name = None
        self._pty = None
        self._pty_stop = True
        self._resize_timer = None
        self._font_size = _TERMINAL_BASE_FONT_SIZE
        # Прогресс таска на вкладке: (текущий шаг, всего шагов) или None
        self._task_progress = None

        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        self._view = VtScreen(self)
        # Бегунок полосы прячется при простое, как в редакторе и дереве
        self._sb_ctl = ScrollBarAutoHide(self._view, self)
        self._view.set_key_handler(self.handle_key)
        self._view.set_zoom_handler(self._zoom_font)
        self._view.setFont(QFont(code_font_family(), self._font_size))
        self._view.document().setDefaultFont(
            QFont(code_font_family(), self._font_size)
        )
        layout.addWidget(self._view)

        self._output_ready.connect(self._on_output_data)
        self._shell_exited.connect(self._on_shell_exited)
        # Агрегатор вывода: куски из потока копим и сбрасываем пачкой
        # (30 мс) — при прогресс-барах/лог-спаме UI не отстаёт
        self._output_buffer = []
        self._output_timer = QTimer(self)
        self._output_timer.setSingleShot(True)
        self._output_timer.setInterval(_OUTPUT_BATCH_MS)
        self._output_timer.timeout.connect(self._flush_output)
        # Соединяем через лямбду: PyQt6 не вызывает bound-методы,
        # подключённые к destroyed напрямую (объект уже разрушается)
        self.destroyed.connect(lambda *_: self._on_destroyed())

        # Стартовые цвета — до первого применения темы
        self._apply_colors(FALLBACK_BG, FALLBACK_FG, None)

        self._start_shell()

    # ---- тема ----

    def apply_theme(self, theme_name: str):
        self._theme_name = theme_name
        theme = self.theme_manager.themes.get(theme_name, {})
        tc = ThemeColors.from_theme_manager(self.theme_manager, theme_name)
        palette = resolve_terminal_palette(theme, tc)
        self._apply_colors(tc.editor_bg_color.name(), tc.fg, tc.selection,
                           palette)
        # Глобальный QSS окна (QWidget { font-size }) сбрасывает шрифт
        # терминала на размер интерфейса — возвращаем текущий зум
        self._apply_font()

    def _apply_font(self):
        """Применяет текущий размер шрифта (_font_size) к окну терминала."""
        font = QFont(code_font_family(), self._font_size)
        self._view.setFont(font)
        self._view.document().setDefaultFont(font)

    def _apply_colors(self, bg_hex: str, fg_hex: str, sel_hex: Optional[str],
                      palette: Optional[list] = None):
        term_bg = QColor(bg_hex)
        fg_qcolor = QColor(fg_hex)

        if sel_hex is None:
            sel_hex = f"rgba({fg_qcolor.red()},{fg_qcolor.green()},"
            sel_hex += f"{fg_qcolor.blue()},70)"

        self._view.setStyleSheet(
            f"""
            QPlainTextEdit {{
                background-color: {term_bg.name()};
                color: {fg_hex};
                selection-background-color: {sel_hex};
                border: none;
            }}
            """
            + scrollbar_qss(term_bg.name(), fg_qcolor)
        )

        # Цвет каретки — из палитры (QSS его не умеет)
        pal = self._view.palette()
        pal.setColor(QPalette.ColorRole.Text, fg_qcolor)
        pal.setColor(QPalette.ColorRole.Base, term_bg)
        self._view.setPalette(pal)
        # Qt не рисует каретку в read-only QPlainTextEdit — VtScreen
        # рисует блочную каретку сам, цвет берём из темы
        self._view.set_caret_color(fg_qcolor)
        if palette is not None:
            self._view.set_palette(palette, fg_qcolor, term_bg)

        # Стиль полос пришёл по наследованию от QPlainTextEdit, а для
        # затухания нужен ещё и цвет бегунка — ставим на сами полосы.
        self._sb_ctl.set_colors(term_bg.name(), fg_qcolor)

    # ---- ввод (проброс клавиш в псевдоконсоль) ----

    def handle_key(self, event: QKeyEvent) -> bool:
        key = event.key()
        mods = event.modifiers()
        ctrl = bool(mods & Qt.KeyboardModifier.ControlModifier)
        shift = bool(mods & Qt.KeyboardModifier.ShiftModifier)

        if ctrl and shift and key == Qt.Key.Key_C:
            self._view.copy()
            return True
        if ctrl and shift and key == Qt.Key.Key_V:
            self._paste()
            return True

        if key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            self._write("\r")
            return True
        if key == Qt.Key.Key_Backspace:
            self._write("\x7f")
            return True
        if key == Qt.Key.Key_Delete:
            self._write("\x1b[3~")
            return True
        if key == Qt.Key.Key_Tab:
            self._write("\t")
            return True
        if key == Qt.Key.Key_Up:
            self._write("\x1b[A")
            return True
        if key == Qt.Key.Key_Down:
            self._write("\x1b[B")
            return True
        if key == Qt.Key.Key_Left:
            self._write("\x1b[D")
            return True
        if key == Qt.Key.Key_Right:
            self._write("\x1b[C")
            return True
        if key == Qt.Key.Key_Home:
            self._write("\x1b[H")
            return True
        if key == Qt.Key.Key_End:
            self._write("\x1b[F")
            return True
        if key == Qt.Key.Key_PageUp:
            self._write("\x1b[5~")
            return True
        if key == Qt.Key.Key_PageDown:
            self._write("\x1b[6~")
            return True
        if key == Qt.Key.Key_Escape:
            self._write("\x1b")
            return True

        if ctrl:
            if key == Qt.Key.Key_Minus:
                self._zoom_font(-1)
            elif key in (Qt.Key.Key_Equal, Qt.Key.Key_Plus):
                self._zoom_font(1)
            elif key == Qt.Key.Key_0:
                self._zoom_font(0)
            elif key == Qt.Key.Key_V:
                self._paste()
            elif key == Qt.Key.Key_C:
                if self._view.has_selection():
                    # Выделенный текст копируем, а не шлём Ctrl+C оболочке
                    self._view.copy()
                else:
                    self._write("\x03")
            elif key == Qt.Key.Key_D:
                self._write("\x04")
            elif key == Qt.Key.Key_L:
                self._write("\x0c")
            elif key == Qt.Key.Key_Z:
                self._write("\x1a")
            else:
                # остальные Ctrl-сочетания (Ctrl+R, Ctrl+F...) — приложению
                text = event.text()
                if text:
                    self._write(text)
            return True

        text = event.text()
        if text and (text.isprintable() or not text.isascii()):
            self._write(text)
            return True
        return False

    def _paste(self):
        text = QApplication.clipboard().text()
        if not text:
            return
        text = text.replace("\r\n", "\r").replace("\n", "\r")
        self._write(text)

    def _zoom_font(self, delta: int):
        """Меняет размер шрифта терминала (delta: -1/+1, 0 — сброс).

        Шрифт терминала и документа меняется синхронно, затем псевдоконсоли
        сообщаются новые размеры (cols/rows), чтобы оболочка перерисовалась.
        """
        if delta == 0:
            self._font_size = _TERMINAL_BASE_FONT_SIZE
        else:
            self._font_size = max(
                _TERMINAL_MIN_FONT_SIZE,
                min(_TERMINAL_MAX_FONT_SIZE, self._font_size + delta),
            )
        self._apply_font()
        self._apply_resize()

    def set_font_size(self, size: int):
        """Восстановление шрифта из сессии: размер + resize псевдоконсоли."""
        self._font_size = max(
            _TERMINAL_MIN_FONT_SIZE,
            min(_TERMINAL_MAX_FONT_SIZE, int(size)),
        )
        self._apply_font()
        self._apply_resize()

    def focus_input(self):
        if self._view is not None:
            self._view.setFocus()

    def title_text(self) -> str:
        """Заголовок вкладки: «Terminal <PID>» (+ прогресс таска, если идёт)."""
        pty = self._pty
        if pty is not None and getattr(pty, "pid", None):
            base = f"Terminal <{pty.pid}>"
        else:
            base = "Terminal"
        if self._task_progress is not None:
            current, total = self._task_progress
            return f"{base} [{current}/{total}] • running"
        return base

    def set_task_progress(self, current: int, total: int):
        """Показывает на вкладке прогресс таска: [шаг/всего] • running."""
        self._task_progress = (current, total)
        self._update_tab_title()

    def clear_task_progress(self):
        """Убирает прогресс таска с вкладки (очередь завершена/отменена)."""
        if self._task_progress is not None:
            self._task_progress = None
            self._update_tab_title()

    def is_running(self) -> bool:
        """Жив ли процесс оболочки (для подтверждения закрытия вкладки)."""
        pty = self._pty
        if pty is None:
            return False
        try:
            return pty.isalive()
        except Exception:
            return False

    def _update_tab_title(self):
        """Обновляет заголовок вкладки после (пере)запуска оболочки."""
        tab_widget = find_tab_widget(self)
        if tab_widget is None:
            return
        idx = tab_widget.indexOf(self)
        if idx >= 0:
            tab_widget.setTabText(idx, self.title_text())

    def _write(self, text: str):
        pty = self._pty
        if pty is None:
            return
        try:
            pty.write(text)
        except Exception:
            pass

    def scroll_to_bottom(self):
        """Прокручивает вывод до низа — следующий вывод будет виден."""
        if self._view is not None:
            self._view.scroll_to_bottom()

    # ---- ожидание завершения команды (для тасков) ----

    def last_prompt_block(self) -> int:
        """Номер блока последней строки-промпта оболочки (-1, если нет).

        Промпт печатается на позиции курсора ConPTY, а курсор всегда в
        последних rows строках документа — ищем только их, а не весь
        scrollback (иначе на больших логах каждый тик поллинга был бы
        O(n) по всему документу).
        """
        view = self._view
        if view is None:
            return -1
        try:
            rows = view.screen_size()[1]
            rx = _PS_PROMPT_RE if self._is_powershell else _CMD_PROMPT_RE
            block = view.document().lastBlock()
            for _ in range(rows + 4):
                if rx.match(block.text()).hasMatch():
                    return block.blockNumber()
                prev = block.previous()
                if not prev.isValid():
                    break
                block = prev
        except RuntimeError:
            pass  # виджет уже уничтожен
        return -1

    def has_new_prompt(self, marker: int) -> bool:
        """Появился ли новый промпт после записи команды.

        marker — номер блока последнего промпта до записи; новый промпт
        печатается в другой блок, поэтому сравнение с == переживает даже
        очистку экрана (cls): документ сброшен, но блок всё равно другой.
        """
        block = self.last_prompt_block()
        return block != -1 and block != marker

    # ---- вывод ----

    def _on_output_data(self, text: str):
        """Куски вывода из фонового потока — копим до срабатывания таймера.

        Таймер не перезапускается, если уже активен: при непрерывном потоке
        пачки сбрасываются каждые _OUTPUT_BATCH_MS, а не «никогда».
        """
        if self._view is None or self._output_timer is None:
            return  # queued-сигнал после shutdown — вывод уже не рисуем
        self._output_buffer.append(text)
        if not self._output_timer.isActive():
            self._output_timer.start()

    def _flush_output(self):
        if not self._output_buffer:
            return
        text = "".join(self._output_buffer)
        self._output_buffer.clear()
        if self._view is not None:
            self._view.vt_write(text)

    # ---- оболочка (псевдоконсоль) ----

    def _start_shell(self):
        if self._pty is not None:
            return
        if self._view is None:
            return
        if winpty is None:
            self._view.vt_write(
                "[terminal] pywinpty не установлен — выполните:\r\n"
                "    pip install pywinpty\r\n"
            )
            return
        if not os.path.isdir(self._cwd):
            self._cwd = os.path.expanduser("~")
        cols, rows = self._view.screen_size()
        try:
            env = dict(os.environ)
            if self._is_powershell:
                argv = [self._shell, "-NoLogo", "-NoExit", "-Command", "-"]
            else:
                argv = [self._shell, "/Q", "/K"]
            pty = winpty.PtyProcess.spawn(
                argv, cwd=self._cwd, env=env, dimensions=(rows, cols)
            )
        except Exception as exc:
            self._view.vt_write(
                f"[terminal] не удалось запустить {self._shell}: {exc}\r\n"
            )
            return
        self._pty = pty
        self._update_tab_title()
        self._pty_stop = False
        threading.Thread(
            target=self._reader_loop, args=(pty,), daemon=True
        ).start()

    def _reader_loop(self, pty):
        """Фоновый поток: читает вывод ConPTY и шлёт его в главный поток.

        winpty не возвращает EOF из read() после выхода дочернего процесса,
        поэтому процесс проверяем через select с таймаутом + isalive().
        """
        fd = pty.fd
        while not self._pty_stop:
            try:
                ready, _, _ = select.select([fd], [], [], 0.2)
            except (OSError, ValueError):
                break
            if ready:
                try:
                    data = pty.read(4096)
                except Exception:
                    break
                if data:
                    self._output_ready.emit(data)
            elif not pty.isalive():
                self._drain_tail(pty, fd)
                break
        # Поток мог дожить до рестарта оболочки — сигналим только о своей
        # псевдоконсоли, иначе есть риск закрыть свежую.
        if not self._pty_stop and self._pty is pty:
            self._shell_exited.emit(pty)

    def _drain_tail(self, pty, fd):
        """Дописывает хвост вывода, пришедший после смерти процесса."""
        try:
            ready, _, _ = select.select([fd], [], [], 0.3)
            if ready:
                data = pty.read(4096)
                if data:
                    self._output_ready.emit(data)
        except Exception:
            pass

    def _on_shell_exited(self, pty):
        if self._view is None or self._pty is not pty:
            return
        self._close_pty(pty)
        self._pty = None
        self._view.vt_write("[shell exited]\r\n")
        self._start_shell()

    def _close_pty(self, pty, kill: bool = True):
        """Убивает дерево процесса и закрывает псевдоконсоль.

        kill=False — только закрыть псевдоконсоль (пользователь выбрал
        «не убивать» при закрытии вкладки): процесс остаётся в фоне.
        """
        if kill:
            try:
                pid = pty.pid
                if pid:
                    subprocess.Popen(
                        ["taskkill", "/F", "/T", "/PID", str(pid)],
                        creationflags=_CREATE_NO_WINDOW,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
            except Exception:
                pass
        try:
            pty.close(force=True)
        except Exception:
            pass

    # ---- размеры ----

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if self._resize_timer is None:
            self._resize_timer = QTimer(self)
            self._resize_timer.setSingleShot(True)
            self._resize_timer.setInterval(60)
            self._resize_timer.timeout.connect(self._apply_resize)
        self._resize_timer.start()

    def _apply_resize(self):
        pty = self._pty
        if pty is None or self._view is None:
            return
        cols, rows = self._view.screen_size()
        try:
            pty.setwinsize(rows, cols)
        except Exception:
            pass

    # ---- закрытие ----

    def _on_destroyed(self, *args):
        """Вкладку закрыли — гасим процесс и сигналы от него.

        *args — PyQt6 эмитит destroyed с аргументом-объектом.
        """
        _TERMINAL_REGISTRY.pop(self.terminal_id, None)
        try:
            self.shutdown()
        except RuntimeError:
            # В момент разрушения C++-детей уже может не быть
            pass

    def shutdown(self, kill: bool = True):
        """Останавливает псевдоконсоль и её фоновый поток чтения.

        kill=False — закрыть вкладку, не убивая процесс (пользовательский
        выбор в диалоге закрытия); поток чтения останавливается в любом
        случае.
        """
        self._pty_stop = True
        pty = self._pty
        self._pty = None
        if pty is not None:
            self._close_pty(pty, kill=kill)
        timer = self._resize_timer
        self._resize_timer = None
        if timer is not None:
            try:
                timer.stop()
            except RuntimeError:
                pass  # таймер уже уничтожен вместе с виджетом
        out_timer = self._output_timer
        self._output_timer = None
        if out_timer is not None:
            try:
                out_timer.stop()
            except RuntimeError:
                pass
        self._output_buffer.clear()
        self._view = None
