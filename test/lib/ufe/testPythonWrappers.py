#!/usr/bin/env python

#
# Copyright 2021 Autodesk
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

import fixturesUtils
import mayaUtils
import ufeUtils
import usdUtils

import mayaUsd

from maya import cmds
from maya import standalone
from pxr import Usd

import ufe

import unittest


class PythonWrappersTestCase(unittest.TestCase):
    '''Test the maya-usd ufe python wrappers.
    '''

    pluginsLoaded = False

    @classmethod
    def setUpClass(cls):
        fixturesUtils.readOnlySetUpClass(__file__, loadPlugin=False)

        if not cls.pluginsLoaded:
            cls.pluginsLoaded = mayaUtils.isMayaUsdPluginLoaded()

    @classmethod
    def tearDownClass(cls):
        standalone.uninitialize()

    def setUp(self):
        ''' Called initially to set up the Maya test environment '''
        self.assertTrue(self.pluginsLoaded)

    def testWrappers(self):

        ''' Verify the python wrappers.'''
        cmds.file(new=True, force=True)

        # Create empty stage and add a prim.
        import mayaUsd_createStageWithNewLayer
        proxyShape = mayaUsd_createStageWithNewLayer.createStageWithNewLayer()
        proxyShapePath = ufe.Path(mayaUtils.createUfePathSegment(proxyShape))

        # Test maya-usd getStage() wrapper.
        mayaUsdStage = mayaUsd.ufe.getStage(ufe.PathString.string(proxyShapePath))
        self.assertIsNotNone(mayaUsdStage)

        # Verify that this the stage returned from maya-usd wrapper
        # is same as one from Pixar wrapper.
        shapeNode = cmds.ls(sl=True, l=True)[0]
        pixarStage = mayaUsd.lib.GetPrim(shapeNode).GetStage()
        self.assertIsNotNone(pixarStage)
        self.assertIs(mayaUsdStage, pixarStage)

        # Test maya-usd stagePath() wrapper.
        stagePath = mayaUsd.ufe.stagePath(mayaUsdStage)
        self.assertIsNotNone(stagePath)

        # It should also be the same as the shape node string.
        self.assertEqual(shapeNode, stagePath)

        # Test the maya-usd ufePathToPrim() wrapper.
        mayaUsdStage.DefinePrim("/Capsule1", "Capsule")
        capsulePrim = mayaUsd.ufe.ufePathToPrim('%s,/Capsule1' % proxyShape)
        self.assertIsNotNone(capsulePrim)

        # Test maya-usd usdPathToUfePathSegment wrapper.
        ufePath = mayaUsd.ufe.usdPathToUfePathSegment('/Capsule1')
        self.assertEqual(ufePath, str(usdUtils.createUfePathSegment('/Capsule1')))

        # Test the uniqueName wrapper.
        # Note: param1 = list of existing children names.
        #       param2 = source name that you want made unique
        #       return = unique name from input source name
        newName = mayaUsd.ufe.uniqueName([], 'Capsule')
        self.assertEqual(newName, 'Capsule1')
        newName = mayaUsd.ufe.uniqueName([], 'Capsule1')
        self.assertEqual(newName, 'Capsule2')
        newName = mayaUsd.ufe.uniqueName(['Cone1'], 'Capsule1')
        self.assertEqual(newName, 'Capsule2')
        newName = mayaUsd.ufe.uniqueName(['Capsule1'], 'Capsule1')
        self.assertEqual(newName, 'Capsule2')
        newName = mayaUsd.ufe.uniqueName(['Capsule1', 'Capsule5'], 'Capsule1')
        self.assertEqual(newName, 'Capsule2')
        newName = mayaUsd.ufe.uniqueName(['Capsule001'], 'Capsule001')
        self.assertEqual(newName, 'Capsule002')
        newName = mayaUsd.ufe.uniqueName(['Capsule001', 'Capsule002'], 'Capsule001')
        self.assertEqual(newName, 'Capsule003')
        newName = mayaUsd.ufe.uniqueName(['Capsule001', 'Capsule005'], 'Capsule001')
        self.assertEqual(newName, 'Capsule002')

        # Test uniqueChildName wrapper.
        newName = mayaUsd.ufe.uniqueChildName(capsulePrim, 'Cone1')
        self.assertEqual(newName, 'Cone1')
        mayaUsdStage.DefinePrim("/Capsule1/Cone1", "Cone")
        newName = mayaUsd.ufe.uniqueChildName(capsulePrim, 'Cone1')
        self.assertEqual(newName, 'Cone2')
        mayaUsdStage.DefinePrim("/Capsule1/Sphere001", "Sphere")
        newName = mayaUsd.ufe.uniqueChildName(capsulePrim, 'Sphere001')
        self.assertEqual(newName, 'Sphere002')

        # stripInstanceIndexFromUfePath/ufePathToInstanceIndex wrappers are tested
        # by testPointInstances.

        # isEditTargetLayerModifiable wrapper is tested by testBlockedLayerEdit.

        # Test the getTime wrapper.
        cmds.currentTime(10)
        t = mayaUsd.ufe.getTime(stagePath)
        self.assertEqual(t, Usd.TimeCode(10))

        # isAttributeEditAllowed wrapper is tested by testAttribute.

        # Test the maya-usd getPrimFromRawItem() wrapper.
        capsulePath = proxyShapePath + usdUtils.createUfePathSegment('/Capsule1')
        capsuleItem = ufe.Hierarchy.createItem(capsulePath)
        rawItem = capsuleItem.getRawAddress()
        capsulePrim2 = mayaUsd.ufe.getPrimFromRawItem(rawItem)
        self.assertIsNotNone(capsulePrim2)
        self.assertEqual(capsulePrim, capsulePrim2)

        # Test the maya-usd getNodeTypeFromRawItem() wrapper.
        nodeType = mayaUsd.ufe.getNodeTypeFromRawItem(rawItem)
        self.assertIsNotNone(nodeType)

        # Test the maya-usd getNodeNameFromRawItem() wrapper.
        nodeName = mayaUsd.ufe.getNodeNameFromRawItem(rawItem)
        self.assertIsNotNone(nodeName)

        # Test the maya-usd runtime id wrappers.
        mayaRtid = mayaUsd.ufe.getMayaRunTimeId()
        usdRtid = mayaUsd.ufe.getUsdRunTimeId()
        self.assertTrue(mayaRtid > 0)
        self.assertTrue(usdRtid > 0)
        self.assertNotEqual(mayaRtid, usdRtid)

        # Added this File|New to command to fix a crash when this test
        # exits (after upgrading to USD v21.08). The crash is coming
        # from USD when it tries to destroy the layer data by creating
        # a worker thread.
        cmds.file(new=True, force=True)

    # In Maya 2022, undo does not restore the stage.  To be
    # investigated as needed.
    @unittest.skipUnless(mayaUtils.mayaMajorVersion() == 2023, 'Only supported in Maya 2023 or greater.')
    def testGetAllStages(self):
        cmds.file(new=True, force=True)

        # Create two stages.
        import mayaUsd_createStageWithNewLayer
        proxyShape1 = mayaUsd_createStageWithNewLayer.createStageWithNewLayer()
        proxyShape2 = mayaUsd_createStageWithNewLayer.createStageWithNewLayer()

        # Test maya-usd getAllStages() wrapper.
        self.assertEqual(len(mayaUsd.ufe.getAllStages()), 2)

        # Delete one of the stages.
        cmds.delete(proxyShape1)

        self.assertEqual(len(mayaUsd.ufe.getAllStages()), 1)

        cmds.undo()

        self.assertEqual(len(mayaUsd.ufe.getAllStages()), 2)

        cmds.redo()

        self.assertEqual(len(mayaUsd.ufe.getAllStages()), 1)

    @unittest.skipUnless(ufeUtils.ufeFeatureSetVersion() >= 4, 'Test for UFE v4 or later')
    def testCreateStageWithNewLayerBinding(self):
        cmds.file(new=True, force=True)

        def verifyProxyShape(proxyShapePathString):
            # Verify that we got a proxy shape object.
            nodeType = cmds.nodeType(proxyShapePathString)
            self.assertEqual('mayaUsdProxyShape', nodeType)
            
            # Verify that the shape node is connected to time.
            self.assertTrue(cmds.isConnected('time1.outTime', proxyShapePathString+'.time'))

        # Create a proxy shape under the world node.
        proxy1PathString = mayaUsd.ufe.createStageWithNewLayer('')
        self.assertEqual('|stage1|stageShape1', proxy1PathString)
        verifyProxyShape(proxy1PathString)
        self.assertEqual(len(mayaUsd.ufe.getAllStages()), 1)
        cmds.undo()
        self.assertEqual(len(mayaUsd.ufe.getAllStages()), 0)
        cmds.redo()
        self.assertEqual(len(mayaUsd.ufe.getAllStages()), 1)

        # Create a proxy shape under a transform.
        cmds.createNode('transform', skipSelect=True, name='transform1')
        transformPathString = '|transform1'
        proxy2PathString = mayaUsd.ufe.createStageWithNewLayer(transformPathString)
        self.assertEqual('|transform1|stage1|stageShape1', proxy2PathString)
        verifyProxyShape(proxy2PathString)
        self.assertEqual(len(mayaUsd.ufe.getAllStages()), 2)
        cmds.undo()
        self.assertEqual(len(mayaUsd.ufe.getAllStages()), 1)
        cmds.redo()
        self.assertEqual(len(mayaUsd.ufe.getAllStages()), 2)

if __name__ == '__main__':
    unittest.main(verbosity=2)
