#define COBJMACROS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FRAME_COUNT = 2 };

typedef struct Renderer {
    IDXGIFactory4* factory;
    ID3D12Device* device;
    ID3D12CommandQueue* command_queue;
    IDXGISwapChain3* swap_chain;
    ID3D12DescriptorHeap* rtv_heap;
    ID3D12Resource* render_targets[FRAME_COUNT];
    ID3D12CommandAllocator* command_allocator;
    ID3D12GraphicsCommandList* command_list;
    ID3D12Fence* fence;
    HANDLE fence_event;
    UINT rtv_descriptor_size;
    UINT frame_index;
    UINT width;
    UINT height;
    UINT64 fence_value;
} Renderer;

static int environment_dimension(const char* name, int fallback)
{
    const char* text = getenv(name);
    if (text == NULL || *text == '\0') {
        return fallback;
    }
    char* end = NULL;
    const long value = strtol(text, &end, 10);
    if (*end != '\0' || value < 64 || value > 4096) {
        return fallback;
    }
    return (int)value;
}

static const char* window_title(void)
{
    const char* title = getenv("MANGO_OVERLAY_WINDOWS_TEST_TITLE");
    if (title == NULL || *title == '\0') {
        return "Mango Overlay Proton D3D12 Test";
    }
    return title;
}

static LRESULT CALLBACK window_procedure(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam)
{
    (void)wparam;
    (void)lparam;
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static HRESULT report_failure(const char* action, HRESULT result)
{
    if (FAILED(result)) {
        fprintf(
            stderr,
            "%s failed with HRESULT 0x%08lx.\n",
            action,
            (unsigned long)result);
    }
    return result;
}

static void release_render_targets(Renderer* renderer)
{
    for (UINT index = 0; index < FRAME_COUNT; ++index) {
        if (renderer->render_targets[index] != NULL) {
            ID3D12Resource_Release(renderer->render_targets[index]);
            renderer->render_targets[index] = NULL;
        }
    }
}

static HRESULT wait_for_gpu(Renderer* renderer)
{
    ++renderer->fence_value;
    HRESULT result = ID3D12CommandQueue_Signal(
        renderer->command_queue,
        renderer->fence,
        renderer->fence_value);
    if (FAILED(result)) {
        return report_failure("ID3D12CommandQueue::Signal", result);
    }
    if (ID3D12Fence_GetCompletedValue(renderer->fence)
        < renderer->fence_value) {
        ResetEvent(renderer->fence_event);
        result = ID3D12Fence_SetEventOnCompletion(
            renderer->fence,
            renderer->fence_value,
            renderer->fence_event);
        if (FAILED(result)) {
            return report_failure(
                "ID3D12Fence::SetEventOnCompletion",
                result);
        }
        if (WaitForSingleObject(renderer->fence_event, INFINITE)
            != WAIT_OBJECT_0) {
            return report_failure(
                "WaitForSingleObject(fence)",
                HRESULT_FROM_WIN32(GetLastError()));
        }
    }
    return S_OK;
}

static void release_renderer(Renderer* renderer)
{
    if (renderer->command_queue != NULL
        && renderer->fence != NULL
        && renderer->fence_event != NULL) {
        (void)wait_for_gpu(renderer);
    }
    release_render_targets(renderer);
    if (renderer->fence_event != NULL) {
        CloseHandle(renderer->fence_event);
    }
    if (renderer->fence != NULL) {
        ID3D12Fence_Release(renderer->fence);
    }
    if (renderer->command_list != NULL) {
        ID3D12GraphicsCommandList_Release(renderer->command_list);
    }
    if (renderer->command_allocator != NULL) {
        ID3D12CommandAllocator_Release(renderer->command_allocator);
    }
    if (renderer->rtv_heap != NULL) {
        ID3D12DescriptorHeap_Release(renderer->rtv_heap);
    }
    if (renderer->swap_chain != NULL) {
        IDXGISwapChain3_Release(renderer->swap_chain);
    }
    if (renderer->command_queue != NULL) {
        ID3D12CommandQueue_Release(renderer->command_queue);
    }
    if (renderer->device != NULL) {
        ID3D12Device_Release(renderer->device);
    }
    if (renderer->factory != NULL) {
        IDXGIFactory4_Release(renderer->factory);
    }
    memset(renderer, 0, sizeof(*renderer));
}

static void rtv_handle(
    const Renderer* renderer,
    UINT index,
    D3D12_CPU_DESCRIPTOR_HANDLE* handle)
{
    renderer->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
        renderer->rtv_heap,
        handle);
    handle->ptr += (SIZE_T)index * renderer->rtv_descriptor_size;
}

