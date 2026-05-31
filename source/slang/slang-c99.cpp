// slang-c99.cpp - Implementation of the simple C99 API for Slang

#include "../../include/slang-c99.h"

#include "slang.h"
#include "slang-com-ptr.h"
#include "slang-linkable.h"
#include "slang-module.h"
#include "slang-ast-support-types.h"
#include "slang-ast-decl.h"
#include "../../source/compiler-core/slang-artifact-associated.h"

#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>
#include <limits>

using namespace Slang;

static std::string normalizeSpecializationExpr(const char* expr)
{
    if (!expr)
        return "";

    std::string s(expr);
    auto isSpace = [](unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };

    size_t begin = 0;
    while (begin < s.size() && isSpace((unsigned char)s[begin]))
        ++begin;

    size_t end = s.size();
    while (end > begin && isSpace((unsigned char)s[end - 1]))
        --end;

    std::string trimmed = s.substr(begin, end - begin);
    if (trimmed == "true")
        return "1";
    if (trimmed == "false")
        return "0";
    return s;
}

//
// Internal structures
//

// Forward declaration
class CallbackFileSystem;

struct SlangcGlobalSessionImpl
{
    ComPtr<slang::IGlobalSession> globalSession;
};

struct SlangcCompilerImpl
{
    // Either owns globalSession (if ownsGlobalSession==true) or borrows from external
    ComPtr<slang::IGlobalSession> globalSession;
    bool ownsGlobalSession = true;

    ComPtr<slang::ISession> session;

    SlangcTarget target = SLANGC_TARGET_SPIRV;
    std::vector<std::string> searchPaths;

    std::vector<ComPtr<slang::IModule>> modules;

    SlangcLoadFileCallback fileLoader = nullptr;
    void* fileLoaderUserData = nullptr;

    std::string errorMessages;

    bool needsSessionRecreate = true;

    SlangcFragmentOutputResolverCallback fragmentOutputResolver = nullptr;
    void* fragmentOutputResolverUserData = nullptr;
    SlangcBindlessArraySizeResolverCallback bindlessArraySizeResolver = nullptr;
    void* bindlessArraySizeResolverUserData = nullptr;

    // Per-instance file system (NOT static - each compiler instance needs its own)
    CallbackFileSystem* callbackFs = nullptr;

    ~SlangcCompilerImpl()
    {
        delete callbackFs;
    }

    void ensureSession();
    void appendError(const char* msg);
    void appendDiagnostics(slang::IBlob* blob);
};

struct ResourceInfoStorage
{
    std::string name;
    std::string typeName;
    int set;
    int binding;
    int bindlessIndex;
    int bindingCount;
    SlangcResourceObjectType objectType;
    int isArray;
    int arraySize;
    SlangcResourceAccess access;
};

struct EntryPointInfo
{
    slang::IModule* module;
    std::string name;
    SlangcStage stage;
};

struct FragmentOutputResolverWrapper
{
    SlangcFragmentOutputResolverCallback userCallback;
    void* userCallbackData;
};

struct BindlessArraySizeResolverWrapper
{
    SlangcBindlessArraySizeResolverCallback userCallback;
    void* userCallbackData;
};

struct SlangcProgramImpl
{
    SlangcCompilerImpl* compiler;  // Back-reference to compiler

    // Pre-link state
    std::vector<slang::IModule*> modules;
    std::vector<EntryPointInfo> entryPoints;

    // Specialization state (pre-link)
    // Positional args (traditional API)
    std::vector<std::string> specializationExprs;
    std::vector<slang::TypeReflection*> specializationTypes;
    std::vector<bool> specializationIsType;
    // Named args (new API) - param name -> type expression
    std::unordered_map<std::string, std::string> namedSpecializationArgs;

    // Cached composed program for param count query
    ComPtr<slang::IComponentType> composedProgram;
    bool needsRecompose = true;

    std::unique_ptr<FragmentOutputResolverWrapper> fragmentOutputResolverWrapper;
    std::unique_ptr<BindlessArraySizeResolverWrapper> bindlessArraySizeResolverWrapper;

    // Post-link state
    ComPtr<slang::IComponentType> linkedProgram;
    ComPtr<slang::IBlob> codeBlob;
    bool isLinked = false;

    std::vector<ResourceInfoStorage> resources;
    std::vector<ResourceInfoStorage> bindlessResources;
    std::vector<std::string> entryPointNames;
    std::vector<SlangcStage> entryPointStages;

    std::string errorMessages;

    void appendError(const char* msg);
    void appendDiagnostics(slang::IBlob* blob);
};

struct SlangcBlobImpl
{
    ComPtr<slang::IBlob> blob;
};

// Convert from internal SlangResourceAccess to C99 SlangcResourceAccess
static SlangcResourceAccess toC99Access(SlangResourceAccess access)
{
    switch (access)
    {
    case SLANG_RESOURCE_ACCESS_NONE:
    case SLANG_RESOURCE_ACCESS_READ:
        return SLANGC_ACCESS_READ;
    case SLANG_RESOURCE_ACCESS_WRITE:
    case SLANG_RESOURCE_ACCESS_APPEND:
        return SLANGC_ACCESS_WRITE;
    case SLANG_RESOURCE_ACCESS_READ_WRITE:
    case SLANG_RESOURCE_ACCESS_RASTER_ORDERED:
    case SLANG_RESOURCE_ACCESS_CONSUME:
    case SLANG_RESOURCE_ACCESS_FEEDBACK:
    default:
        return SLANGC_ACCESS_READ_WRITE;
    }
}

