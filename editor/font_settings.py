"""
Шрифты из настроек: семейства интерфейсного (UI) и кодового шрифта.

Пустая строка в settings.json = константа по умолчанию (editor/constants.py).
Списки семейств для комбобоксов панели настроек строятся через QFontDatabase.
"""

from PyQt6.QtGui import QFont, QFontDatabase

from editor.constants import CODE_FONT_FAMILY, FONT_FAMILY
from editor.settings_store import app_settings


def ui_font_family() -> str:
    """Семейство интерфейсного шрифта (пусто = дефолт)."""
    fam = app_settings().get_str("general.ui_font_family", "").strip()
    return fam or FONT_FAMILY


def code_font_family() -> str:
    """Семейство шрифта кода/терминала (пусто = дефолт)."""
    fam = app_settings().get_str("editor.code_font_family", "").strip()
    return fam or CODE_FONT_FAMILY


def ui_font_families() -> list:
    """Все установленные семейства для интерфейсного шрифта.

    Пустой список брать нельзя: комбобокс шрифта останется без единого
    варианта, и сменить шрифт интерфейса будет нечем (как это и было
    с кодовым шрифтом до исправления QFontDatabase ниже). Поэтому при
    пустом families() отдаём хотя бы шрифт по умолчанию.
    """
    try:
        families = QFontDatabase.families()
    except Exception:
        return [FONT_FAMILY]
    skip = ("", "Symbol", "Webdings", "Wingdings", "Marlett", "MT Extra",
            "MS Outlook", "Segoe MDL2 Assets")
    out = [f for f in families if f and f not in skip]
    return out or [FONT_FAMILY]


def mono_font_families() -> list:
    """Моноширинные семейства для шрифта кода.

    QFontDatabase в PyQt6 — класс только со статическими методами,
    создать его нельзя (TypeError). Раньше здесь был вызов
    QFontDatabase(), из-за чего весь список падал в except и
    комбобокс «Code font» содержал ровно один вариант — сменить
    шрифт кода было нечем.
    """
    try:
        families = [f for f in QFontDatabase.families() if f]
    except Exception:
        return [CODE_FONT_FAMILY]
    out = []
    for f in families:
        try:
            fixed = QFontDatabase.isFixedPitch(f)
        except Exception:
            fixed = False
        if fixed or QFont(f).fixedPitch():
            out.append(f)
    return out or [CODE_FONT_FAMILY]
