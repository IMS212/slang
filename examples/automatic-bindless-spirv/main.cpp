#include <stdio.h>

#include "slang-com-helper.h"
#include "slang-com-ptr.h"
#include "slang.h"

using Slang::ComPtr;

static void diagnoseIfNeeded(slang::IBlob* diagnosticsBlob)
{
    if (diagnosticsBlob)
        printf("%s\n", (const char*)diagnosticsBlob->getBufferPointer());
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    ComPtr<slang::IGlobalSession> globalSession;
    if (SLANG_FAILED(slang::createGlobalSession(globalSession.writeRef())))
    {
        printf("Failed to create global session\n");
        return 1;
    }

    slang::TargetDesc targetDesc = {};
    targetDesc.format = SLANG_SPIRV_ASM;
    targetDesc.profile = globalSession->findProfile("spirv_1_5");

    slang::SessionDesc sessionDesc = {};
    sessionDesc.targets = &targetDesc;
    sessionDesc.targetCount = 1;

    ComPtr<slang::ISession> session;
    if (SLANG_FAILED(globalSession->createSession(sessionDesc, session.writeRef())))
    {
        printf("Failed to create session\n");
        return 1;
    }

    ComPtr<slang::IBlob> diagnosticsBlob;
    slang::IModule* module = session->loadModule("shader.slang", diagnosticsBlob.writeRef());
    diagnoseIfNeeded(diagnosticsBlob);
    if (!module)
    {
        printf("Failed to load shader.slang\n");
        return 1;
    }

    ComPtr<slang::IEntryPoint> entryPoint;
    if (SLANG_FAILED(module->findEntryPointByName("main", entryPoint.writeRef())))
    {
        printf("Failed to find entry point\n");
        return 1;
    }

    slang::IComponentType* components[] = {module, entryPoint.get()};
    ComPtr<slang::IComponentType> program;
    if (SLANG_FAILED(session->createCompositeComponentType(
            components,
            SLANG_COUNT_OF(components),
            program.writeRef(),
            diagnosticsBlob.writeRef())))
    {
        diagnoseIfNeeded(diagnosticsBlob);
        printf("Failed to compose program\n");
        return 1;
    }

    ComPtr<slang::IComponentType> linkedProgram;
    if (SLANG_FAILED(program->link(linkedProgram.writeRef(), diagnosticsBlob.writeRef())))
    {
        diagnoseIfNeeded(diagnosticsBlob);
        printf("Failed to link program\n");
        return 1;
    }

    ComPtr<slang::IBlob> spirvAsm;
    if (SLANG_FAILED(linkedProgram->getTargetCode(
            0,
            spirvAsm.writeRef(),
            diagnosticsBlob.writeRef())))
    {
        diagnoseIfNeeded(diagnosticsBlob);
        printf("Failed to emit SPIR-V assembly\n");
        return 1;
    }

    printf("Automatic bindless resources:\n");
    ComPtr<slang::IMetadata> metadata;
    if (SLANG_SUCCEEDED(linkedProgram->getTargetMetadata(0, metadata.writeRef(), nullptr)))
    {
        ComPtr<slang::IBindlessResourceUsageMetadata> bindlessMetadata;
        if (SLANG_SUCCEEDED(metadata->queryInterface(
                slang::IBindlessResourceUsageMetadata::getTypeGuid(),
                (void**)bindlessMetadata.writeRef())))
        {
            auto count = bindlessMetadata->getBindlessResourceUsageCount();
            for (SlangUInt i = 0; i < count; ++i)
            {
                slang::BindlessResourceUsageInfo info;
                if (SLANG_FAILED(bindlessMetadata->getBindlessResourceUsage(i, &info)))
                    continue;

                printf(
                    "  [%u] %s (%s) heap=set%lld,binding%lld -> indexBuffer[%lld] count %lld\n",
                    (unsigned)i,
                    info.name,
                    info.typeName,
                    (long long)info.set,
                    (long long)info.binding,
                    (long long)info.index,
                    (long long)info.bindingCount);
            }
        }
    }

    printf("\nGenerated SPIR-V assembly:\n");
    printf("%.*s\n", (int)spirvAsm->getBufferSize(), (const char*)spirvAsm->getBufferPointer());
    return 0;
}
