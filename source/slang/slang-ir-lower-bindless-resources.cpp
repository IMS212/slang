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

#include <climits>

namespace Slang
{

static const int kBindlessIndexBufferSet = 2;
static const int kBindlessIndexBufferBinding = 0;
static const int kBindlessIndexBufferElementCount = 200;

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
    IRGlobalParam* indexBuffer = nullptr;
    IRArrayTypeBase* indexBufferArrayType = nullptr;
    IRStructKey* indexBufferArrayKey = nullptr;
    IRType* indexBufferDataLayoutType = nullptr;
    int nextIndexBufferSlot = 0;

    struct ResourceToConvert
    {
        IRGlobalParam* param = nullptr;
        IRType* resourceType = nullptr;
        int index = -1;
        int bindingCount = 1;
        int samplerIndex = -1;
        int samplerBindingCount = 0;
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

    bool tryRemoveConvertedGlobalParam(IRGlobalParam* param)
    {
        List<IRInst*> removableUsers;
        for (auto use = param->firstUse; use; use = use->nextUse)
        {
            auto user = use->getUser();
            if (user->getOp() == kIROp_Load && !user->hasUses())
            {
                removableUsers.add(user);
                continue;
            }

            if (as<IRDecoration>(user) || as<IRStructFieldLayoutAttr>(user))
            {
                removableUsers.add(user);
                continue;
            }

            return false;
        }

        for (auto user : removableUsers)
        {
            if (user->getParent())
                user->removeAndDeallocate();
        }

        if (!param->hasUses())
        {
            param->removeAndDeallocate();
            return true;
        }

        return false;
    }

    bool hasPushConstantLayout(IRGlobalParam* globalParam)
    {
        if (auto layoutDecor = globalParam->findDecoration<IRLayoutDecoration>())
        {
            if (auto varLayout = as<IRVarLayout>(layoutDecor->getLayout()))
            {
                if (varLayout->findOffsetAttr(LayoutResourceKind::PushConstantBuffer))
                    return true;
            }
        }
        return false;
    }

