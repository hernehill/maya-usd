set(TEST_SCRIPT_FILES
    testUsdUndoManager.py
)

if(CMAKE_UFE_V3_FEATURES_AVAILABLE)
    list(APPEND TEST_SCRIPT_FILES
        testOpUndoItemList.py
    )
    if(CMAKE_WANT_VALIDATE_UNDO_ITEM)
        list(APPEND TEST_SCRIPT_FILES
            testOpUndoItem.py
        )
    endif()
endif()

foreach(script ${TEST_SCRIPT_FILES})
    mayaUsd_get_unittest_target(target ${script})
    mayaUsd_add_test(${target}
        PYTHON_MODULE ${target}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        ENV
            "LD_LIBRARY_PATH=${ADDITIONAL_LD_LIBRARY_PATH}"
    )

    # Add a ctest label to these tests for easy filtering.
    set_property(TEST ${target} APPEND PROPERTY LABELS undo)
endforeach()
