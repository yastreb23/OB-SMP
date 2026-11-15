@echo off
rem Builds OB SMP Physics: Bullet (once, into build\bullet.lib), then the plugin, build\OBSMPPhysics.dll.
rem Needs the Visual Studio 2022 (or later) C++ build tools with the x86 compiler. "build.bat clean" rebuilds Bullet too.
setlocal
set ROOT=%~dp0
set OUT=%ROOT%build

rem The x86 build environment: already set up (a developer prompt, or CI), else found with vswhere.
if "%VSCMD_ARG_TGT_ARCH%"=="x86" goto haveenv
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (echo vswhere not found: install the Visual Studio C++ build tools & exit /b 1)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo no Visual Studio with the C++ x86/x64 tools found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
:haveenv

if "%1"=="clean" rmdir /s /q "%OUT%"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"

rem SSE in Bullet's API, as FSMP builds it: btVector3 and friends are 16-byte aligned __m128 values.
rem /GL + /LTCG: whole-program optimization across Bullet, FSMP and the glue.
set COMMON=/nologo /O2 /Oi /GL /arch:SSE2 /fp:fast /EHsc /MT /GS- /DNDEBUG /DWIN32 /D_WINDOWS /DBT_USE_SSE_IN_API /DNOMINMAX /DWIN32_LEAN_AND_MEAN
set BULLET=%ROOT%extern\bullet
if not defined OBSMP_VERSION set OBSMP_VERSION=0.1.0-dev

if not exist "%OUT%\bullet.lib" (
	echo building bullet
	pushd "%OUT%"
	cl %COMMON% /W0 /c /I"%BULLET%" "%BULLET%\btLinearMathAll.cpp" "%BULLET%\btBulletCollisionAll.cpp" "%BULLET%\btBulletDynamicsAll.cpp" || (popd & exit /b 1)
	lib /nologo /OUT:bullet.lib btLinearMathAll.obj btBulletCollisionAll.obj btBulletDynamicsAll.obj || (popd & exit /b 1)
	popd
)

pushd "%OUT%\obj"
echo building the plugin
rem src\ob before src\fsmp: the Oblivion glue keeps FSMP's file and class names (hdtSkyrimSystem.h ...).
cl %COMMON% /std:c++20 /W3 /wd4244 /wd4267 /wd4018 /wd4996 /permissive- /Zc:preprocessor /bigobj /Zi /c ^
	/DOBSMP_VERSION=\"%OBSMP_VERSION%\" ^
	/I"%BULLET%" /I"%ROOT%src\compat" /I"%ROOT%src\ob" /I"%ROOT%src\fsmp" /I"%ROOT%src\fsmp\hdtSkinnedMesh" ^
	/FI"%ROOT%src\compat\PCH.h" ^
	"%ROOT%src\fsmp\hdtSkinnedMesh\*.cpp" "%ROOT%src\fsmp\XmlReader.cpp" "%ROOT%src\ob\*.cpp" || (popd & exit /b 1)

link /nologo /DLL /DEBUG /LTCG /OPT:REF /OPT:ICF /OUT:"%OUT%\OBSMPPhysics.dll" *.obj "%OUT%\bullet.lib" user32.lib || (popd & exit /b 1)
popd
rem The xOBSE entry points must be exported, from a 32-bit DLL.
dumpbin /nologo /headers "%OUT%\OBSMPPhysics.dll" | findstr /c:"machine (x86)" >nul || (echo the DLL is not 32-bit & exit /b 1)
dumpbin /nologo /exports "%OUT%\OBSMPPhysics.dll" | findstr OBSEPlugin_ || (echo the xOBSE entry points are not exported & exit /b 1)
echo build ok
