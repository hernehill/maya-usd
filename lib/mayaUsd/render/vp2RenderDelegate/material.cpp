//
// Copyright 2020 Autodesk
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
#include "material.h"

#include "debugCodes.h"
#include "pxr/usd/sdr/registry.h"
#include "pxr/usd/sdr/shaderNode.h"
#include "renderDelegate.h"
#include "tokens.h"

#include <mayaUsd/base/tokens.h>
#include <mayaUsd/render/vp2RenderDelegate/colorManagementPreferences.h>
#include <mayaUsd/render/vp2RenderDelegate/proxyRenderDelegate.h>
#include <mayaUsd/render/vp2ShaderFragments/shaderFragments.h>
#include <mayaUsd/utils/hash.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/getenv.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/sceneDelegate.h>

#ifdef WANT_MATERIALX_BUILD
#include <pxr/imaging/hdMtlx/hdMtlx.h>
#endif
#include <pxr/pxr.h>
#include <pxr/usd/ar/packageUtils.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdr/registry.h>
#include <pxr/usd/usdHydra/tokens.h>
#include <pxr/usd/usdUtils/pipeline.h>
#include <pxr/usdImaging/usdImaging/textureUtils.h>
#include <pxr/usdImaging/usdImaging/tokens.h>

#include <maya/M3dView.h>
#include <maya/MEventMessage.h>
#include <maya/MFragmentManager.h>
#include <maya/MGlobal.h>
#include <maya/MProfiler.h>
#include <maya/MSceneMessage.h>
#include <maya/MShaderManager.h>
#include <maya/MStatus.h>
#include <maya/MString.h>
#include <maya/MStringArray.h>
#include <maya/MTextureManager.h>
#include <maya/MUintArray.h>
#include <maya/MViewport2Renderer.h>

#ifdef WANT_MATERIALX_BUILD
#include <mayaUsd/render/MaterialXGenOgsXml/CombinedMaterialXVersion.h>
#include <mayaUsd/render/MaterialXGenOgsXml/OgsFragment.h>
#include <mayaUsd/render/MaterialXGenOgsXml/OgsXmlGenerator.h>
#include <mayaUsd/render/MaterialXGenOgsXml/ShaderGenUtil.h>
#if MX_COMBINED_VERSION >= 13808
#include <mayaUsd/render/MaterialXGenOgsXml/LobePruner.h>
#endif

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/File.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXGenGlsl/GlslShaderGenerator.h>
#include <MaterialXGenShader/HwShaderGenerator.h>
#include <MaterialXGenShader/ShaderStage.h>
#include <MaterialXGenShader/Util.h>
#include <MaterialXRender/ImageHandler.h>
#endif

#include <pxr/imaging/hdSt/udimTextureObject.h>
#include <pxr/imaging/hio/image.h>

#include <boost/functional/hash.hpp>
#include <ghc/filesystem.hpp>
#include <tbb/parallel_for.h>

#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef WANT_MATERIALX_BUILD
namespace mx = MaterialX;
#endif

PXR_NAMESPACE_OPEN_SCOPE

static bool _IsDisabledAsyncTextureLoading()
{
    static const MString kOptionVarName(MayaUsdOptionVars->DisableAsyncTextureLoading.GetText());
    if (MGlobal::optionVarExists(kOptionVarName)) {
        return MGlobal::optionVarIntValue(kOptionVarName);
    }
    return true;
}

// Refresh viewport duration (in milliseconds)
static const std::size_t kRefreshDuration { 1000 };

namespace {

// USD `UsdImagingDelegate::ApplyPendingUpdates()` would request to
// remove the material then recreate, this is causing texture disappearing
// when user manipulating a prim (while holding mouse buttion).
// We hold a copy of the texture info reference, so that the texture will not
// get released immediately along with material removal.
// If the textures would have been requested to reload in `ApplyPendingUpdates()`,
// we could still reuse the loaded one from cache, otherwise the idle task can
// safely release the texture.
class _TransientTexturePreserver
{
public:
    static _TransientTexturePreserver& GetInstance()
    {
        static _TransientTexturePreserver sInstance;
        return sInstance;
    }

    void PreserveTextures(HdVP2LocalTextureMap& localTextureMap)
    {
        if (_isExiting) {
            return;
        }

        // Locking to avoid race condition for insertion to pendingRemovalTextures
        std::lock_guard<std::mutex> lock(_removalTaskMutex);

        // Avoid creating multiple idle tasks if there is already one
        bool hasRemovalTask = !_pendingRemovalTextures.empty();
        for (const auto& info : localTextureMap) {
            _pendingRemovalTextures.emplace(info.second);
        }

        if (!hasRemovalTask) {
            // Note that we do not need locking inside idle task since it will
            // only be executed serially.
            MGlobal::executeTaskOnIdle(
                [](void* data) { _TransientTexturePreserver::GetInstance().Clear(); });
        }
    }

    void Clear() { _pendingRemovalTextures.clear(); }

    void OnMayaExit()
    {
        _isExiting = true;
        Clear();
    }

private:
    _TransientTexturePreserver() = default;
    ~_TransientTexturePreserver() = default;

