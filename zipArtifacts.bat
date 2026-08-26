@echo off
@setlocal EnableExtensions EnableDelayedExpansion
set "SAKURA_UTF16_PRODUCTION_PACKAGE=true"
if not defined SAKURA_UTF16_BACKEND set "SAKURA_UTF16_BACKEND=cpp"
if not "%SAKURA_UTF16_BACKEND%" == "cpp" (
	echo Production packaging requires SAKURA_UTF16_BACKEND=cpp; got %SAKURA_UTF16_BACKEND%.
	exit /b 1
)
if not defined SAKURA_OUTPUT_BACKEND set "SAKURA_OUTPUT_BACKEND=cpp"
if not "%SAKURA_OUTPUT_BACKEND%" == "cpp" (
	echo Production packaging requires SAKURA_OUTPUT_BACKEND=cpp; got %SAKURA_OUTPUT_BACKEND%.
	exit /b 1
)
set platform=%1
set configuration=%2

if "%platform%" == "x64" (
	@rem OK
) else (
	call :showhelp %0
	exit /b 1
)

if "%configuration%" == "Release" (
	@rem OK
) else if "%configuration%" == "Debug" (
	@rem OK
) else (
	call :showhelp %0
	exit /b 1
)

set ALPHA=1

set ZIP_CMD=%~dp0tools\zip\zip.bat
set LIST_ZIP_CMD=%~dp0tools\zip\listzip.bat
set CTAGS_ZIP=%~dp0installer\externals\universal-ctags\ctags-v6.1.0-x64.zip

@rem for GIT_TAG_NAME
call %~dp0tools\githash.bat %~dp0sakura_core

@rem ----------------------------------------------------------------
@rem prepare environment variable
@rem ----------------------------------------------------------------
@echo checking CI_REPO_NAME %CI_REPO_NAME%
set BUILD_ACCOUNT=
if "%CI_REPO_NAME%" == "tsuyoshi-otake/sakura-editor-next" (
	set BUILD_ACCOUNT=
) else if "%CI_REPO_NAME%" == "" (
	set BUILD_ACCOUNT=
) else (
	set BUILD_ACCOUNT=%CI_ACCOUNT_NAME%
)

@echo checking CI_BUILD_NUMBER %CI_BUILD_NUMBER%
if not "%CI_BUILD_NUMBER%" == "" (
	set BUILD_NUMBER=build%CI_BUILD_NUMBER%
) else (
	set BUILD_NUMBER=buildLocal
)

@echo checking GIT_TAG_NAME %GIT_TAG_NAME%
if not "%GIT_TAG_NAME%" == "" (
	@rem replace '/' with '_'
	set TEMP_NAME1=!GIT_TAG_NAME:/=_!
	@echo TEMP_NAME1 = !TEMP_NAME1!
	
	@rem replace ' ' with '_'
	set TEMP_NAME2=!TEMP_NAME1: =_!
	@echo TEMP_NAME2 = !TEMP_NAME2!

	@rem replace ' ' with '_'
	set TAG_NAME=tag-!TEMP_NAME2!
	@echo TAG_NAME = !TEMP_NAME2!
)

@echo checking GITHUB_PR_NUMBER %GITHUB_PR_NUMBER%
if not "%GITHUB_PR_NUMBER%" == "" (
	set PR_NAME=PR%GITHUB_PR_NUMBER%
)

@echo hash name
set SHORTHASH=%GIT_SHORT_COMMIT_HASH%

if "%ALPHA%" == "1" (
	set RELEASE_PHASE=alpha
) else (
	set RELEASE_PHASE=
)

@rem ----------------------------------------------------------------
@rem build BASENAME
@rem ----------------------------------------------------------------
set BASENAME=sakura

@echo adding BUILD_ACCOUNT
if not "%BUILD_ACCOUNT%" == "" (
	set BASENAME=%BASENAME%-%BUILD_ACCOUNT%
)
@echo BASENAME = %BASENAME%

