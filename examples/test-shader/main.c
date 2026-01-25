/*
 * Simple C99 API Example
 *
 * This example demonstrates using the simple C99 API for Slang.
 * It compiles a fragment shader with bindless resources using
 * the resolver callback, and prints reflection information.
 */

#include "slang-c99.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simple shader source for testing */
/*
 * Bindless resolver callback
 *
 * Called during linking to resolve index buffer slots for resources.
 * The returned index is a slot in the index buffer (StructuredBuffer<uint2>
 * at set 1, binding 3). At runtime, you fill indexBuffer[slot].x with the
 * actual descriptor heap index.
 *
 * The resourceType tells you which descriptor heap binding (0-5) this
 * resource will use, so you can set up the corresponding heap entry.
 *
 * Returns -1 to skip a resource (leave it as regular binding).
 */
static int nextIndexBufferSlot = 0;

int bindlessResolver(
    const char* resourceName,
    SlangcBindlessResourceType resourceType,
    void* userData)
{
    (void)userData;

    const char* typeName = "unknown";
    int heapBinding = -1;

    switch (resourceType)
    {
    case SLANGC_BINDLESS_SAMPLER:
        typeName = "Sampler";
        heapBinding = 0;
        break;
    case SLANGC_BINDLESS_COMBINED_TEXTURE_SAMPLER:
        typeName = "CombinedTextureSampler";
        heapBinding = 1;
        break;
    case SLANGC_BINDLESS_SAMPLED_IMAGE:
        typeName = "SampledImage";
        heapBinding = 2;
        break;
    case SLANGC_BINDLESS_STORAGE_IMAGE:
        typeName = "StorageImage";
        heapBinding = 3;
        break;
    case SLANGC_BINDLESS_UNIFORM_BUFFER:
        typeName = "UniformBuffer";
        heapBinding = 4;
        break;
    case SLANGC_BINDLESS_STORAGE_BUFFER:
        typeName = "StorageBuffer";
        heapBinding = 5;
        break;
    default:
        break;
    }

    /* Allocate a slot in the index buffer for this resource */
    int slot = nextIndexBufferSlot++;

    printf("  Resolver: %s (%s, heap binding %d) -> index buffer slot %d\n",
           resourceName, typeName, heapBinding, slot);

    /*
     * At runtime, you would:
     * 1. Allocate a descriptor in the appropriate heap (binding heapBinding)
     * 2. Write the descriptor's heap index to indexBuffer[slot].x
     *
     * For example, if 'diffuse' gets slot 0 and you put it at heap index 42:
     *   indexBuffer[0] = uint2(42, 0);
     * Then the shader will load from textureHeap[42].
     */

    return slot;
}


long slurp(char const* path, char **buf, bool add_nul)
{
    FILE  *fp;
    size_t fsz;
    long   off_end;
    int    rc;

    /* Open the file */
    fp = fopen(path, "rb");
    if( NULL == fp ) {
        return -1L;
    }

    /* Seek to the end of the file */
    rc = fseek(fp, 0L, SEEK_END);
    if( 0 != rc ) {
        return -1L;
    }

    /* Byte offset to the end of the file (size) */
    if( 0 > (off_end = ftell(fp)) ) {
        return -1L;
    }
    fsz = (size_t)off_end;

    /* Allocate a buffer to hold the whole file */
    *buf = malloc( fsz+(int)add_nul );
    if( NULL == *buf ) {
        return -1L;
    }

    /* Rewind file pointer to start of file */
    rewind(fp);

    /* Slurp file into buffer */
    if( fsz != fread(*buf, 1, fsz, fp) ) {
        free(*buf);
        return -1L;
    }

    /* Close the file */
    if( EOF == fclose(fp) ) {
        free(*buf);
        return -1L;
    }

    if( add_nul ) {
        (*buf)[fsz] = '\0';
    }

    /* Return the file size */
    return (long)fsz;
}


SlangcModule loadMod(SlangcCompiler compiler, char* str, char* text)
{
    char* buf = 0;
    slurp(text, &buf, true);
    SlangcModule mod = slangc_loadModuleFromString(compiler, str, buf);
    if (!mod)
    {
        printf("Failed to load module:\n%s\n", slangc_getCompilerErrors(compiler));
    }
    return mod;
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

    /* Configure for GLSL output */
    slangc_setTarget(compiler, SLANGC_TARGET_SPIRV_ASM);

    /* Load shader module */
    SlangcModule engine = loadMod(compiler, "engine", "D:\\slang\\examples\\test-shader\\engine.slang");
    SlangcModule object = loadMod(compiler, "object", "D:\\slang\\examples\\test-shader\\object.slang");
    SlangcModule shader = loadMod(compiler, "shader", "D:\\slang\\examples\\test-shader\\shader.slang");

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

    slangc_addModule(program, engine);
    slangc_addModule(program, object);
    slangc_addModule(program, shader);
    slangc_addEntryPoint(program, engine, "vertexMain", SLANGC_STAGE_VERTEX);
    slangc_addEntryPoint(program, engine, "fragmentMain", SLANGC_STAGE_FRAGMENT);
    slangc_setBindlessResolver(compiler, bindlessResolver, NULL);

    int paramCount = slangc_getSpecializationParamCount(program);
    printf("Specialization parameters required: %d\n", paramCount);

    /* Use named specialization args - O is inferred from S : Shader<O> constraint */
    slangc_setSpecializationArg(program, "In", "RealVertexInput");
    slangc_setSpecializationArg(program, "V", "RealVertex");
    slangc_setSpecializationArg(program, "S", "MyShaderPos");
    /* No need to specify O (MyShaderOut) - inferred from MyShader : Shader<MyShaderOut> */

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
        size_t spirvSize = slangc_getBlobSize(spirv);
        printf("SPIRV code size: %zu bytes\n\n", spirvSize);

        /* Write SPIR-V to file */
        const char* outputPath = "D:\\slang\\examples\\test-shader\\object.spv";
        FILE* outFile = fopen(outputPath, "wb");
        if (outFile)
        {
            size_t written = fwrite(slangc_getBlobData(spirv), 1, spirvSize, outFile);
            fclose(outFile);
            if (written == spirvSize)
            {
                printf("SPIR-V written to %s\n", outputPath);
            }
            else
            {
                printf("Error: only wrote %zu of %zu bytes to %s\n", written, spirvSize, outputPath);
            }
        }
        else
        {
            printf("Error: failed to open %s for writing\n", outputPath);
        }
    }
    //printf((char*) slangc_getBlobData(spirv));

    slangc_destroyProgram(program);
    slangc_destroyCompiler(compiler);
    slangc_destroyGlobalSession(globalSession);

    printf("Example completed successfully!\n");
    return 0;
}
