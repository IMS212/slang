// main.cpp
//
// This example demonstrates how to use the bindless resource lowering feature.
// When enabled, the compiler converts global resources (textures, buffers, samplers)
// to use descriptor handles that are looked up from an index buffer.
//
// This is useful for implementing bindless rendering where resources are accessed
// via indices into a global descriptor heap.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slang.h"
#include "slang-com-ptr.h"
#include "slang-com-helper.h"

using Slang::ComPtr;

// Helper to print diagnostics
static void diagnoseIfNeeded(slang::IBlob* diagnosticsBlob)
{
    if (diagnosticsBlob)
    {
        printf("%s\n", (const char*)diagnosticsBlob->getBufferPointer());
    }
}

int main(int argc, char** argv)
{
    // Create global session
    ComPtr<slang::IGlobalSession> globalSession;
    if (SLANG_FAILED(slang::createGlobalSession(globalSession.writeRef())))
    {
        printf("Failed to create global session\n");
        return 1;
    }

    // Create session with SPIRV target
    slang::SessionDesc sessionDesc = {};
    slang::TargetDesc targetDesc = {};
    targetDesc.format = SLANG_SPIRV;
    targetDesc.profile = globalSession->findProfile("spirv_1_5");

    sessionDesc.targets = &targetDesc;
    sessionDesc.targetCount = 1;

    ComPtr<slang::ISession> session;
    if (SLANG_FAILED(globalSession->createSession(sessionDesc, session.writeRef())))
    {
        printf("Failed to create session\n");
        return 1;
    }

    // Load the shader module
    ComPtr<slang::IBlob> diagnosticsBlob;
    slang::IModule* module = session->loadModule("shader.slang", diagnosticsBlob.writeRef());
    diagnoseIfNeeded(diagnosticsBlob);
    if (!module)
    {
        printf("Failed to load module\n");
        return 1;
    }

    // Find the entry point
    ComPtr<slang::IEntryPoint> entryPoint;
    if (SLANG_FAILED(module->findEntryPointByName("main", entryPoint.writeRef())))
    {
        printf("Failed to find entry point\n");
        return 1;
    }

    // Create composite component type
    slang::IComponentType* components[] = { module, entryPoint.get() };
    ComPtr<slang::IComponentType> composedProgram;
    if (SLANG_FAILED(session->createCompositeComponentType(
            components,
            2,
            composedProgram.writeRef(),
            diagnosticsBlob.writeRef())))
    {
        diagnoseIfNeeded(diagnosticsBlob);
        printf("Failed to compose program\n");
        return 1;
    }

    // =========================================================================
    // COMPILE AND GET RESULTS
    // =========================================================================
    // Link the program - this triggers the bindless lowering pass
    ComPtr<slang::IComponentType> linkedProgram;
    if (SLANG_FAILED(composedProgram->link(linkedProgram.writeRef(), diagnosticsBlob.writeRef())))
    {
        diagnoseIfNeeded(diagnosticsBlob);
        printf("Failed to link program\n");
        return 1;
    }

    // Get the compiled SPIRV code
    ComPtr<slang::IBlob> spirvCode;
    if (SLANG_FAILED(linkedProgram->getTargetCode(0, spirvCode.writeRef(), diagnosticsBlob.writeRef())))
    {
        diagnoseIfNeeded(diagnosticsBlob);
        printf("Failed to get target code\n");
        return 1;
    }

    printf("Successfully compiled shader with bindless resources!\n");
    printf("SPIRV code size: %zu bytes\n\n", spirvCode->getBufferSize());

    // =========================================================================
    // QUERY CONVERTED RESOURCES (via metadata)
    // =========================================================================
    ComPtr<slang::IMetadata> metadata;
    if (SLANG_SUCCEEDED(linkedProgram->getTargetMetadata(0, metadata.writeRef(), nullptr)))
    {
        ComPtr<slang::IBindlessResourceUsageMetadata> bindlessMetadata;
        if (SLANG_SUCCEEDED(metadata->queryInterface(
                slang::IBindlessResourceUsageMetadata::getTypeGuid(),
                (void**)bindlessMetadata.writeRef())))
        {
            auto resourceCount = bindlessMetadata->getBindlessResourceUsageCount();
            printf("Converted resources (%u total):\n", (unsigned)resourceCount);
            for (SlangUInt i = 0; i < resourceCount; i++)
            {
                slang::BindlessResourceUsageInfo info;
                if (SLANG_FAILED(bindlessMetadata->getBindlessResourceUsage(i, &info)))
                    continue;
                printf("  [%u] %s (%s) heap=set%lld,binding%lld -> indexBuffer[%lld] count %lld\n",
                    (unsigned)i,
                    info.name,
                    info.typeName,
                    (long long)info.set,
                    (long long)info.binding,
                    (long long)info.index,
                    (long long)info.bindingCount);
            }
            printf("\n");
        }

        printf("The generated SPIRV will:\n");
        printf("  1. Declare descriptor arrays at set 0 (samplers at binding 0, textures at binding 2)\n");
        printf("  2. Declare ConstantBuffer<uint[4000]> at set 2, binding 0 for descriptor indices\n");
        printf("  3. Load descriptor indices from indexBuffer slots before accessing descriptor arrays\n");
    }

    printf("\nExample completed successfully!\n");
    return 0;
}
