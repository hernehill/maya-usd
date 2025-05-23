//
// Copyright 2018 Pixar
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
#include "shadingUtil.h"

#include <mayaUsd/fileio/jobs/jobArgs.h>
#include <mayaUsd/fileio/translators/translatorUtil.h>
#include <mayaUsd/utils/utilFileSystem.h>

#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/token.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/sdr/registry.h>
#include <pxr/usd/sdr/shaderNode.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/output.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usdImaging/usdImaging/tokens.h>

#include <maya/MPlug.h>
#include <maya/MString.h>

#include <ghc/filesystem.hpp>

#include <regex>
#include <string>
#include <system_error>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
// clang-format off
TF_DEFINE_PRIVATE_TOKENS(
    _tokens,

    ((UDIMTag, "<UDIM>"))

    (place2dTexture)
    (coverage)
    (translateFrame)
    (rotateFrame) 
    (mirrorU)
    (mirrorV)
    (stagger)
    (wrapU)
    (wrapV)
    (repeatUV)
    (offset)
    (rotateUV)
    (noiseUV)
    (vertexUvOne)
    (vertexUvTwo)
    (vertexUvThree)
    (vertexCameraOne)
    (outUvFilterSize)
    (uvFilterSize)
    (outUV)
    (uvCoord)
);
// clang-format on

static const TfTokenVector _Place2dTextureConnections = {
    _tokens->coverage,    _tokens->translateFrame, _tokens->rotateFrame,   _tokens->mirrorU,
    _tokens->mirrorV,     _tokens->stagger,        _tokens->wrapU,         _tokens->wrapV,
    _tokens->repeatUV,    _tokens->offset,         _tokens->rotateUV,      _tokens->noiseUV,
    _tokens->vertexUvOne, _tokens->vertexUvTwo,    _tokens->vertexUvThree, _tokens->vertexCameraOne
};

} // namespace

std::string
UsdMayaShadingUtil::GetStandardAttrName(const MPlug& attrPlug, bool allowMultiElementArrays)
{
    if (!attrPlug.isElement()) {
        const MString mayaPlugName = attrPlug.partialName(false, false, false, false, false, true);
        return mayaPlugName.asChar();
    }

    const MString mayaPlugName
        = attrPlug.array().partialName(false, false, false, false, false, true);
    const unsigned int logicalIndex = attrPlug.logicalIndex();

    if (allowMultiElementArrays) {
        return TfStringPrintf("%s_%d", mayaPlugName.asChar(), logicalIndex);
    } else if (logicalIndex == 0) {
        return mayaPlugName.asChar();
    }

    return std::string();
}

UsdShadeInput UsdMayaShadingUtil::CreateMaterialInputAndConnectShader(
    UsdShadeMaterial&       material,
    const TfToken&          materialInputName,
    const SdfValueTypeName& inputTypeName,
    UsdShadeShader&         shader,
    const TfToken&          shaderInputName)
{
    if (!material || !shader) {
        return UsdShadeInput();
    }

    UsdShadeInput materialInput = material.CreateInput(materialInputName, inputTypeName);

    UsdShadeInput shaderInput = shader.CreateInput(shaderInputName, inputTypeName);

    shaderInput.ConnectToSource(materialInput);

    return materialInput;
}

UsdShadeOutput UsdMayaShadingUtil::CreateShaderOutputAndConnectMaterial(
    UsdShadeShader&   shader,
    UsdShadeMaterial& material,
    const TfToken&    terminalName,
    const TfToken&    renderContext)
{
    if (!shader || !material) {
        return UsdShadeOutput();
    }

    UsdShadeOutput materialOutput;
    if (terminalName == UsdShadeTokens->surface) {
        materialOutput = material.CreateSurfaceOutput(renderContext);
    } else if (terminalName == UsdShadeTokens->volume) {
        materialOutput = material.CreateVolumeOutput(renderContext);
    } else if (terminalName == UsdShadeTokens->displacement) {
        materialOutput = material.CreateDisplacementOutput(renderContext);
    } else {
        return UsdShadeOutput();
    }

    // Make sure the shading node has a registered output by that name.
    SdrRegistry& registry = SdrRegistry::GetInstance();
    TfToken      nodeID;
    shader.GetIdAttr().Get(&nodeID);
    TfToken               outputName = terminalName;
    SdrShaderNodeConstPtr shaderNodeDef = registry.GetShaderNodeByIdentifier(nodeID);
    if (shaderNodeDef) {
#if PXR_VERSION >= 2505
        const SdrTokenVec& outputNames = shaderNodeDef->GetShaderOutputNames();
#else
        const NdrTokenVec& outputNames = shaderNodeDef->GetOutputNames();
#endif
        if (std::find(outputNames.cbegin(), outputNames.cend(), terminalName) == outputNames.cend()
            && outputNames.size() == 1) {
            outputName = outputNames.front();
        }
    }
    UsdShadeOutput shaderOutput = shader.CreateOutput(outputName, materialOutput.GetTypeName());

    UsdPrim parentPrim = shader.GetPrim().GetParent();
    if (parentPrim == material.GetPrim()) {
        materialOutput.ConnectToSource(shaderOutput);
    } else {
        // If the surface is inside a multi-material node graph, then we must create an intermediate
        // output on the NodeGraph
        UsdShadeNodeGraph parentNodeGraph(parentPrim);
        UsdShadeOutput    parentOutput
            = parentNodeGraph.CreateOutput(terminalName, materialOutput.GetTypeName());
        parentOutput.ConnectToSource(shaderOutput);
        materialOutput.ConnectToSource(parentOutput);
    }

    return shaderOutput;
}

