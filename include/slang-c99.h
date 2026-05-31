/*
 * slang-c99.h - Simple C99 API for Slang Shader Compiler
 *
 * This header provides a simple, C99-compatible API for compiling shaders
 * with Slang. It is designed to work well with FFI systems like Java Panama.
 *
 * Key features:
 * - Opaque handles (void*) for all objects
 * - Simple flat structs suitable for FFI
 * - Reusable compiler with module caching
 * - Bindless resource support
 * - Basic reflection info
 */

#ifndef SLANG_C99_H
#define SLANG_C99_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Platform detection and export macros */
#if defined(_MSC_VER)
#   define SLANGC_API __declspec(dllexport)
#elif defined(__GNUC__)
#   define SLANGC_API __attribute__((visibility("default")))
#else
#   define SLANGC_API
#endif

/*
 * Opaque handle types
 */
typedef void* SlangcGlobalSession; /* Global session (reusable across compilers, one per thread) */
typedef void* SlangcCompiler;      /* Compiler context / session */
typedef void* SlangcModule;        /* Loaded module (reusable across programs) */
typedef void* SlangcProgram;       /* Program being built (modules + entry points) */
typedef void* SlangcBlob;          /* Binary data (SPIR-V, etc.) */
typedef void* SlangcType;          /* Type handle for specialization */

/*
 * Target format for code generation
 */
typedef enum SlangcTarget {
    SLANGC_TARGET_SPIRV = 0,
    SLANGC_TARGET_SPIRV_ASM = 1,
    SLANGC_TARGET_GLSL = 2,
    SLANGC_TARGET_HLSL = 3,
} SlangcTarget;

/*
 * Shader stage
 */
typedef enum SlangcStage {
    SLANGC_STAGE_VERTEX = 0,
    SLANGC_STAGE_FRAGMENT = 1,
    SLANGC_STAGE_COMPUTE = 2,
    SLANGC_STAGE_GEOMETRY = 3,
    SLANGC_STAGE_HULL = 4,
    SLANGC_STAGE_DOMAIN = 5,
    SLANGC_STAGE_RAYGEN = 6,
    SLANGC_STAGE_INTERSECTION = 7,
    SLANGC_STAGE_ANYHIT = 8,
    SLANGC_STAGE_CLOSESTHIT = 9,
    SLANGC_STAGE_MISS = 10,
    SLANGC_STAGE_CALLABLE = 11,
    SLANGC_STAGE_MESH = 12,
    SLANGC_STAGE_AMPLIFICATION = 13,
} SlangcStage;

/*
 * Resource access mode
 */
typedef enum SlangcResourceAccess {
    SLANGC_ACCESS_READ = 0,
    SLANGC_ACCESS_WRITE = 1,
    SLANGC_ACCESS_READ_WRITE = 2,
} SlangcResourceAccess;

/*
 * Resource object type
 */
typedef enum SlangcResourceObjectType {
    SLANGC_RESOURCE_OBJECT_UNKNOWN = -1,
    SLANGC_RESOURCE_OBJECT_COMBINED_TEXTURE_SAMPLER = 0,
    SLANGC_RESOURCE_OBJECT_SAMPLER = 1,
    SLANGC_RESOURCE_OBJECT_STORAGE_IMAGE = 2,
    SLANGC_RESOURCE_OBJECT_SAMPLED_IMAGE = 3,
    SLANGC_RESOURCE_OBJECT_STORAGE_BUFFER = 4,
    SLANGC_RESOURCE_OBJECT_UNIFORM_BUFFER = 5,
    SLANGC_RESOURCE_OBJECT_ACCELERATION_STRUCTURE = 6,
} SlangcResourceObjectType;

/*
 * Resource information (flat struct for FFI compatibility)
 * String pointers are valid until the program is destroyed.
 */
typedef struct SlangcResourceInfo {
    const char* name;       /* Resource name */
    const char* typeName;   /* Type name (e.g., "Texture2D", "SamplerState") */
    int set;                /* Descriptor set (-1 if not applicable). For bindless resources, this is the heap set. */
    int binding;            /* Binding number (-1 if not applicable). For bindless resources, this is the heap binding. */
    int bindlessIndex;      /* Base slot in the generated bindless index buffer (-1 if not bindless) */
    int bindingCount;       /* Number of index-buffer slots required by this binding. -1 for unsized arrays. */
    SlangcResourceObjectType objectType;  /* Resource object type */
    int isArray;            /* Non-zero if this resource is an array */
    int arraySize;          /* -1 for unsized arrays, otherwise the total array element count. 0 if not an array. */
    SlangcResourceAccess access;  /* Access mode (read, write, or read-write) */
} SlangcResourceInfo;

