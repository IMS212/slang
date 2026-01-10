// slang-ir-lower-bindless-resources.cpp
#include "slang-ir-lower-bindless-resources.h"

#include "slang-ir-insts.h"
#include "slang-ir-util.h"
#include "slang-legalize-types.h"
#include "slang-target-program.h"
#include "slang-diagnostics.h"

namespace Slang
{

struct BindlessResourceLoweringContext
{
    IRModule* module;
    DiagnosticSink* sink;
    TargetProgram* targetProgram;
    List<BindlessConvertedResource>* outConvertedResources;

    // The index buffer SSBO (created once, shared by all)
    IRGlobalParam* indexBuffer = nullptr;

    // Resource heap arrays for each (bindingIndex, elementType) pair
    // We use a combined key: bindingIndex * 65536 + type pointer hash
    Dictionary<uint64_t, IRInst*> resourceHeaps;

    uint64_t makeResourceHeapKey(int bindingIndex, IRType* elementType)
    {
        // Combine binding index with type pointer for unique key
        return ((uint64_t)bindingIndex << 48) | ((uint64_t)(uintptr_t)elementType & 0xFFFFFFFFFFFF);
    }

    // Get or create a resource heap for a given binding index and element type
    IRInst* getOrCreateResourceHeap(int bindingIndex, IRType* elementType)
    {
        auto key = makeResourceHeapKey(bindingIndex, elementType);
        if (auto* existing = resourceHeaps.tryGetValue(key))
            return *existing;

        // Create at module scope
        IRBuilder moduleBuilder(module);
        moduleBuilder.setInsertInto(module->getModuleInst());

        auto unboundedArrayType = moduleBuilder.getUnsizedArrayType(elementType);
        auto bindingIndexLit = moduleBuilder.getIntValue(moduleBuilder.getBasicType(BaseType::UInt), bindingIndex);
        auto resourceHeap = moduleBuilder.emitIntrinsicInst(
            unboundedArrayType,
            kIROp_GetDynamicResourceHeap,
            1,
            &bindingIndexLit);

        resourceHeaps[key] = resourceHeap;
        return resourceHeap;
    }

    // Try to find a resource name in the index map.
    // First tries exact match, then tries suffix match for hoisted struct members.
    // Type legalization hoists struct members using "." as separator (e.g., "structVar.field").
    // Returns -1 if not found.
    int findIndexForName(const String& name, const Dictionary<String, int>& indexMap)
    {
        // Try exact match first
        if (auto* index = indexMap.tryGetValue(name))
            return *index;

        // Try suffix match for hoisted struct members
        // Type legalization uses "." as separator (e.g., "structVar.field" matches key "field")
        for (const auto& [key, value] : indexMap)
        {
            String suffix = "." + key;
            if (name.endsWith(suffix))
                return value;
        }

        return -1;
    }

    // Get the matching key name for a resource (for metadata output)
    String findMatchingKeyName(const String& name, const Dictionary<String, int>& indexMap)
    {
        if (indexMap.containsKey(name))
            return name;

        for (const auto& [key, value] : indexMap)
        {
            String suffix = "." + key;
            if (name.endsWith(suffix))
                return key;
        }
        return name;
    }

    // Get the binding index for a resource type (VkMutable bindings)
    // Sampler = 0, CombinedTextureSampler = 1, everything else = 2
    int getBindingIndexForResourceType(IRType* type)
    {
        switch (type->getOp())
        {
        case kIROp_SamplerStateType:
        case kIROp_SamplerComparisonStateType:
            return 0; // Sampler binding
        case kIROp_TextureType:
        {
            // Check if this is a combined texture sampler
            auto textureType = as<IRTextureType>(type);
            if (textureType && textureType->isCombined())
                return 1; // CombinedTextureSampler binding
            return 2; // SampledImage/StorageImage binding
        }
        default:
            return 2; // Buffer types and others
        }
    }