@echo adding TAG_NAME
if not "%TAG_NAME%" == "" (
	set BASENAME=%BASENAME%-%TAG_NAME%
)
@echo BASENAME = %BASENAME%

@echo adding PR_NAME
if not "%PR_NAME%" == "" (
	set BASENAME=%BASENAME%-%PR_NAME%
)
@echo BASENAME = %BASENAME%

@echo adding BUILD_NUMBER
if not "%BUILD_NUMBER%" == "" (
	set BASENAME=%BASENAME%-%BUILD_NUMBER%
)
@echo BASENAME = %BASENAME%

@echo adding SHORTHASH
if not "%SHORTHASH%" == "" (
	set BASENAME=%BASENAME%-%SHORTHASH%
)
@echo BASENAME = %BASENAME%

@echo adding platform and configuration
set BASENAME=%BASENAME%-%platform%-%configuration%
@echo BASENAME = %BASENAME%

@echo adding RELEASE_PHASE
if not "%RELEASE_PHASE%" == "" (
	set BASENAME=%BASENAME%-%RELEASE_PHASE%
)
@echo BASENAME = %BASENAME%

@rem ---------------------- BASENAME ---------------------------------
@rem "sakura"
@rem TAG_NAME     : (option) tag Name
@rem PR_NAME      : (option) PRxxx (xxx is a PR number)
@rem BUILD_NUMBER : (option) buildYYY or "buildLocal" (YYY is build number)
@rem SHORTHASH    : (option) hash or "buildLocal" (hash is leading 8 charactors)
@rem platform     : Platform ("x64")
@rem configuration: Configuration ("Debug" or "Release")
@rem RELEASE_PHASE: (option) "alpha" (x64 build only)
@rem ----------------------------------------------------------------

@rem ----------------------------------------------------------------
@rem build WORKDIR
@rem ----------------------------------------------------------------
set WORKDIR=%BASENAME%

set RELDIR_LOG=Log
set RELDIR_EXE=EXE
set RELDIR_DEV=DEV
set RELDIR_INST=Installer
set RELDIR_ASM=Asm

set WORKDIR_LOG=%WORKDIR%\%RELDIR_LOG%
set WORKDIR_EXE=%WORKDIR%\%RELDIR_EXE%
set WORKDIR_DEV=%WORKDIR%\%RELDIR_DEV%
set WORKDIR_INST=%WORKDIR%\%RELDIR_INST%
set WORKDIR_ASM=%WORKDIR%\%RELDIR_ASM%

set OUTFILE=%~dp0%BASENAME%-All.zip
set OUTFILE_LOG=%~dp0%BASENAME%-Log.zip
set OUTFILE_ASM=%~dp0%BASENAME%-Asm.zip
set OUTFILE_INST=%~dp0%BASENAME%-Installer.zip
set OUTFILE_EXE=%~dp0%BASENAME%-Exe.zip
set OUTFILE_DEV=%~dp0%BASENAME%-Dev.zip

@rem cleanup for local testing
if exist "%OUTFILE%" (
	del %OUTFILE%
)
if exist "%OUTFILE_LOG%" (
	del %OUTFILE_LOG%
)
if exist "%OUTFILE_ASM%" (
	del %OUTFILE_ASM%
)
if exist "%OUTFILE_INST%" (
	del %OUTFILE_INST%
)
if exist "%OUTFILE_EXE%" (
	del %OUTFILE_EXE%
)
if exist "%OUTFILE_DEV%" (
	del %OUTFILE_DEV%
)
if exist "%WORKDIR%" (
	rmdir /s /q "%WORKDIR%"
)
if exist "%WORKDIR_ASM%" (
	rmdir /s /q "%WORKDIR_ASM%"
)

