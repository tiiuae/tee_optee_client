project (optee-client-headers C)

################################################################################
# Header files to be exported
################################################################################
FILE(GLOB INSTALL_HEADERS "*.h")

################################################################################
# Built library
################################################################################
add_library(${PROJECT_NAME} INTERFACE)

################################################################################
# Public and private header and library dependencies
################################################################################
target_include_directories(${PROJECT_NAME} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_include_directories(teec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

################################################################################
# Install targets
################################################################################
install (FILES ${INSTALL_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
