// slang-c99.cpp - Implementation of the simple C99 API for Slang

#include "../../include/slang-c99.h"

#include "slang.h"
#include "slang-com-ptr.h"
#include "../../source/compiler-core/slang-artifact-associated.h"

#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>

using namespace Slang;

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

    // Bindless resolver callback (set once, used for all programs)
    SlangcBindlessResolverCallback bindlessResolver = nullptr;
    void* bindlessResolverUserData = nullptr;

    // Cache for bindless resolver results (resourceName:resourceType -> index)
    // Persists across programs to avoid redundant callback invocations
    Slang::Dictionary<Slang::String, int> bindlessResolverCache;

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
    SlangcResourceAccess access;
};

struct EntryPointInfo
{
    slang::IModule* module;
    std::string name;
    SlangcStage stage;
};

//
// Bindless resolver wrapper for caching
// Defined early so it can be used in SlangcProgramImpl
//

struct BindlessResolverWrapper
{
    SlangcBindlessResolverCallback userCallback;
    void* userCallbackData;
    Slang::Dictionary<Slang::String, int>* cache;  // Compiler's cache
};

struct SlangcProgramImpl
{
    SlangcCompilerImpl* compiler;  // Back-reference to compiler

    // Pre-link state
    std::vector<slang::IModule*> modules;
    std::vector<EntryPointInfo> entryPoints;
    std::unordered_map<std::string, int> bindlessIndices;

    // Wrapper for passing resolver to internal API (created during link from compiler's resolver)
    std::unique_ptr<BindlessResolverWrapper> resolverWrapper;

    // Post-link state
    ComPtr<slang::IComponentType> linkedProgram;
    ComPtr<slang::IBlob> codeBlob;
    bool isLinked = false;

