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

#include "pythonObjectRegistry.h"
#include "wrapSparseValueWriter.h"

#include <mayaUsd/fileio/primWriter.h>
#include <mayaUsd/fileio/primWriterRegistry.h>
#include <mayaUsd/fileio/registryHelper.h>
#include <mayaUsd/fileio/shaderWriter.h>
#include <mayaUsd/fileio/shaderWriterRegistry.h>
#include <mayaUsd/fileio/shading/symmetricShaderWriter.h>

#include <pxr/base/tf/pyContainerConversions.h>
#include <pxr/base/tf/pyEnum.h>
#include <pxr/base/tf/pyPolymorphic.h>
#include <pxr/base/tf/pyResultConversions.h>
#include <pxr_python.h>

PXR_NAMESPACE_USING_DIRECTIVE

//----------------------------------------------------------------------------------------------------------------------
/// \brief  boost python binding for the UsdMayaPrimWriter
//----------------------------------------------------------------------------------------------------------------------
template <typename T = UsdMayaPrimWriter>
class PrimWriterWrapper
    : public T
    , public TfPyPolymorphic<T>
{
public:
    typedef PrimWriterWrapper This;
    typedef T                 base_t;

    PrimWriterWrapper(
        const MFnDependencyNode& depNodeFn,
        const SdfPath&           usdPath,
        UsdMayaWriteJobContext&  jobCtx)
        : T(depNodeFn, usdPath, jobCtx)
    {
    }

    static std::shared_ptr<This> New(uintptr_t createdWrapper)
    {
        return *((std::shared_ptr<This>*)createdWrapper);
    }

    virtual ~PrimWriterWrapper() = default;

    const UsdStage& GetUsdStage() const { return *get_pointer(base_t::GetUsdStage()); }

    // This is the pattern inspired from USD/pxr/base/tf/wrapTestTfPython.cpp
    // It would have been simpler to call 'this->CallVirtual<>("Write",
    // &base_t::default_Write)(usdTime);' But it is not allowed Instead of having to create a
    // function 'default_Write(...)' to call the base class when there is no Python implementation.
    void default_Write(const UsdTimeCode& usdTime) { base_t::Write(usdTime); }
    void Write(const UsdTimeCode& usdTime) override
    {
        this->template CallVirtual<>("Write", &This::default_Write)(usdTime);
    }

    void default_PostExport() { base_t::PostExport(); }
    void PostExport() override
    {
        this->template CallVirtual<>("PostExport", &This::default_PostExport)();
    }

    bool default_ExportsGprims() const { return base_t::ExportsGprims(); }
    bool ExportsGprims() const override
    {
        return this->template CallVirtual<bool>("ExportsGprims", &This::default_ExportsGprims)();
    }

    bool default_ShouldPruneChildren() const { return base_t::ShouldPruneChildren(); };
    bool ShouldPruneChildren() const override
    {
        return this->template CallVirtual<bool>(
            "ShouldPruneChildren", &This::default_ShouldPruneChildren)();
    }

    bool default__HasAnimCurves() const { return base_t::_HasAnimCurves(); };
    bool _HasAnimCurves() const override
    {
        return this->template CallVirtual<bool>("_HasAnimCurves", &This::default__HasAnimCurves)();
    }

    const SdfPathVector& default_GetModelPaths() const { return base_t::GetModelPaths(); }
    const SdfPathVector& GetModelPaths() const override
    {
        if (TfPyOverride o = this->GetOverride("GetModelPaths")) {
            auto res = std::function<PXR_BOOST_PYTHON_NAMESPACE::object()>(
                TfPyCall<PXR_BOOST_PYTHON_NAMESPACE::object>(o))();
            if (res) {
                const_cast<SdfPathVector*>(&_modelPaths)->clear();
                TfPyLock pyLock;

                PXR_BOOST_PYTHON_NAMESPACE::list keys(res);
                for (int i = 0; i < len(keys); ++i) {
                    PXR_BOOST_PYTHON_NAMESPACE::extract<SdfPath> extractedKey(keys[i]);
                    if (!extractedKey.check()) {
                        TF_CODING_ERROR(
                            "PrimWriterWrapper.GetModelPaths: SdfPath key expected, not "
                            "found!");
                        return _modelPaths;
                    }

                    SdfPath path = extractedKey;
                    const_cast<SdfPathVector*>(&_modelPaths)->push_back(path);
                }
                return _modelPaths;
            }
        }
        return This::default_GetModelPaths();
    }

    const UsdMayaUtil::MDagPathMap<SdfPath>& default_GetDagToUsdPathMapping() const
    {
        return base_t::GetDagToUsdPathMapping();
    }
    const UsdMayaUtil::MDagPathMap<SdfPath>& GetDagToUsdPathMapping() const override
    {
        if (TfPyOverride o = this->GetOverride("GetDagToUsdPathMapping")) {
            auto res = std::function<PXR_BOOST_PYTHON_NAMESPACE::object()>(
                TfPyCall<PXR_BOOST_PYTHON_NAMESPACE::object>(o))();
            if (res) {
                const_cast<UsdMayaUtil::MDagPathMap<SdfPath>*>(&_dagPathMap)->clear();
                TfPyLock pyLock;

                PXR_BOOST_PYTHON_NAMESPACE::list tuples(res);
                for (int i = 0; i < len(tuples); ++i) {
                    PXR_BOOST_PYTHON_NAMESPACE::tuple t(tuples[i]);
                    if (PXR_BOOST_PYTHON_NAMESPACE::len(t) != 2) {
                        TF_CODING_ERROR("PrimWriterWrapper.GetDagToUsdPathMapping: list<tuples> "
                                        "key expected, not "
                                        "found!");
                        return _dagPathMap;
                    }
                    PXR_BOOST_PYTHON_NAMESPACE::extract<MDagPath> extractedDagPath(t[0]);
                    if (!extractedDagPath.check()) {
                        TF_CODING_ERROR(
                            "PrimWriterWrapper.GetDagToUsdPathMapping: MDagPath key expected, not "
                            "found!");
                        return _dagPathMap;
                    }
                    PXR_BOOST_PYTHON_NAMESPACE::extract<SdfPath> extractedSdfPath(t[1]);
                    if (!extractedSdfPath.check()) {
                        TF_CODING_ERROR(
                            "PrimWriterWrapper.GetDagToUsdPathMapping: SdfPath key expected, not "
                            "found!");
                        return _dagPathMap;
                    }

                    MDagPath dagPath = extractedDagPath;
                    SdfPath  path = extractedSdfPath;
                    const_cast<UsdMayaUtil::MDagPathMap<SdfPath>*>(&_dagPathMap)
                        ->
                        operator[](dagPath)
                        = path;
                }
                return _dagPathMap;
            }
        }
        return This::default_GetDagToUsdPathMapping();
    }

    //---------------------------------------------------------------------------------------------
    /// \brief  wraps a factory function that allows registering an updated Python class
    //---------------------------------------------------------------------------------------------
    class FactoryFnWrapper : public UsdMayaPythonObjectRegistry
    {
    public:
        // Instances of this class act as "function objects" that are fully compatible with the
        // std::function requested by UsdMayaSchemaApiAdaptorRegistry::Register. These will create
        // python wrappers based on the latest class registered.
        UsdMayaPrimWriterSharedPtr operator()(
            const MFnDependencyNode& depNodeFn,
            const SdfPath&           usdPath,
            UsdMayaWriteJobContext&  jobCtx)
        {
            PXR_BOOST_PYTHON_NAMESPACE::object pyClass = GetPythonObject(_classIndex);
            if (!pyClass) {
                // Prototype was unregistered
                return nullptr;
            }
            auto     sptr = std::make_shared<This>(depNodeFn, usdPath, jobCtx);
            TfPyLock pyLock;
            PXR_BOOST_PYTHON_NAMESPACE::object instance = pyClass((uintptr_t)&sptr);
            PXR_BOOST_PYTHON_NAMESPACE::incref(instance.ptr());
            initialize_wrapper(instance.ptr(), sptr.get());
            return sptr;
        }

        UsdMayaPrimWriter::ContextSupport
        operator()(const UsdMayaJobExportArgs& exportArgs, const MObject& exportObj)
        {

            PXR_BOOST_PYTHON_NAMESPACE::object pyClass = GetPythonObject(_classIndex);
            if (!pyClass) {
                // Prototype was unregistered
                return UsdMayaPrimWriter::ContextSupport::Unsupported;
            }
            TfPyLock pyLock;
            if (PyObject_HasAttrString(pyClass.ptr(), "CanExport")) {
                PXR_BOOST_PYTHON_NAMESPACE::object CanExport = pyClass.attr("CanExport");
                PyObject*                          callable = CanExport.ptr();
                auto res = PXR_BOOST_PYTHON_NAMESPACE::call<int>(callable, exportArgs, exportObj);
                return UsdMayaPrimWriter::ContextSupport(res);
            } else {
                return UsdMayaPrimWriter::ContextSupport::Fallback;
            }
        }

        // Create a new wrapper for a Python class that is seen for the first time for a given
        // purpose. If we already have a registration for this purpose: update the class to
        // allow the previously issued factory function to use it.
        static FactoryFnWrapper Register(
            PXR_BOOST_PYTHON_NAMESPACE::object cl,
            const std::string&                 mayaTypeName,
            bool&                              updated)
        {
            size_t classIndex = RegisterPythonObject(cl, GetKey(cl, mayaTypeName));
            updated = (classIndex == UsdMayaPythonObjectRegistry::UPDATED);
            // Return a new factory function:
            return FactoryFnWrapper { classIndex };
        }

        // Unregister a class for a given purpose. This will cause the associated factory
        // function to stop producing this Python class.
        static void
        Unregister(PXR_BOOST_PYTHON_NAMESPACE::object cl, const std::string& mayaTypeName)
        {
            UnregisterPythonObject(cl, GetKey(cl, mayaTypeName));
        }

    private:
        // Function object constructor. Requires only the index of the Python class to use.
        FactoryFnWrapper(size_t classIndex)
            : _classIndex(classIndex) {};

        size_t _classIndex;

        // Generates a unique key based on the name of the class, along with the class
        // purpose:
        static std::string
        GetKey(PXR_BOOST_PYTHON_NAMESPACE::object cl, const std::string& mayaTypeName)
        {
            return ClassName(cl) + "," + mayaTypeName + "," + ",PrimWriter";
        }
    };

    static void Register(PXR_BOOST_PYTHON_NAMESPACE::object cl, const std::string& mayaTypeName)
    {
        bool             updated = false;
        FactoryFnWrapper fn = FactoryFnWrapper::Register(cl, mayaTypeName, updated);
        if (!updated) {

            // fn is used twice because the register function will handle both
            // CanExport and FactoryFn
            UsdMayaPrimWriterRegistry::Register(mayaTypeName, fn, fn, true);
        }
    }

    static void Unregister(PXR_BOOST_PYTHON_NAMESPACE::object cl, const std::string& mayaTypeName)
    {
        FactoryFnWrapper::Unregister(cl, mayaTypeName);
    }

private:
    SdfPathVector                     _modelPaths;
    UsdMayaUtil::MDagPathMap<SdfPath> _dagPathMap;
};