static SlangcResourceObjectType toC99ObjectType(slang::SlangBindlessResourceType resourceType)
{
    switch (resourceType)
    {
    case slang::SLANG_BINDLESS_RESOURCE_TYPE_COMBINED_TEXTURE_SAMPLER:
        return SLANGC_RESOURCE_OBJECT_COMBINED_TEXTURE_SAMPLER;
    case slang::SLANG_BINDLESS_RESOURCE_TYPE_SAMPLER:
        return SLANGC_RESOURCE_OBJECT_SAMPLER;
    case slang::SLANG_BINDLESS_RESOURCE_TYPE_STORAGE_IMAGE:
        return SLANGC_RESOURCE_OBJECT_STORAGE_IMAGE;
    case slang::SLANG_BINDLESS_RESOURCE_TYPE_SAMPLED_IMAGE:
        return SLANGC_RESOURCE_OBJECT_SAMPLED_IMAGE;
    case slang::SLANG_BINDLESS_RESOURCE_TYPE_STORAGE_BUFFER:
        return SLANGC_RESOURCE_OBJECT_STORAGE_BUFFER;
    case slang::SLANG_BINDLESS_RESOURCE_TYPE_UNIFORM_BUFFER:
        return SLANGC_RESOURCE_OBJECT_UNIFORM_BUFFER;
    default:
        return SLANGC_RESOURCE_OBJECT_UNKNOWN;
    }
}

static int toC99ArraySize(size_t arraySize)
{
    if (arraySize == 0)
        return 0;

    if (arraySize == SLANG_UNBOUNDED_SIZE || arraySize == SLANG_UNKNOWN_SIZE ||
        arraySize > size_t(std::numeric_limits<int>::max()))
    {
        return -1;
    }

    return int(arraySize);
}

static int bindlessArraySizeResolverWrapperCallback(
    const char* resourceName,
    slang::SlangBindlessResourceType resourceType,
    void* userData)
{
    auto* wrapper = static_cast<BindlessArraySizeResolverWrapper*>(userData);
    if (!wrapper || !wrapper->userCallback)
        return -1;
    return wrapper->userCallback(
        resourceName,
        toC99ObjectType(resourceType),
        wrapper->userCallbackData);
}

static void clearResourceInfo(SlangcResourceInfo* outInfo)
{
    outInfo->name = nullptr;
    outInfo->typeName = nullptr;
    outInfo->set = -1;
    outInfo->binding = -1;
    outInfo->bindlessIndex = -1;
    outInfo->bindingCount = 0;
    outInfo->objectType = SLANGC_RESOURCE_OBJECT_UNKNOWN;
    outInfo->isArray = 0;
    outInfo->arraySize = 0;
    outInfo->access = SLANGC_ACCESS_READ;
}

static void copyResourceInfo(const ResourceInfoStorage& info, SlangcResourceInfo* outInfo)
{
    outInfo->name = info.name.c_str();
    outInfo->typeName = info.typeName.c_str();
    outInfo->set = info.set;
    outInfo->binding = info.binding;
    outInfo->bindlessIndex = info.bindlessIndex;
    outInfo->bindingCount = info.bindingCount;
    outInfo->objectType = info.objectType;
    outInfo->isArray = info.isArray;
    outInfo->arraySize = info.arraySize;
    outInfo->access = info.access;
}

static SlangcResourceObjectType classifyResourceObjectType(slang::TypeReflection* type)
{
    if (!type)
        return SLANGC_RESOURCE_OBJECT_UNKNOWN;

    while (type->isArray())
        type = type->getElementType();

    if (!type)
        return SLANGC_RESOURCE_OBJECT_UNKNOWN;

    auto kind = type->getKind();
    if (kind == slang::TypeReflection::Kind::SamplerState)
        return SLANGC_RESOURCE_OBJECT_SAMPLER;

    if (kind == slang::TypeReflection::Kind::ConstantBuffer ||
        kind == slang::TypeReflection::Kind::ParameterBlock)
    {
        return SLANGC_RESOURCE_OBJECT_UNIFORM_BUFFER;
    }

    if (kind == slang::TypeReflection::Kind::ShaderStorageBuffer)
        return SLANGC_RESOURCE_OBJECT_STORAGE_BUFFER;

    if (kind != slang::TypeReflection::Kind::Resource &&
        kind != slang::TypeReflection::Kind::TextureBuffer)
    {
        return SLANGC_RESOURCE_OBJECT_UNKNOWN;
    }

    auto shape = type->getResourceShape();
    if (shape & SLANG_TEXTURE_COMBINED_FLAG)
        return SLANGC_RESOURCE_OBJECT_COMBINED_TEXTURE_SAMPLER;

    switch (shape & SLANG_RESOURCE_BASE_SHAPE_MASK)
    {
    case SLANG_ACCELERATION_STRUCTURE:
        return SLANGC_RESOURCE_OBJECT_ACCELERATION_STRUCTURE;
    case SLANG_STRUCTURED_BUFFER:
    case SLANG_BYTE_ADDRESS_BUFFER:
        return SLANGC_RESOURCE_OBJECT_STORAGE_BUFFER;
    default:
        break;
    }

    auto access = type->getResourceAccess();
    if (access == SLANG_RESOURCE_ACCESS_READ)
        return SLANGC_RESOURCE_OBJECT_SAMPLED_IMAGE;
    if (access == SLANG_RESOURCE_ACCESS_NONE && kind == slang::TypeReflection::Kind::Resource)
        return SLANGC_RESOURCE_OBJECT_SAMPLED_IMAGE;
    return SLANGC_RESOURCE_OBJECT_STORAGE_IMAGE;
}

static const char* defaultTypeNameForObjectType(SlangcResourceObjectType objectType)
{
    switch (objectType)
    {
    case SLANGC_RESOURCE_OBJECT_COMBINED_TEXTURE_SAMPLER:
        return "CombinedTextureSampler";
    case SLANGC_RESOURCE_OBJECT_SAMPLER:
        return "SamplerState";
    case SLANGC_RESOURCE_OBJECT_STORAGE_IMAGE:
        return "Texture";
    case SLANGC_RESOURCE_OBJECT_SAMPLED_IMAGE:
        return "Texture";
    case SLANGC_RESOURCE_OBJECT_STORAGE_BUFFER:
        return "StorageBuffer";
    case SLANGC_RESOURCE_OBJECT_UNIFORM_BUFFER:
        return "ConstantBuffer";
    case SLANGC_RESOURCE_OBJECT_ACCELERATION_STRUCTURE:
        return "RaytracingAccelerationStructure";
    default:
        return "Resource";
    }
}

