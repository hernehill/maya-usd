list(APPEND scripts_src
    mayaUsdLibRegisterStrings.mel
    mayaUsdLibRegisterStrings.py
    mayaUsdAddMayaReference.mel
    mayaUsdAddMayaReference.py
    mayaUsdAddUSDReference.mel
    mayaUsdCacheMayaReference.mel
    mayaUsdCacheMayaReference.py
    mayaUsdClearRefsOrPayloadsOptions.mel
    mayaUsdClearRefsOrPayloadsOptions.py
    mayaUsdDuplicateAsUsdDataOptions.mel
    mayaUsdDuplicateAsUsdDataOptions.py
    mayaUsdMergeToUSDOptions.mel
    mayaUsdMergeToUSDOptions.py
    mayaUsdMergeToUsd.py
    mayaUsdOptions.py
    mayaUsdStageConversion.py
    mayaUsdUtils.py
    mayaUsdMayaReferenceUtils.py
)

list(APPEND maya_lib_scripts_src
    cacheToUsd.py
)

install(FILES ${scripts_src}
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/scripts
)

set(PYTHON_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX}/lib/python/${PROJECT_NAME}/lib)
install(FILES ${maya_lib_scripts_src} DESTINATION ${PYTHON_INSTALL_PREFIX})
