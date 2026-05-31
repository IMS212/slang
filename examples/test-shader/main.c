/*
 * Simple C99 API Example
 *
 * This example demonstrates using the simple C99 API for Slang.
 * It compiles shaders with automatic bindless resources and prints reflection information.
 */

#include "slang-c99.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
int fragmentOutputResolver(const char* output_name, void* user_data)
{
    printf("%s\n", output_name);
    if (strstr(output_name, "norm") !=  NULL) return 1;
    return 3;
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
    slangc_setFragmentOutputResolver(compiler, fragmentOutputResolver, NULL);

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
