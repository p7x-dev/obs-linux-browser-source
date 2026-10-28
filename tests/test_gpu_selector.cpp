#include <cstdio>
#include <string>
#include <vector>

#include "gpu-selector.h"

namespace {

int failures = 0;

void expect(const char *name, bool condition)
{
    if (!condition) {
        std::printf("FAIL: %s\n", name);
        failures++;
    } else {
        std::printf("ok:   %s\n", name);
    }
}

GpuInfo make_gpu(const char *name, const char *pci, const char *vendor,
                 const char *device, const char *driver, bool bootVga,
                 unsigned long long vram = 0)
{
    GpuInfo gpu;
    gpu.name = name;
    gpu.pciAddress = pci;
    gpu.vendorId = vendor;
    gpu.deviceId = device;
    gpu.driver = driver;
    gpu.bootVga = bootVga;
    gpu.vramBytes = vram;
    classify_gpu(gpu);
    return gpu;
}

bool has_env(const std::vector<GpuEnvVar> &env, const char *name,
             const char *value = nullptr)
{
    for (const GpuEnvVar &var : env) {
        if (var.name == name && (!value || var.value == value))
            return true;
    }
    return false;
}

} // namespace

int main()
{
    constexpr unsigned long long kGiB = 1024ull * 1024ull * 1024ull;

    const GpuInfo intel =
        make_gpu("card2", "0000:00:02.0", "0x8086", "0x9bc4", "i915", true);
    const GpuInfo nvidia =
        make_gpu("card1", "0000:01:00.0", "0x10de", "0x1f99", "nvidia", false);
    const GpuInfo amdDiscrete = make_gpu("card1", "0000:03:00.0", "0x1002",
                                         "0x73ff", "amdgpu", false, 8 * kGiB);
    const GpuInfo amdApu = make_gpu("card1", "0000:04:00.0", "0x1002", "0x1638",
                                    "amdgpu", false, 512ull * 1024 * 1024);
    const GpuInfo intelDiscrete = make_gpu("card1", "0000:05:00.0", "0x8086",
                                           "0x56a0", "xe", false, 6 * kGiB);
    const GpuInfo virtio = make_gpu("card0", "0000:00:01.0", "0x1af4", "0x1050",
                                    "virtio_gpu", true);
    const GpuInfo vkms =
        make_gpu("card3", "0000:00:03.0", "0x0000", "0x0000", "vkms", false);

    expect("intel is integrated", intel.gpuClass == GpuClass::Integrated);
    expect("nvidia is discrete", nvidia.gpuClass == GpuClass::Discrete);
    expect("amd vram is discrete", amdDiscrete.gpuClass == GpuClass::Discrete);
    expect("amd apu is integrated", amdApu.gpuClass == GpuClass::Integrated);
    expect("intel arc is discrete",
           intelDiscrete.gpuClass == GpuClass::Discrete);
    expect("virtio is virtual", virtio.gpuClass == GpuClass::Virtual);
    expect("vkms is virtual", vkms.gpuClass == GpuClass::Virtual);

    expect("nvidia scores highest", nvidia.score > amdDiscrete.score &&
                                         amdDiscrete.score > intel.score);
    expect("arc beats integrated intel", intelDiscrete.score > intel.score);

    expect("picks nvidia over intel",
           pick_best_gpu({intel, nvidia}).pciAddress == nvidia.pciAddress);
    expect("picks amd over intel",
           pick_best_gpu({intel, amdDiscrete}).pciAddress ==
               amdDiscrete.pciAddress);
    expect("picks amd apu over intel",
           pick_best_gpu({intel, amdApu}).pciAddress == amdApu.pciAddress);
    expect("ignores virtual", pick_best_gpu({virtio, vkms}).score == 0);
    expect("empty list has no GPU", pick_best_gpu({}).score == 0);

    expect("prime device format",
           prime_device("0000:01:00.0") == "pci-0000_01_00_0");
    expect("prime device empty", prime_device("") == "pci-");

    const auto nvidiaEnv = gpu_environment(nvidia, {intel, nvidia});
    expect("nvidia offload flag",
           has_env(nvidiaEnv, "__NV_PRIME_RENDER_OFFLOAD", "1"));
    expect("nvidia glx vendor",
           has_env(nvidiaEnv, "__GLX_VENDOR_LIBRARY_NAME", "nvidia"));
    expect("nvidia vulkan layer",
           has_env(nvidiaEnv, "__VK_LAYER_NV_optimus", "NVIDIA_only"));
    expect("nvidia does not set DRI_PRIME",
           !has_env(nvidiaEnv, "DRI_PRIME"));

    const auto amdEnv = gpu_environment(amdDiscrete, {intel, amdDiscrete});
    expect("amd uses DRI_PRIME",
           has_env(amdEnv, "DRI_PRIME", "pci-0000_03_00_0"));
    expect("amd does not set nvidia vars",
           !has_env(amdEnv, "__NV_PRIME_RENDER_OFFLOAD"));

    expect("default intel needs no offload",
           gpu_environment(intel, {intel}).empty());
    const GpuInfo bootAmd = make_gpu("card1", "0000:03:00.0", "0x1002", "0x73ff",
                                     "amdgpu", true, 8 * kGiB);
    expect("boot_vga GPU needs no offload",
           gpu_environment(bootAmd, {intel, bootAmd}).empty());
    expect("nouveau falls back to DRI_PRIME",
           has_env(gpu_environment(make_gpu("card1", "0000:01:00.0", "0x10de",
                                            "0x1f99", "nouveau", false),
                                   {intel, nvidia}),
                   "DRI_PRIME", "pci-0000_01_00_0"));

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all GPU selector tests passed\n");
    return 0;
}
