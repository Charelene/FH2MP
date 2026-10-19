@echo off
rem The developer launcher from this checkout (Windows): py -m venv .venv ; .venv\Scripts\pip install PySide6-Essentials cryptography keyring
cd /d "%~dp0"
.venv\Scripts\python -m fh2mp_launcher --dev %*
