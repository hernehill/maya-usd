list(APPEND scripts_src
    mayaUsdTranslatorImport.mel
    mayaUsdTranslatorExport.mel
    mayaUsd_exportHelpers.py
    mayaUSDRegisterStrings.mel
    mayaUSDRegisterStrings.py
    AEmayaUsdProxyShapeBaseTemplate.mel
    AEmayaUsdProxyShapeTemplate.mel
    AETemplateHelpers.py
    mayaUsdMenu.mel
    mayaUsdMenu.py
    mayaUsd_createStageFromFile.mel
    mayaUsd_createStageWithNewLayer.py
    mayaUsd_createStageFromAsset.mel
    mayaUsd_imageFileDialogs.mel
    mayaUsd_layerEditorFileDialogs.mel
    mayaUsd_fileOptions.mel
    mayaUsd_pluginUICreation.mel
    mayaUsd_pluginUIDeletion.mel
    mayaUsd_pluginBatchLoad.mel
    mayaUsd_pluginBatchUnload.mel
    mayaUsd_preferenceTab.mel
    mayaUsd_selectionUtils.py
    mayaUsd_USDRootFileRelative.py
    usdFileSaveOptions.mel
    USDMenuProc.mel
)

install(FILES ${scripts_src}
    DESTINATION ${INSTALL_DIR_SUFFIX}/scripts
)

set(PREVIEWSURFACE_MAYA_TYPE_NAME "usdPreviewSurface")

configure_file("${PROJECT_SOURCE_DIR}/lib/usd/pxrUsdPreviewSurface/AEusdPreviewSurfaceTemplate.mel"
               "${CMAKE_CURRENT_BINARY_DIR}/AEusdPreviewSurfaceTemplate.mel"
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/AEusdPreviewSurfaceTemplate.mel
    DESTINATION ${INSTALL_DIR_SUFFIX}/scripts
)
