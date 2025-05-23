//
// Copyright 2016 Pixar
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
#include "baseImportCommand.h"

#include <mayaUsd/fileio/jobs/jobArgs.h>
#include <mayaUsd/fileio/jobs/readJob.h>
#include <mayaUsd/undo/OpUndoItemMuting.h>
#include <mayaUsd/utils/util.h>

#include <pxr/pxr.h>
#include <pxr/usd/ar/resolver.h>

#include <maya/MArgList.h>
#include <maya/MSelectionList.h>
#include <maya/MStatus.h>
#include <maya/MSyntax.h>

#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace MAYAUSD_NS_DEF {

/* static */
MSyntax MayaUSDImportCommand::createSyntax()
{
    MSyntax syntax;

    // These flags correspond to entries in
    // UsdMayaJobImportArgs::GetGuideDictionary.
    syntax.addFlag(
        kShadingModeFlag,
        UsdMayaJobImportArgsTokens->shadingMode.GetText(),
        MSyntax::kString,
        MSyntax::kString);
    syntax.makeFlagMultiUse(kShadingModeFlag);
    syntax.addFlag(
        kPreferredMaterialFlag,
        UsdMayaJobImportArgsTokens->preferredMaterial.GetText(),
        MSyntax::kString);
    syntax.addFlag(
        kImportInstancesFlag,
        UsdMayaJobImportArgsTokens->importInstances.GetText(),
        MSyntax::kString);
    syntax.addFlag(
        kImportUSDZTexturesFlag,
        UsdMayaJobImportArgsTokens->importUSDZTextures.GetText(),
        MSyntax::kBoolean);
    syntax.addFlag(
        kImportUSDZTexturesFilePathFlag,
        UsdMayaJobImportArgsTokens->importUSDZTexturesFilePath.GetText(),
        MSyntax::kString);
    syntax.addFlag(
        kImportRelativeTexturesFlag,
        UsdMayaJobImportArgsTokens->importRelativeTextures.GetText(),
        MSyntax::kString);
    syntax.addFlag(
        kImportUpAxisFlag, UsdMayaJobImportArgsTokens->upAxis.GetText(), MSyntax::kBoolean);
    syntax.addFlag(kImportUnitFlag, UsdMayaJobImportArgsTokens->unit.GetText(), MSyntax::kBoolean);
    syntax.addFlag(
        kImportAxisAndUnitMethodFlag,
        UsdMayaJobImportArgsTokens->axisAndUnitMethod.GetText(),
        MSyntax::kString);
    syntax.addFlag(kMetadataFlag, UsdMayaJobImportArgsTokens->metadata.GetText(), MSyntax::kString);
    syntax.makeFlagMultiUse(kMetadataFlag);
    syntax.addFlag(
        kApiSchemaFlag, UsdMayaJobImportArgsTokens->apiSchema.GetText(), MSyntax::kString);
    syntax.makeFlagMultiUse(kApiSchemaFlag);
    syntax.addFlag(
        kJobContextFlag, UsdMayaJobImportArgsTokens->jobContext.GetText(), MSyntax::kString);
    syntax.makeFlagMultiUse(kJobContextFlag);
    syntax.addFlag(
        kExcludePrimvarFlag,
        UsdMayaJobImportArgsTokens->excludePrimvar.GetText(),
        MSyntax::kString);
    syntax.makeFlagMultiUse(kExcludePrimvarFlag);
    syntax.addFlag(
        kExcludePrimvarNamespaceFlag,
        UsdMayaJobImportArgsTokens->excludePrimvarNamespace.GetText(),
        MSyntax::kString);
    syntax.makeFlagMultiUse(kExcludePrimvarNamespaceFlag);
    syntax.addFlag(
        kUseAsAnimationCacheFlag,
        UsdMayaJobImportArgsTokens->useAsAnimationCache.GetText(),
        MSyntax::kBoolean);

    // Import chasers
    syntax.addFlag(
        kImportChaserFlag, UsdMayaJobImportArgsTokens->chaser.GetText(), MSyntax::kString);
    syntax.makeFlagMultiUse(kImportChaserFlag);

    syntax.addFlag(
        kImportChaserArgsFlag,
        UsdMayaJobImportArgsTokens->chaserArgs.GetText(),
        MSyntax::kString,
        MSyntax::kString,
        MSyntax::kString);
    syntax.makeFlagMultiUse(kImportChaserArgsFlag);

    syntax.addFlag(
        kRemapUVSetsToFlag,
        UsdMayaJobImportArgsTokens->remapUVSetsTo.GetText(),
        MSyntax::kString,
        MSyntax::kString);
    syntax.makeFlagMultiUse(kRemapUVSetsToFlag);

    syntax.addFlag(
        kApplyEulerFilterFlag,
        UsdMayaJobImportArgsTokens->applyEulerFilter.GetText(),
        MSyntax::kBoolean);

    // These are additional flags under our control.
    syntax.addFlag(kFileFlag, kFileFlagLong, MSyntax::kString);
    syntax.addFlag(kParentFlag, kParentFlagLong, MSyntax::kString);
    syntax.addFlag(kReadAnimDataFlag, kReadAnimDataFlagLong, MSyntax::kBoolean);
    syntax.addFlag(kFrameRangeFlag, kFrameRangeFlagLong, MSyntax::kDouble, MSyntax::kDouble);
    syntax.addFlag(kPrimPathFlag, kPrimPathFlagLong, MSyntax::kString);
    syntax.addFlag(kRootVariantFlag, kRootVariantFlagLong, MSyntax::kString, MSyntax::kString);
    syntax.makeFlagMultiUse(kRootVariantFlag);
    syntax.addFlag(
        kPrimVariantFlag,
        kPrimVariantFlagLong,
        MSyntax::kString,
        MSyntax::kString,
        MSyntax::kString);
    syntax.makeFlagMultiUse(kPrimVariantFlag);

    syntax.addFlag(kVerboseFlag, kVerboseFlagLong, MSyntax::kNoArg);

    syntax.enableQuery(false);
    syntax.enableEdit(false);

    return syntax;
}

