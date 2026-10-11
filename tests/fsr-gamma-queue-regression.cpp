#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "sl.h"
#include "sl_core_api.h"
#include "sl_fsr_g.h"
#include "sl_version.h"
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
constexpr UINT displayWidth = 2560, displayHeight = 1440;
constexpr UINT renderWidth = 1706, renderHeight = 960;

void check(HRESULT hr, const char *operation)
{
    if (FAILED(hr))
    {
        std::cerr << operation << " failed: 0x" << std::hex << unsigned(hr) << std::dec << '\n';
        // Cleaning up the failed vendor device can fault before the handler.
        // Keep the first error and let Windows release the test process.
        std::_Exit(1);
    }
}
void check(sl::Result result, const char *operation)
{
    if (result != sl::Result::eOk)
    {
        std::cerr << operation << " failed: SL result " << unsigned(result) << '\n';
        throw std::runtime_error(operation);
    }
}
template <class T> T *resolve(HMODULE module, const char *name)
{
    auto *function = reinterpret_cast<T *>(GetProcAddress(module, name));
    if (!function)
        throw std::runtime_error(name);
    return function;
}
void logMessage(sl::LogType type, const char *message)
{
    std::cout << "SL[" << unsigned(type) << "] " << message << std::endl;
}
void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}
ComPtr<ID3D12Resource> texture(ID3D12Device *device, UINT width, UINT height, DXGI_FORMAT format)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> result;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                          D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(&result)),
          "CreateTexture");
    return result;
}
void waitFor(ID3D12Device *device, ID3D12Fence *fence, UINT64 value)
{
    if (!value)
        return;
    auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX)
        check(device->GetDeviceRemovedReason(), "Fence/device removed");
    if (completed >= value)
        return;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        throw std::runtime_error("CreateEvent");
    const HRESULT hr = fence->SetEventOnCompletion(value, event);
    if (FAILED(hr))
    {
        CloseHandle(event);
        check(hr, "SetEventOnCompletion");
    }
    const DWORD waited = WaitForSingleObject(event, 10000);
    CloseHandle(event);
    check(device->GetDeviceRemovedReason(), "Wait/device removed");
    if (waited != WAIT_OBJECT_0 || fence->GetCompletedValue() == UINT64_MAX)
        throw std::runtime_error("GPU completion timeout/removal");
}

