llvm_canonicalize_cmake_booleans(BUILD_SHARED_LIBS)

set(LLVM_UDON_TEST_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# The harness is a .NET project, so the lit suite points at its built assembly
# and runs it through `dotnet`. A published single-file binary would work too;
# this keeps one build of the harness for both lit and the Unity importer.
find_program(UDON_DOTNET NAMES dotnet dotnet.exe)
if (UDON_DOTNET)
  set(UDON_VM "${UDON_DOTNET} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/Udon.VM/bin/Debug/net9.0/udon-vm.dll")
else()
  set(UDON_VM "udon-vm")
endif()

find_program(UDON_FILECHECK NAMES FileCheck FileCheck.exe
             HINTS "${LLVM_TOOLS_BINARY_DIR}")
if (NOT UDON_FILECHECK)
  set(UDON_FILECHECK "FileCheck")
endif()

configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
  PATHS
  "LLVM_TOOLS_DIR"
  )

add_lit_testsuite(check-udon "Running the llvm-udon regression tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS llvm-udon
  )
set_target_properties(check-udon PROPERTIES FOLDER "UDON/Meta")
