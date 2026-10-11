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
        // Failed vendor-device teardown can fault before the exception handler.
        // Preserve the first failure and let Windows release this test process.
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
        auto getToken = resolve<PFun_slGetNewFrameToken>(module, "slGetNewFrameToken");
        auto setConstants = resolve<PFun_slSetConstants>(module, "slSetConstants");
        auto setTags = resolve<PFun_slSetTagForFrame>(module, "slSetTagForFrame");
        auto evaluate = resolve<PFun_slEvaluateFeature>(module, "slEvaluateFeature");
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
        auto getState = reinterpret_cast<PFun_slFSRGGetState *>(stateFunction);
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
        auto depth = texture(device.Get(), renderWidth, renderHeight, DXGI_FORMAT_R32_FLOAT);
        auto motion = texture(device.Get(), renderWidth, renderHeight, DXGI_FORMAT_R16G16_FLOAT);
        auto hudless =
            texture(device.Get(), displayWidth, displayHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
        std::array<ID3D12Resource *, 3> resources{depth.Get(), motion.Get(), hudless.Get()};
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 3;
        check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap)), "CreateRTVHeap");
        const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3> handles{};
        for (UINT i = 0; i < resources.size(); ++i)
        {
            handles[i] = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            handles[i].ptr += i * step;
            device->CreateRenderTargetView(resources[i], nullptr, handles[i]);
        }
        const sl::float4x4 identity{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
        sl::Constants constants{};
        constants.cameraViewToClip = identity;
        constants.clipToCameraView = identity;
        constants.clipToLensClip = identity;
        constants.clipToPrevClip = identity;
        constants.prevClipToClip = identity;
        constants.jitterOffset = {0, 0};
        constants.mvecScale = {1.0f / renderWidth, 1.0f / renderHeight};
        constants.cameraPinholeOffset = {0, 0};
        constants.cameraPos = {0, 0, 0};
        constants.cameraUp = {0, 1, 0};
        constants.cameraRight = {1, 0, 0};
        constants.cameraFwd = {0, 0, 1};
        constants.cameraNear = 0.1f;
        constants.cameraFar = 1000;
        constants.cameraFOV = 1;
        constants.cameraAspectRatio = float(displayWidth) / displayHeight;
        constants.depthInverted = sl::Boolean::eFalse;
        constants.cameraMotionIncluded = sl::Boolean::eTrue;
        constants.motionVectors3D = sl::Boolean::eFalse;
        const sl::Extent renderExtent{0, 0, renderWidth, renderHeight},
            outputExtent{0, 0, displayWidth, displayHeight};
        sl::Resource depthTag(sl::ResourceType::eTex2d, depth.Get(), nullptr, nullptr,
                              D3D12_RESOURCE_STATE_COMMON);
        sl::Resource motionTag(sl::ResourceType::eTex2d, motion.Get(), nullptr, nullptr,
                               D3D12_RESOURCE_STATE_COMMON);
        sl::Resource hudlessTag(sl::ResourceType::eTex2d, hudless.Get(), nullptr, nullptr,
                                D3D12_RESOURCE_STATE_COMMON);
        const sl::ResourceTag tags[]{{&depthTag, sl::kBufferTypeDepth,
                                      sl::ResourceLifecycle::eValidUntilPresent, &renderExtent},
                                     {&motionTag, sl::kBufferTypeMotionVectors,
                                      sl::ResourceLifecycle::eValidUntilPresent, &renderExtent},
                                     {&hudlessTag, sl::kBufferTypeHUDLessColor,
                                      sl::ResourceLifecycle::eValidUntilPresent, &outputExtent}};
        UINT64 submitted = 0;
        options.mode = sl::FSRGMode::eOn;
        check(setOptions(viewport, options), "SetOptionsOn");
        for (UINT iteration = 0; iteration < iterations; ++iteration)
        {
            waitFor(rawDevice.Get(), fence.Get(), submitted);
            check(allocator->Reset(), "ResetAllocator");
            check(list->Reset(allocator.Get(), nullptr), "ResetList");
            const float clear[3][4]{
                {0.5f, 0, 0, 0}, {0, 0, 0, 0}, {0.15f + (iteration % 20) * 0.01f, 0.25f, 0.35f, 1}};
            for (UINT i = 0; i < resources.size(); ++i)
            {
                transition(list.Get(), resources[i], D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_RENDER_TARGET);
                list->ClearRenderTargetView(handles[i], clear[i], 0, nullptr);
                transition(list.Get(), resources[i], D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_COMMON);
            }
            ComPtr<ID3D12Resource> backbuffer;
            check(swap->GetBuffer(swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)),
                  "GetBackbuffer");
            transition(list.Get(), hudless.Get(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(list.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(backbuffer.Get(), hudless.Get());
            transition(list.Get(), hudless.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_COMMON);
            transition(list.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
            const UINT frame = firstFrame + iteration;
            sl::FrameToken *token{};
            check(getToken(token, &frame), "FrameToken");
            constants.reset = iteration == 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
            check(setConstants(constants, *token, viewport), "SetConstants");
            check(setTags(*token, viewport, tags, 3,
                          reinterpret_cast<sl::CommandBuffer *>(list.Get())),
                  "SetTags");
            const sl::BaseStructure *inputs[]{&viewport};
            check(evaluate(sl::kFeatureFSR_G, *token, inputs, 1,
                           reinterpret_cast<sl::CommandBuffer *>(list.Get())),
                  "EvaluateFG");
            check(list->Close(), "CloseList");
            ID3D12CommandList *lists[]{list.Get()};
            queue->ExecuteCommandLists(1, lists);
            check(queue->Signal(fence.Get(), ++submitted), "Signal");
            const HRESULT present = swap->Present(0, 0);
            std::cout << "frame=" << frame << " Present=0x" << std::hex << unsigned(present)
                      << std::dec << '\n';
            check(present, "Present");
            sl::FSRGState state{};
            check(getState(viewport, state), "FGState");
            ComPtr<ID3D12Fence> completion;
            completion.Attach(static_cast<ID3D12Fence *>(state.completionFence));
            std::cout << "active=" << unsigned(state.active)
                      << " mode=" << unsigned(state.completionMode)
                      << " completion=" << state.completionFenceValue << '\n';
            if (!completion || !state.completionFenceValue)
                throw std::runtime_error("No FG host completion dependency");
            waitFor(rawDevice.Get(), completion.Get(), state.completionFenceValue);
            check(rawDevice->GetDeviceRemovedReason(), "PostFG/device removed");
        }
        waitFor(rawDevice.Get(), fence.Get(), submitted);
        check(quiesce(), "Quiesce");
        std::cout << "PASS: " << iterations
                  << " generated-frame submissions completed without device removal\n";
        // Process exit releases the SDK: avoid teardown ordering obscuring the dispatch result.
        std::_Exit(0);
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
