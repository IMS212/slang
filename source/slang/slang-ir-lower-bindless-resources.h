// slang-ir-lower-bindless-resources.h
#pragma once

#include "slang-ir.h"
#include "../../include/slang.h"

namespace Slang
{

struct IRModule;
class DiagnosticSink;
class TargetProgram;
class ArtifactPostEmitMetadata;

/// Bindless resource heap category.
/// These correspond to different descriptor heap bindings.
/// This is an alias for the public SlangBindlessResourceType enum from slang.h
/// to ensure ABI compatibility with the public API.
enum class BindlessResourceType
{
    Sampler = slang::SLANG_BINDLESS_RESOURCE_TYPE_SAMPLER,
    CombinedTextureSampler = slang::SLANG_BINDLESS_RESOURCE_TYPE_COMBINED_TEXTURE_SAMPLER,
    SampledImage = slang::SLANG_BINDLESS_RESOURCE_TYPE_SAMPLED_IMAGE,
    StorageImage = slang::SLANG_BINDLESS_RESOURCE_TYPE_STORAGE_IMAGE,
    UniformBuffer = slang::SLANG_BINDLESS_RESOURCE_TYPE_UNIFORM_BUFFER,
    StorageBuffer = slang::SLANG_BINDLESS_RESOURCE_TYPE_STORAGE_BUFFER,
};

/// Information about a resource that was converted to bindless access.
struct BindlessConvertedResource
{
    String name;
    String typeName;
    int index;
    int binding;
    int bindingCount;
    BindlessResourceType resourceType;
    bool isArray = false;
    int arraySize = 0;
    ::SlangResourceAccess access;  ///< Access mode (uses SlangResourceAccess enum from slang.h)
};

/// Lower global resources to bindless descriptor handle access.
///
/// This pass transforms global resource parameters (Texture2D, RWStructuredBuffer, etc.)
/// into direct descriptor heap lookups using resolved descriptor indices.
///
/// For each used resource,
/// the pass:
/// 1. Assigns an automatic index-buffer slot (or base slot for arrays)
/// 2. Replaces resource uses with indirect heap indexing: resourceHeap[indexBuffer[slot]]
/// 3. Reports converted resources via the output list
///
/// @param module The IR module to transform
/// @param targetProgram Target-specific options
/// @param sink Diagnostic sink for warnings
/// @param outConvertedResources Output list of resources that were converted
///
void lowerBindlessResources(
    IRModule* module,
    TargetProgram* targetProgram,
    DiagnosticSink* sink,
    List<BindlessConvertedResource>* outConvertedResources);

} // namespace Slang