static int combineArraySize(int lhs, int rhs)
{
    if (lhs == -1 || rhs == -1)
        return -1;
    if (lhs == 0)
        return rhs;
    if (rhs == 0)
        return lhs;

    long long combinedSize = static_cast<long long>(lhs) * static_cast<long long>(rhs);
    if (combinedSize > std::numeric_limits<int>::max())
        return -1;
    return int(combinedSize);
}

static int fragmentOutputResolverWrapperCallback(const char* outputName, void* userData)
{
    auto* wrapper = static_cast<FragmentOutputResolverWrapper*>(userData);
    if (!wrapper || !wrapper->userCallback)
        return -1;

    return wrapper->userCallback(outputName, wrapper->userCallbackData);
}

//
// Custom file system wrapper
//

class CallbackFileSystem : public ISlangFileSystem
{
public:
    SlangcLoadFileCallback callback;
    void* userData;

    CallbackFileSystem(SlangcLoadFileCallback cb, void* ud)
        : callback(cb), userData(ud) {}

    virtual ~CallbackFileSystem() = default;

    // ISlangUnknown
    SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(SlangUUID const& uuid, void** outObject) override
    {
        if (uuid == ISlangUnknown::getTypeGuid() ||
            uuid == ISlangCastable::getTypeGuid() ||
            uuid == ISlangFileSystem::getTypeGuid())
        {
            *outObject = this;
            return SLANG_OK;
        }
        return SLANG_E_NO_INTERFACE;
    }

    SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override { return 1; }
    SLANG_NO_THROW uint32_t SLANG_MCALL release() override { return 1; }

    // ISlangCastable
    SLANG_NO_THROW void* SLANG_MCALL castAs(const SlangUUID& guid) override
    {
        if (guid == ISlangUnknown::getTypeGuid() ||
            guid == ISlangCastable::getTypeGuid() ||
            guid == ISlangFileSystem::getTypeGuid())
        {
            return this;
        }
        return nullptr;
    }

    // ISlangFileSystem
    SLANG_NO_THROW SlangResult SLANG_MCALL loadFile(
        const char* path,
        ISlangBlob** outBlob) override
    {
        if (!callback)
            return SLANG_E_NOT_AVAILABLE;

        size_t size = 0;
        const char* contents = callback(path, &size, userData);
        if (!contents)
            return SLANG_E_NOT_FOUND;

        // Create a blob with copied data
        *outBlob = slang_createBlob(contents, size);
        return SLANG_OK;
    }
};

//
// SlangcCompilerImpl methods
//

void SlangcCompilerImpl::ensureSession()
{
    if (!needsSessionRecreate && session)
        return;

    // Create global session if needed
    if (!globalSession)
    {
        if (SLANG_FAILED(slang::createGlobalSession(globalSession.writeRef())))
        {
            appendError("Failed to create global session");
            return;
        }
    }

    // Map our target enum to Slang's
    SlangCompileTarget slangTarget;
    SlangProfileID profile;
    switch (target)
    {
    case SLANGC_TARGET_SPIRV:
        slangTarget = SLANG_SPIRV;
        profile = globalSession->findProfile("spirv_1_5");
        break;
    case SLANGC_TARGET_SPIRV_ASM:
        slangTarget = SLANG_SPIRV_ASM;
        profile = globalSession->findProfile("spirv_1_5");
        break;
    case SLANGC_TARGET_GLSL:
        slangTarget = SLANG_GLSL;
        profile = globalSession->findProfile("glsl_450");
        break;
    case SLANGC_TARGET_HLSL:
        slangTarget = SLANG_HLSL;
        profile = globalSession->findProfile("sm_6_0");
        break;
    default:
        slangTarget = SLANG_SPIRV;
        profile = globalSession->findProfile("spirv_1_5");
        break;
    }

    // Build session desc
    slang::SessionDesc sessionDesc = {};
    slang::TargetDesc targetDesc = {};
    targetDesc.format = slangTarget;
    targetDesc.profile = profile;

    sessionDesc.targets = &targetDesc;
    std::vector<slang::CompilerOptionEntry> compilerOptions;

    auto addIntOption = [&](slang::CompilerOptionName name, int value) {
        slang::CompilerOptionEntry entry;
        entry.name = name;
        entry.value.kind = slang::CompilerOptionValueKind::Int;
        entry.value.intValue0 = value;
        compilerOptions.push_back(entry);
    };

    addIntOption(slang::CompilerOptionName::BindlessSpaceIndex,  0);
    addIntOption(slang::CompilerOptionName::LanguageVersion,     SLANG_LANGUAGE_VERSION_2026);
    addIntOption(slang::CompilerOptionName::DebugInformation,    SLANG_DEBUG_INFO_LEVEL_STANDARD);
    addIntOption(slang::CompilerOptionName::GLSLForceScalarLayout, 1);
    addIntOption(slang::CompilerOptionName::MatrixLayoutRow,     1);
    addIntOption(slang::CompilerOptionName::EnableRichDiagnostics,     1);
    addIntOption(slang::CompilerOptionName::EnableMachineReadableDiagnostics,     1);
    addIntOption(slang::CompilerOptionName::VulkanUseEntryPointName,     1);

    sessionDesc.compilerOptionEntries = compilerOptions.data();
    sessionDesc.compilerOptionEntryCount = static_cast<uint32_t>(compilerOptions.size());
    sessionDesc.targetCount = 1;

    // Convert search paths
    std::vector<const char*> searchPathPtrs;
    for (const auto& path : searchPaths)
        searchPathPtrs.push_back(path.c_str());

    sessionDesc.searchPaths = searchPathPtrs.data();
    sessionDesc.searchPathCount = (SlangInt)searchPathPtrs.size();

    // Set file system if callback provided
    if (fileLoader)
    {
        delete callbackFs;
        callbackFs = new CallbackFileSystem(fileLoader, fileLoaderUserData);
        sessionDesc.fileSystem = callbackFs;
    }

    // Create session
    if (SLANG_FAILED(globalSession->createSession(sessionDesc, session.writeRef())))
    {
        appendError("Failed to create session");
        return;
    }

    needsSessionRecreate = false;
}

void SlangcCompilerImpl::appendError(const char* msg)
{
    if (!errorMessages.empty())
        errorMessages += "\n";
    errorMessages += msg;
}

