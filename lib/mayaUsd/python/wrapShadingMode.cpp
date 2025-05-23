//
// Copyright 2021 Autodesk
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <mayaUsd/fileio/shading/shadingModeImporter.h>
#include <mayaUsd/fileio/shading/shadingModeRegistry.h>

#include <pxr_python.h>

PXR_NAMESPACE_USING_DIRECTIVE

class ShadingModeRegistry
{
public:
    static void RegisterImportConversion(
        const TfToken& materialConversion,
        const TfToken& renderContext,
        const TfToken& niceName,
        const TfToken& description)
    {
        UsdMayaShadingModeRegistry::GetInstance().RegisterImportConversion(
            materialConversion, renderContext, niceName, description);
    }

    static void RegisterExportConversion(
        const TfToken& materialConversion,
        const TfToken& renderContext,
        const TfToken& niceName,
        const TfToken& description)
    {
        UsdMayaShadingModeRegistry::GetInstance().RegisterExportConversion(
            materialConversion, renderContext, niceName, description);
    }
};

//----------------------------------------------------------------------------------------------------------------------
void wrapShadingModeImportContext()
{
    PXR_BOOST_PYTHON_NAMESPACE::class_<UsdMayaShadingModeImportContext>(
        "ShadingModeImportContext", PXR_BOOST_PYTHON_NAMESPACE::no_init)
        .def("GetCreatedObject", &UsdMayaShadingModeImportContext::GetCreatedObject)
        //        .def("AddCreatedObject",&UsdMayaShadingModeImportContext::AddCreatedObject) //
        //        overloads
        .def("CreateShadingEngine", &UsdMayaShadingModeImportContext::CreateShadingEngine)
        .def("GetShadingEngineName", &UsdMayaShadingModeImportContext::GetShadingEngineName)
        .def("GetSurfaceShaderPlugName", &UsdMayaShadingModeImportContext::GetSurfaceShaderPlugName)
        .def("GetVolumeShaderPlugName", &UsdMayaShadingModeImportContext::GetVolumeShaderPlugName)
        .def(
            "GetDisplacementShaderPlugName",
            &UsdMayaShadingModeImportContext::GetDisplacementShaderPlugName)
        .def("SetSurfaceShaderPlugName", &UsdMayaShadingModeImportContext::SetSurfaceShaderPlugName)
        .def("SetVolumeShaderPlugName", &UsdMayaShadingModeImportContext::SetVolumeShaderPlugName)
        .def(
            "SetDisplacementShaderPlugName",
            &UsdMayaShadingModeImportContext::SetDisplacementShaderPlugName)
        .def(
            "GetPrimReaderContext",
            &UsdMayaShadingModeImportContext::GetPrimReaderContext,
            PXR_BOOST_PYTHON_NAMESPACE::return_value_policy<
                PXR_BOOST_PYTHON_NAMESPACE::return_by_value>());
}

//----------------------------------------------------------------------------------------------------------------------
void wrapShadingMode()
{
    PXR_BOOST_PYTHON_NAMESPACE::class_<ShadingModeRegistry>(
        "ShadingModeRegistry", PXR_BOOST_PYTHON_NAMESPACE::no_init)
        .def("RegisterImportConversion", &ShadingModeRegistry::RegisterImportConversion)
        .staticmethod("RegisterImportConversion")
        .def("RegisterExportConversion", &ShadingModeRegistry::RegisterExportConversion)
        .staticmethod("RegisterExportConversion");
}
