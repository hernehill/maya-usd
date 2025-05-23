//
// Copyright 2016 Pixar
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
#include "translatorUtil.h"

#include "pxr/usd/sdr/registry.h"
#include "pxr/usd/sdr/shaderNode.h"
#include "pxr/usd/sdr/shaderProperty.h"

#include <mayaUsd/fileio/primReaderArgs.h>
#include <mayaUsd/fileio/primReaderContext.h>
#include <mayaUsd/fileio/translators/translatorXformable.h>
#include <mayaUsd/fileio/utils/adaptor.h>
#include <mayaUsd/fileio/utils/xformStack.h>
#include <mayaUsd/undo/OpUndoItems.h>
#include <mayaUsd/utils/converter.h>
#include <mayaUsd/utils/util.h>

#include <pxr/pxr.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdLux/shadowAPI.h>
#include <pxr/usd/usdLux/shapingAPI.h>
#include <pxr/usd/usdShade/tokens.h>

#include <maya/MDagModifier.h>
#include <maya/MDagPath.h>
#include <maya/MFn.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MGlobal.h>
#include <maya/MObject.h>
#include <maya/MStatus.h>
#include <maya/MString.h>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,

    (usdSuppressProperty));

const MString _DEFAULT_TRANSFORM_TYPE("transform");

/* static */
bool UsdMayaTranslatorUtil::CreateTransformNode(
    const UsdPrim&               usdPrim,
    MObject&                     parentNode,
    const UsdMayaPrimReaderArgs& args,
    UsdMayaPrimReaderContext*    context,
    MStatus*                     status,
    MObject*                     mayaNodeObj)
{
    if (!usdPrim || !usdPrim.IsA<UsdGeomXformable>()) {
        return false;
    }

    if (!CreateNode(usdPrim, _DEFAULT_TRANSFORM_TYPE, parentNode, context, status, mayaNodeObj)) {
        return false;
    }

    // Read xformable attributes from the UsdPrim on to the transform node.
    UsdGeomXformable xformable(usdPrim);
    UsdMayaTranslatorXformable::Read(xformable, *mayaNodeObj, args, context);

    return true;
}

/* static */
bool UsdMayaTranslatorUtil::CreateDummyTransformNode(
    const UsdPrim&                  usdPrim,
    MObject&                        parentNode,
    bool                            importTypeName,
    const UsdMayaPrimReaderArgs&    args,
    UsdMayaPrimReaderContext*       context,
    MStatus*                        status,
    MObject*                        mayaNodeObj,
    const UsdMayaDummyTransformType dummyTransformType)
{
    if (!usdPrim) {
        return false;
    }

    if (!CreateNode(usdPrim, _DEFAULT_TRANSFORM_TYPE, parentNode, context, status, mayaNodeObj)) {
        return false;
    }

    MFnDagNode dagNode(*mayaNodeObj);

    // Set the typeName on the adaptor.
    if (UsdMayaAdaptor adaptor = UsdMayaAdaptor(*mayaNodeObj)) {
        VtValue typeName;
        if (!usdPrim.HasAuthoredTypeName()) {
            // A regular typeless def.
            typeName = TfToken();
        } else if (importTypeName) {
            // Preserve type info for round-tripping.
            typeName = usdPrim.GetTypeName();
        } else {
            // Unknown type name; treat this as though it were a typeless def.
            typeName = TfToken();

            // If there is a typename that we're ignoring, leave a note so that
            // we know where it came from.
            const std::string notes = TfStringPrintf(
                "Imported from @%s@<%s> with type '%s'",
                usdPrim.GetStage()->GetRootLayer()->GetIdentifier().c_str(),
                usdPrim.GetPath().GetText(),
                usdPrim.GetTypeName().GetText());
            UsdMayaUtil::SetNotes(dagNode, notes);
        }
        adaptor.SetMetadata(SdfFieldKeys->TypeName, typeName);
    }

    if (dummyTransformType == UsdMayaDummyTransformType::UnlockedTransform) {
        // If we are leaving transform-related plugs unlocked, we assume that
        // the Maya transform must reflect the current prim's transformation.
        UsdGeomXformable xformable(usdPrim);
        if (xformable) {
            UsdMayaTranslatorXformable::Read(xformable, *mayaNodeObj, args, context);
        }
        return true;
    }

    // Lock all the transform attributes.
    for (const UsdMayaXformOpClassification& opClass : UsdMayaXformStack::MayaStack().GetOps()) {
        if (!opClass.IsInvertedTwin()) {
            MPlug plug = dagNode.findPlug(opClass.GetName().GetText(), true);
            if (!plug.isNull()) {
                if (plug.isCompound()) {
                    for (unsigned int i = 0; i < plug.numChildren(); ++i) {
                        MPlug child = plug.child(i);
                        child.setKeyable(false);
                        child.setLocked(true);
                        child.setChannelBox(false);
                    }
                } else {
                    plug.setKeyable(false);
                    plug.setLocked(true);
                    plug.setChannelBox(false);
                }
            }
        }
    }

    return true;
}

