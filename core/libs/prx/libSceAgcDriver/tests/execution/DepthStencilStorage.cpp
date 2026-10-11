#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <vector>

namespace {

using DepthFastClearHarness::Compile;
using DepthFastClearHarness::Covered;
using DepthFastClearHarness::Draw;
using DepthFastClearHarness::DrawOptions;
using DepthFastClearHarness::Height;
using DepthFastClearHarness::Require;
using DepthFastClearHarness::Surface;
using DepthFastClearHarness::Width;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t FormatR8Uint = 5;
constexpr std::uint32_t FormatR32Uint = 20;
constexpr std::uint32_t TileDepth64KB = 0x18;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t PlaneBytes = 65536;
constexpr std::size_t BlockBytes = 2 * PlaneBytes;
constexpr std::uint32_t StencilWritten = 0x07u;
constexpr float DepthWritten = 0.25f;

alignas(256) constexpr std::array<std::uint32_t, 5> StoreCode{
    0x7e020280, 0x7e04020c, 0xf0201108, 0x00010200, 0xbf810000,
};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, BlockBytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
        }
#endif
        Require(block != nullptr, "depth-stencil storage: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
#else
        munmap(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 8> PlaneDescriptor(std::uint64_t address, std::uint32_t format) {
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((Width - 1u) & 3u) << 30u),
        ((Width - 1u) >> 2u) | ((Height - 1u) << 14u),
        0xfacu | (TileDepth64KB << 20u) | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

void Store(AgcDriver::VulkanDevice& device, std::uint64_t plane, std::uint32_t format, std::uint32_t value) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto texture = PlaneDescriptor(plane, format);
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    userData[12] = value;
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(StoreCode.data()), std::as_bytes(std::span(StoreCode))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(StoreCode.data()), StoreCode, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(StoreCode.data()));
    device.WaitIdle();
}

VkStencilOpState StencilEqual(std::uint32_t reference) {
    return {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xffu, 0u, reference};
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock block;
        const auto shaders = Compile(*device);
        const auto depth = AddressOf(block.Data());
        const auto stencil = depth + PlaneBytes;
        const Surface surface{depth, stencil, 0, false, VK_FORMAT_D32_SFLOAT_S8_UINT};
        const VkStencilOpState replace{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xffu, 0xffu, 1u};
        Require(Draw(*device, shaders, surface, {.depthWrite = true, .stencilTest = true, .stencil = replace}) == Covered, "the first draw did not write depth 0.5 and stencil 1 over the surface");

        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Store(*device, depth, FormatR32Uint, std::bit_cast<std::uint32_t>(DepthWritten));
        Store(*device, stencil, FormatR8Uint, StencilWritten);

        Require(Draw(*device, shaders, surface, {.depthTest = false, .stencilTest = true, .stencil = StencilEqual(StencilWritten)}) == Threads, "the stencil written through the storage image did not reach the stencil plane");
        Require(Draw(*device, shaders, surface, {.depthTest = false, .stencilTest = true, .stencil = StencilEqual(1u)}) == Covered - Threads, "the storage image over the stencil plane changed texels it did not write");
        Require(Draw(*device, shaders, surface, {.depthCompare = VK_COMPARE_OP_GREATER}) == Threads, "the depth written through the storage image did not reach the depth plane beside a stencil writer");
        AgcDriver::Graphics::ClearCachedTextures(device->Device());
        AgcDriver::Graphics::ClearDepthSurfaces(device->Device());
        std::puts("depth-stencil storage tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