    std::unordered_set<HdVP2TextureInfoSharedPtr> _pendingRemovalTextures;
    std::mutex                                    _removalTaskMutex;
    bool                                          _isExiting = false;
};

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(
    _tokens,

    (file)
    (opacity)
    (opacityThreshold)
    (existence)
    (transmission)
    (transparency)
    (alpha)
    (alpha_mode)
    (transmission_weight)
    (geometry_opacity)
    (useSpecularWorkflow)
    (st)
    (varname)
    (sourceColorSpace)
    ((auto_, "auto"))
    (sRGB)
    (raw)
    (fallback)

    (input)
    (output)

    (rgb)
    (r)
    (g)
    (b)
    (a)

    (xyz)
    (x)
    (y)
    (z)
    (w)

    (Float4ToFloatX)
    (Float4ToFloatY)
    (Float4ToFloatZ)
    (Float4ToFloatW)
    (Float4ToFloat3)
    (Float3ToFloatX)
    (Float3ToFloatY)
    (Float3ToFloatZ)
    (FloatToFloat3)

    // When using OCIO from Maya:
    (Maya_OCIO_)
    (toColor3ForCM)
    (extract)

    (gltf_pbr)

    (UsdPrimvarReader_color)
    (UsdPrimvarReader_vector)

    (Unknown)
    (Computed)

    // XXX Deprecated in PXR_VERSION > 2211
    (result)
    (cardsUv)
    (diffuseColor)

    (glslfx)

    (UsdDrawModeCards)
    ((DrawMode, "drawMode.glslfx"))

    (mayaIsBackFacing)
    (isBackfacing)
    (FallbackShader)

    // Added in PXR_VERSION >= 2311
    ((ColorSpacePrefix, "colorSpace:"))
);
// clang-format on

#ifdef WANT_MATERIALX_BUILD

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(
    _mtlxTokens,

    (USD_Mtlx_VP2_Material)
    (NG_Maya)
    (ND_surface)
    (ND_standard_surface_surfaceshader)
    (filename)
    (i_geomprop_)
    (geomprop)
    (uaddressmode)
    (vaddressmode)
    (filtertype)
    (closest)
    (cubic)
    (channels)
    (out)
    (surfaceshader)

    // Texcoord reader identifiers:
    (texcoord)
    (index)
    (UV0)
    (geompropvalue)
    (ST_reader)
    (vector2)

    // Tangent related identifiers:
    (tangent)
    (normalmap)
    (arbitrarytangents)
    (texcoordtangents)
    (Tworld)
    (Tobject)
    (Tw_reader)
    (vector3)
    (transformvector)
    (constant)
    (value)
    (Tw_to_To)
    (in)
    (space)
    (fromspace)
    (tospace)
    (string)
    (world)
    (object)
    (model)
    (outTworld)
    (outTobject)
    (tangent_fix)

    // Basic color-correction:
    (outColor)
    (colorSpace)
    (color3)
    (color4)

    // MaterialX 1.39 bitangent support:
    (bitangent)
    (bump)
    (normal)
    (crossproduct)
    (in1)
    (in2)
);

// These attribute names usually indicate we have a source color space to handle.
const auto _mtlxKnownColorSpaceAttrs
        = std::vector<TfToken> { _tokens->sourceColorSpace, _mtlxTokens->colorSpace };

#ifndef HAS_COLOR_MANAGEMENT_SUPPORT_API
// Maps from a known Maya target color space name to the corresponding color correct category.
const std::unordered_map<std::string, std::string> _mtlxColorCorrectCategoryMap = {
    { "scene-linear Rec.709-sRGB", "MayaND_sRGBtoLinrec709_" },
    { "scene-linear Rec 709/sRGB", "MayaND_sRGBtoLinrec709_" },
    { "ACEScg",                    "MayaND_sRGBtoACEScg_" },
    { "ACES2065_1",                "MayaND_sRGBtoACES2065_" },
    { "ACES2065-1",                "MayaND_sRGBtoACES2065_" },
    { "scene-linear DCI-P3 D65",   "MayaND_sRGBtoLinDCIP3D65_" },
    { "scene-linear DCI-P3",       "MayaND_sRGBtoLinDCIP3D65_" },
    { "scene-linear Rec.2020",     "MayaND_sRGBtoLinrec2020_" },
    { "scene-linear Rec 2020",     "MayaND_sRGBtoLinrec2020_" },
};
#endif

// clang-format on

struct _MaterialXData
{
    _MaterialXData()
    {
        try {
            _mtlxSearchPath = HdMtlxSearchPaths();
            _mtlxLibrary = mx::createDocument();
#if PXR_VERSION > 2311
            _mtlxLibrary->importLibrary(HdMtlxStdLibraries());
#else
            mx::loadLibraries({}, _mtlxSearchPath, _mtlxLibrary);
#endif

            _FixLibraryTangentInputs(_mtlxLibrary);

            mx::OgsXmlGenerator::setUseLightAPI(MAYA_LIGHTAPI_VERSION_2);

            // This environment variable is defined in USD: pxr\usd\usdMtlx\parser.cpp
            static const std::string env = TfGetenv("USDMTLX_PRIMARY_UV_NAME");
            _mainUvSetName = env.empty() ? UsdUtilsGetPrimaryUVSetName().GetString() : env;

#if MX_COMBINED_VERSION >= 13808
            _lobePruner = MaterialXMaya::ShaderGenUtil::LobePruner::create();
            _lobePruner->setLibrary(_mtlxLibrary);
            _lobePruner->optimizeLibrary(_mtlxLibrary);

            // TODO: Optimize published shaders.
            // SCENARIO: User publishes a shader with a NodeGraph implementation that encapsulates a
            // slow surface shader like OpenPBR or Standard surface. Notices the performance of the
            // published shader is poor compared to the unpublished version. FIX: Run the LobePruner
            // on all custom shader graphs in the _mtlxLibrary to replace the slow surfaces with
            // optimized nodes CAVEAT: If the user has promoted all the weight attributes to the
            // NodeGraph boundary, then no optimization will be found. This would require a change
            // in the LobePruner to detect transitive weights. Doable, but complex. We will wait
            // until there is sufficient demand.
#endif
        } catch (mx::Exception& e) {
            TF_RUNTIME_ERROR(
                "Caught exception '%s' while initializing MaterialX library", e.what());
        }
    }
    MaterialX::FileSearchPath _mtlxSearchPath; //!< MaterialX library search path
    MaterialX::DocumentPtr    _mtlxLibrary;    //!< MaterialX library
    std::string               _mainUvSetName;  //!< Main UV set name
#if MX_COMBINED_VERSION >= 13808
    MaterialXMaya::ShaderGenUtil::LobePruner::Ptr _lobePruner;
#endif

private:
    void _FixLibraryTangentInputs(MaterialX::DocumentPtr& mtlxLibrary);
};

_MaterialXData& _GetMaterialXData()
{
    static std::unique_ptr<_MaterialXData> materialXData;
    static std::once_flag                  once;
    std::call_once(once, []() { materialXData.reset(new _MaterialXData()); });

    return *materialXData;
}

//! Return true if that node parameter has topological impact on the generated code.
//
// Swizzle and geompropvalue nodes are known to have an attribute that affects
// shader topology. The "channels" and "geomprop" attributes will have effects at the codegen level,
// not at runtime. Yes, this is forbidden internal knowledge of the MaterialX shader generator and
// we might get other nodes like this one in a future update.
//
// The index input of the texcoord and geomcolor nodes affect which stream to read and is topo
// affecting.
//
// Any geometric input that can specify model/object/world space is also topo affecting.
//
// Things to look out for are parameters of type "string" and parameters with the "uniform"
// metadata. These need to be reviewed against the code used in their registered
// implementations (see registerImplementation calls in the GlslShaderGenerator CTOR). Sadly
// we can not make that a rule because the filename of an image node is both a "string" and
// has the "uniform" metadata, yet is not affecting topology.
bool _IsTopologicalNode(const HdMaterialNode2& inNode)
{
    mx::NodeDefPtr nodeDef
        = _GetMaterialXData()._mtlxLibrary->getNodeDef(inNode.nodeTypeId.GetString());
    if (nodeDef) {
        return MaterialXMaya::ShaderGenUtil::TopoNeutralGraph::isTopologicalNodeDef(*nodeDef);
    }

#if MX_COMBINED_VERSION >= 13900
    // Swizzles are gone in 1.39, but they are still topological when found in legacy data:
    if (TfStringStartsWith(inNode.nodeTypeId.GetString(), "ND_swizzle_")) {
        return true;
    }
#endif

    return false;
}

#if PXR_VERSION >= 2311
// Hydra in USD 23.11 will add a "colorspace:Foo" parameter matching color managed "Foo" parameter
bool _IsHydraColorSpace(const TfToken& paramName)
{
    if (paramName.GetString().rfind(_tokens->ColorSpacePrefix.GetString(), 0) == 0) {
        return true;
    }
    return std::find(_mtlxKnownColorSpaceAttrs.begin(), _mtlxKnownColorSpaceAttrs.end(), paramName)
        != _mtlxKnownColorSpaceAttrs.end();
};
#endif

bool _IsMaterialX(const HdMaterialNode& node)
{
    SdrRegistry& shaderReg = SdrRegistry::GetInstance();
#if PXR_VERSION >= 2505
    SdrShaderNodeConstPtr sdrNode = shaderReg.GetShaderNodeByIdentifier(node.identifier);
    return sdrNode && sdrNode->GetSourceType() == HdVP2Tokens->mtlx;
#else
    NdrNodeConstPtr ndrNode = shaderReg.GetNodeByIdentifier(node.identifier);
    return ndrNode && ndrNode->GetSourceType() == HdVP2Tokens->mtlx;
#endif
}

bool _MxHasFilenameInput(const mx::NodeDefPtr nodeDef)
{
    for (const auto& input : nodeDef->getActiveInputs()) {
        if (input->getType() == _mtlxTokens->filename.GetString()) {
            return true;
        }
    }
    return false;
}

#ifdef HAS_COLOR_MANAGEMENT_SUPPORT_API
bool _MxHasFilenameInput(const HdMaterialNode2& inNode)
{
    mx::NodeDefPtr nodeDef
        = _GetMaterialXData()._mtlxLibrary->getNodeDef(inNode.nodeTypeId.GetString());
    if (nodeDef) {
        return _MxHasFilenameInput(nodeDef);
    }
    return false;
}
#endif

//! Helper function to generate a topo hash that can be used to detect if two networks share the
//  same topology.
size_t _GenerateNetwork2TopoHash(const HdMaterialNetwork2& materialNetwork)
{
#ifdef HAS_COLOR_MANAGEMENT_SUPPORT_API
    bool hasTextureNode = false;
#endif
    // The HdMaterialNetwork2 structure is stable. Everything is alphabetically sorted.
    size_t topoHash = 0;
    for (const auto& c : materialNetwork.terminals) {
        MayaUsd::hash_combine(topoHash, hash_value(c.first));
        MayaUsd::hash_combine(topoHash, hash_value(c.second.upstreamNode));
        MayaUsd::hash_combine(topoHash, hash_value(c.second.upstreamOutputName));
    }
    for (const auto& nodePair : materialNetwork.nodes) {
        MayaUsd::hash_combine(topoHash, hash_value(nodePair.first));

        const auto& node = nodePair.second;
#if MX_COMBINED_VERSION >= 13808
        TfToken optimizedNodeId = _GetMaterialXData()._lobePruner->getOptimizedNodeId(node);
        if (optimizedNodeId.IsEmpty()) {
            MayaUsd::hash_combine(topoHash, hash_value(node.nodeTypeId));
        } else {
            MayaUsd::hash_combine(topoHash, hash_value(optimizedNodeId));
        }
#else
        MayaUsd::hash_combine(topoHash, hash_value(node.nodeTypeId));
#endif
        if (_IsTopologicalNode(node)) {
            // We need to capture values that affect topology:
            for (auto const& p : node.parameters) {
                MayaUsd::hash_combine(topoHash, hash_value(p.first));
                MayaUsd::hash_combine(topoHash, hash_value(p.second));
            }
        }
#ifdef HAS_COLOR_MANAGEMENT_SUPPORT_API
        if (MayaUsd::ColorManagementPreferences::Active()) {
            // Explicit color management parameters affect topology:
#if PXR_VERSION < 2311
            for (auto&& cmName : _mtlxKnownColorSpaceAttrs) {
                auto cmIt = node.parameters.find(cmName);
                if (cmIt != node.parameters.end()) {
                    MayaUsd::hash_combine(topoHash, hash_value(cmIt->first));
                    if (cmIt->second.IsHolding<TfToken>()) {
                        auto const& colorSpace = cmIt->second.UncheckedGet<TfToken>();
                        MayaUsd::hash_combine(topoHash, hash_value(colorSpace));
                    } else if (cmIt->second.IsHolding<std::string>()) {
                        auto const& colorSpace = cmIt->second.UncheckedGet<std::string>();
                        MayaUsd::hash_combine(topoHash, std::hash<std::string> {}(colorSpace));
                    }
                }
            }
#else
            for (auto&& param : node.parameters) {
                if (_IsHydraColorSpace(param.first)) {
                    MayaUsd::hash_combine(topoHash, hash_value(param.first));
                    if (param.second.IsHolding<TfToken>()) {
                        auto const& colorSpace = param.second.UncheckedGet<TfToken>();
                        MayaUsd::hash_combine(topoHash, hash_value(colorSpace));
                    } else if (param.second.IsHolding<std::string>()) {
                        auto const& colorSpace = param.second.UncheckedGet<std::string>();
                        MayaUsd::hash_combine(topoHash, std::hash<std::string> {}(colorSpace));
                    }
                }
            }
#endif
            if (_MxHasFilenameInput(node)) {
                hasTextureNode = true;
            }
        }
#endif
        for (auto const& i : node.inputConnections) {
            MayaUsd::hash_combine(topoHash, hash_value(i.first));
            for (auto const& c : i.second) {
                MayaUsd::hash_combine(topoHash, hash_value(c.upstreamNode));
                MayaUsd::hash_combine(topoHash, hash_value(c.upstreamOutputName));
            }
        }
    }

    // The specular environment settings used affect the topology of the shader:
    MayaUsd::hash_combine(topoHash, MaterialXMaya::OgsFragment::getSpecularEnvKey());

#ifdef HAS_COLOR_MANAGEMENT_SUPPORT_API
    if (hasTextureNode) {
        MayaUsd::hash_combine(
            topoHash,
            std::hash<std::string> {}(
                MayaUsd::ColorManagementPreferences::RenderingSpaceName().asChar()));
    }
#endif

    return topoHash;
}

//! Helper function to generate a XML string about nodes, relationships and primvars in the
//! specified material network.
std::string _GenerateXMLString(const HdMaterialNetwork2& materialNetwork)
{
    std::ostringstream result;

    if (ARCH_LIKELY(!materialNetwork.nodes.empty())) {

        result << "<terminals>\n";
        for (const auto& c : materialNetwork.terminals) {
            result << "  <terminal name=\"" << c.first << "\" dest=\"" << c.second.upstreamNode
                   << "\"/>\n";
        }
        result << "</terminals>\n";
        result << "<nodes>\n";
        for (const auto& nodePair : materialNetwork.nodes) {
            const auto& node = nodePair.second;
            const bool  hasChildren = !(node.parameters.empty() && node.inputConnections.empty());
            result << "  <node path=\"" << nodePair.first << "\" id=\"" << node.nodeTypeId << "\""
                   << (hasChildren ? ">\n" : "/>\n");
            if (!node.parameters.empty()) {
                result << "    <parameters>\n";
                for (auto const& p : node.parameters) {
                    result << "      <param name=\"" << p.first << "\" value=\"" << p.second
                           << "\"/>\n";
                }
                result << "    </parameters>\n";
            }
            if (!node.inputConnections.empty()) {
                result << "    <inputs>\n";
                for (auto const& i : node.inputConnections) {
                    if (i.second.size() == 1) {
                        result << "      <input name=\"" << i.first << "\" dest=\""
                               << i.second.back().upstreamNode << "."
                               << i.second.back().upstreamOutputName << "\"/>\n";
                    } else {
                        // Extremely rare case seen only with array connections.
                        result << "      <input name=\"" << i.first << "\">\n";
                        result << "      <connections>\n";
                        for (auto const& c : i.second) {
                            result << "        <cnx dest=\"" << c.upstreamNode << "."
                                   << c.upstreamOutputName << "\"/>\n";
                        }
                        result << "      </connections>\n";
                    }
                }
                result << "    </inputs>\n";
            }
            if (hasChildren) {
                result << "  </node>\n";
            }
        }
        result << "</nodes>\n";
        // We do not add primvars. They are found later while traversing the actual effect instance.
    }

    return result.str();
}

// MaterialX FA nodes will "upgrade" the in2 uniform to whatever the vector type it needs for its
// arithmetic operation. So we need to "upgrade" the value we want to set as well.
//
// One example: ND_multiply_vector3FA(vector3 in1, float in2) will generate a float3 in2 uniform.
bool _IsFAParameter(const HdMaterialNode& node, const MString& paramName)
{
    auto _endsWith = [](const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size()
            && s.compare(s.size() - suffix.size(), std::string::npos, suffix) == 0;
    };

    if (_IsMaterialX(node) && _endsWith(paramName.asChar(), "_in2")
        && _endsWith(node.identifier.GetString(), "FA")) {
        return true;
    }
    return false;
}

// Recursively traverse a node graph, depth first, to find target node
mx::NodePtr _RecursiveFindNode(const mx::NodePtr& node, const TfToken& target)
{
    mx::NodePtr retVal;
    for (auto const& input : node->getInputs()) {
        if (mx::NodePtr downstreamNode = input->getConnectedNode()) {
            if (mx::NodeDefPtr nodeDef = downstreamNode->getNodeDef()) {
                if (nodeDef->getNodeString() == _mtlxTokens->geompropvalue.GetString()
                    && downstreamNode->getType() == _mtlxTokens->vector2.GetString()) {
                    retVal = downstreamNode;
                    break;
                }
            }
            retVal = _RecursiveFindNode(downstreamNode, target);
            if (retVal) {
                break;
            }
        }
    }
    return retVal;
}

// We have a few library surface nodes that require tangent inputs, but since the tangent input is
// not expressed in the interface, we will miss it in the _AddMissingTangents function. This
// function goes thru the NodeGraphs in the library and fixes the issue by adding the missing input.
//
// This is done only once, after the libraries have been read.
//
// We voluntarily did not expose a tangent input on the blinn and phong MaterialX nodes in order to
// test this code, but the real target is UsdPreviewSurface.
//
// Note for future self. In some far future, we might start seeing surface shaders that are built
// from other surface shaders. This might require percolating the tangent interface by re-running
// the main loop once for each expected nesting level (or until the loop runs without updating any
// NodeDef).
void _MaterialXData::_FixLibraryTangentInputs(mx::DocumentPtr& mtlxDoc)
{
    for (mx::NodeGraphPtr nodeGraph : mtlxDoc->getNodeGraphs()) {
        mx::NodeDefPtr graphDef = nodeGraph->getNodeDef();
        if (!graphDef) {
            continue;
        }
        auto outputs = graphDef->getActiveOutputs();
        if (outputs.empty()
            || outputs.front()->getType() != _mtlxTokens->surfaceshader.GetString()) {
            continue;
        }
        bool hasTangentInput = false;
        for (mx::InputPtr nodeInput : graphDef->getActiveInputs()) {
            if (nodeInput->hasDefaultGeomPropString()) {
                const std::string& geom = nodeInput->getDefaultGeomPropString();
                if (geom == _mtlxTokens->Tworld.GetString()
                    || geom == _mtlxTokens->Tobject.GetString()) {
                    hasTangentInput = true;
                    break;
                }
            }
        }
        if (hasTangentInput) {
            continue;
        }

        mx::InputPtr tangentInput;

        for (mx::NodePtr node : nodeGraph->getNodes()) {
            mx::NodeDefPtr nodeDef = node->getNodeDef();
            if (!nodeDef) {
#if MX_COMBINED_VERSION >= 13900
                // Can happen with legacy data. Just continue.
                continue;
#else
                break;
#endif
            }

            // Check the inputs of the node for Tworld and Tobject default geom properties
            for (mx::InputPtr input : nodeDef->getActiveInputs()) {
                if (input->hasDefaultGeomPropString()) {
                    const std::string& geomPropString = input->getDefaultGeomPropString();
                    if ((geomPropString == _mtlxTokens->Tworld.GetString()
                         || geomPropString == _mtlxTokens->Tobject.GetString())) {
                        const auto nodeInput = node->getInput(input->getName());
                        if (nodeInput && nodeInput->hasInterfaceName()) {
                            // Whatever created this NodeGraph implementation forgot to copy
                            // the default geom prop string to the interface:
                            auto defInput = graphDef->getInput(nodeInput->getInterfaceName());
                            if (defInput) {
                                defInput->setDefaultGeomPropString(geomPropString);
                            }
                        } else if (node->getConnectedNodeName(input->getName()).empty()) {
                            if (!tangentInput) {
                                tangentInput = graphDef->addInput(
                                    _mtlxTokens->tangent_fix.GetString(),
                                    _mtlxTokens->vector3.GetString());
                                tangentInput->setDefaultGeomPropString(geomPropString);
                            }
                            node->addInput(input->getName(), input->getType())
                                ->setInterfaceName(_mtlxTokens->tangent_fix.GetString());
                        }
                    }
                }
            }
        }
    }
}

// USD does not provide tangents, so we need to build them from UV coordinates when possible:
void _AddMissingTangents(mx::DocumentPtr& mtlxDoc)
{
    // We will need at least one geompropvalue reader to generate tangents:
    mx::NodePtr stReader;
    // If we find one implicit texcoord input we can still try to generate texcoord tangents:
    bool hasOneImplicitTexcoordInput = false;

    // List of all items to fix:
    using nodeInput = std::pair<mx::NodePtr, std::string>;
    std::vector<nodeInput>   graphTworldInputs;
    std::vector<nodeInput>   graphTobjectInputs;
    std::vector<nodeInput>   materialTworldInputs;
    std::vector<nodeInput>   materialTobjectInputs;
    std::vector<mx::NodePtr> nodesToReplace;

    // The materialnode very often will have a tangent input:
    for (mx::NodePtr material : mtlxDoc->getMaterialNodes()) {
        if (material->getName() != _mtlxTokens->USD_Mtlx_VP2_Material.GetText()) {
            continue;
        }
        mx::InputPtr surfaceInput = material->getInput(_mtlxTokens->surfaceshader.GetString());
        if (surfaceInput) {
            material = surfaceInput->getConnectedNode();
        }
        mx::NodeDefPtr nodeDef = material->getNodeDef();
        if (!nodeDef) {
            continue;
        }
        for (mx::InputPtr input : nodeDef->getActiveInputs()) {
            if (input->hasDefaultGeomPropString()) {
                const std::string& geomPropString = input->getDefaultGeomPropString();
                if (geomPropString == _mtlxTokens->Tworld.GetString()
                    && !material->getConnectedOutput(input->getName())) {
                    materialTworldInputs.emplace_back(material, input->getName());
                }
                if (geomPropString == _mtlxTokens->Tobject.GetString()
                    && !material->getConnectedOutput(input->getName())) {
                    materialTobjectInputs.emplace_back(material, input->getName());
                }
                if (geomPropString == _mtlxTokens->UV0
                    && !material->getConnectedOutput(input->getName())) {
                    hasOneImplicitTexcoordInput = true;
                }
            }
        }
    }

    // If we have no nodegraph, but need tangent input on the material node, then we create one:
    mx::NodeGraphPtr nodeGraph = mtlxDoc->getNodeGraph(_mtlxTokens->NG_Maya.GetString());
    if (!nodeGraph && (!materialTworldInputs.empty() || !materialTobjectInputs.empty())) {
        nodeGraph = mtlxDoc->addNodeGraph(_mtlxTokens->NG_Maya.GetString());
    }

    if (nodeGraph) {
        for (mx::NodePtr node : nodeGraph->getNodes()) {
            mx::NodeDefPtr nodeDef = node->getNodeDef();
            // A missing node def is a very bad sign. No need to process further.
            if (!TF_VERIFY(
                    nodeDef,
                    "Could not find MaterialX NodeDef for Node '%s'. Please recheck library paths.",
                    node->getNamePath().c_str())) {
                return;
            }

            if (!stReader && nodeDef->getNodeString() == _mtlxTokens->geompropvalue.GetString()
                && node->getType() == _mtlxTokens->vector2.GetString()) {
                // Grab the first st reader we can find. This will be the default one used for
                // tangents unless we find something better.
                stReader = node;
                continue;
            }

            if (nodeDef->getNodeString() == _mtlxTokens->normalmap.GetString()) {
                // That one is even more important, because the texcoord reader attached to the
                // image is definitely the one we want for our tangents since it is used for normal
                // mapping:
                mx::NodePtr downstreamReader = _RecursiveFindNode(node, _mtlxTokens->geompropvalue);
                if (downstreamReader) {
                    stReader = downstreamReader;
                }
            }

            // Check the inputs of the node for Tworld and Tobject default geom properties
            for (mx::InputPtr input : nodeDef->getActiveInputs()) {
                if (input->hasDefaultGeomPropString()) {
                    const std::string& geomPropString = input->getDefaultGeomPropString();
                    if (geomPropString == _mtlxTokens->Tworld.GetString()
                        && node->getConnectedNodeName(input->getName()).empty()) {
                        graphTworldInputs.emplace_back(node, input->getName());
                    }
                    if (geomPropString == _mtlxTokens->Tobject.GetString()
                        && node->getConnectedNodeName(input->getName()).empty()) {
                        graphTobjectInputs.emplace_back(node, input->getName());
                    }
                    if (geomPropString == _mtlxTokens->UV0
                        && node->getConnectedNodeName(input->getName()).empty()) {
                        hasOneImplicitTexcoordInput = true;
                    }
                }
            }
            // Check if it is an explicit tangent reader:
            if (nodeDef->getNodeString() == _mtlxTokens->tangent.GetString()) {
                nodesToReplace.push_back(node);
            }
        }

        if (nodesToReplace.empty() && graphTworldInputs.empty() && graphTobjectInputs.empty()
            && materialTworldInputs.empty() && materialTobjectInputs.empty()) {
            // Nothing to do.
            return;
        }

        // Create the tangent generator:
        mx::NodePtr tangentGenerator;
        if (stReader || hasOneImplicitTexcoordInput) {
            tangentGenerator = nodeGraph->addNode(
                _mtlxTokens->texcoordtangents.GetString(),
                _mtlxTokens->Tw_reader.GetString(),
                _mtlxTokens->vector3.GetString());
            tangentGenerator->addInput(
                _mtlxTokens->texcoord.GetString(), _mtlxTokens->vector2.GetString());
            if (stReader) {
                // Use an explicit geomprop reader if one was found, otherwise, leave it to the
                // implicit geomprop reader code in shadergen.
                tangentGenerator->setConnectedNodeName(
                    _mtlxTokens->texcoord.GetString(), stReader->getName());
            }
        } else {
            tangentGenerator = nodeGraph->addNode(
                _mtlxTokens->arbitrarytangents.GetString(),
                _mtlxTokens->Tw_reader.GetString(),
                _mtlxTokens->vector3.GetString());
        }

        // We create a world -> object transformation on demand. Computing an object tangent from
        // object space normal and position might be more precise though.
        mx::NodePtr transformVectorToObject;
        mx::NodePtr transformVectorToModel;
        auto        _createTransformVector = [&](const TfToken& toSpace) {
            mx::NodePtr retVal = nodeGraph->addNode(
                _mtlxTokens->transformvector.GetString(),
                _mtlxTokens->Tw_to_To.GetString(),
                _mtlxTokens->vector3.GetString());
            retVal->addInput(_mtlxTokens->in.GetString(), _mtlxTokens->vector3.GetString());
            retVal->setConnectedNodeName(_mtlxTokens->in.GetString(), tangentGenerator->getName());
            retVal->addInput(_mtlxTokens->fromspace.GetString(), _mtlxTokens->string.GetString())
                ->setValueString(_mtlxTokens->world.GetString());
            retVal->addInput(_mtlxTokens->tospace.GetString(), _mtlxTokens->string.GetString())
                ->setValueString(toSpace.GetString());
            return retVal;
        };

        // Reconnect nodes that require Tworld to the tangent generator:
        for (const auto& nodeInput : graphTworldInputs) {
            nodeInput.first->addInput(nodeInput.second, _mtlxTokens->vector3.GetString());
            nodeInput.first->setConnectedNodeName(nodeInput.second, tangentGenerator->getName());
        }

        // Reconnect nodes that require Tobject to the tangent generator via a space transform:
        for (const auto& nodeInput : graphTobjectInputs) {
            if (!transformVectorToObject) {
                transformVectorToObject = _createTransformVector(_mtlxTokens->object);
            }
            nodeInput.first->addInput(nodeInput.second, _mtlxTokens->vector3.GetString());
            nodeInput.first->setConnectedNodeName(
                nodeInput.second, transformVectorToObject->getName());
        }

        // Connect Tworld inputs on the material via an output port on the nodegraph:
        mx::OutputPtr outTworld = nodeGraph->getOutput(_mtlxTokens->outTworld.GetString());
        for (const auto& nodeInput : materialTworldInputs) {
            if (!outTworld) {
                outTworld = nodeGraph->addOutput(
                    _mtlxTokens->outTworld.GetString(), _mtlxTokens->vector3.GetString());
                outTworld->setConnectedNode(tangentGenerator);
            }
            nodeInput.first->addInput(nodeInput.second, _mtlxTokens->vector3.GetString());
            nodeInput.first->setConnectedOutput(nodeInput.second, outTworld);
        }

        // Connect Tobject inputs on the material via an output port on the nodegraph:
        mx::OutputPtr outTobject = nodeGraph->getOutput(_mtlxTokens->outTobject.GetString());
        for (const auto& nodeInput : materialTobjectInputs) {
            if (!outTobject) {
                outTobject = nodeGraph->addOutput(
                    _mtlxTokens->outTworld.GetString(), _mtlxTokens->vector3.GetString());
                if (!transformVectorToObject) {
                    transformVectorToObject = _createTransformVector(_mtlxTokens->object);
                }
                outTobject->setConnectedNode(transformVectorToObject);
            }
            nodeInput.first->addInput(nodeInput.second, _mtlxTokens->vector3.GetString());
            nodeInput.first->setConnectedOutput(nodeInput.second, outTobject);
        }

        // We will replace tangent nodes with a passthru that feeds on the tangent generator. A
        // space transform will be used when appropriate.
        auto replaceWithPassthru = [&](mx::NodePtr& toReplace, mx::NodePtr& newSource) {
            std::string nodeName = toReplace->getName();

            nodeGraph->removeNode(nodeName);

            mx::NodePtr passthruNode = nodeGraph->addNode(
                _mtlxTokens->constant.GetString(), nodeName, _mtlxTokens->vector3.GetString());
            passthruNode->addInput(
                _mtlxTokens->value.GetString(), _mtlxTokens->vector3.GetString());
            passthruNode->setConnectedNodeName(
                _mtlxTokens->value.GetString(), newSource->getName());
        };

        for (auto& tangentNode : nodesToReplace) {
            mx::InputPtr spaceInput = tangentNode->getInput(_mtlxTokens->space.GetString());
            if (spaceInput) {
                if (spaceInput->getValueString() == _mtlxTokens->object.GetString()) {
                    if (!transformVectorToObject) {
                        transformVectorToObject = _createTransformVector(_mtlxTokens->object);
                    }
                    replaceWithPassthru(tangentNode, transformVectorToObject);
                    continue;
                } else if (spaceInput->getValueString() == _mtlxTokens->model.GetString()) {
                    if (!transformVectorToModel) {
                        transformVectorToModel = _createTransformVector(_mtlxTokens->model);
                    }
                    replaceWithPassthru(tangentNode, transformVectorToModel);
                    continue;
                }
            }
            // Default to world.
            replaceWithPassthru(tangentNode, tangentGenerator);
        }
    }
}

#if MX_COMBINED_VERSION >= 13900
// MaterialX 1.39 adds a few bitangent inputs to the standard library nodes. We need to explicitly
// compute them from the cross product of the tangent and normal vectors.
void _AddMissingBitangents(mx::DocumentPtr& mtlxDoc)
{
    // If we have no nodegraph, but need tangent input on the material node, then we create one:
    mx::NodeGraphPtr nodeGraph = mtlxDoc->getNodeGraph(_mtlxTokens->NG_Maya.GetString());
    if (!nodeGraph) {
        return;
    }

    std::vector<mx::NodePtr> nodesToFix = nodeGraph->getNodes(_mtlxTokens->normalmap.GetString());
    const auto               bumpNodes = nodeGraph->getNodes(_mtlxTokens->bump.GetString());
    nodesToFix.insert(nodesToFix.end(), bumpNodes.begin(), bumpNodes.end());

    for (mx::NodePtr node : nodesToFix) {
        mx::NodeDefPtr nodeDef = node->getNodeDef();
        // A missing node def is a very bad sign. No need to process further.
        if (!TF_VERIFY(
                nodeDef,
                "Could not find MaterialX NodeDef for Node '%s'. Please recheck library paths.",
                node->getNamePath().c_str())) {
            continue;
        }

        // Is the Bitangent input connected?
        mx::InputPtr bitangentInput = node->getInput(_mtlxTokens->bitangent.GetString());
        if (bitangentInput && bitangentInput->getConnectedNode()) {
            continue;
        }

        // Get the tangent input. We expect it to be connected since we get called after
        // _AddMissingTangents has run.
        mx::InputPtr tangentInput = node->getInput(_mtlxTokens->tangent.GetString());
        if (!tangentInput) {
            continue;
        }

        // Get the normal input. It can possibly be unconnected.
        mx::InputPtr normalInput = node->getInput(_mtlxTokens->normal.GetString());

        // Create the cross product node:
        mx::NodePtr crossProductNode = nodeGraph->addNode(
            _mtlxTokens->crossproduct.GetString(),
            _mtlxTokens->crossproduct.GetString(),
            _mtlxTokens->vector3.GetString());

        mx::InputPtr crossProductInput1 = crossProductNode->addInput(
            _mtlxTokens->in1.GetString(), _mtlxTokens->vector3.GetString());
        mx::InputPtr crossProductInput2 = crossProductNode->addInput(
            _mtlxTokens->in2.GetString(), _mtlxTokens->vector3.GetString());

        crossProductInput2->setConnectedNode(tangentInput->getConnectedNode());
        if (normalInput && normalInput->getConnectedNode()) {
            crossProductInput1->setConnectedNode(normalInput->getConnectedNode());
        } else {
            // Add a normal node to the graph:
            mx::NodePtr normalNode = nodeGraph->addNode(
                _mtlxTokens->normal.GetString(),
                _mtlxTokens->normal.GetString(),
                _mtlxTokens->vector3.GetString());
            normalNode->addInputFromNodeDef("space")->setValueString("world");
            crossProductInput1->setConnectedNode(normalNode);
        }
        bitangentInput
            = node->addInput(_mtlxTokens->bitangent.GetString(), _mtlxTokens->vector3.GetString());
        bitangentInput->setConnectedNode(crossProductNode);
    }
}

#endif

#endif // WANT_MATERIALX_BUILD

#if PXR_VERSION <= 2211
bool _IsUsdDrawModeId(const TfToken& id)
{
    return id == _tokens->DrawMode || id == _tokens->UsdDrawModeCards;
}

bool _IsUsdDrawModeNode(const HdMaterialNode& node) { return _IsUsdDrawModeId(node.identifier); }

bool _IsUsdFloat2PrimvarReader(const HdMaterialNode& node)
{
    return (node.identifier == UsdImagingTokens->UsdPrimvarReader_float2);
}
#endif

//! Helper utility function to test whether a node is a UsdShade primvar reader.
bool _IsUsdPrimvarReader(const HdMaterialNode& node)
{
    const TfToken& id = node.identifier;
    return (
        id == UsdImagingTokens->UsdPrimvarReader_float
        || id == UsdImagingTokens->UsdPrimvarReader_float2
        || id == UsdImagingTokens->UsdPrimvarReader_float3
        || id == UsdImagingTokens->UsdPrimvarReader_float4 || id == _tokens->UsdPrimvarReader_vector
        || id == UsdImagingTokens->UsdPrimvarReader_int);
}

//! Helper utility function to test whether a node is a UsdShade UV texture.
bool _IsUsdUVTexture(const HdMaterialNode& node)
{
    if (node.identifier.GetString().rfind(UsdImagingTokens->UsdUVTexture.GetString(), 0) == 0) {
        return true;
    }

#ifdef WANT_MATERIALX_BUILD
    if (_IsMaterialX(node)) {
        mx::NodeDefPtr nodeDef
            = _GetMaterialXData()._mtlxLibrary->getNodeDef(node.identifier.GetString());
        return nodeDef && _MxHasFilenameInput(nodeDef);
    }
#endif

    return false;
}

bool _IsTextureFilenameAttribute(const HdMaterialNode& node, const TfToken& token)
{

    if (node.identifier.GetString().rfind(UsdImagingTokens->UsdUVTexture.GetString(), 0) == 0
        && token == _tokens->file) {
        return true;
    }

#ifdef WANT_MATERIALX_BUILD
    if (_IsMaterialX(node)) {
        mx::NodeDefPtr nodeDef
            = _GetMaterialXData()._mtlxLibrary->getNodeDef(node.identifier.GetString());
        if (nodeDef) {
            const auto input = nodeDef->getActiveInput(token.GetString());
            if (input && input->getType() == _mtlxTokens->filename.GetString()) {
                return true;
            }
        }
    }
#endif

    return false;
}

//! Helper function to generate a XML string about nodes, relationships and primvars in the
//! specified material network.
std::string _GenerateXMLString(const HdMaterialNetwork& materialNetwork, bool includeParams = true)
{
    std::string result;

    if (ARCH_LIKELY(!materialNetwork.nodes.empty())) {
        // Reserve enough memory to avoid memory reallocation.
        result.reserve(1024);

        result += "<nodes>\n";

        if (includeParams) {
            for (const HdMaterialNode& node : materialNetwork.nodes) {
                result += "  <node path=\"";
                result += node.path.GetString();
                result += "\" id=\"";
                result += node.identifier;
                result += "\">\n";

                result += "    <params>\n";

                for (auto const& parameter : node.parameters) {
                    result += "      <param name=\"";
                    result += parameter.first;
                    result += "\" value=\"";
                    result += TfStringify(parameter.second);
                    result += "\"/>\n";
                }

                result += "    </params>\n";

                result += "  </node>\n";
            }
        } else {
            for (const HdMaterialNode& node : materialNetwork.nodes) {
                result += "  <node path=\"";
                result += node.path.GetString();
                result += "\" id=\"";
                result += node.identifier;
                result += "\"/>\n";
            }
        }

        result += "</nodes>\n";

        if (!materialNetwork.relationships.empty()) {
            result += "<relationships>\n";

            for (const HdMaterialRelationship& rel : materialNetwork.relationships) {
                result += "  <rel from=\"";
                result += rel.inputId.GetString();
                result += ".";
                result += rel.inputName;
                result += "\" to=\"";
                result += rel.outputId.GetString();
                result += ".";
                result += rel.outputName;
                result += "\"/>\n";
            }

            result += "</relationships>\n";
        }

        if (!materialNetwork.primvars.empty()) {
            result += "<primvars>\n";

            for (TfToken const& primvar : materialNetwork.primvars) {
                result += "  <primvar name=\"";
                result += primvar;
                result += "\"/>\n";
            }

            result += "</primvars>\n";
        }
    }

    return result;
}

#ifdef HAS_COLOR_MANAGEMENT_SUPPORT_API
void _AddColorManagementFragments(HdMaterialNetwork& net)
{
    if (!MayaUsd::ColorManagementPreferences::Active()) {
        return;
    }

    MHWRender::MRenderer* theRenderer = MHWRender::MRenderer::theRenderer();
    if (!theRenderer) {
        return;
    }

    MHWRender::MFragmentManager* fragmentManager = theRenderer->getFragmentManager();
    if (!fragmentManager) {
        return;
    }

    // This will help keep our color management (CM) path identifiers unique:
    size_t cmCounter = net.nodes.size();

    auto netNodesEnd = net.nodes.end();
    for (auto it = net.nodes.begin(); it != netNodesEnd; ++it) {
        auto node = *it;
        if (!_IsUsdUVTexture(node)) {
            continue;
        }

        // Need to find the source color space:
        TfToken sourceColorSpace = _tokens->auto_;
        auto    scsIt = node.parameters.find(_tokens->sourceColorSpace);
        if (scsIt != node.parameters.end()) {
            const VtValue& scsValue = scsIt->second;
            if (scsValue.IsHolding<TfToken>()) {
                sourceColorSpace = scsValue.UncheckedGet<TfToken>();
            }
            if (scsValue.IsHolding<std::string>()) {
                sourceColorSpace = TfToken(scsValue.UncheckedGet<std::string>());
            }
        }

        // We need to insert the proper CM shader fragment here so it becomes part
        // of the material hash:
        MString colorSpace;

        if (sourceColorSpace == _tokens->auto_) {
            auto fileIt = node.parameters.find(_tokens->file);
            if (fileIt == node.parameters.end() || !fileIt->second.IsHolding<SdfAssetPath>()) {
                continue;
            }
            auto const& filenameVal = fileIt->second.Get<SdfAssetPath>();
            auto const& resolvedPath = filenameVal.GetResolvedPath();
            if (resolvedPath.empty()) {
                continue;
            }
            colorSpace = MayaUsd::ColorManagementPreferences::getFileRule(resolvedPath).c_str();
        } else if (sourceColorSpace == _tokens->sRGB) {
            if (MayaUsd::ColorManagementPreferences::sRGBName().isEmpty()) {
                // No alias found. Do not color correct...
                continue;
            }
            colorSpace = MayaUsd::ColorManagementPreferences::sRGBName();
        } else if (sourceColorSpace == _tokens->raw) {
            // No cm necessary for raw:
            continue;
        } else {
            // Let's see if OCIO knows about that one:
            colorSpace = sourceColorSpace.GetText();
        }

        MString fragName, inputName, outputName;
        if (!MayaUsd::ColorManagementPreferences::isUnknownColorSpace(colorSpace.asChar())) {
            MStatus status = fragmentManager->getColorManagementFragmentInfo(
                colorSpace, fragName, inputName, outputName);
            if (!status) {
                // Maya does not know about this color space. Remember that.
                MayaUsd::ColorManagementPreferences::addUnknownColorSpace(colorSpace.asChar());
                continue;
            }
        } else {
            // Don't know how to handle that color space.
            continue;
        }

        // Move all tx output relations to the new cm node
        bool   hadOneColorOutput = false;
        size_t numPassthrough = 0;

        // Explicitly not color correcting alpha connections:
        // Should we consider XYZ vector output to not require CM?
        static const auto sColorOutputs
            = std::set<TfToken> { _tokens->rgb, _tokens->xyz, _tokens->r, _tokens->x,
                                  _tokens->g,   _tokens->y,   _tokens->b, _tokens->z };
        static const auto sChannelSelectorMap = std::map<TfToken, TfToken> {
            { _tokens->r, _tokens->Float3ToFloatX }, { _tokens->x, _tokens->Float3ToFloatX },
            { _tokens->g, _tokens->Float3ToFloatY }, { _tokens->y, _tokens->Float3ToFloatY },
            { _tokens->b, _tokens->Float3ToFloatZ }, { _tokens->z, _tokens->Float3ToFloatZ },
            { _tokens->a, _tokens->Float4ToFloatW }, { _tokens->w, _tokens->Float4ToFloatW }
        };

        for (HdMaterialRelationship& rel : net.relationships) {
            if (rel.inputId == node.path) {
                if (sColorOutputs.count(rel.inputName)) {
                    // We need to add a cm node:
                    hadOneColorOutput = true;
                }
                if (sChannelSelectorMap.count(rel.inputName)) {
                    // Count how many extra passthrough will be needed
                    ++numPassthrough;
                }
            }
        }

        if (hadOneColorOutput) {
            // Need to keep the topological sort order of the network, so need to be careful with
            // our iterators.

            // Using the distance from start of the vector is safe against reallocation:
            auto itDistance = std::distance(net.nodes.begin(), it);
            net.nodes.reserve(net.nodes.size() + 2 + numPassthrough);

            // Need a color4 to color3 passthrough (PT) between the texture (TX) and the color
            // management (CM) node:
            HdMaterialNode ptNode;
            ptNode.identifier = _tokens->Float4ToFloat3;
            ptNode.path = SdfPath(_tokens->toColor3ForCM.GetString() + std::to_string(++cmCounter));

            // Insert it directly after the TX node:
            ++itDistance;
            net.nodes.insert(net.nodes.begin() + itDistance, ptNode);

            // Now add the CM node:
            HdMaterialNode cmNode;
            cmNode.identifier = TfToken(fragName.asChar());
            cmNode.path = SdfPath(
                _tokens->Maya_OCIO_.GetString() + std::to_string(cmNode.identifier.Hash()) + "_"
                + std::to_string(++cmCounter));

            // Directly after the PT node
            ++itDistance;
            net.nodes.insert(net.nodes.begin() + itDistance, cmNode);

            // Add a relationship between the TX and PT
            std::vector<HdMaterialRelationship> newRelationships;
            HdMaterialRelationship              newRel
                = { node.path, _tokens->output, ptNode.path, _tokens->input };
            newRelationships.push_back(newRel);

            // Add a relationship between the PT and CM
            newRel = { ptNode.path, _tokens->output, cmNode.path, TfToken(inputName.asChar()) };
            newRelationships.push_back(newRel);

            std::map<TfToken, SdfPath> addedChannelSelectors;

            // Now reconnect all TX -> destination relationships to the proper passthrough:
            for (HdMaterialRelationship& rel : net.relationships) {
                if (rel.inputId != node.path) {
                    continue;
                }
                // Do we need a channel selector (CS) to extract a component:
                auto exIt = sChannelSelectorMap.find(rel.inputName);

                if (exIt == sChannelSelectorMap.end()) {
                    // No CS necessary: update connection between TX and destination:
                    rel.inputId = cmNode.path;
                    rel.inputName = TfToken(outputName.asChar());
                } else {
                    // Add CS between CM and destination, but only once per channel
                    auto csIt = addedChannelSelectors.find(exIt->second);
                    if (csIt == addedChannelSelectors.end()) {
                        // That CS needs to be added:
                        HdMaterialNode csNode;
                        csNode.identifier = exIt->second;
                        csNode.path = SdfPath(
                            _tokens->extract.GetString() + exIt->second.GetString()
                            + std::to_string(++cmCounter));

                        // Directly after the CM node
                        ++itDistance;
                        net.nodes.insert(net.nodes.begin() + itDistance, csNode);

                        if (exIt->second != _tokens->Float4ToFloatW) {
                            // Add relationship between the CM and CS
                            newRel = { cmNode.path,
                                       TfToken(outputName.asChar()),
                                       csNode.path,
                                       _tokens->input };
                            newRelationships.push_back(newRel);
                        } else {
                            // Alpha channel connects directly from TX to CS:
                            newRel = { node.path, _tokens->output, csNode.path, _tokens->input };
                            newRelationships.push_back(newRel);
                        }

                        csIt = addedChannelSelectors.insert({ exIt->second, csNode.path }).first;
                    }

                    // Repath new relationship between the CS and the destination:
                    rel.inputId = csIt->second;
                    rel.inputName = _tokens->output;
                }
            }
            // Add all new relationships:
            net.relationships.insert(
                net.relationships.end(), newRelationships.begin(), newRelationships.end());

            // Update for loop iterators:
            it = net.nodes.begin() + itDistance;
            netNodesEnd = net.nodes.end();
        }
    }

    if (TfDebug::IsEnabled(HDVP2_DEBUG_MATERIAL)) {
        std::cout << "Color managed network:\n" << _GenerateXMLString(net, true) << "\n";
    }
}
#endif

//! Determines if the shader uses transparency for geometric cut-out, meaning the material would
//! be tagged as 'masked' (HdStMaterialTagTokens->masked) by HdStorm.
//! In this case, the shader instance typically discards transparent fragments and will render
//! others as fully opaque thus without alpha blending.
//! Inspired by:
//! https://github.com/PixarAnimationStudios/OpenUSD/blob/59992d2178afcebd89273759f2bddfe730e59aa8/pxr/imaging/hdSt/materialNetwork.cpp#L59
//! https://github.com/PixarAnimationStudios/OpenUSD/blob/59992d2178afcebd89273759f2bddfe730e59aa8/pxr/imaging/hdSt/materialXFilter.cpp#L754
bool _IsMaskedTransparency(const HdMaterialNetwork& network)
{
    const HdMaterialNode& surfaceShader = network.nodes.back();

    auto testParamValue = [&](const TfToken& name, auto&& predicate, auto rhsVal) {
        using ValueT = std::decay_t<decltype(rhsVal)>;

        const auto itr = surfaceShader.parameters.find(name);
        if (itr == surfaceShader.parameters.end() || !itr->second.IsHolding<ValueT>())
            return false;

        if (!predicate(itr->second.UncheckedGet<ValueT>(), rhsVal))
            return false;

        // Check if any connection to the param makes its value vary.
        return std::none_of(
            network.relationships.begin(),
            network.relationships.end(),
            [&surfaceShader, &name](const HdMaterialRelationship& rel) {
                return (rel.outputId == surfaceShader.path) && (rel.outputName == name);
            });
    };

#ifdef WANT_MATERIALX_BUILD
#if PXR_VERSION >= 2505
    const auto node
        = SdrRegistry::GetInstance().GetShaderNodeByIdentifier(surfaceShader.identifier);
#else
    const auto node = SdrRegistry::GetInstance().GetNodeByIdentifier(surfaceShader.identifier);
#endif

    // Handle MaterialX shaders.
    if (node->GetSourceType() == HdVP2Tokens->mtlx) {
        // Check UsdPreviewSurface node based on opacityThreshold.
        if (node->GetFamily() == UsdImagingTokens->UsdPreviewSurface) {
            return testParamValue(_tokens->opacityThreshold, std::greater<>(), 0.0f);
        }
        // Check if glTF PBR's alpha_mode is `MASK` and that transmission is disabled.
        if (node->GetFamily() == _tokens->gltf_pbr) {
            return testParamValue(_tokens->alpha_mode, std::equal_to<>(), 1)
                && testParamValue(_tokens->transmission, std::equal_to<>(), 0.0f);
        }
        // Unhandled MaterialX terminal.
        return false;
    }
#endif
    // Handle all glslfx surface nodes based on opacityThreshold.
    return testParamValue(_tokens->opacityThreshold, std::greater<>(), 0.0f);
}

//! Return true if the surface shader needs to be rendered in a transparency pass.
bool _IsTransparent(const HdMaterialNetwork& network)
{
    // Masked transparency will not produce semi-transparency and can be rendered in opaque pass.
    if (_IsMaskedTransparency(network)) {
        return false;
    }

    using OpaqueTestPair = std::pair<TfToken, float>;
    using OpaqueTestPairList = std::vector<OpaqueTestPair>;
    const OpaqueTestPairList inputPairList
        = { { _tokens->opacity, 1.0f },         { _tokens->existence, 1.0f },
            { _tokens->alpha, 1.0f },           { _tokens->transmission, 0.0f },
            { _tokens->transparency, 0.0f },    { _tokens->transmission_weight, 0.0f },
            { _tokens->geometry_opacity, 1.0f } };

    const HdMaterialNode& surfaceShader = network.nodes.back();

    // Check for explicitly set value:
    for (auto&& inputPair : inputPairList) {
        auto paramIt = surfaceShader.parameters.find(inputPair.first);
        if (paramIt != surfaceShader.parameters.end()) {
            if (paramIt->second.IsHolding<float>()) {
                if (paramIt->second.Get<float>() != inputPair.second) {
                    return true;
                }
            } else if (paramIt->second.IsHolding<GfVec3f>()) {
                const GfVec3f& value = paramIt->second.Get<GfVec3f>();
                if (value != GfVec3f(inputPair.second, inputPair.second, inputPair.second)) {
                    return true;
                }
            } else if (paramIt->second.IsHolding<GfVec3d>()) {
                const GfVec3d& value = paramIt->second.Get<GfVec3d>();
                if (value != GfVec3d(inputPair.second, inputPair.second, inputPair.second)) {
                    return true;
                }
            }
        }
    }

    // Check for any connection on a parameter affecting transparency. It is quite possible that the
    // output value of the connected subtree is constant and opaque, but discovery is complex. Let's
    // assume there is at least one semi-transparent pixel if that parameter is connected.
    for (const HdMaterialRelationship& rel : network.relationships) {
        if (rel.outputId == surfaceShader.path) {
            for (auto&& inputPair : inputPairList) {
                if (inputPair.first == rel.outputName) {
                    return true;
                }
            }
        }
    }

    // Not the most efficient code, but the upgrade to HdMaterialConnection2 should improve things
    // a lot since we will not have to traverse the whole list of connections, only the ones found
    // on the surface node.

    return false;
}

//! Helper utility function to convert Hydra texture addressing token to VP2 enum.
MHWRender::MSamplerState::TextureAddress _ConvertToTextureSamplerAddressEnum(const TfToken& token)
{
    MHWRender::MSamplerState::TextureAddress address;

    if (token == UsdHydraTokens->clamp) {
        address = MHWRender::MSamplerState::kTexClamp;
    } else if (token == UsdHydraTokens->mirror) {
        address = MHWRender::MSamplerState::kTexMirror;
    } else if (token == UsdHydraTokens->black) {
        address = MHWRender::MSamplerState::kTexBorder;
    } else {
        address = MHWRender::MSamplerState::kTexWrap;
    }

    return address;
}

//! Get sampler state description as required by the material node.
MHWRender::MSamplerStateDesc _GetSamplerStateDesc(const HdMaterialNode& node)
{
    TF_VERIFY(_IsUsdUVTexture(node));

    MHWRender::MSamplerStateDesc desc;
    desc.filter = MHWRender::MSamplerState::kMinMagMipLinear;

#ifdef WANT_MATERIALX_BUILD
    const bool isMaterialXNode = _IsMaterialX(node);
    auto       it
        = node.parameters.find(isMaterialXNode ? _mtlxTokens->uaddressmode : UsdHydraTokens->wrapS);
#else
    auto it = node.parameters.find(UsdHydraTokens->wrapS);
#endif
    if (it != node.parameters.end()) {
        const VtValue& value = it->second;
        if (value.IsHolding<TfToken>()) {
            const TfToken& token = value.UncheckedGet<TfToken>();
            desc.addressU = _ConvertToTextureSamplerAddressEnum(token);
        }
#ifdef WANT_MATERIALX_BUILD
        if (value.IsHolding<std::string>()) {
            TfToken token(value.UncheckedGet<std::string>().c_str());
            desc.addressU = _ConvertToTextureSamplerAddressEnum(token);
        }
#endif
    }

#ifdef WANT_MATERIALX_BUILD
    it = node.parameters.find(isMaterialXNode ? _mtlxTokens->vaddressmode : UsdHydraTokens->wrapT);
#else
    it = node.parameters.find(UsdHydraTokens->wrapT);
#endif
    if (it != node.parameters.end()) {
        const VtValue& value = it->second;
        if (value.IsHolding<TfToken>()) {
            const TfToken& token = value.UncheckedGet<TfToken>();
            desc.addressV = _ConvertToTextureSamplerAddressEnum(token);
        }
#ifdef WANT_MATERIALX_BUILD
        if (value.IsHolding<std::string>()) {
            TfToken token(value.UncheckedGet<std::string>().c_str());
            desc.addressV = _ConvertToTextureSamplerAddressEnum(token);
        }
#endif
    }

#ifdef WANT_MATERIALX_BUILD
    if (isMaterialXNode) {
        it = node.parameters.find(_mtlxTokens->filtertype);
        if (it != node.parameters.end()) {
            const VtValue& value = it->second;
            if (value.IsHolding<std::string>()) {
                TfToken token(value.UncheckedGet<std::string>().c_str());
                if (token == _mtlxTokens->closest) {
                    desc.filter = MHWRender::MSamplerState::kMinMagMipPoint;
                    desc.maxLOD = 0;
                    desc.minLOD = 0;
                } else if (token == _mtlxTokens->cubic) {
                    desc.filter = MHWRender::MSamplerState::kAnisotropic;
                    desc.maxAnisotropy = 16;
                }
            }
        }
    }
#endif

    it = node.parameters.find(_tokens->fallback);
    if (it != node.parameters.end()) {
        const VtValue& value = it->second;
        if (value.IsHolding<GfVec4f>()) {
            const GfVec4f& fallbackValue = value.UncheckedGet<GfVec4f>();
            float const*   value = fallbackValue.data();
            std::copy(value, value + 4, desc.borderColor);
        }
    }

    return desc;
}

MHWRender::MTexture*
_LoadUdimTexture(const std::string& path, bool& isColorSpaceSRGB, MFloatArray& uvScaleOffset)
{
    /*
        For this method to work path needs to be an absolute file path, not an asset path.
        That means that this function depends on the changes in 4e426565 to materialAdapther.cpp
        to work. As of my writing this 4e426565 is not in the USD that MayaUSD normally builds
        against so this code will fail, because UsdImaging_GetUdimTiles won't file the tiles
        because we don't know where on disk to look for them.

        https://github.com/PixarAnimationStudios/USD/commit/4e42656543f4e3a313ce31a81c27477d4dcb64b9
    */

    // test for a UDIM texture
    if (!HdStIsSupportedUdimTexture(path))
        return nullptr;

    /*
        Maya's tiled texture support is implemented quite differently from Usd's UDIM support.
        In Maya the texture tiles get combined into a single big texture, downscaling each tile
        if necessary, and filling in empty regions of a non-square tile with the undefined color.

        In USD the UDIM textures are stored in a texture array that the shader uses to draw.
    */

    MHWRender::MRenderer* const       renderer = MHWRender::MRenderer::theRenderer();
    MHWRender::MTextureManager* const textureMgr
        = renderer ? renderer->getTextureManager() : nullptr;
    if (!TF_VERIFY(textureMgr)) {
        return nullptr;
    }

    MHWRender::MTexture* texture = textureMgr->findTexture(path.c_str());
    if (texture) {
        return texture;
    }

    // HdSt sets the tile limit to the max number of textures in an array of 2d textures. OpenGL
    // says the minimum number of layers in 2048 so I'll use that.
    int                                   tileLimit = 2048;
    std::vector<std::tuple<int, TfToken>> tiles = UsdImaging_GetUdimTiles(path, tileLimit);
    if (tiles.size() == 0) {
        TF_WARN("Unable to find UDIM tiles for %s", path.c_str());
        return nullptr;
    }

    // I don't think there is a downside to setting a very high limit.
    // Maya will clamp the texture size to the VP2 texture clamp resolution and the hardware's
    // max texture size. And Maya doesn't make the tiled texture unnecessarily large. When I
    // try loading two 1k textures I end up with a tiled texture that is 2k x 1k.
    unsigned int maxWidth = 0;
    unsigned int maxHeight = 0;
    renderer->GPUmaximumOutputTargetSize(maxWidth, maxHeight);

    // Open the first image and get it's resolution. Assuming that all the tiles have the same
    // resolution, warn the user if Maya's tiled texture implementation is going to result in
    // a loss of texture data.
    {
        HioImageSharedPtr image = HioImage::OpenForReading(std::get<1>(tiles[0]).GetString());
        if (!TF_VERIFY(image)) {
            return nullptr;
        }
        isColorSpaceSRGB = image->IsColorSpaceSRGB();
        unsigned int tileWidth = image->GetWidth();
        unsigned int tileHeight = image->GetHeight();

        int maxTileId = std::get<0>(tiles.back());
        int maxU = maxTileId % 10;
        int maxV = (maxTileId - maxU) / 10;
        if ((tileWidth * maxU > maxWidth) || (tileHeight * maxV > maxHeight))
            TF_WARN(
                "UDIM texture %s creates a tiled texture larger than the maximum texture size. Some"
                "resolution will be lost.",
                path.c_str());
    }

    MString textureName(
        path.c_str()); // used for caching, using the string with <UDIM> in it is fine
    MStringArray tilePaths;
    MFloatArray  tilePositions;
    for (auto& tile : tiles) {
        tilePaths.append(MString(std::get<1>(tile).GetText()));

        HioImageSharedPtr image = HioImage::OpenForReading(std::get<1>(tile).GetString());
        if (!TF_VERIFY(image)) {
            return nullptr;
        }
        if (isColorSpaceSRGB != image->IsColorSpaceSRGB()) {
            TF_WARN(
                "UDIM texture %s color space doesn't match %s color space",
                std::get<1>(tile).GetText(),
                std::get<1>(tiles[0]).GetText());
        }

        // The image labeled 1001 will have id 0, 1002 will have id 1, 1011 will have id 10.
        // image 1001 starts with UV (0.0f, 0.0f), 1002 is (1.0f, 0.0f) and 1011 is (0.0f, 1.0f)
        int   tileId = std::get<0>(tile);
        float u = (float)(tileId % 10);
        float v = (float)((tileId - u) / 10);
        tilePositions.append(u);
        tilePositions.append(v);
    }

    MColor       undefinedColor(0.0f, 1.0f, 0.0f, 1.0f);
    MStringArray failedTilePaths;
    texture = textureMgr->acquireTiledTexture(
        textureName,
        tilePaths,
        tilePositions,
        undefinedColor,
        maxWidth,
        maxHeight,
        failedTilePaths,
        uvScaleOffset);

    for (unsigned int i = 0; i < failedTilePaths.length(); i++) {
        TF_WARN("Failed to load <UDIM> texture tile %s", failedTilePaths[i].asChar());
    }

    return texture;
}

MHWRender::MTexture* _GenerateFallbackTexture(
    MHWRender::MTextureManager* const textureMgr,
    const std::string&                path,
    const GfVec4f&                    fallbackColor)
{
    MHWRender::MTexture* texture = textureMgr->findTexture(path.c_str());
    if (texture) {
        return texture;
    }

    MHWRender::MTextureDescription desc;
    desc.setToDefault2DTexture();
    desc.fWidth = 1;
    desc.fHeight = 1;
    desc.fFormat = MHWRender::kR8G8B8A8_UNORM;
    desc.fBytesPerRow = 4;
    desc.fBytesPerSlice = desc.fBytesPerRow;

    std::vector<unsigned char> texels(4);
    for (size_t i = 0; i < 4; ++i) {
        float texelValue = GfClamp(fallbackColor[i], 0.0f, 1.0f);
        texels[i] = static_cast<unsigned char>(texelValue * 255.0);
    }
    return textureMgr->acquireTexture(path.c_str(), desc, texels.data());
}

//! Load texture from the specified path
MHWRender::MTexture* _LoadTexture(
    const std::string& path,
    bool               hasFallbackColor,
    const GfVec4f&     fallbackColor,
    bool&              isColorSpaceSRGB,
    MFloatArray&       uvScaleOffset)
{
    MProfilingScope profilingScope(
        HdVP2RenderDelegate::sProfilerCategory, MProfiler::kColorD_L2, "LoadTexture", path.c_str());

    // If it is a UDIM texture we need to modify the path before calling OpenForReading
    if (HdStIsSupportedUdimTexture(path))
        return _LoadUdimTexture(path, isColorSpaceSRGB, uvScaleOffset);

    MHWRender::MRenderer* const       renderer = MHWRender::MRenderer::theRenderer();
    MHWRender::MTextureManager* const textureMgr
        = renderer ? renderer->getTextureManager() : nullptr;
    if (!TF_VERIFY(textureMgr)) {
        return nullptr;
    }

    MHWRender::MTexture* texture = textureMgr->findTexture(path.c_str());
    if (texture) {
        return texture;
    }

    HioImageSharedPtr image = HioImage::OpenForReading(path);
    if (!TF_VERIFY(image, "Unable to create an image from %s", path.c_str())) {
        if (!hasFallbackColor) {
            return nullptr;
        }
        // Create a 1x1 texture of the fallback color, if it was specified:
        return _GenerateFallbackTexture(textureMgr, path, fallbackColor);
    }

    // This image is used for loading pixel data from usdz only and should
    // not trigger any OpenGL call. VP2RenderDelegate will transfer the
    // texels to GPU memory with VP2 API which is 3D API agnostic.
    HioImage::StorageSpec spec;
    spec.width = image->GetWidth();
    spec.height = image->GetHeight();
    spec.depth = 1;
    spec.format = image->GetFormat();
    spec.flipped = false;

    const int bpp = image->GetBytesPerPixel();
    const int bytesPerRow = spec.width * bpp;
    const int bytesPerSlice = bytesPerRow * spec.height;

    std::vector<unsigned char> storage(bytesPerSlice);
    spec.data = storage.data();

    if (!image->Read(spec)) {
        return nullptr;
    }

    MHWRender::MTextureDescription desc;
    desc.setToDefault2DTexture();
    desc.fWidth = spec.width;
    desc.fHeight = spec.height;
    desc.fBytesPerRow = bytesPerRow;
    desc.fBytesPerSlice = bytesPerSlice;

    auto specFormat = spec.format;
    switch (specFormat) {
    // Single Channel
    case HioFormatFloat32: {
        // We want white instead or red when expanding to RGB, so convert to kR32G32B32_FLOAT
        constexpr int bpp_RGB32 = 3 * 4;

        desc.fFormat = MHWRender::kR32G32B32_FLOAT;
        desc.fBytesPerRow = spec.width * bpp_RGB32;
        desc.fBytesPerSlice = desc.fBytesPerRow * spec.height;

        std::vector<unsigned char> texels(desc.fBytesPerSlice);

        uint32_t* texels32 = (uint32_t*)texels.data();
        uint32_t* storage32 = (uint32_t*)storage.data();

        for (int p = 0; p < spec.height * spec.width; p++) {
            const uint32_t pixel = *storage32++;
            *texels32++ = pixel;
            *texels32++ = pixel;
            *texels32++ = pixel;
        }

        texture = textureMgr->acquireTexture(path.c_str(), desc, texels.data());
    } break;
    case HioFormatFloat16: {
        // We want white instead or red when expanding to RGB, so convert to kR16G16B16A16_FLOAT
        constexpr int bpp_8 = 8;

        desc.fFormat = MHWRender::kR16G16B16A16_FLOAT;
        desc.fBytesPerRow = spec.width * bpp_8;
        desc.fBytesPerSlice = desc.fBytesPerRow * spec.height;

        std::vector<unsigned char> texels(desc.fBytesPerSlice);

        GfHalf         opaqueAlpha(1.0f);
        const uint16_t alphaBits = opaqueAlpha.bits();

        uint16_t* texels16 = (uint16_t*)texels.data();
        uint16_t* storage16 = (uint16_t*)storage.data();

        for (int p = 0; p < spec.height * spec.width; p++) {
            const uint16_t pixel = *storage16++;
            *texels16++ = pixel;
            *texels16++ = pixel;
            *texels16++ = pixel;
            *texels16++ = alphaBits;
        }

        texture = textureMgr->acquireTexture(path.c_str(), desc, texels.data());
    } break;
    case HioFormatUNorm8: {
        // We want white instead or red when expanding to RGB, so convert to kR8G8B8A8_UNORM
        constexpr int bpp_4 = 4;

        desc.fFormat = MHWRender::kR8G8B8A8_UNORM;
        desc.fBytesPerRow = spec.width * bpp_4;
        desc.fBytesPerSlice = desc.fBytesPerRow * spec.height;

        std::vector<unsigned char> texels(desc.fBytesPerSlice);

        uint8_t* texels8 = (uint8_t*)texels.data();
        uint8_t* storage8 = (uint8_t*)storage.data();

        for (int p = 0; p < spec.height * spec.width; p++) {
            const uint8_t pixel = *storage8++;
            *texels8++ = pixel;
            *texels8++ = pixel;
            *texels8++ = pixel;
            *texels8++ = 0xFF;
        }

        texture = textureMgr->acquireTexture(path.c_str(), desc, texels.data());
        isColorSpaceSRGB = image->IsColorSpaceSRGB();
    } break;

    // Dual channel (quite rare, but seen with mono + alpha files)
    case HioFormatFloat32Vec2: {
        // R32G32 is supported by VP2. But we want black and white, so R32G32B32A32.
        constexpr int bpp_RGBA32 = 4 * 4;

        desc.fFormat = MHWRender::kR32G32B32A32_FLOAT;
        desc.fBytesPerRow = spec.width * bpp_RGBA32;
        desc.fBytesPerSlice = desc.fBytesPerRow * spec.height;

        std::vector<unsigned char> texels(desc.fBytesPerSlice);

        uint32_t* texels32 = (uint32_t*)texels.data();
        uint32_t* storage32 = (uint32_t*)storage.data();

        for (int p = 0; p < spec.height * spec.width; p++) {
            const uint32_t pixel = *storage32++;
            *texels32++ = pixel;
            *texels32++ = pixel;
            *texels32++ = pixel;
            *texels32++ = *storage32++;
        }

        texture = textureMgr->acquireTexture(path.c_str(), desc, texels.data());
    } break;
    case HioFormatFloat16Vec2: {
        // R16G16 is not supported by VP2. Converted to R16G16B16A16.
        constexpr int bpp_8 = 8;

        desc.fFormat = MHWRender::kR16G16B16A16_FLOAT;
        desc.fBytesPerRow = spec.width * bpp_8;
        desc.fBytesPerSlice = desc.fBytesPerRow * spec.height;

        std::vector<unsigned char> texels(desc.fBytesPerSlice);

        uint16_t* texels16 = (uint16_t*)texels.data();
        uint16_t* storage16 = (uint16_t*)storage.data();

        for (int p = 0; p < spec.height * spec.width; p++) {
            const uint16_t pixel = *storage16++;
            *texels16++ = pixel;
            *texels16++ = pixel;
            *texels16++ = pixel;
            *texels16++ = *storage16++;
        }

        texture = textureMgr->acquireTexture(path.c_str(), desc, texels.data());
        break;
    }
    case HioFormatUNorm8Vec2:
    case HioFormatUNorm8Vec2srgb: {
        // R8G8 is not supported by VP2. Converted to R8G8B8A8.
        constexpr int bpp_4 = 4;

        desc.fFormat = MHWRender::kR8G8B8A8_UNORM;
        desc.fBytesPerRow = spec.width * bpp_4;
        desc.fBytesPerSlice = desc.fBytesPerRow * spec.height;

        std::vector<unsigned char> texels(desc.fBytesPerSlice);

        uint8_t* texels8 = (uint8_t*)texels.data();
        uint8_t* storage8 = (uint8_t*)storage.data();

        for (int p = 0; p < spec.height * spec.width; p++) {
            const uint8_t pixel = *storage8++;
            *texels8++ = pixel;
            *texels8++ = pixel;
            *texels8++ = pixel;
            *texels8++ = *storage8++;
        }

        texture = textureMgr->acquireTexture(path.c_str(), desc, texels.data());
        isColorSpaceSRGB = image->IsColorSpaceSRGB();
        break;
    }

    // 3-Channel
    case HioFormatFloat32Vec3:
        desc.fFormat = MHWRender::kR32G32B32_FLOAT;
        texture = textureMgr->acquireTexture(path.c_str(), desc, spec.data);
        break;
    case HioFormatFloat16Vec3: {
        // R16G16B16 is not supported by VP2. Converted to R16G16B16A16.
        constexpr int bpp_8 = 8;

        desc.fFormat = MHWRender::kR16G16B16A16_FLOAT;
        desc.fBytesPerRow = spec.width * bpp_8;
        desc.fBytesPerSlice = desc.fBytesPerRow * spec.height;

        GfHalf               opaqueAlpha(1.0f);
        const unsigned short alphaBits = opaqueAlpha.bits();
        const unsigned char  lowAlpha = reinterpret_cast<const unsigned char*>(&alphaBits)[0];
        const unsigned char  highAlpha = reinterpret_cast<const unsigned char*>(&alphaBits)[1];

        std::vector<unsigned char> texels(desc.fBytesPerSlice);

        for (int y = 0; y < spec.height; y++) {
            for (int x = 0; x < spec.width; x++) {
                const int t = spec.width * y + x;
                texels[t * bpp_8 + 0] = storage[t * bpp + 0];
                texels[t * bpp_8 + 1] = storage[t * bpp + 1];
                texels[t * bpp_8 + 2] = storage[t * bpp + 2];
                texels[t * bpp_8 + 3] = storage[t * bpp + 3];
                texels[t * bpp_8 + 4] = storage[t * bpp + 4];
                texels[t * bpp_8 + 5] = storage[t * bpp + 5];
                texels[t * bpp_8 + 6] = lowAlpha;
                texels[t * bpp_8 + 7] = highAlpha;
            }
        }

        texture = textureMgr->acquireTexture(path.c_str(), desc, texels.data());
        break;
    }
    case HioFormatFloat16Vec4:
        desc.fFormat = MHWRender::kR16G16B16A16_FLOAT;
        texture = textureMgr->acquireTexture(path.c_str(), desc, spec.data);
        break;
    case HioFormatUNorm8Vec3:
    case HioFormatUNorm8Vec3srgb: {
        // R8G8B8 is not supported by VP2. Converted to R8G8B8A8.
        constexpr int bpp_4 = 4;

        desc.fFormat = MHWRender::kR8G8B8A8_UNORM;
        desc.fBytesPerRow = spec.width * bpp_4;
        desc.fBytesPerSlice = desc.fBytesPerRow * spec.height;

        std::vector<unsigned char> texels(desc.fBytesPerSlice);

        for (int y = 0; y < spec.height; y++) {
            for (int x = 0; x < spec.width; x++) {
                const int t = spec.width * y + x;
                texels[t * bpp_4] = storage[t * bpp];
                texels[t * bpp_4 + 1] = storage[t * bpp + 1];
                texels[t * bpp_4 + 2] = storage[t * bpp + 2];
                texels[t * bpp_4 + 3] = 255;
            }
        }

        texture = textureMgr->acquireTexture(path.c_str(), desc, texels.data());
        isColorSpaceSRGB = image->IsColorSpaceSRGB();
        break;
    }

    // 4-Channel
    case HioFormatFloat32Vec4:
        desc.fFormat = MHWRender::kR32G32B32A32_FLOAT;
        texture = textureMgr->acquireTexture(path.c_str(), desc, spec.data);
        break;
    case HioFormatUNorm8Vec4:
    case HioFormatUNorm8Vec4srgb:
        desc.fFormat = MHWRender::kR8G8B8A8_UNORM;
        isColorSpaceSRGB = image->IsColorSpaceSRGB();
        texture = textureMgr->acquireTexture(path.c_str(), desc, spec.data);
        break;
    default:
        TF_WARN(
            "VP2 renderer delegate: unsupported pixel format (%d) in texture file %s.",
            (int)specFormat,
            path.c_str());
        break;
    }

    return texture;
}

TfToken MayaDescriptorToToken(const MVertexBufferDescriptor& descriptor)
{
    // Attempt to match an MVertexBufferDescriptor to the corresponding
    // USD primvar token. The "Computed" token is used for data which
    // can be computed by an an rprim. Unknown is used for unsupported
    // descriptors.

    TfToken token = _tokens->Unknown;
    switch (descriptor.semantic()) {
    case MGeometry::kPosition: token = HdTokens->points; break;
    case MGeometry::kNormal: token = HdTokens->normals; break;
    case MGeometry::kTexture: break;
    case MGeometry::kColor: token = HdTokens->displayColor; break;
    case MGeometry::kTangent: token = _tokens->Computed; break;
    case MGeometry::kBitangent: token = _tokens->Computed; break;
    case MGeometry::kTangentWithSign: token = _tokens->Computed; break;
    default: break;
    }

    return token;
}

struct MStringHash
{
    std::size_t operator()(const MString& s) const
    {
        // To get rid of boost here, switch to C++17 compatible implementation:
        //     return std::hash(std::string_view(s.asChar(), s.length()))();
        return boost::hash_range(s.asChar(), s.asChar() + s.length());
    }
};

} // anonymous namespace