/* static */
bool UsdMayaTranslatorUtil::CreateNode(
    const UsdPrim&            usdPrim,
    const MString&            nodeTypeName,
    MObject&                  parentNode,
    UsdMayaPrimReaderContext* context,
    MStatus*                  status,
    MObject*                  mayaNodeObj)
{
    return CreateNode(usdPrim.GetPath(), nodeTypeName, parentNode, context, status, mayaNodeObj);
}

/* static */
bool UsdMayaTranslatorUtil::CreateNode(
    const SdfPath&            path,
    const MString&            nodeTypeName,
    MObject&                  parentNode,
    UsdMayaPrimReaderContext* context,
    MStatus*                  status,
    MObject*                  mayaNodeObj)
{
    if (!CreateNode(
            MString(path.GetName().c_str(), path.GetName().size()),
            nodeTypeName,
            parentNode,
            status,
            mayaNodeObj)) {
        return false;
    }

    if (context) {
        context->RegisterNewMayaNode(path.GetString(), *mayaNodeObj);
    }

    return true;
}

/* static */
bool UsdMayaTranslatorUtil::CreateNode(
    const MString& nodeName,
    const MString& nodeTypeName,
    MObject&       parentNode,
    MStatus*       status,
    MObject*       mayaNodeObj)
{
    // XXX:
    // Using MFnDagNode::create() results in nodes that are not properly
    // registered with parent scene assemblies. For now, just massaging the
    // transform code accordingly so that child scene assemblies properly post
    // their edits to their parents-- if this is indeed the best pattern for
    // this, all Maya*Reader node creation needs to be adjusted accordingly (for
    // much less trivial cases like MFnMesh).
    MDagModifier& dagMod = MayaUsd::MDagModifierUndoItem::create("Generic node creation");
    *mayaNodeObj = dagMod.createNode(nodeTypeName, parentNode, status);
    CHECK_MSTATUS_AND_RETURN(*status, false);
    *status = dagMod.renameNode(*mayaNodeObj, nodeName);
    CHECK_MSTATUS_AND_RETURN(*status, false);
    *status = dagMod.doIt();
    CHECK_MSTATUS_AND_RETURN(*status, false);

    return TF_VERIFY(!mayaNodeObj->isNull());
}

