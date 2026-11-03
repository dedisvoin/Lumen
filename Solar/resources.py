"""Пути к данным редактора (темы, иконки, tasks.json, сессии).

Модуль лежит в editor/, поэтому его каталог — каталог данных в любом
окружении: в исходниках (editor/), в standalone-сборке (dist/editor/)
и в onefile (распакованный temp). __file__ нормализуем через resolve():
в скомпилированном приложении путь может быть неабсолютным.
"""

from pathlib import Path

_BASE = Path(__file__).resolve().parent


def solar_path(*parts: str) -> Path:
    """Путь к данным редактора: <Solar>/<parts...>."""
    return _BASE.joinpath(*parts)


# Алиасы для совместимости после переименования
editor_path = solar_path
solar_path = solar_path
solar_path = solar_path
solar_path = solar_path
