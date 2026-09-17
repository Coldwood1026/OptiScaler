#include "pch.h"

#include "XeMFG_Hooks.h"
#include <proxies/XeLL_Proxy.h>
#include <proxies/XeFG_Proxy.h>

xefg_swapchain_result_t XeMFGHooks::hkxefgSwapChainD3D12CreateContext(ID3D12Device* pDevice,
                                                                      xefg_swapchain_handle_t* phSwapChain)
{
    auto result = XeFGProxy::_xefgSwapChainD3D12CreateContext(pDevice, phSwapChain);

    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        if (_xefgContext)
        {
            LOG_WARN("Multiple XeFG context exist at the same time. Destroy old context");
            XeFGProxy::Destroy()(_xefgContext);
        }
        _xefgContext = *phSwapChain;
        xefg_swapchain_properties_t prop {};
        if (XeFGProxy::_xefgSwapChainGetProperties(_xefgContext, &prop) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
            _maxInterpolationCount = prop.maxSupportedInterpolations;
        else
            _maxInterpolationCount = 0;
    }
    return result;
}

xefg_swapchain_result_t XeMFGHooks::hkxefgSwapChainGetProperties(xefg_swapchain_handle_t hSwapChain,
                                                                 xefg_swapchain_properties_t* pProperties)
{
    pProperties->maxSupportedInterpolations = 3;
    return XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

xefg_swapchain_result_t XeMFGHooks::hkxefgSwapChainD3D12GetProperties(
    xefg_swapchain_handle_t context, const xefg_swapchain_d3d12_init_params_t* initParams, uint32_t backBufferWidth,
    uint32_t backBufferHeight, DXGI_FORMAT backBufferFormat, xefg_swapchain_properties_t* properties)
{
    auto result = XeFGProxy::_xefgSwapChainD3D12GetProperties(context, initParams, backBufferWidth, backBufferHeight,
                                                              backBufferFormat, properties);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        properties->maxSupportedInterpolations = 3;
    return result;
}

xefg_swapchain_result_t XeMFGHooks::hkxefgSwapChainSetNumInterpolatedFrames(xefg_swapchain_handle_t hSwapChain,
                                                                            uint32_t numInterpolatedFrames)
{
    if (_currentInterpolationCount != Config::Instance()->FGXeFGInterpolationCount.value_or_default())
    {
        _currentInterpolationCount = Config::Instance()->FGXeFGInterpolationCount.value_or_default();
        return XeFGProxy::_xefgSwapChainSetNumInterpolatedFrames(hSwapChain, _currentInterpolationCount);
    }
    return XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

xefg_swapchain_result_t XeMFGHooks::hkxefgSwapChainSetEnabled(xefg_swapchain_handle_t hSwapChain, uint32_t enable)
{
    if (enable > 0 && _currentInterpolationCount != Config::Instance()->FGXeFGInterpolationCount.value_or_default())
    {
        _currentInterpolationCount = Config::Instance()->FGXeFGInterpolationCount.value_or_default();
        XeFGProxy::_xefgSwapChainSetNumInterpolatedFrames(hSwapChain, _currentInterpolationCount);
    }
    return XeFGProxy::_xefgSwapChainSetEnabled(hSwapChain, enable);
}

xell_result_t XeMFGHooks::hkxellD3D12CreateContext(ID3D12Device* device, xell_context_handle_t* out_context)
{
    auto result = XeLLProxy::_xellD3D12CreateContext(device, out_context);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        if (_xellContext)
        {
            LOG_WARN("Multiple XeLL context exist at the same time. Destroy old context");
            XeLLProxy::DestroyContext()(_xellContext);
        }
        _xellContext = *out_context;
        _xellSleepParms.bLowLatencyMode = 1; // default enable LowLatency Mode
    }
    return result;
}

xell_result_t XeMFGHooks::hkxellSetSleepMode(xell_context_handle_t context, const xell_sleep_params_t* param)
{
    if (_lastFraneLimit == 0.f)
        _xellSleepParms.minimumIntervalUs = 0;
    else
        _xellSleepParms.minimumIntervalUs = 1e6 / _lastFraneLimit;
    return XeLLProxy::_xellSetSleepMode(context, &_xellSleepParms);
}

xell_result_t XeMFGHooks::hkxellSetGeneratedFramesCount(xell_context_handle_t context, uint32_t frameId,
                                                        uint32_t framesCount)
{
    uint32_t Count = 0;
    if (!_xefgContext)
        Count = framesCount;
    else if (framesCount != 0)
        Count = Config::Instance()->FGXeFGInterpolationCount.value_or_default();
    return XeLLProxy::_xellSetGeneratedFramesCount(context, frameId, Count);
    // return o_xellSetGeneratedFramesCount(context, frameId, framesCount);
}

bool XeMFGHooks::HooksXeFG()
{
    if (_hookedFG || !XeFGProxy::InitXeFG())
        return false;

    HRESULT result = S_OK;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (XeFGProxy::_xefgSwapChainD3D12CreateContext)
        result = DetourAttach(&(PVOID&) XeFGProxy::_xefgSwapChainD3D12CreateContext, hkxefgSwapChainD3D12CreateContext);
    if (XeFGProxy::_xefgSwapChainGetProperties)
        result = DetourAttach(&(PVOID&) XeFGProxy::_xefgSwapChainGetProperties, hkxefgSwapChainGetProperties);
    if (XeFGProxy::_xefgSwapChainD3D12GetProperties)
        result = DetourAttach(&(PVOID&) XeFGProxy::_xefgSwapChainD3D12GetProperties, hkxefgSwapChainD3D12GetProperties);
    if (XeFGProxy::_xefgSwapChainSetNumInterpolatedFrames)
        result = DetourAttach(&(PVOID&) XeFGProxy::_xefgSwapChainSetNumInterpolatedFrames,
                              hkxefgSwapChainSetNumInterpolatedFrames);
    if (XeFGProxy::_xefgSwapChainSetEnabled)
        result = DetourAttach(&(PVOID&) XeFGProxy::_xefgSwapChainSetEnabled, hkxefgSwapChainSetEnabled);

    DetourTransactionCommit();
    _hookedFG = true;
    return true;
}
bool XeMFGHooks::HooksXeLL()
{
    if (_hookedLL || !XeLLProxy::InitXeLL())
        return false;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (XeLLProxy::_xellD3D12CreateContext)
        DetourAttach(&(PVOID&) XeLLProxy::_xellD3D12CreateContext, hkxellD3D12CreateContext);
    if (XeLLProxy::_xellSetSleepMode)
        DetourAttach(&(PVOID&) XeLLProxy::_xellSetSleepMode, hkxellSetSleepMode);
    if (XeLLProxy::_xellSetGeneratedFramesCount)
        DetourAttach(&(PVOID&) XeLLProxy::_xellSetGeneratedFramesCount, hkxellSetGeneratedFramesCount);

    DetourTransactionCommit();
    _hookedLL = true;
    return true;
}

void XeMFGHooks::Update()
{
    if (!_xellContext || _lastFraneLimit == Config::Instance()->FramerateLimit.value_or(0.f))
        return;
    _lastFraneLimit = Config::Instance()->FramerateLimit.value_or(0.f);

    hkxellSetSleepMode(_xellContext, &_xellSleepParms);
}

xell_frame_report_t XeMFGHooks::GetLatencyReports(const float frequency)
{
    if (!_xellContext)
        return _xellLatencyData;

    static auto lastCallTime = std::chrono::steady_clock::time_point {};

    auto now = std::chrono::steady_clock::now();
    auto interval = std::chrono::milliseconds(static_cast<long long>(1000.0f / frequency));
    if (now - lastCallTime < interval)
    {
        return _xellLatencyData;
    }

    lastCallTime = now;

    XeLLProxy::GetFramesReports()(_xellContext, _xellReport);
    _xellLatencyData = _xellReport[63];

    return _xellLatencyData;
}

bool XeMFGHooks::Hooks() { return HooksXeFG() && HooksXeLL(); }