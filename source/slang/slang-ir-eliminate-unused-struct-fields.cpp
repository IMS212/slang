#include "slang-ir-eliminate-unused-struct-fields.h"
#include "slang-ir.h"
#include "slang-ir-insts.h"

namespace Slang
{
    struct UnusedFieldEliminationContext
    {
        IRModule* module;

        // Collect all MakeStruct instructions
        List<IRInst*> makeStructInsts;

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
                    // Stage 5 is fragment shader (from slang-ir-entry-point-pass.cpp)
                    if (entryPoint->getProfile().getStage() == Stage::Fragment)
                        return true;
                }
                return false;
            }
            parent = parent->getParent();
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

    void eliminateUnusedStructFieldInits(IRModule* module)
    {
        UnusedFieldEliminationContext context;
        context.module = module;

        // Phase 1: Collect all MakeStruct instructions
        context.collectMakeStructs();

        // Phase 2: For each MakeStruct, find unused fields and replace with defaults
        context.replaceUnusedOperands();

        // DCE will be run separately to clean up dead code
    }
}