void SlangcCompilerImpl::appendDiagnostics(slang::IBlob* blob)
{
    if (blob && blob->getBufferSize() > 0)
    {
        if (!errorMessages.empty())
            errorMessages += "\n";
        errorMessages.append((const char*)blob->getBufferPointer(), blob->getBufferSize());
    }
}

void SlangcProgramImpl::appendError(const char* msg)
{
    if (!errorMessages.empty())
        errorMessages += "\n";
    errorMessages += msg;
}

void SlangcProgramImpl::appendDiagnostics(slang::IBlob* blob)
{
    if (blob && blob->getBufferSize() > 0)
    {
        if (!errorMessages.empty())
            errorMessages += "\n";
        errorMessages.append((const char*)blob->getBufferPointer(), blob->getBufferSize());
    }
}

//
// Stage conversion helpers
//

static SlangStage toSlangStage(SlangcStage stage)
{
    switch (stage)
    {
    case SLANGC_STAGE_VERTEX: return SLANG_STAGE_VERTEX;
    case SLANGC_STAGE_FRAGMENT: return SLANG_STAGE_FRAGMENT;
    case SLANGC_STAGE_COMPUTE: return SLANG_STAGE_COMPUTE;
    case SLANGC_STAGE_GEOMETRY: return SLANG_STAGE_GEOMETRY;
    case SLANGC_STAGE_HULL: return SLANG_STAGE_HULL;
    case SLANGC_STAGE_DOMAIN: return SLANG_STAGE_DOMAIN;
    case SLANGC_STAGE_RAYGEN: return SLANG_STAGE_RAY_GENERATION;
    case SLANGC_STAGE_INTERSECTION: return SLANG_STAGE_INTERSECTION;
    case SLANGC_STAGE_ANYHIT: return SLANG_STAGE_ANY_HIT;
    case SLANGC_STAGE_CLOSESTHIT: return SLANG_STAGE_CLOSEST_HIT;
    case SLANGC_STAGE_MISS: return SLANG_STAGE_MISS;
    case SLANGC_STAGE_CALLABLE: return SLANG_STAGE_CALLABLE;
    case SLANGC_STAGE_MESH: return SLANG_STAGE_MESH;
    case SLANGC_STAGE_AMPLIFICATION: return SLANG_STAGE_AMPLIFICATION;
    default: return SLANG_STAGE_NONE;
    }
}

//
// Helper function for lazy composition
//

static bool ensureComposedProgram(SlangcProgramImpl* impl)
{
    if (!impl->needsRecompose && impl->composedProgram)
        return true;

    if (!impl->compiler || !impl->compiler->session)
        return false;

    if (impl->entryPoints.empty())
        return false;

    // Build component list: modules + entry points
    std::vector<slang::IComponentType*> components;

    // Add all modules
    for (auto mod : impl->modules)
        components.push_back(mod);

    // Find and add entry points
    ComPtr<slang::IBlob> diagnostics;
    std::vector<ComPtr<slang::IEntryPoint>> entryPointRefs;  // Keep refs alive
    for (auto& epInfo : impl->entryPoints)
    {
        ComPtr<slang::IEntryPoint> entryPoint;
        if (SLANG_FAILED(epInfo.module->findAndCheckEntryPoint(
            epInfo.name.c_str(),
            toSlangStage(epInfo.stage),
            entryPoint.writeRef(),
            diagnostics.writeRef())))
        {
            impl->appendDiagnostics(diagnostics);
            return false;
        }
        impl->appendDiagnostics(diagnostics);
        entryPointRefs.push_back(entryPoint);
        components.push_back(entryPoint.get());
    }

    // Create composite component
    if (SLANG_FAILED(impl->compiler->session->createCompositeComponentType(
        components.data(),
        (SlangInt)components.size(),
        impl->composedProgram.writeRef(),
        diagnostics.writeRef())))
    {
        impl->appendDiagnostics(diagnostics);
        return false;
    }
    impl->appendDiagnostics(diagnostics);

    impl->needsRecompose = false;
    return true;
}

//
// C API Implementation
//