/* static */
bool UsdMayaTranslatorUtil::CreateShaderNode(
    const MString&               nodeName,
    const MString&               nodeTypeName,
    const UsdMayaShadingNodeType shadingNodeType,
    MStatus*                     status,
    MObject*                     shaderObj,
    const MObject                parentNode)
{
    MString typeFlag;
    switch (shadingNodeType) {
    case UsdMayaShadingNodeType::Light:
        typeFlag = "-al"; // -asLight
        break;
    case UsdMayaShadingNodeType::PostProcess:
        typeFlag = "-app"; // -asPostProcess
        break;
    case UsdMayaShadingNodeType::Rendering:
        typeFlag = "-ar"; // -asRendering
        break;
    case UsdMayaShadingNodeType::Shader:
        typeFlag = "-as"; // -asShader
        break;
    case UsdMayaShadingNodeType::Texture:
        typeFlag = "-icm -at"; // -isColorManaged -asTexture
        break;
    case UsdMayaShadingNodeType::Utility:
        typeFlag = "-au"; // -asUtility
        break;
    default: {
        MFnDependencyNode depNodeFn;
        depNodeFn.create(nodeTypeName, nodeName, status);
        CHECK_MSTATUS_AND_RETURN(*status, false);
        *shaderObj = depNodeFn.object(status);
        CHECK_MSTATUS_AND_RETURN(*status, false);
        return true;
    }
    }

    MString parentFlag;
    if (!parentNode.isNull()) {
        const MFnDagNode parentDag(parentNode, status);
        CHECK_MSTATUS_AND_RETURN(*status, false);
        *status = parentFlag.format(" -p \"^1s\"", parentDag.fullPathName());
        CHECK_MSTATUS_AND_RETURN(*status, false);
    }

    MString cmd;
    // ss = skipSelect
    *status = cmd.format(
        "shadingNode ^1s^2s -ss -n \"^3s\" \"^4s\"", typeFlag, parentFlag, nodeName, nodeTypeName);
    CHECK_MSTATUS_AND_RETURN(*status, false);

    const MString createdNode = MGlobal::executeCommandStringResult(cmd, false, false, status);
    CHECK_MSTATUS_AND_RETURN(*status, false);

    *status = UsdMayaUtil::GetMObjectByName(createdNode, *shaderObj);
    CHECK_MSTATUS_AND_RETURN(*status, false);

    // Lights are unique in that they're the only DAG nodes we might create in
    // this function, so they also involve a transform node. The shadingNode
    // command unfortunately seems to return the transform node for the light
    // and not the light node itself, so we may need to manually find the light
    // so we can return that instead.
    if (shaderObj->hasFn(MFn::kDagNode)) {
        const MFnDagNode dagNodeFn(*shaderObj, status);
        CHECK_MSTATUS_AND_RETURN(*status, false);
        MDagPath dagPath;
        *status = dagNodeFn.getPath(dagPath);
        CHECK_MSTATUS_AND_RETURN(*status, false);
        unsigned int numShapes = 0u;
        *status = dagPath.numberOfShapesDirectlyBelow(numShapes);
        CHECK_MSTATUS_AND_RETURN(*status, false);
        if (numShapes == 1u) {
            *status = dagPath.extendToShape();
            CHECK_MSTATUS_AND_RETURN(*status, false);
            *shaderObj = dagPath.node(status);
            CHECK_MSTATUS_AND_RETURN(*status, false);
        }
    }

    return true;
}

UsdMayaShadingNodeType
UsdMayaTranslatorUtil::ComputeShadingNodeTypeForMayaTypeName(const TfToken& mayaNodeTypeName)
{
    // Use NonShading as a fallback.
    UsdMayaShadingNodeType shadingNodeType = UsdMayaShadingNodeType::NonShading;

    MStatus status;
    MString cmd;
    status = cmd.format("getClassification ^1s", mayaNodeTypeName.GetText());
    CHECK_MSTATUS_AND_RETURN(status, shadingNodeType);

    MStringArray compoundClassifications;
    status = MGlobal::executeCommand(cmd, compoundClassifications, false, false);
    CHECK_MSTATUS_AND_RETURN(status, shadingNodeType);

    static const std::vector<std::pair<std::string, UsdMayaShadingNodeType>> _classificationsToTypes
        = { { "texture/", UsdMayaShadingNodeType::Texture },
            { "utility/", UsdMayaShadingNodeType::Utility },
            { "shader/", UsdMayaShadingNodeType::Shader } };

    // The docs for getClassification() are pretty confusing. You'd think that
    // the string array returned would give you each "classification", but
    // instead, it's a list of "single compound classification string by
    // joining the individual classifications with ':'".

    // Loop over the compoundClassifications, though I believe
    // compoundClassifications will always have size 0 or 1.
    for (const MString& compoundClassification : compoundClassifications) {
        const std::string compoundClassificationStr(compoundClassification.asChar());
        for (const std::string& classification : TfStringSplit(compoundClassificationStr, ":")) {
            for (const auto& classPrefixAndType : _classificationsToTypes) {
                if (TfStringStartsWith(classification, classPrefixAndType.first)) {
                    return classPrefixAndType.second;
                }
            }
        }
    }

    return shadingNodeType;
}

