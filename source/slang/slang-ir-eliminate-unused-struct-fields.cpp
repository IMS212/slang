#include "slang-ir-eliminate-unused-struct-fields.h"
#include "slang-ir.h"
#include "slang-ir-insts.h"
#include "slang-ir-legalize-varying-params.h"

namespace Slang
{
    struct UnusedFieldEliminationContext
    {
        IRModule* module;

        // Collect all MakeStruct instructions
        List<IRInst*> makeStructInsts;

        // Track which varying locations are actually used by fragment shader inputs
        HashSet<UInt> usedVaryingLocations;

        // Track whether we found a fragment entry point in the module
        bool hasFragmentEntryPoint = false;

        void collectMakeStructs();
        void collectMakeStructsInInst(IRInst* inst);
        void collectMakeStructsInFunc(IRFunc* func);

        // For each MakeStruct, find which fields are actually accessed
        // by following uses transitively
        void collectUsedFieldsForMakeStruct(IRInst* makeStruct, HashSet<IRStructKey*>& usedFields);
        void collectUsedFieldsFromUses(IRInst* value, IRStructType* structType,
                                       HashSet<IRStructKey*>& usedFields,
                                       HashSet<IRInst*>& visited);
        void collectUsedFieldsFromPtrUses(IRInst* ptr, IRStructType* structType,
                                          HashSet<IRStructKey*>& usedFields,
                                          HashSet<IRInst*>& visited);

        bool isVaryingStructType(IRStructType* structType);
        bool isInFragmentEntryPoint(IRInst* inst);

        void replaceUnusedOperands();

        // Phase 3: Collect used varying locations from fragment shader inputs
        void collectUsedVaryingLocations();
        void collectUsedVaryingLocationsFromValue(IRInst* value, HashSet<IRInst*>& visited);

        // Phase 4: Optimize vertex shader outputs
        void optimizeVertexOutputs();

        // Helper: Get varying location from layout decoration
        // Returns -1 if not found
        Int getVaryingLocation(IRInst* globalParam, LayoutResourceKind kind);

        bool isSystemValueParam(IRInst* globalParam);
    };

    void UnusedFieldEliminationContext::collectMakeStructsInInst(IRInst* inst)
    {
        if (inst->getOp() == kIROp_MakeStruct)
        {
            makeStructInsts.add(inst);
        }

        for (auto child : inst->getChildren())
        {
            collectMakeStructsInInst(child);
        }
    }

    void UnusedFieldEliminationContext::collectMakeStructsInFunc(IRFunc* func)
    {
        for (auto block : func->getBlocks())
        {
            for (auto inst : block->getChildren())
            {
                collectMakeStructsInInst(inst);
            }
        }
    }

    void UnusedFieldEliminationContext::collectMakeStructs()
    {
        for (auto globalInst : module->getGlobalInsts())
        {
            if (auto func = as<IRFunc>(globalInst))
            {
                collectMakeStructsInFunc(func);
            }
            else if (auto generic = as<IRGeneric>(globalInst))
            {
                for (auto child : generic->getChildren())
                {
                    if (auto block = as<IRBlock>(child))
                    {
                        for (auto inst : block->getChildren())
                        {
                            if (auto func = as<IRFunc>(inst))
                                collectMakeStructsInFunc(func);
                        }
                    }
                }
            }
        }
    }

