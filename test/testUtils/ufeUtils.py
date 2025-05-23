#!/usr/bin/env python

#
# Copyright 2022 Autodesk
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

"""
    Helper functions regarding UFE that will be used throughout the test.
"""

import ufe


def getUfeGlobalSelectionList():
    """ 
        Returns the current UFE selection in a list
        Returns:
            A list(str) containing all selected UFE items
    """
    selection = []
    for item in ufe.GlobalSelection.get():
        selection.append(str(item.path().back()))
    return selection

def selectPath(path, replace=False):
    """ 
        Select a path in the UFE Global selection
        Args:
            path (ufe.Path): The UFE path to select
            replace (bool=False): Replace the selection with
                                    given UFE path
    """
    # Do this import inside this function rather than at the top of the file
    # so that the module can be imported and ufeFeatureSetVersion() can be
    # called prior to maya.standalone.initialize().
    from maya.internal.ufeSupport import ufeSelectCmd

    sceneItem = ufe.Hierarchy.createItem(path)
    if replace:
        selection = ufe.Selection()
        selection.append(sceneItem)
        ufeSelectCmd.replaceWith(selection)
    else:
        ufeSelectCmd.append(sceneItem)

def createUfeSceneItem(dagPath, sdfPath=None):
    """
    Make ufe item out of dag path and sdfpath
    """
    ufePath = ufe.PathString.path('{},{}'.format(dagPath,sdfPath) if sdfPath != None else '{}'.format(dagPath))
    ufeItem = ufe.Hierarchy.createItem(ufePath)
    return ufeItem

def createItem(ufePathOrPathString):
    '''Create a UFE scene item from a UFE path or path string.'''
    path = ufe.PathString.path(ufePathOrPathString) \
           if isinstance(ufePathOrPathString, str) else ufePathOrPathString
           
    return ufe.Hierarchy.createItem(path)

def createHierarchy(ufePathOrPathStringOrItem):
    '''Create a UFE scene item from a UFE path, path string, or scene item.'''
    item = ufePathOrPathStringOrItem if isinstance(ufePathOrPathStringOrItem, ufe.SceneItem) else createItem(ufePathOrPathStringOrItem)
    return ufe.Hierarchy.hierarchy(item)

def selectUfeItems(selectItems):
    """
    Add given UFE item or list of items to a UFE global selection list
    """
    ufeSelectionList = ufe.Selection()
    
    realListToSelect = selectItems if type(selectItems) is list else [selectItems]
    for item in realListToSelect:
        ufeSelectionList.append(item)
    
    ufe.GlobalSelection.get().replaceWith(ufeSelectionList)

def ufeFeatureSetVersion():
    '''Return the UFE feature set version taking into account rollback to using
    0 for unreleased version after we ship a UFE.
    '''

    # Examples:
    #   v2.0.0 (released version 2).
    #   v0.2.20 (unreleased preview version 2).
    major = ufe.VersionInfo.getMajorVersion()
    return ufe.VersionInfo.getMinorVersion() if major == 0 else major