class HdVP2Material::TextureLoadingTask
{
public:
    TextureLoadingTask(
        HdVP2Material*     parent,
        HdSceneDelegate*   sceneDelegate,
        const std::string& path,
        bool               hasFallbackColor,
        const GfVec4f&     fallbackColor)
        : _parent(parent)
        , _sceneDelegate(sceneDelegate)
        , _path(path)
        , _fallbackColor(fallbackColor)
        , _hasFallbackColor(hasFallbackColor)
    {
    }

    ~TextureLoadingTask() = default;

    const HdVP2TextureInfo& GetFallbackTextureInfo()
    {
        if (!_fallbackTextureInfo._texture) {
            // Create a default texture info with fallback color
            MHWRender::MRenderer* const       renderer = MHWRender::MRenderer::theRenderer();
            MHWRender::MTextureManager* const textureMgr
                = renderer ? renderer->getTextureManager() : nullptr;
            if (textureMgr) {
                // Use a relevant but unique name if there is a fallback color
                // Otherwise reuse the same default texture
                _fallbackTextureInfo._texture.reset(_GenerateFallbackTexture(
                    textureMgr,
                    _hasFallbackColor ? _path + ".fallback" : "default_fallback",
                    _fallbackColor));
            }
        }
        return _fallbackTextureInfo;
    }