/*
 * Fragment output resolver callback.
 * Called during SPIR-V emission for automatic fragment color output locations.
 * Return the SPIR-V Location for this output, or < 0 if unresolved.
 */
typedef int (*SlangcFragmentOutputResolverCallback)(
    const char* outputName,
    void* userData
);

/*
 * Bindless array size resolver callback.
 * Called during automatic bindless lowering for unsized resource arrays.
 * Return the runtime array length/index-buffer slot count, or <= 0 if unresolved.
 */
typedef int (*SlangcBindlessArraySizeResolverCallback)(
    const char* resourceName,
    SlangcResourceObjectType resourceType,
    void* userData
);

/*
 * File loading callback.
 * Returns pointer to file contents and sets *outSize.
 * Returns NULL on failure.
 * Memory is owned by the callback - Slang will copy the contents.
 */
typedef const char* (*SlangcLoadFileCallback)(
    const char* path,
    size_t* outSize,
    void* userData
);

/*
 * Global session lifecycle
 *
 * A global session can be shared across multiple compilers on the same thread.
 * This is useful for reusing cached state and reducing initialization overhead.
 * Global sessions are NOT thread-safe - use one per thread.
 */

/* Create a new global session. Returns handle or NULL on error. */
SLANGC_API SlangcGlobalSession slangc_createGlobalSession(void);

/* Destroy a global session. All compilers using it must be destroyed first. */
SLANGC_API void slangc_destroyGlobalSession(SlangcGlobalSession globalSession);

/*
 * Compiler lifecycle
 */

/* Create a new compiler with its own global session */
SLANGC_API SlangcCompiler slangc_createCompiler(void);

/* Create a new compiler using an existing global session */
SLANGC_API SlangcCompiler slangc_createCompilerWithGlobalSession(SlangcGlobalSession globalSession);

/* Destroy compiler and all associated resources */
SLANGC_API void slangc_destroyCompiler(SlangcCompiler compiler);

/*
 * Compiler configuration
 */

/* Set the target format (SPIRV, GLSL, etc.) */
SLANGC_API void slangc_setTarget(SlangcCompiler compiler, SlangcTarget target);

/* Set a custom file loader callback */
SLANGC_API void slangc_setFileLoader(
    SlangcCompiler compiler,
    SlangcLoadFileCallback callback,
    void* userData
);

/* Add a search path for #include and import */
SLANGC_API void slangc_addSearchPath(SlangcCompiler compiler, const char* path);

/*
 * Module loading
 *
 * Modules are loaded/compiled once and can be reused across multiple programs.
 * Modules are owned by the compiler and freed when the compiler is destroyed.
 */

/* Load module from source string. Returns module handle or NULL on error. */
SLANGC_API SlangcModule slangc_loadModuleFromString(
    SlangcCompiler compiler,
    const char* moduleName,
    const char* source
);

/* Load module from file. Returns module handle or NULL on error. */
SLANGC_API SlangcModule slangc_loadModuleFromFile(
    SlangcCompiler compiler,
    const char* path
);

/* Get the number of file-backed dependencies for this module, including transitive imports/includes. */
SLANGC_API int slangc_getModuleDependencyFileCount(SlangcModule module);

/* Get the resolved file path for a dependency by index. Returns NULL on invalid input/index. */
SLANGC_API const char* slangc_getModuleDependencyFilePath(SlangcModule module, int index);

/*
 * Program creation and linking
 *
 * A program combines modules with entry points and configuration.
 * Multiple programs can be created from the same modules.
 */

/* Create a new program. Returns program handle or NULL on error. */
SLANGC_API SlangcProgram slangc_createProgram(SlangcCompiler compiler);

/* Destroy a program */
SLANGC_API void slangc_destroyProgram(SlangcProgram program);

/* Add a module to the program */
SLANGC_API void slangc_addModule(SlangcProgram program, SlangcModule module);

/* Add an entry point from a module */
SLANGC_API void slangc_addEntryPoint(
    SlangcProgram program,
    SlangcModule module,
    const char* name,
    SlangcStage stage
);

/*
 * Set a fragment output resolver callback.
 * This callback resolves SPIR-V fragment color output names to Location decorations.
 */
SLANGC_API void slangc_setFragmentOutputResolver(
    SlangcCompiler compiler,
    SlangcFragmentOutputResolverCallback callback,
    void* userData
);

/*
 * Set a bindless array size resolver callback.
 * This callback resolves unsized automatic bindless resource arrays to a slot count.
 */
