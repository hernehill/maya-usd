# ---------------------------------------------------------------------------------------------
# install
# ---------------------------------------------------------------------------------------------

# In Python 2 we need an __init__.py file in the ufe_ae folder to declare it as a module.
# In Python 3 we must not use an __init__.py file in that folder to enable the "Implicit Namespace
# Packages" feature as defined in Python PEP 420, which allows other plugins to also be able to
# declare an ufe_ae module that would be merged with this one. See https://peps.python.org/pep-0420/
# for details.
if (NOT BUILD_WITH_PYTHON_3)
install(FILES __init__.py DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/python/ufe_ae)
endif()
# We need an empty __init__.py file in each subfolder so it is considered a python module.
install(FILES __init__.py DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/python/ufe_ae/usd)
install(FILES ae_utils.py DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/python/ufe_ae/usd)
install(FILES __init__.py DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/python/ufe_ae/usd/nodes)

# Maya Attribute Editor python template - just uses a single schema base template file.
set(MAYAUSD_AE_TEMPLATES usdschemabase)

foreach(_SUBDIR ${MAYAUSD_AE_TEMPLATES})
    install(FILES
        ${_SUBDIR}/__init__.py
        ${_SUBDIR}/ae_template.py
        ${_SUBDIR}/aeUITemplate.py
        ${_SUBDIR}/arrayCustomControl.py
        ${_SUBDIR}/attributeCustomControl.py
        ${_SUBDIR}/connectionsCustomControl.py
        ${_SUBDIR}/displayCustomControl.py
        ${_SUBDIR}/enumCustomControl.py
        ${_SUBDIR}/imageCustomControl.py
        ${_SUBDIR}/materialCustomControl.py
        ${_SUBDIR}/metadataCustomControl.py
        ${_SUBDIR}/observers.py
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/python/ufe_ae/usd/nodes/${_SUBDIR}
    )
endforeach()

install(FILES __init__.py DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/python/${PROJECT_NAME})

# Install shared components

if(MAYA_APP_VERSION VERSION_GREATER_EQUAL 2023)
    foreach(_SUBDIR ${MAYAUSD_AE_TEMPLATES})
        install(FILES
            ${_SUBDIR}/lightCustomControl.py
            DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/python/ufe_ae/usd/nodes/${_SUBDIR}
        )
    endforeach()

    set(MAYAUSD_SHARED_COMPONENTS usd-shared-components/src/python/usdSharedComponents)
    install(FILES
        ${MAYAUSD_SHARED_COMPONENTS}/collection/__init__.py
        ${MAYAUSD_SHARED_COMPONENTS}/collection/widget.py
        ${MAYAUSD_SHARED_COMPONENTS}/collection/includeExcludeWidget.py
        ${MAYAUSD_SHARED_COMPONENTS}/collection/expressionWidget.py
        ${MAYAUSD_SHARED_COMPONENTS}/collection/expressionRulesMenu.py
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/python/usd_shared_components/collection/
    )

    install(FILES
        ${MAYAUSD_SHARED_COMPONENTS}/common/__init__.py
        ${MAYAUSD_SHARED_COMPONENTS}/common/filteredStringListModel.py
        ${MAYAUSD_SHARED_COMPONENTS}/common/filteredStringListView.py
        ${MAYAUSD_SHARED_COMPONENTS}/common/list.py
        ${MAYAUSD_SHARED_COMPONENTS}/common/persistentStorage.py
        ${MAYAUSD_SHARED_COMPONENTS}/common/resizable.py
        ${MAYAUSD_SHARED_COMPONENTS}/common/theme.py
        ${MAYAUSD_SHARED_COMPONENTS}/common/menuButton.py
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/python/usd_shared_components/common/
    )

    set(LIB_ICONS
        ${MAYAUSD_SHARED_COMPONENTS}/icons/dark/menu
    )
    foreach(ICON_BASE ${LIB_ICONS})
        # The _100.png files need to be installed without the _100. This is the
        # base icon name that is used. Maya will automatically choose the _150/_200
        # image if neeeded.
        install(FILES "${ICON_BASE}_100.png"
            DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/python/usd_shared_components/icons/dark"
            RENAME "${ICON_BASE}.png"
        )
        install(FILES "${ICON_BASE}_150.png" "${ICON_BASE}_200.png"
            DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/python/usd_shared_components/icons/dark"
        )
    endforeach()

endif()