    bool EnqueueLoadOnIdle()
    {
        if (_started.exchange(true)) {
            return false;
        }
        // Push the texture loading on idle
        auto ret = MGlobal::executeTaskOnIdle(
            [](void* data) {
                auto* task = static_cast<HdVP2Material::TextureLoadingTask*>(data);
                task->_Load();
                // Once it is done, free the memory.
                delete task;
            },
            this);
        return ret == MStatus::kSuccess;
    }

    bool Terminate()
    {
        _terminated = true;
        // Return the started state to caller, the caller will delete this object
        // if this task has not started yet.
        // We will not be able to delete this object within its method.
        return !_started.load();
    }

private:
    void _Load()
    {
        if (_terminated) {
            return;
        }
        bool        isSRGB = false;
        MFloatArray uvScaleOffset;
        auto*       texture
            = _LoadTexture(_path, _hasFallbackColor, _fallbackColor, isSRGB, uvScaleOffset);
        if (_terminated) {
            return;
        }
        _parent->_UpdateLoadedTexture(_sceneDelegate, _path, texture, isSRGB, uvScaleOffset);
    }

    HdVP2TextureInfo  _fallbackTextureInfo;
    HdVP2Material*    _parent;
    HdSceneDelegate*  _sceneDelegate;
    const std::string _path;
    const GfVec4f     _fallbackColor;
    std::atomic_bool  _started { false };
    bool              _terminated { false };
    bool              _hasFallbackColor;
};

