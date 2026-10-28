#include "gpu-selector.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace {

constexpr unsigned long long kOneGiB = 1024ull * 1024ull * 1024ull;

std::string trim(std::string value)
{
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
                value.end());
    return value;
}

std::string read_line(const fs::path &path)
{
    std::ifstream file(path);
    std::string line;
    if (file)
        std::getline(file, line);
    return trim(line);
}

bool is_card_node(const std::string &name)
{
    constexpr const char *prefix = "card";
    if (name.rfind(prefix, 0) != 0)
        return false;
    const std::string suffix = name.substr(4);
    return !suffix.empty() &&
           std::all_of(suffix.begin(), suffix.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

bool is_virtual_gpu(const GpuInfo &gpu)
{
    static const std::set<std::string> virtualDrivers = {
        "virtio_gpu", "virtio-pci", "qxl",    "vmwgfx",  "bochs",
        "bochs-drm",  "cirrus",     "vkms",   "vgem",    "simpledrm",
        "vboxvideo",  "hyperv_drm", "ast",    "mga",     "udl",
    };
    static const std::set<std::string> virtualVendors = {
        "0x1af4", // virtio
        "0x15ad", // VMware
        "0x1234", // QEMU
        "0x1b36", // Red Hat
        "0x80ee", // VirtualBox
        "0x1414", // Microsoft basic display
    };

    return virtualDrivers.count(gpu.driver) != 0 ||
           virtualVendors.count(gpu.vendorId) != 0;
}

std::string read_driver(const fs::path &device)
{
    std::error_code ec;
    const fs::path target = fs::read_symlink(device / "driver", ec);
    if (!ec && !target.empty())
        return target.filename().string();

    std::ifstream uevent(device / "uevent");
    std::string line;
    while (std::getline(uevent, line)) {
        constexpr const char *prefix = "DRIVER=";
        if (line.rfind(prefix, 0) == 0)
            return trim(line.substr(std::char_traits<char>::length(prefix)));
    }
    return {};
}

bool has_other_render_gpu(const GpuInfo &selected,
                          const std::vector<GpuInfo> &all)
{
    return std::any_of(all.begin(), all.end(), [&](const GpuInfo &gpu) {
        return gpu.gpuClass != GpuClass::Virtual &&
               gpu.pciAddress != selected.pciAddress;
    });
}

} // namespace

void classify_gpu(GpuInfo &gpu)
{
    if (is_virtual_gpu(gpu)) {
        gpu.gpuClass = GpuClass::Virtual;
        gpu.score = 0;
        return;
    }

    if (gpu.vendorId == "0x10de") {
        // NVIDIA only ships discrete GPUs on Linux.
        gpu.gpuClass = GpuClass::Discrete;
        gpu.score = 300;
        return;
    }

    if (gpu.vendorId == "0x1002") {
        const bool discrete = gpu.vramBytes >= kOneGiB;
        gpu.gpuClass = discrete ? GpuClass::Discrete : GpuClass::Integrated;
        gpu.score = discrete ? 260 : 120;
        return;
    }

    if (gpu.vendorId == "0x8086") {
        const bool discrete = gpu.vramBytes >= kOneGiB;
        gpu.gpuClass = discrete ? GpuClass::Discrete : GpuClass::Integrated;
        gpu.score = (discrete ? 240 : 100) + (gpu.driver == "xe" ? 20 : 0);
        return;
    }

    gpu.gpuClass = GpuClass::Integrated;
    gpu.score = 50;
}

std::vector<GpuInfo> detect_gpus(const std::string &drmRoot)
{
    std::vector<GpuInfo> gpus;
    std::set<std::string> seen;

    std::error_code ec;
    fs::directory_iterator it(drmRoot, fs::directory_options::skip_permission_denied,
                              ec);
    if (ec)
        return gpus;

    for (const fs::directory_entry &entry : it) {
        const std::string name = entry.path().filename().string();
        if (!is_card_node(name))
            continue;

        std::error_code canonicalEc;
        const fs::path device = fs::canonical(entry.path() / "device", canonicalEc);
        if (canonicalEc)
            continue;

        const std::string pciAddress = device.filename().string();
        if (pciAddress.find(':') == std::string::npos ||
            pciAddress.find('.') == std::string::npos)
            continue;
        if (!seen.insert(pciAddress).second)
            continue;

        GpuInfo gpu;
        gpu.name = name;
        gpu.pciAddress = pciAddress;
        gpu.vendorId = read_line(device / "vendor");
        gpu.deviceId = read_line(device / "device");
        gpu.driver = read_driver(device);
        gpu.bootVga = read_line(device / "boot_vga") == "1";

        const std::string vram = read_line(device / "mem_info_vram_total");
        if (!vram.empty()) {
            try {
                gpu.vramBytes = std::stoull(vram);
            } catch (...) {
                gpu.vramBytes = 0;
            }
        }

        classify_gpu(gpu);
        gpus.push_back(std::move(gpu));
    }

    std::sort(gpus.begin(), gpus.end(),
              [](const GpuInfo &a, const GpuInfo &b) {
                  return a.name < b.name;
              });
    return gpus;
}

GpuInfo pick_best_gpu(const std::vector<GpuInfo> &gpus)
{
    GpuInfo best;
    for (const GpuInfo &gpu : gpus) {
        if (gpu.gpuClass == GpuClass::Virtual || gpu.score <= 0)
            continue;
        if (gpu.score > best.score)
            best = gpu;
    }
    return best;
}

std::string prime_device(const std::string &pciAddress)
{
    std::string result = "pci-";
    for (char c : pciAddress)
        result += (c == ':' || c == '.') ? '_' : c;
    return result;
}

std::vector<GpuEnvVar> gpu_environment(const GpuInfo &selected,
                                       const std::vector<GpuInfo> &all)
{
    std::vector<GpuEnvVar> env;
    if (selected.gpuClass == GpuClass::Virtual || selected.score <= 0)
        return env;

    // A single GPU, or the GPU that already drives the display, is what
    // Chromium picks by default; no offload is necessary.
    if (selected.bootVga || !has_other_render_gpu(selected, all))
        return env;

    if (selected.vendorId == "0x10de" && selected.driver == "nvidia") {
        env.push_back({"__NV_PRIME_RENDER_OFFLOAD", "1"});
        env.push_back({"__GLX_VENDOR_LIBRARY_NAME", "nvidia"});
        env.push_back({"__VK_LAYER_NV_optimus", "NVIDIA_only"});
        return env;
    }

    if (!selected.pciAddress.empty())
        env.push_back({"DRI_PRIME", prime_device(selected.pciAddress)});
    return env;
}

bool gpu_environment_overridden()
{
    static const char *const vars[] = {
        "DRI_PRIME",
        "__NV_PRIME_RENDER_OFFLOAD",
        "__NV_PRIME_RENDER_OFFLOAD_PROVIDER",
        "__GLX_VENDOR_LIBRARY_NAME",
        "__VK_LAYER_NV_optimus",
        "MESA_VK_DEVICE_SELECT",
        "VK_ICD_FILENAMES",
        "VK_DRIVER_FILES",
        "__EGL_VENDOR_LIBRARY_FILENAMES",
    };

    for (const char *var : vars) {
        const char *value = std::getenv(var);
        if (value && *value)
            return true;
    }
    return false;
}
