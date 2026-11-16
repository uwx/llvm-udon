if (USE_SYSTEM_LLVM)
  add_executable(${UDON_TOOL_TARGET} llvm-udon.cpp)
  # The driver asks the target machine for the slot count and heap image, so
  # it includes the backend's headers.
  target_include_directories(${UDON_TOOL_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Target/UDON)
  set_target_properties(${UDON_TOOL_TARGET} PROPERTIES OUTPUT_NAME llvm-udon)
  llvm_update_compile_flags(${UDON_TOOL_TARGET})
  target_link_libraries(${UDON_TOOL_TARGET} PRIVATE
    ${UDON_CODEGEN_TARGET} ${UDON_INFO_TARGET}
    LLVMAnalysis LLVMAsmParser LLVMBitReader LLVMCodeGen LLVMCore
    LLVMIRReader LLVMMC LLVMScalarOpts LLVMSupport LLVMTarget
    LLVMTransformUtils)
else()
  set(LLVM_LINK_COMPONENTS
    Analysis
    AsmParser
    BitReader
    CodeGen
    Core
    IRReader
    Linker
    MC
    ScalarOpts
    Support
    Target
    TransformUtils
    UDONCodeGen
    UDONInfo
    )

  add_llvm_tool(llvm-udon
    llvm-udon.cpp

    DEPENDS
    intrinsics_gen
    )

  target_include_directories(llvm-udon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Target/UDON)
endif()

# Copy the runtime IR beside the driver so a build tree is self-contained: the
# driver looks next to itself first, then walks up to the source tree.
# A generated file with an explicit dependency, not a POST_BUILD step: a
# POST_BUILD command only runs when the driver itself relinks, so editing the
# runtime IR would silently leave a stale copy in the build tree.
set(UDON_RUNTIME_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Target/UDON/runtime/udon_rt.ll)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/udon_rt.ll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${UDON_RUNTIME_SRC} ${CMAKE_CURRENT_BINARY_DIR}/udon_rt.ll
  DEPENDS ${UDON_RUNTIME_SRC}
  COMMENT "Copying udon_rt.ll beside llvm-udon")
add_custom_target(UDONRuntimeIR ALL
                  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/udon_rt.ll)
add_dependencies(${UDON_TOOL_TARGET} UDONRuntimeIR)