/* static */
bool UsdMayaTranslatorUtil::SetUsdTypeName(const MObject& mayaNodeObj, const TfToken& usdTypeName)
{
    if (UsdMayaAdaptor adaptor = UsdMayaAdaptor(mayaNodeObj)) {
        VtValue typeName;
        typeName = usdTypeName;
        adaptor.SetMetadata(SdfFieldKeys->TypeName, typeName);
        return true;
    }
    return false;
}

// Below are some helpers for the using Sdr to read/write RfM lights to Usd.
// XXX: LightFilters not yet implemented.
// XXX: GeometryLight geometry not yet implemented.
// XXX: DomeLight LightPortals not yet implemented.

// Returns true if the property is something that should be written to or read
// from the usd.  Some properties will have metadata that specifies that they
// are not meant to be serialized.
static bool _ShouldBeWrittenInUsd(const SdrShaderPropertyConstPtr& shaderProp)
{
    std::string usdSuppressProperty;

    if (TfMapLookup(shaderProp->GetMetadata(), _tokens->usdSuppressProperty, &usdSuppressProperty)
        && usdSuppressProperty == "True") {
        return false;
    }

    return true;
}

// Reads from the usdAttribute and writes into attrPlug (using sdfValueTypeName
// to help guide the conversion).
//
// Returns true if successful.  This will return false if it failed and populate
// reason with the reason for failure.
static bool _ReadFromUsdAttribute(
    const UsdAttribute&     usdAttr,
    const SdfValueTypeName& sdfValueTypeName,
    MPlug&                  attrPlug,
    std::string*            reason)
{
    static constexpr bool isArrayPlug = false;
    if (const MayaUsd::Converter* converter
        = MayaUsd::Converter::find(sdfValueTypeName, isArrayPlug)) {
        static const MayaUsd::ConverterArgs args;
        converter->convert(usdAttr, attrPlug, args);
    } else {
        if (reason) {
            *reason = TfStringPrintf(
                "Unsupported type %s on attr %s.",
                sdfValueTypeName.GetAsToken().GetText(),
                usdAttr.GetPath().GetText());
        }
        return false;
    }
    return true;
}

static inline TfToken _ShaderAttrName(const std::string& shaderParamName)
{
    return TfToken(UsdShadeTokens->inputs.GetString() + shaderParamName);
}

// Reads the property identified by shaderProp from the prim onto depFn.
//
// Returns true if successful.  This will return false if it failed and populate
// reason with the reason for failure.
static bool _ReadProperty(
    const SdrShaderPropertyConstPtr& shaderProp,
    const UsdPrim&                   usdPrim,
    const MFnDependencyNode&         depFn,
    std::string*                     reason)
{
    const TfToken mayaAttrName(shaderProp->GetImplementationName());
    const TfToken usdAttrName(_ShaderAttrName(shaderProp->GetName()));
#if PXR_VERSION <= 2408
    const SdfValueTypeName sdfValueTypeName = shaderProp->GetTypeAsSdfType().first;
#else
    const SdfValueTypeName sdfValueTypeName = shaderProp->GetTypeAsSdfType().GetSdfType();
#endif

    UsdAttribute usdAttr = usdPrim.GetAttribute(usdAttrName);
    if (!usdAttr) {
        // If no attribute, no need to author anything.
        return true;
    }

    MStatus status;
    MPlug   attrPlug = depFn.findPlug(mayaAttrName.GetText(), &status);
    if (status != MS::kSuccess) {
        if (reason) {
            *reason = TfStringPrintf(
                "Could not get maya attr %s.%s.", depFn.name().asChar(), mayaAttrName.GetText());
        }
        return false;
    }

    // Use the converter to read from usd attribute and set it in Maya.
    return _ReadFromUsdAttribute(usdAttr, sdfValueTypeName, attrPlug, reason);
}

