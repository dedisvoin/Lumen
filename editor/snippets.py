"""
Сниппеты LumenLang: шаблоны, раскрытие в редакторе, навигация по
плейсхолдерам (Tab/Shift+Tab, $0 — финальная позиция курсора).

Вынесено из CompletionEngine: здесь живёт вся механика сниппетов,
а CompletionEngine занимается только фильтрацией и показом списка.
"""

import re
from typing import List, Optional, Tuple

from PyQt6 import sip

from editor.scintilla_constants import (
    SCI_DELETERANGE,
    SCI_GETCURRENTPOS,
    SCI_GOTOPOS,
    SCI_INDICATORCLEARRANGE,
    SCI_INDICATORFILLRANGE,
    SCI_INDICSETALPHA,
    SCI_INDICSETFORE,
    SCI_INDICSETSTYLE,
    SCI_INDICSETUNDER,
    SCI_INSERTTEXT,
    SCI_LINEFROMPOSITION,
    SCI_POSITIONFROMLINE,
    SCI_SCROLLCARET,
    SCI_SETSEL,
    SCI_SETINDICATORCURRENT,
)

# Сниппеты LumenLang: слово -> шаблон. ${N:default} — плейсхолдер (его
# текст по умолчанию выделяется и заменяется вводом), $0 — финальная
# позиция курсора после обхода всех плейсхолдеров по Tab.
SNIPPETS = {
    "fn": "fn ${1:name}(${2:args}) -> ${3:auto} {\n    ${0}\n}",
    "func": "func ${1:name}(${2:args}) -> ${3:auto} {\n    ${0}\n}",
    "struct": "struct ${1:Name} {\n    ${2:let x: auto?;}\n}",
    "if": "if (${1:cond}) {\n    ${0}\n}",
    "else": "else {\n    ${0}\n}",
    "for": ("for (let ${1:i} = ${2:0}; ${3:i < n}; ${4:i = i + 1}) {\n"
             "    ${0}\n}"),
    "while": "while (${1:cond}) {\n    ${0}\n}",
    "do": "do {\n    ${0}\n} while (${1:cond});",
    "lambda": "lambda(${1:args}) -> ${2:auto} { ${0} }",
    "namespace": "namespace ${1:name} {\n    ${0}\n}",
    "new": "new ${1:value}",
    "#macro": "#macro ${1:name}(${2:args}) {\n    ${0}\n}",
    "#define": "#define ${1:NAME} = ${2:value};",
    "#include": "#include \"${1:path}\";",
}

SNIPPET_KIND_ID = 11  # категория «сниппет» в списке автодополнения

# Индикатор подсветки активных плейсхолдеров (0 — скобки, 1 — поиск)
SNIPPET_INDICATOR = 5
INDIC_ROUNDBOX = 7

# Флаги типа модификации из SCN_MODIFIED
SC_MOD_INSERTTEXT = 1
SC_MOD_DELETETEXT = 2

# ${N:default} или $N внутри шаблона сниппета
_TEMPLATE_RE = re.compile(r"\$\{(\d+)(?::([^}]*))?\}|\$(\d)")

# Максимальная длина превью сниппета в списке автодополнения
_SNIPPET_PREVIEW_MAX = 42

# Символы, из которых состоит слово LumenLang (совпадает с wordCharacters
# лексера) — по ним ищем начало введённого слова при раскрытии шаблона
_WORD_CHARS = set(
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789_#"
)


def snippet_preview(template: str) -> str:
    """Однострочная подпись шаблона для списка: что вставит сниппет.

    Плейсхолдеры подставляются значениями по умолчанию, $0 исчезает,
    переносы строк и лишние пробелы сворачиваются.
    """
    preview = _TEMPLATE_RE.sub(lambda m: m.group(2) or "", template)
    preview = re.sub(r"\s*\n\s*", " ", preview)
    preview = re.sub(r"\s{2,}", " ", preview).strip()
    if len(preview) > _SNIPPET_PREVIEW_MAX:
        preview = preview[:_SNIPPET_PREVIEW_MAX - 1].rstrip() + "…"
    return preview