//----------------------------------------------------------------------------------------------------------------------
/// \brief  boost python binding for the UsdMayaShaderWriter
//----------------------------------------------------------------------------------------------------------------------
class ShaderWriterWrapper : public PrimWriterWrapper<UsdMayaShaderWriter>
{
public:
    typedef ShaderWriterWrapper This;
    typedef UsdMayaShaderWriter base_t;

    ShaderWriterWrapper(
        const MFnDependencyNode& depNodeFn,
        const SdfPath&           usdPath,
        UsdMayaWriteJobContext&  jobCtx)
        : PrimWriterWrapper<UsdMayaShaderWriter>(depNodeFn, usdPath, jobCtx)
    {
    }

    static std::shared_ptr<This> New(uintptr_t createdWrapper)
    {
        return *((std::shared_ptr<This>*)createdWrapper);
    }

    virtual ~ShaderWriterWrapper() { }

    TfToken default_GetShadingAttributeNameForMayaAttrName(const TfToken& mayaAttrName)
    {
        return base_t::GetShadingAttributeNameForMayaAttrName(mayaAttrName);
    }
    TfToken GetShadingAttributeNameForMayaAttrName(const TfToken& mayaAttrName) override
    {
        return this->CallVirtual<TfToken>(
            "GetShadingAttributeNameForMayaAttrName",
            &This::default_GetShadingAttributeNameForMayaAttrName)(mayaAttrName);
    }

