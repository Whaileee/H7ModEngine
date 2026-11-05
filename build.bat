@echo off
rem Builds bin\dinput8.dll with MSVC (x64). Usage: build.bat [deploy [examples] | nettest [online]]
rem   nettest = also build build\nettest\nethost.exe and run tests\offline\net.lua (h7.json/http/net/crypto outside the
rem   game); "online" adds the HTTP and UPnP checks (needs internet; UPnP needs a router with UPnP enabled).
setlocal
set ROOT=%~dp0
rem Game folder for "deploy"; set H7_GAME_DIR to use another one.
set GAME=N:\Ubisoft\games\Might and Magic Heroes VII
if defined H7_GAME_DIR set GAME=%H7_GAME_DIR%
rem Find any Visual Studio edition with the C++ x64 tools (Community, BuildTools, Enterprise on CI runners).
rem (%%VSWHERE%% expands inside the child command: the ")" of "Program Files (x86)" would end the for clause.)
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
set VSDIR=
for /f "usebackq delims=" %%i in (`call "%%VSWHERE%%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
echo Using Visual Studio at %VSDIR%
if not defined VSDIR (
  echo Visual Studio with the C++ x64 build tools was not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist "%ROOT%build\obj" mkdir "%ROOT%build\obj"
if not exist "%ROOT%bin" mkdir "%ROOT%bin"
cd /d "%ROOT%build\obj"
del /q *.obj 2>nul

rem Lua is compiled as C++ so Lua errors unwind as exceptions through C++ frames.
set LUA=%ROOT%third_party\lua
set LUASRC=lapi lauxlib lbaselib lcode lcorolib lctype ldblib ldebug ldo ldump lfunc lgc linit liolib llex lmathlib lmem loadlib lobject lopcodes loslib lparser lstate lstring lstrlib ltable ltablib ltm lundump lutf8lib lvm lzio
set CSRC=
for %%f in (%LUASRC%) do call set CSRC=%%CSRC%% "%LUA%\%%f.c"
cl /nologo /c /O2 /MT /W0 /TP /EHsc %CSRC% || exit /b 1

set CPPSRC=
for /r "%ROOT%src" %%f in (*.cpp) do call set CPPSRC=%%CPPSRC%% "%%f"
cl /nologo /c /O2 /MT /EHsc /std:c++20 /W3 /Zi /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I"%LUA%" %CPPSRC% || exit /b 1

rem Embed the default ini, ModManager, DebugConsole and OnlineLobby in the DLL (installed at startup by src\engine\bundle.cpp), so a release
rem is just dinput8.dll. Manifest lines: "<resource id> <keep = write if missing | sync = keep identical> <path>".
set BUNDLE=%ROOT%build\bundle
if not exist "%BUNDLE%" mkdir "%BUNDLE%"
set ID=1000
type nul > "%BUNDLE%\manifest.txt"
set RCPATH=%BUNDLE%\manifest.txt
> "%BUNDLE%\bundle.rc" echo 1000 RCDATA "%RCPATH:\=/%"
call :bundle keep "%ROOT%dist\H7ModEngine.ini" "H7ModEngine.ini"
for %%f in ("%ROOT%dist\mods\ModManager\*.lua") do call :bundle sync "%%~ff" "mods\ModManager\%%~nxf"
for %%f in ("%ROOT%dist\mods\DebugConsole\*.lua") do call :bundle sync "%%~ff" "mods\DebugConsole\%%~nxf"
for %%f in ("%ROOT%dist\mods\OnlineLobby\*.lua") do call :bundle sync "%%~ff" "mods\OnlineLobby\%%~nxf"
rc/nologo /fo "%ROOT%build\obj\bundle.res" "%BUNDLE%\bundle.rc" || exit /b 1

link /nologo /DLL /DEBUG /OPT:REF /OUT:"%ROOT%bin\dinput8.dll" /DEF:"%ROOT%src\proxy\dinput8.def" *.obj bundle.res user32.lib shell32.lib winhttp.lib ws2_32.lib ole32.lib oleaut32.lib bcrypt.lib || exit /b 1
echo Built %ROOT%bin\dinput8.dll

set LUAOBJ=
for %%f in (%LUASRC%) do call set LUAOBJ=%%LUAOBJ%% %%f.obj
if /i "%1"=="nettest" (
  if not exist "%ROOT%build\nettest" mkdir "%ROOT%build\nettest"
  cl /nologo /O2 /MT /EHsc /std:c++20 /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I"%LUA%" /Fo"%ROOT%build\nettest\\" /Fe"%ROOT%build\nettest\nethost.exe" "%ROOT%tests\offline\nethost.cpp" "%ROOT%src\lua\netlib.cpp" "%ROOT%src\net\http.cpp" "%ROOT%src\net\upnp.cpp" "%ROOT%src\net\content.cpp" "%ROOT%src\net\crypto.cpp" "%ROOT%src\net\responder.cpp" %LUAOBJ% winhttp.lib ws2_32.lib ole32.lib oleaut32.lib shell32.lib bcrypt.lib || exit /b 1
  if /i "%2"=="online" set H7_NETTEST_ONLINE=1
  "%ROOT%build\nettest\nethost.exe" "%ROOT%tests\offline\net.lua" || exit /b 1
)

if /i "%1"=="deploy" (
  rem Replace whatever dinput8.dll is in the game folder (the old one was a dangling symlink).
  del /f /q "%GAME%\Binaries\Win64\dinput8.dll" 2>nul
  copy /y "%ROOT%bin\dinput8.dll" "%GAME%\Binaries\Win64\dinput8.dll" >nul || exit /b 1
  if not exist "%GAME%\H7ModEngine" mkdir "%GAME%\H7ModEngine"
  if not exist "%GAME%\H7ModEngine\H7ModEngine.ini" copy /y "%ROOT%dist\H7ModEngine.ini" "%GAME%\H7ModEngine\H7ModEngine.ini" >nul
  rem Only the bundled mods (ModManager, DebugConsole, OnlineLobby) are installed; "build.bat deploy examples" also installs the example mods.
  if /i "%2"=="examples" (
    xcopy /e /i /y /q "%ROOT%dist\mods" "%GAME%\H7ModEngine\mods" >nul
  ) else (
    xcopy /e /i /y /q "%ROOT%dist\mods\ModManager" "%GAME%\H7ModEngine\mods\ModManager" >nul
    xcopy /e /i /y /q "%ROOT%dist\mods\DebugConsole" "%GAME%\H7ModEngine\mods\DebugConsole" >nul
    xcopy /e /i /y /q "%ROOT%dist\mods\OnlineLobby" "%GAME%\H7ModEngine\mods\OnlineLobby" >nul
  )
  xcopy /e /i /y /q "%ROOT%dist\lib" "%GAME%\H7ModEngine\lib" >nul
  rem CLAUDE.md files are repository notes, not mod files.
  del /q "%GAME%\H7ModEngine\mods\ModManager\CLAUDE.md" 2>nul
  echo Deployed to %GAME%
)
endlocal
exit /b 0

:bundle
set /a ID+=1
set RCPATH=%~2
>> "%BUNDLE%\bundle.rc" echo %ID% RCDATA "%RCPATH:\=/%"
>> "%BUNDLE%\manifest.txt" echo %ID% %1 %~3
exit /b 0