void UsdMayaShadingUtil::ConnectPlace2dTexture(MObject textureNode, MObject uvNode)
{
    MStatus           status;
    MFnDependencyNode uvDepFn(uvNode, &status);
    MFnDependencyNode depFn(textureNode, &status);
    {
        MPlug filePlug = depFn.findPlug(_tokens->uvCoord.GetText(), true, &status);
        if (!filePlug.isDestination()) {
            MPlug uvPlug = uvDepFn.findPlug(_tokens->outUV.GetText(), true, &status);
            UsdMayaUtil::Connect(uvPlug, filePlug, false);
        }
    }
    {
        MPlug filePlug = depFn.findPlug(_tokens->uvFilterSize.GetText(), true, &status);
        if (!filePlug.isDestination()) {
            MPlug uvPlug = uvDepFn.findPlug(_tokens->outUvFilterSize.GetText(), true, &status);
            UsdMayaUtil::Connect(uvPlug, filePlug, false);
        }
    }
    for (const TfToken& uvName : _Place2dTextureConnections) {
        MPlug filePlug = depFn.findPlug(uvName.GetText(), true, &status);
        if (!filePlug.isDestination()) {
            MPlug uvPlug = uvDepFn.findPlug(uvName.GetText(), true, &status);
            UsdMayaUtil::Connect(uvPlug, filePlug, false);
        }
    }
}

MObject UsdMayaShadingUtil::CreatePlace2dTextureAndConnectTexture(MObject textureNode)
{
    MStatus status;
    MObject uvObj;
    if (!(UsdMayaTranslatorUtil::CreateShaderNode(
            _tokens->place2dTexture.GetText(),
            _tokens->place2dTexture.GetText(),
            UsdMayaShadingNodeType::Utility,
            &status,
            &uvObj))) {
        // we need to make sure those types are loaded..
        MFnDependencyNode depFn(textureNode);
        TF_RUNTIME_ERROR(
            "Could not create place2dTexture for texture '%s'.\n", depFn.name().asChar());
        return MObject();
    }

    // Connect manually (fileTexturePlacementConnect is not available in batch):
    ConnectPlace2dTexture(textureNode, uvObj);

    return uvObj;
}

namespace {
// Match UDIM pattern, from 1001 to 1999
const std::regex
    _udimRegex(".*[^\\d](1(?:[0-9][0-9][1-9]|[1-9][1-9]0|0[1-9]0|[1-9]00))(?:[^\\d].*|$)");
} // namespace

void UsdMayaShadingUtil::ResolveUsdTextureFileName(
    std::string&       fileTextureName,
    const std::string& usdFileName,
    const TfToken&     relativeMode,
    bool               isUDIM)
{
    // WARNING: This extremely minimal attempt at making the file path relative
    //          to the USD stage is a stopgap measure intended to provide
    //          minimal interop. It will be replaced by proper use of Maya and
    //          USD asset resolvers.

    bool makeRelative = (relativeMode == UsdMayaJobExportArgsTokens->relative);
    bool makeAbsolute = (relativeMode == UsdMayaJobExportArgsTokens->absolute);

    // When in automatic mode (neither relative nor absolute), select a mode based on
    // the input texture filename. Maya always keeps paths as absolute paths internally,
    // so we need to detect if the path is in the Maya project folders.
    const bool isForced = (makeRelative || makeAbsolute);
    if (!isForced) {
        std::string relativePath = UsdMayaUtilFileSystem::getPathRelativeToProject(fileTextureName);
        if (relativePath.empty()) {
            makeAbsolute = true;
        } else {
            makeRelative = true;
        }
    }

    if (makeAbsolute) {
        std::error_code       ec;
        ghc::filesystem::path absolutePath = ghc::filesystem::absolute(fileTextureName, ec);
        if (!ec && !absolutePath.empty()) {
            fileTextureName = absolutePath.generic_string();
        }
    } else if (makeRelative) {
        // Note: for package files, the exporter needs full paths, so check the file name
        //       extension and don't make the path relative if it is an extension-package.
        TfToken fileExt(TfGetExtension(usdFileName));
        if (fileExt != UsdMayaTranslatorTokens->UsdFileExtensionPackage) {
            ghc::filesystem::path usdDir(usdFileName);
            usdDir = usdDir.parent_path();
            std::error_code       ec;
            ghc::filesystem::path relativePath
                = ghc::filesystem::relative(fileTextureName, usdDir, ec);
            if (!ec && !relativePath.empty()) {
                fileTextureName = relativePath.generic_string();
            }
        }
    }

    // Update filename in case of UDIM
    if (isUDIM) {
        std::smatch match;
        if (std::regex_search(fileTextureName, match, _udimRegex) && match.size() == 2) {
            fileTextureName = std::string(match[0].first, match[1].first)
                + _tokens->UDIMTag.GetString() + std::string(match[1].second, match[0].second);
        }
    }
}

int UsdMayaShadingUtil::GetNumberOfChannels(const std::string& fileTextureName)
{
    // Using Hio because the Maya texture node does not provide the information:
    HioImageSharedPtr image = HioImage::OpenForReading(fileTextureName.c_str());
    HioFormat         imageFormat = image ? image->GetFormat() : HioFormat::HioFormatUNorm8Vec4;

    // In case of unknown, use 4 channel image:
    if (imageFormat == HioFormat::HioFormatInvalid) {
        imageFormat = HioFormat::HioFormatUNorm8Vec4;
    }

    return HioGetComponentCount(imageFormat);
}
