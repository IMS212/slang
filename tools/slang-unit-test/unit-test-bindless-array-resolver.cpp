// unit-test-bindless-array-resolver.cpp

#include "slang-com-ptr.h"
#include "slang.h"
#include "unit-test/slang-unit-test.h"

#include <string.h>

using namespace Slang;

struct ArrayResolverState
{
    int callCount = 0;
    int shaderArrayLength = -1;
    int resolvedArrayLength = -1;
    int baseIndex = -1;
    String lastResourceName;
};

static int testArrayResolverCallback(
    const char* resourceName,
    slang::SlangBindlessResourceType,
    int shaderArrayLength,
    int* outResolvedArrayLength,
    void* userData)
{
    auto* state = static_cast<ArrayResolverState*>(userData);
    if (!state)
        return -1;

    state->callCount++;
    state->shaderArrayLength = shaderArrayLength;
    state->lastResourceName = resourceName ? resourceName : "";
    if (outResolvedArrayLength)
        *outResolvedArrayLength = state->resolvedArrayLength;
    return state->baseIndex;
}

SLANG_UNIT_TEST(bindlessArrayResolverPreservesNonUniform)
{
    const char* source = R"(
        Texture2D<float4> textures[4];
        RWStructuredBuffer<float4> output;

        [shader("compute")]
        [numthreads(1, 1, 1)]
        void computeMain(uint3 tid : SV_DispatchThreadID)
        {
            uint idx = NonUniformResourceIndex(tid.x & 3);
            output[0] = textures[idx].Load(int3(0, 0, 0));
        }
    )";

    ComPtr<slang::IGlobalSession> globalSession;
    SLANG_CHECK(slang_createGlobalSession(SLANG_API_VERSION, globalSession.writeRef()) == SLANG_OK);

    slang::TargetDesc targetDesc = {};
    targetDesc.format = SLANG_SPIRV_ASM;
    targetDesc.profile = globalSession->findProfile("spirv_1_5");

    slang::SessionDesc sessionDesc = {};
    sessionDesc.targetCount = 1;
    sessionDesc.targets = &targetDesc;

    ComPtr<slang::ISession> session;
    SLANG_CHECK(globalSession->createSession(sessionDesc, session.writeRef()) == SLANG_OK);

    ComPtr<slang::IBlob> diagnostics;
    auto module = session->loadModuleFromSourceString(
        "array_nonuniform",
        "array_nonuniform.slang",
        source,
        diagnostics.writeRef());
    SLANG_CHECK(module != nullptr);

    ComPtr<slang::IEntryPoint> entryPoint;
    SLANG_CHECK(module->findEntryPointByName("computeMain", entryPoint.writeRef()) == SLANG_OK);
    SLANG_CHECK(entryPoint != nullptr);

    ComPtr<slang::IComponentType> compositeProgram;
    slang::IComponentType* components[] = { module, entryPoint.get() };
    SLANG_CHECK(
        session->createCompositeComponentType(
            components,
            2,
            compositeProgram.writeRef(),
            diagnostics.writeRef()) == SLANG_OK);

    ComPtr<slang::IComponentType> linkedProgram;
    SLANG_CHECK(compositeProgram->link(linkedProgram.writeRef(), diagnostics.writeRef()) == SLANG_OK);

    ComPtr<slang::IComponentType4> linkedComp4;
    SLANG_CHECK(SLANG_SUCCEEDED(linkedProgram->queryInterface(
        slang::IComponentType4::getTypeGuid(),
        (void**)linkedComp4.writeRef())));

    ArrayResolverState state = {};
    state.baseIndex = 17;
    state.resolvedArrayLength = 4;
    SLANG_CHECK(
        linkedComp4->setBindlessArrayResolver(0, testArrayResolverCallback, &state) == SLANG_OK);

    ComPtr<slang::IBlob> code;
    SLANG_CHECK(linkedProgram->getTargetCode(0, code.writeRef(), diagnostics.writeRef()) == SLANG_OK);
    SLANG_CHECK(code != nullptr);
    SLANG_CHECK(state.callCount >= 1);
    SLANG_CHECK(state.shaderArrayLength == 4);

    const char* codeText = (const char*)code->getBufferPointer();
    SLANG_CHECK(codeText != nullptr);
    SLANG_CHECK(strstr(codeText, "NonUniform") != nullptr);
}