mkdir %WORKDIR%
mkdir %WORKDIR_LOG%
mkdir %WORKDIR_EXE%
mkdir %WORKDIR_EXE%\license\
mkdir %WORKDIR_EXE%\license\bregonig\
mkdir %WORKDIR_EXE%\license\ctags\
mkdir %WORKDIR_EXE%\license\windows-terminal\
mkdir %WORKDIR_EXE%\license\codicons\
mkdir %WORKDIR_EXE%\license\seti\
mkdir %WORKDIR_EXE%\license\fmt\
mkdir %WORKDIR_EXE%\license\ms-gsl\
mkdir %WORKDIR_EXE%\license\wil\
mkdir %WORKDIR_DEV%
mkdir %WORKDIR_INST%
call :copyRequired "%platform%\%configuration%\sakura.exe" "%WORKDIR_EXE%\" "sakura executable"
if errorlevel 1 exit /b 1
call :copyRequired "%platform%\%configuration%\sakura-senp-tool.exe" "%WORKDIR_EXE%\" "SENP management executable"
if errorlevel 1 exit /b 1
call :copyRequired "%platform%\%configuration%\sakura-senp-host.exe" "%WORKDIR_EXE%\" "SENP runtime host"
if errorlevel 1 exit /b 1
call :copyRequired "%platform%\%configuration%\*.dll" "%WORKDIR_EXE%\" "runtime DLLs"
if errorlevel 1 exit /b 1

call :copyRequired "%platform%\%configuration%\*.pdb" "%WORKDIR_DEV%\" "debug symbols"
if errorlevel 1 exit /b 1

: LICENSE
call :copyRequired ".\LICENSE" "%WORKDIR_EXE%\license\" "Sakura license"
if errorlevel 1 exit /b 1

: bregonig
if not exist "%platform%\%configuration%\bregonig.dll" (
	echo Error: %platform%\%configuration%\bregonig.dll was not staged by the product build.
	exit /b 1
)
if not exist "%platform%\%configuration%\migemo.dll" (
	echo Error: %platform%\%configuration%\migemo.dll was not staged by the product build.
	exit /b 1
)
set BREGONIG_LICENSE_DIR=%~dp0third_party\owned\bregonig-next
call :copyRequired "%BREGONIG_LICENSE_DIR%\bsd_license.txt" "%WORKDIR_EXE%\license\bregonig\" "bregonig bsd license"
if errorlevel 1 exit /b 1
call :copyRequired "%BREGONIG_LICENSE_DIR%\perl_license.txt" "%WORKDIR_EXE%\license\bregonig\" "bregonig perl license"
if errorlevel 1 exit /b 1
call :copyRequired "%BREGONIG_LICENSE_DIR%\perl_license_jp.txt" "%WORKDIR_EXE%\license\bregonig\" "bregonig perl license ja"
if errorlevel 1 exit /b 1

: ctags.exe
set INSTALLER_RESOURCES_CTAGS=%~dp0installer\temp\ctags
call :copyRequired "%platform%\%configuration%\ctags.exe" "%WORKDIR_EXE%\" "ctags executable"
if errorlevel 1 exit /b 1
call :copyRequired "%INSTALLER_RESOURCES_CTAGS%\license\*.*" "%WORKDIR_EXE%\license\ctags\" "ctags licenses"
if errorlevel 1 exit /b 1
if not defined CMD_7Z call %~dp0tools\find-tools.bat > NUL
if not defined CMD_7Z (
	echo Error: 7z.exe was not found; it is required to stage ctags documentation.
	exit /b 1
)
"%CMD_7Z%" x "%CTAGS_ZIP%" -o"%INSTALLER_RESOURCES_CTAGS%" -y docs > NUL || (echo Error: unable to extract ctags documentation. & exit /b 1)
if not exist "%INSTALLER_RESOURCES_CTAGS%\docs\index.html" (
	echo Error: ctags documentation was not extracted.
	exit /b 1
)
xcopy /E /I /Y "%INSTALLER_RESOURCES_CTAGS%\docs" "%WORKDIR_EXE%\license\ctags\docs" > NUL || (echo Error: unable to copy ctags documentation. & exit /b 1)

