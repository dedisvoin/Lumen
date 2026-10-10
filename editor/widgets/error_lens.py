"""
ErrorLens — inline-сообщения ошибок/предупреждений clangd и inlay-подсказки.

Сообщения и подсказки рисуются поверх кода прямо в paintEvent редактора
через QPainter(viewport()) — как в TwistLang (editor_v2.py, CustomScintilla),
поэтому ничего не накладывается поверх UI. Inlay-подсказки (auto a: int = 10;)
рисуются как в VS Code: текст после подсказки визуально сдвигается вправо —
«хвост» строки перекрывается фоном и перерисовывается со сдвигом, стиль
каждого символа берётся из лексера.
"""

from PyQt6.QtCore import QObject, Qt
from PyQt6.QtGui import QColor, QFont, QFontMetrics, QPainter, QPen

from editor.constants import CODE_FONT_SIZE
from editor.font_settings import code_font_family
from editor.scintilla_constants import (
    SCI_GETFIRSTVISIBLELINE, SCI_GETLINEENDPOSITION, SCI_LINESONSCREEN,
    SCI_POINTXFROMPOSITION, SCI_POINTYFROMPOSITION, SCI_POSITIONFROMLINE,
    SCI_LINEFROMPOSITION, SCI_TEXTHEIGHT, SCI_GETLENGTH,
    SCI_GETCURRENTPOS, SCI_GETSTYLEAT, SCI_GETWRAPMODE,
    SCI_GETCARETLINEVISIBLE, SCI_GETCARETLINEBACK,
    SCI_GETCARETSTYLE, SCI_TEXTWIDTH, SCI_GETMARGINWIDTHN,
    SCI_GETLINEVISIBLE,
)

_GAP_CHARS = 2     # отступ от конца кода (в символах, зумится)
_PAD = 7           # отступ текста внутри подложки
_ALPHA = 230       # прозрачность текста
_RECT_ALPHA = 55   # прозрачность подложки
_MAX_CHARS = 240   # обрезка текста диагностики
_ERR_DARK = QColor("#f14c4c")
_WARN_DARK = QColor("#e5c07b")
_ERR_LIGHT = QColor("#c62828")
_WARN_LIGHT = QColor("#9a6b00")
# Inlay-подсказки: приглушённый серый, курсив (как в VS Code)
_HINT_DARK = QColor("#8a9199")
_HINT_LIGHT = QColor("#57606a")
_HINT_ALPHA = 210
_CTRL_OUTLINE_ALPHA = 170  # обводка рамки Ctrl-токена (без заливки)
_HOVER_ALPHA = 25  # hover-токен: слабее Ctrl (прозрачнее), но видимо
_CARET_DARK = QColor("#e8e8e8")
_CARET_LIGHT = QColor("#111111")
_BG_DARK = QColor("#1e1e1e")
_BG_LIGHT = QColor("#ffffff")


