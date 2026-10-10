"""
Определение установленных Python и выбранного интерпретатора.

Нужен панели настроек (LSP -> Python: выбор интерпретатора из комбобокса)
и статус-бару (версия выбранного Python). На Windows интерпретаторы
ищутся через launcher `py -0p`, реестр (PythonCore) и текущий
интерпретатор. Точная версия (`python --version`) кэшируется — на каждый
путь процесс запускается не чаще одного раза.
"""

import os
import platform
import re
import subprocess
import sys
from typing import Dict, List, Optional

from editor.lsp.config import _interpreter_path
from editor.settings_store import app_settings

# CREATE_NO_WINDOW: не мелькать консолью при запуске python --version
_NO_WINDOW = 0x08000000 if os.name == "nt" else 0

_VER_CACHE: Dict[str, Optional[str]] = {}


def python_version(interp: str) -> Optional[str]:
    """Точная версия интерпретатора ("3.13.7") или None."""
    interp = os.path.normpath(interp)
    if interp not in _VER_CACHE:
        version = None
        try:
            res = subprocess.run(
                [interp, "--version"],
                capture_output=True, text=True, timeout=15,
                creationflags=_NO_WINDOW,
            )
            text = (res.stdout or res.stderr or "").strip()
            m = re.search(r"Python\s+([0-9]+\.[0-9]+(?:\.[0-9]+)?)", text)
            if m:
                version = m.group(1)
        except (OSError, subprocess.SubprocessError):
            version = None
        _VER_CACHE[interp] = version
    return _VER_CACHE[interp]


def _py_launcher_pythons() -> Dict[str, str]:
    """{path: version} из `py -0p` (launcher Windows)."""
    found: Dict[str, str] = {}
    try:
        res = subprocess.run(["py", "-0p"], capture_output=True, text=True,
                             timeout=15, creationflags=_NO_WINDOW)
        for line in (res.stdout or "").splitlines():
            m = re.search(r"-V:([0-9.]+)\s*\*?\s*(.+\.exe)", line.strip())
            if m:
                path = os.path.normpath(m.group(2).strip())
                if os.path.isfile(path):
                    found[path] = m.group(1)
    except (OSError, subprocess.SubprocessError):
        pass
    return found


def _registry_pythons() -> Dict[str, str]:
    """{path: version} из реестра Windows (PythonCore InstallPath)."""
    found: Dict[str, str] = {}
    if os.name != "nt":
        return found
    try:
        import winreg
    except ImportError:
        return found
    for hive, root in (
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Python\PythonCore"),
        (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Python\PythonCore"),
    ):
        try:
            with winreg.OpenKey(hive, root) as key:
                count, _, _ = winreg.QueryInfoKey(key)
                for i in range(count):
                    ver = winreg.EnumKey(key, i)
                    try:
                        with winreg.OpenKey(key, ver + r"\InstallPath") as ik:
                            path, _ = winreg.QueryValueEx(ik, "")
                            exe = os.path.join(path, "python.exe")
                            if not os.path.isfile(exe):
                                try:
                                    exe, _ = winreg.QueryValueEx(ik,
                                                                 "ExecutablePath")
                                except OSError:
                                    pass
                            if os.path.isfile(exe):
                                found[os.path.normpath(exe)] = ver
                    except OSError:
                        continue
        except OSError:
            continue
    return found


def list_installed_pythons() -> List[Dict[str, str]]:
    """Установленные интерпретаторы: [{path, version}], текущий первым."""
    found = _py_launcher_pythons()
    found.update(_registry_pythons())

    result: List[Dict[str, str]] = []
    seen = set()

    def add(path: str, approx: str):
        path = os.path.normpath(path)
        if path in seen or not os.path.isfile(path):
            return
        seen.add(path)
        result.append({
            "path": path,
            "version": python_version(path) or approx,
        })

    add(_interpreter_path(), platform.python_version())
    for path, approx in found.items():
        if path != os.path.normpath(_interpreter_path()):
            add(path, approx)
    return result


def selected_python() -> Dict[str, str]:
    """{path, version} — интерпретатор из настроек или интерпретатор редактора."""
    path = (app_settings().get_str("lsp.python_path", "") or "").strip()
    if not (path and os.path.isfile(path)):
        path = _interpreter_path()
    path = os.path.normpath(path)
    version = python_version(path)
    if version is None and path == os.path.normpath(_interpreter_path()):
        version = platform.python_version()
    return {"path": path, "version": version or ""}