static HRESULT create_render_targets(Renderer* renderer)
{
    for (UINT index = 0; index < FRAME_COUNT; ++index) {
        HRESULT result = IDXGISwapChain3_GetBuffer(
            renderer->swap_chain,
            index,
            &IID_ID3D12Resource,
            (void**)&renderer->render_targets[index]);
        if (FAILED(result)) {
            return report_failure("IDXGISwapChain3::GetBuffer", result);
        }
        D3D12_CPU_DESCRIPTOR_HANDLE handle;
        rtv_handle(renderer, index, &handle);
        ID3D12Device_CreateRenderTargetView(
            renderer->device,
            renderer->render_targets[index],
            NULL,
            handle);
    }
    return S_OK;
}

static HRESULT create_renderer(
    HWND window,
    UINT width,
    UINT height,
    Renderer* renderer)
{
    HRESULT result = CreateDXGIFactory1(
        &IID_IDXGIFactory4,
        (void**)&renderer->factory);
    if (FAILED(result)) {
        return report_failure("CreateDXGIFactory1", result);
    }

    result = D3D12CreateDevice(
        NULL,
        D3D_FEATURE_LEVEL_11_0,
        &IID_ID3D12Device,
        (void**)&renderer->device);
    if (FAILED(result)) {
        return report_failure("D3D12CreateDevice", result);
    }

    const D3D12_COMMAND_QUEUE_DESC queue_description = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
    };
    result = ID3D12Device_CreateCommandQueue(
        renderer->device,
        &queue_description,
        &IID_ID3D12CommandQueue,
        (void**)&renderer->command_queue);
    if (FAILED(result)) {
        return report_failure("ID3D12Device::CreateCommandQueue", result);
    }

    const DXGI_SWAP_CHAIN_DESC1 swap_chain_description = {
        .Width = width,
        .Height = height,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .Stereo = FALSE,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = FRAME_COUNT,
        .Scaling = DXGI_SCALING_STRETCH,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
    };
    IDXGISwapChain1* initial_swap_chain = NULL;
    result = IDXGIFactory4_CreateSwapChainForHwnd(
        renderer->factory,
        (IUnknown*)renderer->command_queue,
        window,
        &swap_chain_description,
        NULL,
        NULL,
        &initial_swap_chain);
    if (FAILED(result)) {
        return report_failure("IDXGIFactory4::CreateSwapChainForHwnd", result);
    }
    result = IDXGISwapChain1_QueryInterface(
        initial_swap_chain,
        &IID_IDXGISwapChain3,
        (void**)&renderer->swap_chain);
    IDXGISwapChain1_Release(initial_swap_chain);
    if (FAILED(result)) {
        return report_failure("IDXGISwapChain1::QueryInterface", result);
    }
    (void)IDXGIFactory4_MakeWindowAssociation(
        renderer->factory,
        window,
        DXGI_MWA_NO_ALT_ENTER);
    renderer->frame_index =
        IDXGISwapChain3_GetCurrentBackBufferIndex(renderer->swap_chain);

    const D3D12_DESCRIPTOR_HEAP_DESC heap_description = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = FRAME_COUNT,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    };
    result = ID3D12Device_CreateDescriptorHeap(
        renderer->device,
        &heap_description,
        &IID_ID3D12DescriptorHeap,
        (void**)&renderer->rtv_heap);
    if (FAILED(result)) {
        return report_failure("ID3D12Device::CreateDescriptorHeap", result);
    }
    renderer->rtv_descriptor_size =
        ID3D12Device_GetDescriptorHandleIncrementSize(
            renderer->device,
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    result = create_render_targets(renderer);
    if (FAILED(result)) {
        return result;
    }

    result = ID3D12Device_CreateCommandAllocator(
        renderer->device,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator,
        (void**)&renderer->command_allocator);
    if (FAILED(result)) {
        return report_failure(
            "ID3D12Device::CreateCommandAllocator",
            result);
    }
    result = ID3D12Device_CreateCommandList(
        renderer->device,
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        renderer->command_allocator,
        NULL,
        &IID_ID3D12GraphicsCommandList,
        (void**)&renderer->command_list);
    if (FAILED(result)) {
        return report_failure("ID3D12Device::CreateCommandList", result);
    }
    result = ID3D12GraphicsCommandList_Close(renderer->command_list);
    if (FAILED(result)) {
        return report_failure("ID3D12GraphicsCommandList::Close", result);
    }

    result = ID3D12Device_CreateFence(
        renderer->device,
        0,
        D3D12_FENCE_FLAG_NONE,
        &IID_ID3D12Fence,
        (void**)&renderer->fence);
    if (FAILED(result)) {
        return report_failure("ID3D12Device::CreateFence", result);
    }
    renderer->fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (renderer->fence_event == NULL) {
        return report_failure(
            "CreateEvent(fence)",
            HRESULT_FROM_WIN32(GetLastError()));
    }
    renderer->width = width;
    renderer->height = height;
    return S_OK;
}