    std::vector<ResourceInfoStorage> resources;
    std::vector<ResourceInfoStorage> bindlessResources;
    std::vector<ResourceInfoStorage> unmappedResources;

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

// Make a cache key from resource name and type
static Slang::String makeCacheKey(const char* name, int resourceType)
{
    Slang::StringBuilder sb;
    sb << name << ":" << resourceType;
    return sb.produceString();
}

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

// Wrapper callback that handles caching
// Takes slang::SlangBindlessResourceType (from slang.h) and converts to SlangcBindlessResourceType (for user callback)
static int bindlessResolverWrapperCallback(
    const char* resourceName,
    slang::SlangBindlessResourceType resourceType,
    void* userData)
{
    auto* wrapper = static_cast<BindlessResolverWrapper*>(userData);
    if (!wrapper || !wrapper->userCallback)
        return -1;

    // Check cache first
    Slang::String cacheKey = makeCacheKey(resourceName, (int)resourceType);
    if (wrapper->cache)
    {
        if (auto* cachedIndex = wrapper->cache->tryGetValue(cacheKey))
            return *cachedIndex;
    }

    // Convert to C99 enum type for user callback (values are identical)
    SlangcBindlessResourceType c99Type = static_cast<SlangcBindlessResourceType>(resourceType);

    // Call user callback
    int result = wrapper->userCallback(resourceName, c99Type, wrapper->userCallbackData);

    // Cache the result if valid
    if (wrapper->cache && result >= 0)
    {
        wrapper->cache->add(cacheKey, result);
    }

    return result;
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
    slang::CompilerOptionEntry compilerOptions[2];
    compilerOptions[0].name = slang::CompilerOptionName::BindlessSpaceIndex;
    compilerOptions[0].value.kind = slang::CompilerOptionValueKind::Int;
    compilerOptions[0].value.intValue0 = 0;

    compilerOptions[1].name = slang::CompilerOptionName::DebugInformation;
    compilerOptions[1].value.kind = slang::CompilerOptionValueKind::Int;
    compilerOptions[1].value.intValue0 = SLANG_DEBUG_INFO_LEVEL_STANDARD;

    sessionDesc.compilerOptionEntries = compilerOptions;
    sessionDesc.compilerOptionEntryCount = 2;
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

SLANGC_API void slangc_setBindlessResourceIndex(
    SlangcProgram program,
    const char* resourceName,
    int index)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    if (!impl || !resourceName)
        return;
    impl->bindlessIndices[resourceName] = index;
}

SLANGC_API void slangc_setBindlessResolver(
    SlangcCompiler compiler,
    SlangcBindlessResolverCallback callback,
    void* userData)
{
    auto impl = static_cast<SlangcCompilerImpl*>(compiler);
    if (!impl)
        return;
    impl->bindlessResolver = callback;
    impl->bindlessResolverUserData = userData;
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

    // Find and add entry points
    ComPtr<slang::IBlob> diagnostics;
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

    // Link
    ComPtr<slang::IComponentType> linkedProgram;
    if (SLANG_FAILED(composedProgram->link(linkedProgram.writeRef(), diagnostics.writeRef())))
    {
        impl->appendDiagnostics(diagnostics);
        impl->appendError("Failed to link program");
        return 0;
    }
    impl->appendDiagnostics(diagnostics);

    impl->linkedProgram = linkedProgram;
    impl->isLinked = true;

    // Set bindless config on the LINKED program (it has its own TargetProgram)
    // This must be done AFTER link() but BEFORE getTargetCode()
    ComPtr<slang::IComponentType3> linkedComp3;
    if (SLANG_SUCCEEDED(linkedProgram->queryInterface(
        slang::IComponentType3::getTypeGuid(),
        (void**)linkedComp3.writeRef())))
    {
        // Set static bindless indices
        if (!impl->bindlessIndices.empty())
        {
            std::vector<const char*> names;
            std::vector<SlangInt> indices;
            for (const auto& [name, index] : impl->bindlessIndices)
            {
                names.push_back(name.c_str());
                indices.push_back(index);
            }
            linkedComp3->setBindlessResourceIndexMap(
                0, names.data(), indices.data(), (SlangInt)names.size());
        }

        // Set resolver callback (runs during IR lowering, after DCE)
        if (compiler->bindlessResolver)
        {
            impl->resolverWrapper = std::make_unique<BindlessResolverWrapper>();
            impl->resolverWrapper->userCallback = compiler->bindlessResolver;
            impl->resolverWrapper->userCallbackData = compiler->bindlessResolverUserData;
            impl->resolverWrapper->cache = &compiler->bindlessResolverCache;

            linkedComp3->setBindlessResolver(
                0,
                bindlessResolverWrapperCallback,
                impl->resolverWrapper.get());
        }
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
    std::function<void(slang::VariableLayoutReflection*, const std::string&)> collectResources;
    collectResources = [&](slang::VariableLayoutReflection* varLayout, const std::string& prefix)
    {
        if (!varLayout)
            return;

        auto typeLayout = varLayout->getTypeLayout();
        if (!typeLayout)
            return;

        auto type = typeLayout->getType();
        if (!type)
            return;

        auto kind = type->getKind();

        // If this is a struct, recurse into its fields
        if (kind == slang::TypeReflection::Kind::Struct)
        {
            unsigned fieldCount = typeLayout->getFieldCount();
            for (unsigned f = 0; f < fieldCount; f++)
            {
                auto fieldLayout = typeLayout->getFieldByIndex(f);
                if (!fieldLayout)
                    continue;

                auto fieldName = fieldLayout->getName();
                if (!fieldName)
                    continue;

                // Use just the field name for struct members (leaf name only)
                collectResources(fieldLayout, fieldName);
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
        info.access = SLANGC_ACCESS_READ;  // Default to read-only

        // Determine type name and access
        switch (kind)
        {
        case slang::TypeReflection::Kind::SamplerState:
            info.typeName = "SamplerState";
            break;
        case slang::TypeReflection::Kind::Resource:
            info.typeName = "Texture";
            break;
        case slang::TypeReflection::Kind::ConstantBuffer:
            info.typeName = "ConstantBuffer";
            break;
        case slang::TypeReflection::Kind::ShaderStorageBuffer:
            info.typeName = "StorageBuffer";
            break;
        default:
            break;
        }

        // Get binding info and determine access from category
        auto category = varLayout->getCategory();
        if (category == slang::ParameterCategory::DescriptorTableSlot ||
            category == slang::ParameterCategory::ShaderResource ||
            category == slang::ParameterCategory::UnorderedAccess ||
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

            collectResources(param, name);
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
                info.set = -1;
                info.binding = -1;
                info.bindlessIndex = (int)res.index;
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
        outInfo->name = nullptr;
        outInfo->typeName = nullptr;
        outInfo->set = -1;
        outInfo->binding = -1;
        outInfo->bindlessIndex = -1;
        outInfo->access = SLANGC_ACCESS_READ;
        return 0;
    }

    const auto& info = impl->resources[index];
    outInfo->name = info.name.c_str();
    outInfo->typeName = info.typeName.c_str();
    outInfo->set = info.set;
    outInfo->binding = info.binding;
    outInfo->bindlessIndex = info.bindlessIndex;
    outInfo->access = info.access;
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
        outInfo->name = nullptr;
        outInfo->typeName = nullptr;
        outInfo->set = -1;
        outInfo->binding = -1;
        outInfo->bindlessIndex = -1;
        outInfo->access = SLANGC_ACCESS_READ;
        return 0;
    }

    const auto& info = impl->bindlessResources[index];
    outInfo->name = info.name.c_str();
    outInfo->typeName = info.typeName.c_str();
    outInfo->set = info.set;
    outInfo->binding = info.binding;
    outInfo->bindlessIndex = info.bindlessIndex;
    outInfo->access = info.access;
    return 1;
}

SLANGC_API int slangc_getUnmappedResourceCount(SlangcProgram program)
{
    auto impl = static_cast<SlangcProgramImpl*>(program);
    return impl ? (int)impl->unmappedResources.size() : 0;
}

SLANGC_API int slangc_getUnmappedResource(SlangcProgram program, int index, SlangcResourceInfo* outInfo)
{
    if (!outInfo)
        return 0;

    auto impl = static_cast<SlangcProgramImpl*>(program);
    if (!impl || index < 0 || index >= (int)impl->unmappedResources.size())
    {
        outInfo->name = nullptr;
        outInfo->typeName = nullptr;
        outInfo->set = -1;
        outInfo->binding = -1;
        outInfo->bindlessIndex = -1;
        outInfo->access = SLANGC_ACCESS_READ;
        return 0;
    }

    const auto& info = impl->unmappedResources[index];
    outInfo->name = info.name.c_str();
    outInfo->typeName = info.typeName.c_str();
    outInfo->set = info.set;
    outInfo->binding = info.binding;
    outInfo->bindlessIndex = info.bindlessIndex;
    outInfo->access = info.access;
    return 1;
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

} // extern "C"
