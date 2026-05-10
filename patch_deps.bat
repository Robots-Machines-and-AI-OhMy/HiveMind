@echo off
:: ============================================================
:: patch_deps.bat
:: Thin wrapper — runs patch_deps.ps1 via PowerShell.
:: Double-click or run from any command prompt.
:: ============================================================

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0patch_deps.ps1"
if %errorlevel% neq 0 pause