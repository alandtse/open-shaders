// Minimal standalone DLSS-G repro.
//
// Default mode: pure D3D12 -- scene rendered on D3D12, everything (device,
// queue, factory, swapchain) created through the SL proxies per
// ProgrammingGuideManualHooking.md. VERIFIED WORKING (numFramesActuallyPresented=2).
//
// --interop mode: mirrors the game's topology -- scene rendered on a native
// D3D11 device, copied into MISC_SHARED|NTHANDLE textures opened on D3D12,
// fence-synchronized, then tagged and presented from the D3D12 side only.
// Prints numFramesActuallyPresented once per second; >1 means interpolation.

#include <windows.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#define NV_WINDOWS
#pragma warning(push)
#pragma warning(disable: 4471)
#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_version.h>
#pragma warning(pop)

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr uint32_t kWidth = 1280;
	constexpr uint32_t kHeight = 720;
	constexpr uint32_t kBackBuffers = 3;
	constexpr float kScrollPxPerFrame = 4.0f;

	PFun_slInit* api_slInit{};
	PFun_slShutdown* api_slShutdown{};
	PFun_slIsFeatureSupported* api_slIsFeatureSupported{};
	PFun_slIsFeatureLoaded* api_slIsFeatureLoaded{};
	PFun_slSetTagForFrame* api_slSetTagForFrame{};
	PFun_slUpgradeInterface* api_slUpgradeInterface{};
	PFun_slSetConstants* api_slSetConstants{};
	PFun_slGetFeatureFunction* api_slGetFeatureFunction{};
	PFun_slGetNewFrameToken* api_slGetNewFrameToken{};
	PFun_slSetD3DDevice* api_slSetD3DDevice{};
	PFun_slDLSSGGetState* api_slDLSSGGetState{};
	PFun_slDLSSGSetOptions* api_slDLSSGSetOptions{};
	PFun_slReflexSetOptions* api_slReflexSetOptions{};
	PFun_slReflexSleep* api_slReflexSleep{};
	PFun_slPCLSetMarker* api_slPCLSetMarker{};

	void LogCallback(sl::LogType type, const char* msg)
	{
		const char* prefix = type == sl::LogType::eError ? "ERROR" : (type == sl::LogType::eWarn ? "WARN" : "info");
		printf("[SL %s] %s", prefix, msg);
	}

	void Fail(const char* what, long code = 0)
	{
		printf("FATAL: %s (0x%lx)\n", what, code);
		fflush(stdout);
		ExitProcess(1);
	}

	void Check(HRESULT hr, const char* what)
	{
		if (FAILED(hr))
			Fail(what, hr);
	}

	void CheckSL(sl::Result r, const char* what)
	{
		if (r != sl::Result::eOk)
			Fail(what, (long)r);
	}

	LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if (msg == WM_DESTROY) {
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	// D3D12 path: MRT2 + SV_Depth into a real depth buffer.
	const char kShader12[] = R"(
cbuffer Frame : register(b0) { float scroll; float pad0; float pad1; float pad2; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
}
struct PSOut {
	float4 color : SV_Target0;
	float2 mvec : SV_Target1;
	float depth : SV_Depth;
};
PSOut PSMain(VSOut i) {
	PSOut o;
	float2 px = i.uv * float2(1280, 720);
	float checker = fmod(floor((px.x + scroll) / 64) + floor(px.y / 64), 2);
	float3 a = float3(0.9, 0.55, 0.15), b = float3(0.1, 0.3, 0.7);
	o.color = float4(lerp(a, b, checker), 1);
	o.mvec = float2(-4.0, 0.0);
	o.depth = 0.3 + 0.4 * i.uv.y;
	return o;
}
)";

	// Interop path: depth written as a color MRT (R32_FLOAT) so all four
	// tagged surfaces are shareable render targets, like the game's flat depth.
	const char kShader11[] = R"(
