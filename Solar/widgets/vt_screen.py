"""
VtScreen — окно вывода терминала с рендером ANSI/VT.

QPlainTextEdit, в который текст пишется по правилам виртуального терминала:
\r\n, табуляция, backspace, курсорные CSI-последовательности (A/B/C/D/E/F/G/H),
стирание (K/J), OSC-заголовки и приватные режимы игнорируются. Цвета (SGR)
рендерятся 16-цветной палитрой выбранной темы редактора.

Модель: документ = экран + прокрутка вверх. Курсор терминала — пара
(row, col), где row — индекс блока документа. Строки не переносятся
(NoWrap) — как у настоящих терминалов, длинные строки дают горизонтальную
прокрутку, а курсорные команды приходят уже в координатах консоли.
"""

from PyQt6.QtCore import QRectF, Qt
from PyQt6.QtGui import (
    QColor, QFont, QFontMetricsF, QKeyEvent, QPainter, QTextCharFormat,
    QTextCursor, QTextFormat, QWheelEvent,
)
from PyQt6.QtWidgets import QPlainTextEdit


# Классическая xterm-палитра — пока к терминалу не применена тема редактора
_XTERM_PALETTE = [
    "#000000", "#cd3131", "#0dbc79", "#e5e510",
    "#2472c8", "#bc3fbc", "#11a8cd", "#e5e5e5",
    "#666666", "#f14c4c", "#23d18b", "#f5f543",
    "#3b8eea", "#d670d6", "#29b8db", "#ffffff",
]

# Спеки SGR, сохраняемые в QTextCharFormat для перекраски при смене темы:
# индекс палитры (int), QColor (truecolor) или метки-сентинелы.
_SGR_PROP_FG = QTextFormat.Property.UserProperty + 1
_SGR_PROP_BG = QTextFormat.Property.UserProperty + 2
_SGR_DEFAULT_FG = -1  # метка: цвет текста по умолчанию (из темы)
_SGR_DEFAULT_BG = -2  # метка: цвет фона по умолчанию (из темы)