extern "C" {

/*
 * Global session lifecycle
 */

SLANGC_API SlangcGlobalSession slangc_createGlobalSession(void)
{
    auto impl = new SlangcGlobalSessionImpl();
    if (SLANG_FAILED(slang::createGlobalSession(impl->globalSession.writeRef())))
    {
        delete impl;
        return nullptr;
    }
    return impl;
}

SLANGC_API void slangc_destroyGlobalSession(SlangcGlobalSession globalSession)
{
    delete static_cast<SlangcGlobalSessionImpl*>(globalSession);
}

/*
 * Compiler lifecycle
 */

SLANGC_API SlangcCompiler slangc_createCompiler(void)
{
    auto impl = new SlangcCompilerImpl();
    impl->ownsGlobalSession = true;
    return impl;
}

SLANGC_API SlangcCompiler slangc_createCompilerWithGlobalSession(SlangcGlobalSession globalSession)
{
    if (!globalSession)
        return nullptr;

    auto gsImpl = static_cast<SlangcGlobalSessionImpl*>(globalSession);
    auto impl = new SlangcCompilerImpl();
    impl->globalSession = gsImpl->globalSession;
    impl->ownsGlobalSession = false;
    return impl;
}

SLANGC_API void slangc_destroyCompiler(SlangcCompiler compiler)
{
    delete static_cast<SlangcCompilerImpl*>(compiler);
}

SLANGC_API void slangc_setTarget(SlangcCompiler compiler, SlangcTarget target)
{
    auto impl = static_cast<SlangcCompilerImpl*>(compiler);
    if (impl->target != target)
    {
        impl->target = target;
        impl->needsSessionRecreate = true;
        impl->modules.clear();  // Modules are session-specific
    }
}

SLANGC_API void slangc_setFileLoader(
    SlangcCompiler compiler,
    SlangcLoadFileCallback callback,
    void* userData)
{
    auto impl = static_cast<SlangcCompilerImpl*>(compiler);
    impl->fileLoader = callback;
    impl->fileLoaderUserData = userData;
    impl->needsSessionRecreate = true;
    impl->modules.clear();
}

SLANGC_API void slangc_addSearchPath(SlangcCompiler compiler, const char* path)
{
    auto impl = static_cast<SlangcCompilerImpl*>(compiler);
    impl->searchPaths.push_back(path);
    impl->needsSessionRecreate = true;
    impl->modules.clear();
}

/*
 * Module loading
 */

SLANGC_API SlangcModule slangc_loadModuleFromString(
    SlangcCompiler compiler,
    const char* moduleName,
    const char* source)
{
    auto impl = static_cast<SlangcCompilerImpl*>(compiler);
    impl->errorMessages.clear();
    impl->ensureSession();

    if (!impl->session)
        return nullptr;

    ComPtr<slang::IBlob> diagnostics;
    slang::IModule* module = impl->session->loadModuleFromSourceString(
        moduleName,
        moduleName,
        source,
        diagnostics.writeRef());

    impl->appendDiagnostics(diagnostics);

    if (!module)
        return nullptr;

    impl->modules.push_back(ComPtr<slang::IModule>(module));
    return module;
}

SLANGC_API SlangcModule slangc_loadModuleFromFile(SlangcCompiler compiler, const char* path)
{
    auto impl = static_cast<SlangcCompilerImpl*>(compiler);
    impl->errorMessages.clear();
    impl->ensureSession();

    if (!impl->session)
        return nullptr;

    ComPtr<slang::IBlob> diagnostics;
    slang::IModule* module = impl->session->loadModule(path, diagnostics.writeRef());

    impl->appendDiagnostics(diagnostics);

    if (!module)
        return nullptr;

    impl->modules.push_back(ComPtr<slang::IModule>(module));
    return module;
}

SLANGC_API int slangc_getModuleDependencyFileCount(SlangcModule module)
{
    auto slangModule = static_cast<slang::IModule*>(module);
    auto internalModule = static_cast<Slang::Module*>(slangModule);
    if (!internalModule)
        return 0;

    int count = 0;
    for (auto sourceFile : internalModule->getFileDependencies())
    {
        if (sourceFile->getPathInfo().hasFileFoundPath())
            ++count;
    }
    return count;
}

SLANGC_API const char* slangc_getModuleDependencyFilePath(SlangcModule module, int index)
{
    auto slangModule = static_cast<slang::IModule*>(module);
    auto internalModule = static_cast<Slang::Module*>(slangModule);
    if (!internalModule || index < 0)
        return nullptr;

    int fileIndex = 0;
    for (auto sourceFile : internalModule->getFileDependencies())
    {
        const auto& pathInfo = sourceFile->getPathInfo();
        if (!pathInfo.hasFileFoundPath())
            continue;

        if (fileIndex == index)
            return pathInfo.foundPath.getBuffer();

        ++fileIndex;
    }

    return nullptr;
}

/*
 * Program creation and configuration
 */

SLANGC_API SlangcProgram slangc_createProgram(SlangcCompiler compiler)
{
    auto impl = static_cast<SlangcCompilerImpl*>(compiler);
    impl->ensureSession();

    if (!impl->session)
        return nullptr;

    auto program = new SlangcProgramImpl();
    program->compiler = impl;
    return program;
}

SLANGC_API void slangc_destroyProgram(SlangcProgram program)
{
    delete static_cast<SlangcProgramImpl*>(program);
}

SLANGC_API void slangc_addModule(SlangcProgram program, SlangcModule module)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    if (!impl || !module)
        return;
    impl->modules.push_back(static_cast<slang::IModule*>(module));
}

SLANGC_API void slangc_addEntryPoint(
    SlangcProgram program,
    SlangcModule module,
    const char* name,
    SlangcStage stage)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    if (!impl || !module || !name)
        return;

    EntryPointInfo info;
    info.module = static_cast<slang::IModule*>(module);
    info.name = name;
    info.stage = stage;
    impl->entryPoints.push_back(info);
}

SLANGC_API void slangc_setFragmentOutputResolver(
    SlangcCompiler compiler,
    SlangcFragmentOutputResolverCallback callback,
    void* userData)
{
    auto impl = static_cast<SlangcCompilerImpl*>(compiler);
    if (!impl)
        return;
    impl->fragmentOutputResolver = callback;
    impl->fragmentOutputResolverUserData = userData;
}

SLANGC_API void slangc_setBindlessArraySizeResolver(
    SlangcCompiler compiler,
    SlangcBindlessArraySizeResolverCallback callback,
    void* userData)
{
    auto impl = static_cast<SlangcCompilerImpl*>(compiler);
    if (!impl)
        return;
    impl->bindlessArraySizeResolver = callback;
    impl->bindlessArraySizeResolverUserData = userData;
}

/*
 * Linking
 */

