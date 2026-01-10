// slang-ir-lower-bindless-resources.h
#pragma once

#include "slang-ir.h"

namespace Slang
{

struct IRModule;
class DiagnosticSink;
class TargetProgram;
class ArtifactPostEmitMetadata;

/// Information about a resource that was converted to bindless access.
struct BindlessConvertedResource
{
    String name;
    String typeName;
    int index;
};

/// Lower global resources to bindless descriptor handle access.
///
/// This pass transforms global resource parameters (Texture2D, RWStructuredBuffer, etc.)
/// into DescriptorHandle-based lookups from an index buffer.
///
/// For each resource in the provided name-to-index map (stored in TargetProgram),
/// the pass:
/// 1. Creates a global index buffer (StructuredBuffer<uint2>) at set 1, binding 3
/// 2. Replaces resource uses with DescriptorHandle<T>(indexBuffer[STATIC_INDEX])
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
