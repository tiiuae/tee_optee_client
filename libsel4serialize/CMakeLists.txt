project (libsel4serialize C CXX ASM)

set (SRC sel4_serializer.c)

add_library (${PROJECT_NAME} STATIC ${SRC})

target_compile_definitions (${PROJECT_NAME}
	PRIVATE -D_GNU_SOURCE
	PRIVATE -DBINARY_PREFIX="LT"
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries (${PROJECT_NAME}
	PRIVATE		optee-client-headers
	PUBLIC		sel4-tool-lib
)

install (TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