SLANGC_API int slangc_link(SlangcProgram program)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    if (!impl || !impl->compiler)
        return 0;

    impl->errorMessages.clear();
    auto compiler = impl->compiler;

    if (impl->entryPoints.empty())
    {
        impl->appendError("No entry points specified");
        return 0;
    }

    // Build component list: modules + entry points
    std::vector<slang::IComponentType*> components;

    // Add all modules
    for (auto mod : impl->modules)
        components.push_back(mod);

    // Find and add entry points (keep refs alive during compose/specialize/link)
    ComPtr<slang::IBlob> diagnostics;
    std::vector<ComPtr<slang::IEntryPoint>> entryPointRefs;
    for (auto& epInfo : impl->entryPoints)
    {
        ComPtr<slang::IEntryPoint> entryPoint;
        if (SLANG_FAILED(epInfo.module->findAndCheckEntryPoint(
            epInfo.name.c_str(),
            toSlangStage(epInfo.stage),
            entryPoint.writeRef(),
            diagnostics.writeRef())))
        {
            impl->appendDiagnostics(diagnostics);
            impl->appendError(("Failed to find entry point: " + epInfo.name).c_str());
            return 0;
        }
        impl->appendDiagnostics(diagnostics);
        entryPointRefs.push_back(entryPoint);
        components.push_back(entryPoint.get());
    }

    // Create composite component
    ComPtr<slang::IComponentType> composedProgram;
    if (SLANG_FAILED(compiler->session->createCompositeComponentType(
        components.data(),
        (SlangInt)components.size(),
        composedProgram.writeRef(),
        diagnostics.writeRef())))
    {
        impl->appendDiagnostics(diagnostics);
        impl->appendError("Failed to compose program");
        return 0;
    }
    impl->appendDiagnostics(diagnostics);

    // Handle specialization if arguments were provided
    ComPtr<slang::IComponentType> programToLink = composedProgram;

    bool hasPositionalArgs = !impl->specializationExprs.empty() || !impl->specializationTypes.empty();
    bool hasNamedArgs = !impl->namedSpecializationArgs.empty();

    if (hasPositionalArgs && hasNamedArgs)
    {
        impl->appendError("Cannot mix positional and named specialization arguments");
        return 0;
    }

    if (hasPositionalArgs)
    {
        auto paramCount = composedProgram->getSpecializationParamCount();
        size_t argCount = impl->specializationIsType.size();

        if (argCount > (size_t)paramCount)
        {
            std::string msg = "Too many specialization arguments: expected at most " +
                std::to_string(paramCount) + ", got " + std::to_string(argCount);
            impl->appendError(msg.c_str());
            return 0;
        }
        // Note: fewer args than params is allowed - the constraint solver
        // will try to infer missing arguments from constraints (e.g., if
        // S : Shader<O> and S = MyShader : Shader<MyShaderOut>, then O can
        // be inferred as MyShaderOut)

        std::vector<slang::SpecializationArg> specArgs;
        for (size_t i = 0; i < argCount; i++)
        {
            slang::SpecializationArg arg;
            if (impl->specializationIsType[i])
            {
                arg.kind = slang::SpecializationArg::Kind::Type;
                arg.type = impl->specializationTypes[i];
            }
            else
            {
                arg.kind = slang::SpecializationArg::Kind::Expr;
                arg.expr = impl->specializationExprs[i].c_str();
            }
            specArgs.push_back(arg);
        }

        ComPtr<slang::IComponentType> specializedProgram;
        if (SLANG_FAILED(composedProgram->specialize(
            specArgs.data(), (SlangInt)specArgs.size(),
            specializedProgram.writeRef(), diagnostics.writeRef())))
        {
            impl->appendDiagnostics(diagnostics);
            impl->appendError("Specialization failed");
            return 0;
        }
        impl->appendDiagnostics(diagnostics);
        programToLink = specializedProgram;
    }
    else if (hasNamedArgs)
    {
        // Named specialization args: look up each param by name
        auto* componentType = static_cast<ComponentType*>(composedProgram.get());
        auto paramCount = componentType->getSpecializationParamCount();
        auto getDeclName = [](NodeBase* object) -> const char*
        {
            if (auto decl = as<Decl>(object))
            {
                auto name = decl->getName();
                return name ? name->text.getBuffer() : nullptr;
            }
            return nullptr;
        };

        std::vector<slang::SpecializationArg> specArgs;
        for (SlangInt i = 0; i < paramCount; i++)
        {
            auto& param = componentType->getSpecializationParam(i);
            slang::SpecializationArg arg;
            arg.kind = slang::SpecializationArg::Kind::Unknown;  // Default: let inference handle it
            arg.expr = nullptr;

            // Get param name based on flavor
            const char* paramName = nullptr;
            if (param.flavor == SpecializationParam::Flavor::GenericType)
            {
                if (auto typeParam = as<GenericTypeParamDeclBase>(param.object))
                    paramName = typeParam->getName() ? typeParam->getName()->text.getBuffer() : nullptr;
                else if (auto globalTypeParam = as<GlobalGenericParamDecl>(param.object))
                    paramName = globalTypeParam->getName() ? globalTypeParam->getName()->text.getBuffer() : nullptr;
                else
                    paramName = getDeclName(param.object);
            }
            else if (param.flavor == SpecializationParam::Flavor::GenericValue)
            {
                if (auto valParam = as<GenericValueParamDecl>(param.object))
                    paramName = valParam->getName() ? valParam->getName()->text.getBuffer() : nullptr;
                else if (auto globalValParam = as<GlobalGenericValueParamDecl>(param.object))
                    paramName = globalValParam->getName() ? globalValParam->getName()->text.getBuffer() : nullptr;
                else
                    paramName = getDeclName(param.object);
            }

            // Look up in named args map
            if (paramName)
            {
                auto it = impl->namedSpecializationArgs.find(paramName);
                if (it != impl->namedSpecializationArgs.end())
                {
                    arg.kind = slang::SpecializationArg::Kind::Expr;
                    arg.expr = it->second.c_str();
                }
            }

            specArgs.push_back(arg);
        }

        ComPtr<slang::IComponentType> specializedProgram;
        if (SLANG_FAILED(composedProgram->specialize(
            specArgs.data(), (SlangInt)specArgs.size(),
            specializedProgram.writeRef(), diagnostics.writeRef())))
        {
            impl->appendDiagnostics(diagnostics);
            impl->appendError("Specialization failed");
            return 0;
        }
        impl->appendDiagnostics(diagnostics);
        programToLink = specializedProgram;
    }

    // Link
    ComPtr<slang::IComponentType> linkedProgram;
    if (SLANG_FAILED(programToLink->link(linkedProgram.writeRef(), diagnostics.writeRef())))
    {
        impl->appendDiagnostics(diagnostics);
        impl->appendError("Failed to link program");
        return 0;
    }
    impl->appendDiagnostics(diagnostics);

    impl->linkedProgram = linkedProgram;
    impl->isLinked = true;

    if (compiler->fragmentOutputResolver)
    {
        ComPtr<slang::IComponentType6> linkedComp6;
        if (SLANG_FAILED(linkedProgram->queryInterface(
                slang::IComponentType6::getTypeGuid(),
                (void**)linkedComp6.writeRef())))
        {
            impl->appendError("Linked program does not support fragment output resolver");
            return 0;
        }

        impl->fragmentOutputResolverWrapper = std::make_unique<FragmentOutputResolverWrapper>();
        impl->fragmentOutputResolverWrapper->userCallback = compiler->fragmentOutputResolver;
        impl->fragmentOutputResolverWrapper->userCallbackData =
            compiler->fragmentOutputResolverUserData;

        linkedComp6->setFragmentOutputResolver(
            0,
            fragmentOutputResolverWrapperCallback,
            impl->fragmentOutputResolverWrapper.get());
    }

    if (compiler->bindlessArraySizeResolver)
    {
        ComPtr<slang::IComponentType7> linkedComp7;
        if (SLANG_FAILED(linkedProgram->queryInterface(
                slang::IComponentType7::getTypeGuid(),
                (void**)linkedComp7.writeRef())))
        {
            impl->appendError("Linked program does not support bindless array size resolver");
            return 0;
        }

        impl->bindlessArraySizeResolverWrapper =
            std::make_unique<BindlessArraySizeResolverWrapper>();
        impl->bindlessArraySizeResolverWrapper->userCallback =
            compiler->bindlessArraySizeResolver;
        impl->bindlessArraySizeResolverWrapper->userCallbackData =
            compiler->bindlessArraySizeResolverUserData;

        linkedComp7->setBindlessArraySizeResolver(
            0,
            bindlessArraySizeResolverWrapperCallback,
            impl->bindlessArraySizeResolverWrapper.get());
    }

    // Get compiled code
    if (SLANG_FAILED(linkedProgram->getTargetCode(
        0,
        impl->codeBlob.writeRef(),
        diagnostics.writeRef())))
    {
        impl->appendDiagnostics(diagnostics);
        impl->appendError("Failed to get target code");
        return 0;
    }
    impl->appendDiagnostics(diagnostics);

    // Helper lambda to recursively collect resources from a type layout
    std::function<void(slang::VariableLayoutReflection*, const std::string&, int)> collectResources;
    collectResources =
        [&](slang::VariableLayoutReflection* varLayout, const std::string& prefix, int inheritedArraySize)
    {
        if (!varLayout)
            return;

        auto typeLayout = varLayout->getTypeLayout();
        if (!typeLayout)
            return;

        int localArraySize = 0;
        if (typeLayout->isArray())
            localArraySize = toC99ArraySize(typeLayout->getTotalArrayElementCount());

        auto effectiveArraySize = combineArraySize(inheritedArraySize, localArraySize);
        auto leafTypeLayout = typeLayout->isArray() ? typeLayout->unwrapArray() : typeLayout;
        auto type = leafTypeLayout ? leafTypeLayout->getType() : nullptr;
        if (!type)
            return;

        auto kind = type->getKind();

        // If this is a struct, recurse into its fields
        if (kind == slang::TypeReflection::Kind::Struct)
        {
            unsigned fieldCount = leafTypeLayout->getFieldCount();
            for (unsigned f = 0; f < fieldCount; f++)
            {
                auto fieldLayout = leafTypeLayout->getFieldByIndex(f);
                if (!fieldLayout)
                    continue;

                auto fieldName = fieldLayout->getName();
                if (!fieldName)
                    continue;

                // Use just the field name for struct members (leaf name only)
                collectResources(fieldLayout, fieldName, effectiveArraySize);
            }
            return;
        }

        // This is a leaf resource type - record it
        ResourceInfoStorage info;
        info.name = prefix;
        info.typeName = "Resource";  // Default
        info.set = -1;
        info.binding = -1;
        info.bindlessIndex = -1;
        info.bindingCount = info.isArray ? info.arraySize : 1;
        info.objectType = classifyResourceObjectType(type);
        info.isArray = effectiveArraySize != 0 ? 1 : 0;
        info.arraySize = effectiveArraySize;
        info.access = SLANGC_ACCESS_READ;  // Default to read-only

        // Determine type name and access
        const char* typeName = type->getName();
        info.typeName = typeName ? typeName : defaultTypeNameForObjectType(info.objectType);

        auto resourceAccess = type->getResourceAccess();
        if (resourceAccess != SLANG_RESOURCE_ACCESS_NONE)
            info.access = toC99Access(resourceAccess);

        // Get binding info and determine access from category
        auto category = varLayout->getCategory();
        if (category == slang::ParameterCategory::DescriptorTableSlot ||
            category == slang::ParameterCategory::ShaderResource ||
            category == slang::ParameterCategory::UnorderedAccess ||
            category == slang::ParameterCategory::ConstantBuffer ||
            category == slang::ParameterCategory::SamplerState)
        {
            info.set = (int)varLayout->getBindingSpace(category);
            info.binding = (int)varLayout->getOffset(category);

            // UnorderedAccess implies read-write access
            if (category == slang::ParameterCategory::UnorderedAccess)
                info.access = SLANGC_ACCESS_READ_WRITE;
        }

        impl->resources.push_back(info);
    };

    // Collect reflection info
    // Get layout for resources
    slang::ProgramLayout* layout = linkedProgram->getLayout(0, diagnostics.writeRef());
    if (layout)
    {
        // Iterate global parameters
        unsigned paramCount = layout->getParameterCount();
        for (unsigned i = 0; i < paramCount; i++)
        {
            auto param = layout->getParameterByIndex(i);
            if (!param)
                continue;

            auto name = param->getName();
            if (!name)
                continue;

            collectResources(param, name, 0);
        }
    }

    // Get bindless resources from metadata
    ComPtr<slang::IMetadata> metadata;
    if (SLANG_SUCCEEDED(linkedProgram->getTargetMetadata(0, metadata.writeRef(), nullptr)))
    {
        ComPtr<IArtifactPostEmitMetadata> postEmitMetadata;
        if (SLANG_SUCCEEDED(metadata->queryInterface(
            IArtifactPostEmitMetadata::getTypeGuid(),
            (void**)postEmitMetadata.writeRef())))
        {
            auto convertedResources = postEmitMetadata->getBindlessConvertedResources();
            for (Index i = 0; i < convertedResources.count; i++)
            {
                const auto& res = convertedResources[i];
                ResourceInfoStorage info;
                info.name = std::string(res.name.begin(), res.name.end());
                info.typeName = std::string(res.typeName.begin(), res.typeName.end());
                info.set = (int)res.set;
                info.binding = (int)res.binding;
                info.bindlessIndex = (int)res.index;
                info.bindingCount = (int)res.bindingCount;
                info.objectType = toC99ObjectType(res.resourceType);
                info.isArray = res.isArray ? 1 : 0;
                info.arraySize = (int)res.arraySize;
                info.access = toC99Access(res.access);
                impl->bindlessResources.push_back(info);
            }
        }
    }

    // Store entry point info for reflection
    for (const auto& epInfo : impl->entryPoints)
    {
        impl->entryPointNames.push_back(epInfo.name);
        impl->entryPointStages.push_back(epInfo.stage);
    }

    return 1;
}