    UsdAttribute default_GetShadingAttributeForMayaAttrName(
        const TfToken&          mayaAttrName,
        const SdfValueTypeName& typeName)
    {
        return base_t::GetShadingAttributeForMayaAttrName(mayaAttrName, typeName);
    }
    UsdAttribute GetShadingAttributeForMayaAttrName(
        const TfToken&          mayaAttrName,
        const SdfValueTypeName& typeName) override
    {
        return this->CallVirtual<UsdAttribute>(
            "GetShadingAttributeForMayaAttrName",
            &This::default_GetShadingAttributeForMayaAttrName)(mayaAttrName, typeName);
    }

    void default_Write(const UsdTimeCode& usdTime) { base_t::Write(usdTime); }
    void Write(const UsdTimeCode& usdTime) override
    {
        this->template CallVirtual<>("Write", &This::default_Write)(usdTime);
    }

    void default_PostExport() { base_t::PostExport(); }
    void PostExport() override
    {
        this->template CallVirtual<>("PostExport", &This::default_PostExport)();
    }

    //---------------------------------------------------------------------------------------------
    /// \brief  wraps a factory function that allows registering an updated Python class
    //---------------------------------------------------------------------------------------------
    class FactoryFnWrapper : public UsdMayaPythonObjectRegistry
    {
    public:
        // Instances of this class act as "function objects" that are fully compatible with the
        // std::function requested by UsdMayaSchemaApiAdaptorRegistry::Register. These will create
        // python wrappers based on the latest class registered.
        UsdMayaShaderWriterSharedPtr operator()(
            const MFnDependencyNode& depNodeFn,
            const SdfPath&           usdPath,
            UsdMayaWriteJobContext&  jobCtx)
        {
            PXR_BOOST_PYTHON_NAMESPACE::object pyClass = GetPythonObject(_classIndex);
            auto     sptr = std::make_shared<This>(depNodeFn, usdPath, jobCtx);
            TfPyLock pyLock;
            PXR_BOOST_PYTHON_NAMESPACE::object instance = pyClass((uintptr_t)&sptr);
            PXR_BOOST_PYTHON_NAMESPACE::incref(instance.ptr());
            initialize_wrapper(instance.ptr(), sptr.get());
            return sptr;
        }

