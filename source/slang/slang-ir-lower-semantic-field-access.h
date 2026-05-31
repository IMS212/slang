// slang-ir-lower-semantic-field-access.h
#pragma once

namespace Slang
{
struct IRModule;
class DiagnosticSink;

bool lowerSemanticFieldAccessBuiltins(IRModule* module, DiagnosticSink* sink);
} // namespace Slang