class VtScreen(QPlainTextEdit):
    """Окно терминала: рисует поток вывода псевдоконсоли."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setReadOnly(True)
        self.setUndoRedoEnabled(False)
        self.setFrameStyle(0)
        self.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        self._key_handler = None
        self._zoom_handler = None
        # Позиция курсора терминала (row = блок документа, col = колонка)
        self._row = 0
        self._col = 0
        self._saved = (0, 0)
        self._run = ""       # накопленные печатные символы (вставляем пачкой)
        self._pending = ""   # буфер ESC-последовательности
        self._osc = False    # внутри OSC (до BEL или ESC\)
        self._osc_esc = False
        # Блочная каретка терминала. Qt не рисует каретку в read-only
        # QPlainTextEdit, поэтому рисуем свою в paintEvent (цвет из темы).
        self._caret_color = None
        self._caret_ratio = None  # форма клетки шрифта (ширина/высота)
        # cmd после cls (ConPTY) шлёт \r\n — «проглатываем» один \n,
        # чтобы промпт не сползал на вторую строку. ConPTY при этом остаётся
        # на 1 строку ниже нас — учитываем это смещение в абсолютных
        # позиционированиях (H/f), иначе эхо ввода печатается на 2-й строке.
        self._clear_skip_newline = False
        self._row_offset = 0
        # SGR-атрибуты: _fg/_bg — индекс палитры (int) или QColor (truecolor),
        # None — цвет по умолчанию. Палитра приходит из темы редактора.
        self._palette = [QColor(c) for c in _XTERM_PALETTE]
        self._default_fg = QColor("#d4d4d4")
        self._default_bg = QColor("#1e1e1e")
        self._fg = None
        self._bg = None
        self._bold = False
        self._dim = False
        self._italic = False
        self._underline = False
        self._reverse = False

    def set_key_handler(self, handler):
        self._key_handler = handler

    def set_zoom_handler(self, handler):
        """Обработчик зума (delta: -1/+1/0) для Ctrl+колесо и Ctrl+±/0."""
        self._zoom_handler = handler

    def set_caret_color(self, color):
        """Цвет блочной каретки терминала (обычно — цвет текста темы)."""
        self._caret_color = color
        self.viewport().update()

    def has_selection(self) -> bool:
        """Есть ли выделенный текст (для Ctrl+C-копирования)."""
        return self.textCursor().hasSelection()

    def _caret_width(self) -> float:
        """Ширина клетки символа для блочной каретки (дробная).

        Метрики берём от шрифта документа — им рисуется текст и считается
        высота строки. Ширину каретки привязываем к высоте строки: высота
        меняется на каждом шаге зума, а ширина глифа квантуется пикселями
        и на соседних размерах шрифта может «застревать». Коэффициент формы
        клетки у моноширинного шрифта постоянен, поэтому ширина всегда
        совпадает с реальной клеткой символа.
        """
        fm = QFontMetricsF(self.document().defaultFont())
        h = fm.height()
        if h <= 0:
            return 1.0
        if self._caret_ratio is None:
            cw = fm.averageCharWidth()
            self._caret_ratio = (cw / h) if cw > 0 else 0.5
        return max(1.0, h * self._caret_ratio)

    def paintEvent(self, event):
        super().paintEvent(event)
        if self._caret_color is None or not self.hasFocus():
            return
        cr = self.cursorRect()
        if cr.height() <= 0:
            return
        painter = QPainter(self.viewport())
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(
            QRectF(cr.x(), cr.y(), self._caret_width(), cr.height()),
            self._caret_color,
        )
        painter.end()

    def focusInEvent(self, event):
        super().focusInEvent(event)
        self.viewport().update()

    def focusOutEvent(self, event):
        super().focusOutEvent(event)
        self.viewport().update()

    def keyPressEvent(self, event: QKeyEvent):
        if self._key_handler is not None and self._key_handler(event):
            return
        super().keyPressEvent(event)

    def wheelEvent(self, event: QWheelEvent):
        # Ctrl+колесо — зум шрифта, иначе стандартная прокрутка
        if event.modifiers() & Qt.KeyboardModifier.ControlModifier:
            delta = event.angleDelta().y()
            if delta and self._zoom_handler is not None:
                self._zoom_handler(1 if delta > 0 else -1)
                event.accept()
                return
        super().wheelEvent(event)

    # ---- размеры ----

    def screen_size(self):
        """Размер псевдотерминала (cols, rows) по текущему шрифту."""
        fm = self.fontMetrics()
        cw = max(1, int(fm.averageCharWidth()))
        ch = max(1, int(fm.height()))
        cols = max(1, self.viewport().width() // cw)
        rows = max(1, self.viewport().height() // ch)
        return cols, rows

    # ---- вывод ----

    def vt_write(self, text: str):
        if not text:
            return
        follow = self._at_bottom()
        for ch in text:
            self._feed(ch)
        self._flush_run()
        self._sync(follow)

    def vt_clear(self):
        self.clear()
        self._row = 0
        self._col = 0
        self._run = ""
        self._pending = ""
        self._osc = False
        self._osc_esc = False
        self._clear_skip_newline = True
        self._row_offset = 0
        self._reset_sgr()

    def set_palette(self, palette, default_fg, default_bg):
        """Палитра ANSI (16 цветов) и цвета по умолчанию — из темы.

        Уже выведенный текст перекрашивается по новой палитре.
        """
        self._palette = [QColor(c) for c in palette]
        self._default_fg = QColor(default_fg)
        self._default_bg = QColor(default_bg)
        self._recolor_document()
        self.viewport().update()

    # ---- парсер потока ----

    def _feed(self, ch: str):
        if self._osc:
            if ch == "\x07":
                self._osc = False
            elif ch == "\x1b":
                self._osc_esc = True
            elif self._osc_esc:
                self._osc_esc = False
                self._osc = False
            return
        if self._pending:
            self._flush_run()
            self._feed_pending(ch)
            return
        if ch == "\x1b":
            self._flush_run()
            self._pending = ch
        elif ch in "\r\n\t\b\x0c\x00\x07\x1a\x0e\x0f":
            self._flush_run()
            self._control(ch)
        else:
            self._run += ch

    def _control(self, ch: str):
        if ch == "\r":
            self._col = 0
        elif ch == "\n":
            if self._clear_skip_newline:
                # Сразу после cls cmd шлёт \r\n — в настоящей консоли
                # курсор остаётся на верхней строке, пропускаем сдвиг.
                # ConPTY же сдвинулся — запоминаем расхождение строк.
                self._clear_skip_newline = False
                # Офсет не копим сверх 1: после cls лишний \n бывает ровно
                # один, а повторных сбросов экрана без cls не бывает.
                self._row_offset = min(1, self._row_offset + 1)
                return
            self._move_down(1)
        elif ch == "\t":
            stop = ((self._col // 8) + 1) * 8
            n = stop - self._col
            if n > 0:
                # Таб = пробелы до позиции 8; применяются одной операцией
                # в _flush_run, а не посимвольно через _put_char.
                self._run += " " * n
        elif ch == "\b":
            self._col = max(0, self._col - 1)
        elif ch == "\x0c":
            self.vt_clear()
        # NUL, BEL, SUB, SO, SI — не влияют на текст

    def _feed_pending(self, ch: str):
        if self._pending == "\x1b":
            if ch == "[":
                self._pending = "\x1b["
            elif ch == "]":
                self._osc = True
                self._pending = ""
            elif ch == "7":
                self._saved = (self._row, self._col)
                self._pending = ""
            elif ch == "8":
                self._row, self._col = self._saved
                self._pending = ""
            else:
                self._pending = ""  # ESC + одиночный символ — игнорируем
            return
        # CSI: копим параметры до финального байта (0x40–0x7E)
        buf = self._pending + ch
        if 0x40 <= ord(ch) <= 0x7E:
            self._pending = ""
            self._apply_csi(buf)
        else:
            self._pending = buf

    def _apply_csi(self, buf: str):
        body = buf[2:]  # без ESC[
        if not body:
            return
        final = body[-1]
        params = body[:-1]
        if params.startswith("?"):
            return  # приватные режимы (?25h, ?2004h...) — на текст не влияют
        try:
            nums = [int(p) if p else 0 for p in params.split(";")]
        except ValueError:
            return
        if not nums:
            nums = [0]
        n = nums[0] or 1
        if final == "A":
            self._row = max(0, self._row - n)
        elif final == "B":
            self._move_down(n)
        elif final == "C":
            self._col += n
        elif final == "D":
            self._col = max(0, self._col - n)
        elif final == "E":
            self._move_down(n)
            self._col = 0
        elif final == "F":
            self._row = max(0, self._row - n)
            self._col = 0
        elif final == "G":
            self._col = max(0, n - 1)
        elif final in ("H", "f"):
            self._row = max(0, (nums[0] or 1) - 1 - self._row_offset)
            c = nums[1] if len(nums) > 1 else 1
            self._col = max(0, c - 1)
        elif final == "K":
            self._erase_line(nums[0] if nums else 0)
        elif final == "J":
            self._erase_display(nums[0] if nums else 0)
        elif final == "m":
            self._apply_sgr(nums)
        elif final == "s":
            self._saved = (self._row, self._col)
        elif final == "u":
            self._row, self._col = self._saved
        # прочие (h/l — режимы, t — окно, c — запрос...) игнорируем

    # ---- SGR-атрибуты (цвета и начертание) ----

    def _apply_sgr(self, nums: list):
        i = 0
        while i < len(nums):
            n = nums[i]
            if n == 0:
                self._reset_sgr()
            elif n == 1:
                self._bold = True
            elif n == 2:
                self._dim = True
            elif n == 3:
                self._italic = True
            elif n == 4:
                self._underline = True
            elif n == 7:
                self._reverse = True
            elif n == 22:
                self._bold = False
                self._dim = False
            elif n == 23:
                self._italic = False
            elif n == 24:
                self._underline = False
            elif n == 27:
                self._reverse = False
            elif n == 39:
                self._fg = None
            elif n == 49:
                self._bg = None
            elif 30 <= n <= 37:
                self._fg = n - 30
            elif 40 <= n <= 47:
                self._bg = n - 40
            elif 90 <= n <= 97:
                self._fg = 8 + n - 90
            elif 100 <= n <= 107:
                self._bg = 8 + n - 100
            elif n in (38, 48):
                # Расширенные: 38;5;idx (256 цветов) / 38;2;r;g;b (truecolor)
                if i + 1 < len(nums) and nums[i + 1] == 5 \
                        and i + 2 < len(nums):
                    self._set_sgr_color(n, nums[i + 2])
                    i += 2
                elif i + 1 < len(nums) and nums[i + 1] == 2 \
                        and i + 4 < len(nums):
                    self._set_sgr_color(
                        n, QColor(nums[i + 2], nums[i + 3], nums[i + 4])
                    )
                    i += 4
            i += 1

    def _reset_sgr(self):
        self._fg = None
        self._bg = None
        self._bold = False
        self._dim = False
        self._italic = False
        self._underline = False
        self._reverse = False

    def _set_sgr_color(self, code: int, value):
        if code == 38:
            self._fg = value
        else:
            self._bg = value

    def _index_color(self, idx: int) -> QColor:
        if idx < 16:
            return self._palette[idx]
        if idx < 232:  # куб 6×6×6
            idx -= 16
            r, g, b = idx // 36, (idx % 36) // 6, idx % 6
            return QColor(*[55 + c * 40 for c in (r, g, b)])
        v = 8 + (idx - 232) * 10  # оттенки серого
        return QColor(v, v, v)

    def _resolve_color(self, value) -> QColor:
        if isinstance(value, QColor):
            return QColor(value)
        return self._index_color(value)

    def _current_format(self) -> QTextCharFormat:
        fg = self._fg_color()
        bg = self._bg_color()
        if self._reverse:
            fg, bg = bg, fg
            if fg is None:
                fg = self._default_bg
            if bg is None:
                bg = self._default_fg
        fmt = QTextCharFormat()
        if fg is not None:
            fmt.setForeground(fg)
        if bg is not None:
            fmt.setBackground(bg)
        # Спеки (исходные коды SGR) — для перекраски при смене темы
        if self._reverse:
            fg_spec = self._bg if self._bg is not None else _SGR_DEFAULT_BG
            bg_spec = self._fg if self._fg is not None else _SGR_DEFAULT_FG
        else:
            fg_spec, bg_spec = self._fg, self._bg
        if fg_spec is not None:
            fmt.setProperty(_SGR_PROP_FG, fg_spec)
        if bg_spec is not None:
            fmt.setProperty(_SGR_PROP_BG, bg_spec)
        if self._bold and not self._dim:
            fmt.setFontWeight(QFont.Weight.Bold)
        elif self._dim:
            fmt.setFontWeight(QFont.Weight.Light)
        if self._italic:
            fmt.setFontItalic(True)
        if self._underline:
            fmt.setFontUnderline(True)
        return fmt

    def _spec_color(self, spec, fmt: QTextCharFormat = None,
                    is_fg: bool = False):
        """Цвет спека SGR по текущей палитре (для перекраски документа)."""
        if spec is None:
            return None
        if spec == _SGR_DEFAULT_FG:
            color = self._default_fg
        elif spec == _SGR_DEFAULT_BG:
            color = self._default_bg
        elif isinstance(spec, QColor):
            return QColor(spec)
        else:
            color = self._index_color(spec)
            if is_fg and fmt is not None \
                    and fmt.fontWeight() == QFont.Weight.Bold and spec < 8:
                color = self._palette[spec + 8]  # bold = яркая версия
        if is_fg and fmt is not None \
                and fmt.fontWeight() == QFont.Weight.Light:
            color = QColor(color).darker(150)  # dim
        return color

    def _recolor_document(self):
        """Перекрашивает уже выведенный текст по новой палитре темы."""
        doc = self.document()
        cur = QTextCursor(doc)
        cur.beginEditBlock()
        block = doc.firstBlock()
        while block.isValid():
            it = block.begin()
            while not it.atEnd():
                frag = it.fragment()
                fmt = frag.charFormat()
                fg_spec = fmt.property(_SGR_PROP_FG)
                bg_spec = fmt.property(_SGR_PROP_BG)
                new_fmt = None
                if fg_spec is not None or bg_spec is not None:
                    # SGR-фрагмент: цвет пересчитываем по новой палитре
                    new_fmt = QTextCharFormat(fmt)
                    fg = self._spec_color(fg_spec, fmt, True)
                    bg = self._spec_color(bg_spec, fmt, False)
                    if fg is not None:
                        new_fmt.setForeground(fg)
                    else:
                        new_fmt.clearForeground()
                    if bg is not None:
                        new_fmt.setBackground(bg)
                    else:
                        new_fmt.clearBackground()
                elif (fmt.foreground().style() != Qt.BrushStyle.NoBrush
                      or fmt.background().style() != Qt.BrushStyle.NoBrush):
                    # Дефолтный текст: явные цвета старой темы — сбрасываем,
                    # чтобы действовали новые из QSS/палитры
                    new_fmt = QTextCharFormat(fmt)
                    new_fmt.clearForeground()
                    new_fmt.clearBackground()
                if new_fmt is not None:
                    cur.setPosition(frag.position())
                    cur.setPosition(
                        frag.position() + frag.length(),
                        QTextCursor.MoveMode.KeepAnchor,
                    )
                    cur.setCharFormat(new_fmt)
                it += 1
            block = block.next()
        cur.endEditBlock()

    def _fg_color(self):
        if self._fg is None:
            color = self._default_fg
        elif isinstance(self._fg, int) and self._bold and self._fg < 8:
            color = self._palette[self._fg + 8]  # bold = яркая версия
        else:
            color = self._resolve_color(self._fg)
        if self._dim:
            color = QColor(color).darker(150)
        return color

    def _bg_color(self):
        if self._bg is None:
            return None
        return self._resolve_color(self._bg)

    # ---- перевод строки / скролл экрана ----

    def _move_down(self, n: int):
        """Сдвиг курсора вниз с терминальным скроллом.

        Настоящий терминал не растягивает экран: когда курсор доходит до
        последней строки, содержимое сдвигается вверх, внизу появляется
        пустая строка, а ушедшие строки остаются в scrollback документа.
        Без этого \x1b[H перезаписывал бы самую первую строку истории,
        а не верх экрана — вывод "накладывался" на старые строки.
        """
        self._row += n
        rows = self.screen_size()[1]
        if self._row < rows:
            return
        over = self._row - (rows - 1)
        self._row = rows - 1
        if over > 0:
            end = self.textCursor()
            end.movePosition(QTextCursor.MoveOperation.End)
            end.insertText("\n" * over)

    # ---- операции с документом ----

    def _flush_run(self):
        run = self._run
        self._run = ""
        if not run:
            return
        self._clear_skip_newline = False
        c = self._cursor_at()
        block_text = c.block().text()
        replace = min(len(run), max(0, len(block_text) - self._col))
        if replace:
            c.setPosition(
                c.position() + replace,
                QTextCursor.MoveMode.KeepAnchor,
            )
            c.removeSelectedText()
        c.insertText(run, self._current_format())
        self._col += len(run)

    def _put_char(self, ch: str):
        self._clear_skip_newline = False
        c = self._cursor_at()
        block_text = c.block().text()
        fmt = self._current_format()
        if self._col < len(block_text):
            c.deleteChar()
        c.insertText(ch, fmt)
        self._col += 1

    def _erase_line(self, mode: int):
        c = self._cursor_at()
        if mode == 2:
            c.movePosition(QTextCursor.MoveOperation.StartOfBlock)
            c.movePosition(
                QTextCursor.MoveOperation.EndOfBlock,
                QTextCursor.MoveMode.KeepAnchor,
            )
        elif mode == 1:
            c.movePosition(
                QTextCursor.MoveOperation.StartOfBlock,
                QTextCursor.MoveMode.KeepAnchor,
            )
        else:  # 0 — от курсора до конца строки
            c.movePosition(
                QTextCursor.MoveOperation.EndOfBlock,
                QTextCursor.MoveMode.KeepAnchor,
            )
        c.removeSelectedText()

    def _erase_display(self, mode: int):
        if mode in (2, 3):
            self.vt_clear()
            return
        c = self._cursor_at()
        if mode == 0:
            c.movePosition(
                QTextCursor.MoveOperation.End,
                QTextCursor.MoveMode.KeepAnchor,
            )
            c.removeSelectedText()
        else:  # 1 — от начала документа до курсора
            c.movePosition(
                QTextCursor.MoveOperation.Start,
                QTextCursor.MoveMode.KeepAnchor,
            )
            c.removeSelectedText()
            self._row = 0
            self._col = 0

    def _cursor_at(self) -> QTextCursor:
        doc = self.document()
        rows = self.screen_size()[1]
        # Экран = последние rows строк документа, всё выше — scrollback.
        start = max(0, doc.blockCount() - rows)
        need = start + self._row + 1 - doc.blockCount()
        if need > 0:
            end = self.textCursor()
            end.movePosition(QTextCursor.MoveOperation.End)
            end.insertText("\n" * need)
            start = max(0, doc.blockCount() - rows)
        c = self.textCursor()
        c.movePosition(QTextCursor.MoveOperation.Start)
        row = start + self._row
        if row:
            c.movePosition(QTextCursor.MoveOperation.NextBlock, n=row)
        if self._col:
            c.movePosition(QTextCursor.MoveOperation.Right, n=self._col)
        return c

    def _sync(self, follow: bool):
        old = self.cursorRect()
        c = self._cursor_at()
        self.setTextCursor(c)
        if follow:
            self.ensureCursorVisible()
        new = self.cursorRect()
        # Перерисовать области старой и новой позиции блочной каретки
        cw = max(1, int(self._caret_width()))
        self.viewport().update(old.adjusted(0, 0, cw + 1, 1))
        self.viewport().update(new.adjusted(0, 0, cw + 1, 1))

    def _at_bottom(self) -> bool:
        sb = self.verticalScrollBar()
        return sb.value() >= sb.maximum() - 1

    def scroll_to_bottom(self):
        """Принудительно прокручивает документ до конца.

        Нужно при запуске таска: вывод после этого снова следует за
        курсором (_sync), даже если пользователь был проскроллен вверх.
        """
        sb = self.verticalScrollBar()
        sb.setValue(sb.maximum())
        self.ensureCursorVisible()