static HRESULT resize_renderer(Renderer* renderer, UINT width, UINT height)
{
    if (width == renderer->width && height == renderer->height) {
        return S_OK;
    }
    HRESULT result = wait_for_gpu(renderer);
    if (FAILED(result)) {
        return result;
    }
    release_render_targets(renderer);
    result = IDXGISwapChain3_ResizeBuffers(
        renderer->swap_chain,
        FRAME_COUNT,
        width,
        height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        0);
    if (FAILED(result)) {
        return report_failure("IDXGISwapChain3::ResizeBuffers", result);
    }
    renderer->frame_index =
        IDXGISwapChain3_GetCurrentBackBufferIndex(renderer->swap_chain);
    renderer->width = width;
    renderer->height = height;
    return create_render_targets(renderer);
}

static HRESULT render_frame(Renderer* renderer, unsigned int frame)
{
    HRESULT result = ID3D12CommandAllocator_Reset(
        renderer->command_allocator);
    if (FAILED(result)) {
        return report_failure("ID3D12CommandAllocator::Reset", result);
    }
    result = ID3D12GraphicsCommandList_Reset(
        renderer->command_list,
        renderer->command_allocator,
        NULL);
    if (FAILED(result)) {
        return report_failure("ID3D12GraphicsCommandList::Reset", result);
    }

    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource =
        renderer->render_targets[renderer->frame_index];
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(
        renderer->command_list,
        1,
        &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    rtv_handle(renderer, renderer->frame_index, &handle);
    const float background[4] = { 0.08F, 0.12F, 0.20F, 1.0F };
    ID3D12GraphicsCommandList_ClearRenderTargetView(
        renderer->command_list,
        handle,
        background,
        0,
        NULL);

    const UINT accent_width = renderer->width > 360U
        ? renderer->width / 3U
        : renderer->width / 2U;
    const UINT accent_height = renderer->height > 300U
        ? renderer->height / 3U
        : renderer->height / 2U;
    const UINT horizontal_room = renderer->width > accent_width
        ? renderer->width - accent_width
        : 0;
    const UINT left = horizontal_room == 0
        ? 0
        : (frame * 5U) % horizontal_room;
    const UINT top = (renderer->height - accent_height) / 2U;
    const D3D12_RECT accent_rectangle = {
        .left = (LONG)left,
        .top = (LONG)top,
        .right = (LONG)(left + accent_width),
        .bottom = (LONG)(top + accent_height),
    };
    const float phase = (float)(frame % 180U) / 179.0F;
    const float accent[4] = {
        0.25F + 0.45F * phase,
        0.70F,
        0.85F - 0.45F * phase,
        1.0F,
    };
    ID3D12GraphicsCommandList_ClearRenderTargetView(
        renderer->command_list,
        handle,
        accent,
        1,
        &accent_rectangle);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    ID3D12GraphicsCommandList_ResourceBarrier(
        renderer->command_list,
        1,
        &barrier);
    result = ID3D12GraphicsCommandList_Close(renderer->command_list);
    if (FAILED(result)) {
        return report_failure("ID3D12GraphicsCommandList::Close", result);
    }

    ID3D12CommandList* command_lists[] = {
        (ID3D12CommandList*)renderer->command_list,
    };
    ID3D12CommandQueue_ExecuteCommandLists(
        renderer->command_queue,
        1,
        command_lists);
    result = IDXGISwapChain3_Present(renderer->swap_chain, 1, 0);
    if (FAILED(result)) {
        return report_failure("IDXGISwapChain3::Present", result);
    }
    result = wait_for_gpu(renderer);
    if (FAILED(result)) {
        return result;
    }
    renderer->frame_index =
        IDXGISwapChain3_GetCurrentBackBufferIndex(renderer->swap_chain);
    return S_OK;
}

int WINAPI WinMain(
    HINSTANCE instance,
    HINSTANCE previous_instance,
    LPSTR command_line,
    int show_command)
{
    (void)previous_instance;
    (void)command_line;

    const char* class_name = "MangoOverlayProtonD3D12";
    WNDCLASSA window_class = { 0 };
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
    window_class.lpszClassName = class_name;
    if (RegisterClassA(&window_class) == 0) {
        return 1;
    }

    RECT rectangle = {
        .left = 0,
        .top = 0,
        .right = environment_dimension("MANGO_OVERLAY_DESKTOP_WIDTH", 960),
        .bottom = environment_dimension("MANGO_OVERLAY_DESKTOP_HEIGHT", 600),
    };
    AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExA(
        0,
        class_name,
        window_title(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        80,
        80,
        rectangle.right - rectangle.left,
        rectangle.bottom - rectangle.top,
        NULL,
        NULL,
        instance,
        NULL);
    if (window == NULL) {
        UnregisterClassA(class_name, instance);
        return 1;
    }

    RECT client_rectangle;
    GetClientRect(window, &client_rectangle);
    const UINT initial_width =
        (UINT)(client_rectangle.right - client_rectangle.left);
    const UINT initial_height =
        (UINT)(client_rectangle.bottom - client_rectangle.top);
    Renderer renderer;
    memset(&renderer, 0, sizeof(renderer));
    HRESULT result = create_renderer(
        window,
        initial_width,
        initial_height,
        &renderer);
    if (FAILED(result)) {
        release_renderer(&renderer);
        DestroyWindow(window);
        UnregisterClassA(class_name, instance);
        return 1;
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);
    int running = 1;
    unsigned int frame = 0;
    while (running) {
        MSG message;
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = 0;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if (!running) {
            break;
        }

        GetClientRect(window, &client_rectangle);
        const UINT width =
            (UINT)(client_rectangle.right - client_rectangle.left);
        const UINT height =
            (UINT)(client_rectangle.bottom - client_rectangle.top);
        if (width == 0 || height == 0) {
            Sleep(16);
            continue;
        }
        result = resize_renderer(&renderer, width, height);
        if (SUCCEEDED(result)) {
            result = render_frame(&renderer, frame++);
        }
        if (FAILED(result)) {
            running = 0;
        }
    }

    release_renderer(&renderer);
    if (IsWindow(window)) {
        DestroyWindow(window);
    }
    UnregisterClassA(class_name, instance);
    return FAILED(result) ? 1 : 0;
}
