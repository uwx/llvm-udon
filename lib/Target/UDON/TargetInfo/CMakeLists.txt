if (USE_SYSTEM_LLVM)
  add_library(${UDON_INFO_TARGET} STATIC UDONTargetInfo.cpp)
  set_target_properties(${UDON_INFO_TARGET}
    PROPERTIES OUTPUT_NAME LLVMUDONInfo)
  target_link_libraries(${UDON_INFO_TARGET} PRIVATE LLVMMC LLVMSupport)
else()
  add_llvm_component_library(LLVMUDONInfo
    UDONTargetInfo.cpp

    LINK_COMPONENTS
    MC
    Support

    ADD_TO_COMPONENT
    UDON
    )
endif()