        // We can have multiple function objects, this one apapts the CanImport function:
        UsdMayaPrimWriter::ContextSupport operator()(const UsdMayaJobExportArgs& exportArgs)
        {
            PXR_BOOST_PYTHON_NAMESPACE::object pyClass = GetPythonObject(_classIndex);
            if (!pyClass) {
                // Prototype was unregistered
                return UsdMayaPrimWriter::ContextSupport::Unsupported;
            }
            TfPyLock                           pyLock;
            PXR_BOOST_PYTHON_NAMESPACE::object CanExport = pyClass.attr("CanExport");
            PyObject*                          callable = CanExport.ptr();
            auto res = PXR_BOOST_PYTHON_NAMESPACE::call<int>(callable, exportArgs);
            return UsdMayaPrimWriter::ContextSupport(res);
        }

        // Create a new wrapper for a Python class that is seen for the first time for a given
        // purpose. If we already have a registration for this purpose: update the class to
        // allow the previously issued factory function to use it.
        static FactoryFnWrapper Register(
            PXR_BOOST_PYTHON_NAMESPACE::object cl,
            const std::string&                 usdShaderId,
            bool&                              updated)
        {
            size_t classIndex = RegisterPythonObject(cl, GetKey(cl, usdShaderId));
            updated = classIndex == UsdMayaPythonObjectRegistry::UPDATED;
            // Return a new factory function:
            return FactoryFnWrapper { classIndex };
        }

        // Unregister a class for a given purpose. This will cause the associated factory
        // function to stop producing this Python class.
        static void
        Unregister(PXR_BOOST_PYTHON_NAMESPACE::object cl, const std::string& usdShaderId)
        {
            UnregisterPythonObject(cl, GetKey(cl, usdShaderId));
        }

    private:
        // Function object constructor. Requires only the index of the Python class to use.
        FactoryFnWrapper(size_t classIndex)
            : _classIndex(classIndex) {};

        size_t _classIndex;

        // Generates a unique key based on the name of the class, along with the class
        // purpose:
        static std::string
        GetKey(PXR_BOOST_PYTHON_NAMESPACE::object cl, const std::string& usdShaderId)
        {
            return ClassName(cl) + "," + usdShaderId + "," + ",ShaderWriter";
        }
    };

    static void Register(PXR_BOOST_PYTHON_NAMESPACE::object cl, const TfToken& usdShaderId)
    {
        bool             updated = false;
        FactoryFnWrapper fn = FactoryFnWrapper::Register(cl, usdShaderId, updated);
        if (!updated) {
            UsdMayaShaderWriterRegistry::Register(usdShaderId, fn, fn, true);
        }
    }

    static void Unregister(PXR_BOOST_PYTHON_NAMESPACE::object cl, const TfToken& usdShaderId)
    {
        FactoryFnWrapper::Unregister(cl, usdShaderId);
    }

    static void RegisterSymmetric(
        PXR_BOOST_PYTHON_NAMESPACE::object cl,
        const TfToken&                     mayaNodeTypeName,
        const TfToken&                     usdShaderId,
        const TfToken&                     materialConversionName)
    {
        UsdMayaSymmetricShaderWriter::RegisterWriter(
            mayaNodeTypeName, usdShaderId, materialConversionName, true);
    }
};