/* static */
void* MayaUSDImportCommand::creator() { return new MayaUSDImportCommand(); }

/* virtual */
std::unique_ptr<UsdMaya_ReadJob> MayaUSDImportCommand::initializeReadJob(
    const MayaUsd::ImportData&  data,
    const UsdMayaJobImportArgs& args)
{
    return std::unique_ptr<UsdMaya_ReadJob>(new UsdMaya_ReadJob(data, args));
}

/* virtual */
MStatus MayaUSDImportCommand::doIt(const MArgList& args)
{
    // The import process has its own undo/redo recording.
    // See: UsdMaya_ReadJob::Undo() and Redo().
    OpUndoItemMuting undoInfoMuting;

    MStatus status;

    MArgDatabase argData(syntax(), args, &status);

    // Check that all flags were valid
    if (status != MS::kSuccess) {
        return status;
    }

    // Get dictionary values.
    const VtDictionary userArgs = UsdMayaUtil::GetDictionaryFromArgDatabase(
        argData, UsdMayaJobImportArgs::GetGuideDictionary());

    std::string mFileName;
    if (argData.isFlagSet(kFileFlag)) {
        // Get the value
        MString tmpVal;
        argData.getFlagArgument(kFileFlag, 0, tmpVal);
        mFileName = UsdMayaUtil::convert(tmpVal);

        // Use the usd resolver for validation (but save the unresolved)
        if (ArGetResolver().Resolve(mFileName).empty()
            && !SdfLayer::IsAnonymousLayerIdentifier(mFileName)) {
            TF_RUNTIME_ERROR(
                "File '%s' does not exist, or could not be resolved. "
                "Exiting.",
                mFileName.c_str());
            return MS::kFailure;
        }

        TF_STATUS("Importing '%s'", mFileName.c_str());
    }

    if (mFileName.empty()) {
        TF_RUNTIME_ERROR("Empty file specified. Exiting.");
        return MS::kFailure;
    }

    std::string mPrimPath;
    if (argData.isFlagSet(kPrimPathFlag)) {
        // Get the value
        MString tmpVal;
        argData.getFlagArgument(kPrimPathFlag, 0, tmpVal);
        mPrimPath = UsdMayaUtil::convert(tmpVal);
    }

    // Add root prim variant (variantSet, variant).  Multi-use
    SdfVariantSelectionMap rootVariants;
    unsigned int           nbFlags = argData.numberOfFlagUses(kRootVariantFlag);
    for (unsigned int i = 0; i < nbFlags; ++i) {
        MArgList tmpArgList;
        status = argData.getFlagArgumentList(kRootVariantFlag, i, tmpArgList);
        // Get the value
        MString tmpKey = tmpArgList.asString(0, &status);
        MString tmpVal = tmpArgList.asString(1, &status);
        rootVariants.emplace(tmpKey.asChar(), tmpVal.asChar());
    }

    // Add prim variant (prim path, variant set, variant selection). Multi-use
    ImportData::PrimVariantSelections primVariants;
    nbFlags = argData.numberOfFlagUses(kPrimVariantFlag);
    for (unsigned int i = 0; i < nbFlags; ++i) {
        MArgList tmpArgList;
        status = argData.getFlagArgumentList(kPrimVariantFlag, i, tmpArgList);
        PXR_NS::SdfPath primPath { tmpArgList.asString(0, &status).asChar() };
        std::string     variantName { tmpArgList.asString(1, &status).asChar() };
        std::string     variantSel { tmpArgList.asString(2, &status).asChar() };
        primVariants[primPath].emplace(variantName, variantSel);
    }

    bool readAnimData = false;
    if (argData.isFlagSet(kReadAnimDataFlag)) {
        argData.getFlagArgument(kReadAnimDataFlag, 0, readAnimData);
    }

    GfInterval timeInterval;
    if (readAnimData) {
        if (argData.isFlagSet(kFrameRangeFlag)) {
            double startTime = 1.0;
            double endTime = 1.0;
            argData.getFlagArgument(kFrameRangeFlag, 0, startTime);
            argData.getFlagArgument(kFrameRangeFlag, 1, endTime);
            if (endTime < startTime) {
                std::swap(startTime, endTime);
            }

            timeInterval = GfInterval(startTime, endTime);
        } else {
            timeInterval = GfInterval::GetFullInterval();
        }
    } else {
        timeInterval = GfInterval();
    }

    UsdMayaJobImportArgs jobArgs = UsdMayaJobImportArgs::CreateFromDictionary(
        userArgs,
        /* importWithProxyShapes = */ false,
        timeInterval);

    MayaUsd::ImportData importData(mFileName);
    importData.setRootVariantSelections(std::move(rootVariants));
    importData.setPrimVariantSelections(std::move(primVariants));
    importData.setRootPrimPath(mPrimPath);

    _readJob = initializeReadJob(importData, jobArgs);

    // Add optional command params
    if (argData.isFlagSet(kParentFlag)) {
        // Get the value
        MString tmpVal;
        argData.getFlagArgument(kParentFlag, 0, tmpVal);

        if (tmpVal.length()) {
            MSelectionList selList;
            selList.add(tmpVal);
            MDagPath dagPath;
            status = selList.getDagPath(0, dagPath);
            if (status != MS::kSuccess) {
                TF_RUNTIME_ERROR("Invalid path '%s' for -parent.", tmpVal.asChar());
                return MS::kFailure;
            }
            _readJob->SetMayaRootDagPath(dagPath);
        }
    }

    // Execute the command
    std::vector<MDagPath> addedDagPaths;
    bool                  success = _readJob->Read(&addedDagPaths);
    if (success) {
        TF_FOR_ALL(iter, addedDagPaths) { appendToResult(iter->fullPathName()); }
    }
    return (success) ? MS::kSuccess : MS::kFailure;
}

/* virtual */
MStatus MayaUSDImportCommand::redoIt()
{
    if (!_readJob) {
        return MS::kFailure;
    }

    bool success = _readJob->Redo();

    return (success) ? MS::kSuccess : MS::kFailure;
}

/* virtual */
MStatus MayaUSDImportCommand::undoIt()
{
    if (!_readJob) {
        return MS::kFailure;
    }

    bool success = _readJob->Undo();

    return (success) ? MS::kSuccess : MS::kFailure;
}

} // namespace MAYAUSD_NS_DEF