SLANG_UNIT_TEST(bindlessArrayResolverLengthMismatch)
{
    const char* source = R"(
        Texture2D<float4> textures[4];
        RWStructuredBuffer<float4> output;

        [shader("compute")]
        [numthreads(1, 1, 1)]
        void computeMain(uint3 tid : SV_DispatchThreadID)
        {
            output[0] = textures[tid.x & 3].Load(int3(0, 0, 0));
        }
    )";

    ComPtr<slang::IGlobalSession> globalSession;
    SLANG_CHECK(slang_createGlobalSession(SLANG_API_VERSION, globalSession.writeRef()) == SLANG_OK);

    slang::TargetDesc targetDesc = {};
    targetDesc.format = SLANG_SPIRV_ASM;
    targetDesc.profile = globalSession->findProfile("spirv_1_5");

    slang::SessionDesc sessionDesc = {};
    sessionDesc.targetCount = 1;
    sessionDesc.targets = &targetDesc;

    ComPtr<slang::ISession> session;
    SLANG_CHECK(globalSession->createSession(sessionDesc, session.writeRef()) == SLANG_OK);

    ComPtr<slang::IBlob> diagnostics;
    auto module = session->loadModuleFromSourceString(
        "array_mismatch",
        "array_mismatch.slang",
        source,
        diagnostics.writeRef());
    SLANG_CHECK(module != nullptr);

    ComPtr<slang::IEntryPoint> entryPoint;
    SLANG_CHECK(module->findEntryPointByName("computeMain", entryPoint.writeRef()) == SLANG_OK);
    SLANG_CHECK(entryPoint != nullptr);

    ComPtr<slang::IComponentType> compositeProgram;
    slang::IComponentType* components[] = { module, entryPoint.get() };
    SLANG_CHECK(
        session->createCompositeComponentType(
            components,
            2,
            compositeProgram.writeRef(),
            diagnostics.writeRef()) == SLANG_OK);

    ComPtr<slang::IComponentType> linkedProgram;
    SLANG_CHECK(compositeProgram->link(linkedProgram.writeRef(), diagnostics.writeRef()) == SLANG_OK);

    ComPtr<slang::IComponentType4> linkedComp4;
    SLANG_CHECK(SLANG_SUCCEEDED(linkedProgram->queryInterface(
        slang::IComponentType4::getTypeGuid(),
        (void**)linkedComp4.writeRef())));

    ArrayResolverState state = {};
    state.baseIndex = 2;
    state.resolvedArrayLength = 3;
    SLANG_CHECK(
        linkedComp4->setBindlessArrayResolver(0, testArrayResolverCallback, &state) == SLANG_OK);

    ComPtr<slang::IBlob> code;
    auto result = linkedProgram->getTargetCode(0, code.writeRef(), diagnostics.writeRef());
    SLANG_CHECK(SLANG_FAILED(result));
    SLANG_CHECK(diagnostics != nullptr);

    const char* diagnosticText = (const char*)diagnostics->getBufferPointer();
    SLANG_CHECK(diagnosticText != nullptr);
    SLANG_CHECK(strstr(diagnosticText, "bindless array resolver returned length") != nullptr);
}

SLANG_UNIT_TEST(bindlessArrayScalarConflictWarning)
{
    const char* source = R"(
        struct ScalarContainer
        {
            Texture2D<float4> shadowMap;
        };

        struct ArrayContainer
        {
            Texture2D<float4> shadowMap[4];
        };

        ScalarContainer gScalar;
        ArrayContainer gArray;
        RWStructuredBuffer<float4> output;

        [shader("compute")]
        [numthreads(1, 1, 1)]
        void computeMain(uint3 tid : SV_DispatchThreadID)
        {
            float4 s = gScalar.shadowMap.Load(int3(0, 0, 0));
            float4 a = gArray.shadowMap[tid.x & 3].Load(int3(0, 0, 0));
            output[0] = s + a;
        }
    )";

    ComPtr<slang::IGlobalSession> globalSession;
    SLANG_CHECK(slang_createGlobalSession(SLANG_API_VERSION, globalSession.writeRef()) == SLANG_OK);

    slang::TargetDesc targetDesc = {};
    targetDesc.format = SLANG_SPIRV_ASM;
    targetDesc.profile = globalSession->findProfile("spirv_1_5");

    slang::SessionDesc sessionDesc = {};
    sessionDesc.targetCount = 1;
    sessionDesc.targets = &targetDesc;

    ComPtr<slang::ISession> session;
    SLANG_CHECK(globalSession->createSession(sessionDesc, session.writeRef()) == SLANG_OK);

    ComPtr<slang::IBlob> diagnostics;
    auto module = session->loadModuleFromSourceString(
        "array_scalar_conflict",
        "array_scalar_conflict.slang",
        source,
        diagnostics.writeRef());
    SLANG_CHECK(module != nullptr);

    ComPtr<slang::IEntryPoint> entryPoint;
    SLANG_CHECK(module->findEntryPointByName("computeMain", entryPoint.writeRef()) == SLANG_OK);
    SLANG_CHECK(entryPoint != nullptr);

    ComPtr<slang::IComponentType> compositeProgram;
    slang::IComponentType* components[] = { module, entryPoint.get() };
    SLANG_CHECK(
        session->createCompositeComponentType(
            components,
            2,
            compositeProgram.writeRef(),
            diagnostics.writeRef()) == SLANG_OK);

    ComPtr<slang::IComponentType> linkedProgram;
    SLANG_CHECK(compositeProgram->link(linkedProgram.writeRef(), diagnostics.writeRef()) == SLANG_OK);

    ComPtr<slang::IComponentType3> linkedComp3;
    SLANG_CHECK(SLANG_SUCCEEDED(linkedProgram->queryInterface(
        slang::IComponentType3::getTypeGuid(),
        (void**)linkedComp3.writeRef())));

    const char* names[] = { "shadowMap" };
    SlangInt indices[] = { 5 };
    SLANG_CHECK(linkedComp3->setBindlessResourceIndexMap(0, names, indices, 1) == SLANG_OK);

    ComPtr<slang::IBlob> code;
    SLANG_CHECK(linkedProgram->getTargetCode(0, code.writeRef(), diagnostics.writeRef()) == SLANG_OK);
    SLANG_CHECK(diagnostics != nullptr);

    const char* diagnosticText = (const char*)diagnostics->getBufferPointer();
    SLANG_CHECK(diagnosticText != nullptr);
    SLANG_CHECK(strstr(diagnosticText, "only a scalar bindless binding exists") != nullptr);
}