std::mutex                            HdVP2Material::_refreshMutex;
std::chrono::steady_clock::time_point HdVP2Material::_startTime;
std::atomic_size_t                    HdVP2Material::_runningTasksCounter;
HdVP2GlobalTextureMap                 HdVP2Material::_globalTextureMap;

/*! \brief  Releases the reference to the texture owned by a smart pointer.
 */
void HdVP2TextureDeleter::operator()(MHWRender::MTexture* texture)
{
    MRenderer* const                  renderer = MRenderer::theRenderer();
    MHWRender::MTextureManager* const textureMgr
        = renderer ? renderer->getTextureManager() : nullptr;
    if (TF_VERIFY(textureMgr)) {
        textureMgr->releaseTexture(texture);
    }
}

/*! \brief  Constructor
 */
HdVP2Material::HdVP2Material(HdVP2RenderDelegate* renderDelegate, const SdfPath& id)
    : HdMaterial(id)
    , _renderDelegate(renderDelegate)
    , _compiledNetworks { this, this }
{
}

HdVP2Material::~HdVP2Material()
{
    // Tell pending tasks or running tasks (if any) to terminate
    ClearPendingTasks();

    if (!_localTextureMap.empty()) {
        _TransientTexturePreserver::GetInstance().PreserveTextures(_localTextureMap);
    }
}

void ConvertNetworkMapToUntextured(HdMaterialNetworkMap& networkMap)
{
    for (auto& item : networkMap.map) {
        auto& network = item.second;
        auto  isInputNode = [&networkMap](const HdMaterialNode& node) {
            return std::find(networkMap.terminals.begin(), networkMap.terminals.end(), node.path)
                == networkMap.terminals.end();
        };

        auto eraseBegin = std::remove_if(network.nodes.begin(), network.nodes.end(), isInputNode);
        network.nodes.erase(eraseBegin, network.nodes.end());
        network.relationships.clear();
#ifdef WANT_MATERIALX_BUILD
        // Raw MaterialX surface constructor node does not render. Replace with default
        // standard_surface:
        for (auto& node : network.nodes) {
            if (node.identifier == _mtlxTokens->ND_surface) {
                node.identifier = _mtlxTokens->ND_standard_surface_surfaceshader;
                node.parameters.clear();
            }
        }
#endif
    }
}

/*! \brief  Synchronize VP2 state with scene delegate state based on dirty bits
 */
void HdVP2Material::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* /*renderParam*/,
    HdDirtyBits* dirtyBits)
{
    if (*dirtyBits & (HdMaterial::DirtyResource | HdMaterial::DirtyParams)) {
        const SdfPath& id = GetId();

        MProfilingScope profilingScope(
            HdVP2RenderDelegate::sProfilerCategory,
            MProfiler::kColorC_L2,
            "HdVP2Material::Sync",
            id.GetText());

        VtValue vtMatResource = sceneDelegate->GetMaterialResource(id);

        if (vtMatResource.IsHolding<HdMaterialNetworkMap>()) {
            const HdMaterialNetworkMap& fullNetworkMap
                = vtMatResource.UncheckedGet<HdMaterialNetworkMap>();

            // untextured network is always synced
            HdMaterialNetworkMap untexturedNetworkMap = fullNetworkMap;
            ConvertNetworkMapToUntextured(untexturedNetworkMap);
            _compiledNetworks[kUntextured].Sync(sceneDelegate, untexturedNetworkMap);

            // full network is synced only if required by display style
            auto* const param = static_cast<HdVP2RenderParam*>(_renderDelegate->GetRenderParam());
            if (param->GetDrawScene().NeedTexturedMaterials()) {
                _compiledNetworks[kFull].Sync(sceneDelegate, fullNetworkMap);
            }
        } else {
            TF_WARN(
                "Expected material resource for <%s> to hold HdMaterialNetworkMap,"
                "but found %s instead.",
                id.GetText(),
                vtMatResource.GetTypeName().c_str());
        }
    }

    *dirtyBits = HdMaterial::Clean;
}

