//
// Copyright 2019 Luma Pictures
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
#include <mayaHydraLib/adapters/adapterDebugCodes.h>
#include <mayaHydraLib/adapters/adapterRegistry.h>
#include <mayaHydraLib/adapters/lightAdapter.h>
#include <mayaHydraLib/adapters/mayaAttrs.h>
#include <mayaHydraLib/adapters/tokens.h>
#include <mayaHydraLib/utils.h>

#include <pxr/base/tf/type.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usdLux/tokens.h>

#include <maya/MPlugArray.h>

PXR_NAMESPACE_OPEN_SCOPE

/**
 * \brief MayaHydraAiSkyDomeLightAdapter is used to handle the translation from an Arnold skydome light to hydra.
 */
class MayaHydraAiSkyDomeLightAdapter : public MayaHydraLightAdapter
{
public:
    MayaHydraAiSkyDomeLightAdapter(MayaHydraDelegateCtx* delegate, const MDagPath& dag)
        : MayaHydraLightAdapter(delegate, dag)
    {
    }

    const TfToken& LightType() const override { return HdPrimTypeTokens->domeLight; }

    VtValue GetLightParamValue(const TfToken& paramName) override
    {
        MStatus           status;
        MFnDependencyNode light(GetNode(), &status);
        if (ARCH_UNLIKELY(!status)) {
            return {};
        }

        // We are not using precomputed attributes here, because we don't have
        // a guarantee that mtoa will be loaded before mayaHydra.
        if (paramName == HdLightTokens->color) {
            const auto plug = light.findPlug("color", true);
            MPlugArray conns;
            plug.connectedTo(conns, true, false);
            return VtValue(
                conns.length() > 0
                    ? GfVec3f(1.0f, 1.0f, 1.0f)
                    : GfVec3f(
                        plug.child(0).asFloat(), plug.child(1).asFloat(), plug.child(2).asFloat()));
        } else if (paramName == HdLightTokens->intensity) {
            return VtValue(light.findPlug("intensity", true).asFloat());
        } else if (paramName == HdLightTokens->diffuse) {
            MPlug aiDiffuse = light.findPlug("aiDiffuse", true, &status);
            if (status == MS::kSuccess) {
                return VtValue(aiDiffuse.asFloat());
            }
        } else if (paramName == HdLightTokens->specular) {
            MPlug aiSpecular = light.findPlug("aiSpecular", true, &status);
            if (status == MS::kSuccess) {
                return VtValue(aiSpecular.asFloat());
            }
        } else if (paramName == HdLightTokens->exposure) {
            return VtValue(light.findPlug("aiExposure", true).asFloat());
        } else if (paramName == HdLightTokens->normalize) {
            return VtValue(light.findPlug("aiNormalize", true).asBool());
        } else if (paramName == HdLightTokens->textureFormat) {
            const auto format = light.findPlug("format", true).asShort();
            // mirrored_ball : 0
            // angular : 1
            // latlong : 2
            if (format == 0) {
                return VtValue(UsdLuxTokens->mirroredBall);
            } else if (format == 2) {
                return VtValue(UsdLuxTokens->latlong);
            } else {
                return VtValue(UsdLuxTokens->automatic);
            }
        } else if (paramName == HdLightTokens->textureFile) {
            MPlugArray conns;
            light.findPlug("color", true).connectedTo(conns, true, false);
            if (conns.length() < 1) {
                // Be aware that dome lights in HdStorm always need a texture to work correctly,
                // the color is not used if no texture is present. This is in USD 22.11.
                return VtValue(SdfAssetPath());
            }
            MFnDependencyNode file(conns[0].node(), &status);
            if (ARCH_UNLIKELY(
                    !status || (file.typeName() != MayaHydraAdapterTokens->file.GetText()))) {
                // Be aware that dome lights in HdStorm always need a texture to work correctly,
                // the color is not used if no texture is present. This is in USD 22.11.
                return VtValue(SdfAssetPath());
            }

            const char* fileTextureName
                = file.findPlug(MayaAttrs::file::fileTextureName, true).asString().asChar();
            // SdfAssetPath requires both "path" "resolvedPath"
            return VtValue(SdfAssetPath(fileTextureName, fileTextureName));

        } else if (paramName == HdLightTokens->enableColorTemperature) {
            return VtValue(false);
        }
        return {};
    }
};

TF_REGISTRY_FUNCTION(TfType)
{
    TfType::Define<MayaHydraAiSkyDomeLightAdapter, TfType::Bases<MayaHydraLightAdapter>>();
}

TF_REGISTRY_FUNCTION_WITH_TAG(MayaHydraAdapterRegistry, domeLight)
{
    MayaHydraAdapterRegistry::RegisterLightAdapter(
        TfToken("aiSkyDomeLight"),
        [](MayaHydraDelegateCtx* delegate, const MDagPath& dag) -> MayaHydraLightAdapterPtr {
            return MayaHydraLightAdapterPtr(new MayaHydraAiSkyDomeLightAdapter(delegate, dag));
        });
}

PXR_NAMESPACE_CLOSE_SCOPE