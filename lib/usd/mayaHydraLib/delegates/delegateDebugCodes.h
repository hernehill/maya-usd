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
#ifndef MAYAHYDRALIB_DELEGATE_DEBUG_CODES_H
#define MAYAHYDRALIB_DELEGATE_DEBUG_CODES_H

#include <pxr/base/tf/debug.h>
#include <pxr/pxr.h>

PXR_NAMESPACE_OPEN_SCOPE

// clang-format off
TF_DEBUG_CODES(
    MAYAHYDRALIB_DELEGATE_GET,
    MAYAHYDRALIB_DELEGATE_GET_CULL_STYLE,
    MAYAHYDRALIB_DELEGATE_GET_CURVE_TOPOLOGY,
    MAYAHYDRALIB_DELEGATE_GET_DISPLAY_STYLE,
    MAYAHYDRALIB_DELEGATE_GET_DOUBLE_SIDED,
    MAYAHYDRALIB_DELEGATE_GET_EXTENT,
    MAYAHYDRALIB_DELEGATE_GET_INSTANCER_ID,
    MAYAHYDRALIB_DELEGATE_GET_INSTANCE_INDICES,
    MAYAHYDRALIB_DELEGATE_GET_LIGHT_PARAM_VALUE,
    MAYAHYDRALIB_DELEGATE_GET_MATERIAL_ID,
    MAYAHYDRALIB_DELEGATE_GET_MATERIAL_RESOURCE,
    MAYAHYDRALIB_DELEGATE_GET_MESH_TOPOLOGY,
    MAYAHYDRALIB_DELEGATE_GET_PRIMVAR_DESCRIPTORS,
    MAYAHYDRALIB_DELEGATE_GET_RENDER_TAG,
    MAYAHYDRALIB_DELEGATE_GET_SUBDIV_TAGS,
    MAYAHYDRALIB_DELEGATE_GET_TRANSFORM,
    MAYAHYDRALIB_DELEGATE_GET_VISIBLE,
    MAYAHYDRALIB_DELEGATE_INSERTDAG,
    MAYAHYDRALIB_DELEGATE_IS_ENABLED,
    MAYAHYDRALIB_DELEGATE_RECREATE_ADAPTER,
    MAYAHYDRALIB_DELEGATE_REGISTRY,
    MAYAHYDRALIB_DELEGATE_SAMPLE_PRIMVAR,
    MAYAHYDRALIB_DELEGATE_SAMPLE_TRANSFORM,
    MAYAHYDRALIB_DELEGATE_SELECTION);
// clang-format on

// Debug codes for Hydra API that was deprecated with USD 20.11.
// These are declared in a separate block to avoid using a preprocessor
// directive inside the TF_DEBUG_CODES() macro invocation, which breaks
// compilation on Windows.
#if PXR_VERSION < 2011
// clang-format off
TF_DEBUG_CODES(
    MAYAHYDRALIB_DELEGATE_GET_TEXTURE_RESOURCE,
    MAYAHYDRALIB_DELEGATE_GET_TEXTURE_RESOURCE_ID);
// clang-format on
#endif // PXR_VERSION < 2011

PXR_NAMESPACE_CLOSE_SCOPE

#endif // MAYAHYDRALIB_DELEGATE_DEBUG_CODES_H