: Windows Terminal parser / Unicode / input provenance
set WINDOWS_TERMINAL_VENDOR=%~dp0sakura_core\terminal\vendor\windows_terminal
if not exist "%WINDOWS_TERMINAL_VENDOR%\LICENSE" (
	echo Windows Terminal license payload was not found.
	exit /b 1
)
if not exist "%WINDOWS_TERMINAL_VENDOR%\UPSTREAM.md" (
	echo Windows Terminal provenance payload was not found.
	exit /b 1
)
if not exist "%WINDOWS_TERMINAL_VENDOR%\IMPORTED_FILES.md" (
	echo Windows Terminal imported-files payload was not found.
	exit /b 1
)
call :copyRequired "%WINDOWS_TERMINAL_VENDOR%\LICENSE" "%WORKDIR_EXE%\license\windows-terminal\" "Windows Terminal license"
if errorlevel 1 exit /b 1
call :copyRequired "%WINDOWS_TERMINAL_VENDOR%\UPSTREAM.md" "%WORKDIR_EXE%\license\windows-terminal\" "Windows Terminal provenance"
if errorlevel 1 exit /b 1
call :copyRequired "%WINDOWS_TERMINAL_VENDOR%\IMPORTED_FILES.md" "%WORKDIR_EXE%\license\windows-terminal\" "Windows Terminal imported-files list"
if errorlevel 1 exit /b 1

: Codicons Activity Bar geometry attribution
set CODICONS_VENDOR=%~dp0sakura_core\workbench\icons
if not exist "%CODICONS_VENDOR%\CODICONS-ATTRIBUTION.md" (
	echo Codicons attribution payload was not found.
	exit /b 1
)
call :copyRequired "%CODICONS_VENDOR%\CODICONS-ATTRIBUTION.md" "%WORKDIR_EXE%\license\codicons\" "Codicons attribution"
if errorlevel 1 exit /b 1

: Seti file icon theme, bundled as the default Explorer icon theme
set SETI_VENDOR=%~dp0sakura_core\workbench\icons
if not exist "%SETI_VENDOR%\SETI-ATTRIBUTION.md" (
	echo Seti attribution payload was not found.
	exit /b 1
)
if not exist "%SETI_VENDOR%\SETI-LICENSE" (
	echo Seti license payload was not found.
	exit /b 1
)
call :copyRequired "%SETI_VENDOR%\SETI-ATTRIBUTION.md" "%WORKDIR_EXE%\license\seti\" "Seti attribution"
if errorlevel 1 exit /b 1
call :copyRequired "%SETI_VENDOR%\SETI-LICENSE" "%WORKDIR_EXE%\license\seti\" "Seti license"
if errorlevel 1 exit /b 1

: Windows Terminal compatibility dependencies
set WINDOWS_TERMINAL_LICENSES=%~dp0sakura_core\terminal\vendor\licenses
if not exist "%WINDOWS_TERMINAL_LICENSES%\fmt\LICENSE" (
	echo fmt license payload was not found.
	exit /b 1
)
if not exist "%WINDOWS_TERMINAL_LICENSES%\ms-gsl\LICENSE" (
	echo Microsoft GSL license payload was not found.
	exit /b 1
)
if not exist "%WINDOWS_TERMINAL_LICENSES%\wil\LICENSE" (
	echo WIL license payload was not found.
	exit /b 1
)
call :copyRequired "%WINDOWS_TERMINAL_LICENSES%\fmt\LICENSE" "%WORKDIR_EXE%\license\fmt\" "fmt license"
if errorlevel 1 exit /b 1
call :copyRequired "%WINDOWS_TERMINAL_LICENSES%\ms-gsl\LICENSE" "%WORKDIR_EXE%\license\ms-gsl\" "Microsoft GSL license"
if errorlevel 1 exit /b 1
call :copyRequired "%WINDOWS_TERMINAL_LICENSES%\wil\LICENSE" "%WORKDIR_EXE%\license\wil\" "WIL license"
if errorlevel 1 exit /b 1

