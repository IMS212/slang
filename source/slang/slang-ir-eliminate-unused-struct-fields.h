#pragma once
#include "slang-ir.h"

namespace Slang
{
    // Eliminate unused varying struct fields and vertex outputs.
    // This allows DCE to eliminate loads of unused varyings.
    //
    // Phase 1-2 (Fragment shader optimization):
    // After specialization and inlining, interface method calls become
    // direct FieldExtract operations. This pass finds MakeStruct instructions
    // where some operands (field initializers) are never accessed via
    // FieldExtract/FieldAddress, and replaces those operands with default
    // values. Subsequent DCE can then eliminate the now-dead loads.
    //
    // Phase 3-4 (Vertex shader optimization):
    // When vertex and fragment shaders are compiled together in the same module,
    // this pass identifies vertex outputs that are not used by the fragment shader
    // and replaces the stored values with defaults. This optimization uses varying
    // location matching: it collects the locations of fragment inputs that are
    // actually used, then marks vertex outputs with non-matching locations for
    // elimination.
    //
    // Note: The vertex output optimization only runs when both shaders are in
    // the same IR module. For GLSL targets (which compile shaders separately),
    // only the fragment shader optimization applies.
    void eliminateUnusedStructFieldInits(IRModule* module);
}
