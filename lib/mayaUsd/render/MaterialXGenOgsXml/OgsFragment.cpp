#include "OgsFragment.h"

#include <mayaUsd/render/MaterialXGenOgsXml/CombinedMaterialXVersion.h>
#include <mayaUsd/render/MaterialXGenOgsXml/GlslFragmentGenerator.h>
#include <mayaUsd/render/MaterialXGenOgsXml/GlslOcioNodeImpl.h>
#include <mayaUsd/render/MaterialXGenOgsXml/OgsXmlGenerator.h>

#include <MaterialXFormat/XmlIo.h>
#include <MaterialXGenShader/DefaultColorManagementSystem.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/Util.h>

#ifdef MATERIALX_BUILD_CROSS
#include <MaterialXCross/Cross.h>
#endif

#include <maya/MGlobal.h>
#include <maya/MString.h>

#include <cctype>
#include <iostream>
#include <unordered_map>

namespace MaterialXMaya {
namespace {

/// String option var controlling the environment method. Valid values are "none", "prefiltered" and
/// "fis". Default values are based on available light API:
///
///  API |  Default      | Options
///   V1 |  prefiltered  | none, prefiltered
///   V2 |  prefiltered  | none, prefiltered
///   V3 |  fis          | none, prefiltered, fis
const MString OPTVAR_ENVIRONMENT_METHOD = "MxMayaEnvironmentMethod";

/// Int option var to control the number of FIS samples. Larger values will slow down GLSL
/// rendering speed and can lead to TDR. Default is 64, expected range is 1 - 1024 by powers of
/// two. Has no effect unless fis mode is available and selected.
const MString OPTVAR_NUM_SAMPLES = "MxMayaEnvironmentSamples";

/// String option var to control the GGX albedo computations. Valid values are "polynomial" and
/// "montecarlo". The latter one has a performance impact on the rendering. Has no effect unless
/// fis mode is available and selected.
const MString OPTVAR_ALBEDO_METHOD = "MxMayaEnvironmentAlbedoMethod";

// Find the expected environment mode depending on Maya capabilities and optionVars:
mx::HwSpecularEnvironmentMethod _getEnvironmentOptions(int& numSamples, bool& isMonteCarlo)
{
    bool varExists = false;
    switch (mx::OgsXmlGenerator::useLightAPI()) {
    case 1:
    case 2: {
        // We default with prefilter but will respect "None" as a choice
        MString envMethod = MGlobal::optionVarStringValue(OPTVAR_ENVIRONMENT_METHOD, &varExists);
        if (varExists && envMethod == "none") {
            return mx::SPECULAR_ENVIRONMENT_NONE;
        } else {
            return mx::SPECULAR_ENVIRONMENT_PREFILTER;
        }
    } break;
    case 3: {
        // We default with fis
        MString envMethod = MGlobal::optionVarStringValue(OPTVAR_ENVIRONMENT_METHOD, &varExists);
        if (varExists) {
            if (envMethod == "none") {
                return mx::SPECULAR_ENVIRONMENT_NONE;
            } else if (envMethod == "prefiltered") {
                return mx::SPECULAR_ENVIRONMENT_PREFILTER;
            }
        }
        numSamples = MGlobal::optionVarIntValue(OPTVAR_NUM_SAMPLES, &varExists);
        if (!varExists) {
            numSamples = 64;
        }
        MString albedoMethod = MGlobal::optionVarStringValue(OPTVAR_ALBEDO_METHOD, &varExists);
        isMonteCarlo = (varExists && albedoMethod == "montecarlo");
        return mx::SPECULAR_ENVIRONMENT_FIS;
    } break;
    }
    return mx::SPECULAR_ENVIRONMENT_NONE;
}

// The base class for classes wrapping GLSL fragment generators for use during
// OgsFragment construction.
class GlslGeneratorWrapperBase
{
    GlslGeneratorWrapperBase() = delete;

protected:
    explicit GlslGeneratorWrapperBase(mx::ElementPtr element)
        : _element(element)
    {
        if (!_element)
            throw mx::Exception("No element specified");

        mx::TypedElementPtr typeElement = element->asA<mx::TypedElement>();
        if (typeElement && typeElement->getType() == mx::SURFACE_SHADER_TYPE_STRING) {
            _isSurface = true;
        } else if (element->isA<mx::Node>()) {
            mx::NodePtr outputNode = element->asA<mx::Node>();
            if (outputNode->getType() == mx::MATERIAL_TYPE_STRING) {
                std::vector<mx::NodePtr> shaderNodes
                    = mx::getShaderNodes(outputNode, mx::SURFACE_SHADER_TYPE_STRING);
                if (!shaderNodes.empty()) {
                    _element = *shaderNodes.begin();
                    _isSurface = true;
                }
            } else if (outputNode->getType() == mx::SURFACE_SHADER_TYPE_STRING) {
                _isSurface = true;
            }
        } else if (!element->asA<mx::Output>()) {
            throw mx::Exception("Invalid element to create fragment for " + element->getName());
        }
    }

protected:
    void setCommonOptions(
        mx::GenOptions&            genOptions,
        mx::GenContext&            context,
        const mx::ShaderGenerator& generator)
    {
        int  numSamples = 64;
        bool isMonteCarlo = false;
        genOptions.hwSpecularEnvironmentMethod = _getEnvironmentOptions(numSamples, isMonteCarlo);
        // FIS option has further sub-options to check:
        if (genOptions.hwSpecularEnvironmentMethod == mx::SPECULAR_ENVIRONMENT_FIS) {
            context.pushUserData(
                mx::HwSpecularEnvironmentSamples::name(),
                mx::HwSpecularEnvironmentSamples::create(numSamples));
            if (isMonteCarlo) {
                genOptions.hwDirectionalAlbedoMethod = mx::DIRECTIONAL_ALBEDO_MONTE_CARLO;
            }
        }

#if MX_COMBINED_VERSION >= 13805
        // MaterialX has a new implementation of transmission as refraction in version 1.38.5, but
        // does not work out of the box in Maya (probably because we only output a color).
        // We will deactivate until we have time to upgrade the MaterialX output to a Maya surface
        // struct where we can expose transmission values.
        genOptions.hwTransmissionRenderMethod = mx::TRANSMISSION_OPACITY;
#endif

        // Set to use no direct lighting
        if (mx::OgsXmlGenerator::useLightAPI() >= 2) {
            genOptions.hwMaxActiveLightSources = 0;
        } else {
            genOptions.hwMaxActiveLightSources = _isSurface ? 16 : 0;
        }

        // Maya images require a texture coordinates to be flipped in V.
        genOptions.fileTextureVerticalFlip = true;
        // Enabling hwTransparency to ensure a vec4 output with correct alpha value.
        genOptions.hwTransparency = true;

        // Maya viewport uses texture atlas for tile image so enabled
        // texture coordinate transform to go from original UDIM range to
        // normalized 0..1 range.
        genOptions.hwNormalizeUdimTexCoords = true;
    }

