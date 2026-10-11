#include "IntermediateRepresentation/IrBuilder.hpp"
#include "Optimization/HostInterpolationChecker.hpp"
#include <stdexcept>

using namespace ShaderRecompiler;
static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

enum class Shape { Helper, Wqm, ReadFirstLane, FrontFacing };

// I/J kept by a select on the initial EXEC and read by an instruction the masked select
// elimination does not look through.
static bool Lowered(Shape shape) {
    IrProgram program;
    program.Resources().stage = IrShaderStage::Pixel;
    program.Metadata().wholeQuadMode = shape == Shape::Wqm;
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    program.BlockOrder().push_back(&block);
    IrBuilder builder(program);
    builder.SetInsertionPoint(block);
    const auto builtin = [&](StageInputKind kind, IrType type) -> IrValue& {
        return builder.Emit(IrOpcode::GetBuiltin, type, {&builder.Constant(static_cast<std::uint32_t>(kind)), &builder.Constant(0u)});
    };
    IrValue& barycentric = builtin(StageInputKind::BaryCoordSmooth, IrType::U32);
    IrValue& active = shape == Shape::FrontFacing ? builtin(StageInputKind::FrontFacing, IrType::U1)
                                                  : builder.Emit(IrOpcode::IEqual32, IrType::Bool, {&builtin(StageInputKind::HelperInvocation, IrType::U32), &builder.Constant(0u)});
    IrValue& kept = builder.Emit(IrOpcode::SelectU32, IrType::U32, {&active, &builder.Constant(0x3f800000u), &barycentric});
    IrValue& read = shape == Shape::ReadFirstLane ? builder.Emit(IrOpcode::ReadFirstLane, IrType::U32, {&kept, &builder.ConstantBool(true)}) : kept;
    static_cast<void>(builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&read}));
    static_cast<void>(builder.Emit(IrOpcode::Return, IrType::Void, {}));
    const ShaderPixelInputInfo pixel {};
    const bool lowered = HostInterpolationChecker().Lower(program, pixel);
    Require(!lowered || (kept.Argument(2)->Resolve()->HasImmediate() && kept.Argument(2)->Resolve()->ImmediateU32() == 0u), "helper lanes did not read zero for the dropped barycentric");
    return lowered;
}

int main() {
    Require(Lowered(Shape::Helper), "a barycentric kept only on helper lanes needed emulation");
    Require(!Lowered(Shape::Wqm), "a barycentric was dropped from a whole-quad-mode shader");
    Require(!Lowered(Shape::ReadFirstLane), "a barycentric read by another lane was dropped");
    Require(!Lowered(Shape::FrontFacing), "a barycentric kept on live lanes was dropped");
}
