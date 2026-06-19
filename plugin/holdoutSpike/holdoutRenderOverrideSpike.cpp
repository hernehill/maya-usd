//-
// holdoutRenderOverrideSpike.cpp
//
// PURPOSE (throwaway spike):
//   Answer one question and nothing else:
//   "In a custom MRenderOverride pass, can we rasterize a primitive that
//    writes DEPTH while masking ALL color (raw glColorMask), and have that
//    depth survive into a later scene render that shares the same depth
//    target -- thereby occluding scene geometry without contributing color?"
//
//   This is the holdout effect in miniature. If it works, an invisible quad
//   punches a hole through whatever is on screen, revealing the clear color
//   behind it. If OGS cancels the depth write when color is masked (the wall
//   hit in the OGSFX / MStateManager-blend experiments), no hole appears.
//
// WHY THIS IS NEW:
//   Previous attempts masked color via OGS blend state (kNoChannels) on a
//   render item, where color-off is coupled to depth-off. Here we use raw
//   OpenGL glColorMask inside a MUserRenderOperation, which never touches an
//   MShaderInstance or an OGS blend state -- the textbook depth-prepass path.
//   GL-only by design (Linux / OpenGL Core Profile).
//
// HOW TO READ THE RESULT (after loading the plugin and choosing the
// "Holdout Depth Spike" renderer in the viewport's Renderer menu, with any
// object filling the view):
//   * A solid TEAL rectangle punched through the scene  -> SUCCESS.
//         depth-write-with-color-masked works in a custom pass. Route alive.
//   * Scene fully visible, no rectangle                 -> depth write was
//         cancelled. Same wall, confirmed from the raw-GL side. Route dead.
//   * A MAGENTA rectangle (the quad's own color)        -> color mask was
//         ignored entirely (diagnostic; unexpected).
//+

#include <maya/MFnPlugin.h>
#include <maya/MGlobal.h>
#include <maya/MString.h>
#include <maya/MStatus.h>
#include <maya/MViewport2Renderer.h>
#include <maya/MDrawContext.h>

// Modern GL entry points, loaded exactly like the rest of maya-usd does it.
#include <pxr/imaging/garch/glApi.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

const MString kOverrideName("holdoutSpike");
const MString kOverrideUIName("Holdout Depth Spike");

// Attributeless full-ish quad in NDC. gl_VertexID 0..3 -> triangle strip.
// z = -1.0 (NDC near) => window depth 0.0 (nearest possible), so ANY scene
// geometry behind it is occluded regardless of where the user's objects sit.
const char* kVertexSrc =
    "#version 330\n"
    "void main() {\n"
    "    float x = (gl_VertexID == 1 || gl_VertexID == 3) ? 0.5 : -0.5;\n"
    "    float y = (gl_VertexID >= 2) ? 0.5 : -0.5;\n"
    "    gl_Position = vec4(x, y, -1.0, 1.0);\n"
    "}\n";

// Bright magenta: only visible if color masking is (unexpectedly) ignored.
const char* kFragmentSrc =
    "#version 330\n"
    "out vec4 fragColor;\n"
    "void main() { fragColor = vec4(1.0, 0.0, 1.0, 1.0); }\n";

GLuint compileShader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = { 0 };
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        MGlobal::displayError(MString("[holdoutSpike] shader compile failed: ") + log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

//============================================================================
// Operation 1: the depth-only pass (the actual test)
//============================================================================
class HoldoutDepthOp : public MHWRender::MUserRenderOperation
{
public:
    HoldoutDepthOp(const MString& name)
        : MHWRender::MUserRenderOperation(name)
    {
    }

    ~HoldoutDepthOp() override = default;

    MStatus execute(const MHWRender::MDrawContext& /*drawContext*/) override
    {
        if (!ensureGLResources())
            return MS::kFailure;

        // --- save the GL state we are about to disturb -----------------
        GLint     prevProgram = 0, prevVao = 0, prevDepthFunc = GL_LESS;
        GLboolean prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
        GLboolean prevDepthMask = GL_TRUE;
        GLboolean prevColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
        glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);

        // --- (a) establish the shared buffers --------------------------
        // This op runs first, so it clears. Color + depth must be writable
        // for the clear to land.
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(0.10f, 0.15f, 0.20f, 1.0f); // teal -> the "hole" color
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // --- (b) THE TEST: write depth, mask every color channel -------
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);                       // write unconditionally
        glDepthMask(GL_TRUE);                          // depth write ON
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE); // color write OFF
        glUseProgram(mProgram);
        glBindVertexArray(mVao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glUseProgram(0);

        // --- restore state so the beauty scene render starts clean -----
        glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
        glDepthMask(prevDepthMask);
        glDepthFunc(prevDepthFunc);
        if (prevDepthTest)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        glUseProgram(static_cast<GLuint>(prevProgram));
        glBindVertexArray(static_cast<GLuint>(prevVao));

        return MS::kSuccess;
    }