    void processModule()
    {
        // 1. Get the name-to-index map from TargetProgram
        auto& indexMap = targetProgram->m_bindlessResourceIndexMap;
        if (indexMap.getCount() == 0)
            return; // Nothing to do

        // 2. Find global resources to convert
        List<IRGlobalParam*> resourcesToConvert;
        List<IRGlobalParam*> unmappedResources;

        for (auto inst : module->getGlobalInsts())
        {
            auto globalParam = as<IRGlobalParam>(inst);
            if (!globalParam)
                continue;

            // Check if it's a resource type
            auto paramType = globalParam->getDataType();
            if (!isResourceType(paramType))
                continue;

            // Check if it has uses (actively used)
            if (!globalParam->hasUses())
                continue;

            // Get name from decoration
            auto nameHint = globalParam->findDecoration<IRNameHintDecoration>();
            if (!nameHint)
                continue;

            String name = nameHint->getName();

            // Check if name is in map (supports both exact match and suffix match for hoisted members)
            if (findIndexForName(name, indexMap) >= 0)
            {
                resourcesToConvert.add(globalParam);
            }
            else
            {
                unmappedResources.add(globalParam);
            }
        }

        // 3. Emit warnings for unmapped resources
        for (auto param : unmappedResources)
        {
            auto nameHint = param->findDecoration<IRNameHintDecoration>();
            if (nameHint && sink)
            {
                sink->diagnose(
                    param->sourceLoc,
                    Diagnostics::resourceNotInBindlessMap,
                    nameHint->getName());
            }
        }

        // 4. If no resources to convert, we're done
        if (resourcesToConvert.getCount() == 0)
            return;

        // 5. Create the index buffer SSBO at set 1, binding 3
        createIndexBuffer();

        // 6. Convert each resource
        for (auto globalParam : resourcesToConvert)
        {
            convertResource(globalParam, indexMap);
        }
    }

    void createIndexBuffer()
    {
        IRBuilder builder(module);
        builder.setInsertInto(module->getModuleInst());

        // Type: StructuredBuffer<uint2>
        // uint2 is used because DescriptorHandle is typically 64 bits (2x32-bit)
        auto uintType = builder.getBasicType(BaseType::UInt);
        auto uint2Type = builder.getVectorType(uintType, 2);
        auto structuredBufferType = builder.getType(kIROp_HLSLStructuredBufferType, uint2Type);

        indexBuffer = builder.createGlobalParam(structuredBufferType);
        builder.addNameHintDecoration(indexBuffer, toSlice("__slang_bindless_indices"));

        // Create layout: set 1, binding 3
        auto varLayout = createIndexBufferLayout(builder);
        builder.addLayoutDecoration(indexBuffer, varLayout);
    }

    IRVarLayout* createIndexBufferLayout(IRBuilder& builder)
    {
        // Create type layout indicating this uses a descriptor table slot
        IRTypeLayout::Builder typeLayoutBuilder(&builder);
        typeLayoutBuilder.addResourceUsage(LayoutResourceKind::DescriptorTableSlot, LayoutSize(1));
        auto typeLayout = typeLayoutBuilder.build();

        // Create var layout with set 1, binding 3
        IRVarLayout::Builder varLayoutBuilder(&builder, typeLayout);
        varLayoutBuilder.findOrAddResourceInfo(LayoutResourceKind::RegisterSpace)->offset = 1;
        varLayoutBuilder.findOrAddResourceInfo(LayoutResourceKind::DescriptorTableSlot)->offset = 3;
        return varLayoutBuilder.build();
    }