class ErrorLensOverlay(QObject):
    """Рисует сообщения диагностик и inlay-подсказки в paintEvent редактора."""

    def __init__(self, editor):
        super().__init__(editor)
        self.editor = editor
        self._line_msgs: dict = {}
        self._inlay_hints: list = []   # [(байтовая позиция, текст)]
        self._err = QColor(_ERR_DARK)
        self._warn = QColor(_WARN_DARK)
        self._hint = QColor(_HINT_DARK)
        self._caret = QColor(_CARET_DARK)
        self._dark = True
        self._hint_font_cache = None
        self._style_cache = {}         # style -> (QFont, QColor)
        self._ctrl_range = None        # (start, end, QColor) токена под Ctrl
        self._hover_range = None       # (start, end, QColor) литерала под мышью
        self._diag_ranges = []         # (line, start, end, severity, message)
        try:
            self._caret_style = editor.SendScintilla(SCI_GETCARETSTYLE)
        except RuntimeError:
            self._caret_style = 2      # блок — дефолт редактора

    # --- публичный интерфейс для LspEditorBridge ---

    def set_messages(self, line_msgs: dict):
        self._line_msgs = line_msgs
        self.refresh()

    def clear(self):
        self._line_msgs = {}
        self._diag_ranges = []
        self.refresh()

    def set_inlay_hints(self, hints: list):
        self._inlay_hints = hints
        self.refresh()

    def set_diag_ranges(self, ranges: list):
        """Диапазоны диагностик для подчёркивания (со сдвигом хинтов)."""
        self._diag_ranges = ranges
        self.refresh()

    def shift_inlay_hints(self, edit_point: int, delta: int):
        """Сдвигает позиции хинтов после точки правки — кеш не исчезает.

        Пока clangd не прислал свежие, старые подсказки «едут» вместе
        с кодом, как в VS Code, вместо того чтобы пропадать на время
        набора и анализа.
        """
        if not self._inlay_hints or not delta:
            return
        out = []
        for pos, text in self._inlay_hints:
            if pos >= edit_point:
                pos = max(pos + delta, edit_point)
            out.append((pos, text))
        self._inlay_hints = out
        self.refresh()

    def should_hide_caret(self) -> bool:
        """True, если каретка лежит в сдвигаемой inlay-области.

        Scintilla рисует свою каретку на СТАРОМ (несдвинутом) месте и при
        этом перерисовывает её поверх нашего оверлея (виден «призрак» на
        конце строки). На таких строках каретку Scintilla прячем, а видимую
        рисуем сами в правильной позиции.
        """
        ed = self.editor
        try:
            if not self._inlay_hints:
                return False
            caret = ed.SendScintilla(SCI_GETCURRENTPOS)
            line = ed.SendScintilla(SCI_LINEFROMPOSITION, caret)
            # Фолбэк без сдвига (см. _paint_line_hints): каретка корректна
            if ed.SendScintilla(SCI_GETWRAPMODE) != 0:
                return False
            line_start = ed.SendScintilla(SCI_POSITIONFROMLINE, line)
            line_end = ed.SendScintilla(SCI_GETLINEENDPOSITION, line)
            if not (line_start <= caret <= line_end):
                return False
            line_str = ed.text(line)
            if len(line_str.encode("utf-8")) != len(line_str):
                return False
            if ed.hasSelectedText() and self._line_has_selection(ed, line):
                return False
            # Есть ли сдвигающая подсказка до позиции каретки (включая
            # позицию привязки: подсказка вставлена ПЕРЕД символом pos,
            # поэтому каретка на pos визуально стоит ПОСЛЕ подсказки)
            for pos, _text in self._inlay_hints:
                if line_start <= pos <= caret:
                    return True
            return False
        except RuntimeError:
            return False

    def adjust_click_x(self, line, x) -> int:
        """Корректирует x клика для строки со сдвигающими inlay-подсказками.

        Возвращает x в системе координат БЕЗ сдвига, чтобы Scintilla поставил
        каретку на символ, который пользователь видит под курсором, а не на
        «сдвинутый» в сторону. Клик по тексту самой подсказки ставит каретку
        на символ, к которому подсказка привязана.
        """
        ed = self.editor
        try:
            if not self._inlay_hints:
                return x
            # Фолбэк без сдвига (см. _paint_line_hints) — сдвигать нечего
            if ed.SendScintilla(SCI_GETWRAPMODE) != 0:
                return x
            line_start = ed.SendScintilla(SCI_POSITIONFROMLINE, line)
            line_end = ed.SendScintilla(SCI_GETLINEENDPOSITION, line)
            line_str = ed.text(line)
            if len(line_str.encode("utf-8")) != len(line_str):
                return x
            if ed.hasSelectedText() and self._line_has_selection(ed, line):
                return x
            hints = [(p, t) for p, t in self._inlay_hints
                     if line_start <= p < line_end]
            hints.sort()  # как в _paint_hints — сдвиг должен совпадать с отрисовкой
            if not hints:
                return x
            hf = self._hint_font()
            fm = QFontMetrics(hf)
            cum = 0
            for pos, text in hints:
                hx = ed.SendScintilla(SCI_POINTXFROMPOSITION, 0, pos) + cum
                hw = fm.horizontalAdvance(text)
                if x < hx:
                    return x - cum            # до подсказки: сдвиг до неё
                if x <= hx + hw:
                    # клик по тексту подсказки: каретка на позицию ВСТАВКИ
                    # подсказки (anchor). НЕ anchor+cw-1 — последний пиксель
                    # ячейки Scintilla трактует как следующий символ, и
                    # каретка оказывается в КОНЦЕ подсказки
                    anchor = ed.SendScintilla(SCI_POINTXFROMPOSITION, 0, pos)
                    return min(x - cum, anchor)
                cum += hw                     # подсказка пройдена
            return x - cum                    # после всех подсказок
        except RuntimeError:
            return x

    def set_ctrl_range(self, start, end=None, color=None):
        """Токен под Ctrl: рисуем рамку (без заливки) со сдвигом в оверлее.

        Scintilla-индикатор рисует на НЕсдвинутых позициях и на строках
        с inlay-подсказками не совпадает с видимым словом.
        """
        self._ctrl_range = (start, end, color) if start is not None else None
        self.refresh()

    def set_hover_range(self, start, end=None, color=None):
        """Литерал под мышью: слабое блочное выделение (прозрачнее Ctrl)."""
        self._hover_range = (start, end, color) if start is not None else None
        self.refresh()

    def _ctrl_shift(self, ed, line, pos) -> int:
        """Сдвиг пикселей в строке line ДО позиции pos (как в adjust_click_x)."""
        try:
            if not self._inlay_hints:
                return 0
            if ed.SendScintilla(SCI_GETWRAPMODE) != 0:
                return 0
            line_start = ed.SendScintilla(SCI_POSITIONFROMLINE, line)
            line_end = ed.SendScintilla(SCI_GETLINEENDPOSITION, line)
            line_str = ed.text(line)
            if len(line_str.encode("utf-8")) != len(line_str):
                return 0
            if ed.hasSelectedText() and self._line_has_selection(ed, line):
                return 0
            if not (line_start <= pos <= line_end):
                return 0
            fm = QFontMetrics(self._hint_font())
            cum = 0
            for hp, text in self._inlay_hints:
                if not (line_start <= hp < line_end):
                    continue
                if hp > pos:
                    break
                cum += fm.horizontalAdvance(text)
            return cum
        except RuntimeError:
            return 0

    @staticmethod
    def margin_x(ed) -> int:
        """Суммарная ширина левых маргинов.

        SCI_POINTXFROMPOSITION возвращает x от начала текстовой области
        (без маргинов), а QPainter(viewport) и клики работают в координатах
        viewport (с маргинами) — разница и есть суммарная ширина маргинов.
        """
        total = 0
        try:
            for i in range(5):
                total += ed.SendScintilla(SCI_GETMARGINWIDTHN, i)
        except RuntimeError:
            pass
        return total

    def _paint_ctrl_underline(self, p, ed, line_h):
        """Рамка Ctrl-токена (без заливки) со сдвигом inlay-подсказок."""
        self._paint_token_block(p, ed, line_h, self._ctrl_range,
                                _CTRL_OUTLINE_ALPHA, outline=True)

    def _paint_hover_underline(self, p, ed, line_h):
        """Слабое выделение литерала под мышью (прозрачнее Ctrl)."""
        self._paint_token_block(p, ed, line_h, self._hover_range, _HOVER_ALPHA)

    def _paint_token_block(self, p, ed, line_h, rng, alpha, outline=False):
        """Блок-заливка или рамка диапазона токена (Ctrl/hover).

        outline=True — только обводка без заливки (Ctrl-токен).
        """
        if not rng:
            return
        start, end, color = rng
        if end is None or end <= start:
            return
        try:
            line = ed.SendScintilla(SCI_LINEFROMPOSITION, start)
            first = ed.SendScintilla(SCI_GETFIRSTVISIBLELINE)
            visible = ed.SendScintilla(SCI_LINESONSCREEN)
            if not (first - 1 <= line <= first + visible + 1):
                return
            sh_s = self._ctrl_shift(ed, line, start)
            sh_e = self._ctrl_shift(ed, line, end - 1)
            x0 = ed.SendScintilla(SCI_POINTXFROMPOSITION, 0, start) + sh_s
            x1 = (ed.SendScintilla(SCI_POINTXFROMPOSITION, 0, end - 1)
                  + sh_e + (ed.SendScintilla(SCI_TEXTWIDTH, 0, b"0") or 1))
            y = ed.SendScintilla(SCI_POINTYFROMPOSITION, 0, start)
            col = QColor(color) if color is not None else QColor(self._hint)
            col.setAlpha(alpha)
            w = int(max(x1 - x0, 2))
            if outline:
                p.setPen(QPen(col, 1))
                p.setBrush(Qt.BrushStyle.NoBrush)
                p.drawRect(int(x0), int(y), w, line_h - 1)
            else:
                p.fillRect(int(x0), int(y), w, line_h, col)
        except Exception:
            pass

    def _paint_diag_underlines(self, p, ed, line_h):
        """Подчёркивание ошибок/предупреждений со сдвигом inlay-подсказок.

        Scintilla-индикатор рисуется на несдвинутых позициях и на строках
        с inlay-подсказками не совпадает с видимым текстом — рисуем сами,
        как рамку Ctrl-токена (_paint_token_block).
        """
        if not self._diag_ranges:
            return
        try:
            first = ed.SendScintilla(SCI_GETFIRSTVISIBLELINE)
            visible = ed.SendScintilla(SCI_LINESONSCREEN)
            cw = ed.SendScintilla(SCI_TEXTWIDTH, 0, b"0") or 1
            for line, start, end, severity, _msg in self._diag_ranges:
                if not (first - 1 <= line <= first + visible + 1):
                    continue
                if end <= start:
                    end = start + 1
                sh_s = self._ctrl_shift(ed, line, start)
                sh_e = self._ctrl_shift(ed, line, end - 1)
                x0 = ed.SendScintilla(SCI_POINTXFROMPOSITION, 0, start) + sh_s
                x1 = (ed.SendScintilla(SCI_POINTXFROMPOSITION, 0, end - 1)
                      + sh_e + cw)
                y = ed.SendScintilla(SCI_POINTYFROMPOSITION, 0, start)
                color = self._err if severity <= 1 else self._warn
                y0 = y + line_h - 2 if line_h >= 3 else y
                p.fillRect(int(x0), int(y0), int(max(x1 - x0, 2)), 2, color)
        except Exception:
            pass  # редактор уничтожен

    def apply_theme(self, dark, hint_color=None, error_color=None,
                    warning_color=None):
        """Цвета текста по типу темы; цвета из темы или дефолты."""
        self._dark = dark
        self._err = (QColor(error_color) if error_color
                     else QColor(_ERR_DARK if dark else _ERR_LIGHT))
        self._warn = (QColor(warning_color) if warning_color
                      else QColor(_WARN_DARK if dark else _WARN_LIGHT))
        self._hint = (QColor(hint_color) if hint_color
                      else QColor(_HINT_DARK if dark else _HINT_LIGHT))
        self._caret = QColor(_CARET_DARK if dark else _CARET_LIGHT)
        self._style_cache = {}
        self.refresh()

    def update(self):
        """Размер шрифта мог измениться (zoom) — перерисовать."""
        self._hint_font_cache = None
        self._style_cache = {}
        self.refresh()

    def refresh(self):
        try:
            self.editor.viewport().update()
        except RuntimeError:
            pass  # редактор уничтожен

    # --- отрисовка (вызывается из CodeEditor.paintEvent после super) ---

    def paint(self):
        ed = self.editor
        try:
            view = ed.viewport()
        except RuntimeError:
            return
        painter = QPainter(view)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        try:
            font = self._code_font()
            painter.setFont(font)
            fm = painter.fontMetrics()
            line_h = ed.SendScintilla(SCI_TEXTHEIGHT, 0) or fm.height()
            cw = fm.horizontalAdvance("0") or 1
            first = ed.SendScintilla(SCI_GETFIRSTVISIBLELINE)
            visible = ed.SendScintilla(SCI_LINESONSCREEN)
            # Отрисовка — только правее маргинов: плашка номеров строк
            # всегда поверх оверлея («хинты под плашкой», а не над ней)
            mx = self.margin_x(ed)
            if mx > 0:
                painter.setClipRect(mx, 0, max(view.width() - mx, 1),
                                    view.height())
            # Сначала подсказки: они сдвигают текст, сдвиг нужен и error lens
            shifts = {}
            if self._inlay_hints:
                shifts = self._paint_hints(painter, ed, first, visible, line_h)
                painter.setFont(font)  # подсказки меняли шрифт/перо
            # Подчёркивание диагностик — ПОСЛЕ хинтов: хвост строки уже
            # сдвинут, рисуем по видимым позициям, а не несдвинутым
            self._paint_diag_underlines(painter, ed, line_h)
            for line in range(first, first + visible + 1):
                msgs = self._line_msgs.get(line)
                if msgs:
                    self._paint_line(painter, ed, line, msgs, cw, line_h,
                                     shifts.get(line, 0))
            if self._ctrl_range:
                self._paint_ctrl_underline(painter, ed, line_h)
            elif self._hover_range:
                self._paint_hover_underline(painter, ed, line_h)
        except RuntimeError:
            pass  # редактор уничтожен
        finally:
            painter.end()

    # --- inlay-подсказки со сдвигом текста (как VS Code) ---

    def _paint_hints(self, p, ed, first, visible, line_h) -> dict:
        """Возвращает {line: сдвиг_пикселей} для строк со сдвинутым текстом."""
        doc_len = ed.SendScintilla(SCI_GETLENGTH)
        by_line: dict = {}
        for pos, text in self._inlay_hints:
            if pos < 0 or pos > doc_len:
                continue
            line = ed.SendScintilla(SCI_LINEFROMPOSITION, pos)
            if not (first - 1 <= line <= first + visible + 1):
                continue
            # В свёрнутом (скрытом фолдом) куске кода подсказки не рисуем
            if not ed.SendScintilla(SCI_GETLINEVISIBLE, line):
                continue
            by_line.setdefault(line, []).append((pos, text))
        shifts = {}
        for line, hints in by_line.items():
            hints.sort()
            s = self._paint_line_hints(p, ed, line, hints, line_h)
            if s:
                shifts[line] = s
        return shifts

    def _paint_line_hints(self, p, ed, line, hints, line_h) -> int:
        """Подсказки одной строки; сдвигает «хвост» строки. Возвращает сдвиг."""
        line_start = ed.SendScintilla(SCI_POSITIONFROMLINE, line)
        line_end = ed.SendScintilla(SCI_GETLINEENDPOSITION, line)
        line_str = ed.text(line)
        line_bytes = line_str.encode("utf-8")
        hf = self._hint_font()
        fm = QFontMetrics(hf)
        hc = QColor(self._hint)
        hc.setAlpha(_HINT_ALPHA)

        # Сдвиг невозможен: перенос строк, не-ASCII, выделение на строке.
        # Подсказки не рисуем — иначе они лягут ПОВЕРХ кода без сдвига
        # (выглядит как наложение). После снятия выделения всё вернётся.
        if (ed.SendScintilla(SCI_GETWRAPMODE) != 0
                or len(line_bytes) != len(line_str)
                or (ed.hasSelectedText() and self._line_has_selection(ed, line))):
            return 0

        bg = self._line_bg(ed, line)
        caret = ed.SendScintilla(SCI_GETCURRENTPOS)
        caret_line = ed.SendScintilla(SCI_LINEFROMPOSITION, caret)
        x_end = ed.SendScintilla(SCI_POINTXFROMPOSITION, 0, line_end)
        y = ed.SendScintilla(SCI_POINTYFROMPOSITION, 0, hints[0][0])

        cum = 0
        first_pos = None
        hint_widths = []   # (позиция, ширина) сдвигающих подсказок
        for pos, text in hints:
            if pos >= line_end:  # подсказка в конце строки — без сдвига
                x = ed.SendScintilla(SCI_POINTXFROMPOSITION, 0, pos)
                p.setFont(hf)
                p.setPen(QPen(hc))
                p.drawText(int(x), int(y) + fm.ascent(), text)
                continue
            x = ed.SendScintilla(SCI_POINTXFROMPOSITION, 0, pos) + cum
            hw = fm.horizontalAdvance(text)
            hint_widths.append((pos, hw))
            if first_pos is None:
                first_pos = pos
            # Перекрыть «хвост» строки фоном, нарисовать подсказку и сдвинуть
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(bg)
            p.drawRect(int(x), int(y), int((x_end + cum) - x + 4), line_h)
            p.setFont(hf)
            p.setPen(QPen(hc))
            p.drawText(int(x), int(y) + fm.ascent(), text)
            self._draw_shifted_tail(p, ed, line, pos, x + hw, y)
            cum += hw

        # Каретка в сдвигаемой области: Scintilla рисует её на старом месте.
        # Сдвиг до позиции каретки = сумма ширин подсказок строго до неё.
        if (cum and first_pos is not None and caret_line == line
                and line_start <= caret <= line_end and caret >= first_pos):
            cshift = 0
            for hp, hw in hint_widths:
                if hp <= caret:
                    cshift += hw
                else:
                    break
            cx = ed.SendScintilla(SCI_POINTXFROMPOSITION, 0, caret) + cshift
            caret_color = self._caret_color(ed)
            cw_px = fm.horizontalAdvance("0") or 1
            p.fillRect(int(cx), int(y), int(cw_px), line_h, caret_color)
        return cum

    def _draw_shifted_tail(self, p, ed, line, from_pos, x0, y):
        """Перерисовывает «хвост» строки со сдвигом, по стилям лексера."""
        line_start = ed.SendScintilla(SCI_POSITIONFROMLINE, line)
        line_end = ed.SendScintilla(SCI_GETLINEENDPOSITION, line)
        if from_pos >= line_end:
            return
        tail = ed.text(line)[from_pos - line_start:]
        lexer = getattr(ed, "current_lexer", None) or ed.lexer()
        tx = x0
        i = 0
        n = len(tail)
        while i < n:
            style = ed.SendScintilla(SCI_GETSTYLEAT, from_pos + i)
            j = i + 1
            while (j < n and ed.SendScintilla(SCI_GETSTYLEAT, from_pos + j)
                   == style):
                j += 1
            font, color = self._style_info(lexer, style)
            p.setFont(font)
            p.setPen(QPen(color))
            fmi = QFontMetrics(font)
            run = tail[i:j]
            p.drawText(int(tx), int(y) + fmi.ascent(), run)
            tx += fmi.horizontalAdvance(run)
            i = j

    def _caret_color(self, ed) -> QColor:
        """Цвет каретки из темы (colors.caret), как у блока Scintilla."""
        tm = getattr(ed, "theme_manager", None)
        theme = getattr(ed, "_current_theme", None)
        if tm is not None and theme:
            colors = (tm.themes.get(theme, {}) or {}).get("colors", {})
            v = colors.get("caret")
            if v:
                c = QColor(v)
                if c.isValid():
                    return c
        return self._caret

    def _style_info(self, lexer, style):
        """(шрифт, цвет) стиля лексера с кэшем (обнуляется при теме/зуме)."""
        info = self._style_cache.get(style)
        if info is None:
            font = None
            color = None
            if lexer is not None:
                try:
                    font = QFont(lexer.font(style))
                    color = QColor(lexer.color(style))
                    if not color.isValid():
                        color = None
                except (RuntimeError, TypeError):
                    font = None
            if font is None or font.pointSize() <= 0:
                font = self._code_font()
            if color is None:
                color = QColor("#d4d4d4" if self._dark else "#1f1f1f")
            info = (font, color)
            self._style_cache[style] = info
        return info

    def _line_bg(self, ed, line) -> QColor:
        """Фон строки: подсветка строки каретки или фон кода из лексера.

        ed.paper() в этой сборке PyQt6 возвращает дефолтный фон, а не то,
        что задал setPaper — реальный фон живёт в стилях лексера.
        """
        caret = ed.SendScintilla(SCI_GETCURRENTPOS)
        if (ed.SendScintilla(SCI_LINEFROMPOSITION, caret) == line
                and ed.SendScintilla(SCI_GETCARETLINEVISIBLE)):
            c = ed.SendScintilla(SCI_GETCARETLINEBACK)
            if c:
                return QColor(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF)
        lexer = getattr(ed, "current_lexer", None) or ed.lexer()
        if lexer is not None:
            try:
                p = QColor(lexer.paper(0))
                if p.isValid():
                    return p
            except (RuntimeError, TypeError):
                pass
            try:
                p = QColor(lexer.defaultPaper())
                if p.isValid():
                    return p
            except (RuntimeError, TypeError):
                pass
        paper = ed.paper()
        if paper and paper.isValid():
            return QColor(paper)
        return QColor(_BG_DARK if self._dark else _BG_LIGHT)

    @staticmethod
    def _line_has_selection(ed, line) -> bool:
        l1, _, l2, _ = ed.getSelection()
        return l1 <= line <= l2

    def _hint_font(self) -> QFont:
        if self._hint_font_cache is None:
            f = QFont(self._code_font())
            f.setItalic(True)
            self._hint_font_cache = f
        return self._hint_font_cache

    # --- сообщения диагностик (error lens) ---

    def _paint_line(self, p, ed, line, msgs, cw, line_h, shift=0):
        try:
            p.setFont(self._code_font())
            pos = ed.SendScintilla(SCI_POSITIONFROMLINE, line)
            if pos < 0:
                return
            y = ed.SendScintilla(SCI_POINTYFROMPOSITION, 0, pos)
            line_end = ed.SendScintilla(SCI_GETLINEENDPOSITION, line)
            if line_end > pos:
                x = ed.SendScintilla(SCI_POINTXFROMPOSITION, 0, line_end)
            else:
                x = sum(ed.marginWidth(m) for m in range(4))
            x += shift + _GAP_CHARS * cw

            text = self._join(msgs)
            if not text:
                return
            worst = min(sev for sev, _ in msgs)
            color = self._err if worst <= 1 else self._warn

            fm = p.fontMetrics()
            text_width = fm.horizontalAdvance(text)
            max_w = ed.width() - x - 20
            if text_width > max_w and max_w > 50:
                avail = max_w - 30
                if avail > 20:
                    avg = fm.averageCharWidth()
                    if avg > 0:
                        keep = int(avail / avg)
                        if keep > 5:
                            text = text[:keep] + "..."

            # Полупрозрачная подложка-прямоугольник цвета ошибки
            bg = QColor(color)
            bg.setAlpha(_RECT_ALPHA)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(bg)
            p.drawRect(int(x) - _PAD, int(y),
                       int(fm.horizontalAdvance(text)) + 2 * _PAD, line_h)

            # Текст поверх подложки
            c = QColor(color)
            c.setAlpha(_ALPHA)
            p.setPen(QPen(c))
            p.drawText(int(x), int(y) + fm.ascent(), text)
        except RuntimeError:
            pass  # редактор уничтожен

    # --- вспомогательное ---

    def _code_font(self) -> QFont:
        """Шрифт кода из стилей лексера (_apply_font_size)."""
        ed = self.editor
        lexer = getattr(ed, "current_lexer", None) or ed.lexer()
        if lexer is not None:
            try:
                f = QFont(lexer.font(0))
                if f.pointSize() > 0:
                    return f
            except (RuntimeError, TypeError):
                pass
        return QFont(code_font_family(),
                     getattr(ed, "_font_size", CODE_FONT_SIZE))

    @staticmethod
    def _join(msgs) -> str:
        parts = []
        for sev, m in msgs:
            m = " ".join(m.split())
            if not m:
                continue
            parts.append(("Error: " if sev <= 1 else "Warning: ") + m)
        text = "; ".join(parts)
        if len(text) > _MAX_CHARS:
            text = text[:_MAX_CHARS] + "…"
        return text