SLANGC_API void slangc_setBindlessArraySizeResolver(
    SlangcCompiler compiler,
    SlangcBindlessArraySizeResolverCallback callback,
    void* userData
);

/* Link the program. Returns 1 on success, 0 on error. */
SLANGC_API int slangc_link(SlangcProgram program);

/*
 * Results (after linking)
 */

/* Get compiled code (SPIR-V, etc.). Returns blob handle or NULL. */
SLANGC_API SlangcBlob slangc_getCode(SlangcProgram program);

/* Get error/warning messages from module loading. */
SLANGC_API const char* slangc_getCompilerErrors(SlangcCompiler compiler);

/* Get error/warning messages from linking. */
SLANGC_API const char* slangc_getProgramErrors(SlangcProgram program);

/*
 * Blob access
 */

/* Get pointer to blob data */
SLANGC_API const void* slangc_getBlobData(SlangcBlob blob);

/* Get size of blob data in bytes */
SLANGC_API size_t slangc_getBlobSize(SlangcBlob blob);

/* Destroy a blob */
SLANGC_API void slangc_destroyBlob(SlangcBlob blob);

/*
 * Reflection - Resources
 *
 * Note: These functions use output pointers instead of returning structs
 * by value to ensure ABI compatibility between different compilers
 * (MinGW vs MSVC) and FFI systems like Java Panama.
 */

/* Get total number of resources in the program */
SLANGC_API int slangc_getResourceCount(SlangcProgram program);

/* Get resource info by index. Returns 1 on success, 0 on failure. */
SLANGC_API int slangc_getResource(SlangcProgram program, int index, SlangcResourceInfo* outInfo);

/* Get number of resources converted to bindless */
SLANGC_API int slangc_getBindlessResourceCount(SlangcProgram program);

/* Get bindless resource info by index. Returns 1 on success, 0 on failure. */
SLANGC_API int slangc_getBindlessResource(SlangcProgram program, int index, SlangcResourceInfo* outInfo);

/* Get number of resources converted to automatic bindless index-buffer access. */
SLANGC_API int slangc_getUsedBindingCount(SlangcProgram program);

/* Get automatic bindless binding info by index. Returns 1 on success, 0 on failure. */
SLANGC_API int slangc_getUsedBinding(SlangcProgram program, int index, SlangcResourceInfo* outInfo);

/*
 * Reflection - Entry Points
 */

/* Get number of entry points in the program */
SLANGC_API int slangc_getEntryPointCount(SlangcProgram program);

/* Get entry point name by index */
SLANGC_API const char* slangc_getEntryPointName(SlangcProgram program, int index);

/* Get entry point stage by index */
SLANGC_API SlangcStage slangc_getEntryPointStage(SlangcProgram program, int index);

/*
 * Generic Specialization
 *
 * These functions allow compiling shaders with generic entry points
 * by providing concrete type arguments.
 */

/* Get the number of specialization parameters required by the program.
 * Call this after adding modules and entry points but before link.
 * Returns 0 if the program has no generic parameters.
 */
SLANGC_API int slangc_getSpecializationParamCount(SlangcProgram program);

/* Find a type by name in a module. Returns NULL if not found.
 * The returned type handle is valid until the module is destroyed.
 */
SLANGC_API SlangcType slangc_findTypeByName(SlangcModule module, const char* typeName);

/* Add a specialization argument using a type expression string (positional).
 * Call once for each specialization parameter before calling slangc_link.
 * Example: slangc_addSpecializationArgExpr(program, "Grayscale");
 */
SLANGC_API void slangc_addSpecializationArgExpr(SlangcProgram program, const char* typeExpr);

/* Add a specialization argument using a type handle from slangc_findTypeByName (positional).
 * Call once for each specialization parameter before calling slangc_link.
 */
SLANGC_API void slangc_addSpecializationArgType(SlangcProgram program, SlangcType type);

/* Set a named specialization argument using a type expression string.
 * Named args are matched by parameter name across all entry points.
 * Parameters not specified will be inferred from constraints if possible.
 * Example: slangc_setSpecializationArg(program, "S", "MyShader");
 *
 * Named and positional args cannot be mixed - use one style or the other.
 */
SLANGC_API void slangc_setSpecializationArg(SlangcProgram program, const char* paramName, const char* typeExpr);

/* Clear all specialization arguments to reuse the program with different types.
 * This allows re-linking the same program with different specializations.
 */
SLANGC_API void slangc_clearSpecializationArgs(SlangcProgram program);

#ifdef __cplusplus
}
#endif

#endif /* SLANG_C99_H */
