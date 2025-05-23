# -----------------------------------------------------------------------------
# sources
# -----------------------------------------------------------------------------
target_sources(${PROJECT_NAME}
    PRIVATE
        moduleDeps.cpp
)

# Python extension library for mayaUsdAPI library bindings. Pixar's TF_WRAP_MODULE
# macro expects the library name to be prefixed with _, but adds an underscore
# to the package name when creating the Python extension module initialization
# function (see Pixar's pyModule.h).
if(IS_WINDOWS AND MAYAUSD_DEFINE_BOOST_DEBUG_PYTHON_FLAG)
    # On Windows when compiling with debug python the library must be named with _d.
    set(PYTHON_TARGET_NAME _${PROJECT_NAME}_d)
else()
    set(PYTHON_TARGET_NAME _${PROJECT_NAME})
endif()

add_library(${PYTHON_TARGET_NAME} SHARED)

# -----------------------------------------------------------------------------
# sources
# -----------------------------------------------------------------------------
target_sources(${PYTHON_TARGET_NAME} 
    PRIVATE
        module.cpp
        wrapProxyStage.cpp
        wrapProxyShapeNotice.cpp
)

# -----------------------------------------------------------------------------
# compiler configuration
# -----------------------------------------------------------------------------
target_compile_definitions(${PYTHON_TARGET_NAME}
    PRIVATE
        $<$<BOOL:${IS_MACOSX}>:OSMac_>
        MFB_PACKAGE_NAME=${PROJECT_NAME}
        MFB_ALT_PACKAGE_NAME=${PROJECT_NAME}
        MFB_PACKAGE_MODULE=${PROJECT_NAME}
)

mayaUsd_compile_config(${PYTHON_TARGET_NAME})

# -----------------------------------------------------------------------------
# include directories
# -----------------------------------------------------------------------------
target_include_directories(${PROJECT_NAME} 
    PUBLIC
        ${PXR_INCLUDE_DIRS}
        ${MAYA_INCLUDE_DIRS}
        ${UFE_INCLUDE_DIR}
        ${CMAKE_BINARY_DIR}/include
)

# -----------------------------------------------------------------------------
# link libraries
# -----------------------------------------------------------------------------
target_link_libraries(${PYTHON_TARGET_NAME}
    PRIVATE
        ${PROJECT_NAME}
        sdf
        mayaUsd
)

# -----------------------------------------------------------------------------
# properties
# -----------------------------------------------------------------------------
set_python_module_property(${PYTHON_TARGET_NAME})

# -----------------------------------------------------------------------------
# run-time search paths
# -----------------------------------------------------------------------------
if(IS_MACOSX OR IS_LINUX)
    mayaUsd_init_rpath(rpath "lib")
    mayaUsd_add_rpath(rpath "../../..")
    if(IS_MACOSX AND DEFINED MAYAUSD_TO_USD_RELATIVE_PATH)
        mayaUsd_add_rpath(rpath "../../../../../../Maya.app/Contents/MacOS")
        mayaUsd_add_rpath(rpath "../../../../${MAYAUSD_TO_USD_RELATIVE_PATH}/lib")
    endif()
    mayaUsd_install_rpath(rpath ${PYTHON_TARGET_NAME})
endif()

# -----------------------------------------------------------------------------
# install
# -----------------------------------------------------------------------------
# We will use the following Python package structure:
#
# mayaUsdAPI: top-level package
# mayaUsdAPI.lib: Python wrappers for mayaUsdAPI library functionality.
#
# We re-map the Python library namespace to mayaUsdAPI.lib through a simple
# __init__.py.
set(PYLIB_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX}/lib/python/${PROJECT_NAME}/lib)

install(TARGETS ${PYTHON_TARGET_NAME}
    LIBRARY
    DESTINATION ${PYLIB_INSTALL_PREFIX}
    ARCHIVE
    DESTINATION ${PYLIB_INSTALL_PREFIX}
    RUNTIME
    DESTINATION ${PYLIB_INSTALL_PREFIX}
)

if(IS_WINDOWS)
    install(FILES $<TARGET_PDB_FILE:${PYTHON_TARGET_NAME}> 
            DESTINATION ${PYLIB_INSTALL_PREFIX} OPTIONAL)
endif()

install(FILES __init__.py DESTINATION ${PYLIB_INSTALL_PREFIX})
