#pragma once

#include <string>
#include <vector>

// Linux DRM GPU discovery and PRIME selection.
//
// The plugin prefers the most capable GPU (a discrete NVIDIA/AMD card over an
// integrated one) and configures Chromium's subprocesses to render on it via
// the standard PRIME environment variables. All functions are pure or only
// touch the process environment, so the selection logic is unit-testable.

enum class GpuClass { Virtual, Integrated, Discrete };

struct GpuInfo
{
    std::string name;       // DRM node name, e.g. "card1"
    std::string pciAddress; // e.g. "0000:01:00.0"
    std::string vendorId;   // e.g. "0x10de"
    std::string deviceId;   // e.g. "0x1f99"
    std::string driver;     // e.g. "nvidia", "amdgpu", "i915"
    bool bootVga = false;
    unsigned long long vramBytes = 0;
    GpuClass gpuClass = GpuClass::Virtual;
    int score = 0;
};

struct GpuEnvVar
{
    std::string name;
    std::string value;
};

// Fills gpuClass and score from the vendor/device/driver data.
void classify_gpu(GpuInfo &gpu);

// Enumerates GPUs under /sys/class/drm (or a custom root for tests).
std::vector<GpuInfo> detect_gpus(const std::string &drmRoot = "/sys/class/drm");

// Returns the best non-virtual GPU, or a default-constructed GpuInfo when no
// usable GPU is present.
GpuInfo pick_best_gpu(const std::vector<GpuInfo> &gpus);

// "0000:01:00.0" -> "pci-0000_01_00_0" (the format Mesa's DRI_PRIME expects).
std::string prime_device(const std::string &pciAddress);

// Environment variables that make Chromium render on the selected GPU.
// Empty when the GPU is already the default one or no offload is possible.
std::vector<GpuEnvVar> gpu_environment(const GpuInfo &selected,
                                       const std::vector<GpuInfo> &all);

// True when the user already configured GPU selection explicitly.
bool gpu_environment_overridden();