def _parse_template(template: str) -> Tuple[str, List[Tuple[int, int, int]]]:
    """Разбирает шаблон на текст и список (байт_старт, байт_конец, №)."""
    parts: List[str] = []
    placeholders: List[Tuple[int, int, int]] = []
    last = 0
    for m in _TEMPLATE_RE.finditer(template):
        parts.append(template[last:m.start()])
        idx = int(m.group(1) or m.group(3))
        default = m.group(2) or ""
        pos = len("".join(parts).encode("utf-8"))
        parts.append(default)
        length = len(default.encode("utf-8"))
        placeholders.append((pos, pos + length, idx))
        last = m.end()
    parts.append(template[last:])
    return "".join(parts), placeholders


class SnippetSession:
    """Раскрытый сниппет: плейсхолдеры, индикатор, навигация по Tab.

    Состояние одного сеанса: список плейсхолдеров (байтовые позиции в
    документе), финальная позиция $0 и текущий индекс плейсхолдера.
    Сеанс завершается clear() — по завершении $0, клику/стрелкам за
    пределы плейсхолдеров или смене языка.
    """

    def __init__(self):
        self.placeholders: List[Tuple[int, int, int]] = []
        self.final_pos: Optional[int] = None
        self.current_idx: int = -1
        self._indicator_ranges: List[Tuple[int, int]] = []

    @property
    def active(self) -> bool:
        return bool(self.placeholders)

    # --- настройка индикатора ---

    def setup_indicator(self, editor):
        """Инициализирует стиль индикатора подсветки плейсхолдеров."""
        editor.SendScintilla(SCI_INDICSETSTYLE, SNIPPET_INDICATOR,
                             INDIC_ROUNDBOX)
        editor.SendScintilla(SCI_INDICSETUNDER, SNIPPET_INDICATOR, 1)
        editor.SendScintilla(SCI_INDICSETALPHA, SNIPPET_INDICATOR, 70)

    def set_indicator_color(self, editor, color_int: int):
        editor.SendScintilla(SCI_INDICSETFORE, SNIPPET_INDICATOR, color_int)

    # --- раскрытие ---

    def expand(self, word: str, editor) -> bool:
        """Заменяет введённое слово на шаблон и начинает сеанс.

        Автокомплит уже вставил слово; находим его начало сканированием
        от каретки и заменяем диапазон на текст шаблона с плейсхолдерами.
        True, если слово под курсором совпало и шаблон раскрыт.
        """
        template = SNIPPETS[word]
        pos = editor.SendScintilla(SCI_GETCURRENTPOS)
        line = editor.SendScintilla(SCI_LINEFROMPOSITION, pos)
        line_start = editor.SendScintilla(SCI_POSITIONFROMLINE, line)
        col = pos - line_start
        line_bytes = editor.text(line).encode("utf-8")
        b = col
        while b > 0 and chr(line_bytes[b - 1]) in _WORD_CHARS:
            b -= 1
        # Текст мог измениться с момента выбора — раскрываем только если
        # под курсором действительно выбранное слово
        if line_bytes[b:col].decode("utf-8", "replace") != word:
            return False
        start = line_start + b
        text, placeholders = _parse_template(template)
        final = [p for p in placeholders if p[2] == 0]
        named = [p for p in placeholders if p[2] != 0]
        editor.SendScintilla(SCI_DELETERANGE, start, pos - start)
        editor.SendScintilla(SCI_INSERTTEXT, start, text.encode("utf-8"))
        self.placeholders = [(s + start, e + start, i)
                             for s, e, i in named]
        self.final_pos = (final[0][0] + start) if final else None
        self.current_idx = 0 if self.placeholders else -1
        self._redraw(editor)
        if self.placeholders:
            self._select(editor, self.placeholders[0])
        elif self.final_pos is not None:
            self._move_caret(editor, self.final_pos)
            self.final_pos = None
        return True

    # --- навигация по Tab ---

    def handle_tab(self, editor, backward: bool = False) -> bool:
        """Tab/Shift+Tab: переход к следующему/предыдущему плейсхолдеру.

        True, если клавиша обработана сниппетом (символ таба вставлять
        не нужно). После последнего плейсхолдера — финальная позиция $0.
        """
        if not self.placeholders:
            return False
        n = len(self.placeholders)
        if self.current_idx < 0:
            self.current_idx = 0
        if backward:
            if self.current_idx > 0:
                self.current_idx -= 1
                self._select(editor, self.placeholders[self.current_idx])
            return True  # на первом плейсхолдере Tab всё равно потребляем
        if self.current_idx < n - 1:
            self.current_idx += 1
            self._select(editor, self.placeholders[self.current_idx])
            return True
        # Все плейсхолдеры пройдены — финальная позиция $0
        if self.final_pos is not None:
            self._move_caret(editor, self.final_pos)
            self.clear(editor)
            return True
        self.clear(editor)
        return False

    def clear(self, editor):
        """Завершает сеанс: снимает подсветку и сбрасывает позиции."""
        if (editor is not None and not sip.isdeleted(editor)
                and self._indicator_ranges):
            editor.SendScintilla(SCI_SETINDICATORCURRENT, SNIPPET_INDICATOR)
            for start, end in self._indicator_ranges:
                editor.SendScintilla(SCI_INDICATORCLEARRANGE, start,
                                     end - start)
        self.placeholders = []
        self._indicator_ranges = []
        self.final_pos = None
        self.current_idx = -1

    # --- сдвиг позиций при правках ---

    def on_modified(self, editor, pos: int, mod_type: int, text, length: int):
        """Правка текста: сдвигает позиции плейсхолдеров после точки вставки.

        Scintilla даёт позицию и размер изменения; плейсхолдеры справа от
        неё сдвигаются на дельту, чтобы Tab продолжал попадать в них после
        ввода в текущем плейсхолдере.
        """
        if not self.placeholders:
            return
        if not (mod_type & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)):
            return
        if mod_type & SC_MOD_INSERTTEXT:
            raw = text.encode("utf-8") if isinstance(text, str) else text
            delta = len(raw)
        else:
            delta = -length
        new: List[Tuple[int, int, int]] = []
        for start, end, idx in self.placeholders:
            if start >= pos:
                start = max(pos, start + delta)
                end = max(start, end + delta)
            elif end > pos:
                end = max(pos, end + delta)
            # Нулевые (в момент удаления текста плейсхолдера) оставляем:
            # вставка в ту же позицию восстановит их, а индекс в списке
            # не должен «плыть» — иначе Tab перескочит плейсхолдер
            new.append((start, end, idx))
        self.placeholders = new
        self.current_idx = min(self.current_idx, len(new) - 1)
        if self.final_pos is not None and self.final_pos >= pos:
            self.final_pos = max(pos, self.final_pos + delta)
        self._redraw(editor)
        if not self.placeholders:
            self.final_pos = None

    def on_cursor_moved(self, editor, pos: int) -> bool:
        """Курсор ушёл за пределы плейсхолдеров — сеанс завершён."""
        if not self.placeholders:
            return False
        for start, end, _ in self.placeholders:
            if start <= pos <= end:
                return False
        self.clear(editor)
        return True

    # --- внутреннее ---

    def _redraw(self, editor):
        editor.SendScintilla(SCI_SETINDICATORCURRENT, SNIPPET_INDICATOR)
        for start, end in self._indicator_ranges:
            editor.SendScintilla(SCI_INDICATORCLEARRANGE, start, end - start)
        self._indicator_ranges = [
            (s, e) for s, e, _ in self.placeholders if e > s
        ]
        for start, end in self._indicator_ranges:
            editor.SendScintilla(SCI_INDICATORFILLRANGE, start, end - start)

    def _select(self, editor, placeholder: Tuple[int, int, int]):
        start, end = placeholder[0], placeholder[1]
        if end > start:
            editor.SendScintilla(SCI_SETSEL, start, end)
            editor.SendScintilla(SCI_SCROLLCARET)
        else:
            self._move_caret(editor, start)

    def _move_caret(self, editor, pos: int):
        editor.SendScintilla(SCI_GOTOPOS, pos)
        editor.SendScintilla(SCI_SCROLLCARET)
