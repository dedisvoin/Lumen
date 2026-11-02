@echo off
setlocal
set "ROSWELL_INSTALL_DIR=C:\Users\Ivan\scoop\apps\roswell\current"
set "ROSWELL_HOME=%USERPROFILE%\.roswell"
"%ROSWELL_INSTALL_DIR%\ros.exe" -Q -- "%~dp0cl-lsp" %*
