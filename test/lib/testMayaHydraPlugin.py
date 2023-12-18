#!/usr/bin/env python

#
# Copyright 2024 Autodesk
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

import unittest

import mayaUtils

import os, platform

class MayaHydraPluginCheckTestCase(unittest.TestCase):
    """
    Verify that the MayaHydra plugin(s) can be loaded.
    """

    @unittest.skipUnless(mayaUtils.mayaMajorVersion() >= 2024, 'MayaHydra requires Maya 2024 or greater.')
    def testPluginLoadable(self):
        # The mayaHydra plugin is mandatory and available in all versions.
        self.assertTrue(mayaUtils.loadPlugin('mayaHydra'))

        def mayaPluginExt():
            if platform.system() == 'Windows':
                return '.mll'
            elif platform.system() == 'Linux':
                return '.so'
            elif platform.system() == 'Darwin':
                return '.bundle'
            return ''

        # The following list of plugins may or may not be present, depending on the version of
        # Maya Hydra being used. So try and find the actual Maya plugin file and if present
        # test that it loads.
        extraPluginsToTry = ['mayaHydraSceneBrowser']

        ext = mayaPluginExt()
        plugin_paths = os.getenv('MAYA_PLUG_IN_PATH')
        if plugin_paths:
            for pluginToTry in extraPluginsToTry:
                for p in plugin_paths.split(';'):
                    pathToTry = os.path.join(os.path.normpath(p), f'{pluginToTry}{ext}')
                    if os.path.exists(pathToTry):
                        self.assertTrue(mayaUtils.loadPlugin(pluginToTry))
                        break
