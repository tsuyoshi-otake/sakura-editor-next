# sakura.cmake - sub-targets for sakura editor project
#

# exeの出力先フォルダーを決める
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  if(CMAKE_GENERATOR MATCHES "^Visual Studio")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${CMAKE_GENERATOR_PLATFORM}/$<CONFIG>")
  else()
    if(NOT DEFINED BUILD_PLATFORM)
      if(DEFINED ENV{BUILD_PLATFORM})
        set(BUILD_PLATFORM "$ENV{BUILD_PLATFORM}")
      elseif(DEFINED ENV{MSYSTEM})
        set(BUILD_PLATFORM "$ENV{MSYSTEM}")
      else()
        set(BUILD_PLATFORM "out")
      endif()
    endif()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${BUILD_PLATFORM}/${CMAKE_BUILD_TYPE}")
  endif()
endif()

set(OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

option(
  SAKURA_GENERATE_ASSEMBLY_LISTINGS
  "Generate MSVC source and assembly listing files"
  OFF
)

set(SAKURA_NESTED_BUILD_TOOL_ARGS)
if(CMAKE_GENERATOR MATCHES "^Visual Studio")
  # Generated child projects own their CMake dependency graphs. FileTracker's
  # injection can otherwise leave cl/link suspended before they start.
  list(APPEND SAKURA_NESTED_BUILD_TOOL_ARGS
    --
    /nologo
    /nr:false
    /p:TrackFileAccess=false
  )
endif()

# ビルド対象のCPUアーキテクチャを決める
if(CMAKE_GENERATOR MATCHES "^Visual Studio")
  # VSジェネレーターは -A の値で判定する
  if(CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
    set(ARCH "x64")
  elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
    set(ARCH "arm64")
  elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
    set(ARCH "x86")
  endif()

  # CMakeジェネレーターに渡すパラメーターを作る
  set(GENERATOR_ARGS "-A ${CMAKE_GENERATOR_PLATFORM} -DCMAKE_CONFIGURATION_TYPES=\"Debug\;Release\"")

else()
  # CMakeが持ってる値を整形する
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    set(ARCH "x64")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    set(ARCH "arm64")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|i686")
    set(ARCH "x86")
  endif()

  # CMakeジェネレーターに渡すパラメータを作る
  set(GENERATOR_ARGS "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
endif()

# マニフェスト用にCPUアーキテクチャを編集する
if(ARCH STREQUAL "x64")
  set(EXE_ARCH "amd64")
else()
  set(EXE_ARCH "${ARCH}")
endif()

# ホストツールのプラットフォームとCPUを決める
if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
  set(HOST_PLATFORM "x64")
  set(HOST_ARCH "x64")
elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
  set(HOST_PLATFORM "ARM64")
  set(HOST_ARCH "arm64")
else()
  set(HOST_PLATFORM "Win32")
  set(HOST_ARCH "x86")
endif()

# ホストツールのCMakeジェネレーターに渡すパラメーターを作る
if(CMAKE_GENERATOR MATCHES "^Visual Studio")
  set(GENERATOR_ARGS_FOR_HOST_TOOLS "-A ${HOST_PLATFORM} -DCMAKE_CONFIGURATION_TYPES=\"Debug\;Release\"")
else()
  set(GENERATOR_ARGS_FOR_HOST_TOOLS "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
endif()

# vswhereを探す。（必須）
# Chocolatey版vswhere(ver3.1.7+)を最優先とする
# Visual Studio付属のvswhereは %ProgramFiles(x86)% に入っている
# %ProgramFiles% は Windows 10 32bit版向けなので削除可
find_program(CMD_VSWHERE vswhere.exe
  PATHS
    "$ENV{ChocolateyInstall}/bin"
    "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer"
    "$ENV{ProgramFiles}/Microsoft Visual Studio/Installer"
  DOC "Visual Studio Locator"
)

if(CMD_VSWHERE)
  message(STATUS "Found vswhere: ${CMD_VSWHERE}")
else()
  message(FATAL_ERROR "vswhere not found")
endif()

# 環境変数とvswhereを使ってVSバージョンを取得する
if($ENV{VisualStudioVersion})
  set(VISUAL_STUDIO_VERSION "$ENV{VisualStudioVersion}")
elseif(CMD_VSWHERE)
  # Use vswhere to get Visual Studio version
  execute_process(
    COMMAND ${CMD_VSWHERE} -latest -property installationVersion
    OUTPUT_VARIABLE VISUAL_STUDIO_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()

# VSバージョンが取れた場合VsDevCmdを探す
if(VISUAL_STUDIO_VERSION)
  # extract major version
  string(REGEX REPLACE "([0-9]+)\\..+" "\\1" VS_VERSION "${VISUAL_STUDIO_VERSION}")

  # Use vswhere to find VsDevCmd.bat
  execute_process(
    COMMAND ${CMD_VSWHERE} -find "Common7\\Tools\\VsDevCmd.bat" -version "${VS_VERSION}"
    OUTPUT_VARIABLE CMD_VS_DEV
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )

  # Convert backslashes to forward slashes
  string(REPLACE "\\" "/" CMD_VS_DEV "${CMD_VS_DEV}")

  if(CMD_VS_DEV)
    message(STATUS "Found VsDevCmd: ${CMD_VS_DEV}")
  endif()
endif(VISUAL_STUDIO_VERSION)

# Find Git with additional search paths
find_program(GIT_EXECUTABLE git
  PATHS
    "$ENV{ProgramFiles}/Git"
  PATH_SUFFIXES
    cmd
    bin
)

if(NOT GIT_EXECUTABLE)
  message(FATAL_ERROR "Git not found")
endif()

message(STATUS "Found Git: ${GIT_EXECUTABLE}")

# Find patch.exe from Git installation
find_program(PATCH_EXECUTABLE patch
  PATHS
    "$ENV{ProgramFiles}/Git/usr/bin"
    "$ENV{LOCALAPPDATA}/Programs/Git/usr/bin"
  NO_DEFAULT_PATH
  DOC "patch.exe command from Git"
)

if(NOT PATCH_EXECUTABLE)
  message(FATAL_ERROR "patch.exe was not found. Please install Git.")
endif()

message(STATUS "Found patch.exe: ${PATCH_EXECUTABLE}")

# Find PowerShell Core(required)
find_program(CMD_PWSH pwsh.exe
  PATHS
    "$ENV{LOCALAPPDATA}/Microsoft/WindowsApps"
    "$ENV{ProgramFiles}/PowerShell/7"
  DOC "PowerShell Core"
)

if(NOT CMD_PWSH)
  message(FATAL_ERROR "pwsh.exe was not found.")
endif()

message(STATUS "Found PowerShell Core: ${CMD_PWSH}")

# Find Python Interpreter(required)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

message(STATUS "Found Python: ${Python3_EXECUTABLE}")

# Find 7zip for archive extraction
find_program(7ZIP_EXECUTABLE 7z
  PATHS
    "$ENV{ProgramFiles}/7-zip"
    "$ENV{ChocolateyInstall}/bin"
)

if(NOT 7ZIP_EXECUTABLE)
  message(FATAL_ERROR "7z.exe not found")
endif()

message(STATUS "Found 7z: ${7ZIP_EXECUTABLE}")

# Create a custom command for banner generation
add_custom_target(show_dev_banner ALL
  COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan "-------------------------------------------------------------------------------------"
  COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan "---  This is a Dev version and under development. Be careful to use this version. ---"
  COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan "-------------------------------------------------------------------------------------"
  VERBATIM
)

# Include compiletests.cmake
include(${CMAKE_SOURCE_DIR}/src/test/cmake/compiletests.cmake)

# Git and CI metadata are dynamic inputs that cannot be represented completely
# by file timestamps. Observe them on each requested build, while
# configure_file keeps version.h content- and timestamp-stable on a true no-op.
add_custom_target(generate_version_header
  COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
    -DOUTPUT_FILE=${CMAKE_BINARY_DIR}/version.h
    -DQUIET=ON
    -P ${CMAKE_SOURCE_DIR}/src/main/cmake/version.cmake
  BYPRODUCTS "${CMAKE_BINARY_DIR}/version.h"
  DEPENDS
    "${CMAKE_SOURCE_DIR}/src/main/cmake/version.cmake"
    "${CMAKE_SOURCE_DIR}/src/main/cmake/version.h.in"
  COMMENT "Ensuring version.h matches Git and CI state"
  VERBATIM
)

# Create a custom command for funccode_define generation
add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/Funccode_define.h"
  COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_SOURCE_DIR}/src/main/py/header_make.py"
    -in=${CMAKE_SOURCE_DIR}/sakura_core/Funccode_x.hsrc
    -out=${CMAKE_BINARY_DIR}/Funccode_define.h
    -mode=define
  DEPENDS
    ${CMAKE_SOURCE_DIR}/src/main/py/header_make.py
    ${CMAKE_SOURCE_DIR}/sakura_core/Funccode_x.hsrc
  COMMENT "Generating Funccode_define.h"
  VERBATIM
)

# Create a custom target that depends on the generated file
add_custom_target(generate_funccode_define
  DEPENDS
    "${CMAKE_BINARY_DIR}/Funccode_define.h"
)

# Create a custom command for funccode_enum generation
add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/Funccode_enum.h"
  COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_SOURCE_DIR}/src/main/py/header_make.py"
    -in=${CMAKE_SOURCE_DIR}/sakura_core/Funccode_x.hsrc
    -out=${CMAKE_BINARY_DIR}/Funccode_enum.h
    -mode=enum
    -enum=EFunctionCode
  DEPENDS
    ${CMAKE_SOURCE_DIR}/src/main/py/header_make.py
    ${CMAKE_SOURCE_DIR}/sakura_core/Funccode_x.hsrc
  COMMENT "Generating Funccode_enum.h"
  VERBATIM
)

# Create a custom target that depends on the generated file
add_custom_target(generate_funccode_enum
  DEPENDS
    "${CMAKE_BINARY_DIR}/Funccode_enum.h"
)

# The manifest has a stable template contract. Running the lightweight
# renderer is cheaper and more reliable than coupling a Visual Studio phony
# target to a file custom command; configure_file preserves a true no-op.
add_custom_target(generate_sakura_exe_manifest
  COMMAND ${CMAKE_COMMAND} 
    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DEXE_NAME=sakura.exe
    -DEXE_ARCH=${EXE_ARCH}
    -DOUTPUT_FILE=${CMAKE_BINARY_DIR}/sakura.exe.manifest
    -P ${CMAKE_SOURCE_DIR}/src/main/cmake/manifest.cmake
  BYPRODUCTS "${CMAKE_BINARY_DIR}/sakura.exe.manifest"
  DEPENDS
    "${CMAKE_SOURCE_DIR}/src/main/cmake/manifest.cmake"
    "${CMAKE_SOURCE_DIR}/src/main/cmake/manifest.in"
  COMMENT "Ensuring sakura.exe.manifest matches its template"
  VERBATIM
)

# Resolve darkmodelib from vcpkg local registry
find_package(darkmodelib CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(Microsoft.GSL CONFIG REQUIRED)
find_package(WIL CONFIG REQUIRED)

# Resolve bregonig from vcpkg local registry
find_package(bregonig CONFIG REQUIRED)
set(BREGONIG_RUNTIME
  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/bregonig.dll")
set(COPY_RUNTIME_ASSET_SCRIPT
  "${CMAKE_SOURCE_DIR}/src/main/cmake/copy_runtime_asset.cmake")

add_custom_target(generate_bregonig
  COMMAND ${CMAKE_COMMAND}
    -DINPUT_FILE:FILEPATH=${BREGONIG_RUNTIME}
    -DOUTPUT_FILE:FILEPATH=${OUTPUT_DIRECTORY}/bregonig.dll
    -P ${COPY_RUNTIME_ASSET_SCRIPT}
  BYPRODUCTS "${OUTPUT_DIRECTORY}/bregonig.dll"
  DEPENDS
    "${BREGONIG_RUNTIME}"
    "${COPY_RUNTIME_ASSET_SCRIPT}"
  COMMENT "Ensuring bregonig.dll is staged"
  VERBATIM
)

# Resolve cmigemo from vcpkg local registry
find_package(cmigemo CONFIG REQUIRED)
set(CMIGEMO_RUNTIME
  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/migemo.dll")

add_custom_target(generate_cmigemo
  COMMAND ${CMAKE_COMMAND}
    -DINPUT_FILE:FILEPATH=${CMIGEMO_RUNTIME}
    -DOUTPUT_FILE:FILEPATH=${OUTPUT_DIRECTORY}/migemo.dll
    -P ${COPY_RUNTIME_ASSET_SCRIPT}
  BYPRODUCTS "${OUTPUT_DIRECTORY}/migemo.dll"
  DEPENDS
    "${CMIGEMO_RUNTIME}"
    "${COPY_RUNTIME_ASSET_SCRIPT}"
  COMMENT "Ensuring migemo.dll is staged"
  VERBATIM
)

find_package(ppa-stub CONFIG REQUIRED)
set(PPA_STUB_RUNTIME
  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/ppa_stub.dll")

add_custom_target(ppa_stub
  COMMAND ${CMAKE_COMMAND}
    -DINPUT_FILE:FILEPATH=${PPA_STUB_RUNTIME}
    -DOUTPUT_FILE:FILEPATH=${OUTPUT_DIRECTORY}/ppa_stub.dll
    -P ${COPY_RUNTIME_ASSET_SCRIPT}
  BYPRODUCTS "${OUTPUT_DIRECTORY}/ppa_stub.dll"
  DEPENDS
    "${PPA_STUB_RUNTIME}"
    "${COPY_RUNTIME_ASSET_SCRIPT}"
  COMMENT "Ensuring ppa_stub.dll is staged"
  VERBATIM
)

find_package(dll-plugin1 CONFIG REQUIRED)
set(DLL_PLUGIN1_RUNTIME
  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/dll_plugin1.dll")

add_custom_target(dll_plugin1
  COMMAND ${CMAKE_COMMAND}
    -DINPUT_FILE:FILEPATH=${DLL_PLUGIN1_RUNTIME}
    -DOUTPUT_FILE:FILEPATH=${OUTPUT_DIRECTORY}/dll_plugin1.dll
    -P ${COPY_RUNTIME_ASSET_SCRIPT}
  BYPRODUCTS "${OUTPUT_DIRECTORY}/dll_plugin1.dll"
  DEPENDS
    "${DLL_PLUGIN1_RUNTIME}"
    "${COPY_RUNTIME_ASSET_SCRIPT}"
  COMMENT "Ensuring dll_plugin1.dll is staged"
  VERBATIM
)

if(MINGW)
  # Find iconv
  find_program(ICONV_PATH iconv REQUIRED)

  if(NOT ICONV_PATH)
    message(FATAL_ERROR "iconv was not found.")
  endif()

  message(STATUS "Found iconv: ${ICONV_PATH}")
endif(MINGW)

# Function to convert RC files from UTF-16LE to UTF-8 for MinGW
# Parameters:
#   RC_FILES_VAR - Variable name containing list of RC file paths
#   LOCALE_NAME  - Locale name
#   BINARY_DIR   - Directory where converted files will be placed
function(convert_rc_files_to_utf8 RC_FILES_VAR LOCALE_NAME BINARY_DIR)
  set(RC_FILES_UTF8)
  foreach(RC_FILE ${${RC_FILES_VAR}})
    get_filename_component(RC_NAME ${RC_FILE} NAME_WE)
    get_filename_component(RC_EXT ${RC_FILE} EXT)
    set(UTF8_RC_FILE ${BINARY_DIR}/${RC_NAME}_${LOCALE_NAME}/${RC_NAME}${RC_EXT})
    
    add_custom_command(
      OUTPUT ${UTF8_RC_FILE}
      COMMAND ${ICONV_PATH} -f UTF-16LE -t UTF-8 "${RC_FILE}" > "${UTF8_RC_FILE}"
      DEPENDS ${RC_FILE}
      COMMENT "Converting ${RC_NAME}_${LOCALE_NAME}${RC_EXT} from UTF-16LE to UTF-8 using iconv"
    )
    
    list(APPEND RC_FILES_UTF8 ${UTF8_RC_FILE})
  endforeach()
  
  # Replace the original variable with UTF-8 converted files
  set(${RC_FILES_VAR} ${RC_FILES_UTF8} PARENT_SCOPE)
endfunction()

# Function to create a language DLL project
# Parameters:
#   LOCALE_NAME  - Name of the locale (e.g., en-US, zh-CN)
#   LOCALE_ID    - Locale identifier in decimal (e.g., 1033 for en-US, 2052 for zh-CN)
function(create_language_dll LOCALE_NAME LOCALE_ID)
  string(REPLACE "-" "_" LOCALE_NAME_UNDERSCORE "${LOCALE_NAME}")
  set(SAKURA_LANG sakura_lang_${LOCALE_NAME_UNDERSCORE})
  
  set(RC_FOLDER ${CMAKE_SOURCE_DIR}/sakura_lang)

  set(RESOURCE_SCRIPTS
    ${RC_FOLDER}/sakura_rc_${LOCALE_NAME}.rc
    ${RC_FOLDER}/sakura_rc_${LOCALE_NAME}.rc2)
  
  if(MINGW)
    # Convert RC files to UTF-8 for MinGW
    convert_rc_files_to_utf8(RESOURCE_SCRIPTS "${LOCALE_NAME}" ${CMAKE_CURRENT_BINARY_DIR})
  endif(MINGW)
  
  # Create the library
  add_library(${SAKURA_LANG} MODULE ${RESOURCE_SCRIPTS})

  # Add dependencies
  add_dependencies(${SAKURA_LANG}
    generate_version_header
    generate_funccode_define
  )
  
  # Set target properties
  set_target_properties(${SAKURA_LANG}
    PROPERTIES
      # ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIRECTORY}"
      LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIRECTORY}"
      LINKER_LANGUAGE "CXX"
      PDB_OUTPUT_DIRECTORY "${OUTPUT_DIRECTORY}"
      # RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIRECTORY}"
  )

  add_custom_command(TARGET ${SAKURA_LANG} PRE_LINK
    COMMAND ${CMAKE_COMMAND} -E remove -f $<TARGET_FILE:${SAKURA_LANG}>
  )

  # MSVC specific settings
  if(MSVC)
    # Convert decimal LOCALE_ID to hexadecimal for MSVC RC
    math(EXPR LOCALE_ID_HEX "${LOCALE_ID}" OUTPUT_FORMAT HEXADECIMAL)

    # Set RC flags for MSVC
    target_compile_options(${SAKURA_LANG}
      PRIVATE
        /l ${LOCALE_ID_HEX}
    )

    # avoid error LNK2001 for "__DllMainCRTStartup@12"
    set_target_properties(${SAKURA_LANG}
      PROPERTIES
        LINK_FLAGS "/NOENTRY /INCREMENTAL:NO"
    )
  endif(MSVC)
  
  # MinGW specific settings
  if(MINGW)
    # Set RC flags for MinGW (windres uses decimal)
    target_compile_options(${SAKURA_LANG}
      PRIVATE
        "$<$<COMPILE_LANGUAGE:RC>:-c 65001-l ${LOCALE_ID} --use-temp-file>"
    )

    # avoid prefixing of DLL name, set PREFIX to blank.
    # https://cmake.org/cmake/help/v3.12/prop_tgt/PREFIX.html?highlight=prefix
    set_target_properties(${SAKURA_LANG}
      PROPERTIES
        PREFIX ""
    )
  endif(MINGW)
endfunction(create_language_dll)

# add global definitions
add_compile_definitions(
  UNICODE
  _UNICODE
  WINVER=0x0A00
  _WIN32_WINNT=0x0A00
  NTDDI_VERSION=NTDDI_WIN10_CO
  $<$<CONFIG:Debug>:_DEBUG>
  $<$<CONFIG:Release>:NDEBUG>
)

# add include directories
include_directories(
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/src/main/cpp
  ${CMAKE_SOURCE_DIR}/src/main/resources
  ${CMAKE_SOURCE_DIR}/sakura_core/include
  ${CMAKE_SOURCE_DIR}/sakura_core
)

if(MSVC)
  # VCランタイムを指定する（/MTd, /MTにする）
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

  # 静的リンクするライブラリの構築に使うパラメーターを作る
  set(GENERATOR_ARGS_FOR_STATIC_LIBRARY "\"-DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}\"")

  add_compile_options(
    /source-charset:utf-8
    /execution-charset:shift_jis
    /w34996
  )
endif(MSVC)

if(MINGW)
  add_compile_options(
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Release>:-O2>
    -MMD
    -finput-charset=utf-8
    -fexec-charset=cp932
    -Wdeprecated-declarations
    -Wno-trigraphs
  )
endif(MINGW)

# define header files of sakura-editor
file(GLOB_RECURSE HEADERS
  ${CMAKE_SOURCE_DIR}/src/main/cpp/*.hpp
  ${CMAKE_SOURCE_DIR}/src/main/cpp/*.h
  ${CMAKE_SOURCE_DIR}/sakura_core/*.hpp
  ${CMAKE_SOURCE_DIR}/sakura_core/*.h
)

# define source files of sakura_core
file(GLOB_RECURSE SOURCES
  ${CMAKE_SOURCE_DIR}/src/main/cpp/*.cpp
  ${CMAKE_SOURCE_DIR}/sakura_core/*.cpp
)

# Keep the native Markdown code highlighter explicit so its MSBuild and CMake
# registrations remain aligned even though the broad source discovery sees it.
set(MARKDOWN_CODE_HIGHLIGHTER_HEADERS
  ${CMAKE_SOURCE_DIR}/sakura_core/markdown/MarkdownCodeHighlighter.h
)
set(MARKDOWN_CODE_HIGHLIGHTER_SOURCES
  ${CMAKE_SOURCE_DIR}/sakura_core/markdown/MarkdownCodeHighlighter.cpp
)
list(REMOVE_ITEM HEADERS ${MARKDOWN_CODE_HIGHLIGHTER_HEADERS})
list(APPEND HEADERS ${MARKDOWN_CODE_HIGHLIGHTER_HEADERS})
list(REMOVE_ITEM SOURCES ${MARKDOWN_CODE_HIGHLIGHTER_SOURCES})
list(APPEND SOURCES ${MARKDOWN_CODE_HIGHLIGHTER_SOURCES})

# Keep the native terminal rendering seam explicit even though the broad source
# discovery above also sees these paths.  This makes CMake registration match
# the MSBuild project and preserves their deterministic build order.
set(TERMINAL_RENDERER_HEADERS
  ${CMAKE_SOURCE_DIR}/sakura_core/terminal/window/TerminalBuiltinGlyphRenderer.h
  ${CMAKE_SOURCE_DIR}/sakura_core/terminal/window/TerminalDWriteRenderer.h
  ${CMAKE_SOURCE_DIR}/sakura_core/terminal/window/TerminalRenderPlan.h
)
set(TERMINAL_RENDERER_SOURCES
  ${CMAKE_SOURCE_DIR}/sakura_core/terminal/window/TerminalBuiltinGlyphRenderer.cpp
  ${CMAKE_SOURCE_DIR}/sakura_core/terminal/window/TerminalDWriteRenderer.cpp
  ${CMAKE_SOURCE_DIR}/sakura_core/terminal/window/TerminalRenderPlan.cpp
)
list(REMOVE_ITEM HEADERS ${TERMINAL_RENDERER_HEADERS})
list(APPEND HEADERS ${TERMINAL_RENDERER_HEADERS})
list(REMOVE_ITEM SOURCES ${TERMINAL_RENDERER_SOURCES})
list(APPEND SOURCES ${TERMINAL_RENDERER_SOURCES})

# Do not let the broad source glob pull in the complete Windows Terminal tree.
# Only the dependency-closed parser, input and Unicode boundary is compiled.
set(WINDOWS_TERMINAL_VENDOR_ROOT
  ${CMAKE_SOURCE_DIR}/sakura_core/terminal/vendor/windows_terminal
)
set(WINDOWS_TERMINAL_VENDOR_SOURCES
  ${WINDOWS_TERMINAL_VENDOR_ROOT}/src/types/CodepointWidthDetector.cpp
  ${WINDOWS_TERMINAL_VENDOR_ROOT}/src/terminal/parser/stateMachine.cpp
  ${WINDOWS_TERMINAL_VENDOR_ROOT}/src/terminal/input/terminalInput.cpp
  ${WINDOWS_TERMINAL_VENDOR_ROOT}/src/terminal/input/mouseInput.cpp
  ${WINDOWS_TERMINAL_VENDOR_ROOT}/sakura_compat/WindowsTerminalCompat.cpp
)
list(FILTER SOURCES EXCLUDE REGEX
  ".*/sakura_core/terminal/vendor/windows_terminal/.*\\.cpp$"
)
list(APPEND SOURCES ${WINDOWS_TERMINAL_VENDOR_SOURCES})

# Onigmo is vendored as a submodule under externals/Onigmo/, outside the
# sakura_core/src-main-cpp trees the globs above cover, so it is never
# auto-discovered. Compile its own .c sources as ordinary translation units
# straight into sakura_core (Option A in sakura_core/textmate/CLAUDE.md,
# "Onigmo build integration") instead of adding a vcpkg/find_package
# dependency: the vendored copy is submodule-pinned, not vcpkg-versioned, and
# this matches how OnigmoRegexEngine.cpp already resolves
# "Onigmo/onigmo.h" straight from externals/Onigmo. This file list mirrors
# the vetted-but-dormant tools/vcpkg-local-registry/ports/onigmo/CMakeLists.txt
# ONIGMO_SOURCES list; do not add testc.c, test_enc_utf8.c, or testu.c, which
# are Onigmo's own test executables.
set(ONIGMO_ROOT ${CMAKE_SOURCE_DIR}/externals/Onigmo)
set(ONIGMO_SOURCES
  ${ONIGMO_ROOT}/regcomp.c
  ${ONIGMO_ROOT}/regenc.c
  ${ONIGMO_ROOT}/regerror.c
  ${ONIGMO_ROOT}/regexec.c
  ${ONIGMO_ROOT}/regext.c
  ${ONIGMO_ROOT}/reggnu.c
  ${ONIGMO_ROOT}/regparse.c
  ${ONIGMO_ROOT}/regposerr.c
  ${ONIGMO_ROOT}/regposix.c
  ${ONIGMO_ROOT}/regsyntax.c
  ${ONIGMO_ROOT}/regtrav.c
  ${ONIGMO_ROOT}/regversion.c
  ${ONIGMO_ROOT}/st.c
  ${ONIGMO_ROOT}/enc/ascii.c
  ${ONIGMO_ROOT}/enc/big5.c
  ${ONIGMO_ROOT}/enc/euc_jp.c
  ${ONIGMO_ROOT}/enc/euc_kr.c
  ${ONIGMO_ROOT}/enc/euc_tw.c
  ${ONIGMO_ROOT}/enc/gb18030.c
  ${ONIGMO_ROOT}/enc/iso_8859_1.c
  ${ONIGMO_ROOT}/enc/iso_8859_2.c
  ${ONIGMO_ROOT}/enc/iso_8859_3.c
  ${ONIGMO_ROOT}/enc/iso_8859_4.c
  ${ONIGMO_ROOT}/enc/iso_8859_5.c
  ${ONIGMO_ROOT}/enc/iso_8859_6.c
  ${ONIGMO_ROOT}/enc/iso_8859_7.c
  ${ONIGMO_ROOT}/enc/iso_8859_8.c
  ${ONIGMO_ROOT}/enc/iso_8859_9.c
  ${ONIGMO_ROOT}/enc/iso_8859_10.c
  ${ONIGMO_ROOT}/enc/iso_8859_11.c
  ${ONIGMO_ROOT}/enc/iso_8859_13.c
  ${ONIGMO_ROOT}/enc/iso_8859_14.c
  ${ONIGMO_ROOT}/enc/iso_8859_15.c
  ${ONIGMO_ROOT}/enc/iso_8859_16.c
  ${ONIGMO_ROOT}/enc/koi8_r.c
  ${ONIGMO_ROOT}/enc/koi8_u.c
  ${ONIGMO_ROOT}/enc/shift_jis.c
  ${ONIGMO_ROOT}/enc/unicode.c
  ${ONIGMO_ROOT}/enc/utf_8.c
  ${ONIGMO_ROOT}/enc/utf_16be.c
  ${ONIGMO_ROOT}/enc/utf_16le.c
  ${ONIGMO_ROOT}/enc/utf_32be.c
  ${ONIGMO_ROOT}/enc/utf_32le.c
  ${ONIGMO_ROOT}/enc/windows_1250.c
  ${ONIGMO_ROOT}/enc/windows_1251.c
  ${ONIGMO_ROOT}/enc/windows_1252.c
  ${ONIGMO_ROOT}/enc/windows_1253.c
  ${ONIGMO_ROOT}/enc/windows_1254.c
  ${ONIGMO_ROOT}/enc/windows_1257.c
  ${ONIGMO_ROOT}/enc/windows_31j.c
)
list(APPEND SOURCES ${ONIGMO_SOURCES})

# A committed generated projection removes extracted provider sources from the
# legacy glob and defines their standalone native targets.  The OPTIONAL hook
# is the rollback seam: without the projection the original glob owns them.
include(
  "${CMAKE_SOURCE_DIR}/src/main/modules/generated/cmake/legacy/source-ownership.cmake"
  OPTIONAL
)

set(RESOURCE_SCRIPTS
  ${CMAKE_SOURCE_DIR}/sakura_core/sakura_rc.rc
  ${CMAKE_SOURCE_DIR}/sakura_core/sakura_rc.rc2
)

set(NATVIS_FILES
  ${CMAKE_SOURCE_DIR}/src/main/resources/sakura.natvis
)

# define precompiled headers
set(PCH_HEADER ${CMAKE_SOURCE_DIR}/sakura_core/StdAfx.h)

if(MINGW)
  # Convert RC files to UTF-8 for MinGW
  convert_rc_files_to_utf8(RESOURCE_SCRIPTS "ja-JP" ${CMAKE_BINARY_DIR})
endif(MINGW)

# Create sakura_core object library
add_library(sakura_core OBJECT ${PCH_HEADER} ${SOURCES} ${RESOURCE_SCRIPTS} ${HEADERS})

# Enable precompiled headers for sakura_core
target_precompile_headers(sakura_core PRIVATE ${PCH_HEADER})

# This upstream TU owns its minimal compatibility precomp.h.  Injecting
# Sakura's StdAfx PCH would make the vendor boundary differ between CMake and
# MSBuild builds.
set_source_files_properties(${WINDOWS_TERMINAL_VENDOR_SOURCES}
  PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

# Onigmo is plain C compiled with Sakura's own C++ StdAfx PCH disabled (a C
# translation unit cannot consume a C++ precompiled header), its own
# Windows-targeted config.h under win32/ (the same header its own nmake build
# uses, so HAVE_CONFIG_H needs no newly written config header), and its own
# enc/unicode/ headers (casefold.h, name2ctype.h) included unqualified from
# enc/unicode.c. ONIG_EXTERN=extern matches how Onigmo is consumed as a
# statically linked, not exported/imported, translation unit. These
# definitions and include directories are scoped to only the Onigmo sources so
# they do not leak into the rest of sakura_core, matching the MSBuild
# per-file overrides in sakura_core/sakura.vcxproj.
# Onigmo's own internal headers (regenc.h, regint.h, regparse.h, st.h,
# onigmo.h, onigmoposix.h) live at ${ONIGMO_ROOT} itself, not only under
# win32/ or enc/unicode/; several enc/*.c files #include "regenc.h" /
# "regint.h" unqualified, and euc_jp.c / enc/shift_jis.h #include the
# relatively pathed "enc/jis/props.h", which only resolves once ${ONIGMO_ROOT}
# itself is on the include path (as ${ONIGMO_ROOT}/enc/jis/props.h).
set_source_files_properties(${ONIGMO_SOURCES}
  PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_DEFINITIONS "HAVE_CONFIG_H;ONIG_EXTERN=extern"
    INCLUDE_DIRECTORIES "${ONIGMO_ROOT};${ONIGMO_ROOT}/win32;${ONIGMO_ROOT}/enc/unicode"
)

# GCC 14 and later diagnose the vendored Onigmo version's K&R-compatible
# ANYARGS callback declarations as errors, and GCC 15 defaults to C23 where an
# empty parameter list means no arguments. Compile this legacy boundary as GNU
# C17 and keep the diagnostic downgrade local to Onigmo's C translation units;
# do not weaken diagnostics for Sakura.
if(MINGW AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
  set_property(SOURCE ${ONIGMO_SOURCES} APPEND PROPERTY
    COMPILE_OPTIONS -std=gnu17 -Wno-error=incompatible-pointer-types
  )

  # MinGW's wincodec.h does not yet expose the Windows SDK's high-quality
  # cubic enumerator. Keep that SDK spelling difference at the MinGW build
  # boundary and retain cubic interpolation for the three WIC consumers.
  set_property(SOURCE
    ${CMAKE_SOURCE_DIR}/sakura_core/markdown/CMarkdownPreviewWnd.cpp
    ${CMAKE_SOURCE_DIR}/sakura_core/workbench/extension/ExtensionIconDecoder.cpp
    ${CMAKE_SOURCE_DIR}/sakura_core/workbench/explorer/CExplorerTool.cpp
    APPEND PROPERTY COMPILE_DEFINITIONS
      WICBitmapInterpolationModeHighQualityCubic=WICBitmapInterpolationModeCubic
  )
endif()

# Keep higher-ISA code in isolated translation units.  The baseline executable
# remains AVX-compatible, while the process-wide dispatch table calls these
# implementations only after CPUID and XGETBV validation.
set(SAKURA_CPU_DISPATCH_AVX_SOURCE
  ${CMAKE_SOURCE_DIR}/sakura_core/util/CpuDispatchAvx.cpp
)
set(SAKURA_CPU_DISPATCH_AVX2_SOURCE
  ${CMAKE_SOURCE_DIR}/sakura_core/util/CpuDispatchAvx2.cpp
)
set(SAKURA_CPU_DISPATCH_AVX512_SOURCE
  ${CMAKE_SOURCE_DIR}/sakura_core/util/CpuDispatchAvx512.cpp
)
set_source_files_properties(
  ${SAKURA_CPU_DISPATCH_AVX_SOURCE}
  ${SAKURA_CPU_DISPATCH_AVX2_SOURCE}
  ${SAKURA_CPU_DISPATCH_AVX512_SOURCE}
  PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)
if(MSVC)
  set_source_files_properties(${SAKURA_CPU_DISPATCH_AVX_SOURCE}
    PROPERTIES COMPILE_OPTIONS "/arch:AVX;/GL-"
  )
  set_source_files_properties(${SAKURA_CPU_DISPATCH_AVX2_SOURCE}
    PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/GL-"
  )
  set_source_files_properties(${SAKURA_CPU_DISPATCH_AVX512_SOURCE}
    PROPERTIES COMPILE_OPTIONS "/arch:AVX512;/GL-"
  )
elseif(MINGW)
  set_source_files_properties(${SAKURA_CPU_DISPATCH_AVX_SOURCE}
    PROPERTIES COMPILE_OPTIONS "-mavx"
  )
  set_source_files_properties(${SAKURA_CPU_DISPATCH_AVX2_SOURCE}
    PROPERTIES COMPILE_OPTIONS "-mavx2"
  )
  set_source_files_properties(${SAKURA_CPU_DISPATCH_AVX512_SOURCE}
    PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512bw"
  )
endif()

# Set C++ standard for sakura_core
target_compile_features(sakura_core PUBLIC cxx_std_20)

# Add include directories for sakura_core
set(WINDOWS_TERMINAL_MINGW_COMPAT_INCLUDE_DIRS)
if(MINGW)
  # Keep MinGW-only stubs ahead of the vendor include roots without shadowing
  # Windows SDK headers on the MSVC path.
  list(APPEND WINDOWS_TERMINAL_MINGW_COMPAT_INCLUDE_DIRS
    ${WINDOWS_TERMINAL_VENDOR_ROOT}/sakura_compat/mingw
  )
endif()

target_include_directories(sakura_core
  SYSTEM
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/externals>"
  PRIVATE
    ${WINDOWS_TERMINAL_MINGW_COMPAT_INCLUDE_DIRS}
    ${WINDOWS_TERMINAL_VENDOR_ROOT}/sakura_compat
    ${WINDOWS_TERMINAL_VENDOR_ROOT}/src
    ${WINDOWS_TERMINAL_VENDOR_ROOT}/src/inc
    ${WINDOWS_TERMINAL_VENDOR_ROOT}/src/types
)

# Add link directories for sakura_core
target_link_directories(sakura_core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/lib$<$<CONFIG:Debug>:/Debug>>"
)

# link libraries
target_link_libraries(sakura_core
  PUBLIC
    darkmodelib::darkmodelib
    fmt::fmt
    Microsoft.GSL::GSL
    WIL::WIL
    advapi32
    bcrypt
    comctl32
    crypt32
    d2d1
    dbghelp
    dwmapi
    dwrite
    htmlhelp
    imm32
    mpr
    msimg32
    ole32
    oleaut32
    shlwapi
    uuid
    uxtheme
    windowscodecs
    winhttp
    winmm
    winspool
)

# Add dependencies for sakura_core
add_dependencies(sakura_core
  generate_version_header
  generate_funccode_define
  generate_funccode_enum
  generate_bregonig
  generate_cmigemo
)

if(MSVC)
  target_sources(sakura_core
    PUBLIC
      ${NATVIS_FILES}
  )
  set_target_properties(sakura_core
    PROPERTIES
      VS_DEBUGGER_VISUALIZER "${NATVIS_FILES}"
  )
  # add definitions for sakura_core
  target_compile_definitions(sakura_core
    PUBLIC
      NOMINMAX
  )
  if(SAKURA_GENERATE_ASSEMBLY_LISTINGS)
    target_compile_options(sakura_core
      PRIVATE
        /FAsu
        /Fa"${CMAKE_BINARY_DIR}/"
    )
  endif()
endif(MSVC)

if(MINGW)
  # Set RC flags for MinGW (windres uses decimal)
  set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -c 65001 -l 1041 --use-temp-file")

  # Add include directories for sakura_core
  target_include_directories(sakura_core
    PRIVATE
      "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/sakura_rc_ja-JP>"
  )

  target_link_options(sakura_core
    PUBLIC
      -municode
      -static
      $<$<CONFIG:Release>:-s>
  )

  set(SAKURA_EXE_MANIFEST "${CMAKE_BINARY_DIR}/sakura.exe.manifest")
  set(SAKURA_MANIFEST_RC "${CMAKE_BINARY_DIR}/sakura_manifest.rc")

  # Create a custom command for sakura_manifest.rc generation
  add_custom_command(
    OUTPUT "${SAKURA_MANIFEST_RC}"
    COMMAND ${CMAKE_COMMAND} 
      -DSOURCE_DIR="${CMAKE_SOURCE_DIR}"
      -DOUTPUT_FILE="${SAKURA_MANIFEST_RC}"
      -DMANIFEST_FILE="${SAKURA_EXE_MANIFEST}"
      -P ${CMAKE_SOURCE_DIR}/src/main/cmake/manifest_resource.cmake
    DEPENDS
      "${SAKURA_EXE_MANIFEST}"
      "${CMAKE_SOURCE_DIR}/src/main/cmake/manifest_resource.cmake"
      "${CMAKE_SOURCE_DIR}/src/main/cmake/manifest_resource.in"
    COMMENT "Generating sakura_manifest.rc"
    VERBATIM
  )
endif(MINGW)
