#pragma once

#include <xefg_swapchain_d3d12.h>
#include <xell_d3d12.h>

class XeMFGHooks
{
    inline static bool _hookedFG = false;
    inline static bool _hookedLL = false;
    inline static int _maxInterpolationCount = 1;
    inline static int _currentInterpolationCount = 1;
    inline static xefg_swapchain_handle_t _xefgContext = nullptr;

    // Native XeFG Support
    static xefg_swapchain_result_t hkxefgSwapChainD3D12CreateContext(ID3D12Device* pDevice,
                                                                     xefg_swapchain_handle_t* phSwapChain);
    static xefg_swapchain_result_t hkxefgSwapChainGetProperties(xefg_swapchain_handle_t hSwapChain,
                                                                xefg_swapchain_properties_t* pProperties);
    static xefg_swapchain_result_t hkxefgSwapChainD3D12GetProperties(
        xefg_swapchain_handle_t context, const xefg_swapchain_d3d12_init_params_t* initParams, uint32_t backBufferWidth,
        uint32_t backBufferHeight, DXGI_FORMAT backBufferFormat, xefg_swapchain_properties_t* properties);
    static xefg_swapchain_result_t hkxefgSwapChainSetNumInterpolatedFrames(xefg_swapchain_handle_t hSwapChain,
                                                                           uint32_t numInterpolatedFrames);
    static xefg_swapchain_result_t hkxefgSwapChainSetEnabled(xefg_swapchain_handle_t hSwapChain, uint32_t enable);

    static xell_result_t hkxellSetGeneratedFramesCount(xell_context_handle_t context, uint32_t frameId,
                                                       uint32_t framesCount);

  public:
    inline static int GetCurrentInterpolationCount() { return _currentInterpolationCount; }

    static bool Hooks();
    static bool HooksXeFG();
    static bool HooksXeLL();
    static xefg_swapchain_handle_t GetContext() { return _xefgContext; };
    static uint32_t GetMaxInterpolationCount() { return _maxInterpolationCount; };
};