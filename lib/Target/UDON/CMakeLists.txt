include_directories(${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

# The component group has to exist before TargetInfo adds a library to it.
if (NOT USE_SYSTEM_LLVM)
  add_llvm_component_group(UDON)
endif()

add_subdirectory(TargetInfo)

# Grows as the lowering lands: the memory, call, runtime, call-graph, slot
# allocation and extern-table units are added by the steps that implement them.
set(UDON_WRITER_SOURCES
  UDONInliner.cpp
  UDONProtectFPConv.cpp
  UDONLowerOverflow.cpp
  UDONWriter.cpp
  UDONWriterSelect.cpp
  UDONWriterMemory.cpp
  UDONWriterCalls.cpp
  UDONWriterIntrinsics.cpp
  UDONWriterRuntime.cpp
  UDONCallGraph.cpp
  UDONTypedArrays.cpp)

# A standalone build links against an LLVM whose export file may already define
# LLVMUDONCodeGen (when that LLVM was itself built with this project in-tree),
# so the target gets a suffix while the artifact keeps its name.
if (USE_SYSTEM_LLVM)
  add_library(${UDON_CODEGEN_TARGET} STATIC UDONTargetMachine.cpp
              ${UDON_WRITER_SOURCES})
  set_target_properties(${UDON_CODEGEN_TARGET}
    PROPERTIES OUTPUT_NAME LLVMUDONCodeGen)
  # add_library does not apply LLVM's -fno-rtti/-fno-exceptions the way
  # add_llvm_* does; without them the vtables reference typeinfo that an
  # RTTI-off LLVM does not export (and /EHs-c- and _HAS_EXCEPTIONS=0 are the
  # MSVC half of the same setting).
  llvm_update_compile_flags(${UDON_CODEGEN_TARGET})
  target_link_libraries(${UDON_CODEGEN_TARGET} PRIVATE
    LLVMAsmParser LLVMCodeGen LLVMCore LLVMDemangle LLVMIRReader
    LLVMLinker ${UDON_INFO_TARGET} LLVMScalarOpts LLVMSupport LLVMTarget
    LLVMTransformUtils)
else()
  # add_llvm_target assumes the in-tree layout with LLVMBuild.txt-style target
  # registration; this target is built as a component library instead.
  add_llvm_component_library(LLVMUDONCodeGen
    UDONTargetMachine.cpp
    ${UDON_WRITER_SOURCES}

    LINK_COMPONENTS
    AsmParser
    CodeGen
    Core
    Demangle
    IRReader
    Linker
    UDONInfo
    ScalarOpts
    Support
    Target
    TransformUtils

    ADD_TO_COMPONENT
    UDON
    )
endif()
