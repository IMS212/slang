#pragma once
#include "slang-ir.h"

namespace Slang
{
    // Replace unused struct field initializers with default values.
    // This allows DCE to eliminate loads of unused varyings.
    //
    // After specialization and inlining, interface method calls become
    // direct FieldExtract operations. This pass finds MakeStruct instructions
    // where some operands (field initializers) are never accessed via
    // FieldExtract/FieldAddress, and replaces those operands with default
    // values. Subsequent DCE can then eliminate the now-dead loads.
    void eliminateUnusedStructFieldInits(IRModule* module);
}