void HdVP2Material::CompiledNetwork::Sync(
    HdSceneDelegate*            sceneDelegate,
    const HdMaterialNetworkMap& networkMap)
{
    auto updateShaderInstance = [this, &sceneDelegate](const HdMaterialNetwork& bxdfNet) {
        const bool wasTransparent = _transparent;
        _UpdateShaderInstance(sceneDelegate, bxdfNet);
        // If the transparency flag changed, then the drawItems must be updated.
        // e.g. if MRenderItem was first marked transparent and then the shader's transparency
        // is turned off, MRenderItem must now be marked opaque.
        bool drawItemsDirty = (wasTransparent != _transparent);

// Consolidation workaround requires dirtying the mesh even on a ValueChanged
#ifdef HDVP2_MATERIAL_CONSOLIDATION_UPDATE_WORKAROUND
        drawItemsDirty = true;
#endif
        if (drawItemsDirty) {
            _owner->MaterialChanged(sceneDelegate);
        }
    };

    const SdfPath&    id = _owner->GetId();
    HdMaterialNetwork bxdfNet, dispNet, vp2BxdfNet;

    TfMapLookup(networkMap.map, HdMaterialTerminalTokens->surface, &bxdfNet);
    TfMapLookup(networkMap.map, HdMaterialTerminalTokens->displacement, &dispNet);

#ifdef WANT_MATERIALX_BUILD
    if (!bxdfNet.nodes.empty()) {
        if (_IsMaterialX(bxdfNet.nodes.back())) {

            bool isVolume = false;
#if PXR_VERSION > 2203
            const HdMaterialNetwork2 surfaceNetwork
                = HdConvertToHdMaterialNetwork2(networkMap, &isVolume);
#else
            HdMaterialNetwork2 surfaceNetwork;
            HdMaterialNetwork2ConvertFromHdMaterialNetworkMap(
                networkMap, &surfaceNetwork, &isVolume);
#endif
            if (isVolume) {
                // Not supported.
                return;
            }

            size_t topoHash = _GenerateNetwork2TopoHash(surfaceNetwork);

            if (!_surfaceShader || topoHash != _topoHash) {
                _surfaceShader.reset(_CreateMaterialXShaderInstance(id, surfaceNetwork));
                _frontFaceShader.reset(nullptr);
                _pointShader.reset(nullptr);
                _topoHash = topoHash;
                // TopoChanged: We have a brand new surface material, tell the mesh to use
                // it.
                _owner->MaterialChanged(sceneDelegate);
            }

            if (_surfaceShader) {
                updateShaderInstance(bxdfNet);
            }
            return;
        }
    }
#endif

    _ApplyVP2Fixes(vp2BxdfNet, bxdfNet);

    if (!vp2BxdfNet.nodes.empty()) {
        // Generate a XML string from the material network and convert it to a token for
        // faster hashing and comparison.
        const TfToken token(_GenerateXMLString(vp2BxdfNet, false));

        // Skip creating a new shader instance if the token is unchanged. There is no plan
        // to implement fine-grain dirty bit in Hydra for the same purpose:
        // https://groups.google.com/g/usd-interest/c/xytT2azlJec/m/22Tnw4yXAAAJ
        if (_surfaceNetworkToken != token) {
            MProfilingScope subProfilingScope(
                HdVP2RenderDelegate::sProfilerCategory,
                MProfiler::kColorD_L2,
                "CreateShaderInstance");

            // Remember the path of the surface shader for special handling: unlike other
            // fragments, the parameters of the surface shader fragment can't be renamed.
            _surfaceShaderId = vp2BxdfNet.nodes.back().path;

            MHWRender::MShaderInstance* shader;

#ifndef HDVP2_DISABLE_SHADER_CACHE
            // Acquire a shader instance from the shader cache. If a shader instance has
            // been cached with the same token, a clone of the shader instance will be
            // returned. Multiple clones of a shader instance will share the same shader
            // effect, thus reduce compilation overhead and enable material consolidation.
            shader = _owner->_renderDelegate->GetShaderFromCache(token);

            // If the shader instance is not found in the cache, create one from the
            // material network and add a clone to the cache for reuse.
            if (!shader) {
                shader = _CreateShaderInstance(vp2BxdfNet);

                if (shader) {
                    _owner->_renderDelegate->AddShaderToCache(token, *shader);
                }
            }
#else
            shader = _CreateShaderInstance(vp2BxdfNet);
#endif

            // The shader instance is owned by the material solely.
            _surfaceShader.reset(shader);
            _frontFaceShader.reset(nullptr);
            _pointShader.reset(nullptr);
            // TopoChanged: We have a brand new surface material, tell the mesh to use it.
            _owner->MaterialChanged(sceneDelegate);

            if (TfDebug::IsEnabled(HDVP2_DEBUG_MATERIAL)) {
                std::cout << "BXDF material network for " << id << ":\n"
                          << _GenerateXMLString(bxdfNet) << "\n"
                          << "BXDF (with VP2 fixes) material network for " << id << ":\n"
                          << _GenerateXMLString(vp2BxdfNet) << "\n"
                          << "Displacement material network for " << id << ":\n"
                          << _GenerateXMLString(dispNet) << "\n";

                if (_surfaceShader) {
                    auto tmpDir = ghc::filesystem::temp_directory_path();
                    tmpDir /= "HdVP2Material_";
                    tmpDir += id.GetName();
                    tmpDir += ".txt";
                    _surfaceShader->writeEffectSourceToFile(tmpDir.c_str());

                    std::cout << "BXDF generated shader code for " << id << ":\n";
                    std::cout << "  " << tmpDir << "\n";
                }
            }

            // Store primvar requirements.
            _requiredPrimvars = std::move(vp2BxdfNet.primvars);

            // Verify that _requiredPrivars contains all the requiredVertexBuffers() the
            // shader instance needs.
            if (shader) {
                MVertexBufferDescriptorList requiredVertexBuffers;
                MStatus status = shader->requiredVertexBuffers(requiredVertexBuffers);
                if (status) {
                    for (int reqIndex = 0; reqIndex < requiredVertexBuffers.length(); reqIndex++) {
                        MVertexBufferDescriptor desc;
                        requiredVertexBuffers.getDescriptor(reqIndex, desc);
                        TfToken requiredPrimvar = MayaDescriptorToToken(desc);
                        // now make sure something matching requiredPrimvar is in
                        // _requiredPrimvars
                        if (requiredPrimvar != _tokens->Unknown
                            && requiredPrimvar != _tokens->Computed) {
                            bool found = false;
                            for (TfToken const& primvar : _requiredPrimvars) {
                                if (primvar == requiredPrimvar) {
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) {
                                _requiredPrimvars.push_back(requiredPrimvar);
                            }
                        }
                    }
                }
            }

            // The token is saved and will be used to determine whether a new shader
            // instance is needed during the next sync.
            _surfaceNetworkToken = token;
        }

        updateShaderInstance(bxdfNet);
    }
}

/*! \brief  Returns the minimal set of dirty bits to place in the
change tracker for use in the first sync of this prim.
*/
HdDirtyBits HdVP2Material::GetInitialDirtyBitsMask() const { return HdMaterial::AllDirty; }

/*! \brief  Applies VP2-specific fixes to the material network.
 */
void HdVP2Material::CompiledNetwork::_ApplyVP2Fixes(
    HdMaterialNetwork&       outNet,
    const HdMaterialNetwork& inNet)
{
    // To avoid relocation, reserve enough space for possible maximal size. The
    // output network is temporary C++ object that will be released after use.
    const size_t numNodes = inNet.nodes.size();
    const size_t numRelationships = inNet.relationships.size();

    size_t nodeCounter = 0;

    _nodePathMap.clear();
    _nodePathMap.reserve(numNodes);

    HdMaterialNetwork tmpNet;
    tmpNet.nodes.reserve(numNodes);
    tmpNet.relationships.reserve(numRelationships);

#if PXR_VERSION <= 2211
    // Some material networks require us to add nodes and connections to the base
    // HdMaterialNetwork. Keep track of the existence of some key nodes to help
    // with performance.
    HdMaterialNode* usdDrawModeCardsNode = nullptr;
    HdMaterialNode* cardsUvPrimvarReader = nullptr;
#endif

    // Get the shader registry so I can look up the real names of shading nodes.
    SdrRegistry& shaderReg = SdrRegistry::GetInstance();

    // Replace the authored node paths with simplified paths in the form of "node#". By doing so
    // we will be able to reuse shader effects among material networks which have the same node
    // identifiers and relationships but different node paths, reduce shader compilation overhead
    // and enable material consolidation for faster rendering.
    for (const HdMaterialNode& node : inNet.nodes) {
        tmpNet.nodes.push_back(node);

        HdMaterialNode& outNode = tmpNet.nodes.back();
#if PXR_VERSION <= 2211
        // For card draw mode the HdMaterialNode will have an identifier which is the hash of the
        // file path to drawMode.glslfx on disk. Using that value I can get the SdrShaderNode, and
        // then get the actual name of the shader "drawMode.glslfx". For other node names the
        // HdMaterialNode identifier and the SdrShaderNode name seem to be the same, so just convert
        // everything to use the SdrShaderNode name.
        SdrShaderNodeConstPtr sdrNode
            = shaderReg.GetShaderNodeByIdentifierAndType(outNode.identifier, _tokens->glslfx);
#else
        // Ensure that our node identifiers are correct. The HdMaterialNode identifier
        // and the SdrShaderNode name seem to be the same in most cases, but we
        // convert everything to use the SdrShaderNode name to be sure.
        SdrShaderNodeConstPtr sdrNode = shaderReg.GetShaderNodeByIdentifier(outNode.identifier);
#endif
        if (_IsUsdUVTexture(node)) {
            outNode.identifier
                = TfToken(HdVP2ShaderFragments::getUsdUVTextureFragmentName(
                              MayaUsd::ColorManagementPreferences::RenderingSpaceName())
                              .asChar());
        } else {
            if (!sdrNode) {
                TF_WARN("Could not find a shader node for <%s>", node.path.GetText());
                return;
            }
            outNode.identifier = TfToken(sdrNode->GetName());
        }

#if PXR_VERSION <= 2211
        if (_IsUsdDrawModeNode(outNode)) {
            // I can't easily name a Maya fragment something with a '.' in it, so pick a different
            // fragment name.
            outNode.identifier = _tokens->UsdDrawModeCards;
            TF_VERIFY(!usdDrawModeCardsNode); // there should only be one.
            usdDrawModeCardsNode = &outNode;
        }

        if (_IsUsdFloat2PrimvarReader(outNode)
            && outNode.parameters[_tokens->varname] == _tokens->cardsUv) {
            TF_VERIFY(!cardsUvPrimvarReader);
            cardsUvPrimvarReader = &outNode;
        }
#endif

        outNode.path = SdfPath(outNode.identifier.GetString() + std::to_string(++nodeCounter));

        _nodePathMap[node.path] = outNode.path;
    }

    // Update the relationships to use the new node paths.
    for (const HdMaterialRelationship& rel : inNet.relationships) {
        tmpNet.relationships.push_back(rel);

        HdMaterialRelationship& outRel = tmpNet.relationships.back();
        outRel.inputId = _nodePathMap[outRel.inputId];
        outRel.outputId = _nodePathMap[outRel.outputId];
    }

#ifdef HAS_COLOR_MANAGEMENT_SUPPORT_API
    _AddColorManagementFragments(tmpNet);
#endif

    outNet.nodes.reserve(numNodes + numRelationships);
    outNet.relationships.reserve(numRelationships * 2);
    outNet.primvars.reserve(numNodes);

#if PXR_VERSION <= 2211
    // Add additional nodes necessary for Maya's fragment compiler
    // to work that are logical predecessors of node.
    auto addPredecessorNodes = [&](const HdMaterialNode& node) {
        // If the node is a UsdUVTexture node, verify there is a UsdPrimvarReader_float2 connected
        // to the st input of it. If not, find the basic st reader and/or create it and connect it.
        // Adding the UV reader only works for cards draw mode. We wouldn't know which UV stream to
        // read if another material was missing the primvar reader.
        if (_IsUsdUVTexture(node) && usdDrawModeCardsNode) {
            // the DrawModeCardsFragment has UsdUVtexture nodes without primvar readers.
            // Add a primvar reader to each UsdUVTexture which doesn't already have one.
            if (!cardsUvPrimvarReader) {
                HdMaterialNode stReader;
                stReader.identifier = UsdImagingTokens->UsdPrimvarReader_float2;
                stReader.path
                    = SdfPath(stReader.identifier.GetString() + std::to_string(++nodeCounter));
                stReader.parameters[_tokens->varname] = _tokens->cardsUv;
                outNet.nodes.push_back(stReader);
                cardsUvPrimvarReader = &outNet.nodes.back();
                // Specifically looking for the cardsUv primvar
                outNet.primvars.push_back(_tokens->cardsUv);
            }

            // search for an existing relationship between cardsUvPrimvarReader & node.
            // TODO: if there are multiple UV sets this can fail, it is looking for
            // a connection to a specific UsdPrimvarReader_float2.
            bool hasRelationship = false;
            for (const HdMaterialRelationship& rel : tmpNet.relationships) {
                if (rel.inputId == cardsUvPrimvarReader->path && rel.inputName == _tokens->result
                    && rel.outputId == node.path && rel.outputName == _tokens->st) {
                    hasRelationship = true;
                    break;
                }
            }

            if (!hasRelationship) {
                // The only case I'm currently aware of where we have UsdUVTexture nodes without
                // a corresponding UsdPrimvarReader_float2 to read the UVs is draw mode cards.
                // There could be other cases, and it could be find to add the primvar reader
                // and connection, but we want to know when it is happening.
                TF_VERIFY(usdDrawModeCardsNode);

                HdMaterialRelationship newRel
                    = { cardsUvPrimvarReader->path, _tokens->result, node.path, _tokens->st };
                outNet.relationships.push_back(newRel);
            }
        }

        // If the node is a DrawModeCardsFragment add a MayaIsBackFacing fragment to cull out
        // backfaces.
        if (_IsUsdDrawModeNode(node)) {
            // Add the MayaIsBackFacing fragment
            HdMaterialNode mayaIsBackFacingNode;
            mayaIsBackFacingNode.identifier = _tokens->mayaIsBackFacing;
            mayaIsBackFacingNode.path = SdfPath(
                mayaIsBackFacingNode.identifier.GetString() + std::to_string(++nodeCounter));
            outNet.nodes.push_back(mayaIsBackFacingNode);

            // Connect to the isBackfacing input of the DrawModeCards fragment
            HdMaterialRelationship newRel = { mayaIsBackFacingNode.path,
                                              _tokens->mayaIsBackFacing,
                                              node.path,
                                              _tokens->isBackfacing };
            outNet.relationships.push_back(newRel);
        }
    };
#endif

    // Add additional nodes necessary for Maya's fragment compiler
    // to work that are logical successors of node.
    auto addSuccessorNodes = [&](const HdMaterialNode& node, const TfToken& primvarToRead) {
#if PXR_VERSION <= 2211
        // If the node is a DrawModeCardsFragment add the fallback material after it to do
        // the lighting etc.
        if (_IsUsdDrawModeNode(node)) {

            // Add the fallback shader node and hook it up. This has to be the last node in
            // outNet.nodes.
            HdMaterialNode fallbackShaderNode;
            fallbackShaderNode.identifier = _tokens->FallbackShader;
            fallbackShaderNode.path = SdfPath(
                fallbackShaderNode.identifier.GetString() + std::to_string(++nodeCounter));
            outNet.nodes.push_back(fallbackShaderNode);

            // The DrawModeCards fragment is basically a texture picker. Connect its output to
            // the diffuseColor input of the fallback shader node.
            HdMaterialRelationship newRel
                = { node.path, _tokens->output, fallbackShaderNode.path, _tokens->diffuseColor };
            outNet.relationships.push_back(newRel);

            // Add the required primvars
            outNet.primvars.push_back(HdTokens->points);
            outNet.primvars.push_back(HdTokens->normals);

            // no passthrough nodes necessary between the draw mode cards node & the fallback
            // shader.
            return;
        }
#endif

        // Copy outgoing connections and if needed add passthrough node/connection.
        for (const HdMaterialRelationship& rel : tmpNet.relationships) {
            if (rel.inputId != node.path) {
                continue;
            }

            TfToken passThroughId;
            if (rel.inputName == _tokens->rgb || rel.inputName == _tokens->xyz) {
                passThroughId = _tokens->Float4ToFloat3;
            } else if (rel.inputName == _tokens->r || rel.inputName == _tokens->x) {
                passThroughId = _tokens->Float4ToFloatX;
            } else if (rel.inputName == _tokens->g || rel.inputName == _tokens->y) {
                passThroughId = _tokens->Float4ToFloatY;
            } else if (rel.inputName == _tokens->b || rel.inputName == _tokens->z) {
                passThroughId = _tokens->Float4ToFloatZ;
            } else if (rel.inputName == _tokens->a || rel.inputName == _tokens->w) {
                passThroughId = _tokens->Float4ToFloatW;
            } else if (primvarToRead == HdTokens->displayColor) {
                passThroughId = _tokens->Float4ToFloat3;
            } else if (primvarToRead == HdTokens->displayOpacity) {
                passThroughId = _tokens->Float4ToFloatW;
            } else {
                outNet.relationships.push_back(rel);
                continue;
            }

            const SdfPath passThroughPath(
                passThroughId.GetString() + std::to_string(++nodeCounter));

            const HdMaterialNode passThroughNode = { passThroughPath, passThroughId, {} };
            outNet.nodes.push_back(passThroughNode);

            HdMaterialRelationship newRel
                = { rel.inputId, _tokens->output, passThroughPath, _tokens->input };
            outNet.relationships.push_back(newRel);

            newRel = { passThroughPath, _tokens->output, rel.outputId, rel.outputName };
            outNet.relationships.push_back(newRel);
        }
    };

    // Add nodes necessary for the fragment compiler to produce a shader that works.
    for (const HdMaterialNode& node : tmpNet.nodes) {
        TfToken primvarToRead;

        const bool isUsdPrimvarReader = _IsUsdPrimvarReader(node);
        if (isUsdPrimvarReader) {
            auto it = node.parameters.find(_tokens->varname);
            if (it != node.parameters.end()) {
                primvarToRead = TfToken(TfStringify(it->second));
            }
        }

#if PXR_VERSION <= 2211
        addPredecessorNodes(node);
#endif
        outNet.nodes.push_back(node);

        // If the primvar reader is reading color or opacity, replace it with
        // UsdPrimvarReader_color which can create COLOR stream requirement
        // instead of generic TEXCOORD stream.
        // Do this before addSuccessorNodes, because changing the identifier may change the
        // input/output types and require another conversion node.
        if (primvarToRead == HdTokens->displayColor || primvarToRead == HdTokens->displayOpacity) {
            HdMaterialNode& nodeToChange = outNet.nodes.back();
            nodeToChange.identifier = _tokens->UsdPrimvarReader_color;
        }
        addSuccessorNodes(node, primvarToRead);

        // Normal map is not supported yet. For now primvars:normals is used for
        // shading, which is also the current behavior of USD/Hydra.
        // https://groups.google.com/d/msg/usd-interest/7epU16C3eyY/X9mLW9VFEwAJ

        // UsdImagingMaterialAdapter doesn't create primvar requirements as
        // expected. Workaround by manually looking up "varname" parameter.
        // https://groups.google.com/forum/#!msg/usd-interest/z-14AgJKOcU/1uJJ1thXBgAJ
        if (isUsdPrimvarReader) {
            if (!primvarToRead.IsEmpty()) {
                outNet.primvars.push_back(primvarToRead);
            }
        }
    }
}

#ifdef WANT_MATERIALX_BUILD

// We will be extremely arbitrary on all MaterialX image and tiledimage nodes and assume that color3
// and color4 require color management based on the file extension.
//
// For image nodes connected to a fileTexture post-processor, we will also check for the colorSpace
// attribute and respect requests.
TfToken _RequiresColorManagement(
    const HdMaterialNode2& node,
    const HdMaterialNode2& upstream,
#ifdef HAS_COLOR_MANAGEMENT_SUPPORT_API
    const HdMaterialNetwork2& inNet,
    TfToken&                  cmInputName,
    TfToken&                  cmOutputName)
#else
    const HdMaterialNetwork2& inNet)
#endif
{
    const mx::NodeDefPtr upstreamDef
        = _GetMaterialXData()._mtlxLibrary->getNodeDef(upstream.nodeTypeId);
    if (!upstreamDef) {
        return {};
    }

    if (!_MxHasFilenameInput(upstreamDef)) {
        // upstream is not a texture
        return {};
    }

    std::vector<TfToken> fileInputs;
    for (const auto& input : upstreamDef->getActiveInputs()) {
        if (input->getType() == _mtlxTokens->filename.GetString()) {
            fileInputs.push_back(TfToken(input->getName()));
        }
    }

    mx::OutputPtr colorOutput = upstreamDef->getActiveOutput(_mtlxTokens->out.GetString());
    if (!colorOutput) {
        return {};
    }

    // Only managing color3 and color4 outputs:
    if (colorOutput->getType() != _mtlxTokens->color3.GetString()
        && colorOutput->getType() != _mtlxTokens->color4.GetString()) {
        return {};
    }

    // Look for explicit color spaces first:
    static const auto _mxFindColorSpace = [](const auto& n, auto& sourceColorSpace) {
        if (!sourceColorSpace.empty()) {
            return;
        }
#if PXR_VERSION < 2311
        for (auto&& csAttrName : _mtlxKnownColorSpaceAttrs) {
            auto paramIt = n.parameters.find(csAttrName);
            if (paramIt != n.parameters.end()) {
                const VtValue& val = paramIt->second;
                if (val.IsHolding<TfToken>()) {
                    sourceColorSpace = val.UncheckedGet<TfToken>().GetString();
                    return;
                } else if (val.IsHolding<std::string>()) {
                    sourceColorSpace = val.UncheckedGet<std::string>();
                    return;
                }
            }
        }
#else
        for (auto&& param : n.parameters) {
            if (_IsHydraColorSpace(param.first)) {
                const VtValue& val = param.second;
                if (val.IsHolding<TfToken>()) {
                    sourceColorSpace = val.UncheckedGet<TfToken>().GetString();
                    return;
                } else if (val.IsHolding<std::string>()) {
                    sourceColorSpace = val.UncheckedGet<std::string>();
                    return;
                }
            }
        }
#endif
    };

    std::string sourceColorSpace;
    // Can be on the upstream node (UsdUVTexture):
    _mxFindColorSpace(upstream, sourceColorSpace);
    // Can sometimes be on node (MayaND_fileTexture):
    _mxFindColorSpace(node, sourceColorSpace);
    // To be updated as soon as color space metadata gets transmitted through Hydra.

    if (sourceColorSpace.empty() || sourceColorSpace == _tokens->auto_) {
        SdfAssetPath filenameVal;
        for (const auto& inputName : fileInputs) {
            auto itFileParam = upstream.parameters.find(inputName);
            if (itFileParam != upstream.parameters.end()
                && itFileParam->second.IsHolding<SdfAssetPath>()) {
                filenameVal = itFileParam->second.Get<SdfAssetPath>();
                break;
            }
        }

        auto const& resolvedPath = filenameVal.GetResolvedPath();
        if (resolvedPath.empty()) {
            return {};
        }
        sourceColorSpace = MayaUsd::ColorManagementPreferences::getFileRule(resolvedPath);
    }

    if (sourceColorSpace == "Raw" || sourceColorSpace == "raw") {
        return {};
    }

#ifdef HAS_COLOR_MANAGEMENT_SUPPORT_API
    MHWRender::MRenderer* theRenderer = MHWRender::MRenderer::theRenderer();
    if (!theRenderer) {
        return {};
    }

    MHWRender::MFragmentManager* fragmentManager = theRenderer->getFragmentManager();
    if (!fragmentManager) {
        return {};
    }

    MString fragName, fragInput, fragOutput;
    if (!MayaUsd::ColorManagementPreferences::isUnknownColorSpace(sourceColorSpace)) {
        if (fragmentManager->getColorManagementFragmentInfo(
                sourceColorSpace.c_str(), fragName, fragInput, fragOutput)) {
            std::string untypedNodeDefId
                = MaterialXMaya::OgsFragment::registerOCIOFragment(fragName.asChar());
            if (!untypedNodeDefId.empty()) {
                cmInputName = TfToken(fragInput.asChar());
                cmOutputName = TfToken(fragOutput.asChar());
                return TfToken((untypedNodeDefId + colorOutput->getType()));
            }
        } else {
            // Maya does not know about this color space. Remember that.
            MayaUsd::ColorManagementPreferences::addUnknownColorSpace(sourceColorSpace);
        }
    }
#else
    if (sourceColorSpace == _tokens->sRGB.GetString()) {
        // Non-OCIO code only knows how to handle sRGB source color space.
        // If we ended up here, then a color management node was required:
        if (colorOutput->getType().back() == '3') {
            return _mtlxTokens->color3;
        } else {
            return _mtlxTokens->color4;
        }
    }
#endif
    return {};
}