// Gets the stored in attrPlug using the sdfValueTypeName to help guide the
// conversion.
static VtValue _GetValue(const MPlug& attrPlug, const SdfValueTypeName& sdfValueTypeName)
{
    static constexpr bool isArrayPlug = false;
    if (const MayaUsd::Converter* converter
        = MayaUsd::Converter::find(sdfValueTypeName, isArrayPlug)) {
        static const MayaUsd::ConverterArgs args;

        VtValue value;
        converter->convert(attrPlug, value, args);
        return value;
    } else {
        TF_RUNTIME_ERROR(
            "Could not get value for  %s (unconvertable type).", attrPlug.name().asChar());
        return VtValue();
    }
}

// based on UsdSchemaBase::_CreateAttr
static UsdAttribute _CreateAttr(
    const UsdPrim&          usdPrim,
    const TfToken&          attrName,
    const SdfValueTypeName& typeName,
    const VtValue&          defaultValue)
{
    const bool                      writeSparsely = true;
    static constexpr bool           custom = false;
    static constexpr SdfVariability variability = SdfVariabilityVarying;

    VtValue fallback;
    if (writeSparsely && !custom) {
        UsdAttribute attr = usdPrim.GetAttribute(attrName);
        if (defaultValue.IsEmpty()
            || (!attr.HasAuthoredValue() && attr.Get(&fallback) && fallback == defaultValue)) {
            return attr;
        }
    }

    UsdAttribute attr = usdPrim.CreateAttribute(attrName, typeName, custom, variability);
    if (attr && !defaultValue.IsEmpty()) {
        attr.Set(defaultValue);
    }

    return attr;
}

// Writes the property specified via the shaderProp from the depFn onto the
// usdPrim.
//
// Returns true if successful.  This will return false if it failed and populate
// reason with the reason for failure.
static bool _WriteProperty(
    const SdrShaderPropertyConstPtr& shaderProp,
    const MFnDependencyNode&         depFn,
    UsdPrim                          usdPrim,
    std::string*                     reason)
{
    const TfToken mayaAttrName(shaderProp->GetImplementationName());
    const TfToken usdAttrName(_ShaderAttrName(shaderProp->GetName()));
#if PXR_VERSION <= 2408
    const SdfValueTypeName sdfValueTypeName = shaderProp->GetTypeAsSdfType().first;
#else
    const SdfValueTypeName sdfValueTypeName = shaderProp->GetTypeAsSdfType().GetSdfType();
#endif

    MStatus     status;
    const MPlug attrPlug = depFn.findPlug(mayaAttrName.GetText(), &status);
    if (status == MS::kSuccess) {
        const VtValue value = _GetValue(attrPlug, sdfValueTypeName);
        if (!value.IsEmpty()) {
            _CreateAttr(usdPrim, usdAttrName, sdfValueTypeName, value);
        }
    }
    return true;
}