SLANGC_API SlangcBlob slangc_getCode(SlangcProgram program)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    if (!impl || !impl->codeBlob)
        return nullptr;

    auto blob = new SlangcBlobImpl();
    blob->blob = impl->codeBlob;
    return blob;
}

SLANGC_API const char* slangc_getCompilerErrors(SlangcCompiler compiler)
{
    auto impl = static_cast<SlangcCompilerImpl*>(compiler);
    return impl ? impl->errorMessages.c_str() : "";
}

SLANGC_API const char* slangc_getProgramErrors(SlangcProgram program)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    return impl ? impl->errorMessages.c_str() : "";
}

/*
 * Blob access
 */

SLANGC_API const void* slangc_getBlobData(SlangcBlob blob)
{
    auto impl = static_cast<SlangcBlobImpl*>(blob);
    return impl ? impl->blob->getBufferPointer() : nullptr;
}

SLANGC_API size_t slangc_getBlobSize(SlangcBlob blob)
{
    auto impl = static_cast<SlangcBlobImpl*>(blob);
    return impl ? impl->blob->getBufferSize() : 0;
}

SLANGC_API void slangc_destroyBlob(SlangcBlob blob)
{
    delete static_cast<SlangcBlobImpl*>(blob);
}

SLANGC_API int slangc_getResourceCount(SlangcProgram program)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    return impl ? (int)impl->resources.size() : 0;
}