namespace {

PXR_BOOST_PYTHON_NAMESPACE::object get_allChaserArgs(UsdMayaJobExportArgs& self)
{
    PXR_BOOST_PYTHON_NAMESPACE::dict allChaserArgs;
    for (auto&& perChaser : self.allChaserArgs) {
        auto perChaserDict = PXR_BOOST_PYTHON_NAMESPACE::dict();
        for (auto&& perItem : perChaser.second) {
            perChaserDict[perItem.first] = perItem.second;
        }
        allChaserArgs[perChaser.first] = perChaserDict;
    }
    return PXR_BOOST_PYTHON_NAMESPACE::object(allChaserArgs);
}

bool get_ExportCameras(UsdMayaJobExportArgs& self) { return self.isExportingCameras(); }

bool get_ExportMeshes(UsdMayaJobExportArgs& self) { return self.isExportingMeshes(); }

bool get_ExportLights(UsdMayaJobExportArgs& self) { return self.isExportingLights(); }

PXR_BOOST_PYTHON_NAMESPACE::object get_remapUVSetsTo(UsdMayaJobExportArgs& self)
{
    PXR_BOOST_PYTHON_NAMESPACE::dict uvSetRemaps;
    for (auto&& remap : self.remapUVSetsTo) {
        uvSetRemaps[remap.first] = remap.second;
    }
    return PXR_BOOST_PYTHON_NAMESPACE::object(uvSetRemaps);
}

// This class is used to expose protected members of UsdMayaPrimWriter to Python
class PrimWriterAllowProtected : public UsdMayaPrimWriter
{
    typedef UsdMayaPrimWriter base_t;

public:
    void _SetUsdPrim(const UsdPrim& usdPrim) { base_t::_SetUsdPrim(usdPrim); }
    const UsdMayaJobExportArgs& _GetExportArgs() const { return base_t::_GetExportArgs(); }
    FlexibleSparseValueWriter*  _GetSparseValueWriter() { return base_t::_GetSparseValueWriter(); }
};

void unprotect_SetUsdPrim(UsdMayaPrimWriter& pw, const UsdPrim& prim)
{
    reinterpret_cast<PrimWriterAllowProtected&>(pw)._SetUsdPrim(prim);
}
const UsdMayaJobExportArgs& unprotect_GetExportArgs(UsdMayaPrimWriter& pw)
{
    return reinterpret_cast<PrimWriterAllowProtected&>(pw)._GetExportArgs();
}
MayaUsdLibSparseValueWriter unprotect_GetSparseValueWriter(UsdMayaPrimWriter& pw)
{
    return MayaUsdLibSparseValueWriter(
        reinterpret_cast<PrimWriterAllowProtected&>(pw)._GetSparseValueWriter());
}

} // namespace
//----------------------------------------------------------------------------------------------------------------------
void wrapJobExportArgs()
{
    using namespace PXR_BOOST_PYTHON_NAMESPACE;

    class_<UsdMayaJobExportArgs>("JobExportArgs", no_init)
        .add_property("allChaserArgs", ::get_allChaserArgs)
        .add_property(
            "allMaterialConversions",
            make_getter(
                &UsdMayaJobExportArgs::allMaterialConversions,
                return_value_policy<TfPySequenceToSet>()))
        .add_property(
            "chaserNames",
            make_getter(
                &UsdMayaJobExportArgs::chaserNames, return_value_policy<TfPySequenceToSet>()))
        .add_property(
            "compatibility",
            make_getter(
                &UsdMayaJobExportArgs::compatibility, return_value_policy<return_by_value>()))
        .add_property(
            "convertMaterialsTo",
            make_getter(
                &UsdMayaJobExportArgs::convertMaterialsTo, return_value_policy<return_by_value>()))
        .add_property("remapUVSetsTo", ::get_remapUVSetsTo)
        //.add_property("dagPaths", requires exporting UsdMayaUtil::MDagPathSet)
        .add_property(
            "defaultMeshScheme",
            make_getter(
                &UsdMayaJobExportArgs::defaultMeshScheme, return_value_policy<return_by_value>()))
        .add_property(
            "defaultUSDFormat",
            make_getter(
                &UsdMayaJobExportArgs::defaultUSDFormat, return_value_policy<return_by_value>()))
        .def_readonly("defaultPrim", &UsdMayaJobExportArgs::defaultPrim)
        .def_readonly("eulerFilter", &UsdMayaJobExportArgs::eulerFilter)
        .def_readonly("excludeInvisible", &UsdMayaJobExportArgs::excludeInvisible)
        .def_readonly("exportBlendShapes", &UsdMayaJobExportArgs::exportBlendShapes)
        .def_readonly(
            "exportCollectionBasedBindings", &UsdMayaJobExportArgs::exportCollectionBasedBindings)
        .def_readonly("exportColorSets", &UsdMayaJobExportArgs::exportColorSets)
        .def_readonly("exportMaterials", &UsdMayaJobExportArgs::exportMaterials)
        .def_readonly("exportAssignedMaterials", &UsdMayaJobExportArgs::exportAssignedMaterials)
        .def_readonly("exportComponentTags", &UsdMayaJobExportArgs::exportComponentTags)
        .def_readonly("exportStagesAsRefs", &UsdMayaJobExportArgs::exportStagesAsRefs)
        .def_readonly("exportDefaultCameras", &UsdMayaJobExportArgs::exportDefaultCameras)
        .def_readonly("exportDisplayColor", &UsdMayaJobExportArgs::exportDisplayColor)
        .def_readonly("exportDistanceUnit", &UsdMayaJobExportArgs::exportDistanceUnit)
        .def_readonly("exportInstances", &UsdMayaJobExportArgs::exportInstances)
        .def_readonly("exportMaterialCollections", &UsdMayaJobExportArgs::exportMaterialCollections)
        .def_readonly("exportMeshUVs", &UsdMayaJobExportArgs::exportMeshUVs)
        .def_readonly("exportNurbsExplicitUV", &UsdMayaJobExportArgs::exportNurbsExplicitUV)
        .add_property(
            "exportRelativeTextures",
            make_getter(
                &UsdMayaJobExportArgs::exportRelativeTextures,
                return_value_policy<return_by_value>()))
        .def_readonly("legacyMaterialScope", &UsdMayaJobExportArgs::legacyMaterialScope)
        .add_property(
            "referenceObjectMode",
            make_getter(
                &UsdMayaJobExportArgs::referenceObjectMode, return_value_policy<return_by_value>()))
        .def_readonly("exportRefsAsInstanceable", &UsdMayaJobExportArgs::exportRefsAsInstanceable)
        .def_readonly("exportSelected", &UsdMayaJobExportArgs::exportSelected)
        .add_property(
            "exportSkels",
            make_getter(&UsdMayaJobExportArgs::exportSkels, return_value_policy<return_by_value>()))
        .add_property(
            "exportSkin",
            make_getter(&UsdMayaJobExportArgs::exportSkin, return_value_policy<return_by_value>()))
        .def_readonly("exportVisibility", &UsdMayaJobExportArgs::exportVisibility)
        .def_readonly("file", &UsdMayaJobExportArgs::file)
        .def_readonly("rootPrim", &UsdMayaJobExportArgs::rootPrim)
        .def_readonly("rootPrimType", &UsdMayaJobExportArgs::rootPrimType)
        .add_property(
            "exportRoots",
            make_getter(
                &UsdMayaJobExportArgs::exportRoots, return_value_policy<TfPySequenceToSet>()))
        .def_readonly("upAxis", &UsdMayaJobExportArgs::upAxis)
        .def_readonly("unit", &UsdMayaJobExportArgs::unit)
        .add_property(
            "filteredTypeIds",
            make_getter(
                &UsdMayaJobExportArgs::filteredTypeIds, return_value_policy<TfPySequenceToSet>()))
        .add_property(
            "geomSidedness",
            make_getter(
                &UsdMayaJobExportArgs::geomSidedness, return_value_policy<return_by_value>()))
        .def_readonly("ignoreWarnings", &UsdMayaJobExportArgs::ignoreWarnings)
        .def_readonly("includeEmptyTransforms", &UsdMayaJobExportArgs::includeEmptyTransforms)
        .def_readonly("isDuplicating", &UsdMayaJobExportArgs::isDuplicating)
        .add_property(
            "includeAPINames",
            make_getter(
                &UsdMayaJobExportArgs::includeAPINames, return_value_policy<TfPySequenceToSet>()))
        .add_property(
            "jobContextNames",
            make_getter(
                &UsdMayaJobExportArgs::jobContextNames, return_value_policy<TfPySequenceToSet>()))
        .add_property(
            "materialCollectionsPath",
            make_getter(
                &UsdMayaJobExportArgs::materialCollectionsPath,
                return_value_policy<return_by_value>()))
        .add_property(
            "materialsScopeName",
            make_getter(
                &UsdMayaJobExportArgs::materialsScopeName, return_value_policy<return_by_value>()))
        .def_readonly("melPerFrameCallback", &UsdMayaJobExportArgs::melPerFrameCallback)
        .def_readonly("melPostCallback", &UsdMayaJobExportArgs::melPostCallback)
        .def_readonly("mergeTransformAndShape", &UsdMayaJobExportArgs::mergeTransformAndShape)
        .def_readonly("normalizeNurbs", &UsdMayaJobExportArgs::normalizeNurbs)
        .def_readonly("preserveUVSetNames", &UsdMayaJobExportArgs::preserveUVSetNames)
        .def_readonly("writeDefaults", &UsdMayaJobExportArgs::writeDefaults)
        .def_readonly("metersPerUnit", &UsdMayaJobExportArgs::metersPerUnit)
        .add_property(
            "parentScope",
            make_getter(&UsdMayaJobExportArgs::parentScope, return_value_policy<return_by_value>()))
        .add_property(
            "rootPrim",
            make_getter(&UsdMayaJobExportArgs::rootPrim, return_value_policy<return_by_value>()))
        .add_property(
            "rootPrimType",
            make_getter(
                &UsdMayaJobExportArgs::rootPrimType, return_value_policy<return_by_value>()))
        .add_property(
            "upAxis",
            make_getter(&UsdMayaJobExportArgs::upAxis, return_value_policy<return_by_value>()))
        .add_property(
            "unit",
            make_getter(&UsdMayaJobExportArgs::unit, return_value_policy<return_by_value>()))
        .def_readonly("pythonPerFrameCallback", &UsdMayaJobExportArgs::pythonPerFrameCallback)
        .def_readonly("pythonPostCallback", &UsdMayaJobExportArgs::pythonPostCallback)
        .add_property(
            "renderLayerMode",
            make_getter(
                &UsdMayaJobExportArgs::renderLayerMode, return_value_policy<return_by_value>()))
        .add_property(
            "rootKind",
            make_getter(&UsdMayaJobExportArgs::rootKind, return_value_policy<return_by_value>()))
        .add_property(
            "animationType",
            make_getter(
                &UsdMayaJobExportArgs::animationType, return_value_policy<return_by_value>()))
        .def_readonly("disableModelKindProcessor", &UsdMayaJobExportArgs::disableModelKindProcessor)
        .add_property(
            "rootMapFunction",
            make_getter(
                &UsdMayaJobExportArgs::rootMapFunction, return_value_policy<return_by_value>()))
        .add_property(
            "shadingMode",
            make_getter(&UsdMayaJobExportArgs::shadingMode, return_value_policy<return_by_value>()))
        .def_readonly("staticSingleSample", &UsdMayaJobExportArgs::staticSingleSample)
        .def_readonly("stripNamespaces", &UsdMayaJobExportArgs::stripNamespaces)
        .def_readonly("worldspace", &UsdMayaJobExportArgs::worldspace)
        .add_property(
            "timeSamples",
            make_getter(&UsdMayaJobExportArgs::timeSamples, return_value_policy<return_by_value>()))
        .add_property(
            "usdModelRootOverridePath",
            make_getter(
                &UsdMayaJobExportArgs::usdModelRootOverridePath,
                return_value_policy<return_by_value>()))
        .add_property("exportMeshes", ::get_ExportMeshes)
        .add_property("exportCameras", ::get_ExportCameras)
        .add_property("exportLights", ::get_ExportLights)
        .def_readonly("verbose", &UsdMayaJobExportArgs::verbose)
        .def("GetResolvedFileName", &UsdMayaJobExportArgs::GetResolvedFileName)
        .def("GetDefaultMaterialsScopeName", &UsdMayaJobExportArgs::GetDefaultMaterialsScopeName)
        .staticmethod("GetDefaultMaterialsScopeName");
}

