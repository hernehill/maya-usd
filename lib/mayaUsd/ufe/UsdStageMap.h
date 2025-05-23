//
// Copyright 2019 Autodesk
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
#ifndef MAYAUSD_USDSTAGEMAP_H
#define MAYAUSD_USDSTAGEMAP_H

#include <mayaUsd/base/api.h>
#include <mayaUsd/utils/mayaNodeTypeObserver.h>

#include <pxr/base/tf/hash.h>
#include <pxr/base/tf/hashmap.h>
#include <pxr/base/tf/hashset.h>
#include <pxr/usd/usd/stage.h>

#include <maya/MObjectHandle.h>
#include <ufe/path.h>

#include <unordered_map>
#include <vector>

// Pending rework of mayaUsd namespaces, MayaUsdProxyShapeBase is in the Pixar
// namespace.  PPT, 9-Mar-2021.
PXR_NAMESPACE_OPEN_SCOPE
class MayaUsdProxyShapeBase;
PXR_NAMESPACE_CLOSE_SCOPE

namespace MAYAUSD_NS_DEF {
namespace ufe {

//! \brief USD Stage Map
/*!
    Two-way map of proxy shape UFE path to corresponding stage.

    We will assume that	a USD proxy shape will not be instanced (even though
    nothing in the data model prevents it).  To generalized access to the
    underlying node, we store an MObjectHandle in the maps.

    The cache is refreshed on access to a stage given a path which cannot be
    found.  In this way, the cache does not need to observe the Maya data
    model, and we avoid order of notification problems where one observer would
    need to access the cache before it is refreshed, since there is no
    guarantee on the order of notification of Ufe observers.  An earlier
    implementation with rename observation had the Maya Outliner (which
    observes rename) access the UsdStageMap on rename before the UsdStageMap
    had been updated.
*/
class MAYAUSD_CORE_PUBLIC UsdStageMap
    : private MayaNodeTypeObserver::Listener
    , private MayaNodeObserver::Listener
{
public:
    typedef PXR_NS::TfHashSet<PXR_NS::UsdStageWeakPtr, PXR_NS::TfHash> StageSet;

    //! Retrieve the global instance of the stage map.
    static UsdStageMap& getInstance();

    //! Get USD stage for the first segment of the argument path.
    PXR_NS::UsdStageWeakPtr stage(const Ufe::Path& path, bool rebuildCacheIfNeeded = true);

    //! Return the ProxyShape object for the first segment of the argument
    //! path.  If no such proxy shape exists, returns a null MObject.
    MObject proxyShape(const Ufe::Path& path, bool rebuildCacheIfNeeded = true);

    //! Return the ProxyShape node for the first segment of the argument path.
    //! If no such proxy shape node exists, returns a null pointer.
    PXR_NS::MayaUsdProxyShapeBase*
    proxyShapeNode(const Ufe::Path& path, bool rebuildCacheIfNeeded = true);

    //! Return the ProxyShape node UFE path for the argument stage.
    Ufe::Path path(PXR_NS::UsdStageWeakPtr stage);

    //! Return all the USD stages.
    StageSet allStages();

    //! Return all the USD stage paths.
    std::vector<Ufe::Path> allStagesPaths();

    //! Returns true if the a stage with the given path exists, returns false otherwise.
    bool isInStagesCache(const Ufe::Path& path);

    //! Set the stage map as dirty. It will be cleared immediately, but
    //! only repopulated when stage info is requested.
    void setDirty();

    //! Returns true if the stage map is dirty (meaning it needs to be filled in).
    bool isDirty() const { return _dirty; }

private:
    UsdStageMap();
    ~UsdStageMap();

    MAYAUSD_DISALLOW_COPY_MOVE_AND_ASSIGNMENT(UsdStageMap);

    void addItem(const Ufe::Path& path);
    bool rebuildIfDirty();

    // MayaNodeTypeObserver::Listener
    void processNodeAdded(MObject& node) override;
    void processNodeRemoved(MObject& node) override;

    // MayaNodeObserver::Listener
    void processNodeRenamed(MObject& node, const MString& str) override;
    void processParentAdded(MObject& node, MDagPath& child, MDagPath& parent) override;

    //! Called when a proxy shape node is added to the Maya scene.
    void addProxyShapeNode(const PXR_NS::MayaUsdProxyShapeBase& proxyShape, MObject& node);

    //! Called when a proxy shape node is removed from the Maya scene.
    void removeProxyShapeNode(const PXR_NS::MayaUsdProxyShapeBase& proxyShape, MObject& node);

    //! Called when a proxy shape is renamed. (Including when initially created.)
    void updateProxyShapeName(
        const PXR_NS::MayaUsdProxyShapeBase& proxyShape,
        const MString&                       oldName,
        const MString&                       newName);

    //! Called when a proxy shape is reparented.
    void updateProxyShapePath(
        const PXR_NS::MayaUsdProxyShapeBase& proxyShape,
        const MDagPath&                      newParentPath);

private:
    // We keep two maps for fast lookup when there are many proxy shapes.
    using PathToObject = std::unordered_map<Ufe::Path, MObjectHandle>;
    using StageToObject = PXR_NS::TfHashMap<PXR_NS::UsdStageWeakPtr, MObjectHandle, PXR_NS::TfHash>;
    PathToObject  _pathToObject;
    StageToObject _stageToObject;
    bool          _dirty { true };

}; // UsdStageMap

} // namespace ufe
} // namespace MAYAUSD_NS_DEF

#endif // MAYAUSD_USDSTAGEMAP_H