cbuffer Frame : register(b0) { float scroll; float pad0; float pad1; float pad2; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
}
struct PSOut {
	float4 color : SV_Target0;
	float2 mvec : SV_Target1;
	float depth : SV_Target2;
};
PSOut PSMain(VSOut i) {
	PSOut o;
	float2 px = i.uv * float2(1280, 720);
	float checker = fmod(floor((px.x + scroll) / 64) + floor(px.y / 64), 2);
	float3 a = float3(0.9, 0.55, 0.15), b = float3(0.1, 0.3, 0.7);
	o.color = float4(lerp(a, b, checker), 1);
	o.mvec = float2(-4.0, 0.0);
	o.depth = 0.3 + 0.4 * i.uv.y;
	return o;
}
)";

	sl::float4x4 Identity()
	{
		sl::float4x4 m{};
		m.row[0] = sl::float4(1, 0, 0, 0);
		m.row[1] = sl::float4(0, 1, 0, 0);
		m.row[2] = sl::float4(0, 0, 1, 0);
		m.row[3] = sl::float4(0, 0, 0, 1);
		return m;
	}

	sl::float4x4 Perspective(float fovY, float aspect, float zn, float zf)
	{
		const float ys = 1.0f / tanf(fovY * 0.5f);
		const float xs = ys / aspect;
		sl::float4x4 m{};
		m.row[0] = sl::float4(xs, 0, 0, 0);
		m.row[1] = sl::float4(0, ys, 0, 0);
		m.row[2] = sl::float4(0, 0, zf / (zf - zn), 1);
		m.row[3] = sl::float4(0, 0, -zn * zf / (zf - zn), 0);
		return m;
	}
}

