/*
 * Simple C99 API Example
 *
 * This example demonstrates using the simple C99 API for Slang.
 * It compiles a fragment shader with automatic bindless resources
 * and prints reflection information.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slang-c99.h"

/* Simple shader source for testing */
static const char* shaderSource =
    "// Simple fragment shader with bindless textures\n"
    "\n"
    "Texture2D<float4> diffuse;\n"
    "Texture2D<float4> normal;\n"
    "RWTexture2D<float4> output;\n"
    "SamplerState textureSampler;\n"
    "\n"
    "struct PSOutput\n"
    "{\n"
    "    float4 color : SV_Target0;\n"
    "    float4 normal : SV_Target1;\n"
    "};\n"
    "\n"
    "[shader(\"fragment\")]\n"
    "PSOutput main(float2 uv : TEXCOORD0)\n"
    "{\n"
    "    PSOutput result;\n"
    "    float4 d = diffuse.Sample(textureSampler, uv);\n"
    "    float4 n = normal.Sample(textureSampler, uv);\n"
    "    result.color = d * 0.5 + n * 0.5;\n"
    "    result.normal = n;\n"
    "    return result;\n"
    "}\n";

int fragmentOutputResolver(const char* outputName, void* userData)
{
    (void)userData;

    if (strcmp(outputName, "color") == 0)
    {
        printf("  Fragment output resolver: %s -> Location 3\n", outputName);
        return 3;
    }
    if (strcmp(outputName, "normal") == 0)
    {
        printf("  Fragment output resolver: %s -> Location 7\n", outputName);
        return 7;
    }

    printf("  Fragment output resolver: %s unresolved\n", outputName);
    return -1;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("=== Simple C99 API Example ===\n\n");

    /* Create global session (reusable across compilers on same thread) */
    SlangcGlobalSession globalSession = slangc_createGlobalSession();
    if (!globalSession)
    {
        printf("Failed to create global session\n");
        return 1;
    }

    /* Create compiler using the global session */
    SlangcCompiler compiler = slangc_createCompilerWithGlobalSession(globalSession);
    if (!compiler)
    {
        printf("Failed to create compiler\n");
        slangc_destroyGlobalSession(globalSession);
        return 1;
    }

    /* Configure for SPIRV assembly output */
    slangc_setTarget(compiler, SLANGC_TARGET_SPIRV_ASM);

    /* Load shader module */
    SlangcModule mod = slangc_loadModuleFromString(compiler, "shader", shaderSource);
    if (!mod)
    {
        printf("Failed to load module:\n%s\n", slangc_getCompilerErrors(compiler));
        slangc_destroyCompiler(compiler);
        slangc_destroyGlobalSession(globalSession);
        return 1;
    }

    printf("Module loaded successfully.\n\n");

    /* Create program */
    SlangcProgram program = slangc_createProgram(compiler);
    if (!program)
    {
        printf("Failed to create program\n");
        slangc_destroyCompiler(compiler);
        slangc_destroyGlobalSession(globalSession);
        return 1;
    }

    /* Add module and entry point */
    slangc_addModule(program, mod);
    slangc_addEntryPoint(program, mod, "main", SLANGC_STAGE_FRAGMENT);

    printf("Setting fragment output resolver callback...\n");
    slangc_setFragmentOutputResolver(compiler, fragmentOutputResolver, NULL);

    /* Link */
    printf("\nLinking program...\n");
    if (!slangc_link(program))
    {
        printf("Linking failed:\n%s\n", slangc_getProgramErrors(program));
        slangc_destroyProgram(program);
        slangc_destroyCompiler(compiler);
        slangc_destroyGlobalSession(globalSession);
        return 1;
    }

    printf("\nLinking successful!\n\n");

    /* Get SPIRV code */
    SlangcBlob spirv = slangc_getCode(program);
    if (spirv)
    {
        printf("SPIRV code size: %zu bytes\n\n", slangc_getBlobSize(spirv));
    }

    printf((char*) slangc_getBlobData(spirv));

    /* Print entry points */
    printf("Entry points (%d):\n", slangc_getEntryPointCount(program));
    for (int i = 0; i < slangc_getEntryPointCount(program); i++)
    {
        const char* name = slangc_getEntryPointName(program, i);
        SlangcStage stage = slangc_getEntryPointStage(program, i);
        const char* stageName = "unknown";
        switch (stage)
        {
        case SLANGC_STAGE_VERTEX: stageName = "vertex"; break;
        case SLANGC_STAGE_FRAGMENT: stageName = "fragment"; break;
        case SLANGC_STAGE_COMPUTE: stageName = "compute"; break;
        default: break;
        }
        printf("  [%d] %s (%s)\n", i, name, stageName);
    }
    printf("\n");

    /* Print all resources */
    printf("Resources (%d):\n", slangc_getResourceCount(program));
    for (int i = 0; i < slangc_getResourceCount(program); i++)
    {
        SlangcResourceInfo info;
        slangc_getResource(program, i, &info);
        printf("  %s (%s) set=%d binding=%d\n",
            info.name, info.typeName, info.set, info.binding);
    }
    printf("\n");

    /* Print automatic bindless resources */
    printf("Used automatic bindless bindings (%d):\n", slangc_getUsedBindingCount(program));
    for (int i = 0; i < slangc_getUsedBindingCount(program); i++)
    {
        SlangcResourceInfo info;
        slangc_getUsedBinding(program, i, &info);
        printf("  %s (%s) heap=set%d,binding%d -> indexBuffer[%d] count %d\n",
            info.name, info.typeName, info.set, info.binding, info.bindlessIndex, info.bindingCount);
    }
    printf("\n");

    /* ========================================
     * Demonstrate generic specialization
     * ======================================== */
    printf("=== Generic specialization example ===\n\n");

    static const char* genericShaderSource =
        "// Generic fragment shader with interface constraint\n"
        "interface IEffect { float4 apply(float4 c); }\n"
        "\n"
        "struct Grayscale : IEffect {\n"
        "    float4 apply(float4 c) {\n"
        "        float g = dot(c.rgb, float3(0.3, 0.59, 0.11));\n"
        "        return float4(g, g, g, c.a);\n"
        "    }\n"
        "};\n"
        "\n"
        "struct Sepia : IEffect {\n"
        "    float4 apply(float4 c) {\n"
        "        float3 sepia = float3(\n"
        "            dot(c.rgb, float3(0.393, 0.769, 0.189)),\n"
        "            dot(c.rgb, float3(0.349, 0.686, 0.168)),\n"
        "            dot(c.rgb, float3(0.272, 0.534, 0.131)));\n"
        "        return float4(sepia, c.a);\n"
        "    }\n"
        "};\n"
        "\n"
        "[shader(\"fragment\")]\n"
        "float4 effectMain<T:IEffect>(float2 uv : UV, uniform T effect) : SV_Target {\n"
        "    return effect.apply(float4(1, 0, 0, 1));\n"
        "}\n";

    SlangcModule genericMod = slangc_loadModuleFromString(compiler, "generic", genericShaderSource);
    if (genericMod)
    {
        SlangcProgram genericProgram = slangc_createProgram(compiler);
        slangc_addModule(genericProgram, genericMod);
        slangc_addEntryPoint(genericProgram, genericMod, "effectMain", SLANGC_STAGE_FRAGMENT);

        /* Query specialization parameter count before linking */
        int paramCount = slangc_getSpecializationParamCount(genericProgram);
        printf("Specialization parameters required: %d\n", paramCount);

        if (paramCount > 0)
        {
            /* Option 1: Use expression string (simplest) */
            printf("Specializing with Grayscale effect...\n");
            slangc_addSpecializationArgExpr(genericProgram, "Grayscale");

            if (slangc_link(genericProgram))
            {
                SlangcBlob genericSpirv = slangc_getCode(genericProgram);
                printf("Generic shader specialized and compiled successfully!\n");
                printf("SPIRV code size: %zu bytes\n\n", slangc_getBlobSize(genericSpirv));
                slangc_destroyBlob(genericSpirv);
            }
            else
            {
                printf("Specialization failed:\n%s\n", slangc_getProgramErrors(genericProgram));
            }

            /* Demonstrate reusing program with different specialization */
            printf("Respecializing with Sepia effect...\n");
            slangc_clearSpecializationArgs(genericProgram);

            /* Option 2: Use type lookup (allows programmatic type selection) */
            SlangcType sepiaType = slangc_findTypeByName(genericMod, "Sepia");
            if (sepiaType)
            {
                slangc_addSpecializationArgType(genericProgram, sepiaType);

                if (slangc_link(genericProgram))
                {
                    SlangcBlob sepiaSpirv = slangc_getCode(genericProgram);
                    printf("Sepia specialization successful!\n");
                    printf("SPIRV code size: %zu bytes\n\n", slangc_getBlobSize(sepiaSpirv));
                    slangc_destroyBlob(sepiaSpirv);
                }
                else
                {
                    printf("Sepia specialization failed:\n%s\n", slangc_getProgramErrors(genericProgram));
                }
            }
            else
            {
                printf("Could not find Sepia type\n");
            }
        }

        slangc_destroyProgram(genericProgram);
    }
    else
    {
        printf("Failed to load generic module:\n%s\n", slangc_getCompilerErrors(compiler));
    }

    /* ========================================
     * Demonstrate adding a compute shader
     * ======================================== */
    printf("=== Compute shader example ===\n\n");

    static const char* computeSource =
        "[shader(\"compute\")]\n"
        "[numthreads(8, 8, 1)]\n"
        "void computeMain(uint3 id : SV_DispatchThreadID) {}\n";

    SlangcModule computeMod = slangc_loadModuleFromString(compiler, "compute", computeSource);
    if (computeMod)
    {
        SlangcProgram computeProgram = slangc_createProgram(compiler);
        slangc_addModule(computeProgram, computeMod);
        slangc_addEntryPoint(computeProgram, computeMod, "computeMain", SLANGC_STAGE_COMPUTE);

        if (slangc_link(computeProgram))
        {
            SlangcBlob computeSpirv = slangc_getCode(computeProgram);
            printf("Compute shader compiled successfully!\n");
            printf("SPIRV code size: %zu bytes\n\n", slangc_getBlobSize(computeSpirv));
            slangc_destroyBlob(computeSpirv);
        }
        else
        {
            printf("Compute shader failed:\n%s\n", slangc_getProgramErrors(computeProgram));
        }

        slangc_destroyProgram(computeProgram);
    }

    /* Cleanup */
    if (spirv)
        slangc_destroyBlob(spirv);
    slangc_destroyProgram(program);
    slangc_destroyCompiler(compiler);
    slangc_destroyGlobalSession(globalSession);

    printf("Example completed successfully!\n");
    return 0;
}
