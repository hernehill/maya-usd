#!/usr/bin/env mayapy
#
# Copyright 2018 Pixar
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
from maya import cmds

import os
import sys
import unittest

import fixturesUtils
import imageUtils
import mayaUtils

from pxr import Usd


class testPxrUsdMayaGLInstancerDraw(imageUtils.ImageDiffingTestCase):

    @classmethod
    def setUpClass(cls):
        # The test USD data is authored Z-up, so make sure Maya is configured
        # that way too.
        cmds.upAxis(axis='z')

        cls._testName = 'InstancerDrawTest'
        inputPath = fixturesUtils.setUpClass(
            __file__, initializeStandalone=False, loadPlugin=False)
        
        cmds.loadPlugin('pxrUsd')
        
        cls._testDir = os.path.abspath('.')
        cls._inputDir = os.path.join(inputPath, cls._testName)

        # To control where the rendered images are written, we force Maya to
        # use the test directory as the workspace.
        cmds.workspace(cls._testDir, o=True)

        # Make sure the relative-path to the USD file works by setting the current
        # directory to where that file is.
        os.chdir(cls._inputDir)

    def _WriteViewportImage(self, outputImageName, suffix, baselineSuffix = None):
        if baselineSuffix is None:
            baselineSuffix = suffix

        # Make sure the hardware renderer is available
        MAYA_RENDERER_NAME = 'mayaHardware2'
        mayaRenderers = cmds.renderer(query=True,
            namesOfAvailableRenderers=True)
        self.assertIn(MAYA_RENDERER_NAME, mayaRenderers)

        # Make it the current renderer.
        cmds.setAttr('defaultRenderGlobals.currentRenderer', MAYA_RENDERER_NAME,
            type='string')
        # Set the image format to PNG.
        cmds.setAttr('defaultRenderGlobals.imageFormat', 32)
        # Set the render mode to shaded and textured.
        cmds.setAttr('hardwareRenderingGlobals.renderMode', 4)
        # Specify the output image prefix. The path to it is built from the
        # workspace directory.
        cmds.setAttr('defaultRenderGlobals.imageFilePrefix',
            '%s_%s' % (outputImageName, suffix),
            type='string')
        # Apply the viewer's color transform to the rendered image, otherwise
        # it comes out too dark.
        cmds.setAttr("defaultColorMgtGlobals.outputTransformEnabled", 1)

        # Do the render.
        #
        # We need to render twice, once in the input directory where the input
        # test file resides and once in the test output directory.
        #
        # The first render is needed because:
        #    1. The USD file used in the assembly lives in the input directory
        #    2. It uses a relative path
        #    3. It is only resolved when the Maya node gets computed
        #    4. The Maya node gets computed only when we try to render it
        #
        # So we need to do a first compute in the input directory so that the
        # input USD file is found.
        #
        # The second render is needed so that the output file is found in
        # the directory the test expects.

        # Make sure the relative-path input assembly USD file is found.
        cmds.ogsRender(camera="persp", currentFrame=True, width=960, height=540)

        # Make sure the image is written in the test output folder.
        os.chdir(self._testDir)
        cmds.ogsRender(camera="persp", currentFrame=True, width=960, height=540)

        baselineImageName = '%s_%s.png' % (outputImageName, baselineSuffix)
        outputImageName = '%s_%s.png' % (outputImageName, suffix)

        baselineImagePath = os.path.join(self._inputDir, 'baseline', baselineImageName)
        outputImagePath = os.path.join('.', 'tmp', outputImageName)

        self.assertImagesClose(baselineImagePath, outputImagePath)

        # Make sure the relative-path to the USD file works by setting the current
        # directory to where that file is.
        os.chdir(self._inputDir)

    def _SetModelPanelsToViewport2(self):
        modelPanels = cmds.getPanel(type='modelPanel')
        for modelPanel in modelPanels:
            cmds.modelEditor(modelPanel, edit=True, rendererName='vp2Renderer')

    def testGenerateImages(self):
        cmds.file(os.path.abspath('InstancerDrawTest.ma'),
                open=True, force=True)

        # The cards rendering colors in older versions of Maya is lighter,
        mayaSuffix = ''
        if mayaUtils.mayaMajorVersion() <= 2024:
            mayaSuffix = '_v1'

        # Cards rendering became lighter with a change to lighting computations
        # in Storm in USD versions beyond 24.08
        usdSuffix = ''
        if Usd.GetVersion() <= (0, 24, 8):
            usdSuffix = '_legacyUsd'

        # Draw in VP2 at current frame.
        self._SetModelPanelsToViewport2()
        self._WriteViewportImage("InstancerTest", "initial")

        # Load assembly in "Cards" to use cards drawmode.
        cmds.assembly("CubeModel", edit=True, active="Cards")
        self._WriteViewportImage(
            "InstancerTest", "cards" + mayaSuffix + usdSuffix)

        # Change the time; this will change the instancer positions.
        cmds.currentTime(50, edit=True)
        self._WriteViewportImage(
            "InstancerTest", "frame50" + mayaSuffix + usdSuffix)

        # Delete the first instance.
        # Tickle the second instance so that it draws.
        # Note: it's OK that we need to tickle; the instancing support is not
        # the greatest and we're just checking that it doesn't break horribly.
        cmds.delete("instance1")
        cmds.setAttr("instance2.t", 15, 0, 0, type="double3")
        self._WriteViewportImage(
            "InstancerTest", "instance2" + mayaSuffix + usdSuffix)

        # Delete the second instance.
        cmds.delete("instance2")
        self._WriteViewportImage("InstancerTest", "empty")

        # Reload the scene. We should see the instancer come back up.
        # Hopefully the instancer imager has reloaded in a sane way!
        cmds.file(os.path.abspath('InstancerDrawTest.ma'),
                open=True, force=True)
        self._SetModelPanelsToViewport2()
        self._WriteViewportImage("InstancerTest", "reload", "initial")


if __name__ == '__main__':
    fixturesUtils.runTests(globals())
