"""
SBCL-проверка Common Lisp-файлов (вариант 3).

cl-lsp не шлёт publishDiagnostics, поэтому проверяем файл сами: запускаем
sbcl_check.lisp, который читает top-level формы, eval-ит каждую с
перехватом warning/error и печатает построчно
    <строка>:<колонка>:<severity>:<текст>
Позиция — начало top-level формы (SBCL не даёт точных позиций при
поштучном eval). Результат отдаётся в общий пайплайн диагностик Lumen.
"""

import os
import re
import shutil
import subprocess
from pathlib import Path
from typing import List, Optional, Tuple

from editor.resources import editor_path
from editor.settings_store import app_settings

_CHECK_SCRIPT = editor_path("lsp", "sbcl_check.lisp")

# Поиск sbcl кэшируется, чтобы не гонять PATH при каждой проверке.
# (None — ещё не искали, "" — не найден.)
_sbcl_cache: List = [None]


def find_sbcl() -> str:
    """Путь к sbcl.exe: настройка lsp.sbcl_path, PATH, затем Roswell.

    Roswell кладёт SBCL в ~/.roswell/impls/<arch>/<имя>/<версия>/bin:
    перебираем все вхождения sbcl.exe, берём самое новое (сортировка
    строк даёт стабильный порядок имён).
    """
    if _sbcl_cache[0] is not None:
        return _sbcl_cache[0]
    exe = ""
    path = app_settings().get_str("lsp.sbcl_path", "").strip()
    if path:
        exe = path if os.path.isfile(path) else ""
    if not exe:
        exe = shutil.which("sbcl") or ""
    if not exe:
        home = Path.home()
        impls = home / ".roswell" / "impls"
        if impls.is_dir():
            matches = sorted(p for p in impls.rglob("sbcl.exe") if p.is_file())
            if matches:
                exe = str(matches[-1])
    _sbcl_cache[0] = exe
    return exe


def check_file(path: str, timeout: float = 20.0) -> List[Tuple[int, int, str, str]]:
    """Проверка .lisp-файла через SBCL.

    Возвращает [(line, col, severity, message), ...]; line/col — 1-based.
    Пустой список — ошибок нет или SBCL недоступен/сбой (не роняем UI).
    """
    sbcl = find_sbcl()
    if not sbcl or not _CHECK_SCRIPT.is_file():
        return []
    try:
        proc = subprocess.run(
            [sbcl, "--noinform", "--non-interactive",
             "--load", str(_CHECK_SCRIPT), "--", path],
            capture_output=True, timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError):
        return []
    text = proc.stdout.decode("utf-8", errors="replace")
    items = []
    for line in text.splitlines():
        m = re.match(r"^(\d+):(\d+):(warning|error):(.*)$", line)
        if m:
            items.append((int(m.group(1)), int(m.group(2)),
                          m.group(3), m.group(4)))
    return items


def check_text(text: str, display_path: str = "") -> List[Tuple[int, int, str, str]]:
    """Проверка текста буфера (а не файла на диске) через SBCL.

    Буфер может быть не сохранён или отличаться от файла — пишем его во
    временный файл (с расширением оригинала, чтобы скрипт читал его как
    Lisp) и проверяем его. Пустой список — ошибок нет или SBCL недоступен.
    """
    import tempfile
    suffix = os.path.splitext(display_path)[1] or ".lisp"
    fd, tmp = tempfile.mkstemp(suffix=suffix)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        return check_file(tmp)
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass
