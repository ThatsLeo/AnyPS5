#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_HELPERLANES_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_HELPERLANES_HPP

#include "IntermediateRepresentation/IrProgram.hpp"

namespace ShaderRecompiler {

// Whether a pixel shader value reaches this use only on helper lanes: the use is the kept operand
// of a select on the initial EXEC, the shader has no s_wqm, and nothing derived from the select
// reaches a read of another lane.
[[nodiscard]] bool KeptOnlyOnHelperLanes(const IrProgram& program, const IrUse& use);

}

#endif