call :copyRequired "help\macro\macro.chm" "%WORKDIR_EXE%\" "macro help"
if errorlevel 1 exit /b 1
call :copyRequired "help\plugin\plugin.chm" "%WORKDIR_EXE%\" "plugin help"
if errorlevel 1 exit /b 1
call :copyRequired "help\sakura\sakura.chm" "%WORKDIR_EXE%\" "Sakura help"
if errorlevel 1 exit /b 1
if exist "html\sakura-doxygen.chm" (
	call :copyRequired "html\sakura-doxygen.chm" "%WORKDIR_DEV%\" "Doxygen help"
	if errorlevel 1 exit /b 1
)
if exist "html\sakura-doxygen.chi" (
	call :copyRequired "html\sakura-doxygen.chi" "%WORKDIR_DEV%\" "Doxygen index"
	if errorlevel 1 exit /b 1
)

call :copyRequired "installer\Output-%platform%\*.exe" "%WORKDIR_INST%\" "installer executable"
if errorlevel 1 exit /b 1
call :copyRequired "build\logs\msbuild-%platform%-%configuration%.log" "%WORKDIR_LOG%\" "build log"
if errorlevel 1 exit /b 1
if exist "build\logs\msbuild-%platform%-%configuration%.log.csv" (
	call :copyRequired "build\logs\msbuild-%platform%-%configuration%.log.csv" "%WORKDIR_LOG%\" "build log CSV"
	if errorlevel 1 exit /b 1
)
if exist "build\logs\msbuild-%platform%-%configuration%.log.xlsx" (
	call :copyRequired "build\logs\msbuild-%platform%-%configuration%.log.xlsx" "%WORKDIR_LOG%\" "build log spreadsheet"
	if errorlevel 1 exit /b 1
)
set ISS_LOG_FILE=iss-%platform%-%configuration%.log
if exist "%ISS_LOG_FILE%" (
	call :copyRequired "%ISS_LOG_FILE%" "%WORKDIR_LOG%\" "installer log"
	if errorlevel 1 exit /b 1
)

call :copyRequired "sakura_core\githash.h" "%WORKDIR_LOG%\" "Git hash header"
if errorlevel 1 exit /b 1
if exist "cppcheck-%platform%-%configuration%.xml" (
	call :copyRequired "cppcheck-%platform%-%configuration%.xml" "%WORKDIR_LOG%\" "cppcheck XML"
	if errorlevel 1 exit /b 1
)
if exist "cppcheck-%platform%-%configuration%.log" (
	call :copyRequired "cppcheck-%platform%-%configuration%.log" "%WORKDIR_LOG%\" "cppcheck log"
	if errorlevel 1 exit /b 1
)
if exist "doxygen-%platform%-%configuration%.log" (
	call :copyRequired "doxygen-%platform%-%configuration%.log" "%WORKDIR_LOG%\" "Doxygen log"
	if errorlevel 1 exit /b 1
)

call :copyRequired "installer\warning.txt" "%WORKDIR%\" "release warning"
if errorlevel 1 exit /b 1
if "%ALPHA%" == "1" (
	call :copyRequired "installer\warning-alpha.txt" "%WORKDIR%\" "alpha release warning"
	if errorlevel 1 exit /b 1
)

call :archiveRequired "%WORKDIR_LOG%" "%OUTFILE_LOG%" "log"
if errorlevel 1 exit /b 1
call :archiveRequired "%WORKDIR%" "%OUTFILE%" "complete"
if errorlevel 1 exit /b 1

@rem copy text files for warning after zipping %OUTFILE% because %WORKDIR% is the parent directory of %WORKDIR_EXE% and %WORKDIR_INST%.
if "%ALPHA%" == "1" (
	call :copyRequired "installer\warning-alpha.txt" "%WORKDIR_EXE%\" "executable alpha warning"
	if errorlevel 1 exit /b 1
	call :copyRequired "installer\warning-alpha.txt" "%WORKDIR_INST%\" "installer alpha warning"
	if errorlevel 1 exit /b 1
)
call :copyRequired "installer\warning.txt" "%WORKDIR_EXE%\" "executable warning"
if errorlevel 1 exit /b 1
call :copyRequired "installer\warning.txt" "%WORKDIR_INST%\" "installer warning"
if errorlevel 1 exit /b 1

