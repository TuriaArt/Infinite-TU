@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 exit /b 1
pushd "%RUNNER_TEMP%"
if "%~1"=="" goto fixed
rem Optional diagnostic: the previous implementation must fail before main.
git -C "%GITHUB_WORKSPACE%" show %1:src/core/CategoryColors.cpp > old-CategoryColors.cpp
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /O2 /utf-8 /I"%GITHUB_WORKSPACE%\src" /I"%GITHUB_WORKSPACE%\src\core" "%GITHUB_WORKSPACE%\tests\appearance\early_theme_access.cpp" old-CategoryColors.cpp /Fe:old-theme.exe
if errorlevel 1 exit /b 1
old-theme.exe
if not errorlevel 1 (
  echo ::error::Previous implementation did not reproduce the startup failure
  exit /b 1
)
echo ::notice::Previous implementation failed the early-initialization test, as expected.
:fixed
cl /nologo /EHsc /std:c++17 /O2 /utf-8 /I"%GITHUB_WORKSPACE%\src" "%GITHUB_WORKSPACE%\tests\appearance\early_theme_access.cpp" "%GITHUB_WORKSPACE%\src\core\CategoryColors.cpp" /Fe:fixed-theme.exe
if errorlevel 1 exit /b 1
fixed-theme.exe
if errorlevel 1 exit /b 1
echo ::notice::Current implementation passed the early-initialization test.
popd
exit /b 0