TF_REGISTRY_FUNCTION(TfEnum)
{
    TF_ADD_ENUM_NAME(UsdMayaPrimWriter::ContextSupport::Supported, "Supported");
    TF_ADD_ENUM_NAME(UsdMayaPrimWriter::ContextSupport::Fallback, "Fallback");
    TF_ADD_ENUM_NAME(UsdMayaPrimWriter::ContextSupport::Unsupported, "Unsupported");
}

void wrapPrimWriter()
{

    PXR_BOOST_PYTHON_NAMESPACE::class_<PrimWriterWrapper<>, PXR_BOOST_PYTHON_NAMESPACE::noncopyable>
        c("PrimWriter", PXR_BOOST_PYTHON_NAMESPACE::no_init);

    PXR_BOOST_PYTHON_NAMESPACE::scope s(c);

    TfPyWrapEnum<UsdMayaPrimWriter::ContextSupport>();

    c.def("__init__", make_constructor(&PrimWriterWrapper<>::New))
        .def(
            "PostExport",
            &PrimWriterWrapper<>::PostExport,
            &PrimWriterWrapper<>::default_PostExport)
        .def("Write", &PrimWriterWrapper<>::Write, &PrimWriterWrapper<>::default_Write)

        .def("GetExportVisibility", &PrimWriterWrapper<>::GetExportVisibility)
        .def("SetExportVisibility", &PrimWriterWrapper<>::SetExportVisibility)
        .def(
            "GetMayaObject",
            &PrimWriterWrapper<>::GetMayaObject,
            PXR_BOOST_PYTHON_NAMESPACE::return_value_policy<
                PXR_BOOST_PYTHON_NAMESPACE::return_by_value>())
        .def(
            "GetUsdPrim",
            &PrimWriterWrapper<>::GetUsdPrim,
            PXR_BOOST_PYTHON_NAMESPACE::return_internal_reference<>())
        .def("_SetUsdPrim", &::unprotect_SetUsdPrim)
        .def(
            "MakeSingleSamplesStatic",
            static_cast<void (PrimWriterWrapper<>::*)()>(
                &PrimWriterWrapper<>::MakeSingleSamplesStatic))
        .def(
            "MakeSingleSamplesStatic",
            static_cast<void (PrimWriterWrapper<>::*)(UsdAttribute attr)>(
                &PrimWriterWrapper<>::MakeSingleSamplesStatic))

        .def(
            "_HasAnimCurves",
            &PrimWriterWrapper<>::_HasAnimCurves,
            &PrimWriterWrapper<>::default__HasAnimCurves)
        .def(
            "_GetExportArgs",
            &::unprotect_GetExportArgs,
            PXR_BOOST_PYTHON_NAMESPACE::return_internal_reference<>())
        .def("_GetSparseValueWriter", &::unprotect_GetSparseValueWriter)

        .def(
            "GetDagPath",
            &PrimWriterWrapper<>::GetDagPath,
            PXR_BOOST_PYTHON_NAMESPACE::return_value_policy<
                PXR_BOOST_PYTHON_NAMESPACE::return_by_value>())
        .def(
            "GetUsdStage",
            &UsdMayaPrimWriter::GetUsdStage,
            PXR_BOOST_PYTHON_NAMESPACE::return_value_policy<
                PXR_BOOST_PYTHON_NAMESPACE::return_by_value>())
        .def(
            "GetUsdPath",
            &UsdMayaPrimWriter::GetUsdPath,
            PXR_BOOST_PYTHON_NAMESPACE::return_value_policy<
                PXR_BOOST_PYTHON_NAMESPACE::return_by_value>())

        .def("Register", &PrimWriterWrapper<>::Register)
        .staticmethod("Register")
        .def("Unregister", &PrimWriterWrapper<>::Unregister)
        .staticmethod("Unregister");
}