void HdVP2Material::CompiledNetwork::_ApplyMtlxVP2Fixes(
    HdMaterialNetwork2&       outNet,
    const HdMaterialNetwork2& inNet)
{

    // The goal here is to strip all local names in the network paths in order to reduce the shader
    // to its topological elements only.

    // We also strip all local values so that the Maya effect gets created with all values set to
    // their MaterialX default values.

    // Once we have that, we can fully re-use any previously encountered effect that has the same
    // MaterialX topology and only update the values that are found in the material network.

    size_t nodeCounter = 0;
    _nodePathMap.clear();

    // Paths will go /NG_Maya/N0, /NG_Maya/N1, /NG_Maya/N2...
    // We need NG_Maya, one level up, as this will be the name assigned to the MaterialX node graph
    // when run thru HdMtlxCreateMtlxDocumentFromHdNetwork (I know, forbidden knowledge again).
    SdfPath ngBase(_mtlxTokens->NG_Maya);

    // We might have to add color management nodes:
    std::string colorManagementCategory;

    // We will traverse the network in a depth-first traversal starting at the
    // terminals. This will allow a stable traversal that will not be affected
    // by the ordering of the SdfPaths and make sure we assign the same index to
    // all nodes regardless of the way they are sorted in the network node map.
    std::vector<const SdfPath*> pathsToTraverse;
    for (const auto& terminal : inNet.terminals) {
        const auto& connection = terminal.second;
        pathsToTraverse.push_back(&(connection.upstreamNode));
    }
    while (!pathsToTraverse.empty()) {
        const SdfPath* path = pathsToTraverse.back();
        pathsToTraverse.pop_back();
        if (!_nodePathMap.count(*path)) {
            const HdMaterialNode2& node = inNet.nodes.find(*path)->second;
            // We only need to create the anonymized name at this time:
            _nodePathMap[*path] = ngBase.AppendChild(TfToken("N" + std::to_string(nodeCounter++)));
            for (const auto& input : node.inputConnections) {
                for (const auto& connection : input.second) {
                    pathsToTraverse.push_back(&(connection.upstreamNode));
                }
            }
        }
    }

    // Copy the incoming network using only the anonymized names:
    outNet.primvars = inNet.primvars;
    for (const auto& terminal : inNet.terminals) {
        outNet.terminals.emplace(
            terminal.first,
            HdMaterialConnection2 { _nodePathMap[terminal.second.upstreamNode],
                                    terminal.second.upstreamOutputName });
    }
    for (const auto& nodePair : inNet.nodes) {
        const HdMaterialNode2& inNode = nodePair.second;
        HdMaterialNode2        outNode;
#if MX_COMBINED_VERSION >= 13808
        TfToken optimizedNodeId = _GetMaterialXData()._lobePruner->getOptimizedNodeId(inNode);
        if (optimizedNodeId.IsEmpty()) {
            outNode.nodeTypeId = inNode.nodeTypeId;
        } else {
            outNode.nodeTypeId = optimizedNodeId;
        }
#else
        outNode.nodeTypeId = inNode.nodeTypeId;
#endif
        if (_IsTopologicalNode(inNode)) {
            // These parameters affect topology:
            outNode.parameters = inNode.parameters;
        }

        for (const auto& cnxPair : inNode.inputConnections) {
            std::vector<HdMaterialConnection2> outCnx;
            for (const auto& c : cnxPair.second) {
                TfToken cmNodeDefId, cmInputName, cmOutputName;
#ifdef HAS_COLOR_MANAGEMENT_SUPPORT_API
                if (MayaUsd::ColorManagementPreferences::Active()) {
                    cmNodeDefId = _RequiresColorManagement(
                        inNode,
                        inNet.nodes.find(c.upstreamNode)->second,
                        inNet,
                        cmInputName,
                        cmOutputName);
                }
#else
                TfToken colorManagementType = _RequiresColorManagement(
                    inNode, inNet.nodes.find(c.upstreamNode)->second, inNet);
                if (!colorManagementType.IsEmpty()) {
                    if (colorManagementCategory.empty()) {
                        auto categoryIt = _mtlxColorCorrectCategoryMap.find(
                            MayaUsd::ColorManagementPreferences::RenderingSpaceName().asChar());
                        if (categoryIt != _mtlxColorCorrectCategoryMap.end()) {
                            colorManagementCategory = categoryIt->second;
                        }
                    }
                    cmNodeDefId
                        = TfToken(colorManagementCategory + colorManagementType.GetString());
                    cmInputName = _mtlxTokens->in;
                    cmOutputName = _mtlxTokens->out;
                }
#endif
                if (cmNodeDefId.IsEmpty()) {
                    outCnx.emplace_back(HdMaterialConnection2 { _nodePathMap[c.upstreamNode],
                                                                c.upstreamOutputName });
                } else {
                    // Insert color management node:
                    HdMaterialNode2 ccNode;
                    ccNode.nodeTypeId = cmNodeDefId;
                    HdMaterialConnection2 ccCnx { _nodePathMap[c.upstreamNode],
                                                  c.upstreamOutputName };
                    ccNode.inputConnections.insert({ cmInputName, { ccCnx } });
                    SdfPath ccPath
                        = ngBase.AppendChild(TfToken("N" + std::to_string(nodeCounter++)));
                    outCnx.emplace_back(HdMaterialConnection2 { ccPath, cmOutputName });
                    outNet.nodes.emplace(ccPath, std::move(ccNode));
                }
            }
            outNode.inputConnections.emplace(cnxPair.first, std::move(outCnx));
        }
        outNet.nodes.emplace(_nodePathMap[nodePair.first], std::move(outNode));
    }
}

/*! \brief  Detects MaterialX networks and rehydrates them.
 */
