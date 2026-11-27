@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo ::error::Visual Studio C++ tools not found
  exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 (
  echo ::error::vcvarsall failed
  exit /b 1
)
echo ::notice::MSVC environment ready.
pushd "%RUNNER_TEMP%"
cl /nologo /EHsc /std:c++17 /O2 /utf-8 /I"%GITHUB_WORKSPACE%\src" "%GITHUB_WORKSPACE%\tests\appearance\early_theme_access.cpp" "%GITHUB_WORKSPACE%\src\core\CategoryColors.cpp" /Fe:fixed-theme.exe > fixed-compile.log 2>&1
if errorlevel 1 (
  for /f "usebackq delims=" %%L in ("fixed-compile.log") do echo ::error::%%L
  exit /b 1
)
echo ::notice::Current test compiled.
fixed-theme.exe
if errorlevel 1 (
  echo ::error::Current implementation failed the early-initialization test.
  exit /b 1
)
echo ::notice::Current implementation passed the early-initialization test.
popd
exit /b 0