    mx::ElementPtr _element;

private:
    bool _isSurface = false;
};

// Knows how to create a temporary local GLSL fragment generator to generate
// GLSL fragment code during OgsFragment construction.
//
class LocalGlslGeneratorWrapper : public GlslGeneratorWrapperBase
{
public:
    LocalGlslGeneratorWrapper(mx::ElementPtr element, const mx::FileSearchPath& librarySearchPath)
        : GlslGeneratorWrapperBase(element)
        , _librarySearchPath(librarySearchPath)
    {
    }

    ~LocalGlslGeneratorWrapper() { }

    mx::ShaderPtr operator()(const std::string& baseFragmentName)
    {
        mx::ShaderGeneratorPtr generator = mx::GlslFragmentGenerator::create();
        mx::GenContext         genContext(generator);
        mx::GenOptions&        genOptions = genContext.getOptions();

        // Set up color management. We assume the target render space is linear
        // if not found in the document. Currently the default system has no other color space
        // targets.
        //
        if (mx::DefaultColorManagementSystemPtr colorManagementSystem
            = mx::DefaultColorManagementSystem::create(generator->getTarget())) {
            generator->setColorManagementSystem(colorManagementSystem);

            mx::DocumentPtr document = _element->getDocument();
            colorManagementSystem->loadLibrary(document);
            const std::string& documentColorSpace
                = document->getAttribute(mx::Element::COLOR_SPACE_ATTRIBUTE);

            static const std::string MATERIALX_LINEAR_WORKING_SPACE("lin_rec709");
            genOptions.targetColorSpaceOverride
                = documentColorSpace.empty() ? MATERIALX_LINEAR_WORKING_SPACE : documentColorSpace;
        }

        // Set up unit system. We assume default distance unit of 1 meter:
        if (mx::UnitSystemPtr unitSystem = mx::UnitSystem::create(generator->getTarget())) {
            generator->setUnitSystem(unitSystem);
            mx::UnitConverterRegistryPtr registry = mx::UnitConverterRegistry::create();
            mx::DocumentPtr              document = _element->getDocument();
            mx::UnitTypeDefPtr           distanceTypeDef = document->getUnitTypeDef("distance");
            registry->addUnitConverter(
                distanceTypeDef, mx::LinearUnitConverter::create(distanceTypeDef));
            mx::UnitTypeDefPtr angleTypeDef = document->getUnitTypeDef("angle");
            registry->addUnitConverter(angleTypeDef, mx::LinearUnitConverter::create(angleTypeDef));
            generator->getUnitSystem()->loadLibrary(document);
            generator->getUnitSystem()->setUnitConverterRegistry(registry);

            // Set target unit space
            genOptions.targetDistanceUnit = "meter";
        }

#if MATERIALX_MAJOR_VERSION == 1 && MATERIALX_MINOR_VERSION == 38 && MATERIALX_BUILD_VERSION == 3
        genContext.registerSourceCodeSearchPath(_librarySearchPath);
#else
        // Starting from MaterialX 1.38.4 at PR 877, we must remove the "libraries" part:
        mx::FileSearchPath libSearchPaths;
        for (const mx::FilePath& path : _librarySearchPath) {
            if (path.getBaseName() == "libraries") {
                libSearchPaths.append(path.getParentPath());
            } else {
                libSearchPaths.append(path);
            }
        }
        genContext.registerSourceCodeSearchPath(libSearchPaths);
#endif

        setCommonOptions(genOptions, genContext, *generator);

        // Every light ends up as a directional light once processed thru Maya:
        mx::DocumentPtr document = _element->getDocument();
        mx::NodeDefPtr  directionalLightShader = document->getNodeDef("ND_directional_light");
        mx::HwShaderGenerator::bindLightShader(*directionalLightShader, 1, genContext);

        return generator->generate(baseFragmentName, _element, genContext);
    }

