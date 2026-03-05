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

/// Bindless resource type for resolver callback.
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

/// Callback type for resolving bindless resource indices.
/// Called during IR lowering (after DCE) for each actually-used resource.
/// Uses SlangBindlessResourceType from slang.h for ABI compatibility.
/// @param resourceName The name of the resource
/// @param resourceType The type category (determines which heap binding)
/// @param userData User-provided context pointer
/// @return Descriptor heap index for this resource, or -1 to skip (not bindless)
typedef slang::SlangBindlessResolverCallback BindlessResolverCallback;

/// Callback type for resolving bindless resource array base indices.
/// Called during IR lowering (after DCE) for each actually-used resource array.
/// Uses SlangBindlessResourceType from slang.h for ABI compatibility.
/// @param resourceName The name of the resource array
/// @param resourceType The type category (determines which heap binding)
/// @param shaderArrayLength Declared array length in shader, or -1 for unsized
/// @param outResolvedArrayLength Resolved array length written by callback
/// @param userData User-provided context pointer
/// @return Base descriptor heap index for this resource array, or -1 to skip (not bindless)
typedef slang::SlangBindlessArrayResolverCallback BindlessArrayResolverCallback;

/// Callback type for resolving fixed sampler descriptor indices used when
/// lowering combined texture-sampler resources to texture + sampler pairs.
typedef slang::SlangBindlessCombinedSamplerResolverCallback
    BindlessCombinedSamplerResolverCallback;

/// Information about a resource that was converted to bindless access.
struct BindlessConvertedResource
{
    String name;
    String typeName;
    int index;
    BindlessResourceType resourceType;
    ::SlangResourceAccess access;  ///< Access mode (uses SlangResourceAccess enum from slang.h)
};

/// Lower global resources to bindless descriptor handle access.
///
/// This pass transforms global resource parameters (Texture2D, RWStructuredBuffer, etc.)
/// into direct descriptor heap lookups using resolved descriptor indices.
///
/// For each resource in the provided name-to-index map (stored in TargetProgram),
/// the pass:
/// 1. Resolves a descriptor index (or base index for arrays) from map/callback
/// 2. Replaces resource uses with direct heap indexing: resourceHeap[resolvedIndex]
/// 3. Reports converted resources via the output list
///
/// Resources not found in the map are left unchanged and a warning is emitted.
///
/// @param module The IR module to transform
/// @param targetProgram Contains the name-to-index map in m_bindlessResourceIndexMap
/// @param sink Diagnostic sink for warnings
/// @param outConvertedResources Output list of resources that were converted
///
void lowerBindlessResources(
    IRModule* module,
    TargetProgram* targetProgram,
    DiagnosticSink* sink,
    List<BindlessConvertedResource>* outConvertedResources);

} // namespace Slang