    void UnusedFieldEliminationContext::collectUsedFieldsFromUses(
        IRInst* value,
        IRStructType* structType,
        HashSet<IRStructKey*>& usedFields,
        HashSet<IRInst*>& visited)
    {
        if (visited.contains(value))
            return;
        visited.add(value);

        for (auto use = value->firstUse; use; use = use->nextUse)
        {
            auto user = use->getUser();

            // FieldExtract or FieldAddress on this value marks the field as used
            if ((user->getOp() == kIROp_FieldExtract || user->getOp() == kIROp_FieldAddress) &&
                user->getOperand(0) == value)
            {
                if (auto fieldKey = as<IRStructKey>(user->getOperand(1)))
                {
                    usedFields.add(fieldKey);
                }
                // Also follow uses of the field extract result
                collectUsedFieldsFromUses(user, nullptr, usedFields, visited);
            }
            // If a struct value is nested inside another MakeStruct, conservatively
            // treat all of its fields as used. Otherwise we can incorrectly replace
            // the nested value with defaults before the outer struct's field use
            // gets observed, which breaks nested varying payloads like
            // `RealVertexOutput<T>.surfaceData`.
            else if (user->getOp() == kIROp_MakeStruct && structType)
            {
                for (auto field : structType->getFields())
                {
                    usedFields.add(field->getKey());
                }
            }
            // If the struct value is passed to a function, follow into the function
            else if (auto call = as<IRCall>(user))
            {
                // Find which parameter this value is passed as
                auto callee = call->getCallee();
                auto func = as<IRFunc>(callee);
                if (!func)
                {
                    if (auto specialize = as<IRSpecialize>(callee))
                        func = as<IRFunc>(specialize->getBase());
                }

                if (func && func->getFirstBlock())
                {
                    // Find the parameter index
                    UInt argIndex = 0;
                    for (UInt i = 0; i < call->getArgCount(); i++)
                    {
                        if (call->getArg(i) == value)
                        {
                            argIndex = i;
                            break;
                        }
                    }

                    // Get the corresponding parameter
                    auto firstBlock = func->getFirstBlock();
                    UInt paramIndex = 0;
                    for (auto param : firstBlock->getParams())
                    {
                        if (paramIndex == argIndex)
                        {
                            // Follow uses of this parameter
                            collectUsedFieldsFromUses(param, structType, usedFields, visited);
                            break;
                        }
                        paramIndex++;
                    }
                }
            }
            // If the struct is stored somewhere (value is operand 1), follow uses of that storage
            else if (user->getOp() == kIROp_Store && user->getOperand(1) == value)
            {
                auto ptr = user->getOperand(0);
                // Follow all uses of this pointer (loads, field addresses, calls)
                collectUsedFieldsFromPtrUses(ptr, structType, usedFields, visited);
            }
            // If the struct is loaded from, continue tracking
            else if (user->getOp() == kIROp_Load)
            {
                collectUsedFieldsFromUses(user, structType, usedFields, visited);
            }
        }
    }

