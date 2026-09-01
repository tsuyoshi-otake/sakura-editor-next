get_filename_component(REPO_ROOT "${CURRENT_PORT_DIR}/../../../.." ABSOLUTE)
set(BREGONIG_SOURCE_DIR "${REPO_ROOT}/third_party/owned/bregonig-next")

if(NOT EXISTS "${BREGONIG_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "bregonig-next CMakeLists.txt not found: ${BREGONIG_SOURCE_DIR}")
endif()
if(NOT EXISTS "${BREGONIG_SOURCE_DIR}/src/bregexp.h")
  message(FATAL_ERROR "bregonig source not found: ${BREGONIG_SOURCE_DIR}")
endif()

# The snapshot defines its exported entry points at global scope as
# `int ::BMatch(...)`. GCC 15 rejects that qualification outright - it is a
# hard error, not a permerror, so -fpermissive does not restore the old
# behaviour - and the MinGW triplet stopped building a tree MSVC still
# accepts. `third_party/owned/bregonig-next/SNAPSHOT.json` sets
# `localModificationAllowed` to false and `tools/owned_snapshot.py` digests
# the whole tree, so the checked-in source must stay byte for byte as
# imported. The canonical fix belongs in tsuyoshi-otake/bregonig-next
# followed by a re-imported snapshot.
#
# Until then the MinGW build configures from a private copy in the build
# tree with the MSVC-only assumptions patched out of it. The copy is
# throwaway, the verified snapshot is never written to, and the transform
# fails loudly if it stops matching, so a future snapshot cannot silently
# build unpatched.
set(BREGONIG_CONFIGURE_SOURCE "${BREGONIG_SOURCE_DIR}")
set(BREGONIG_CONFIGURE_OPTIONS
  -DBREGONIG_BUILD_TESTS=OFF
  -DBREGONIG_BUILD_FUZZ=OFF
)

if(VCPKG_TARGET_IS_MINGW)
  set(BREGONIG_MINGW_SOURCE "${CURRENT_BUILDTREES_DIR}/src-mingw")
  file(REMOVE_RECURSE "${BREGONIG_MINGW_SOURCE}")
  file(COPY "${BREGONIG_SOURCE_DIR}/" DESTINATION "${BREGONIG_MINGW_SOURCE}")

  set(BREGONIG_MINGW_MAIN "${BREGONIG_MINGW_SOURCE}/src/bregonig.cpp")
  file(READ "${BREGONIG_MINGW_MAIN}" BREGONIG_MINGW_TEXT)
  string(REGEX MATCHALL "\n[A-Za-z_][A-Za-z0-9_]*[ \t]+[*]?::" BREGONIG_QUALIFIED "${BREGONIG_MINGW_TEXT}")
  list(LENGTH BREGONIG_QUALIFIED BREGONIG_QUALIFIED_COUNT)
  if(BREGONIG_QUALIFIED_COUNT EQUAL 0)
    message(FATAL_ERROR
      "bregonig.cpp no longer declares its exports with an explicit global "
      "qualification; remove this MinGW workaround.")
  endif()
  string(REGEX REPLACE "(\n[A-Za-z_][A-Za-z0-9_]*[ \t]+[*]?)::" "\\1" BREGONIG_MINGW_TEXT "${BREGONIG_MINGW_TEXT}")
  file(WRITE "${BREGONIG_MINGW_MAIN}" "${BREGONIG_MINGW_TEXT}")
  message(STATUS "bregonig: removed ${BREGONIG_QUALIFIED_COUNT} global qualifications for the MinGW build")


  # `mem_vc6.h` guards its Visual C++ 6 fallback with a bare
  # `#if _MSC_VER < 1300`. Under GCC the macro is undefined, evaluates to 0,
  # and the VC6 branch is taken, so the build ends up calling the MSVC-only
  # `_set_new_handler` and fails to link with an undefined reference. The
  # guard has to test that the macro is defined at all.
  set(BREGONIG_MINGW_MEM "${BREGONIG_MINGW_SOURCE}/src/mem_vc6.h")
  file(READ "${BREGONIG_MINGW_MEM}" BREGONIG_MEM_TEXT)
  string(FIND "${BREGONIG_MEM_TEXT}" "#if _MSC_VER < 1300" BREGONIG_MEM_FOUND)
  if(BREGONIG_MEM_FOUND EQUAL -1)
    message(FATAL_ERROR
      "mem_vc6.h no longer guards its VC6 fallback with a bare _MSC_VER "
      "test; remove this MinGW workaround.")
  endif()
  string(REPLACE "#if _MSC_VER < 1300" "#if defined(_MSC_VER) && _MSC_VER < 1300"
    BREGONIG_MEM_TEXT "${BREGONIG_MEM_TEXT}")
  file(WRITE "${BREGONIG_MINGW_MEM}" "${BREGONIG_MEM_TEXT}")
  message(STATUS "bregonig: fixed the mem_vc6.h _MSC_VER guard for the MinGW build")

  set(BREGONIG_CONFIGURE_SOURCE "${BREGONIG_MINGW_SOURCE}")
  # The product stages only bregonig.dll. Keep the GCC C++ support runtimes in
  # that image so an installed Sakura does not depend on MSYS2's bin directory.
  list(APPEND BREGONIG_CONFIGURE_OPTIONS
    "-DCMAKE_SHARED_LINKER_FLAGS=-static-libgcc -static-libstdc++"
  )
endif()

vcpkg_cmake_configure(
  SOURCE_PATH "${BREGONIG_CONFIGURE_SOURCE}"
  OPTIONS ${BREGONIG_CONFIGURE_OPTIONS}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME bregonig CONFIG_PATH share/bregonig)
vcpkg_copy_pdbs()

configure_file("${CMAKE_CURRENT_LIST_DIR}/usage" "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" COPYONLY)

file(REMOVE_RECURSE
  "${CURRENT_PACKAGES_DIR}/debug/include"
  "${CURRENT_PACKAGES_DIR}/debug/share"
)

if(NOT EXISTS "${CURRENT_PACKAGES_DIR}/include/bregexp.h")
  message(FATAL_ERROR "installed bregexp.h is missing")
endif()
if(NOT EXISTS "${CURRENT_PACKAGES_DIR}/bin/bregonig.dll")
  message(FATAL_ERROR "installed bregonig.dll is missing")
endif()
if(NOT VCPKG_TARGET_IS_MINGW AND NOT EXISTS "${CURRENT_PACKAGES_DIR}/lib/bregonig.lib")
  message(FATAL_ERROR "installed import library is missing")
endif()

file(INSTALL
  "${BREGONIG_SOURCE_DIR}/bsd_license.txt"
  DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
  RENAME copyright
)