SLANGC_API int slangc_getResource(SlangcProgram program, int index, SlangcResourceInfo* outInfo)
{
    if (!outInfo)
        return 0;

    auto impl = static_cast<SlangcProgramImpl*>(program);
    if (!impl || index < 0 || index >= (int)impl->resources.size())
    {
        clearResourceInfo(outInfo);
        return 0;
    }

    copyResourceInfo(impl->resources[index], outInfo);
    return 1;
}

SLANGC_API int slangc_getBindlessResourceCount(SlangcProgram program)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    return impl ? (int)impl->bindlessResources.size() : 0;
}

SLANGC_API int slangc_getBindlessResource(SlangcProgram program, int index, SlangcResourceInfo* outInfo)
{
    if (!outInfo)
        return 0;

    auto impl = static_cast<SlangcProgramImpl*>(program);
    if (!impl || index < 0 || index >= (int)impl->bindlessResources.size())
    {
        clearResourceInfo(outInfo);
        return 0;
    }

    copyResourceInfo(impl->bindlessResources[index], outInfo);
    return 1;
}

SLANGC_API int slangc_getUsedBindingCount(SlangcProgram program)
{
    return slangc_getBindlessResourceCount(program);
}

SLANGC_API int slangc_getUsedBinding(SlangcProgram program, int index, SlangcResourceInfo* outInfo)
{
    return slangc_getBindlessResource(program, index, outInfo);
}

SLANGC_API int slangc_getEntryPointCount(SlangcProgram program)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    return impl ? (int)impl->entryPointNames.size() : 0;
}

SLANGC_API const char* slangc_getEntryPointName(SlangcProgram program, int index)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    if (!impl || index < 0 || index >= (int)impl->entryPointNames.size())
        return nullptr;
    return impl->entryPointNames[index].c_str();
}

SLANGC_API SlangcStage slangc_getEntryPointStage(SlangcProgram program, int index)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    if (!impl || index < 0 || index >= (int)impl->entryPointStages.size())
        return SLANGC_STAGE_VERTEX;
    return impl->entryPointStages[index];
}

/*
 * Generic Specialization
 */

SLANGC_API int slangc_getSpecializationParamCount(SlangcProgram program)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    if (!impl)
        return 0;

    // Create composed program lazily to query param count
    if (!ensureComposedProgram(impl))
        return 0;

    return (int)impl->composedProgram->getSpecializationParamCount();
}

SLANGC_API SlangcType slangc_findTypeByName(SlangcModule module, const char* typeName)
{
    if (!module || !typeName)
        return nullptr;

    auto mod = static_cast<slang::IModule*>(module);
    auto layout = mod->getLayout();
    if (!layout)
        return nullptr;

    return layout->findTypeByName(typeName);
}

SLANGC_API void slangc_addSpecializationArgExpr(SlangcProgram program, const char* typeExpr)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    if (!impl || !typeExpr)
        return;

    impl->specializationExprs.push_back(normalizeSpecializationExpr(typeExpr));
    impl->specializationTypes.push_back(nullptr);
    impl->specializationIsType.push_back(false);
    impl->needsRecompose = true;  // Invalidate cached composed program
}

SLANGC_API void slangc_addSpecializationArgType(SlangcProgram program, SlangcType type)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    if (!impl || !type)
        return;

    impl->specializationExprs.push_back("");
    impl->specializationTypes.push_back(static_cast<slang::TypeReflection*>(type));
    impl->specializationIsType.push_back(true);
    impl->needsRecompose = true;  // Invalidate cached composed program
}

SLANGC_API void slangc_setSpecializationArg(SlangcProgram program, const char* paramName, const char* typeExpr)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    if (!impl || !paramName || !typeExpr)
        return;

    impl->namedSpecializationArgs[paramName] = normalizeSpecializationExpr(typeExpr);
    impl->needsRecompose = true;
}

SLANGC_API void slangc_clearSpecializationArgs(SlangcProgram program)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    if (!impl)
        return;

    impl->specializationExprs.clear();
    impl->specializationTypes.clear();
    impl->specializationIsType.clear();
    impl->namedSpecializationArgs.clear();
    impl->needsRecompose = true;  // Invalidate cached composed program
}

} // extern "C"