    void UnusedFieldEliminationContext::collectUsedFieldsFromPtrUses(
        IRInst* ptr,
        IRStructType* structType,
        HashSet<IRStructKey*>& usedFields,
        HashSet<IRInst*>& visited)
    {
        if (visited.contains(ptr))
            return;
        visited.add(ptr);

        for (auto ptrUse = ptr->firstUse; ptrUse; ptrUse = ptrUse->nextUse)
        {
            auto ptrUser = ptrUse->getUser();

            // Load from the pointer - follow the loaded value
            if (ptrUser->getOp() == kIROp_Load)
            {
                collectUsedFieldsFromUses(ptrUser, structType, usedFields, visited);
            }
            // FieldAddress on the pointer - marks the field as used
            else if (ptrUser->getOp() == kIROp_FieldAddress ||
                     ptrUser->getOp() == kIROp_GetElementPtr)
            {
                if (ptrUser->getOperand(0) == ptr)
                {
                    if (auto fieldKey = as<IRStructKey>(ptrUser->getOperand(1)))
                    {
                        usedFields.add(fieldKey);
                    }
                    // Also follow uses of the field address (for nested field access)
                    collectUsedFieldsFromPtrUses(ptrUser, nullptr, usedFields, visited);
                }
            }
            // Pointer passed to a function call
            else if (auto call = as<IRCall>(ptrUser))
            {
                auto callee = call->getCallee();
                auto func = as<IRFunc>(callee);
                if (!func)
                {
                    if (auto specialize = as<IRSpecialize>(callee))
                        func = as<IRFunc>(specialize->getBase());
                }

                if (func && func->getFirstBlock())
                {
                    // Find which argument position this pointer is at
                    UInt argIndex = 0;
                    for (UInt i = 0; i < call->getArgCount(); i++)
                    {
                        if (call->getArg(i) == ptr)
                        {
                            argIndex = i;

                            // Get the corresponding parameter
                            auto firstBlock = func->getFirstBlock();
                            UInt paramIndex = 0;
                            for (auto param : firstBlock->getParams())
                            {
                                if (paramIndex == argIndex)
                                {
                                    // Follow uses of this parameter pointer
                                    collectUsedFieldsFromPtrUses(param, structType, usedFields, visited);
                                    break;
                                }
                                paramIndex++;
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    void UnusedFieldEliminationContext::collectUsedFieldsForMakeStruct(
        IRInst* makeStruct,
        HashSet<IRStructKey*>& usedFields)
    {
        auto structType = as<IRStructType>(makeStruct->getDataType());
        if (!structType)
            return;

        HashSet<IRInst*> visited;
        collectUsedFieldsFromUses(makeStruct, structType, usedFields, visited);
    }

    bool UnusedFieldEliminationContext::isVaryingStructType(IRStructType* structType)
    {
        // A varying struct type has fields with semantic decorations
        // (like SV_Position, TEXCOORD, etc.)
        for (auto field : structType->getFields())
        {
            auto key = field->getKey();
            if (key->findDecoration<IRSemanticDecoration>())
                return true;
        }
        return false;
    }

    bool UnusedFieldEliminationContext::isInFragmentEntryPoint(IRInst* inst)
    {
        // Walk up to find the containing function
        auto parent = inst->getParent();
        while (parent)
        {
            if (auto func = as<IRFunc>(parent))
            {
                // Check if this function is a fragment entry point
                if (auto entryPoint = func->findDecoration<IREntryPointDecoration>())
                {
                    if (entryPoint->getProfile().getStage() == Stage::Fragment)
                        return true;
                }
                return false;
            }
            parent = parent->getParent();
        }
        return false;
    }

    bool isInVertexEntryPoint(IRInst* inst)
    {
        // Walk up to find the containing function
        auto parent = inst->getParent();
        while (parent)
        {
            if (auto func = as<IRFunc>(parent))
            {
                // Check if this function is a vertex entry point
                if (auto entryPoint = func->findDecoration<IREntryPointDecoration>())
                {
                    if (entryPoint->getProfile().getStage() == Stage::Vertex)
                        return true;
                }
                return false;
            }
            parent = parent->getParent();
        }
        return false;
    }

    bool isVertexOutput(IRGlobalParam* globalParam)
    {
        // Check if any store to this param is inside a vertex entry point
        for (auto use = globalParam->firstUse; use; use = use->nextUse)
        {
            auto user = use->getUser();
            if (user->getOp() == kIROp_Store)
            {
                auto store = as<IRStore>(user);
                if (store->getPtr() == globalParam)
                {
                    if (isInVertexEntryPoint(store))
                        return true;
                }
            }
        }
        return false;
    }

    void UnusedFieldEliminationContext::replaceUnusedOperands()
    {
        for (auto makeStruct : makeStructInsts)
        {
            auto structType = as<IRStructType>(makeStruct->getDataType());
            if (!structType)
                continue;

            // Only optimize varying struct types in fragment shaders
            // This avoids incorrectly optimizing existential types, witness wrappers, etc.
            if (!isVaryingStructType(structType))
                continue;
            if (!isInFragmentEntryPoint(makeStruct))
                continue;

            // Find which fields are actually used for THIS specific MakeStruct
            HashSet<IRStructKey*> usedFields;
            collectUsedFieldsForMakeStruct(makeStruct, usedFields);

            IRBuilder builder(module);
            builder.setInsertBefore(makeStruct);

            Index fieldIdx = 0;
            for (auto field : structType->getFields())
            {
                bool fieldUsed = usedFields.contains(field->getKey());

                if (!fieldUsed)
                {
                    // Replace with default value
                    auto defaultVal = builder.emitDefaultConstruct(field->getFieldType());
                    if (defaultVal)
                        makeStruct->setOperand(fieldIdx, defaultVal);
                }
                fieldIdx++;
            }
        }
    }

    Int UnusedFieldEliminationContext::getVaryingLocation(IRInst* globalParam, LayoutResourceKind kind)
    {
        auto layoutDecor = globalParam->findDecoration<IRLayoutDecoration>();
        if (!layoutDecor)
            return -1;

        auto varLayout = as<IRVarLayout>(layoutDecor->getLayout());
        if (!varLayout)
            return -1;

        auto offsetAttr = varLayout->findOffsetAttr(kind);
        if (!offsetAttr)
            return -1;

        return (Int)offsetAttr->getOffset();
    }

    bool UnusedFieldEliminationContext::isSystemValueParam(IRInst* globalParam)
    {
        if (auto layoutDecor = globalParam->findDecoration<IRLayoutDecoration>())
        {
            if (auto varLayout = as<IRVarLayout>(layoutDecor->getLayout()))
            {
                if (varLayout->findSystemValueSemanticAttr())
                    return true;
            }
        }

        if (auto semanticDecor = globalParam->findDecoration<IRSemanticDecoration>())
        {
            if (convertSystemValueSemanticNameToEnum(String(semanticDecor->getSemanticName())) !=
                SystemValueSemanticName::None)
            {
                return true;
            }
        }

        return false;
    }

    void UnusedFieldEliminationContext::collectUsedVaryingLocationsFromValue(
        IRInst* value,
        HashSet<IRInst*>& visited)
    {
        if (!value || visited.contains(value))
            return;
        visited.add(value);

        switch (value->getOp())
        {
        case kIROp_DefaultConstruct:
        case kIROp_MakeVectorFromScalar:
            return;

        case kIROp_Load:
            {
                auto loadInst = as<IRLoad>(value);
                auto loadPtr = loadInst->getPtr();
                auto globalParam = as<IRGlobalParam>(loadPtr);
                if (!globalParam)
                    return;

                auto paramType = globalParam->getDataType();
                if (paramType->getOp() != kIROp_BorrowInParamType)
                    return;

                if (isSystemValueParam(globalParam) ||
                    globalParam->findDecoration<IRGLPositionInputDecoration>())
                {
                    return;
                }

                Int location = getVaryingLocation(globalParam, LayoutResourceKind::VaryingInput);
                if (location >= 0)
                {
                    usedVaryingLocations.add((UInt)location);
                }
                return;
            }

        case kIROp_MakeStruct:
        case kIROp_MakeArray:
        case kIROp_MakeTuple:
            for (UInt i = 0; i < value->getOperandCount(); i++)
            {
                collectUsedVaryingLocationsFromValue(value->getOperand(i), visited);
            }
            return;

        default:
            return;
        }
    }

    void UnusedFieldEliminationContext::collectUsedVaryingLocations()
    {
        // After Phase 2, MakeStruct operands that were unused have been replaced with
        // defaults. We need to find which fragment input global params are STILL
        // referenced (i.e., their loads are still used as MakeStruct operands).
        //
        // For each MakeStruct in a fragment entry point, trace back non-default operands
        // to find which global params they load from, then collect those params' locations.

        for (auto makeStruct : makeStructInsts)
        {
            // Only process MakeStructs in fragment entry points
            if (!isInFragmentEntryPoint(makeStruct))
                continue;

            auto structType = as<IRStructType>(makeStruct->getDataType());
            if (!structType)
                continue;

            // Only process varying struct types
            if (!isVaryingStructType(structType))
                continue;

            // We found a varying struct MakeStruct in a fragment entry point
            hasFragmentEntryPoint = true;

            // For each operand in the MakeStruct, check if it's a load from a
            // fragment input global param (vs a default value). Nested payloads
            // are represented as nested MakeStruct values, so recurse.
            HashSet<IRInst*> visited;
            for (UInt i = 0; i < makeStruct->getOperandCount(); i++)
            {
                collectUsedVaryingLocationsFromValue(makeStruct->getOperand(i), visited);
            }
        }
    }

    void UnusedFieldEliminationContext::optimizeVertexOutputs()
    {
        IRBuilder builder(module);

        for (auto globalInst : module->getGlobalInsts())
        {
            auto globalParam = as<IRGlobalParam>(globalInst);
            if (!globalParam)
                continue;

            // Check if this is a vertex output param (OutParamType)
            auto paramType = globalParam->getDataType();
            if (paramType->getOp() != kIROp_OutParamType)
                continue;

            // Skip system-value outputs like SV_Position and
            // SV_RenderTargetArrayIndex. They are not fragment varyings and
            // must not be defaulted just because the fragment stage doesn't
            // read a matching location.
            if (isSystemValueParam(globalParam) ||
                globalParam->findDecoration<IRGLPositionOutputDecoration>())
                continue;

            // Only process vertex outputs (not fragment outputs which are render targets)
            if (!isVertexOutput(globalParam))
                continue;

            // Get the varying output location from layout
            Int location = getVaryingLocation(globalParam, LayoutResourceKind::VaryingOutput);
            if (location < 0)
                continue;

            // If this location is used by fragment shader, skip it
            if (usedVaryingLocations.contains((UInt)location))
                continue;

            // This vertex output is not used by fragment shader.
            // Find all stores to this param and replace the stored value with a default.
            auto ptrType = as<IRPtrTypeBase>(paramType);
            if (!ptrType)
                continue;
            auto valueType = ptrType->getValueType();

            for (auto use = globalParam->firstUse; use; use = use->nextUse)
            {
                auto user = use->getUser();
                if (user->getOp() == kIROp_Store)
                {
                    auto store = as<IRStore>(user);
                    // Make sure this is a store TO the param (param is operand 0)
                    if (store->getPtr() == globalParam)
                    {
                        builder.setInsertBefore(store);
                        auto defaultVal = builder.emitDefaultConstruct(valueType);
                        if (defaultVal)
                        {
                            store->setOperand(1, defaultVal);
                        }
                    }
                }
            }
        }
    }

    void eliminateUnusedStructFieldInits(IRModule* module)
    {
        UnusedFieldEliminationContext context;
        context.module = module;

        // Phase 1: Collect all MakeStruct instructions
        context.collectMakeStructs();

        // Phase 2: For each MakeStruct, find unused fields and replace with defaults
        // (fragment shader optimization - handles struct field elimination)
        context.replaceUnusedOperands();

        // Phase 3: Collect used varying locations from fragment shader inputs
        context.collectUsedVaryingLocations();

        // Phase 4: Optimize vertex shader outputs that are not used by fragment shader
        // Only run if we found a fragment entry point with a varying struct MakeStruct.
        // If no such fragment entry point exists, don't optimize vertex outputs
        // because we don't know which outputs will be used.
        if (context.hasFragmentEntryPoint)
        {
            context.optimizeVertexOutputs();
        }

        // DCE will be run separately to clean up dead code
    }
}