private:
    // Built lazily on first execute (live GL context). Intentionally not
    // deleted -- this is a spike; the objects are reclaimed on plugin unload.
    bool ensureGLResources()
    {
        if (mInitialized)
            return mProgram != 0;
        mInitialized = true;

        GarchGLApiLoad();

        GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexSrc);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentSrc);
        if (!vs || !fs)
            return false;

        mProgram = glCreateProgram();
        glAttachShader(mProgram, vs);
        glAttachShader(mProgram, fs);
        glLinkProgram(mProgram);
        glDeleteShader(vs);
        glDeleteShader(fs);

        GLint linked = GL_FALSE;
        glGetProgramiv(mProgram, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[1024] = { 0 };
            glGetProgramInfoLog(mProgram, sizeof(log), nullptr, log);
            MGlobal::displayError(MString("[holdoutSpike] program link failed: ") + log);
            glDeleteProgram(mProgram);
            mProgram = 0;
            return false;
        }

        glGenVertexArrays(1, &mVao); // empty VAO; required by core profile
        return true;
    }

    bool   mInitialized = false;
    GLuint mProgram = 0;
    GLuint mVao = 0;
};

//============================================================================
// Operation 2: the beauty scene render -- draws everything, does NOT clear,
// so it depth-tests against the depth our user op just stamped.
//============================================================================
class BeautySceneRender : public MHWRender::MSceneRender
{
public:
    BeautySceneRender(const MString& name)
        : MHWRender::MSceneRender(name)
    {
    }

    MHWRender::MClearOperation& clearOperation() override
    {
        mClearOperation.setMask(
            static_cast<unsigned int>(MHWRender::MClearOperation::kClearNone));
        return mClearOperation;
    }
};

//============================================================================
// The override: [ depth-only user op ] -> [ beauty scene ] -> [ present ]
//============================================================================
class HoldoutSpikeOverride : public MHWRender::MRenderOverride
{
public:
    HoldoutSpikeOverride(const MString& name)
        : MHWRender::MRenderOverride(name)
    {
        mOps[0] = new HoldoutDepthOp("holdoutSpike_DepthOnly");
        mOps[1] = new BeautySceneRender("holdoutSpike_Beauty");
        mOps[2] = new MHWRender::MPresentTarget("holdoutSpike_Present");
    }

    ~HoldoutSpikeOverride() override
    {
        for (auto*& op : mOps) {
            delete op;
            op = nullptr;
        }
    }

    MHWRender::DrawAPI supportedDrawAPIs() const override
    {
        // GL only: the depth pass uses raw OpenGL. (Linux / Core Profile.)
        return static_cast<MHWRender::DrawAPI>(
            MHWRender::kOpenGL | MHWRender::kOpenGLCoreProfile);
    }

    MString uiName() const override { return kOverrideUIName; }

    bool startOperationIterator() override
    {
        mCurrentOp = 0;
        return true;
    }

    MHWRender::MRenderOperation* renderOperation() override
    {
        if (mCurrentOp >= 0 && mCurrentOp < kNumOps)
            return mOps[mCurrentOp];
        return nullptr;
    }

    bool nextRenderOperation() override
    {
        ++mCurrentOp;
        return mCurrentOp < kNumOps;
    }

private:
    static const int                  kNumOps = 3;
    MHWRender::MRenderOperation*       mOps[kNumOps] = { nullptr, nullptr, nullptr };
    int                               mCurrentOp = 0;
};

HoldoutSpikeOverride* gOverride = nullptr;

} // anonymous namespace

//============================================================================
// Plugin entry points
//============================================================================
MStatus initializePlugin(MObject obj)
{
    MFnPlugin plugin(obj, "HoldoutSpike", "0.1", "Any");

    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    if (!renderer) {
        MGlobal::displayError("[holdoutSpike] VP2 renderer not available.");
        return MS::kFailure;
    }

    if (!gOverride) {
        gOverride = new HoldoutSpikeOverride(kOverrideName);
        renderer->registerOverride(gOverride);
    }

    MGlobal::displayInfo(
        "[holdoutSpike] Registered. Pick 'Holdout Depth Spike' in the "
        "viewport Renderer menu.");
    return MS::kSuccess;
}

MStatus uninitializePlugin(MObject obj)
{
    MFnPlugin plugin(obj);

    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    if (renderer && gOverride) {
        renderer->deregisterOverride(gOverride);
        delete gOverride;
        gOverride = nullptr;
    }
    return MS::kSuccess;
}