//----------------------------------------------------------------------------------------------------------------------
void wrapShaderWriter()
{
    PXR_BOOST_PYTHON_NAMESPACE::class_<
        ShaderWriterWrapper,
        PXR_BOOST_PYTHON_NAMESPACE::bases<UsdMayaPrimWriter>,
        PXR_BOOST_PYTHON_NAMESPACE::noncopyable>
        c("ShaderWriter", PXR_BOOST_PYTHON_NAMESPACE::no_init);

    PXR_BOOST_PYTHON_NAMESPACE::scope s(c);

    c.def("__init__", make_constructor(&ShaderWriterWrapper::New))
        .def(
            "GetShadingAttributeNameForMayaAttrName",
            &ShaderWriterWrapper::GetShadingAttributeNameForMayaAttrName,
            &ShaderWriterWrapper::default_GetShadingAttributeNameForMayaAttrName)
        .def(
            "GetShadingAttributeForMayaAttrName",
            &ShaderWriterWrapper::GetShadingAttributeForMayaAttrName,
            &ShaderWriterWrapper::default_GetShadingAttributeForMayaAttrName)
        .def("Write", &ShaderWriterWrapper::Write, &ShaderWriterWrapper::default_Write)
        .def(
            "PostExport",
            &ShaderWriterWrapper::PostExport,
            &ShaderWriterWrapper::default_PostExport)

        .def("Register", &ShaderWriterWrapper::Register)
        .staticmethod("Register")
        .def("Unregister", &ShaderWriterWrapper::Unregister)
        .staticmethod("Unregister")
        .def("RegisterSymmetric", &ShaderWriterWrapper::RegisterSymmetric)
        .staticmethod("RegisterSymmetric");
}
