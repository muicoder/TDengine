aux_source_directory(src MNODE_SRC)
IF (TD_PRIVILEGE)
  ADD_DEFINITIONS(-D_PRIVILEGE)
ENDIF ()
IF (TD_ENTERPRISE)
  LIST(APPEND MNODE_SRC ${TD_ENTERPRISE_DIR}/src/plugins/privilege/src/privilege.c)
  LIST(APPEND MNODE_SRC ${TD_ENTERPRISE_DIR}/src/plugins/mnode/src/mndDb.c)
  LIST(APPEND MNODE_SRC ${TD_ENTERPRISE_DIR}/src/plugins/mnode/src/mndVgroup.c)
ENDIF ()

add_library(mnode STATIC ${MNODE_SRC})
target_include_directories(
    mnode
    PUBLIC "${TD_SOURCE_DIR}/include/dnode/mnode"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/inc"
)
target_link_libraries(
    mnode scheduler sdb wal transport cjson sync monitor executor qworker stream parser
)

IF (TD_GRANT)
  TARGET_LINK_LIBRARIES(mnode grant)
  ADD_DEFINITIONS(-D_GRANT)
ENDIF ()

if(${BUILD_TEST})
    add_subdirectory(test)
endif(${BUILD_TEST})
