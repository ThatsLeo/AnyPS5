#include "Optimization/HelperLanes.hpp"
#include "IntermediateRepresentation/IrMetadata/StageIO.hpp"
#include "RdnaDecoder/RdnaInstruction.hpp"
#include <algorithm>
#include <vector>

namespace ShaderRecompiler {
namespace {

bool isHelperInvocation(const IrValue& value) {
    if (value.Opcode() != IrOpcode::GetBuiltin) return false;
    const IrValue* kind = value.Argument(0)->Resolve();
    return kind->HasImmediate() && kind->Type() == IrType::U32 && static_cast<StageInputKind>(kind->ImmediateU32()) == StageInputKind::HelperInvocation;
}

bool isZero(const IrValue& value) {
    return value.HasImmediate() && value.Type() == IrType::U32 && value.ImmediateU32() == 0u;
}

bool excludesHelperLanes(const IrValue& condition) {
    if (condition.Opcode() != IrOpcode::IEqual32) return false;
    const IrValue& lhs = *condition.Argument(0)->Resolve();
    const IrValue& rhs = *condition.Argument(1)->Resolve();
    return (isHelperInvocation(lhs) && isZero(rhs)) || (isHelperInvocation(rhs) && isZero(lhs));
}

bool readsOtherLanes(const IrProgram& program, const IrValue& value) {
    if (value.Opcode() == IrOpcode::ImageSampleRaw || value.Opcode() == IrOpcode::ImageGatherRaw) {
        const auto index = value.Flags<MemoryFlags>().index;
        return index >= program.Resources().memoryInfo.size() ||
               (program.Resources().memoryInfo[index].imageSampleFlags & (RdnaImageSampleFlagDerivative | RdnaImageSampleFlagLod | RdnaImageSampleFlagLevelZero)) == 0u;
    }
    switch (value.Opcode()) {
        case IrOpcode::ReadFirstLane:
        case IrOpcode::ReadLane:
        case IrOpcode::DppMoveU32:
        case IrOpcode::DppUpdateU32:
        case IrOpcode::Permlane16U32:
        case IrOpcode::BpermuteU32:
        case IrOpcode::PermuteU32:
        case IrOpcode::SwizzleU32:
        case IrOpcode::ImageSampleImplicitLod:
            return true;
        default:
            return false;
    }
}

bool reachesOtherLanes(const IrProgram& program, const IrValue& value) {
    std::vector<const IrValue*> pending {&value};
    std::vector<const IrValue*> visited {&value};
    while (!pending.empty()) {
        const IrValue* current = pending.back();
        pending.pop_back();
        for (const IrUse& use : current->OperandUses()) {
            if (std::ranges::find(visited, use.user) != visited.end()) continue;
            if (readsOtherLanes(program, *use.user)) return true;
            visited.push_back(use.user);
            pending.push_back(use.user);
        }
    }
    return false;
}

}

bool KeptOnlyOnHelperLanes(const IrProgram& program, const IrUse& use) {
    return program.Resources().stage == IrShaderStage::Pixel && !program.Metadata().wholeQuadMode && use.user->Opcode() == IrOpcode::SelectU32 && use.operand == 2u &&
           excludesHelperLanes(*use.user->Argument(0)->Resolve()) && !reachesOtherLanes(program, *use.user);
}

}
