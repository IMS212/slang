// slang-ir-lower-bindless-resources.cpp
#include "slang-ir-lower-bindless-resources.h"

#include "slang-ir-insts.h"
#include "slang-ir-inline.h"
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

    struct ResourceToConvert
    {
        IRGlobalParam* param = nullptr;
        IRType* resourceType = nullptr;
        int index = -1;
        bool isArrayResource = false;
    };

    IRType* unwrapArrayType(IRType* type)
    {
        while (auto arrayType = as<IRArrayTypeBase>(type))
            type = arrayType->getElementType();
        return type;
    }

    bool isInsideFunction(IRInst* inst)
    {
        auto parent = inst ? inst->getParent() : nullptr;
        while (parent)
        {
            if (as<IRBlock>(parent))
                return true;
            parent = parent->getParent();
        }
        return false;
    }

    bool getDirectArrayResourceInfo(IRType* type, IRType** outElementType, int* outShaderArrayLength)
    {
        auto arrayType = as<IRArrayTypeBase>(type);
        if (!arrayType)
            return false;

        auto elementType = arrayType->getElementType();
        if (!isResourceType(elementType) || as<IRArrayTypeBase>(elementType))
            return false;

        int shaderArrayLength = -1;
        if (auto sizedArrayType = as<IRArrayType>(arrayType))
        {
            if (auto elementCount = as<IRIntLit>(sizedArrayType->getElementCount()))
                shaderArrayLength = (int)elementCount->getValue();
        }

        if (outElementType)
            *outElementType = elementType;
        if (outShaderArrayLength)
            *outShaderArrayLength = shaderArrayLength;
        return true;
    }

    bool isCombinedTextureType(IRType* type)
    {
        if (auto textureType = as<IRTextureType>(type))
            return textureType->isCombined();
        return false;
    }

    IRType* getUncombinedTextureType(IRBuilder& builder, IRType* type)
    {
        auto textureType = as<IRTextureType>(type);
        if (!textureType || !textureType->isCombined())
            return type;

        return builder.getTextureType(
            textureType->getElementType(),
            textureType->getShapeInst(),
            textureType->getIsArrayInst(),
            textureType->getIsMultisampleInst(),
            textureType->getSampleCountInst(),
            textureType->getAccessInst(),
            textureType->getIsShadowInst(),
            builder.getIntValue(builder.getIntType(), 0),
            textureType->getFormatInst());
    }

    bool canRewriteCombinedResourceUses(IRGlobalParam* param)
    {
        for (auto use = param->firstUse; use; use = use->nextUse)
        {
            auto user = use->getUser();
            if (as<IRStructFieldLayoutAttr>(user) || as<IRDecoration>(user))
                continue;
            if (!isInsideFunction(user))
                continue;

            switch (user->getOp())
            {
            case kIROp_CombinedTextureSamplerGetTexture:
            case kIROp_CombinedTextureSamplerGetSampler:
            case kIROp_Sample:
            case kIROp_SampleGrad:
            case kIROp_Call:
            case kIROp_SPIRVAsmOperandInst:
                break;
            default:
                return false;
            }
        }
        return true;
    }

    bool canRewriteCombinedArrayResourceUses(IRGlobalParam* param)
    {
        for (auto use = param->firstUse; use; use = use->nextUse)
        {
            auto user = use->getUser();
            if (as<IRStructFieldLayoutAttr>(user) || as<IRDecoration>(user))
                continue;
            if (!isInsideFunction(user))
                continue;

            auto getElementInst = as<IRGetElement>(user);
            if (!getElementInst || getElementInst->getOperand(0) != param)
                return false;

            for (auto elementUse = getElementInst->firstUse; elementUse; elementUse = elementUse->nextUse)
            {
                auto elementUser = elementUse->getUser();
                if (as<IRStructFieldLayoutAttr>(elementUser) || as<IRDecoration>(elementUser))
                    continue;
                if (!isInsideFunction(elementUser))
                    continue;

                switch (elementUser->getOp())
                {
                case kIROp_CombinedTextureSamplerGetTexture:
                case kIROp_CombinedTextureSamplerGetSampler:
                case kIROp_Sample:
                case kIROp_SampleGrad:
                case kIROp_Call:
                case kIROp_SPIRVAsmOperandInst:
                    break;
                default:
                    return false;
                }
            }
        }
        return true;
    }

    bool tryInlineCombinedScalarResourceCallUses(IRGlobalParam* param)
    {
        bool changed = false;
        bool progress = false;
        do
        {
            progress = false;

            List<IRUse*> uses;
            for (auto use = param->firstUse; use; use = use->nextUse)
                uses.add(use);

            for (auto use : uses)
            {
                auto user = use->getUser();
                if (as<IRStructFieldLayoutAttr>(user) || as<IRDecoration>(user))
                    continue;
                if (!isInsideFunction(user))
                    continue;
                if (auto call = as<IRCall>(user))
                {
                    if (inlineCall(call))
                    {
                        changed = true;
                        progress = true;
                        break;
                    }
                }
            }
        } while (progress);

        return changed;
    }

    bool tryInlineCombinedArrayResourceCallUses(IRGlobalParam* param)
    {
        bool changed = false;
        bool progress = false;
        do
        {
            progress = false;

            List<IRUse*> paramUses;
            for (auto use = param->firstUse; use; use = use->nextUse)
                paramUses.add(use);

            for (auto paramUse : paramUses)
            {
                auto getElementInst = as<IRGetElement>(paramUse->getUser());
                if (!getElementInst || getElementInst->getOperand(0) != param)
                    continue;
                if (!isInsideFunction(getElementInst))
                    continue;

                List<IRUse*> elementUses;
                for (auto elementUse = getElementInst->firstUse; elementUse; elementUse = elementUse->nextUse)
                    elementUses.add(elementUse);

                for (auto elementUse : elementUses)
                {
                    auto elementUser = elementUse->getUser();
                    if (as<IRStructFieldLayoutAttr>(elementUser) || as<IRDecoration>(elementUser))
                        continue;
                    if (!isInsideFunction(elementUser))
                        continue;
                    if (auto call = as<IRCall>(elementUser))
                    {
                        if (inlineCall(call))
                        {
                            changed = true;
                            progress = true;
                            break;
                        }
                    }
                }

                if (progress)
                    break;
            }
        } while (progress);

        return changed;
    }

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

    void removeConflictingStructLayoutDecorations(IRStructType* elementStruct, IRTypeLayoutRules* layoutRules)
    {
        // Ensure the emitter sees layout data for the intended rule first.
        List<IRDecoration*> decorationsToRemove;
        for (auto decor : elementStruct->getDecorations())
        {
            if (auto sizeAlignDecor = as<IRSizeAndAlignmentDecoration>(decor))
            {
                if (sizeAlignDecor->getLayoutName() != layoutRules->ruleName)
                    decorationsToRemove.add(decor);
            }
        }
        for (auto decor : decorationsToRemove)
            decor->removeAndDeallocate();

        for (auto field : elementStruct->getFields())
        {
            List<IRDecoration*> fieldDecorsToRemove;
            for (auto decor : field->getDecorations())
            {
                if (auto offsetDecor = as<IROffsetDecoration>(decor))
                {
                    if (offsetDecor->getLayoutName() != layoutRules->ruleName)
                        fieldDecorsToRemove.add(decor);
                }
            }
            for (auto decor : fieldDecorsToRemove)
                decor->removeAndDeallocate();
        }
    }

    void ensureUniformBufferElementLayoutForSPIRVBlock(IRType* bufferType, IRType* elementType)
    {
        auto elementStruct = as<IRStructType>(elementType);
        if (!elementStruct)
            return;

        IRBuilder moduleBuilder(module);
        moduleBuilder.setInsertInto(module->getModuleInst());

        auto layoutRules = getTypeLayoutRuleForBuffer(targetProgram, bufferType);
        removeConflictingStructLayoutDecorations(elementStruct, layoutRules);

        IRSizeAndAlignment elementSize;
        getSizeAndAlignment(targetProgram->getTargetReq(), layoutRules, elementType, &elementSize);

        moduleBuilder.addPhysicalTypeDecoration(elementStruct);
        moduleBuilder.addDecorationIfNotExist(elementStruct, kIROp_SPIRVBlockDecoration);
    }

    // Try to find a scalar resource name in the index map.
    // First tries exact match, then tries suffix match for hoisted struct members.
    int findIndexForName(const String& name, const Dictionary<String, int>& indexMap)
    {
        if (auto* index = indexMap.tryGetValue(name))
            return *index;

        for (const auto& [key, value] : indexMap)
        {
            if (key.endsWith("[]"))
                continue;

            String suffix = "." + key;
            if (name.endsWith(suffix))
                return value;
        }

        return -1;
    }

    // Try to find an array resource name in the index map.
    // Array keys are represented as "name[]".
    int findArrayIndexForName(const String& name, const Dictionary<String, int>& indexMap)
    {
        String arrayName = name + "[]";
        if (auto* index = indexMap.tryGetValue(arrayName))
            return *index;

        for (const auto& [key, value] : indexMap)
        {
            if (!key.endsWith("[]"))
                continue;

            String baseKey = key.subString(0, key.getLength() - 2);
            String suffix = "." + baseKey;
            if (name.endsWith(suffix))
                return value;
        }

        return -1;
    }

    // Map IR type to BindlessResourceType enum for resolver callbacks.
    BindlessResourceType getBindlessResourceTypeForIRType(IRType* type)
    {
        type = unwrapArrayType(type);
        switch (type->getOp())
        {
        case kIROp_SamplerStateType:
        case kIROp_SamplerComparisonStateType:
            return BindlessResourceType::Sampler;

        case kIROp_TextureType:
        {
            auto textureType = as<IRTextureType>(type);
            if (textureType && textureType->isCombined())
                return BindlessResourceType::SampledImage;
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
        type = unwrapArrayType(type);
        switch (type->getOp())
        {
        case kIROp_HLSLRWStructuredBufferType:
        case kIROp_HLSLRWByteAddressBufferType:
            return SLANG_RESOURCE_ACCESS_READ_WRITE;

        case kIROp_HLSLRasterizerOrderedStructuredBufferType:
        case kIROp_HLSLRasterizerOrderedByteAddressBufferType:
            return SLANG_RESOURCE_ACCESS_RASTER_ORDERED;

        case kIROp_HLSLAppendStructuredBufferType:
            return SLANG_RESOURCE_ACCESS_APPEND;
        case kIROp_HLSLConsumeStructuredBufferType:
            return SLANG_RESOURCE_ACCESS_CONSUME;

        case kIROp_TextureType:
        {
            auto textureType = as<IRTextureType>(type);
            if (textureType)
                return textureType->getAccess();
            return SLANG_RESOURCE_ACCESS_READ;
        }

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
    int getBindingIndexForResourceType(IRType* type)
    {
        type = unwrapArrayType(type);
        switch (type->getOp())
        {
        case kIROp_SamplerStateType:
        case kIROp_SamplerComparisonStateType:
            return 0;

        case kIROp_TextureType:
        {
            auto textureType = as<IRTextureType>(type);
            if (textureType && textureType->isCombined())
                return 2;
            if (textureType && textureType->getAccess() == SLANG_RESOURCE_ACCESS_READ_WRITE)
                return 3;
            return 2;
        }

        case kIROp_ConstantBufferType:
        case kIROp_ParameterBlockType:
            return 4;

        case kIROp_HLSLStructuredBufferType:
        case kIROp_HLSLRWStructuredBufferType:
        case kIROp_HLSLByteAddressBufferType:
        case kIROp_HLSLRWByteAddressBufferType:
        case kIROp_HLSLAppendStructuredBufferType:
        case kIROp_HLSLConsumeStructuredBufferType:
        case kIROp_HLSLRasterizerOrderedStructuredBufferType:
        case kIROp_HLSLRasterizerOrderedByteAddressBufferType:
            return 5;

        default:
            return 5;
        }
    }

    int resolveBindlessIndex(const String& name, IRType* resourceType)
    {
        auto& indexMap = targetProgram->m_bindlessResourceIndexMap;
        auto resolver = targetProgram->m_bindlessResolver;
        auto cache = targetProgram->m_bindlessResolverCache;
        auto userData = targetProgram->m_bindlessResolverUserData;

        int staticIndex = findIndexForName(name, indexMap);
        if (staticIndex >= 0)
            return staticIndex;

        if (!resolver)
            return -1;

        BindlessResourceType bindlessType = getBindlessResourceTypeForIRType(resourceType);
        String cacheKey = makeCacheKey(name, bindlessType);
        if (cache)
        {
            if (auto* cachedIndex = cache->tryGetValue(cacheKey))
                return *cachedIndex;
        }

        slang::SlangBindlessResourceType publicType =
            static_cast<slang::SlangBindlessResourceType>(bindlessType);
        int resolvedIndex = resolver(name.getBuffer(), publicType, userData);
        if (cache && resolvedIndex >= 0)
            cache->add(cacheKey, resolvedIndex);
        return resolvedIndex;
    }

    bool hasScalarBindingForArrayName(const String& name, IRType* resourceType)
    {
        return resolveBindlessIndex(name, resourceType) >= 0;
    }

    int resolveBindlessArrayBaseIndex(
        const String& name,
        IRType* resourceType,
        int shaderArrayLength,
        int* outResolvedArrayLength)
    {
        if (outResolvedArrayLength)
            *outResolvedArrayLength = -1;

        auto& indexMap = targetProgram->m_bindlessResourceIndexMap;
        int staticIndex = findArrayIndexForName(name, indexMap);
        if (staticIndex >= 0)
        {
            if (outResolvedArrayLength)
                *outResolvedArrayLength = shaderArrayLength;
            return staticIndex;
        }

        auto resolver = targetProgram->m_bindlessArrayResolver;
        auto userData = targetProgram->m_bindlessArrayResolverUserData;
        if (!resolver)
            return -1;

        BindlessResourceType bindlessType = getBindlessResourceTypeForIRType(resourceType);
        slang::SlangBindlessResourceType publicType =
            static_cast<slang::SlangBindlessResourceType>(bindlessType);
        int resolvedArrayLength = -1;
        int baseIndex =
            resolver(name.getBuffer(), publicType, shaderArrayLength, &resolvedArrayLength, userData);
        if (outResolvedArrayLength)
            *outResolvedArrayLength = resolvedArrayLength;
        return baseIndex;
    }

    int resolveCombinedSamplerIndex(const String& name)
    {
        auto resolver = targetProgram->m_bindlessCombinedSamplerResolver;
        auto userData = targetProgram->m_bindlessCombinedSamplerResolverUserData;
        if (!resolver)
            return 0;

        int resolvedIndex = resolver(name.getBuffer(), userData);
        return resolvedIndex >= 0 ? resolvedIndex : 0;
    }

    void processModule()
    {
        auto& indexMap = targetProgram->m_bindlessResourceIndexMap;
        auto resolver = targetProgram->m_bindlessResolver;
        auto arrayResolver = targetProgram->m_bindlessArrayResolver;
        if (indexMap.getCount() == 0 && !resolver && !arrayResolver)
            return;

        List<ResourceToConvert> resourcesToConvert;
        List<IRGlobalParam*> unmappedResources;
        List<IRGlobalParam*> arrayResourcesWithScalarConflict;

        for (auto inst : module->getGlobalInsts())
        {
            auto globalParam = as<IRGlobalParam>(inst);
            if (!globalParam)
                continue;
            if (globalParam->findDecoration<IRHasExplicitVulkanBindingDecoration>())
                continue;

            auto paramType = globalParam->getDataType();
            if (!isResourceType(paramType) || !globalParam->hasUses())
                continue;

            auto nameHint = globalParam->findDecoration<IRNameHintDecoration>();
            if (!nameHint)
                continue;

            String name = nameHint->getName();

            IRType* arrayElementType = nullptr;
            int shaderArrayLength = -1;
            if (getDirectArrayResourceInfo(paramType, &arrayElementType, &shaderArrayLength))
            {
                int resolvedArrayLength = -1;
                int baseIndex = resolveBindlessArrayBaseIndex(
                    name,
                    arrayElementType,
                    shaderArrayLength,
                    &resolvedArrayLength);

                if (baseIndex >= 0)
                {
                    if (isCombinedTextureType(arrayElementType) &&
                        isSPIRV(targetProgram->getTargetReq()->getTarget()))
                    {
                        tryInlineCombinedArrayResourceCallUses(globalParam);
                    }

                    if (isCombinedTextureType(arrayElementType) &&
                        !canRewriteCombinedArrayResourceUses(globalParam))
                    {
                        continue;
                    }

                    if (shaderArrayLength >= 0 &&
                        resolvedArrayLength >= 0 &&
                        shaderArrayLength != resolvedArrayLength)
                    {
                        if (sink)
                        {
                            sink->diagnose(
                                globalParam->sourceLoc,
                                Diagnostics::bindlessArrayLengthMismatch,
                                name,
                                resolvedArrayLength,
                                shaderArrayLength);
                        }
                        continue;
                    }

                    ResourceToConvert info;
                    info.param = globalParam;
                    info.resourceType = arrayElementType;
                    info.index = baseIndex;
                    info.isArrayResource = true;
                    resourcesToConvert.add(info);
                }
                else
                {
                    if (hasScalarBindingForArrayName(name, arrayElementType))
                        arrayResourcesWithScalarConflict.add(globalParam);
                    else
                        unmappedResources.add(globalParam);
                }
                continue;
            }

            // Only direct arrays of resource objects are lowered through the array resolver path.
            // More complex array nesting is currently left unmapped.
            if (as<IRArrayTypeBase>(paramType))
            {
                unmappedResources.add(globalParam);
                continue;
            }

            int index = resolveBindlessIndex(name, paramType);
            if (index >= 0)
            {
                if (isCombinedTextureType(paramType) &&
                    isSPIRV(targetProgram->getTargetReq()->getTarget()))
                {
                    tryInlineCombinedScalarResourceCallUses(globalParam);
                }

                if (isCombinedTextureType(paramType) && !canRewriteCombinedResourceUses(globalParam))
                {
                    // Leave this resource unchanged if it has unsupported combined-type use patterns.
                    continue;
                }

                ResourceToConvert info;
                info.param = globalParam;
                info.resourceType = paramType;
                info.index = index;
                info.isArrayResource = false;
                resourcesToConvert.add(info);
            }
            else
            {
                unmappedResources.add(globalParam);
            }
        }

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

        for (auto param : arrayResourcesWithScalarConflict)
        {
            auto nameHint = param->findDecoration<IRNameHintDecoration>();
            if (nameHint && sink)
            {
                sink->diagnose(
                    param->sourceLoc,
                    Diagnostics::arrayResourceConflictsWithScalarBindlessBinding,
                    nameHint->getName());
            }
        }

        if (resourcesToConvert.getCount() == 0)
            return;

        for (const auto& resource : resourcesToConvert)
        {
            if (resource.isArrayResource)
                convertArrayResourceWithResolver(resource.param, resource.resourceType, resource.index);
            else
                convertResourceWithResolver(resource.param, resource.resourceType, resource.index);
        }
    }

    IRInst* stripNonUniformIndexWrappers(IRInst* userIndex, bool& hasNonUniform)
    {
        hasNonUniform = false;
        while (userIndex->getOp() == kIROp_NonUniformResourceIndex)
        {
            hasNonUniform = true;
            userIndex = userIndex->getOperand(0);
        }
        return userIndex;
    }

    void removeDeadNonUniformIndexWrappers(IRInst* userIndex)
    {
        while (userIndex && userIndex->getOp() == kIROp_NonUniformResourceIndex)
        {
            auto next = userIndex->getOperand(0);
            if (userIndex->hasUses())
                break;
            userIndex->removeAndDeallocate();
            userIndex = next;
        }
    }

    IRInst* buildArrayedDescriptorIndex(IRBuilder& builder, int baseIndex, IRInst* userIndex)
    {

        IRType* indexType = userIndex->getDataType();
        auto basicType = as<IRBasicType>(indexType);
        if (!basicType || (basicType->getBaseType() != BaseType::Int && basicType->getBaseType() != BaseType::UInt))
        {
            indexType = builder.getBasicType(BaseType::Int);
            userIndex = builder.emitCast(indexType, userIndex);
        }

        auto baseLiteral = builder.getIntValue(indexType, baseIndex);
        return builder.emitAdd(indexType, baseLiteral, userIndex);
    }

    void rewriteResourceUseWithHeapIndex(
        IRBuilder& builder,
        const String& resourceName,
        IRType* resourceType,
        IRUse* use,
        IRInst* heapIndex,
        bool nonUniformHeapIndex = false)
    {
        auto user = use->getUser();
        auto combinedTextureType = as<IRTextureType>(resourceType);
        bool isCombinedTextureResource = combinedTextureType && combinedTextureType->isCombined();
        IRType* bindlessLookupType = resourceType;
        if (isCombinedTextureResource)
            bindlessLookupType = getUncombinedTextureType(builder, resourceType);

        int bindingIndex = getBindingIndexForResourceType(bindlessLookupType);
        bool useSPIRVNonUniformDecoration =
            isSPIRV(targetProgram->getTargetReq()->getTarget());

        auto cbufferType = as<IRConstantBufferType>(bindlessLookupType);
        auto paramBlockType = as<IRParameterBlockType>(bindlessLookupType);
        auto structuredBufferType = as<IRHLSLStructuredBufferTypeBase>(bindlessLookupType);

        IRType* heapElementType = bindlessLookupType;
        bool isUniformBuffer = false;
        bool isStructuredBuffer = false;
        LoweredStructuredBufferTypeInfo sbInfo = {};

        if (cbufferType || paramBlockType)
        {
            heapElementType = as<IRUniformParameterGroupType>(resourceType)->getElementType();
            isUniformBuffer = true;
            ensureUniformBufferElementLayoutForSPIRVBlock(resourceType, heapElementType);
        }
        else if (structuredBufferType)
        {
            sbInfo = getOrCreateStructuredBufferWrapperType(structuredBufferType);
            heapElementType = sbInfo.wrapperStructType;
            isStructuredBuffer = true;
        }

        IRInst* resourceHeap = getOrCreateResourceHeap(bindingIndex, heapElementType);

        if (isCombinedTextureResource)
        {
            auto samplerType = getIntVal(combinedTextureType->getIsShadowInst()) != 0
                                   ? builder.getType(kIROp_SamplerComparisonStateType)
                                   : builder.getType(kIROp_SamplerStateType);
            int fixedSamplerIndex = resolveCombinedSamplerIndex(resourceName);
            auto target = targetProgram->getTargetReq()->getTarget();
            bool useSamplerHeapIntrinsic = !(
                target == CodeGenTarget::SPIRV || target == CodeGenTarget::SPIRVAssembly ||
                target == CodeGenTarget::GLSL);

            switch (user->getOp())
            {
            case kIROp_Call:
            case kIROp_SPIRVAsmOperandInst:
                {
                    if (!useSamplerHeapIntrinsic)
                    {
                        // Ensure a sampler heap global exists for emitter-side reconstruction.
                        auto samplerHeap = getOrCreateResourceHeap(0, samplerType);
                        builder.addKeepAliveDecoration(samplerHeap);
                    }

                    auto uintType = builder.getUIntType();
                    IRInst* textureIndex = heapIndex;
                    if (textureIndex->getDataType() != uintType)
                        textureIndex = builder.emitCast(uintType, textureIndex);
                    if (nonUniformHeapIndex && !useSPIRVNonUniformDecoration)
                        textureIndex = builder.emitNonUniformResourceIndexInst(textureIndex);

                    auto samplerIndex = builder.getIntValue(uintType, fixedSamplerIndex);
                    auto uint2Type = builder.getVectorType(uintType, 2);
                    IRInst* handleComps[2] = {textureIndex, samplerIndex};
                    auto packedHandle = builder.emitMakeVector(uint2Type, 2, handleComps);
                    IRInst* combinedVal = builder.emitIntrinsicInst(
                        resourceType,
                        kIROp_MakeCombinedTextureSamplerFromHandle,
                        1,
                        &packedHandle);
                    if (nonUniformHeapIndex && useSPIRVNonUniformDecoration)
                        builder.addSPIRVNonUniformResourceDecoration(combinedVal);
                    builder.replaceOperand(use, combinedVal);
                    return;
                }
            case kIROp_CombinedTextureSamplerGetTexture:
            case kIROp_CombinedTextureSamplerGetSampler:
            case kIROp_Sample:
            case kIROp_SampleGrad:
                break;
            default:
                return;
            }

            if (nonUniformHeapIndex && !useSPIRVNonUniformDecoration)
                heapIndex = builder.emitNonUniformResourceIndexInst(heapIndex);
            auto elementPtr = builder.emitElementAddress(resourceHeap, heapIndex);
            if (nonUniformHeapIndex && useSPIRVNonUniformDecoration)
                builder.addSPIRVNonUniformResourceDecoration(elementPtr);

            auto textureVal = builder.emitLoad(bindlessLookupType, elementPtr);
            if (nonUniformHeapIndex && useSPIRVNonUniformDecoration)
                builder.addSPIRVNonUniformResourceDecoration(textureVal);

            auto samplerIndexLiteral =
                builder.getIntValue(builder.getBasicType(BaseType::Int), fixedSamplerIndex);
            IRInst* samplerVal = nullptr;
            if (useSamplerHeapIntrinsic)
            {
                samplerVal = builder.emitIntrinsicInst(
                    samplerType,
                    kIROp_LoadSamplerDescriptorFromHeap,
                    1,
                    &samplerIndexLiteral);
            }
            else
            {
                auto samplerHeap = getOrCreateResourceHeap(0, samplerType);
                auto samplerPtr = builder.emitElementAddress(samplerHeap, samplerIndexLiteral);
                samplerVal = builder.emitLoad(samplerType, samplerPtr);
            }

            switch (user->getOp())
            {
            case kIROp_CombinedTextureSamplerGetTexture:
                user->replaceUsesWith(textureVal);
                user->removeAndDeallocate();
                return;
            case kIROp_CombinedTextureSamplerGetSampler:
                user->replaceUsesWith(samplerVal);
                user->removeAndDeallocate();
                return;
            case kIROp_Sample:
            case kIROp_SampleGrad:
                if (use == user->getOperandUse(0))
                    builder.replaceOperand(use, textureVal);
                else if (use == user->getOperandUse(1))
                    builder.replaceOperand(use, samplerVal);
                return;
            default:
                return;
            }
        }

        if (nonUniformHeapIndex && !useSPIRVNonUniformDecoration)
            heapIndex = builder.emitNonUniformResourceIndexInst(heapIndex);
        auto elementPtr = builder.emitElementAddress(resourceHeap, heapIndex);
        if (nonUniformHeapIndex && useSPIRVNonUniformDecoration)
            builder.addSPIRVNonUniformResourceDecoration(elementPtr);

        if (isStructuredBuffer)
        {
            auto userOp = user->getOp();

            if (userOp == kIROp_StructuredBufferLoad ||
                userOp == kIROp_RWStructuredBufferLoad ||
                userOp == kIROp_StructuredBufferLoadStatus ||
                userOp == kIROp_RWStructuredBufferLoadStatus)
            {
                auto loadIndex = user->getOperand(1);
                auto arrayFieldPtr = builder.emitFieldAddress(
                    builder.getPtrType(sbInfo.unsizedArrayType, AddressSpace::StorageBuffer),
                    elementPtr,
                    sbInfo.arrayKey);
                auto elementAddr = builder.emitElementAddress(arrayFieldPtr, loadIndex);
                auto loadedValue = builder.emitLoad(structuredBufferType->getElementType(), elementAddr);
                user->replaceUsesWith(loadedValue);
                user->removeAndDeallocate();
            }
            else if (userOp == kIROp_RWStructuredBufferStore)
            {
                auto storeIndex = user->getOperand(1);
                auto storeValue = user->getOperand(2);
                auto arrayFieldPtr = builder.emitFieldAddress(
                    builder.getPtrType(sbInfo.unsizedArrayType, AddressSpace::StorageBuffer),
                    elementPtr,
                    sbInfo.arrayKey);
                auto elementAddr = builder.emitElementAddress(arrayFieldPtr, storeIndex);
                builder.emitStore(elementAddr, storeValue);
                user->removeAndDeallocate();
            }
            else if (userOp == kIROp_RWStructuredBufferGetElementPtr)
            {
                auto gepIndex = user->getOperand(1);
                auto arrayFieldPtr = builder.emitFieldAddress(
                    builder.getPtrType(sbInfo.unsizedArrayType, AddressSpace::StorageBuffer),
                    elementPtr,
                    sbInfo.arrayKey);
                auto elementAddr = builder.emitElementAddress(arrayFieldPtr, gepIndex);
                user->replaceUsesWith(elementAddr);
                user->removeAndDeallocate();
            }
            else
            {
                builder.replaceOperand(use, elementPtr);
            }
        }
        else if (isUniformBuffer)
        {
            builder.replaceOperand(use, elementPtr);
        }
        else
        {
            auto replacement = builder.emitLoad(bindlessLookupType, elementPtr);
            if (nonUniformHeapIndex && useSPIRVNonUniformDecoration)
                builder.addSPIRVNonUniformResourceDecoration(replacement);
            builder.replaceOperand(use, replacement);
        }
    }

    void convertResourceWithResolver(IRGlobalParam* param, IRType* resourceType, int index)
    {
        auto nameHint = param->findDecoration<IRNameHintDecoration>();
        String name = nameHint->getName();
        IRBuilder builder(module);

        List<IRUse*> uses;
        for (auto use = param->firstUse; use; use = use->nextUse)
            uses.add(use);

        for (auto use : uses)
        {
            auto user = use->getUser();
            if (as<IRStructFieldLayoutAttr>(user) || as<IRDecoration>(user))
                continue;
            if (!isInsideFunction(user))
                continue;

            builder.setInsertBefore(user);
            auto indexLiteral = builder.getIntValue(builder.getBasicType(BaseType::Int), index);
            rewriteResourceUseWithHeapIndex(builder, name, resourceType, use, indexLiteral);
        }

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

        if (!param->hasUses())
            param->removeAndDeallocate();
    }

    void convertArrayResourceWithResolver(IRGlobalParam* param, IRType* resourceType, int baseIndex)
    {
        auto nameHint = param->findDecoration<IRNameHintDecoration>();
        String name = nameHint->getName();
        IRBuilder builder(module);

        List<IRUse*> paramUses;
        for (auto use = param->firstUse; use; use = use->nextUse)
            paramUses.add(use);

        for (auto paramUse : paramUses)
        {
            auto getElementInst = as<IRGetElement>(paramUse->getUser());
            if (!getElementInst || getElementInst->getOperand(0) != param)
                continue;
            if (!isInsideFunction(getElementInst))
                continue;

            auto originalUserIndex = getElementInst->getOperand(1);
            bool hasNonUniformUserIndex = false;
            auto userIndex = stripNonUniformIndexWrappers(
                originalUserIndex,
                hasNonUniformUserIndex);

            List<IRUse*> elementUses;
            for (auto use = getElementInst->firstUse; use; use = use->nextUse)
                elementUses.add(use);

            for (auto elementUse : elementUses)
            {
                auto elementUser = elementUse->getUser();
                if (as<IRStructFieldLayoutAttr>(elementUser) || as<IRDecoration>(elementUser))
                    continue;
                if (!isInsideFunction(elementUser))
                    continue;

                builder.setInsertBefore(elementUser);
                auto effectiveIndex = buildArrayedDescriptorIndex(builder, baseIndex, userIndex);
                rewriteResourceUseWithHeapIndex(
                    builder,
                    name,
                    resourceType,
                    elementUse,
                    effectiveIndex,
                    hasNonUniformUserIndex);
            }

            if (!getElementInst->hasUses())
                getElementInst->removeAndDeallocate();
            removeDeadNonUniformIndexWrappers(originalUserIndex);
        }

        if (outConvertedResources)
        {
            BindlessConvertedResource info;
            info.name = name;
            info.typeName = getResourceTypeName(param->getDataType());
            info.index = baseIndex;
            info.resourceType = getBindlessResourceTypeForIRType(resourceType);
            info.access = getResourceAccess(resourceType);
            outConvertedResources->add(info);
        }

        if (!param->hasUses())
            param->removeAndDeallocate();
    }

    String getResourceTypeName(IRType* type)
    {
        int arrayDepth = 0;
        while (auto arrayType = as<IRArrayTypeBase>(type))
        {
            arrayDepth++;
            type = arrayType->getElementType();
        }

        String baseName;
        // Get a human-readable name for the resource type
        switch (type->getOp())
        {
        case kIROp_HLSLStructuredBufferType:
            baseName = "StructuredBuffer";
            break;
        case kIROp_HLSLRWStructuredBufferType:
            baseName = "RWStructuredBuffer";
            break;
        case kIROp_HLSLByteAddressBufferType:
            baseName = "ByteAddressBuffer";
            break;
        case kIROp_HLSLRWByteAddressBufferType:
            baseName = "RWByteAddressBuffer";
            break;
        case kIROp_SamplerStateType:
            baseName = "SamplerState";
            break;
        case kIROp_SamplerComparisonStateType:
            baseName = "SamplerComparisonState";
            break;
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
                baseName = sb.produceString();
                break;
            }
            baseName = "Texture";
            break;
        }
        default:
            baseName = "Resource";
            break;
        }

        if (arrayDepth == 0)
            return baseName;

        StringBuilder sb;
        sb << baseName;
        for (int i = 0; i < arrayDepth; ++i)
            sb << "[]";
        return sb.produceString();
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
