// slang-ir-lower-semantic-field-access.cpp
#include "slang-ir-lower-semantic-field-access.h"

#include "slang-ir-insts.h"
#include "slang-ir-util.h"
#include "slang-ir.h"
#include "slang-rich-diagnostics.h"

namespace Slang
{

namespace
{

struct IRSemanticFieldPath
{
    List<IRStructKey*> keys;
    IRType* fieldType = nullptr;
};

struct IRSemanticFieldLookupResult
{
    IRSemanticFieldPath path;
    int matchCount = 0;
    bool hasUnresolvedTypes = false;
};

static bool _tryGetSemanticNameAndIndex(
    IRSemanticDecoration* semantic,
    UnownedStringSlice& outName,
    int& outIndex)
{
    if (!semantic)
        return false;

    auto semanticText = semantic->getSemanticName();
    Index splitIndex = semanticText.getLength();
    while (splitIndex > 0)
    {
        const auto c = semanticText[splitIndex - 1];
        if (c < '0' || c > '9')
            break;
        splitIndex--;
    }

    outName = semanticText.head(splitIndex);
    outIndex = 0;
    if (splitIndex == semanticText.getLength())
        return true;

    for (Index i = splitIndex; i < semanticText.getLength(); ++i)
    {
        outIndex = outIndex * 10 + int(semanticText[i] - '0');
    }
    return true;
}

static bool _fieldMatchesSemantic(
    IRStructField* field,
    UnownedStringSlice wantedName,
    int wantedIndex)
{
    auto semantic = field->getKey()->findDecoration<IRSemanticDecoration>();
    UnownedStringSlice fieldName;
    int fieldIndex = 0;
    if (!_tryGetSemanticNameAndIndex(semantic, fieldName, fieldIndex))
        return false;

    return fieldIndex == wantedIndex && fieldName.caseInsensitiveEquals(wantedName);
}

static bool _isUnresolvedSemanticLookupType(IRInst* type)
{
    type = unwrapAttributedType(type);
    if (!type)
        return false;

    if (as<IRGlobalGenericParam>(type))
        return true;

    if (auto param = as<IRParam>(type))
        return isGenericParam(param);

    switch (type->getOp())
    {
    case kIROp_InterfaceType:
    case kIROp_WitnessTableType:
    case kIROp_FuncType:
    case kIROp_GenericKind:
    case kIROp_TypeKind:
    case kIROp_TypeType:
        return true;

    default:
        return false;
    }
}

static void _findSemanticFieldPathRec(
    IRType* type,
    UnownedStringSlice semanticName,
    int semanticIndex,
    int maxMatches,
    HashSet<IRInst*>& recursionStack,
    List<IRStructKey*>& ioCurrentPath,
    IRSemanticFieldLookupResult& ioResult)
{
    if (ioResult.matchCount >= maxMatches)
        return;

    type = type ? (IRType*)unwrapAttributedType(type) : nullptr;
    if (!type)
        return;

    auto structType = as<IRStructType>(type);
    if (!structType)
    {
        if (_isUnresolvedSemanticLookupType(type))
            ioResult.hasUnresolvedTypes = true;
        return;
    }

    if (recursionStack.contains(type))
        return;

    recursionStack.add(type);

    for (auto field : structType->getFields())
    {
        ioCurrentPath.add(field->getKey());

        if (_fieldMatchesSemantic(field, semanticName, semanticIndex))
        {
            if (ioResult.matchCount == 0)
            {
                ioResult.path.keys = ioCurrentPath;
                ioResult.path.fieldType = field->getFieldType();
            }
            ioResult.matchCount++;
        }

        if (ioResult.matchCount < maxMatches)
        {
            _findSemanticFieldPathRec(
                field->getFieldType(),
                semanticName,
                semanticIndex,
                maxMatches,
                recursionStack,
                ioCurrentPath,
                ioResult);
        }

        ioCurrentPath.removeLast();

        if (ioResult.matchCount >= maxMatches)
            break;
    }

    recursionStack.remove(type);
}

static IRSemanticFieldLookupResult _findSemanticFieldPath(
    IRType* type,
    UnownedStringSlice semanticName,
    int semanticIndex,
    int maxMatches)
{
    IRSemanticFieldLookupResult result;
    HashSet<IRInst*> recursionStack;
    List<IRStructKey*> currentPath;
    _findSemanticFieldPathRec(
        type,
        semanticName,
        semanticIndex,
        maxMatches,
        recursionStack,
        currentPath,
        result);
    return result;
}

static IRType* _getBaseLookupType(IRBuilder& builder, IRInst* valueArg)
{
    auto type = (IRType*)unwrapAttributedType(valueArg->getDataType());
    if (auto pointedToType = tryGetPointedToType(&builder, type))
        type = pointedToType;
    return (IRType*)unwrapAttributedType(type);
}

static IRInst* _emitFieldValueAtPath(
    IRBuilder& builder,
    IRInst* base,
    const IRSemanticFieldPath& path)
{
    if (tryGetPointedToType(&builder, (IRType*)unwrapAttributedType(base->getDataType())))
    {
        IRInst* fieldAddr = base;
        for (auto key : path.keys)
            fieldAddr = builder.emitFieldAddress(fieldAddr, key);
        return builder.emitLoad(fieldAddr);
    }

    IRInst* fieldValue = base;
    for (auto key : path.keys)
        fieldValue = builder.emitFieldExtract(fieldValue, key);
    return fieldValue;
}

static IRInst* _emitFieldAddressAtPath(
    IRBuilder& builder,
    IRInst* basePtr,
    const IRSemanticFieldPath& path)
{
    IRInst* fieldAddr = basePtr;
    for (auto key : path.keys)
        fieldAddr = builder.emitFieldAddress(fieldAddr, key);
    return fieldAddr;
}

static bool _replaceCallWithBool(IRBuilder& builder, IRCall* call, bool value)
{
    call->replaceUsesWith(builder.getBoolValue(value));
    call->removeAndDeallocate();
    return true;
}

static bool _isTypeCompatible(IRType* a, IRType* b)
{
    a = (IRType*)unwrapAttributedType(a);
    b = (IRType*)unwrapAttributedType(b);
    return isTypeEqual(a, b);
}

static bool _processSemanticFieldCall(IRModule* module, IRCall* call, DiagnosticSink* sink)
{
    auto builtinName = getBuiltinFuncEnum(call->getCallee());
    if (builtinName != KnownBuiltinDeclName::HasSemanticField &&
        builtinName != KnownBuiltinDeclName::TryGetSemanticField &&
        builtinName != KnownBuiltinDeclName::TrySetSemanticField)
    {
        return false;
    }

    const UInt argCount = call->getArgCount();
    UInt valueArgIndex = 0;
    UInt semanticNameArgIndex = 1;
    UInt semanticIndexArgIndex = UInt(-1);
    UInt resultOrValueArgIndex = UInt(-1);

    switch (builtinName)
    {
    case KnownBuiltinDeclName::HasSemanticField:
        if (argCount != 2 && argCount != 3)
            return false;
        if (argCount == 3)
            semanticIndexArgIndex = 2;
        break;

    case KnownBuiltinDeclName::TryGetSemanticField:
    case KnownBuiltinDeclName::TrySetSemanticField:
        if (argCount != 3 && argCount != 4)
            return false;
        resultOrValueArgIndex = argCount - 1;
        if (argCount == 4)
            semanticIndexArgIndex = 2;
        break;

    default:
        return false;
    }

    auto semanticNameLit = as<IRStringLit>(call->getArg(semanticNameArgIndex));
    if (!semanticNameLit)
        return false;

    int semanticIndex = 0;
    if (semanticIndexArgIndex != UInt(-1))
    {
        auto semanticIndexLit = as<IRIntLit>(call->getArg(semanticIndexArgIndex));
        if (!semanticIndexLit)
            return false;
        semanticIndex = int(semanticIndexLit->getValue());
    }

    IRBuilder builder(module);
    auto baseType = _getBaseLookupType(builder, call->getArg(valueArgIndex));
    auto lookup = _findSemanticFieldPath(
        baseType,
        semanticNameLit->getStringSlice(),
        semanticIndex,
        builtinName == KnownBuiltinDeclName::HasSemanticField ? 1 : 2);

    if (lookup.hasUnresolvedTypes)
        return false;

    builder.setInsertBefore(call);
    IRBuilderSourceLocRAII sourceLocScope(&builder, call->sourceLoc);

    if (builtinName == KnownBuiltinDeclName::HasSemanticField)
        return _replaceCallWithBool(builder, call, lookup.matchCount != 0);

    if (lookup.matchCount == 0)
        return _replaceCallWithBool(builder, call, false);

    if (lookup.matchCount > 1)
    {
        sink->diagnose(Diagnostics::SemanticFieldLookupAmbiguousIr{
            .location = call->sourceLoc,
            .semanticName = semanticNameLit->getStringSlice(),
            .semanticIndex = semanticIndex,
            .type = baseType});
        return _replaceCallWithBool(builder, call, false);
    }

    if (builtinName == KnownBuiltinDeclName::TryGetSemanticField)
    {
        auto resultPtr = call->getArg(resultOrValueArgIndex);
        auto resultType = tryGetPointedToType(
            &builder,
            (IRType*)unwrapAttributedType(resultPtr->getDataType()));
        if (!resultType || !_isTypeCompatible(resultType, lookup.path.fieldType))
        {
            sink->diagnose(Diagnostics::SemanticFieldTypeMismatchIr{
                .location = call->sourceLoc,
                .semanticName = semanticNameLit->getStringSlice(),
                .semanticIndex = semanticIndex,
                .fieldType = lookup.path.fieldType,
                .expectedType = resultType ? resultType : builder.getVoidType()});
            return _replaceCallWithBool(builder, call, false);
        }

        auto fieldValue = _emitFieldValueAtPath(builder, call->getArg(valueArgIndex), lookup.path);
        builder.emitStore(resultPtr, fieldValue);
        return _replaceCallWithBool(builder, call, true);
    }

    auto basePtr = call->getArg(valueArgIndex);
    auto basePtrValueType = tryGetPointedToType(
        &builder,
        (IRType*)unwrapAttributedType(basePtr->getDataType()));
    auto newValue = call->getArg(resultOrValueArgIndex);
    if (!basePtrValueType || !_isTypeCompatible((IRType*)newValue->getDataType(), lookup.path.fieldType))
    {
        sink->diagnose(Diagnostics::SemanticFieldTypeMismatchIr{
            .location = call->sourceLoc,
            .semanticName = semanticNameLit->getStringSlice(),
            .semanticIndex = semanticIndex,
            .fieldType = lookup.path.fieldType,
            .expectedType = (IRType*)unwrapAttributedType(newValue->getDataType())});
        return _replaceCallWithBool(builder, call, false);
    }

    auto fieldAddr = _emitFieldAddressAtPath(builder, basePtr, lookup.path);
    builder.emitStore(fieldAddr, newValue);
    return _replaceCallWithBool(builder, call, true);
}

static void _findSemanticFieldCallsRec(IRInst* inst, List<IRCall*>& outCalls)
{
    for (auto child = inst->getFirstDecorationOrChild(); child; child = child->getNextInst())
    {
        if (auto call = as<IRCall>(child))
        {
            auto builtinName = getBuiltinFuncEnum(call->getCallee());
            if (builtinName == KnownBuiltinDeclName::HasSemanticField ||
                builtinName == KnownBuiltinDeclName::TryGetSemanticField ||
                builtinName == KnownBuiltinDeclName::TrySetSemanticField)
            {
                outCalls.add(call);
            }
        }

        _findSemanticFieldCallsRec(child, outCalls);
    }
}

} // namespace

bool lowerSemanticFieldAccessBuiltins(IRModule* module, DiagnosticSink* sink)
{
    List<IRCall*> calls;
    _findSemanticFieldCallsRec(module->getModuleInst(), calls);

    bool changed = false;
    for (auto call : calls)
    {
        if (call->parent)
            changed |= _processSemanticFieldCall(module, call, sink);
    }
    return changed;
}

} // namespace Slang
