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

// Internal header for IArtifactPostEmitMetadata (provides getBindlessConvertedResources)
#include "../../source/compiler-core/slang-artifact-associated.h"

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
    // BINDLESS RESOURCE CONFIGURATION
    // =========================================================================
    // Query for IComponentType3 interface which provides bindless resource support
    ComPtr<slang::IComponentType3> comp3;
    if (SLANG_FAILED(composedProgram->queryInterface(
            slang::IComponentType3::getTypeGuid(),
            (void**)comp3.writeRef())))
    {
        printf("IComponentType3 interface not available\n");
        return 1;
    }

    // Define the mapping from resource names to bindless index buffer indices.
    // In a real application, these indices would correspond to positions in your
    // bindless descriptor heap.
    // Note: For struct members, use the original field name (e.g., "diffuse").
    // It will match the hoisted name (e.g., "materialTextures_diffuse") via suffix matching.
    const char* resourceNames[] = {
        "diffuse",          // Index 0 - matches MaterialTextures.diffuse
        "normal",           // Index 1 - matches MaterialTextures.normal
        "textureSampler",   // Index 2 in bindless heap
    };
    SlangInt resourceIndices[] = { 0, 1, 2 };

    // Set the bindless resource index map for target 0 (SPIRV)
    if (SLANG_FAILED(comp3->setBindlessResourceIndexMap(
            0,  // targetIndex
            resourceNames,
            resourceIndices,
            3   // count
        )))
    {
        printf("Failed to set bindless resource index map\n");
        return 1;
    }

    printf("Bindless resource index map configured:\n");
    for (int i = 0; i < 3; i++)
    {
        printf("  %s -> index %lld\n", resourceNames[i], (long long)resourceIndices[i]);
    }
    printf("\n");

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
        // Query for the extended metadata interface
        ComPtr<Slang::IArtifactPostEmitMetadata> postEmitMetadata;
        if (SLANG_SUCCEEDED(metadata->queryInterface(
                Slang::IArtifactPostEmitMetadata::getTypeGuid(),
                (void**)postEmitMetadata.writeRef())))
        {
            // Get the list of resources that were converted to bindless
            auto convertedResources = postEmitMetadata->getBindlessConvertedResources();

            printf("Converted resources (%d total):\n", (int)convertedResources.count);
            for (Slang::Index i = 0; i < convertedResources.count; i++)
            {
                const auto& info = convertedResources[i];
                printf("  [%d] %s (%s) -> bindless index %lld\n",
                    (int)i,
                    info.name.begin(),
                    info.typeName.begin(),
                    (long long)info.index);
            }
            printf("\n");
        }

        printf("The generated SPIRV will:\n");
        printf("  1. Declare descriptor arrays at set 2 (samplers at binding 0, textures at binding 2)\n");
        printf("  2. Declare a StructuredBuffer<uint2> at set 1, binding 3 for indices\n");
        printf("  3. Load indices from the buffer and use them to access descriptor arrays\n");
    }

    printf("\nExample completed successfully!\n");
    return 0;
}