/* static */
void UsdMayaTranslatorUtil::ReadShaderAttributesFromUsdPrim(
    const UsdPrim&           usdPrim,
    const TfToken&           sdrShaderId,
    const MFnDependencyNode& depFn)
{
    SdrShaderNodeConstPtr shaderNode
        = SdrRegistry::GetInstance().GetShaderNodeByIdentifier(sdrShaderId);
    if (!shaderNode) {
        TF_RUNTIME_ERROR(
            "Could not find SdrShader %s for %s.",
            sdrShaderId.GetText(),
            usdPrim.GetPath().GetText());
        return;
    }

    std::string reason;
#if PXR_VERSION >= 2505
    for (const TfToken& inputName : shaderNode->GetShaderInputNames()) {
#else
    for (const TfToken& inputName : shaderNode->GetInputNames()) {
#endif
        if (SdrShaderPropertyConstPtr shaderProp = shaderNode->GetShaderInput(inputName)) {
            // We only read attributes that should have been written.
            if (_ShouldBeWrittenInUsd(shaderProp)) {
                if (!_ReadProperty(shaderProp, usdPrim, depFn, &reason)) {
                    TF_RUNTIME_ERROR(reason);
                }
            }
        } else {
            TF_RUNTIME_ERROR("Failed to get SdrProperty for input %s.", inputName.GetText());
        }
    }
}

/* static */
void UsdMayaTranslatorUtil::WriteShaderAttributesToUsdPrim(
    const MFnDependencyNode& depFn,
    const TfToken&           sdrShaderId,
    const UsdPrim&           usdPrim)
{
    SdrShaderNodeConstPtr shaderNode
        = SdrRegistry::GetInstance().GetShaderNodeByIdentifier(sdrShaderId);
    if (!shaderNode) {
        TF_RUNTIME_ERROR(
            "Could not find SdrShader %s for %s.",
            sdrShaderId.GetText(),
            usdPrim.GetPrim().GetPath().GetText());
        return;
    }

    UsdMayaTranslatorUtil::GetAPISchemaForAuthoring<UsdLuxShapingAPI>(usdPrim);
    UsdMayaTranslatorUtil::GetAPISchemaForAuthoring<UsdLuxShadowAPI>(usdPrim);
    std::string reason;
#if PXR_VERSION >= 2505
    for (const TfToken& inputName : shaderNode->GetShaderInputNames()) {
#else
    for (const TfToken& inputName : shaderNode->GetInputNames()) {
#endif
        if (SdrShaderPropertyConstPtr shaderProp = shaderNode->GetShaderInput(inputName)) {
            if (_ShouldBeWrittenInUsd(shaderProp)) {
                if (!_WriteProperty(shaderProp, depFn, usdPrim, &reason)) {
                    TF_RUNTIME_ERROR(reason);
                }
            }
        } else {
            TF_RUNTIME_ERROR("Failed to get SdrProperty for input %s.", inputName.GetText());
        }
    }
}

std::map<TfToken, TfToken>
UsdMayaTranslatorUtil::ComputeUsdAttributeToMayaAttributeNamesForShader(const TfToken& sdrShaderId)
{
    SdrShaderNodeConstPtr shaderNode
        = SdrRegistry::GetInstance().GetShaderNodeByIdentifier(sdrShaderId);
    if (!shaderNode) {
        TF_RUNTIME_ERROR("Could not find SdrShader %s.", sdrShaderId.GetText());
        return {};
    }

    std::map<TfToken, TfToken> usdAttrToMayaAttrNames;
#if PXR_VERSION >= 2505
    for (const TfToken& inputName : shaderNode->GetShaderInputNames()) {
#else
    for (const TfToken& inputName : shaderNode->GetInputNames()) {
#endif
        if (SdrShaderPropertyConstPtr shaderProp = shaderNode->GetShaderInput(inputName)) {
            if (_ShouldBeWrittenInUsd(shaderProp)) {
                const TfToken mayaAttrName(shaderProp->GetImplementationName());
                const TfToken usdAttrName(_ShaderAttrName(shaderProp->GetName()));
                usdAttrToMayaAttrNames[usdAttrName] = mayaAttrName;
            }
        } else {
            TF_RUNTIME_ERROR("Failed to get SdrProperty for input %s.", inputName.GetText());
        }
    }
    return usdAttrToMayaAttrNames;
}

bool UsdMayaTranslatorUtil::HasXformOps(const UsdPrim& usdPrim)
{
    // If it is not a prim, then it has no XformOps.
    if (!usdPrim)
        return false;

    // If it is not transformable, then it has no XformOps.
    if (!usdPrim.IsA<UsdGeomXformable>())
        return false;

    UsdGeomXformable xformable(usdPrim);
    bool             resetStack = false;
    return xformable.GetOrderedXformOps(&resetStack).size() > 0;
}

PXR_NAMESPACE_CLOSE_SCOPE