call :archiveRequired "%WORKDIR_INST%" "%OUTFILE_INST%" "installer"
if errorlevel 1 exit /b 1
call :archiveRequired "%WORKDIR_EXE%" "%OUTFILE_EXE%" "executable"
if errorlevel 1 exit /b 1
call :archiveRequired "%WORKDIR_DEV%" "%OUTFILE_DEV%" "development"
if errorlevel 1 exit /b 1

if not exist "build\logs" mkdir build\logs
if not defined CMD_7Z call %~dp0tools\find-tools.bat > NUL
if not defined CMD_7Z (
	echo Error: 7z.exe was not found; it is required to prove installer SHA-256 identity.
	exit /b 1
)
py -3 "%~dp0tools\verify_runtime_artifact_identity.py" --staged "%platform%\%configuration%" --zip "%OUTFILE_EXE%" --installer-zip "%OUTFILE_INST%" --seven-zip "%CMD_7Z%" --clean-extract "installer\temp\runtime-identity-%platform%-%configuration%-zip" --report "build\logs\runtime-artifact-identity-%platform%-%configuration%-zip.json"
if errorlevel 1 (
	echo Error: staged runtime files do not match the installer or executable ZIP payload.
	exit /b 1
)

@rem SAKURA_GENERATE_ASSEMBLY_LISTINGS=0 (or unset) means MSBuild produced no
@rem .asm files. copyRequired would abort the whole packaging run on missing
@rem files, so treat the Asm archive as optional instead of required.
set ZIP_ASM=0
if /I "%SAKURA_GENERATE_ASSEMBLY_LISTINGS%" == "1" set ZIP_ASM=1
if /I "%SAKURA_GENERATE_ASSEMBLY_LISTINGS%" == "true" set ZIP_ASM=1

if "%ZIP_ASM%" == "1" (
	@echo start zip asm
	mkdir %WORKDIR_ASM%
	call :copyRequired "build\%platform%\%configuration%\sakura_core\*.asm" "%WORKDIR_ASM%\" "assembly listings"
	if errorlevel 1 exit /b 1
	call :archiveRequired "%WORKDIR_ASM%" "%OUTFILE_ASM%" "assembly"
	if errorlevel 1 exit /b 1

	@echo end   zip asm
) else (
	@echo skip zip asm because SAKURA_GENERATE_ASSEMBLY_LISTINGS is not "1" or "true"
)

if exist "%WORKDIR%" (
	rmdir /s /q "%WORKDIR%"
)
if exist "%WORKDIR_ASM%" (
	rmdir /s /q "%WORKDIR_ASM%"
)


exit /b 0

:copyRequired
xcopy /Y /I /Q /H "%~1" "%~f2\" > NUL
if errorlevel 1 (
	echo Error: unable to copy %~3.
	exit /b 1
)
if not exist "%~f2\%~nx1" (
	echo Error: copied %~3 was not found in the destination.
	exit /b 1
)
goto :eof

:archiveRequired
pushd "%~1" || (
	echo Error: unable to enter the %~3 artifact directory.
	exit /b 1
)
call "%ZIP_CMD%" "%~2" .
set ARCHIVE_ERROR=%ERRORLEVEL%
popd
if not "%ARCHIVE_ERROR%" == "0" (
	echo Error: unable to create the %~3 archive.
	exit /b 1
)
if not exist "%~2" (
	echo Error: the %~3 archive was not created.
	exit /b 1
)
goto :eof

@rem ------------------------------------------------------------------------------
@rem show help
@rem see http://orangeclover.hatenablog.com/entry/20101004/1286120668
@rem ------------------------------------------------------------------------------
:showhelp
@echo off
@echo usage
@echo    %~nx1 platform configuration
@echo.
@echo parameter
@echo    platform      : x64
@echo    configuration : Release or Debug
@echo.
@echo example
@echo    %~nx1 x64   Release
@echo    %~nx1 x64   Debug
exit /b 0