    const mx::FileSearchPath& _librarySearchPath;
};

// Wraps an externally-provided GLSL fragment generator (such as the one
// created once for multiple tests by the test harness) to generate
// GLSL fragment code during OgsFragment construction.
//
class ExternalGlslGeneratorWrapper : public GlslGeneratorWrapperBase
{
public:
    ExternalGlslGeneratorWrapper(mx::ElementPtr element, mx::GenContext& genContext)
        : GlslGeneratorWrapperBase(element)
        , _genContext(genContext)
    {
    }

    ~ExternalGlslGeneratorWrapper() { }

    mx::ShaderPtr operator()(const std::string& baseFragmentName)
    {
        mx::ShaderGenerator& generator = _genContext.getShaderGenerator();
        mx::GenOptions&      genOptions = _genContext.getOptions();

        setCommonOptions(genOptions, _genContext, generator);

        return generator.generate(baseFragmentName, _element, _genContext);
    }

private:
    mx::GenContext& _genContext;
};

/// Generate the complete XML fragment source embedding both GLSL and HLSL code.
/// @return The unique name of the fragment
std::string generateFragment(
    std::string&       fragmentSource,
    const mx::Shader&  glslShader,
    const std::string& baseFragmentName)
{
    static const std::string FRAGMENT_NAME_TOKEN = "$fragmentName";

    {
        std::string hlslSource;
#ifdef MATERIALX_BUILD_CROSS
        try {
            // Cross-compile GLSL fragment code generated by MaterialX to HLSL
            //
            hlslSource = mx::Cross::glslToHlsl(
                glslShader.getSourceCode(mx::Stage::UNIFORMS),
                glslShader.getSourceCode(mx::Stage::PIXEL),
                baseFragmentName);
        } catch (std::exception& e) {
            std::cerr << "Failed to cross-compile GLSL fragment to HLSL: " << e.what() << "\n";
        }
#endif
        // Generate the XML wrapper for the fragment embedding both the GLSL
        // and HLSL code.
        // Supply a placeholder name token to be replaced with an actual unique
        // name later.
        //
        fragmentSource = mx::OgsXmlGenerator::generate(FRAGMENT_NAME_TOKEN, glslShader, hlslSource);
        if (fragmentSource.empty()) {
            throw mx::Exception("Generated fragment source is empty");
        }
    }

    // Strip out any '\r' characters.
    fragmentSource.erase(
        std::remove(fragmentSource.begin(), fragmentSource.end(), '\r'), fragmentSource.end());

    // Hash the generated fragment source and use it to generate a unique
    // fragment name to use for registration with Maya API that won't clash
    // with other fragments (possibly different versions of the same
    // MaterialX fragment).
    std::ostringstream nameStream;
    const size_t       sourceHash = std::hash<std::string> {}(fragmentSource);
    nameStream << baseFragmentName << "__" << std::hex << sourceHash
               << OgsFragment::getSpecularEnvKey();
    std::string fragmentName = nameStream.str();

    // Substitute the placeholder name token with the actual name.
    //
    const mx::StringMap substitutions { { FRAGMENT_NAME_TOKEN, fragmentName } };
    mx::tokenSubstitution(substitutions, fragmentSource);

    return fragmentName;
}

/// Generate a fragment graph linking Maya lights to the generated fragment:
/// @return The unique name of the fragment
std::string generateLightRig(
    std::string&       lightRigSource,
    const mx::Shader&  glslShader,
    const std::string& baseFragmentName)
{
    static const std::string FRAGMENT_NAME_TOKEN = "$fragmentName";
    static const std::string BASE_FRAGMENT_NAME_TOKEN = "$baseFragmentName";

    {
        // Supply a placeholder name token to be replaced with an actual unique
        // name later.
        //
        lightRigSource = mx::OgsXmlGenerator::generateLightRig(
            FRAGMENT_NAME_TOKEN, BASE_FRAGMENT_NAME_TOKEN, glslShader);
        if (lightRigSource.empty()) {
            throw mx::Exception("Generated light rig is empty");
        }
    }

    // Strip out any '\r' characters.
    lightRigSource.erase(
        std::remove(lightRigSource.begin(), lightRigSource.end(), '\r'), lightRigSource.end());

    std::ostringstream nameStream;
    const size_t       sourceHash = std::hash<std::string> {}(lightRigSource);
    nameStream << "Lit_" << baseFragmentName << "__" << std::hex << sourceHash;
    std::string fragmentName = nameStream.str();

    // Substitute the placeholder name token with the actual name.
    //
    const mx::StringMap substitutions { { FRAGMENT_NAME_TOKEN, fragmentName },
                                        { BASE_FRAGMENT_NAME_TOKEN, baseFragmentName } };
    mx::tokenSubstitution(substitutions, lightRigSource);

    return fragmentName;
}

// Need to find back renamed parameters and map them to their original names:
std::string getOriginalName(const std::string& parameterName, const mx::NodeDefPtr& shaderNodeDef)
{
    if (!shaderNodeDef) {
        return parameterName;
    }

    if (shaderNodeDef->getInput(parameterName)) {
        return parameterName;
    }

    // Here we try to find back the original name in case it conflicted with an identifier (like
    // "mix") A one level cache will help reduce churn:
    static std::string           gLastNodeDef;
    static std::set<std::string> gParameters;

    if (gLastNodeDef != shaderNodeDef->getName()) {
        gParameters.clear();
        for (auto const& input : shaderNodeDef->getActiveInputs()) {
            gParameters.insert(input->getName());
        }
        gLastNodeDef = shaderNodeDef->getName();
    }

    auto it = gParameters.lower_bound(parameterName);

    if (it != gParameters.end() && *it == parameterName) {
        return *it;
    }

    // Need to check the element just before to see if it is a substring of parameterName:
    if (it == gParameters.begin()) {
        return {};
    }

    --it;
    if (parameterName.rfind(*it, 0) != 0) {
        // Does not begin with that name
        return {};
    }

    // Check that the remainder of the string only digits:
    for (size_t i = it->length(); i < parameterName.length(); ++i) {
        if (!std::isdigit(parameterName.at(i))) {
            // Not a digit:
            return {};
        }
    }

    // Found a valid parameter name that was renamed by shadergen:
    return *it;
}

template <typename T>
static bool
parameterDiffersFrom(const mx::NodePtr& node, const std::string& paramName, T const& paramValue)
{
    if (const auto input = node->getInput(paramName)) {
        // A connected value is always considered to differ:
        if (!input->getNodeName().empty() || !input->getInterfaceName().empty()
            || !input->getOutputString().empty()) {
            return true;
        }

        // Check the value itself:
        if (input->hasValue()) {
            const auto value = input->getValue();
            return value->asA<T>() != paramValue;
        }
    }
    // Assume default value is equal to paramValue.
    return false;
}

bool isTransparentStandardSurface(const mx::NodePtr& surfaceNode)
{
    // See https://autodesk.github.io/standard-surface/
    // and implementation in MaterialX libraries/bxdf/standard_surface.mtlx
    if (parameterDiffersFrom(surfaceNode, "transmission", 0.0f)
        || parameterDiffersFrom(surfaceNode, "opacity", mx::Color3(1.0f, 1.0f, 1.0f))) {
        return true;
    }

    return false;
}

bool isTransparentOpenPBRSurface(const mx::NodePtr& surfaceNode)
{
    // See https://academysoftwarefoundation.github.io/OpenPBR/
    // and the provided implementation
    if (parameterDiffersFrom(surfaceNode, "transmission_weight", 0.0f)
        || parameterDiffersFrom(surfaceNode, "geometry_opacity", 1.0f)) {
        return true;
    }

    return false;
}

bool isTransparentUsdPreviewSurface(const mx::NodePtr& surfaceNode)
{
    // See https://openusd.org/release/spec_usdpreviewsurface.html
    // and implementation in MaterialX libraries/bxdf/usd_preview_surface.mtlx

    // Non-zero opacityThreshold (or connected) triggers masked mode:
    if (parameterDiffersFrom(surfaceNode, "opacityThreshold", 0.0f)) {
        return true;
    }

    // Opacity less than 1.0 (or connected) triggers transparent mode:
    if (parameterDiffersFrom(surfaceNode, "opacity", 1.0f)) {
        return true;
    }

    return false;
}

bool isTransparentGlTfPBRSurface(const mx::NodePtr& surfaceNode)
{
    // See https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#alpha-coverage
    // And implementation in MaterialX /libraries/bxdf/gltf_pbr.mtlx

    int alphaMode = 0; // Opaque
    if (auto const modeInput = surfaceNode->getInput("alpha_mode")) {
        if (!modeInput->getNodeName().empty() || !modeInput->getInterfaceName().empty()
            || !modeInput->getOutputString().empty()) {
            // A connected alpha_mode is non-standard, but will be considered to overall imply
            // blend.
            alphaMode = 2; // Blend
        } else if (modeInput->hasValue()) {
            const auto value = modeInput->getValue()->asA<int>();
            if (value >= 0 && value <= 2) {
                alphaMode = value;
            }
        }
    }

    bool retVal = false;
    if (alphaMode == 1) // Mask
    {
        if (parameterDiffersFrom(surfaceNode, "alpha_cutoff", 1.0f)
            && parameterDiffersFrom(surfaceNode, "alpha", 1.0f)) {
            retVal = true;
        }
    } else if (alphaMode == 2) // Blend
    {
        if (parameterDiffersFrom(surfaceNode, "alpha", 1.0f)) {
            retVal = true;
        }
    }

    if (parameterDiffersFrom(surfaceNode, "transmission", 0.0f)) {
        return true;
    }

    return retVal;
}

bool isTranparentMayaSurface(const mx::NodePtr& surfaceNode)
{
    if (parameterDiffersFrom(surfaceNode, "transparency", mx::Color3(0.0f, 0.0f, 0.0f))) {
        return true;
    }
    return false;
}

enum class TransparencyResult
{
    kTransparent,
    kOpaque,
    kUnknown
};
TransparencyResult isTransparentSurfaceNode(const mx::NodePtr& surfaceNode)
{
    static const auto sKnownSurfaces
        = std::unordered_map<std::string, std::function<bool(const mx::NodePtr&)>> {
              { "ND_standard_surface_surfaceshader", isTransparentStandardSurface },
              { "ND_open_pbr_surface_surfaceshader", isTransparentOpenPBRSurface },
              { "ND_UsdPreviewSurface_surfaceshader", isTransparentUsdPreviewSurface },
              { "ND_gltf_pbr_surfaceshader", isTransparentGlTfPBRSurface },
              { "MayaND_lambert_surfaceshader", isTranparentMayaSurface },
              { "MayaND_phong_surfaceshader", isTranparentMayaSurface },
              { "MayaND_blinn_surfaceshader", isTranparentMayaSurface },
              { "ND_convert_color3_surfaceshader", [](const mx::NodePtr&) { return false; } },
              { "ND_convert_color4_surfaceshader", [](const mx::NodePtr&) { return true; } },
              { "ND_convert_float_surfaceshader", [](const mx::NodePtr&) { return false; } },
              { "ND_convert_vector2_surfaceshader", [](const mx::NodePtr&) { return false; } },
              { "ND_convert_vector3_surfaceshader", [](const mx::NodePtr&) { return false; } },
              { "ND_convert_vector4_surfaceshader", [](const mx::NodePtr&) { return true; } },
              { "ND_convert_integer_surfaceshader", [](const mx::NodePtr&) { return false; } },
              { "ND_convert_boolean_surfaceshader", [](const mx::NodePtr&) { return false; } },
          };

    if (const auto nodeDef = surfaceNode->getNodeDef()) {
        auto it = sKnownSurfaces.find(nodeDef->getName());
        if (it != sKnownSurfaces.end()) {
            return it->second(surfaceNode) ? TransparencyResult::kTransparent
                                           : TransparencyResult::kOpaque;
        }
    }

    return TransparencyResult::kUnknown;
}

} // anonymous namespace

OgsFragment::OgsFragment(mx::ElementPtr element, const mx::FileSearchPath& librarySearchPath)
    : OgsFragment(element, LocalGlslGeneratorWrapper(element, librarySearchPath))
{
}

OgsFragment::OgsFragment(mx::ElementPtr element, mx::GenContext& genContext)
    : OgsFragment(element, ExternalGlslGeneratorWrapper(element, genContext))
{
}

template <typename GLSL_GENERATOR_WRAPPER>
OgsFragment::OgsFragment(mx::ElementPtr element, GLSL_GENERATOR_WRAPPER&& glslGeneratorWrapper)
    : _element(element)
{
    if (!_element)
        throw mx::Exception("No element specified");

    // The non-unique name of the fragment.
    // Must match the name of the root function of the fragment.
    const std::string baseFragmentName = mx::createValidName(_element->getNamePath());

    // Generate the GLSL version of the fragment.
    //
    _glslShader = glslGeneratorWrapper(baseFragmentName);
    if (!_glslShader) {
        throw mx::Exception("Failed to generate GLSL fragment code");
    }

    // Generate the complete XML fragment source embedding both GLSL and HLSL
    // code.
    _fragmentName = generateFragment(_fragmentSource, *_glslShader, baseFragmentName);

    const mx::ShaderGraph& graph = _glslShader->getGraph();
    bool                   lighting
        = graph.hasClassification(
              mx::ShaderNode::Classification::SHADER | mx::ShaderNode::Classification::SURFACE)
        || graph.hasClassification(mx::ShaderNode::Classification::BSDF);
    if (lighting && mx::OgsXmlGenerator::useLightAPI() < 2) {
        _lightRigName = generateLightRig(_lightRigSource, *_glslShader, _fragmentName);
    }

    // Get the root surfaceshader name and parameters:
    auto surfaceNode = std::dynamic_pointer_cast<mx::Node>(_element);
    if (surfaceNode && surfaceNode->getCategory() == "surfacematerial") {
        const auto surfaceInput = surfaceNode->getInput("surfaceshader");
        if (surfaceInput) {
            surfaceNode = surfaceInput->getConnectedNode();
        }
    }
    std::string    surfaceNodeName;
    mx::NodeDefPtr surfaceNodeDef;
    if (surfaceNode) {
        surfaceNodeName = surfaceNode->getName();
        surfaceNodeDef = surfaceNode->getNodeDef();
    }

    // Extract the input fragment parameter names along with their
    // associated element paths to allow for value binding.
    //
    const mx::ShaderStage& pixelShader = _glslShader->getStage(mx::Stage::PIXEL);
    for (const auto& uniformBlock : pixelShader.getUniformBlocks()) {
        const mx::VariableBlock& uniforms = *uniformBlock.second;

        // Skip light uniforms
        if (uniforms.getName() == mx::HW::LIGHT_DATA) {
            continue;
        }

        for (size_t i = 0; i < uniforms.size(); ++i) {
            const mx::ShaderPort* const port = uniforms[i];
            if (!port->getNode()) {
#if MX_COMBINED_VERSION < 13900
                if (port->getType()->getSemantic() == mx::TypeDesc::SEMANTIC_FILENAME) {
#else
                if (port->getType().getSemantic() == mx::TypeDesc::SEMANTIC_FILENAME) {
#endif
                    // Might be an embedded texture. Retrieve the filename.
                    std::string textureName
                        = mx::OgsXmlGenerator::samplerToTextureName(port->getVariable());
                    if (!textureName.empty() && !port->getPath().empty()) {
                        _embeddedTextures[textureName] = port->getPath();
                    }
                }
                continue;
            }
            std::string path = port->getPath();
            // MaterialX 1.38.10 will return empty path for the root shader.
            // work around that. We know there is an associated node. No help
            // from the Node* in the port though since this will be the N0
            // ShaderGraph.
            const std::string& variableName = port->getVariable();
            if (path.empty()) {
                // Assume it is the surface shader:
                const std::string& originalName = getOriginalName(variableName, surfaceNodeDef);
                if (originalName.empty()) {
                    continue;
                }
                path.reserve(surfaceNodeName.size() + 1 + variableName.size());
                path += surfaceNodeName;
                path += "/";
                path += originalName;
            }
            if (!path.empty()) {
#if MX_COMBINED_VERSION < 13900
                if (port->getType()->getSemantic() == mx::TypeDesc::SEMANTIC_FILENAME) {
#else
                if (port->getType().getSemantic() == mx::TypeDesc::SEMANTIC_FILENAME) {
#endif
                    std::string textureName
                        = mx::OgsXmlGenerator::samplerToTextureName(variableName);
                    if (!textureName.empty()) {
                        _pathInputMap[path] = textureName;
                        continue;
                    }
                }
                _pathInputMap[path] = variableName;
            }
        }
    }
}

OgsFragment::~OgsFragment() { }

const std::string& OgsFragment::getFragmentName() const { return _fragmentName; }

const std::string& OgsFragment::getFragmentSource() const { return _fragmentSource; }

const std::string& OgsFragment::getLightRigName() const { return _lightRigName; }

const std::string& OgsFragment::getLightRigSource() const { return _lightRigSource; }

const mx::StringMap& OgsFragment::getPathInputMap() const { return _pathInputMap; }

const mx::StringMap& OgsFragment::getEmbeddedTextureMap() const { return _embeddedTextures; }

bool OgsFragment::isElementAShader() const
{
    mx::TypedElementPtr typeElement = _element ? _element->asA<mx::TypedElement>() : nullptr;
    return typeElement && typeElement->getType() == mx::SURFACE_SHADER_TYPE_STRING;
}

bool OgsFragment::isTransparent() const { return isTransparentSurface(_element); }

/// Implements transparency detection for well known shaders and then
/// delegates to MaterialX for complex ones.
bool OgsFragment::isTransparentSurface(const mx::ElementPtr& element)
{
    if (mx::NodePtr node = element->asA<mx::Node>()) {
        // Handle material nodes.
        if (node->getCategory() == mx::SURFACE_MATERIAL_NODE_STRING) {
            auto shaderNodes = getShaderNodes(node);
            if (!shaderNodes.empty()) {
                node = shaderNodes[0];
            }
        }

        const auto transparencyResult = isTransparentSurfaceNode(node);
        if (transparencyResult == TransparencyResult::kTransparent) {
            return true;
        }
        if (transparencyResult == TransparencyResult::kOpaque) {
            return false;
        }
        // Can also return kUnknown.
    }

    // Delegate to MaterialX for all other situations:
    return MaterialX::isTransparentSurface(element, mx::GlslShaderGenerator::TARGET);
}

mx::ImageSamplingProperties
OgsFragment::getImageSamplingProperties(const std::string& fileParameterName) const
{
    mx::ImageSamplingProperties samplingProperties;
    if (_glslShader && _glslShader->hasStage(mx::Stage::PIXEL)) {
        mx::ShaderStage&   stage = _glslShader->getStage(mx::Stage::PIXEL);
        mx::VariableBlock& block = stage.getUniformBlock(mx::HW::PUBLIC_UNIFORMS);
        samplingProperties.setProperties(fileParameterName, block);
    }
    return samplingProperties;
}

std::string OgsFragment::getMatrix4Name(const std::string& matrix3Name)
{
    return matrix3Name + mx::GlslFragmentGenerator::MATRIX3_TO_MATRIX4_POSTFIX;
}

std::string OgsFragment::getSpecularEnvKey()
{
    std::string retVal;
    int         numSamples = 64;
    bool        isMonteCarlo = false;
    switch (_getEnvironmentOptions(numSamples, isMonteCarlo)) {
    case mx::SPECULAR_ENVIRONMENT_FIS:
        retVal += "F" + std::to_string(numSamples) + (isMonteCarlo ? "MC" : "P");
        break;
    case mx::SPECULAR_ENVIRONMENT_PREFILTER: retVal = "P"; break;
    default: retVal = "N"; break;
    }

    return retVal;
}

std::string OgsFragment::registerOCIOFragment(const std::string& fragName)
{
    // Delegate to the GlslOcioNodeImpl:
    return mx::GlslOcioNodeImpl::registerOCIOFragment(fragName);
}

mx::DocumentPtr OgsFragment::getOCIOLibrary()
{
    // Delegate to the GlslOcioNodeImpl:
    return mx::GlslOcioNodeImpl::getOCIOLibrary();
}

} // namespace MaterialXMaya