MHWRender::MShaderInstance* HdVP2Material::CompiledNetwork::_CreateMaterialXShaderInstance(
    SdfPath const&            materialId,
    HdMaterialNetwork2 const& surfaceNetwork)
{
    auto                        renderDelegate = _owner->_renderDelegate;
    MHWRender::MShaderInstance* shaderInstance = nullptr;

    auto const& terminalConnIt = surfaceNetwork.terminals.find(HdMaterialTerminalTokens->surface);
    if (terminalConnIt == surfaceNetwork.terminals.end()) {
        // No surface material
        return shaderInstance;
    }

    HdMaterialNetwork2 fixedNetwork;
    _ApplyMtlxVP2Fixes(fixedNetwork, surfaceNetwork);

    SdfPath       terminalPath = terminalConnIt->second.upstreamNode;
    const TfToken shaderCacheID(
        _GenerateXMLString(fixedNetwork) + MaterialXMaya::OgsFragment::getSpecularEnvKey());

    // Acquire a shader instance from the shader cache. If a shader instance has been cached with
    // the same token, a clone of the shader instance will be returned. Multiple clones of a shader
    // instance will share the same shader effect, thus reduce compilation overhead and enable
    // material consolidation.
    shaderInstance = renderDelegate->GetShaderFromCache(shaderCacheID);
    if (shaderInstance) {
        _surfaceShaderId = terminalPath;
        const TfTokenVector* cachedPrimvars = renderDelegate->GetPrimvarsFromCache(shaderCacheID);
        if (cachedPrimvars) {
            _requiredPrimvars = *cachedPrimvars;
        }
        const HdVP2ShaderCache::StringMap* cachedRenamedParameters
            = renderDelegate->GetRenamedParametersFromCache(shaderCacheID);
        if (cachedRenamedParameters) {
            _renamedParameters = *cachedRenamedParameters;
        }
        return shaderInstance;
    }

    SdfPath fixedPath = fixedNetwork.terminals[HdMaterialTerminalTokens->surface].upstreamNode;
    HdMaterialNode2 const* surfTerminal = &fixedNetwork.nodes[fixedPath];
    if (!surfTerminal) {
        return shaderInstance;
    }

    try {
        // The HdMtlxCreateMtlxDocumentFromHdNetwork function can throw if any MaterialX error is
        // raised.

        // Check if the Terminal is a MaterialX Node
        SdrRegistry&                sdrRegistry = SdrRegistry::GetInstance();
        const SdrShaderNodeConstPtr mtlxSdrNode = sdrRegistry.GetShaderNodeByIdentifierAndType(
            surfTerminal->nodeTypeId, HdVP2Tokens->mtlx);

        mx::DocumentPtr           mtlxDoc;
        const mx::FileSearchPath& crLibrarySearchPath(_GetMaterialXData()._mtlxSearchPath);
#if MX_COMBINED_VERSION >= 13808
        if (mtlxSdrNode
            || _GetMaterialXData()._lobePruner->isOptimizedNodeId(surfTerminal->nodeTypeId)) {
#else
        if (mtlxSdrNode) {
#endif

#ifdef HAS_COLOR_MANAGEMENT_SUPPORT_API
            mx::DocumentPtr completeLibrary = mx::createDocument();
            completeLibrary->importLibrary(_GetMaterialXData()._mtlxLibrary);
            completeLibrary->importLibrary(MaterialXMaya::OgsFragment::getOCIOLibrary());
#else
            mx::DocumentPtr completeLibrary = _GetMaterialXData()._mtlxLibrary;
#endif

            // Create the MaterialX Document from the HdMaterialNetwork
#if PXR_VERSION > 2111
            mtlxDoc = HdMtlxCreateMtlxDocumentFromHdNetwork(
                fixedNetwork,
                *surfTerminal, // MaterialX HdNode
                fixedPath,
                SdfPath(_mtlxTokens->USD_Mtlx_VP2_Material),
                completeLibrary);
#else
            std::set<SdfPath> hdTextureNodes;
            mx::StringMap mxHdTextureMap; // Mx-Hd texture name counterparts
            mtlxDoc = HdMtlxCreateMtlxDocumentFromHdNetwork(
                fixedNetwork,
                *surfTerminal, // MaterialX HdNode
                SdfPath(_mtlxTokens->USD_Mtlx_VP2_Material),
                completeLibrary,
                &hdTextureNodes,
                &mxHdTextureMap);
#endif

            if (!mtlxDoc) {
                return shaderInstance;
            }

            // Touchups required to fix input stream issues:
            _AddMissingTangents(mtlxDoc);
#if MX_COMBINED_VERSION >= 13900
            _AddMissingBitangents(mtlxDoc);
#endif

            _surfaceShaderId = terminalPath;

            if (TfDebug::IsEnabled(HDVP2_DEBUG_MATERIAL)) {
                std::cout << "generated shader code for " << materialId.GetText() << ":\n";
                std::cout << "Generated graph\n==============================\n";
                mx::writeToXmlStream(mtlxDoc, std::cout);
                std::cout << "\n==============================\n";
            }
        } else {
            return shaderInstance;
        }

        mx::NodePtr materialNode;
        for (const mx::NodePtr& material : mtlxDoc->getMaterialNodes()) {
            if (material->getName() == _mtlxTokens->USD_Mtlx_VP2_Material.GetText()) {
                materialNode = material;
            }
        }

        if (!materialNode) {
            return shaderInstance;
        }

        // Enable changing texcoord to geompropvalue
        const auto prevUVSetName = mx::OgsXmlGenerator::getPrimaryUVSetName();
        mx::OgsXmlGenerator::setPrimaryUVSetName(_GetMaterialXData()._mainUvSetName);

        MaterialXMaya::OgsFragment ogsFragment(materialNode, crLibrarySearchPath);

        // Restore previous UV set name
        mx::OgsXmlGenerator::setPrimaryUVSetName(prevUVSetName);

        // Explore the fragment for primvars:
        mx::ShaderPtr            shader = ogsFragment.getShader();
        const mx::VariableBlock& vertexInputs
            = shader->getStage(mx::Stage::VERTEX).getInputBlock(mx::HW::VERTEX_INPUTS);
        for (size_t i = 0; i < vertexInputs.size(); ++i) {
            const mx::ShaderPort* variable = vertexInputs[i];
            // Position is always assumed.
            // Tangent will be generated in the vertex shader using a utility fragment
            if (variable->getName() == mx::HW::T_IN_NORMAL) {
                _requiredPrimvars.push_back(HdTokens->normals);
            }
        }

        MHWRender::MRenderer* const renderer = MHWRender::MRenderer::theRenderer();
        if (!TF_VERIFY(renderer)) {
            return shaderInstance;
        }

        MHWRender::MFragmentManager* const fragmentManager = renderer->getFragmentManager();
        if (!TF_VERIFY(fragmentManager)) {
            return shaderInstance;
        }

        MString fragmentName(ogsFragment.getFragmentName().c_str());

        if (!fragmentManager->hasFragment(fragmentName)) {
            std::string   fragSrc = ogsFragment.getFragmentSource();
            const MString registeredFragment
                = fragmentManager->addShadeFragmentFromBuffer(fragSrc.c_str(), false);
            if (registeredFragment.length() == 0) {
                TF_WARN("Failed to register shader fragment %s", fragmentName.asChar());
                return shaderInstance;
            }
        }

        const MHWRender::MShaderManager* const shaderMgr = renderer->getShaderManager();
        if (!TF_VERIFY(shaderMgr)) {
            return shaderInstance;
        }

        shaderInstance = shaderMgr->getFragmentShader(fragmentName, "outColor", true);
        shaderInstance->addInputFragment("NwFaceCameraIfNAN", "output", "Nw");

        // Find named primvar readers:
        MStringArray parameterList;
        shaderInstance->parameterList(parameterList);
        for (unsigned int i = 0; i < parameterList.length(); ++i) {
            static const unsigned int u_geomprop_length
                = static_cast<unsigned int>(_mtlxTokens->i_geomprop_.GetString().length());
            if (parameterList[i].substring(0, u_geomprop_length - 1)
                == _mtlxTokens->i_geomprop_.GetText()) {
                MString varname
                    = parameterList[i].substring(u_geomprop_length, parameterList[i].length());
                shaderInstance->renameParameter(parameterList[i], varname);
                _requiredPrimvars.push_back(TfToken(varname.asChar()));
            }
        }

        // Remember inputs that were renamed because they conflicted with reserved keywords:
        for (const auto& namePair : ogsFragment.getPathInputMap()) {
            std::string path = namePair.first;
            std::string input = namePair.second;
            // Renaming adds digits at the end, so only compare the backs.
            if (path.back() != input.back()) {
                // If a digit was added, we should be able to find the last path element inside the
                // input name:
                size_t      lastSlash = path.rfind("/");
                std::string originalName = path;
                if (lastSlash != std::string::npos) {
                    originalName = path.substr(lastSlash + 1);
                }
                size_t foundOriginal = input.find(originalName);
                if (foundOriginal != std::string::npos) {
                    MString uniqueName(input.c_str());
                    input = input.substr(0, foundOriginal + originalName.size());
                    _renamedParameters.emplace(input, uniqueName);
                }
            }
        }
    } catch (mx::Exception& e) {
        TF_RUNTIME_ERROR(
            "Caught exception '%s' while processing '%s'", e.what(), materialId.GetText());
        return nullptr;
    }

    if (TfDebug::IsEnabled(HDVP2_DEBUG_MATERIAL)) {
        std::cout << "BXDF material network for " << materialId << ":\n"
                  << _GenerateXMLString(surfaceNetwork) << "\n"
                  << "Topology-only network for " << materialId << ":\n"
                  << shaderCacheID << "\n"
                  << "Required primvars:\n";

        for (TfToken const& primvar : _requiredPrimvars) {
            std::cout << "\t" << primvar << std::endl;
        }

        auto tmpDir = ghc::filesystem::temp_directory_path();
        tmpDir /= "HdVP2Material_";
        tmpDir += materialId.GetName();
        tmpDir += ".txt";
        shaderInstance->writeEffectSourceToFile(tmpDir.c_str());

        std::cout << "BXDF generated shader code for " << materialId << ":\n";
        std::cout << "  " << tmpDir << "\n";
    }

    if (shaderInstance) {
        renderDelegate->AddShaderToCache(shaderCacheID, *shaderInstance);
        renderDelegate->AddPrimvarsToCache(shaderCacheID, _requiredPrimvars);
        if (!_renamedParameters.empty()) {
            renderDelegate->AddRenamedParametersToCache(shaderCacheID, _renamedParameters);
        }
    }

    return shaderInstance;
}

#endif

/*! \brief  Creates a shader instance for the surface shader.
 */
MHWRender::MShaderInstance*
HdVP2Material::CompiledNetwork::_CreateShaderInstance(const HdMaterialNetwork& mat)
{
    MHWRender::MRenderer* const renderer = MHWRender::MRenderer::theRenderer();
    if (!TF_VERIFY(renderer)) {
        return nullptr;
    }

    const MHWRender::MShaderManager* const shaderMgr = renderer->getShaderManager();
    if (!TF_VERIFY(shaderMgr)) {
        return nullptr;
    }

    MHWRender::MShaderInstance* shaderInstance = nullptr;

    // UsdImagingMaterialAdapter has walked the shader graph and emitted nodes
    // and relationships in topological order to avoid forward-references, thus
    // we can run a reverse iteration to avoid connecting a fragment before any
    // of its downstream fragments.
    const auto rend = mat.nodes.rend();
    for (auto rit = mat.nodes.rbegin(); rit != rend; rit++) {
        const HdMaterialNode& node = *rit;

        const MString nodeId = node.identifier.GetText();
        const MString nodeName = node.path.GetNameToken().GetText();

        if (shaderInstance == nullptr) {
            shaderInstance = shaderMgr->getFragmentShader(nodeId, "outSurfaceFinal", true);
            if (shaderInstance == nullptr) {
                TF_WARN("Failed to create shader instance for %s", nodeId.asChar());
                break;
            }

            continue;
        }

        MStringArray outputNames, inputNames;

        for (const HdMaterialRelationship& rel : mat.relationships) {
            if (rel.inputId == node.path) {
                MString outputName = rel.inputName.GetText();
                outputNames.append(outputName);

                if (rel.outputId != _surfaceShaderId) {
                    std::string str = rel.outputId.GetName();
                    str += rel.outputName.GetString();
                    inputNames.append(str.c_str());
                } else {
                    inputNames.append(rel.outputName.GetText());
                }
            }
        }

        if (outputNames.length() > 0) {
            MUintArray invalidParamIndices;
            MStatus    status = shaderInstance->addInputFragmentForMultiParams(
                nodeId, nodeName, outputNames, inputNames, &invalidParamIndices);

            if (!status && TfDebug::IsEnabled(HDVP2_DEBUG_MATERIAL)) {
                TF_WARN(
                    "Error %s happened when connecting shader %s",
                    status.errorString().asChar(),
                    node.path.GetText());

                const unsigned int len = invalidParamIndices.length();
                for (unsigned int i = 0; i < len; i++) {
                    unsigned int   index = invalidParamIndices[i];
                    const MString& outputName = outputNames[index];
                    const MString& inputName = inputNames[index];
                    TF_WARN("  %s -> %s", outputName.asChar(), inputName.asChar());
                }
            }

            if (_IsUsdPrimvarReader(node)) {
                auto it = node.parameters.find(_tokens->varname);
                if (it != node.parameters.end()) {
                    const MString paramName = HdTokens->primvar.GetText();
                    const MString varname = TfStringify(it->second).c_str();
                    shaderInstance->renameParameter(paramName, varname);
                }
            }
        } else {
            TF_DEBUG(HDVP2_DEBUG_MATERIAL)
                .Msg("Failed to connect shader %s\n", node.path.GetText());
        }
    }

    return shaderInstance;
}

/*! \brief  Sets whether the compiled network's shaders are transparent or not.
 */
MStatus HdVP2Material::CompiledNetwork::SetShaderIsTransparent(bool isTransparent)
{
    MStatus status;
    if (_surfaceShader && status) {
        status = _surfaceShader->setIsTransparent(isTransparent);
    }
    if (_frontFaceShader && status) {
        status = _frontFaceShader->setIsTransparent(isTransparent);
    }
    if (_pointShader && status) {
        status = _pointShader->setIsTransparent(isTransparent);
    }
    return status;
}

/*! \brief  Updates parameters for the surface shader.
 */
void HdVP2Material::CompiledNetwork::_UpdateShaderInstance(
    HdSceneDelegate*         sceneDelegate,
    const HdMaterialNetwork& mat)
{
    if (!_surfaceShader) {
        _transparent = false;
        return;
    }

    MProfilingScope profilingScope(
        HdVP2RenderDelegate::sProfilerCategory, MProfiler::kColorD_L2, "UpdateShaderInstance");

    std::unordered_set<MString, MStringHash> updatedAttributes;
    auto setShaderParam = [&updatedAttributes, this](auto&& paramName, auto&& paramValue) {
        updatedAttributes.insert(paramName);
        return SetShaderParameter(paramName, paramValue);
    };

    // Update the transparency flag based on the material network.
    _transparent = _IsTransparent(mat);
    if (_transparent != _surfaceShader->isTransparent()) {
        SetShaderIsTransparent(_transparent);
    }

    for (const HdMaterialNode& node : mat.nodes) {
        MString nodeName = "";
#ifdef WANT_MATERIALX_BUILD
        const bool isMaterialXNode = _IsMaterialX(node);
        if (isMaterialXNode) {
            mx::NodeDefPtr nodeDef
                = _GetMaterialXData()._mtlxLibrary->getNodeDef(node.identifier.GetString());
            if (nodeDef
                && MaterialXMaya::ShaderGenUtil::TopoNeutralGraph::isTopologicalNodeDef(*nodeDef)) {
                // A topo node does not emit editable parameters:
                continue;
            }
#if MX_COMBINED_VERSION >= 13900
            if (!nodeDef && TfStringStartsWith(node.identifier.GetString(), "ND_swizzle_")) {
                // Swizzle was a topo node in 1.38 with no editable parameters.
                continue;
            }
#endif
            nodeName += _nodePathMap[node.path].GetName().c_str();
            if (node.path == _surfaceShaderId) {
                nodeName = "";
            } else {
                nodeName += "_";
            }
        } else
#endif
        {
            // Find the simplified path for the authored node path from the map which has been
            // created when applying VP2-specific fixes.
            const auto it = _nodePathMap.find(node.path);
            if (it == _nodePathMap.end()) {
                continue;
            }

            // The simplified path has only one token which is the node name.
            const SdfPath& nodePath = it->second;
            if (nodePath != _surfaceShaderId) {
                nodeName = nodePath.GetText();
            }
        }

        MStatus samplerStatus = MStatus::kFailure;

        if (_IsUsdUVTexture(node)) {
            const MHWRender::MSamplerStateDesc desc = _GetSamplerStateDesc(node);
            const MHWRender::MSamplerState*    sampler
                = _owner->_renderDelegate->GetSamplerState(desc);
            if (sampler) {
#ifdef WANT_MATERIALX_BUILD
                if (isMaterialXNode) {
                    const MString paramName = "_" + nodeName + "file_sampler";
                    samplerStatus = setShaderParam(paramName, *sampler);
                } else
#endif
                {
                    const MString paramName = nodeName + "fileSampler";
                    samplerStatus = setShaderParam(paramName, *sampler);
                }
            }
        }

        for (auto const& entry : node.parameters) {
            const TfToken& token = entry.first;
            const VtValue& value = entry.second;

            MString paramName = nodeName + token.GetText();

#ifdef WANT_MATERIALX_BUILD
            const auto itRename = _renamedParameters.find(paramName.asChar());
            if (itRename != _renamedParameters.end()) {
                paramName = itRename->second;
            }
#endif

            MStatus status = MStatus::kFailure;

            if (value.IsHolding<float>()) {
                const float& val = value.UncheckedGet<float>();
                status = setShaderParam(paramName, val);

#ifdef WANT_MATERIALX_BUILD
                if (!status) {
                    if (_IsFAParameter(node, paramName)) {
                        // Try as vector
                        float vec[4] { val, val, val, val };
                        status = setShaderParam(paramName, &vec[0]);
                    }
                }
#endif
            } else if (value.IsHolding<GfVec2f>()) {
                const float* val = value.UncheckedGet<GfVec2f>().data();
                status = setShaderParam(paramName, val);
            } else if (value.IsHolding<GfVec3f>()) {
                const float* val = value.UncheckedGet<GfVec3f>().data();
                status = setShaderParam(paramName, val);
            } else if (value.IsHolding<GfVec4f>()) {
                const float* val = value.UncheckedGet<GfVec4f>().data();
                status = setShaderParam(paramName, val);
            } else if (value.IsHolding<TfToken>()) {
                if (_IsUsdUVTexture(node)) {
                    if (token == UsdHydraTokens->wrapS || token == UsdHydraTokens->wrapT) {
                        // The two parameters have been converted to sampler state before entering
                        // this loop.
                        status = samplerStatus;
                    } else if (token == _tokens->sourceColorSpace) {
                        status = MStatus::kSuccess;
                    }
                }
            } else if (value.IsHolding<SdfAssetPath>()) {
                const SdfAssetPath& val = value.UncheckedGet<SdfAssetPath>();
                const std::string&  resolvedPath = val.GetResolvedPath();
                const std::string&  assetPath = val.GetAssetPath();
                if (_IsTextureFilenameAttribute(node, token)) {
                    const HdVP2TextureInfo& info = _owner->_AcquireTexture(
                        sceneDelegate, !resolvedPath.empty() ? resolvedPath : assetPath, node);

                    MHWRender::MTextureAssignment assignment;
                    assignment.texture = info._texture.get();
                    status = setShaderParam(paramName, assignment);

#ifndef HAS_COLOR_MANAGEMENT_SUPPORT_API
#ifdef WANT_MATERIALX_BUILD
                    // TODO: MaterialX image nodes have colorSpace metadata on the file attribute,
                    // and this can be found in the UsdShade version of the MaterialX document. At
                    // this point in time, there is no mechanism in Hydra to transmit metadata so
                    // this information will not reach the render delegate. Follow
                    // https://github.com/PixarAnimationStudios/USD/issues/1523 for future updates
                    // on colorspace handling in MaterialX/Hydra.
                    if (status && !isMaterialXNode) {
#else
                    if (status) {
#endif
                        paramName = nodeName + "isColorSpaceSRGB";
                        bool isSRGB = info._isColorSpaceSRGB;
                        auto scsIt = node.parameters.find(_tokens->sourceColorSpace);
                        if (scsIt != node.parameters.end()) {
                            const VtValue& scsValue = scsIt->second;
                            if (scsValue.IsHolding<TfToken>()) {
                                const TfToken& scsToken = scsValue.UncheckedGet<TfToken>();
                                if (scsToken == _tokens->raw) {
                                    isSRGB = false;
                                } else if (scsToken == _tokens->sRGB) {
                                    isSRGB = true;
                                }
                            }
                        }
                        status = setShaderParam(paramName, isSRGB);
                    }
#endif
                    // These parameters allow scaling texcoords into the proper coordinates of the
                    // Maya UDIM texture atlas:
                    if (status) {
#ifdef WANT_MATERIALX_BUILD
                        paramName = nodeName + (isMaterialXNode ? "uv_scale" : "stScale");
#else
                        paramName = nodeName + "stScale";
#endif
                        status = setShaderParam(paramName, info._stScale.data());
                    }
                    if (status) {
#ifdef WANT_MATERIALX_BUILD
                        paramName = nodeName + (isMaterialXNode ? "uv_offset" : "stOffset");
#else
                        paramName = nodeName + "stOffset";
#endif
                        status = setShaderParam(paramName, info._stOffset.data());
                    }
                }
            } else if (value.IsHolding<int>()) {
                const int& val = value.UncheckedGet<int>();
                if (node.identifier == UsdImagingTokens->UsdPreviewSurface
                    && token == _tokens->useSpecularWorkflow) {
                    status = setShaderParam(paramName, val != 0);
                } else {
                    status = setShaderParam(paramName, val);
                }
            } else if (value.IsHolding<bool>()) {
                const bool& val = value.UncheckedGet<bool>();
                status = setShaderParam(paramName, val);
            } else if (value.IsHolding<GfMatrix4d>()) {
                MMatrix matrix;
                value.UncheckedGet<GfMatrix4d>().Get(matrix.matrix);
                status = setShaderParam(paramName, matrix);
            } else if (value.IsHolding<GfMatrix4f>()) {
                MFloatMatrix matrix;
                value.UncheckedGet<GfMatrix4f>().Get(matrix.matrix);
                status = setShaderParam(paramName, matrix);
#ifdef WANT_MATERIALX_BUILD
            } else if (value.IsHolding<std::string>()) {
                // Some MaterialX nodes have a string member that does not translate to a shader
                // parameter.
                if (isMaterialXNode
                    && (token == _mtlxTokens->geomprop || token == _mtlxTokens->uaddressmode
                        || token == _mtlxTokens->vaddressmode || token == _mtlxTokens->filtertype
                        || token == _mtlxTokens->channels || token == _mtlxTokens->colorSpace)) {
                    status = MS::kSuccess;
                }
#endif
            }

            if (!status) {
                TF_DEBUG(HDVP2_DEBUG_MATERIAL)
                    .Msg("Failed to set shader parameter %s\n", paramName.asChar());
            }
        }
    }

    // Now that we have updated all parameters that were driven by Hydra, we must make sure all the
    // non-hydra-controlled parameters are at their default values. This fixes missing refresh when
    // deleting an authored attribute. The shader must not remember the value it had when the
    // attribute was authored.
    MStringArray parameterList;
    _surfaceShader->parameterList(parameterList);
    for (unsigned int i = 0; i < parameterList.length(); ++i) {
        const auto& parameterName = parameterList[i];
        if (updatedAttributes.count(parameterName)
            || _surfaceShader->isVaryingParameter(parameterName)) {
            continue;
        }

        switch (_surfaceShader->parameterType(parameterName)) {
        case MHWRender::MShaderInstance::kInvalid:
        case MHWRender::MShaderInstance::kSampler: continue;
        case MHWRender::MShaderInstance::kBoolean: {
            MStatus status;
            void*   defaultValue = _surfaceShader->parameterDefaultValue(parameterName, status);
            if (status) {
                SetShaderParameter(parameterName, static_cast<bool*>(defaultValue)[0]);
            }
        } break;
        case MHWRender::MShaderInstance::kInteger: {
            MStatus status;
            void*   defaultValue = _surfaceShader->parameterDefaultValue(parameterName, status);
            if (status) {
                SetShaderParameter(parameterName, static_cast<int*>(defaultValue)[0]);
            }
        } break;
        case MHWRender::MShaderInstance::kFloat:
        case MHWRender::MShaderInstance::kFloat2:
        case MHWRender::MShaderInstance::kFloat3:
        case MHWRender::MShaderInstance::kFloat4:
        case MHWRender::MShaderInstance::kFloat4x4Row:
        case MHWRender::MShaderInstance::kFloat4x4Col: {
            MStatus status;
            void*   defaultValue = _surfaceShader->parameterDefaultValue(parameterName, status);
            if (status) {
                SetShaderParameter(parameterName, static_cast<float*>(defaultValue));
            }
        } break;
        case MHWRender::MShaderInstance::kTexture1:
        case MHWRender::MShaderInstance::kTexture2:
        case MHWRender::MShaderInstance::kTexture3:
        case MHWRender::MShaderInstance::kTextureCube:
            MHWRender::MTextureAssignment assignment;
            assignment.texture = nullptr;
            SetShaderParameter(parameterName, assignment);
            break;
        }
    }

    // Front face shader should always have culling enabled
    if (_frontFaceShader) {
        _frontFaceShader->setParameter("cullStyle", (int)1);
    }

#ifdef HAS_COLOR_MANAGEMENT_SUPPORT_API
    _surfaceShader->addColorManagementTextures();
#endif
}

namespace {

void exitingCallback(void* /* unusedData */)
{
    // Maya does not unload plugins on exit.  Make sure we perform an orderly
    // cleanup of the texture cache. Otherwise the cache will clear after VP2
    // has shut down.
    HdVP2Material::OnMayaExit();
}

MCallbackId gExitingCbId = 0;
} // namespace

/*! \brief  Acquires a texture for the given image path.
 */
const HdVP2TextureInfo& HdVP2Material::_AcquireTexture(
    HdSceneDelegate*      sceneDelegate,
    const std::string&    path,
    const HdMaterialNode& node)
{
    // Register for the Maya exit message
    if (!gExitingCbId) {
        gExitingCbId = MSceneMessage::addCallback(MSceneMessage::kMayaExiting, exitingCallback);
    }

    // see if we already have the texture loaded.
    const auto it = _globalTextureMap.find(path);
    if (it != _globalTextureMap.end()) {
        HdVP2TextureInfoSharedPtr cacheEntry = it->second.lock();
        if (cacheEntry) {
            _localTextureMap[path] = cacheEntry;
            return *cacheEntry;
        } else {
            // if cacheEntry is nullptr then there is a stale entry in the _globalTextureMap. Erase
            // it now to simplify adding the new texture to the global cache later.
            _globalTextureMap.erase(it);
        }
    }

    // Get fallback color if defined
    bool    hasFallbackColor { false };
    GfVec4f fallbackColor { 0.18, 0.18, 0.18, 0 };
    auto    fallbackIt = node.parameters.find(_tokens->fallback);
    if (fallbackIt != node.parameters.end() && fallbackIt->second.IsHolding<GfVec4f>()) {
        fallbackColor = fallbackIt->second.UncheckedGet<GfVec4f>();
        hasFallbackColor = true;
    }

    if (_IsDisabledAsyncTextureLoading()) {
        bool        isSRGB = false;
        MFloatArray uvScaleOffset;

        MHWRender::MTexture* texture
            = _LoadTexture(path, hasFallbackColor, fallbackColor, isSRGB, uvScaleOffset);

        HdVP2TextureInfoSharedPtr info = std::make_shared<HdVP2TextureInfo>();
        // path should never already be in _localTextureMap because if it was
        // we'd have found it in _globalTextureMap
        _localTextureMap.emplace(path, info);
        // path should never already be in _globalTextureMap because if it was present
        // and nullptr then we erased it.
        _globalTextureMap.emplace(path, info);
        info->_texture.reset(texture);
        info->_isColorSpaceSRGB = isSRGB;
        if (uvScaleOffset.length() > 0) {
            TF_VERIFY(uvScaleOffset.length() == 4);
            info->_stScale.Set(
                uvScaleOffset[0], uvScaleOffset[1]); // The first 2 elements are the scale
            info->_stOffset.Set(
                uvScaleOffset[2], uvScaleOffset[3]); // The next two elements are the offset
        }

        return *info;
    }

    auto* task = new TextureLoadingTask(this, sceneDelegate, path, hasFallbackColor, fallbackColor);
    _textureLoadingTasks.emplace(path, task);
    return task->GetFallbackTextureInfo();
}

void HdVP2Material::EnqueueLoadTextures()
{
    for (const auto& task : _textureLoadingTasks) {
        if (task.second->EnqueueLoadOnIdle()) {
            ++_runningTasksCounter;
        }
    }
}

void HdVP2Material::ClearPendingTasks()
{
    // Inform tasks that have not started or finished that this material object
    // is no longer valid
    for (auto& task : _textureLoadingTasks) {
        if (task.second->Terminate()) {
            // Delete the pointer: we can only do that outside of the object scope
            delete task.second;
        }
    }

    // Remove the reference of all the tasks
    _textureLoadingTasks.clear();
    // Reset counter, tasks that have started but not finished yet would be
    // terminated and won't trigger any refresh
    _runningTasksCounter = 0;
}

void HdVP2Material::_UpdateLoadedTexture(
    HdSceneDelegate*     sceneDelegate,
    const std::string&   path,
    MHWRender::MTexture* texture,
    bool                 isColorSpaceSRGB,
    const MFloatArray&   uvScaleOffset)
{
    // Decrease the counter if texture finished loading.
    // Please notice that we do not do the same thing for terminated tasks,
    // when termination is requested, the scene delegate is being reset and
    // the counter would be reset to 0 (see `ClearPendingTasks()` method),
    // no need to decrease the counter one by one.
    if (_runningTasksCounter.load() > 0) {
        --_runningTasksCounter;
    }

    // Pop the task object from the container, since this method is
    // called directly from the task object method `loadOnIdle()`,
    // we do not handle the deletion here, we will let the
    // function on idle to delete the task object.
    _textureLoadingTasks.erase(path);

    // Check the cache again. If the texture is not in the cache
    // the add it.
    if (_globalTextureMap.find(path) == _globalTextureMap.end()) {
        HdVP2TextureInfoSharedPtr info = std::make_shared<HdVP2TextureInfo>();
        // path should never already be in _localTextureMap because if it was
        // we'd have found it in _globalTextureMap
        _localTextureMap.emplace(path, info);
        // path should never already be in _globalTextureMap because if it was present
        // and nullptr then we erased it.
        _globalTextureMap.emplace(path, info);
        info->_texture.reset(texture);
        info->_isColorSpaceSRGB = isColorSpaceSRGB;
        if (uvScaleOffset.length() > 0) {
            TF_VERIFY(uvScaleOffset.length() == 4);
            info->_stScale.Set(
                uvScaleOffset[0], uvScaleOffset[1]); // The first 2 elements are the scale
            info->_stOffset.Set(
                uvScaleOffset[2], uvScaleOffset[3]); // The next two elements are the offset
        }
    }

    // Mark sprim dirty
    sceneDelegate->GetRenderIndex().GetChangeTracker().MarkSprimDirty(
        GetId(), HdMaterial::DirtyResource);

    _ScheduleRefresh();
}

/*static*/
void HdVP2Material::_ScheduleRefresh()
{
    // We need this mutex due to the variables used in this method are static
    std::lock_guard<std::mutex> lock(_refreshMutex);

    auto isTimeout = []() {
        auto diff(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - _startTime));
        if (static_cast<std::size_t>(diff.count()) < kRefreshDuration) {
            return false;
        }
        _startTime = std::chrono::steady_clock::now();
        return true;
    };

    // Trigger refresh for the last texture or when it is timeout
    if (!_runningTasksCounter.load() || isTimeout()) {
        M3dView::scheduleRefreshAllViews();
    }
}

MHWRender::MShaderInstance* HdVP2Material::CompiledNetwork::GetFrontFaceShader() const
{
    if (!_frontFaceShader && _surfaceShader) {
        _frontFaceShader.reset(_surfaceShader->clone());
        _frontFaceShader->setParameter("cullStyle", (int)1);
    }

    return _frontFaceShader.get();
}

MHWRender::MShaderInstance* HdVP2Material::CompiledNetwork::GetPointShader() const
{
    if (!_pointShader && _surfaceShader) {
        _pointShader.reset(_surfaceShader->clone());
        _pointShader->addInputFragment("PointsGeometry", "GPUStage", "GPUStage");
    }

    return _pointShader.get();
}

HdVP2Material::NetworkConfig HdVP2Material::_GetCompiledConfig(const TfToken& reprToken) const
{
    return (reprToken == HdReprTokens->smoothHull) ? kFull : kUntextured;
}

MHWRender::MShaderInstance*
HdVP2Material::GetSurfaceShader(const TfToken& reprToken, bool backfaceCull) const
{
    const auto& compiledNetwork = _compiledNetworks[_GetCompiledConfig(reprToken)];
    return backfaceCull ? compiledNetwork.GetFrontFaceShader() : compiledNetwork.GetSurfaceShader();
}

MHWRender::MShaderInstance* HdVP2Material::GetPointShader(const TfToken& reprToken) const
{
    return _compiledNetworks[_GetCompiledConfig(reprToken)].GetPointShader();
}

const TfTokenVector& HdVP2Material::GetRequiredPrimvars(const TfToken& reprToken) const
{
    return _compiledNetworks[_GetCompiledConfig(reprToken)].GetRequiredPrimvars();
}

void HdVP2Material::SubscribeForMaterialUpdates(const SdfPath& rprimId)
{
    std::lock_guard<std::mutex> lock(_materialSubscriptionsMutex);

    _materialSubscriptions.insert(rprimId);
}

void HdVP2Material::UnsubscribeFromMaterialUpdates(const SdfPath& rprimId)
{
    std::lock_guard<std::mutex> lock(_materialSubscriptionsMutex);

    _materialSubscriptions.erase(rprimId);
}

void HdVP2Material::MaterialChanged(HdSceneDelegate* sceneDelegate)
{
    std::lock_guard<std::mutex> lock(_materialSubscriptionsMutex);

    HdChangeTracker& changeTracker = sceneDelegate->GetRenderIndex().GetChangeTracker();
    for (const SdfPath& rprimId : _materialSubscriptions) {
        changeTracker.MarkRprimDirty(rprimId, HdChangeTracker::DirtyMaterialId);
    }
}

void HdVP2Material::OnMayaExit()
{
    _TransientTexturePreserver::GetInstance().OnMayaExit();
    _globalTextureMap.clear();
    HdVP2RenderDelegate::OnMayaExit();
}

PXR_NAMESPACE_CLOSE_SCOPE