int wmain(int argc, wchar_t **argv)
{
    std::cout << std::unitbuf;
    if (argc < 2)
    {
        std::cerr << "Usage: fsr-gamma-queue-regression SDK-directory [fsr3|fsr4] [gamma|srgb] "
                     "[frame-ms] [first-frame] [iterations]\n";
        return 2;
    }
    const auto algorithm = argc > 2 && std::wstring(argv[2]) == L"fsr3" ? sl::FSRGAlgorithm::eFSR3
                                                                        : sl::FSRGAlgorithm::eFSR4;
    const auto color = argc > 3 && std::wstring(argv[3]) == L"srgb" ? sl::FSRColorSpace::eSRGB
                                                                    : sl::FSRColorSpace::eGamma22;
    const float frameMs = argc > 4 ? std::stof(argv[4]) : 16.666f;
    const UINT firstFrame = argc > 5 ? std::stoul(argv[5]) : 1;
    const UINT iterations = argc > 6 ? std::stoul(argv[6]) : 96;
    if (!iterations || iterations > 1000 || firstFrame > UINT32_MAX - iterations)
        return 2;
    std::cout << "Case: algorithm=" << unsigned(algorithm) << " color=" << unsigned(color)
              << " dt=" << frameMs << " first=" << firstFrame << '\n';
    try
    {
        const std::wstring sdk = std::filesystem::absolute(argv[1]).wstring();
        const auto module =
            LoadLibraryW((std::filesystem::path(sdk) / L"sl.interposer.dll").c_str());
        if (!module)
            throw std::runtime_error("Load interposer");
        auto init = resolve<PFun_slInit>(module, "slInit");
        auto upgrade = resolve<PFun_slUpgradeInterface>(module, "slUpgradeInterface");
        auto setDevice = resolve<PFun_slSetD3DDevice>(module, "slSetD3DDevice");
        auto getFunction = resolve<PFun_slGetFeatureFunction>(module, "slGetFeatureFunction");
        const wchar_t *paths[]{sdk.c_str()};
        sl::Feature features[]{sl::kFeatureFSR_G};
        sl::Preferences preferences{};
        preferences.pathsToPlugins = paths;
        preferences.numPathsToPlugins = 1;
        preferences.featuresToLoad = features;
        preferences.numFeaturesToLoad = 1;
        preferences.logLevel = sl::LogLevel::eVerbose;
        preferences.logMessageCallback = logMessage;
        preferences.showConsole = false;
        preferences.engine = sl::EngineType::eCustom;
        preferences.engineVersion = "1.0.0";
        preferences.projectId = "f8776929-c969-43bd-ac2b-294b4de58aac";
        preferences.renderAPI = sl::RenderAPI::eD3D12;
        preferences.flags = sl::PreferenceFlags::eUseManualHooking |
                            sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        check(init(preferences, sl::kSDKVersion), "slInit");

        ComPtr<ID3D12Device> rawDevice, device;
        check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&rawDevice)),
              "D3D12CreateDevice");
        device = rawDevice;
        void *upgraded = device.Detach();
        check(upgrade(&upgraded), "UpgradeDevice");
        device.Attach(static_cast<ID3D12Device *>(upgraded));
        check(setDevice(device.Get()), "SetDevice");
        void *optionsFunction{}, *stateFunction{}, *quiesceFunction{};
        check(getFunction(sl::kFeatureFSR_G, "slFSRGSetOptions", optionsFunction),
              "GetOptionsFunction");
        check(getFunction(sl::kFeatureFSR_G, "slFSRGGetState", stateFunction), "GetStateFunction");
        check(getFunction(sl::kFeatureFSR_G, "slFSRGQuiesce", quiesceFunction),
              "GetQuiesceFunction");
        auto setOptions = reinterpret_cast<PFun_slFSRGSetOptions *>(optionsFunction);
        auto quiesce = reinterpret_cast<PFun_slFSRGQuiesce *>(quiesceFunction);
        const sl::ViewportHandle viewport(0);
        sl::FSRGAlgorithmOptions algorithmOptions{};
        algorithmOptions.algorithm = algorithm;
        sl::FSRGOptions options{};
        options.next = &algorithmOptions;
        options.displayWidth = displayWidth;
        options.displayHeight = displayHeight;
        options.maxRenderWidth = displayWidth;
        options.maxRenderHeight = displayHeight;
        options.backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        options.colorSpace = color;
        options.frameTimeDeltaMilliseconds = frameMs;
        options.viewSpaceToMetersFactor = 0.01428222656f;
        check(setOptions(viewport, options), "SetOptionsOff");

        ComPtr<IDXGIFactory2> factory;
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateFactory");
        upgraded = factory.Detach();
        check(upgrade(&upgraded), "UpgradeFactory");
        factory.Attach(static_cast<IDXGIFactory2 *>(upgraded));
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateQueue");
        const HWND window = CreateWindowExW(0, L"STATIC", L"FSR gamma queue regression",
                                            WS_OVERLAPPEDWINDOW, 0, 0, displayWidth, displayHeight,
                                            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window)
            throw std::runtime_error("CreateWindow");
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = displayWidth;
        desc.Height = displayHeight;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> swap1;
        check(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &swap1),
              "CreateFGSwapchain");
        ComPtr<IDXGISwapChain3> swap;
        check(swap1.As(&swap), "GetSwapchain3");
        ComPtr<ID3D12CommandAllocator> allocator;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&allocator)),
              "CreateAllocator");
        ComPtr<ID3D12GraphicsCommandList> list;
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&list)),
              "CreateList");
        check(list->Close(), "InitialClose");
        ComPtr<ID3D12Fence> fence;
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        check(quiesce(), "Quiesce");
        std::cout << "READY: Streamline and the D3D12 swapchain initialized\n";
        std::_Exit(0);
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