int main(int argc, char** argv)
{
	const bool interop = argc > 1 && std::string(argv[1]) == "--interop";
	const bool pingpong = argc > 1 && std::string(argv[1]) == "--pingpong";
	const bool vsync = argc > 1 && std::string(argv[1]) == "--vsync";
	const bool reflexOff = argc > 1 && std::string(argv[1]) == "--reflexoff";
	const bool tokenLag = argc > 1 && std::string(argv[1]) == "--tokenlag";
	const bool optsPerFrame = argc > 1 && std::string(argv[1]) == "--optsperframe";
	const bool skewed = argc > 1 && std::string(argv[1]) == "--skewed";
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	printf("DLSS-G minimal repro, SL SDK %u.%u.%u, mode=%s\n",
		SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH,
		argc > 1 ? argv[1] : "D3D12-only");
	(void)interop;

	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();

	HMODULE interposer = LoadLibraryW((exeDir / L"sl.interposer.dll").c_str());
	if (!interposer)
		Fail("LoadLibrary sl.interposer.dll", GetLastError());

	auto get = [&](const char* name) {
		void* p = (void*)GetProcAddress(interposer, name);
		if (!p)
			Fail(name);
		return p;
	};
	api_slInit = (PFun_slInit*)get("slInit");
	api_slShutdown = (PFun_slShutdown*)get("slShutdown");
	api_slIsFeatureSupported = (PFun_slIsFeatureSupported*)get("slIsFeatureSupported");
	api_slIsFeatureLoaded = (PFun_slIsFeatureLoaded*)get("slIsFeatureLoaded");
	api_slSetTagForFrame = (PFun_slSetTagForFrame*)get("slSetTagForFrame");
	api_slUpgradeInterface = (PFun_slUpgradeInterface*)get("slUpgradeInterface");
	api_slSetConstants = (PFun_slSetConstants*)get("slSetConstants");
	api_slGetFeatureFunction = (PFun_slGetFeatureFunction*)get("slGetFeatureFunction");
	api_slGetNewFrameToken = (PFun_slGetNewFrameToken*)get("slGetNewFrameToken");
	api_slSetD3DDevice = (PFun_slSetD3DDevice*)get("slSetD3DDevice");

	const std::wstring pluginDir = exeDir.wstring();
	const wchar_t* paths[] = { pluginDir.c_str() };
	sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
	sl::Preferences pref{};
	pref.showConsole = true;
	pref.logLevel = sl::LogLevel::eVerbose;
	pref.pathsToPlugins = paths;
	pref.numPathsToPlugins = 1;
	pref.pathToLogsAndData = pluginDir.c_str();
	pref.logMessageCallback = LogCallback;
	pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging |
	             sl::PreferenceFlags::eDisableCLStateTracking;
	pref.featuresToLoad = features;
	pref.numFeaturesToLoad = _countof(features);
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0";
	pref.projectId = "a0f57b54-1daf-4934-90ae-c4035c19df05";
	pref.renderAPI = sl::RenderAPI::eD3D12;
	CheckSL(api_slInit(pref, sl::kSDKVersion), "slInit");

	// Factory + device + queue + swapchain, all through SL proxies.
	ComPtr<IDXGIFactory2> factory;
	Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
	CheckSL(api_slUpgradeInterface((void**)factory.GetAddressOf()), "slUpgradeInterface(factory)");

	ComPtr<IDXGIAdapter1> adapter;
	Check(factory->EnumAdapters1(0, adapter.GetAddressOf()), "EnumAdapters1");
	DXGI_ADAPTER_DESC1 adapterDesc{};
	adapter->GetDesc1(&adapterDesc);
	printf("Adapter: %ls\n", adapterDesc.Description);

	ComPtr<ID3D12Device> device;
	Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
	CheckSL(api_slUpgradeInterface((void**)device.GetAddressOf()), "slUpgradeInterface(device)");
	CheckSL(api_slSetD3DDevice(device.Get()), "slSetD3DDevice");

	sl::AdapterInfo adapterInfo{};
	adapterInfo.deviceLUID = (uint8_t*)&adapterDesc.AdapterLuid;
	adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);
	sl::Result supported = api_slIsFeatureSupported(sl::kFeatureDLSS_G, adapterInfo);
	printf("slIsFeatureSupported(DLSS_G) = %d\n", (int)supported);
	bool loaded = false;
	api_slIsFeatureLoaded(sl::kFeatureDLSS_G, loaded);
	printf("slIsFeatureLoaded(DLSS_G) = %d\n", loaded ? 1 : 0);
	if (supported != sl::Result::eOk || !loaded)
		Fail("DLSS-G unsupported or not loaded");

	void* fn{};
	CheckSL(api_slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fn), "get slDLSSGGetState");
	api_slDLSSGGetState = (PFun_slDLSSGGetState*)fn;
	CheckSL(api_slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fn), "get slDLSSGSetOptions");
	api_slDLSSGSetOptions = (PFun_slDLSSGSetOptions*)fn;
	CheckSL(api_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", fn), "get slReflexSetOptions");
	api_slReflexSetOptions = (PFun_slReflexSetOptions*)fn;
	CheckSL(api_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", fn), "get slReflexSleep");
	api_slReflexSleep = (PFun_slReflexSleep*)fn;
	CheckSL(api_slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", fn), "get slPCLSetMarker");
	api_slPCLSetMarker = (PFun_slPCLSetMarker*)fn;

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	ComPtr<ID3D12CommandQueue> queue;
	Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

	WNDCLASSW wc{};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"DLSSGRepro";
	RegisterClassW(&wc);
	RECT rect{ 0, 0, (LONG)kWidth, (LONG)kHeight };
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowW(wc.lpszClassName, L"DLSS-G repro", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		100, 100, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
	if (!hwnd)
		Fail("CreateWindow");

	DXGI_SWAP_CHAIN_DESC1 scDesc{};
	scDesc.Width = kWidth;
	scDesc.Height = kHeight;
	scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	scDesc.SampleDesc.Count = 1;
	scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scDesc.BufferCount = kBackBuffers;
	scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	scDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
	ComPtr<IDXGISwapChain1> swapChain1;
	Check(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &scDesc, nullptr, nullptr, swapChain1.GetAddressOf()),
		"CreateSwapChainForHwnd");
	ComPtr<IDXGISwapChain3> swapChain;
	Check(swapChain1.As(&swapChain), "QI IDXGISwapChain3");

	// ---- D3D11 side (interop mode): native device + scene pipeline + shared surfaces ----
	ComPtr<ID3D11Device> d11Device;
	ComPtr<ID3D11DeviceContext> d11Context;
	ComPtr<ID3D11VertexShader> d11VS;
	ComPtr<ID3D11PixelShader> d11PS;
	ComPtr<ID3D11Buffer> d11CB;
	ComPtr<ID3D11Texture2D> d11Color, d11Mvec, d11Depth;                 // render targets
	ComPtr<ID3D11Texture2D> d11ColorSh, d11MvecSh, d11DepthSh, d11UISh;  // shared copies
	ComPtr<ID3D11RenderTargetView> d11RTVs[3];
	ComPtr<ID3D11RenderTargetView> d11UIRTV;
	ComPtr<ID3D12Resource> shColor12, shMvec12, shDepth12, shUI12;  // D3D12 views of shared
	ComPtr<ID3D11Fence> d11Fence;
	ComPtr<ID3D12Fence> crossFence;
	uint64_t crossFenceValue = 0;

	// ---- D3D12 scene resources (D3D12-only mode) ----
	ComPtr<ID3D12Resource> colorRT, mvecRT, depthTex, uiTex;
	ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
	ComPtr<ID3D12RootSignature> rootSig;
	ComPtr<ID3D12PipelineState> pipeline;
	UINT rtvSize = 0;

	ComPtr<ID3DBlob> errBlob;
	if (interop) {
		Check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
				  D3D11_SDK_VERSION, &d11Device, nullptr, &d11Context),
			"D3D11CreateDevice");

		ComPtr<ID3DBlob> vs, ps;
		if (FAILED(D3DCompile(kShader11, sizeof(kShader11) - 1, nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &errBlob)))
			Fail(errBlob ? (const char*)errBlob->GetBufferPointer() : "D3DCompile failed with no diagnostics");
		if (FAILED(D3DCompile(kShader11, sizeof(kShader11) - 1, nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &errBlob)))
			Fail(errBlob ? (const char*)errBlob->GetBufferPointer() : "D3DCompile failed with no diagnostics");
		Check(d11Device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &d11VS), "11 VS");
		Check(d11Device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &d11PS), "11 PS");

		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = 16;
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		Check(d11Device->CreateBuffer(&cbDesc, nullptr, &d11CB), "11 cbuffer");

		auto makeTex11 = [&](DXGI_FORMAT fmt, bool shared, ComPtr<ID3D11Texture2D>& out) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = kWidth;
			desc.Height = kHeight;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = fmt;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_RENDER_TARGET;
			if (shared)
				desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
			Check(d11Device->CreateTexture2D(&desc, nullptr, &out), "11 texture");
		};
		makeTex11(DXGI_FORMAT_R8G8B8A8_UNORM, false, d11Color);
		makeTex11(DXGI_FORMAT_R16G16_FLOAT, false, d11Mvec);
		makeTex11(DXGI_FORMAT_R32_FLOAT, false, d11Depth);
		makeTex11(DXGI_FORMAT_R8G8B8A8_UNORM, true, d11ColorSh);
		makeTex11(DXGI_FORMAT_R16G16_FLOAT, true, d11MvecSh);
		makeTex11(DXGI_FORMAT_R32_FLOAT, true, d11DepthSh);
		makeTex11(DXGI_FORMAT_R8G8B8A8_UNORM, true, d11UISh);
		Check(d11Device->CreateRenderTargetView(d11Color.Get(), nullptr, &d11RTVs[0]), "11 rtv color");
		Check(d11Device->CreateRenderTargetView(d11Mvec.Get(), nullptr, &d11RTVs[1]), "11 rtv mvec");
		Check(d11Device->CreateRenderTargetView(d11Depth.Get(), nullptr, &d11RTVs[2]), "11 rtv depth");
		Check(d11Device->CreateRenderTargetView(d11UISh.Get(), nullptr, &d11UIRTV), "11 rtv ui");
		const float clearUI[4] = { 0, 0, 0, 0 };
		d11Context->ClearRenderTargetView(d11UIRTV.Get(), clearUI);

		auto openOn12 = [&](ID3D11Texture2D* tex, ComPtr<ID3D12Resource>& out) {
			ComPtr<IDXGIResource1> dxgiRes;
			Check(tex->QueryInterface(IID_PPV_ARGS(&dxgiRes)), "QI IDXGIResource1");
			HANDLE h{};
			Check(dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h),
				"CreateSharedHandle");
			Check(device->OpenSharedHandle(h, IID_PPV_ARGS(&out)), "OpenSharedHandle");
			CloseHandle(h);
		};
		openOn12(d11ColorSh.Get(), shColor12);
		openOn12(d11MvecSh.Get(), shMvec12);
		openOn12(d11DepthSh.Get(), shDepth12);
		openOn12(d11UISh.Get(), shUI12);

		// Cross-API fence: D3D11 signals after the copy, D3D12 queue waits.
		Check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&crossFence)), "cross fence");
		HANDLE fh{};
		Check(device->CreateSharedHandle(crossFence.Get(), nullptr, GENERIC_ALL, nullptr, &fh), "fence shared handle");
		ComPtr<ID3D11Device5> d11Device5;
		Check(d11Device.As(&d11Device5), "QI ID3D11Device5");
		Check(d11Device5->OpenSharedFence(fh, IID_PPV_ARGS(&d11Fence)), "OpenSharedFence");
		CloseHandle(fh);
	} else {
		auto makeTex = [&](DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS resFlags, const D3D12_CLEAR_VALUE* clear,
						   ComPtr<ID3D12Resource>& out) {
			D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = kWidth;
			desc.Height = kHeight;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = fmt;
			desc.SampleDesc.Count = 1;
			desc.Flags = resFlags;
			Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, clear,
					  IID_PPV_ARGS(&out)),
				"CreateCommittedResource");
		};
		D3D12_CLEAR_VALUE colorClear{ DXGI_FORMAT_R8G8B8A8_UNORM, { 0, 0, 0, 1 } };
		D3D12_CLEAR_VALUE mvecClear{ DXGI_FORMAT_R16G16_FLOAT, { 0, 0, 0, 0 } };
		D3D12_CLEAR_VALUE depthClear{ DXGI_FORMAT_D32_FLOAT };
		depthClear.DepthStencil.Depth = 1.0f;
		D3D12_CLEAR_VALUE uiClear{ DXGI_FORMAT_R8G8B8A8_UNORM, { 0, 0, 0, 0 } };
		makeTex(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &colorClear, colorRT);
		makeTex(DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &mvecClear, mvecRT);
		makeTex(DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, &depthClear, depthTex);
		makeTex(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &uiClear, uiTex);

		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8 };
		Check(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)), "rtv heap");
		D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
		Check(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap)), "dsv heap");
		rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		auto rtv = [&](UINT i) {
			D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += (SIZE_T)i * rtvSize;
			return h;
		};
		device->CreateRenderTargetView(colorRT.Get(), nullptr, rtv(0));
		device->CreateRenderTargetView(mvecRT.Get(), nullptr, rtv(1));
		device->CreateRenderTargetView(uiTex.Get(), nullptr, rtv(2));
		device->CreateDepthStencilView(depthTex.Get(), nullptr, dsvHeap->GetCPUDescriptorHandleForHeapStart());

		D3D12_ROOT_PARAMETER rootParam{};
		rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		rootParam.Constants.Num32BitValues = 4;
		rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.NumParameters = 1;
		rootDesc.pParameters = &rootParam;
		ComPtr<ID3DBlob> rootBlob;
		Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errBlob), "serialize root sig");
		Check(device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig)),
			"CreateRootSignature");

		ComPtr<ID3DBlob> vs, ps;
		if (FAILED(D3DCompile(kShader12, sizeof(kShader12) - 1, nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &errBlob)))
			Fail(errBlob ? (const char*)errBlob->GetBufferPointer() : "D3DCompile failed with no diagnostics");
		if (FAILED(D3DCompile(kShader12, sizeof(kShader12) - 1, nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &errBlob)))
			Fail(errBlob ? (const char*)errBlob->GetBufferPointer() : "D3DCompile failed with no diagnostics");

		D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = rootSig.Get();
		pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
		pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pso.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pso.SampleMask = UINT_MAX;
		pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pso.DepthStencilState.DepthEnable = TRUE;
		pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pso.NumRenderTargets = 2;
		pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		pso.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
		pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		pso.SampleDesc.Count = 1;
		Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)), "CreateGraphicsPipelineState");
	}

	ComPtr<ID3D12CommandAllocator> allocators[kBackBuffers];
	ComPtr<ID3D12GraphicsCommandList> cmdList;
	for (auto& a : allocators)
		Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "allocator");
	Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&cmdList)),
		"CreateCommandList");
	cmdList->Close();

	ComPtr<ID3D12Fence> fence;
	Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
	HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	uint64_t fenceValue = 0;
	uint64_t frameFence[kBackBuffers] = {};

	// Enable Reflex + DLSS-G.
	sl::ReflexOptions reflexOpts{};
	reflexOpts.mode = reflexOff ? sl::ReflexMode::eOff : sl::ReflexMode::eLowLatency;
	CheckSL(api_slReflexSetOptions(reflexOpts), "slReflexSetOptions");

	sl::ViewportHandle viewport{ 0 };
	sl::DLSSGState state{};
	sl::DLSSGOptions opts{};
	opts.mode = sl::DLSSGMode::eOn;
	opts.numFramesToGenerate = 1;
	CheckSL(api_slDLSSGGetState(viewport, state, &opts), "slDLSSGGetState(initial)");
	printf("DLSS-G numFramesToGenerateMax=%u minWidthOrHeight=%u\n", state.numFramesToGenerateMax, state.minWidthOrHeight);
	CheckSL(api_slDLSSGSetOptions(viewport, opts), "slDLSSGSetOptions");
	printf("DLSS-G enabled (2x)\n");

	sl::Constants consts{};
	consts.cameraViewToClip = Perspective(1.2f, (float)kWidth / kHeight, 0.1f, 100.f);
	consts.clipToCameraView = Identity();
	consts.clipToPrevClip = Identity();
	consts.prevClipToClip = Identity();
	consts.jitterOffset = { 0, 0 };
	consts.mvecScale = { 1.f / kWidth, 1.f / kHeight };
	consts.cameraPinholeOffset = { 0, 0 };
	consts.cameraPos = { 0, 0, 0 };
	consts.cameraUp = { 0, 1, 0 };
	consts.cameraRight = { 1, 0, 0 };
	consts.cameraFwd = { 0, 0, 1 };
	consts.cameraNear = 0.1f;
	consts.cameraFar = 100.f;
	consts.cameraFOV = 1.2f;
	consts.cameraAspectRatio = (float)kWidth / kHeight;
	consts.depthInverted = sl::Boolean::eFalse;
	consts.cameraMotionIncluded = sl::Boolean::eTrue;
	consts.motionVectors3D = sl::Boolean::eFalse;
	consts.reset = sl::Boolean::eFalse;

	// Two persistent present workers to mimic the game's alternating present
	// threads (main render thread vs loading/menu thread).
	struct PresentWorker
	{
		std::thread thread;
		std::mutex mutex;
		std::condition_variable cv;
		std::function<void()> job;
		bool quit = false;
		void Start()
		{
			thread = std::thread([this] {
				std::unique_lock lk(mutex);
				for (;;) {
					cv.wait(lk, [this] { return job || quit; });
					if (quit)
						return;
					job();
					job = nullptr;
					cv.notify_all();
				}
			});
		}
		void Run(std::function<void()> f)
		{
			{
				std::unique_lock lk(mutex);
				job = std::move(f);
			}
			cv.notify_all();
			std::unique_lock lk(mutex);
			cv.wait(lk, [this] { return !job; });
		}
		void Stop()
		{
			{
				std::unique_lock lk(mutex);
				quit = true;
			}
			cv.notify_all();
			if (thread.joinable())
				thread.join();
		}
	};
	PresentWorker workers[2];
	if (pingpong) {
		workers[0].Start();
		workers[1].Start();
	}

	uint32_t frameIndex = 0;
	auto lastReport = std::chrono::steady_clock::now();
	uint32_t presentsSinceReport = 0;
	uint32_t reports = 0;
	printf("Entering render loop (close window or wait ~20 reports to exit)\n");
	fflush(stdout);

	MSG msg{};
	bool running = true;
	while (running) {
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT)
				running = false;
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (!running)
			break;

		sl::FrameToken* frameToken{};
		if (skewed) {
			// Replicate the game's ordering bug: simStart on token N at "frame
			// start", then the counter bumps in the present hook and everything
			// else (constants/tags/present markers) lands on token N+1.
			CheckSL(api_slGetNewFrameToken(frameToken, &frameIndex), "slGetNewFrameToken");
			api_slReflexSleep(*frameToken);
			api_slPCLSetMarker(sl::PCLMarker::eSimulationStart, *frameToken);
			frameIndex++;
			CheckSL(api_slGetNewFrameToken(frameToken, &frameIndex), "slGetNewFrameToken(skewed)");
		} else {
			frameIndex++;
			CheckSL(api_slGetNewFrameToken(frameToken, &frameIndex), "slGetNewFrameToken");
			api_slReflexSleep(*frameToken);
			api_slPCLSetMarker(sl::PCLMarker::eSimulationStart, *frameToken);
		}

		consts.reset = sl::Boolean::eFalse;
		CheckSL(api_slSetConstants(consts, *frameToken, viewport), "slSetConstants");
		if (optsPerFrame) {
			// Mirror the game: re-issue options and query state every single frame.
			CheckSL(api_slDLSSGSetOptions(viewport, opts), "slDLSSGSetOptions(perframe)");
			sl::DLSSGState perFrameState{};
			api_slDLSSGGetState(viewport, perFrameState, &opts);
		}
		api_slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *frameToken);
		api_slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *frameToken);

		const float scroll = (float)frameIndex * kScrollPxPerFrame;

		if (interop) {
			// D3D11: render scene, copy into shared surfaces, signal cross fence.
			D3D11_MAPPED_SUBRESOURCE mapped{};
			Check(d11Context->Map(d11CB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "map cbuffer");
			((float*)mapped.pData)[0] = scroll;
			d11Context->Unmap(d11CB.Get(), 0);

			ID3D11RenderTargetView* rts[3] = { d11RTVs[0].Get(), d11RTVs[1].Get(), d11RTVs[2].Get() };
			d11Context->OMSetRenderTargets(3, rts, nullptr);
			D3D11_VIEWPORT vp11{ 0, 0, (float)kWidth, (float)kHeight, 0, 1 };
			d11Context->RSSetViewports(1, &vp11);
			d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			d11Context->VSSetShader(d11VS.Get(), nullptr, 0);
			d11Context->PSSetShader(d11PS.Get(), nullptr, 0);
			ID3D11Buffer* cbs[1] = { d11CB.Get() };
			d11Context->PSSetConstantBuffers(0, 1, cbs);
			d11Context->Draw(3, 0);

			d11Context->CopyResource(d11ColorSh.Get(), d11Color.Get());
			d11Context->CopyResource(d11MvecSh.Get(), d11Mvec.Get());
			d11Context->CopyResource(d11DepthSh.Get(), d11Depth.Get());

			ComPtr<ID3D11DeviceContext4> ctx4;
			Check(d11Context.As(&ctx4), "QI ID3D11DeviceContext4");
			crossFenceValue++;
			Check(ctx4->Signal(d11Fence.Get(), crossFenceValue), "d3d11 fence signal");
			queue->Wait(crossFence.Get(), crossFenceValue);
		}

		const UINT bb = swapChain->GetCurrentBackBufferIndex();
		if (frameFence[bb] && fence->GetCompletedValue() < frameFence[bb]) {
			fence->SetEventOnCompletion(frameFence[bb], fenceEvent);
			WaitForSingleObject(fenceEvent, INFINITE);
		}
		Check(allocators[bb]->Reset(), "allocator reset");
		Check(cmdList->Reset(allocators[bb].Get(), pipeline.Get()), "cmdlist reset");

		auto barrier = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
			D3D12_RESOURCE_BARRIER b{};
			b.Transition.pResource = res;
			b.Transition.StateBefore = from;
			b.Transition.StateAfter = to;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			cmdList->ResourceBarrier(1, &b);
		};

		ComPtr<ID3D12Resource> backBuffer;
		Check(swapChain->GetBuffer(bb, IID_PPV_ARGS(&backBuffer)), "GetBuffer");

		ID3D12Resource* tagDepth{};
		ID3D12Resource* tagMvec{};
		ID3D12Resource* tagHudless{};
		ID3D12Resource* tagUI{};

		if (interop) {
			// D3D12 side only copies the shared color to the backbuffer, like the game.
			barrier(shColor12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
			barrier(backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
			cmdList->CopyResource(backBuffer.Get(), shColor12.Get());
			barrier(backBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
			barrier(shColor12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			tagDepth = shDepth12.Get();
			tagMvec = shMvec12.Get();
			tagHudless = shColor12.Get();
			tagUI = shUI12.Get();
		} else {
			barrier(colorRT.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
			barrier(mvecRT.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
			barrier(depthTex.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);

			D3D12_VIEWPORT vp{ 0, 0, (float)kWidth, (float)kHeight, 0, 1 };
			D3D12_RECT scissor{ 0, 0, (LONG)kWidth, (LONG)kHeight };
			cmdList->RSSetViewports(1, &vp);
			cmdList->RSSetScissorRects(1, &scissor);
			auto rtv = [&](UINT i) {
				D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
				h.ptr += (SIZE_T)i * rtvSize;
				return h;
			};
			D3D12_CPU_DESCRIPTOR_HANDLE rts[2] = { rtv(0), rtv(1) };
			D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
			cmdList->OMSetRenderTargets(2, rts, FALSE, &dsv);
			cmdList->SetGraphicsRootSignature(rootSig.Get());
			float frameConsts[4] = { scroll, 0, 0, 0 };
			cmdList->SetGraphicsRoot32BitConstants(0, 4, frameConsts, 0);
			cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			cmdList->DrawInstanced(3, 1, 0, 0);

			barrier(colorRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			barrier(backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
			cmdList->CopyResource(backBuffer.Get(), colorRT.Get());
			barrier(backBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);

			barrier(colorRT.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			barrier(mvecRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
			barrier(depthTex.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COMMON);
			tagDepth = depthTex.Get();
			tagMvec = mvecRT.Get();
			tagHudless = colorRT.Get();
			tagUI = uiTex.Get();
		}

		sl::Extent extent{ 0, 0, kWidth, kHeight };
		auto makeRes = [&](ID3D12Resource* r) {
			sl::Resource res{};
			res.type = sl::ResourceType::eTex2d;
			res.native = r;
			res.state = D3D12_RESOURCE_STATE_COMMON;
			return res;
		};
		sl::Resource depthRes = makeRes(tagDepth);
		sl::Resource mvecRes = makeRes(tagMvec);
		sl::Resource hudlessRes = makeRes(tagHudless);
		sl::Resource uiRes = makeRes(tagUI);
		sl::ResourceTag tags[] = {
			{ &depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extent },
			{ &mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extent },
			{ &hudlessRes, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &extent },
			{ &uiRes, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &extent },
		};
		CheckSL(api_slSetTagForFrame(*frameToken, viewport, tags, _countof(tags), cmdList.Get()), "slSetTagForFrame");

		Check(cmdList->Close(), "cmdlist close");
		ID3D12CommandList* lists[] = { cmdList.Get() };
		queue->ExecuteCommandLists(1, lists);
		api_slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *frameToken);

		api_slPCLSetMarker(sl::PCLMarker::ePresentStart, *frameToken);
		if (pingpong) {
			// Alternate the presenting thread every 120 frames.
			PresentWorker& w = workers[(frameIndex / 120) % 2];
			w.Run([&] { Check(swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING), "Present"); });
		} else if (vsync) {
			Check(swapChain->Present(1, 0), "Present");
		} else if (tokenLag && (frameIndex % 8) == 0) {
			// Double-present on one frame token, like loading screens re-presenting
			// without a new simulation frame.
			Check(swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING), "Present");
			Check(swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING), "Present");
		} else {
			Check(swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING), "Present");
		}
		api_slPCLSetMarker(sl::PCLMarker::ePresentEnd, *frameToken);
		presentsSinceReport++;

		fenceValue++;
		queue->Signal(fence.Get(), fenceValue);
		frameFence[bb] = fenceValue;

		auto now = std::chrono::steady_clock::now();
		if (now - lastReport >= std::chrono::seconds(1)) {
			sl::DLSSGState st{};
			api_slDLSSGGetState(viewport, st, nullptr);
			printf("[%2u] render fps=%u  numFramesActuallyPresented=%u  status=0x%x\n",
				reports, presentsSinceReport, st.numFramesActuallyPresented, (uint32_t)st.status);
			fflush(stdout);
			presentsSinceReport = 0;
			lastReport = now;
			if (++reports >= 20)
				running = false;
		}
	}

	// Drain GPU before teardown.
	fenceValue++;
	queue->Signal(fence.Get(), fenceValue);
	if (fence->GetCompletedValue() < fenceValue) {
		fence->SetEventOnCompletion(fenceValue, fenceEvent);
		WaitForSingleObject(fenceEvent, INFINITE);
	}
	if (pingpong) {
		workers[0].Stop();
		workers[1].Stop();
	}
	sl::DLSSGOptions off{};
	off.mode = sl::DLSSGMode::eOff;
	api_slDLSSGSetOptions(viewport, off);
	api_slShutdown();
	CloseHandle(fenceEvent);
	printf("Done.\n");
	return 0;
}