    IRSPIRVAsm* findParentSPIRVAsm(IRInst* inst)
    {
        auto parent = inst ? inst->getParent() : nullptr;
        while (parent)
        {
            if (auto asmBlock = as<IRSPIRVAsm>(parent))
                return asmBlock;
            parent = parent->getParent();
        }
        return nullptr;
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

    void getArrayResourceInfo(IRType* type, bool& outIsArray, int& outArraySize)
    {
        outIsArray = false;
        outArraySize = 0;

        long long totalSize = 1;
        while (auto arrayType = as<IRArrayTypeBase>(type))
        {
            outIsArray = true;

            if (auto sizedArrayType = as<IRArrayType>(arrayType))
            {
                auto elementCount = as<IRIntLit>(sizedArrayType->getElementCount());
                if (!elementCount || elementCount->getValue() < 0)
                {
                    outArraySize = -1;
                    return;
                }

                totalSize *= elementCount->getValue();
                if (totalSize > INT_MAX)
                {
                    outArraySize = -1;
                    return;
                }
            }
            else
            {
                outArraySize = -1;
                return;
            }

            type = arrayType->getElementType();
        }

        if (outIsArray)
            outArraySize = (int)totalSize;
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

    uint64_t makeResourceHeapKey(int bindingIndex, IRType* elementType, IRType* dataLayoutType)
    {
        // Combine binding index, element type, and layout type for unique key.
        auto typeHash = (uint64_t)(uintptr_t)elementType;
        auto layoutHash = (uint64_t)(uintptr_t)dataLayoutType;
        return ((uint64_t)bindingIndex << 56) ^ ((typeHash << 8) | (typeHash >> 56)) ^ layoutHash;
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

    IRVarLayout* createIndexBufferLayout(IRBuilder& builder)
    {
        IRTypeLayout::Builder typeLayoutBuilder(&builder);
        typeLayoutBuilder.addResourceUsage(LayoutResourceKind::ConstantBuffer, 1);
        auto typeLayout = typeLayoutBuilder.build();
        IRVarLayout::Builder varLayoutBuilder(&builder, typeLayout);
        varLayoutBuilder.findOrAddResourceInfo(LayoutResourceKind::RegisterSpace)->offset =
            kBindlessIndexBufferSet;
        varLayoutBuilder.findOrAddResourceInfo(LayoutResourceKind::ConstantBuffer)->offset =
            kBindlessIndexBufferBinding;
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
    IRGlobalParam* getOrCreateResourceHeap(
        int bindingIndex,
        IRType* elementType,
        IRType* dataLayoutType = nullptr)
    {
        IRBuilder moduleBuilder(module);
        if (!dataLayoutType)
            dataLayoutType = moduleBuilder.getDefaultBufferLayoutType();

        auto key = makeResourceHeapKey(bindingIndex, elementType, dataLayoutType);
        if (auto* existing = resourceHeaps.tryGetValue(key))
            return *existing;

        // Create a GlobalParam at module scope - this becomes OpVariable in SPIR-V
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
            addrSpace,
            dataLayoutType);

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

    IRGlobalParam* getOrCreateIndexBuffer()
    {
        if (indexBuffer)
            return indexBuffer;

        IRBuilder moduleBuilder(module);
        moduleBuilder.setInsertInto(module->getModuleInst());

        auto uintType = moduleBuilder.getUIntType();
        auto intType = moduleBuilder.getIntType();
        auto elementCount = moduleBuilder.getIntValue(intType, kBindlessIndexBufferElementCount);
        auto elementStride = moduleBuilder.getIntValue(intType, 4);
        indexBufferArrayType = moduleBuilder.getArrayTypeBase(
            kIROp_ArrayType,
            uintType,
            elementCount,
            elementStride);

        indexBufferDataLayoutType = moduleBuilder.getScalarBufferLayoutType();

        auto indexBufferStructType = moduleBuilder.createStructType();
        indexBufferArrayKey = moduleBuilder.createStructKey();
        moduleBuilder.createStructField(
            indexBufferStructType,
            indexBufferArrayKey,
            indexBufferArrayType);
        moduleBuilder.addNameHintDecoration(
            indexBufferStructType,
            toSlice("__slang_bindless_index_buffer_t"));

        auto constantBufferType =
            moduleBuilder.getConstantBufferType(indexBufferStructType, indexBufferDataLayoutType);
        auto layoutRules = getTypeLayoutRuleForBuffer(targetProgram, constantBufferType);
        IRSizeAndAlignment indexBufferSize;
        getSizeAndAlignment(
            targetProgram->getTargetReq(),
            layoutRules,
            indexBufferStructType,
            &indexBufferSize);
        moduleBuilder.addDecorationIfNotExist(indexBufferStructType, kIROp_SPIRVBlockDecoration);

        indexBuffer = moduleBuilder.createGlobalParam(constantBufferType);
        auto varLayout = createIndexBufferLayout(moduleBuilder);
        moduleBuilder.addLayoutDecoration(indexBuffer, varLayout);
        moduleBuilder.addNameHintDecoration(indexBuffer, toSlice("__slang_bindless_index_buffer"));
        return indexBuffer;
    }

    int allocateIndexBufferSlots(int bindingCount)
    {
        int slot = nextIndexBufferSlot;
        int slotsToReserve = bindingCount > 0 ? bindingCount : 1;
        nextIndexBufferSlot += slotsToReserve;
        return slot;
    }

    bool canAllocateIndexBufferSlots(const String& name, int bindingCount)
    {
        int slotsToReserve = bindingCount > 0 ? bindingCount : 1;
        if (nextIndexBufferSlot + slotsToReserve <= kBindlessIndexBufferElementCount)
            return true;

        if (sink)
        {
            StringBuilder sb;
            sb << "automatic bindless index buffer needs "
               << nextIndexBufferSlot + slotsToReserve
               << " slots after adding '" << name
               << "', but the generated index buffer only has "
               << kBindlessIndexBufferElementCount << " slots.";
            sink->diagnoseRaw(Severity::Warning, sb.getUnownedSlice());
        }
        return false;
    }

    bool resolveArrayBindingCount(
        const String& name,
        BindlessResourceType resourceType,
        int& bindingCount)
    {
        if (bindingCount >= 0)
            return true;

        if (!targetProgram || !targetProgram->hasBindlessArraySizeResolver())
        {
            if (sink)
            {
                StringBuilder sb;
                sb << "automatic bindless resource array '" << name
                   << "' has unknown size, but no bindless array size resolver was provided.";
                sink->diagnoseRaw(Severity::Warning, sb.getUnownedSlice());
            }
            return false;
        }

        int resolvedBindingCount = targetProgram->resolveBindlessArraySize(
            name.getBuffer(),
            static_cast<slang::SlangBindlessResourceType>(resourceType));
        if (resolvedBindingCount <= 0)
        {
            if (sink)
            {
                StringBuilder sb;
                sb << "bindless array size resolver returned " << resolvedBindingCount
                   << " for '" << name << "'; expected a positive slot count.";
                sink->diagnoseRaw(Severity::Warning, sb.getUnownedSlice());
            }
            return false;
        }

        bindingCount = resolvedBindingCount;
        return true;
    }

    IRInst* buildIndexBufferSlot(IRBuilder& builder, int baseIndex, IRInst* userIndex)
    {
        IRType* indexType = userIndex->getDataType();
        auto basicType = as<IRBasicType>(indexType);
        if (!basicType ||
            (basicType->getBaseType() != BaseType::Int && basicType->getBaseType() != BaseType::UInt))
        {
            indexType = builder.getBasicType(BaseType::Int);
            userIndex = builder.emitCast(indexType, userIndex);
        }

        auto baseLiteral = builder.getIntValue(indexType, baseIndex);
        return builder.emitAdd(indexType, baseLiteral, userIndex);
    }

    IRInst* emitDescriptorIndexLoad(IRBuilder& builder, IRInst* indexBufferSlot)
    {
        auto indexBufferParam = getOrCreateIndexBuffer();
        SLANG_ASSERT(indexBufferArrayKey);
        auto arrayPtr = builder.emitFieldAddress(
            builder.getPtrType(
                indexBufferArrayType,
                AccessQualifier::Read,
                AddressSpace::Uniform,
                indexBufferDataLayoutType),
            indexBufferParam,
            indexBufferArrayKey);
        auto elementPtr = builder.emitElementAddress(
            builder.getPtrType(
                builder.getUIntType(),
                AccessQualifier::Read,
                AddressSpace::Uniform,
                indexBufferDataLayoutType),
            arrayPtr,
            indexBufferSlot);
        return builder.emitLoad(builder.getUIntType(), elementPtr);
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
        bool shouldKeepLogicalElementType = !typeNeedsStorageLoweringForSPIRV(elementType);

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

        if (shouldKeepLogicalElementType)
        {
            if (auto elementStruct = as<IRStructType>(elementType))
            {
                moduleBuilder.addPhysicalTypeDecoration(elementStruct);
            }
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
        auto arrayField = moduleBuilder.createStructField(wrapperStruct, arrayKey, unsizedArrayType);
        auto intType = moduleBuilder.getIntType();
        moduleBuilder.addDecoration(
            arrayField,
            kIROp_OffsetDecoration,
            moduleBuilder.getIntValue(intType, (IRIntegerValue)layoutRules->ruleName),
            moduleBuilder.getIntValue(intType, 0));

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

        if (!typeNeedsStorageLoweringForSPIRV(elementType))
            moduleBuilder.addPhysicalTypeDecoration(elementStruct);
        moduleBuilder.addDecorationIfNotExist(elementStruct, kIROp_SPIRVBlockDecoration);
    }

    bool typeNeedsStorageLoweringForSPIRV(IRType* type)
    {
        type = (IRType*)unwrapAttributedType(type);
        if (!type)
            return false;
        if (as<IRBoolType>(type))
            return true;
        if (auto vectorType = as<IRVectorType>(type))
            return typeNeedsStorageLoweringForSPIRV(vectorType->getElementType());
        if (auto matrixType = as<IRMatrixType>(type))
            return typeNeedsStorageLoweringForSPIRV(matrixType->getElementType());
        if (auto arrayType = as<IRArrayTypeBase>(type))
            return typeNeedsStorageLoweringForSPIRV(arrayType->getElementType());
        if (auto structType = as<IRStructType>(type))
        {
            for (auto field : structType->getFields())
            {
                if (typeNeedsStorageLoweringForSPIRV(field->getFieldType()))
                    return true;
            }
        }
        return false;
    }

    // Map IR type to BindlessResourceType enum for usage metadata.
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

    void processModule()
    {
        List<ResourceToConvert> resourcesToConvert;
        List<IRGlobalParam*> unconvertedResources;

        for (auto inst : module->getGlobalInsts())
        {
            auto globalParam = as<IRGlobalParam>(inst);
            if (!globalParam)
                continue;
            if (globalParam->findDecoration<IRHasExplicitVulkanBindingDecoration>())
                continue;
            if (hasPushConstantLayout(globalParam))
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
                int bindingCount = shaderArrayLength >= 0 ? shaderArrayLength : -1;
                auto resolverResourceType = isCombinedTextureType(arrayElementType)
                                                ? BindlessResourceType::CombinedTextureSampler
                                                : getBindlessResourceTypeForIRType(arrayElementType);
                if (!resolveArrayBindingCount(
                        name,
                        resolverResourceType,
                        bindingCount))
                {
                    unconvertedResources.add(globalParam);
                    continue;
                }
                int slotsToReserve = bindingCount >= 0 ? bindingCount : 1;
                int totalSlotsToReserve = slotsToReserve;
                if (isCombinedTextureType(arrayElementType))
                    totalSlotsToReserve += slotsToReserve;
                if (canAllocateIndexBufferSlots(name, totalSlotsToReserve))
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

                    ResourceToConvert info;
                    info.param = globalParam;
                    info.resourceType = arrayElementType;
                    info.index = allocateIndexBufferSlots(slotsToReserve);
                    info.bindingCount = bindingCount;
                    info.isArrayResource = true;
                    if (isCombinedTextureType(arrayElementType))
                    {
                        info.samplerIndex = allocateIndexBufferSlots(slotsToReserve);
                        info.samplerBindingCount = bindingCount;
                    }
                    resourcesToConvert.add(info);
                }
                continue;
            }

            // Only direct arrays of resource objects are lowered automatically.
            // More complex array nesting is currently left unchanged.
            if (as<IRArrayTypeBase>(paramType))
            {
                unconvertedResources.add(globalParam);
                continue;
            }

            int totalSlotsToReserve = isCombinedTextureType(paramType) ? 2 : 1;
            if (canAllocateIndexBufferSlots(name, totalSlotsToReserve))
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
                info.index = allocateIndexBufferSlots(1);
                info.bindingCount = 1;
                info.isArrayResource = false;
                if (isCombinedTextureType(paramType))
                {
                    info.samplerIndex = allocateIndexBufferSlots(1);
                    info.samplerBindingCount = 1;
                }
                resourcesToConvert.add(info);
            }
        }

        for (auto param : unconvertedResources)
        {
            auto nameHint = param->findDecoration<IRNameHintDecoration>();
            if (nameHint && sink)
            {
                StringBuilder sb;
                sb << "resource '" << nameHint->getName()
                   << "' could not be converted to automatic bindless access.";
                sink->diagnoseRaw(Severity::Warning, sb.getUnownedSlice());
            }
        }

        if (resourcesToConvert.getCount() == 0)
            return;

        for (const auto& resource : resourcesToConvert)
        {
            if (resource.isArrayResource)
                convertArrayResourceWithIndexSlot(
                    resource.param,
                    resource.resourceType,
                    resource.index,
                    resource.bindingCount,
                    resource.samplerIndex,
                    resource.samplerBindingCount);
            else
                convertResourceWithIndexSlot(
                    resource.param,
                    resource.resourceType,
                    resource.index,
                    resource.bindingCount,
                    resource.samplerIndex,
                    resource.samplerBindingCount);
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

    IRInst* emitBindlessTextureLookup(
        IRBuilder& builder,
        IRType* resourceType,
        IRInst* indexBufferSlot,
        bool nonUniformHeapIndex)
    {
        auto bindlessLookupType = getUncombinedTextureType(builder, resourceType);
        int bindingIndex = getBindingIndexForResourceType(bindlessLookupType);
        bool useSPIRVNonUniformDecoration =
            isSPIRV(targetProgram->getTargetReq()->getTarget());

        if (nonUniformHeapIndex && !useSPIRVNonUniformDecoration)
            indexBufferSlot = builder.emitNonUniformResourceIndexInst(indexBufferSlot);

        auto heapIndex = emitDescriptorIndexLoad(builder, indexBufferSlot);
        auto resourceHeap = getOrCreateResourceHeap(bindingIndex, bindlessLookupType);
        auto elementPtr = builder.emitElementAddress(resourceHeap, heapIndex);
        if (nonUniformHeapIndex && useSPIRVNonUniformDecoration)
            builder.addSPIRVNonUniformResourceDecoration(elementPtr);

        auto replacement = builder.emitLoad(bindlessLookupType, elementPtr);
        if (nonUniformHeapIndex && useSPIRVNonUniformDecoration)
            builder.addSPIRVNonUniformResourceDecoration(replacement);
        return replacement;
    }

    bool tryRewriteAsmImageTypeOperandUse(
        const String& resourceName,
        IRType* resourceType,
        IRUse* use,
        IRInst* indexBufferSlot,
        bool nonUniformHeapIndex = false)
    {
        SLANG_UNUSED(resourceName);

        auto asmOperand = as<IRSPIRVAsmOperand>(use->getUser());
        if (!asmOperand)
            return false;

        switch (asmOperand->getOp())
        {
        case kIROp_SPIRVAsmOperandImageType:
        case kIROp_SPIRVAsmOperandSampledImageType:
            break;
        default:
            return false;
        }

        List<IRUse*> asmOperandUses;
        for (auto operandUse = asmOperand->firstUse; operandUse; operandUse = operandUse->nextUse)
            asmOperandUses.add(operandUse);

        for (auto operandUse : asmOperandUses)
        {
            auto operandUser = operandUse->getUser();
            if (!isInsideFunction(operandUser))
                continue;

            auto asmBlock = findParentSPIRVAsm(operandUser);
            if (!asmBlock)
                continue;

            IRBuilder builder(operandUser);
            builder.setInsertBefore(operandUser);

            auto replacementValue =
                emitBindlessTextureLookup(builder, resourceType, indexBufferSlot, nonUniformHeapIndex);

            IRSPIRVAsmOperand* replacementOperand = nullptr;
            switch (asmOperand->getOp())
            {
            case kIROp_SPIRVAsmOperandImageType:
                replacementOperand = builder.emitSPIRVAsmOperandImageType(replacementValue);
                break;
            case kIROp_SPIRVAsmOperandSampledImageType:
                replacementOperand = builder.emitSPIRVAsmOperandSampledImageType(replacementValue);
                break;
            default:
                SLANG_UNREACHABLE("unexpected asm operand op");
            }

            builder.replaceOperand(operandUse, replacementOperand);
        }

        if (!asmOperand->hasUses())
            asmOperand->removeAndDeallocate();

        return true;
    }

    void rewriteResourceUseWithHeapIndex(
        IRBuilder& builder,
        const String& resourceName,
        IRType* resourceType,
        IRUse* use,
        IRInst* indexBufferSlot,
        IRInst* samplerIndexBufferSlot = nullptr,
        bool nonUniformHeapIndex = false)
    {
        SLANG_UNUSED(resourceName);
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

        IRType* heapDataLayoutType = nullptr;
        if (isUniformBuffer)
        {
            IRBuilder moduleBuilder(module);
            moduleBuilder.setInsertInto(module->getModuleInst());
            heapDataLayoutType =
                getTypeLayoutTypeForBuffer(targetProgram, moduleBuilder, resourceType);
        }

        IRInst* resourceHeap =
            getOrCreateResourceHeap(bindingIndex, heapElementType, heapDataLayoutType);
        IRInst* heapIndex = emitDescriptorIndexLoad(builder, indexBufferSlot);

        if (isCombinedTextureResource)
        {
            auto samplerType = getIntVal(combinedTextureType->getIsShadowInst()) != 0
                                   ? builder.getType(kIROp_SamplerComparisonStateType)
                                   : builder.getType(kIROp_SamplerStateType);
            auto target = targetProgram->getTargetReq()->getTarget();
            bool useSamplerHeapIntrinsic = !(
                target == CodeGenTarget::SPIRV || target == CodeGenTarget::SPIRVAssembly ||
                target == CodeGenTarget::GLSL);
            IRInst* samplerIndex = nullptr;
            if (samplerIndexBufferSlot)
            {
                samplerIndex = emitDescriptorIndexLoad(builder, samplerIndexBufferSlot);
            }
            else
            {
                samplerIndex = builder.getIntValue(builder.getUIntType(), 0);
            }

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

                    if (samplerIndex->getDataType() != uintType)
                        samplerIndex = builder.emitCast(uintType, samplerIndex);
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

            IRInst* samplerVal = nullptr;
            if (useSamplerHeapIntrinsic)
            {
                samplerVal = builder.emitIntrinsicInst(
                    samplerType,
                    kIROp_LoadSamplerDescriptorFromHeap,
                    1,
                    &samplerIndex);
            }
            else
            {
                auto samplerHeap = getOrCreateResourceHeap(0, samplerType);
                auto samplerPtr = builder.emitElementAddress(samplerHeap, samplerIndex);
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
            if (user->getOp() == kIROp_Load)
            {
                user->replaceUsesWith(replacement);
                user->removeAndDeallocate();
            }
            else
            {
                builder.replaceOperand(use, replacement);
            }
        }
    }

    void addConvertedResourceInfo(
        const String& name,
        IRType* originalType,
        IRType* resourceType,
        int index,
        int binding,
        int bindingCount,
        BindlessResourceType bindlessResourceType)
    {
        if (!outConvertedResources)
            return;

        BindlessConvertedResource info;
        info.name = name;
        info.typeName = getResourceTypeName(originalType);
        info.index = index;
        info.binding = binding;
        info.bindingCount = bindingCount;
        info.resourceType = bindlessResourceType;
        getArrayResourceInfo(originalType, info.isArray, info.arraySize);
        if (info.isArray && info.arraySize < 0 && bindingCount > 0)
            info.arraySize = bindingCount;
        info.access = getResourceAccess(resourceType);
        outConvertedResources->add(info);
    }

    void addConvertedResourceInfoForTextureSampler(
        const String& name,
        IRType* originalType,
        int index,
        int bindingCount)
    {
        if (!outConvertedResources)
            return;

        BindlessConvertedResource info;
        info.name = name;
        info.typeName = "SamplerState";
        info.index = index;
        info.binding = 0;
        info.bindingCount = bindingCount;
        info.resourceType = BindlessResourceType::Sampler;
        getArrayResourceInfo(originalType, info.isArray, info.arraySize);
        if (info.isArray && info.arraySize < 0 && bindingCount > 0)
            info.arraySize = bindingCount;
        info.access = SLANG_RESOURCE_ACCESS_READ;
        outConvertedResources->add(info);
    }

    void convertResourceWithIndexSlot(
        IRGlobalParam* param,
        IRType* resourceType,
        int index,
        int bindingCount,
        int samplerIndex,
        int samplerBindingCount)
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

            auto indexLiteral = builder.getIntValue(builder.getBasicType(BaseType::Int), index);
            IRInst* samplerIndexLiteral = nullptr;
            if (samplerIndex >= 0)
            {
                samplerIndexLiteral =
                    builder.getIntValue(builder.getBasicType(BaseType::Int), samplerIndex);
            }
            if (!isInsideFunction(user))
            {
                if (tryRewriteAsmImageTypeOperandUse(name, resourceType, use, indexLiteral))
                    continue;
                continue;
            }

            builder.setInsertBefore(user);
            rewriteResourceUseWithHeapIndex(
                builder,
                name,
                resourceType,
                use,
                indexLiteral,
                samplerIndexLiteral);
        }

        IRType* lookupType = getUncombinedTextureType(builder, resourceType);
        addConvertedResourceInfo(
            name,
            resourceType,
            lookupType,
            index,
            getBindingIndexForResourceType(lookupType),
            bindingCount,
            getBindlessResourceTypeForIRType(lookupType));
        if (samplerIndex >= 0)
            addConvertedResourceInfoForTextureSampler(
                name,
                resourceType,
                samplerIndex,
                samplerBindingCount);

        if (!tryRemoveConvertedGlobalParam(param) && !param->hasUses())
            param->removeAndDeallocate();
    }

    void convertArrayResourceWithIndexSlot(
        IRGlobalParam* param,
        IRType* resourceType,
        int baseIndex,
        int bindingCount,
        int samplerBaseIndex,
        int samplerBindingCount)
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
                auto effectiveIndex = buildIndexBufferSlot(builder, baseIndex, userIndex);
                IRInst* effectiveSamplerIndex = nullptr;
                if (samplerBaseIndex >= 0)
                    effectiveSamplerIndex =
                        buildIndexBufferSlot(builder, samplerBaseIndex, userIndex);
                rewriteResourceUseWithHeapIndex(
                    builder,
                    name,
                    resourceType,
                    elementUse,
                    effectiveIndex,
                    effectiveSamplerIndex,
                    hasNonUniformUserIndex);
            }

            if (!getElementInst->hasUses())
                getElementInst->removeAndDeallocate();
            removeDeadNonUniformIndexWrappers(originalUserIndex);
        }

        IRType* lookupType = getUncombinedTextureType(builder, resourceType);
        addConvertedResourceInfo(
            name,
            param->getDataType(),
            lookupType,
            baseIndex,
            getBindingIndexForResourceType(lookupType),
            bindingCount,
            getBindlessResourceTypeForIRType(lookupType));
        if (samplerBaseIndex >= 0)
            addConvertedResourceInfoForTextureSampler(
                name,
                param->getDataType(),
                samplerBaseIndex,
                samplerBindingCount);

        if (!tryRemoveConvertedGlobalParam(param) && !param->hasUses())
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

    if (isSPIRV(targetProgram->getTargetReq()->getTarget()))
    {
        BufferElementTypeLoweringOptions options;
        options.loweringPolicyKind = BufferElementTypeLoweringPolicyKind::KhronosTarget;
        lowerBufferElementTypeToStorageType(module, targetProgram, options);
    }
}

} // namespace Slang
