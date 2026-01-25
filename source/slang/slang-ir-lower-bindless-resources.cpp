// slang-ir-lower-bindless-resources.cpp
#include "slang-ir-lower-bindless-resources.h"

#include "slang-ir-insts.h"
#include "slang-ir-util.h"
#include "slang-ir-layout.h"
#include "slang-ir-lower-buffer-element-type.h"
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
    Dictionary<uint64_t, IRGlobalParam*> resourceHeaps;

    // Info about lowered structured buffer wrapper types
    struct LoweredStructuredBufferTypeInfo
    {
        IRStructType* wrapperStructType;
        IRStructKey* arrayKey;
        IRArrayTypeBase* unsizedArrayType;
    };
    Dictionary<IRType*, LoweredStructuredBufferTypeInfo> loweredStructuredBufferTypes;

    uint64_t makeResourceHeapKey(int bindingIndex, IRType* elementType)
    {
        // Combine binding index with type pointer for unique key
        return ((uint64_t)bindingIndex << 48) | ((uint64_t)(uintptr_t)elementType & 0xFFFFFFFFFFFF);
    }

    // Create a var layout for a resource heap with specific set/binding
    IRVarLayout* createResourceHeapLayout(IRBuilder& builder, UInt spaceIndex, UInt bindingIndex)
    {
        IRTypeLayout::Builder typeLayoutBuilder(&builder);
        typeLayoutBuilder.addResourceUsage(
            LayoutResourceKind::DescriptorTableSlot,
            LayoutSize::infinite());
        auto typeLayout = typeLayoutBuilder.build();
        IRVarLayout::Builder varLayoutBuilder(&builder, typeLayout);
        varLayoutBuilder.findOrAddResourceInfo(LayoutResourceKind::RegisterSpace)->offset = spaceIndex;
        varLayoutBuilder.findOrAddResourceInfo(LayoutResourceKind::DescriptorTableSlot)->offset = bindingIndex;
        return varLayoutBuilder.build();
    }

    AddressSpace getResourceHeapAddressSpace(IRType* elementType)
    {
        switch (elementType->getOp())
        {
        case kIROp_SamplerStateType:
        case kIROp_SamplerComparisonStateType:
        case kIROp_TextureType:
            return AddressSpace::UniformConstant;

        case kIROp_ConstantBufferType:
        case kIROp_ParameterBlockType:
            return AddressSpace::Uniform;

        case kIROp_HLSLStructuredBufferType:
        case kIROp_HLSLRWStructuredBufferType:
        case kIROp_HLSLByteAddressBufferType:
        case kIROp_HLSLRWByteAddressBufferType:
        case kIROp_HLSLAppendStructuredBufferType:
        case kIROp_HLSLConsumeStructuredBufferType:
        case kIROp_HLSLRasterizerOrderedStructuredBufferType:
        case kIROp_HLSLRasterizerOrderedByteAddressBufferType:
            return AddressSpace::StorageBuffer;

        default:
            return AddressSpace::StorageBuffer;
        }
    }

    // Get address space from binding index (for when element type doesn't indicate it)
    AddressSpace getAddressSpaceFromBindingIndex(int bindingIndex)
    {
        switch (bindingIndex)
        {
        case 0: // Samplers
        case 1: // Combined Texture Samplers
        case 2: // Textures (SampledImage)
        case 3: // RWTextures (StorageImage)
            return AddressSpace::UniformConstant;
        case 4: // UBOs (uniform buffers)
            return AddressSpace::Uniform;
        case 5: // SSBOs (storage buffers)
        default:
            return AddressSpace::StorageBuffer;
        }
    }

    // Get or create a resource heap for a given binding index and element type
    // Creates a real GlobalParam (OpVariable in SPIR-V), not an intrinsic
    IRGlobalParam* getOrCreateResourceHeap(int bindingIndex, IRType* elementType)
    {
        auto key = makeResourceHeapKey(bindingIndex, elementType);
        if (auto* existing = resourceHeaps.tryGetValue(key))
            return *existing;

        // Create a GlobalParam at module scope - this becomes OpVariable in SPIR-V
        IRBuilder moduleBuilder(module);
        moduleBuilder.setInsertInto(module->getModuleInst());

        auto unboundedArrayType = moduleBuilder.getUnsizedArrayType(elementType);

        // Determine address space: first try based on element type, fall back to binding index
        AddressSpace addrSpace = getResourceHeapAddressSpace(elementType);
        // If the element type didn't give us a specific address space (e.g., it's a struct),
        // use the binding index to determine it
        if (addrSpace == AddressSpace::StorageBuffer && !isResourceType(elementType))
        {
            addrSpace = getAddressSpaceFromBindingIndex(bindingIndex);
        }

        // For uniform buffer heaps (binding 4), the element type needs the SPIRV Block decoration
        if (bindingIndex == 4 && as<IRStructType>(elementType))
        {
            moduleBuilder.addDecorationIfNotExist(elementType, kIROp_SPIRVBlockDecoration);
        }

        auto heapPtrType = moduleBuilder.getPtrType(
            unboundedArrayType,
            AccessQualifier::ReadWrite,
            addrSpace);

        // Create a real global parameter (becomes OpVariable in SPIR-V)
        auto resourceHeap = moduleBuilder.createGlobalParam(heapPtrType);

        // Add layout decoration with set 0 (bindless descriptor set) and the binding index
        // The space index comes from the target program's bindless configuration
        UInt spaceIndex = 0;
        if (targetProgram)
        {
            spaceIndex = targetProgram->getOptionSet().getIntOption(
                CompilerOptionName::BindlessSpaceIndex);
        }
        auto varLayout = createResourceHeapLayout(moduleBuilder, spaceIndex, (UInt)bindingIndex);
        moduleBuilder.addLayoutDecoration(resourceHeap, varLayout);
        moduleBuilder.addNameHintDecoration(resourceHeap, toSlice("__slang_resource_heap"));
        moduleBuilder.addRequireSPIRVDescriptorIndexingExtensionDecoration(resourceHeap);

        resourceHeaps[key] = resourceHeap;
        return resourceHeap;
    }

    // Create or get a wrapper struct type for StructuredBuffer<T>
    // This mirrors what SPIRV legalization does: creates a struct containing T[]
    LoweredStructuredBufferTypeInfo getOrCreateStructuredBufferWrapperType(IRHLSLStructuredBufferTypeBase* bufferType)
    {
        auto elementType = bufferType->getElementType();

        // Check cache
        if (auto* existing = loweredStructuredBufferTypes.tryGetValue(elementType))
            return *existing;

        // Create wrapper struct at module scope
        IRBuilder moduleBuilder(module);
        moduleBuilder.setInsertInto(module->getModuleInst());

        // Get the layout rules for this buffer type to compute proper stride
        auto layoutRules = getTypeLayoutRuleForBuffer(targetProgram, bufferType);

        // For element types that are structs used in storage buffers, we need to ensure
        // they have the correct Std430 layout. The SPIRV emitter uses the first
        // IRSizeAndAlignmentDecoration it finds on the struct to determine layout rules.
        // We need to remove any existing decorations with wrong layout rules so our
        // Std430 decoration takes precedence.
        if (auto elementStruct = as<IRStructType>(elementType))
        {
            // Remove ALL existing size/alignment decorations that use a different layout rule
            // This ensures our Std430 decoration is the first one found by the emitter
            List<IRDecoration*> decorationsToRemove;
            for (auto decor : elementStruct->getDecorations())
            {
                if (auto sizeAlignDecor = as<IRSizeAndAlignmentDecoration>(decor))
                {
                    if (sizeAlignDecor->getLayoutName() != layoutRules->ruleName)
                    {
                        decorationsToRemove.add(decor);
                    }
                }
            }
            for (auto decor : decorationsToRemove)
            {
                decor->removeAndDeallocate();
            }

            // Also remove any existing field offset decorations with wrong layout rule
            // The SPIRV emitter checks for these when computing member offsets
            for (auto field : elementStruct->getFields())
            {
                List<IRDecoration*> fieldDecorsToRemove;
                for (auto decor : field->getDecorations())
                {
                    if (auto offsetDecor = as<IROffsetDecoration>(decor))
                    {
                        if (offsetDecor->getLayoutName() != layoutRules->ruleName)
                        {
                            fieldDecorsToRemove.add(decor);
                        }
                    }
                }
                for (auto decor : fieldDecorsToRemove)
                {
                    decor->removeAndDeallocate();
                }
            }
        }

        // Compute element size and alignment for proper array stride
        // This adds IRSizeAndAlignmentDecoration to the element type with the proper layout rules
        IRSizeAndAlignment elementSize;
        getSizeAndAlignment(
            targetProgram->getTargetReq(),
            layoutRules,
            elementType,
            &elementSize);
        elementSize = layoutRules->alignCompositeElement(elementSize);

        // Mark element type as physical if it's a struct (enables SPIRV member offset decorations)
        if (auto elementStruct = as<IRStructType>(elementType))
        {
            moduleBuilder.addPhysicalTypeDecoration(elementStruct);
        }

        auto wrapperStruct = moduleBuilder.createStructType();
        moduleBuilder.addPhysicalTypeDecoration(wrapperStruct);

        // Create the struct key for the inner array field
        auto arrayKey = moduleBuilder.createStructKey();
        moduleBuilder.addNameHintDecoration(arrayKey, toSlice("_data"));

        // Create unsized array of element type WITH proper stride for SPIRV
        auto unsizedArrayType = moduleBuilder.getUnsizedArrayType(
            elementType,
            moduleBuilder.getIntValue(moduleBuilder.getIntType(), elementSize.getStride()));

        // Add the array as a field of the wrapper struct
        moduleBuilder.createStructField(wrapperStruct, arrayKey, unsizedArrayType);

        // Compute size/alignment for the wrapper struct - this adds IRSizeAndAlignmentDecoration
        // which is needed by SPIRV emitter for proper layout decorations
        IRSizeAndAlignment structSize;
        getSizeAndAlignment(targetProgram->getTargetReq(), layoutRules, wrapperStruct, &structSize);

        // Add Block decoration for SPIRV
        moduleBuilder.addDecorationIfNotExist(wrapperStruct, kIROp_SPIRVBlockDecoration);

        // Add name hint based on buffer type
        StringBuilder nameSb;
        switch (bufferType->getOp())
        {
        case kIROp_HLSLRWStructuredBufferType:
            nameSb << "RWStructuredBuffer_";
            break;
        case kIROp_HLSLAppendStructuredBufferType:
            nameSb << "AppendStructuredBuffer_";
            break;
        case kIROp_HLSLConsumeStructuredBufferType:
            nameSb << "ConsumeStructuredBuffer_";
            break;
        case kIROp_HLSLRasterizerOrderedStructuredBufferType:
            nameSb << "RasterizerOrderedStructuredBuffer_";
            break;
        default:
            nameSb << "StructuredBuffer_";
            break;
        }
        getTypeNameHint(nameSb, elementType);
        moduleBuilder.addNameHintDecoration(wrapperStruct, nameSb.getUnownedSlice());

        LoweredStructuredBufferTypeInfo result;
        result.wrapperStructType = wrapperStruct;
        result.arrayKey = arrayKey;
        result.unsizedArrayType = unsizedArrayType;

        loweredStructuredBufferTypes[elementType] = result;
        return result;
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

    // Map IR type to BindlessResourceType enum for the resolver callback
    BindlessResourceType getBindlessResourceTypeForIRType(IRType* type)
    {
        switch (type->getOp())
        {
        case kIROp_SamplerStateType:
        case kIROp_SamplerComparisonStateType:
            return BindlessResourceType::Sampler;

        case kIROp_TextureType:
        {
            auto textureType = as<IRTextureType>(type);
            if (textureType && textureType->isCombined())
                return BindlessResourceType::CombinedTextureSampler;
            if (textureType && textureType->getAccess() == SLANG_RESOURCE_ACCESS_READ_WRITE)
                return BindlessResourceType::StorageImage;
            return BindlessResourceType::SampledImage;
        }

        case kIROp_ConstantBufferType:
        case kIROp_ParameterBlockType:
            return BindlessResourceType::UniformBuffer;

        case kIROp_HLSLStructuredBufferType:
        case kIROp_HLSLRWStructuredBufferType:
        case kIROp_HLSLByteAddressBufferType:
        case kIROp_HLSLRWByteAddressBufferType:
        case kIROp_HLSLAppendStructuredBufferType:
        case kIROp_HLSLConsumeStructuredBufferType:
        case kIROp_HLSLRasterizerOrderedStructuredBufferType:
        case kIROp_HLSLRasterizerOrderedByteAddressBufferType:
            return BindlessResourceType::StorageBuffer;

        default:
            return BindlessResourceType::StorageBuffer;
        }
    }

    // Determine the access mode for a resource type
    ::SlangResourceAccess getResourceAccess(IRType* type)
    {
        switch (type->getOp())
        {
        // Read-write buffer types
        case kIROp_HLSLRWStructuredBufferType:
        case kIROp_HLSLRWByteAddressBufferType:
            return SLANG_RESOURCE_ACCESS_READ_WRITE;

        // Rasterizer ordered types
        case kIROp_HLSLRasterizerOrderedStructuredBufferType:
        case kIROp_HLSLRasterizerOrderedByteAddressBufferType:
            return SLANG_RESOURCE_ACCESS_RASTER_ORDERED;

        // Append/consume types
        case kIROp_HLSLAppendStructuredBufferType:
            return SLANG_RESOURCE_ACCESS_APPEND;
        case kIROp_HLSLConsumeStructuredBufferType:
            return SLANG_RESOURCE_ACCESS_CONSUME;

        // Textures need access check
        case kIROp_TextureType:
        {
            auto textureType = as<IRTextureType>(type);
            if (textureType)
                return textureType->getAccess();
            return SLANG_RESOURCE_ACCESS_READ;
        }

        // Read-only types (StructuredBuffer, ByteAddressBuffer, ConstantBuffer, samplers, etc.)
        default:
            return SLANG_RESOURCE_ACCESS_READ;
        }
    }

    // Make a cache key from resource name and type
    String makeCacheKey(const String& name, BindlessResourceType resourceType)
    {
        StringBuilder sb;
        sb << name << ":" << (int)resourceType;
        return sb.produceString();
    }

    // Get the binding index for a resource type (VkMutable bindings)
    // Binding 0: Samplers
    // Binding 1: Combined Texture Samplers
    // Binding 2: Textures (read-only, SampledImage)
    // Binding 3: RWTextures (read-write, StorageImage)
    // Binding 4: UBOs (ConstantBuffer)
    // Binding 5: SSBOs (StructuredBuffer, ByteAddressBuffer, etc.)
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
            // Check access mode for read-only vs read-write
            if (textureType && textureType->getAccess() == SLANG_RESOURCE_ACCESS_READ_WRITE)
                return 3; // RWTexture (StorageImage) binding
            return 2; // Texture (SampledImage) binding
        }

        case kIROp_ConstantBufferType:
        case kIROp_ParameterBlockType:
            return 4; // UBO binding

        case kIROp_HLSLStructuredBufferType:
        case kIROp_HLSLRWStructuredBufferType:
        case kIROp_HLSLByteAddressBufferType:
        case kIROp_HLSLRWByteAddressBufferType:
        case kIROp_HLSLAppendStructuredBufferType:
        case kIROp_HLSLConsumeStructuredBufferType:
        case kIROp_HLSLRasterizerOrderedStructuredBufferType:
        case kIROp_HLSLRasterizerOrderedByteAddressBufferType:
            return 5; // SSBO binding

        default:
            return 5; // Other buffer types default to SSBO
        }
    }

    // Resolve a bindless index for a resource, using cache and callback if available
    int resolveBindlessIndex(const String& name, IRType* resourceType)
    {
        auto& indexMap = targetProgram->m_bindlessResourceIndexMap;
        auto resolver = targetProgram->m_bindlessResolver;
        auto cache = targetProgram->m_bindlessResolverCache;
        auto userData = targetProgram->m_bindlessResolverUserData;

        // 1. First check static index map (exact or suffix match)
        int staticIndex = findIndexForName(name, indexMap);
        if (staticIndex >= 0)
            return staticIndex;

        // 2. If no resolver callback, resource is not mapped
        if (!resolver)
            return -1;

        // 3. Get the resource type for the callback
        BindlessResourceType bindlessType = getBindlessResourceTypeForIRType(resourceType);

        // 4. Check the cache
        String cacheKey = makeCacheKey(name, bindlessType);
        if (cache)
        {
            if (auto* cachedIndex = cache->tryGetValue(cacheKey))
                return *cachedIndex;
        }

        // 5. Call the resolver callback (convert enum class to C enum for public API)
        slang::SlangBindlessResourceType publicType = static_cast<slang::SlangBindlessResourceType>(bindlessType);
        int resolvedIndex = resolver(name.getBuffer(), publicType, userData);

        // 6. Cache the result (even if -1, to avoid repeated calls)
        if (cache && resolvedIndex >= 0)
        {
            cache->add(cacheKey, resolvedIndex);
        }

        return resolvedIndex;
    }

    void processModule()
    {
        // Check if we have any bindless configuration
        auto& indexMap = targetProgram->m_bindlessResourceIndexMap;
        auto resolver = targetProgram->m_bindlessResolver;
        if (indexMap.getCount() == 0 && !resolver)
            return; // Nothing to do

        // Find global resources to convert (after DCE, so only used resources remain)
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

            // Check if it has uses (actively used after DCE)
            if (!globalParam->hasUses())
                continue;

            // Get name from decoration
            auto nameHint = globalParam->findDecoration<IRNameHintDecoration>();
            if (!nameHint)
                continue;

            String name = nameHint->getName();

            // Try to resolve an index for this resource
            int index = resolveBindlessIndex(name, paramType);
            if (index >= 0)
            {
                resourcesToConvert.add(globalParam);
            }
            else
            {
                unmappedResources.add(globalParam);
            }
        }

        // Emit warnings for unmapped resources (only if we have any bindless config)
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

        // If no resources to convert, we're done
        if (resourcesToConvert.getCount() == 0)
            return;

        // Create the index buffer SSBO at set 1, binding 3
        createIndexBuffer();

        // Convert each resource
        for (auto globalParam : resourcesToConvert)
        {
            convertResourceWithResolver(globalParam);
        }
    }

    void createIndexBuffer()
    {
        IRBuilder builder(module);
        builder.setInsertInto(module->getModuleInst());

        // Type: StructuredBuffer<uint>
        auto uintType = builder.getBasicType(BaseType::UInt);
        auto structuredBufferType = builder.getType(kIROp_HLSLStructuredBufferType, uintType);

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

        // Create var layout with set 0, binding 6
        IRVarLayout::Builder varLayoutBuilder(&builder, typeLayout);
        varLayoutBuilder.findOrAddResourceInfo(LayoutResourceKind::RegisterSpace)->offset = 0;
        varLayoutBuilder.findOrAddResourceInfo(LayoutResourceKind::DescriptorTableSlot)->offset = 6;
        return varLayoutBuilder.build();
    }

    void convertResourceWithResolver(IRGlobalParam* param)
    {
        auto nameHint = param->findDecoration<IRNameHintDecoration>();
        String name = nameHint->getName();
        auto resourceType = param->getDataType();

        // Resolve the index (will use cache if already resolved)
        int index = resolveBindlessIndex(name, resourceType);

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
            auto uintType = builder.getBasicType(BaseType::UInt);
            IRInst* loadArgs[] = { indexBuffer, indexLiteral };
            auto heapIndex = builder.emitIntrinsicInst(uintType, kIROp_StructuredBufferLoad, 2, loadArgs);

            // Get binding index based on resource type (VkMutable bindings)
            int bindingIndex = getBindingIndexForResourceType(resourceType);

            // Determine how to handle this resource type
            auto cbufferType = as<IRConstantBufferType>(resourceType);
            auto paramBlockType = as<IRParameterBlockType>(resourceType);
            auto structuredBufferType = as<IRHLSLStructuredBufferTypeBase>(resourceType);

            IRType* heapElementType = resourceType;
            bool isUniformBuffer = false;
            bool isStructuredBuffer = false;
            LoweredStructuredBufferTypeInfo sbInfo = {};

            if (cbufferType || paramBlockType)
            {
                // For ConstantBuffer<T> and ParameterBlock<T>:
                // Create a heap of the inner element type T
                heapElementType = as<IRUniformParameterGroupType>(resourceType)->getElementType();
                isUniformBuffer = true;
            }
            else if (structuredBufferType)
            {
                // For StructuredBuffer<T> and RWStructuredBuffer<T>:
                // Create a wrapper struct containing T[] and use that as the heap element type
                sbInfo = getOrCreateStructuredBufferWrapperType(structuredBufferType);
                heapElementType = sbInfo.wrapperStructType;
                isStructuredBuffer = true;
            }

            // Get or create the resource heap at module scope
            auto resourceHeap = getOrCreateResourceHeap(bindingIndex, heapElementType);

            // For SPIRV, unbounded arrays need pointer-based access:
            // 1. Get a pointer to the element (OpAccessChain)
            // 2. Handle based on resource type
            auto elementPtr = builder.emitElementAddress(resourceHeap, heapIndex);

            if (isStructuredBuffer)
            {
                // For structured buffers, we need to transform the operation that uses this buffer.
                // The user of the global param should be a StructuredBufferLoad/Store/GetElementPtr
                auto userOp = user->getOp();

                if (userOp == kIROp_StructuredBufferLoad ||
                    userOp == kIROp_RWStructuredBufferLoad ||
                    userOp == kIROp_StructuredBufferLoadStatus ||
                    userOp == kIROp_RWStructuredBufferLoadStatus)
                {
                    // StructuredBufferLoad(buffer, index) -> Load(FieldAddress(ElementAddress(heap, heapIdx), arrayKey)[index])
                    auto loadIndex = user->getOperand(1);

                    // Get pointer to the _data array field in the wrapper struct
                    auto arrayFieldPtr = builder.emitFieldAddress(
                        builder.getPtrType(sbInfo.unsizedArrayType, AddressSpace::StorageBuffer),
                        elementPtr,
                        sbInfo.arrayKey);

                    // Get pointer to element at loadIndex
                    auto elementAddr = builder.emitElementAddress(arrayFieldPtr, loadIndex);

                    // Load the element
                    auto loadedValue = builder.emitLoad(structuredBufferType->getElementType(), elementAddr);

                    // Replace the entire StructuredBufferLoad instruction
                    user->replaceUsesWith(loadedValue);
                    user->removeAndDeallocate();
                }
                else if (userOp == kIROp_RWStructuredBufferStore)
                {
                    // RWStructuredBufferStore(buffer, index, value) -> Store(ptr, value)
                    auto storeIndex = user->getOperand(1);
                    auto storeValue = user->getOperand(2);

                    // Get pointer to the _data array field
                    auto arrayFieldPtr = builder.emitFieldAddress(
                        builder.getPtrType(sbInfo.unsizedArrayType, AddressSpace::StorageBuffer),
                        elementPtr,
                        sbInfo.arrayKey);

                    // Get pointer to element at storeIndex
                    auto elementAddr = builder.emitElementAddress(arrayFieldPtr, storeIndex);

                    // Store the value
                    builder.emitStore(elementAddr, storeValue);

                    // Remove the original store instruction
                    user->removeAndDeallocate();
                }
                else if (userOp == kIROp_RWStructuredBufferGetElementPtr)
                {
                    // RWStructuredBufferGetElementPtr(buffer, index) -> ElementAddress(FieldAddress(...), index)
                    auto gepIndex = user->getOperand(1);

                    // Get pointer to the _data array field
                    auto arrayFieldPtr = builder.emitFieldAddress(
                        builder.getPtrType(sbInfo.unsizedArrayType, AddressSpace::StorageBuffer),
                        elementPtr,
                        sbInfo.arrayKey);

                    // Get pointer to element at gepIndex
                    auto elementAddr = builder.emitElementAddress(arrayFieldPtr, gepIndex);

                    // Replace the GetElementPtr instruction
                    user->replaceUsesWith(elementAddr);
                    user->removeAndDeallocate();
                }
                else
                {
                    // Other uses - just provide the element pointer and hope for the best
                    // This might need adjustment for specific cases
                    builder.replaceOperand(use, elementPtr);
                }
            }
            else if (isUniformBuffer)
            {
                // For uniform buffers, the element pointer IS the replacement.
                // The original ConstantBuffer<T> acts like a pointer to T.
                builder.replaceOperand(use, elementPtr);
            }
            else
            {
                // For other resources (textures, samplers), load the value from the pointer
                auto replacement = builder.emitLoad(resourceType, elementPtr);
                builder.replaceOperand(use, replacement);
            }
        }

        // Record for metadata output
        if (outConvertedResources)
        {
            BindlessConvertedResource info;
            info.name = name;
            info.typeName = getResourceTypeName(resourceType);
            info.index = index;
            info.resourceType = getBindlessResourceTypeForIRType(resourceType);
            info.access = getResourceAccess(resourceType);
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