    void convertResource(IRGlobalParam* param, const Dictionary<String, int>& indexMap)
    {
        auto nameHint = param->findDecoration<IRNameHintDecoration>();
        String name = nameHint->getName();
        int index = findIndexForName(name, indexMap);

        auto resourceType = param->getDataType();

        IRBuilder builder(module);

        // We need to create a replacement value that can be used wherever the
        // global param was used. Since global params are used directly (not loaded from),
        // we need to create a global that computes the value.
        //
        // Strategy: For each use of the global param, replace it with the
        // dereferenced descriptor handle.

        // Collect all uses first (since we'll be modifying them)
        List<IRUse*> uses;
        for (auto use = param->firstUse; use; use = use->nextUse)
        {
            uses.add(use);
        }

        // For each use, insert the lookup and cast instructions before the user
        for (auto use : uses)
        {
            auto user = use->getUser();

            // Skip uses in layout attributes and decorations - they shouldn't be replaced
            // with actual resource loads
            if (as<IRStructFieldLayoutAttr>(user))
                continue;
            if (as<IRDecoration>(user))
                continue;

            // Only process uses that are inside functions (i.e., have an IRBlock ancestor)
            auto parent = user->getParent();
            bool isInsideFunction = false;
            while (parent)
            {
                if (as<IRBlock>(parent))
                {
                    isInsideFunction = true;
                    break;
                }
                parent = parent->getParent();
            }
            if (!isInsideFunction)
                continue;

            builder.setInsertBefore(user);

            // 1. Load index buffer element: indexBuffer[STATIC_INDEX]
            auto indexLiteral = builder.getIntValue(builder.getBasicType(BaseType::Int), index);
            auto uint2Type = builder.getVectorType(builder.getBasicType(BaseType::UInt), 2);
            IRInst* loadArgs[] = { indexBuffer, indexLiteral };
            auto uint2Value = builder.emitIntrinsicInst(uint2Type, kIROp_StructuredBufferLoad, 2, loadArgs);

            // Get binding index based on resource type (VkMutable bindings)
            int bindingIndex = getBindingIndexForResourceType(resourceType);

            // Get or create the resource heap at module scope
            auto resourceHeap = getOrCreateResourceHeap(bindingIndex, resourceType);

            // Extract the index from uint2 (use .x component)
            auto uintType = builder.getBasicType(BaseType::UInt);
            uint32_t swizzleIndex = 0; // .x component
            auto heapIndex = builder.emitSwizzle(uintType, uint2Value, 1, &swizzleIndex);

            // For SPIRV, unbounded arrays need pointer-based access:
            // 1. Get a pointer to the element (OpAccessChain)
            // 2. Load from that pointer
            auto ptrType = builder.getPtrType(resourceType);
            auto elementPtr = builder.emitElementAddress(ptrType, resourceHeap, heapIndex);
            auto dereferencedResource = builder.emitLoad(resourceType, elementPtr);

            // Replace this use with the dereferenced resource
            builder.replaceOperand(use, dereferencedResource);
        }

        // Record for metadata output (use the matched key name, not the hoisted name)
        if (outConvertedResources)
        {
            BindlessConvertedResource info;
            info.name = findMatchingKeyName(name, indexMap);
            info.typeName = getResourceTypeName(resourceType);
            info.index = index;
            outConvertedResources->add(info);
        }

        // Remove the original global param only if it has no remaining uses
        // (some uses like layout attributes may have been skipped)
        if (!param->hasUses())
        {
            param->removeAndDeallocate();
        }
    }

    String getResourceTypeName(IRType* type)
    {
        // Get a human-readable name for the resource type
        switch (type->getOp())
        {
        case kIROp_HLSLStructuredBufferType:
            return "StructuredBuffer";
        case kIROp_HLSLRWStructuredBufferType:
            return "RWStructuredBuffer";
        case kIROp_HLSLByteAddressBufferType:
            return "ByteAddressBuffer";
        case kIROp_HLSLRWByteAddressBufferType:
            return "RWByteAddressBuffer";
        case kIROp_SamplerStateType:
            return "SamplerState";
        case kIROp_SamplerComparisonStateType:
            return "SamplerComparisonState";
        case kIROp_TextureType:
        {
            auto textureType = as<IRTextureType>(type);
            if (textureType)
            {
                StringBuilder sb;
                auto access = textureType->getAccess();
                if (access == SLANG_RESOURCE_ACCESS_READ_WRITE)
                    sb << "RW";
                sb << "Texture";
                switch (textureType->GetBaseShape())
                {
                case SLANG_TEXTURE_1D:
                    sb << "1D";
                    break;
                case SLANG_TEXTURE_2D:
                    sb << "2D";
                    break;
                case SLANG_TEXTURE_3D:
                    sb << "3D";
                    break;
                case SLANG_TEXTURE_CUBE:
                    sb << "Cube";
                    break;
                case SLANG_TEXTURE_BUFFER:
                    sb << "Buffer";
                    break;
                }
                if (textureType->isArray())
                    sb << "Array";
                return sb.produceString();
            }
            return "Texture";
        }
        default:
            return "Resource";
        }
    }
};

void lowerBindlessResources(
    IRModule* module,
    TargetProgram* targetProgram,
    DiagnosticSink* sink,
    List<BindlessConvertedResource>* outConvertedResources)
{
    BindlessResourceLoweringContext context;
    context.module = module;
    context.sink = sink;
    context.targetProgram = targetProgram;
    context.outConvertedResources = outConvertedResources;
    context.processModule();
}

} // namespace Slang
