#include "ShaderPackageIncludes.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <winrt/base.h>

using uint = unsigned;
namespace RE
{
	enum class RENDER_TARGET
	{
		Mask
	};
	struct BSShader
	{
		enum class Type
		{
			Lighting,
			Grass,
			DistantTree,
			Sky,
			Effect,
			Water,
			ImageSpace
		};
		struct ShaderType
		{
			Type value = Type::Lighting;
			Type get() const { return value; }
		} shaderType;
	};
}
constexpr auto MASKS2 = RE::RENDER_TARGET::Mask;
namespace magic_enum
{
	constexpr std::string_view enum_name(RE::RENDER_TARGET) { return "Mask"; }
}
namespace Util
{
	template <class T>
	T* AsW32(T* value)
	{
		return value;
	}
#include "deferred_names_under_test.h"
}
namespace DX
{
	void ThrowIfFailed(HRESULT result)
	{
		if (FAILED(result))
			throw std::runtime_error(std::format("D3D failure: 0x{:08x}", static_cast<uint32_t>(result)));
	}
}
namespace logger
{
	unsigned errors = 0;
	template <class... T>
	void error(const char*, T...)
	{
		++errors;
	}
}
struct Target
{
	ID3D11Texture2D* texture = nullptr;
	ID3D11Texture2D* textureCopy = nullptr;
	ID3D11ShaderResourceView* SRV = nullptr;
	ID3D11ShaderResourceView* SRVCopy = nullptr;
	ID3D11RenderTargetView* RTV = nullptr;
	ID3D11UnorderedAccessView* UAV = nullptr;
};
struct Targets
{
	Target target;
	Target& operator[](RE::RENDER_TARGET) { return target; }
};
struct Renderer
{
	Targets renderTargets;
	Renderer& GetRuntimeData() { return *this; }
};
struct Device
{
	winrt::com_ptr<ID3D11Device> real;
	unsigned failStage = 0, calls = 0;
	bool Fail(unsigned stage)
	{
		++calls;
		return stage == failStage;
	}
	HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* i, ID3D11Texture2D** o)
	{
		return Fail(1) ? E_OUTOFMEMORY : real->CreateTexture2D(d, i, o);
	}
	HRESULT CreateShaderResourceView(ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC* d, ID3D11ShaderResourceView** o)
	{
		return Fail(2) ? E_OUTOFMEMORY : real->CreateShaderResourceView(r, d, o);
	}
	HRESULT CreateRenderTargetView(ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC* d, ID3D11RenderTargetView** o)
	{
		return Fail(3) ? E_OUTOFMEMORY : real->CreateRenderTargetView(r, d, o);
	}
	HRESULT CreateUnorderedAccessView(ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC* d, ID3D11UnorderedAccessView** o)
	{
		return Fail(4) ? E_OUTOFMEMORY : real->CreateUnorderedAccessView(r, d, o);
	}
};
struct Feature
{
	bool loaded = true, categories = false;
	unsigned fallbacks = 0;
	bool NeedsDeferredMaterialCategories() const { return categories; }
	void OnPixelShaderFallback(RE::BSShader::Type) { ++fallbacks; }
	template <class Callback>
	static void ForEachLoadedFeature(const char*, Callback callback)
	{
		for (auto* feature : GetFeatureList())
			if (feature->loaded)
				callback(feature);
	}
	static std::array<Feature*, 1> GetFeatureList()
	{
		static Feature feature;
		return { &feature };
	}
};
namespace globals
{
	namespace game
	{
		Renderer* renderer;
	}
	namespace d3d
	{
		Device* device;
	}
	struct State
	{
		uint32_t frameCount = 7;
		bool settingCustomShader = false;
		RE::BSShader* currentShader = nullptr;
	} stateStorage;
	State* state = &stateStorage;
}
class Deferred
{
public:
	static bool MaterialCategoriesRequested();
	void UpdateMaterialCategoryTarget();
	bool IsMaterialCategoriesEnabled() const { return materialCategoriesEnabled; }
	bool IsMaterialCategoriesReady() const;
	bool materialCategoriesEnabled = false, materialCategoriesValid = false, sceneDepthFinal = false, deferredPass = false;
	uint32_t materialCategoryFrame = 0;
	std::optional<bool> failedMaterialCategoryMode;
};
namespace globals
{
	Deferred* deferred;
}
namespace SIE
{
	struct ShaderCache
	{
		std::vector<uint32_t> requests;
		void GetPixelShader(const RE::BSShader&, uint32_t descriptor) { requests.push_back(descriptor); }
		void PrewarmDeferredPixelShaders(const RE::BSShader&, uint32_t, bool);
#include "deferred_flag_under_test.h"
	};
#include "deferred_prewarm_under_test.h"
}
void SetupRenderTarget(RE::RENDER_TARGET, D3D11_TEXTURE2D_DESC, D3D11_SHADER_RESOURCE_VIEW_DESC, D3D11_RENDER_TARGET_VIEW_DESC, D3D11_UNORDERED_ACCESS_VIEW_DESC, DXGI_FORMAT, uint);

#include "deferred_mask_under_test.h"
#include "deferred_target_under_test.h"

void Require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}
void CheckTarget(const Target& target, DXGI_FORMAT format, uint bindFlags)
{
	D3D11_TEXTURE2D_DESC texture{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	D3D11_RENDER_TARGET_VIEW_DESC rtv{};
	target.texture->GetDesc(&texture);
	target.SRV->GetDesc(&srv);
	target.RTV->GetDesc(&rtv);
	Require(texture.Format == format && srv.Format == format && rtv.Format == format, "Texture/view format mismatch");
	Require(texture.Width == 128 && texture.Height == 64 && texture.SampleDesc.Count == 1, "Layout switch changed dimensions or samples");
	Require(texture.BindFlags == bindFlags, "Layout switch changed bindings");
	if (bindFlags & D3D11_BIND_UNORDERED_ACCESS) {
		Require(target.UAV != nullptr, "VR fill lost its UAV");
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		target.UAV->GetDesc(&uav);
		Require(uav.Format == format, "VR UAV format did not follow the target");
	} else
		Require(!target.UAV, "Flat target unexpectedly gained a UAV");
	for (ID3D11DeviceChild* resource : std::array<ID3D11DeviceChild*, 4>{ target.texture, target.SRV, target.RTV, target.UAV }) {
		if (!resource)
			continue;
		std::array<char, 128> name{};
		UINT size = static_cast<UINT>(name.size());
		DX::ThrowIfFailed(resource->GetPrivateData(Util::WKPDID_D3DDebugObjectNameT, &size, name.data()));
		Require(std::string_view(name.data(), size).starts_with("Deferred::Mask"), "Resource debug name missing");
	}
}
void AllocationCases(Renderer& renderer, Device& device, bool uav)
{
	const uint bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
	D3D11_TEXTURE2D_DESC texture{};
	texture.Width = 128;
	texture.Height = 64;
	texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	D3D11_RENDER_TARGET_VIEW_DESC rtv{};
	rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	SetupRenderTarget(MASKS2, texture, srv, rtv, uavDesc, DXGI_FORMAT_R16_UNORM, bindFlags);
	auto& target = renderer.renderTargets[MASKS2];
	auto* feature = Feature::GetFeatureList()[0];
	Deferred deferred;
	for (bool grow : { true, false }) {
		const bool originalMode = !grow;
		if (originalMode) {
			feature->categories = true;
			deferred.UpdateMaterialCategoryTarget();
		}
		for (unsigned failure = 1; failure <= (uav ? 4u : 3u); ++failure) {
			const auto original = target;
			device.failStage = failure;
			feature->categories = grow;
			deferred.UpdateMaterialCategoryTarget();
			Require(deferred.materialCategoriesEnabled == originalMode && target.texture == original.texture &&
						target.SRV == original.SRV && target.RTV == original.RTV && target.UAV == original.UAV,
				"Failure published a partial target/layout");
			CheckTarget(target, originalMode ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM, bindFlags);
			const auto calls = device.calls;
			for (unsigned frame = 0; frame < 100; ++frame) deferred.UpdateMaterialCategoryTarget();
			Require(device.calls == calls, "Failed allocation retried every frame");
			feature->categories = originalMode;
			deferred.UpdateMaterialCategoryTarget();
			Require(!deferred.failedMaterialCategoryMode, "Toggling back did not rearm allocation");
		}
		device.failStage = 0;
	}
	feature->categories = false;
	deferred.UpdateMaterialCategoryTarget();
	for (unsigned transition = 0; transition < 20; ++transition) {
		feature->categories = (transition % 2) == 0;
		deferred.materialCategoriesValid = true;
		deferred.UpdateMaterialCategoryTarget();
		Require(deferred.materialCategoriesEnabled == feature->categories && !deferred.materialCategoriesValid, "Layout transition retained stale material inputs");
		CheckTarget(target, feature->categories ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM, bindFlags);
		const auto calls = device.calls;
		for (unsigned frame = 0; frame < 100; ++frame) deferred.UpdateMaterialCategoryTarget();
		Require(device.calls == calls, "Steady mode recreated its target");
	}
	feature->loaded = false;
	feature->categories = true;
	Require(!Deferred::MaterialCategoriesRequested(), "Disabled features requested a category lane");
	feature->loaded = true;
	feature->categories = false;
	ReleaseRenderTargetSlot(MASKS2);
	Require(!target.texture && !target.SRV && !target.RTV && !target.UAV, "Retirement retained target slots");
}
void PrewarmCases()
{
	auto* feature = Feature::GetFeatureList()[0];
	for (auto type : { RE::BSShader::Type::Lighting, RE::BSShader::Type::Grass, RE::BSShader::Type::DistantTree,
			 RE::BSShader::Type::Sky, RE::BSShader::Type::Effect, RE::BSShader::Type::Water })
		for (bool loaded : { false, true })
			for (bool requested : { false, true })
				for (bool previouslyWide : { false, true }) {
					feature->loaded = loaded;
					feature->categories = requested;
					RE::BSShader shader;
					shader.shaderType.value = type;
					SIE::ShaderCache cache;
					const auto flag = SIE::ShaderCache::GetMaterialCategoryFlag(type);
					constexpr uint32_t narrow = 0x10001;
					cache.PrewarmDeferredPixelShaders(shader, narrow | (previouslyWide ? flag : 0), Deferred::MaterialCategoriesRequested());
					Require(cache.requests.front() == narrow, "AO-only prewarming retained category bits");
					if (loaded && requested && flag) {
						Require(cache.requests.size() == 2 && cache.requests.back() == (narrow | flag),
							"Requested category variant was not prewarmed");
					} else {
						Require(cache.requests.size() == 1, "Unused category shader was prewarmed");
					}
				}
	feature->loaded = true;
	feature->categories = false;
	std::cout << "Deferred prewarming: 48 demand/layout conditions passed\n";
}
void FallbackNotifications()
{
	Deferred deferred;
	globals::deferred = &deferred;
	RE::BSShader shader;
	auto* state = globals::state;
	auto* feature = Feature::GetFeatureList()[0];
	for (bool categories : { false, true })
		for (bool active : { false, true })
			for (bool custom : { false, true })
				for (bool native : { false, true })
					for (bool known : { false, true }) {
						deferred.materialCategoriesEnabled = categories;
						deferred.deferredPass = active;
						state->settingCustomShader = custom;
						state->currentShader = known ? &shader : nullptr;
						auto* a_pixelShader = native ? &shader : nullptr;
						const auto before = feature->fallbacks;
#include "deferred_fallback_under_test.h"
						Require(feature->fallbacks - before == unsigned(categories && active && !custom && native && known),
							"Fallback notification escaped an authored deferred pass");
					}
	state->settingCustomShader = false;
	state->currentShader = nullptr;
	globals::deferred = nullptr;
	std::cout << "Native fallback notifications: 32 rendering/binding conditions passed\n";
}
void AdmissionCases()
{
	Deferred deferred;
	deferred.materialCategoriesEnabled = deferred.materialCategoriesValid = deferred.sceneDepthFinal = true;
	deferred.materialCategoryFrame = globals::state->frameCount;
	Require(deferred.IsMaterialCategoriesReady(), "Completed material lane not admitted");
	deferred.deferredPass = true;
	Require(!deferred.IsMaterialCategoriesReady(), "Active deferred compositing admitted its material lane");
	deferred.deferredPass = false;
	Require(deferred.IsMaterialCategoriesReady(), "Ending deferred rendering did not admit the complete lane");
	++globals::state->frameCount;
	Require(!deferred.IsMaterialCategoriesReady(), "Previous frame's material lane was admitted");
	deferred.materialCategoryFrame = globals::state->frameCount;
	deferred.materialCategoriesValid = false;
	Require(!deferred.IsMaterialCategoriesReady(), "Native shader fallback admitted incomplete categories");
	deferred.materialCategoriesValid = true;
	deferred.materialCategoriesEnabled = false;
	Require(!deferred.IsMaterialCategoriesReady(), "AO-only layout admitted categories");
	deferred.materialCategoriesEnabled = true;
	deferred.sceneDepthFinal = false;
	Require(!deferred.IsMaterialCategoriesReady(), "Incomplete deferred pass admitted categories");
	for (auto type : { RE::BSShader::Type::Lighting, RE::BSShader::Type::Grass, RE::BSShader::Type::DistantTree, RE::BSShader::Type::Sky, RE::BSShader::Type::Effect }) {
		const auto flag = SIE::ShaderCache::GetMaterialCategoryFlag(type);
		Require(flag != 0 && (flag & (flag - 1)) == 0, "Layout descriptor must have one reserved bit");
		Require((0x10001u | flag) != 0x10001u, "AO and category variants share a cache descriptor");
	}
	Require(SIE::ShaderCache::GetMaterialCategoryFlag(RE::BSShader::Type::Water) == 0, "Water gained an unsupported layout variant");
}
winrt::com_ptr<ID3DBlob> Compile(const char* text, const char* profile)
{
	winrt::com_ptr<ID3DBlob> code, errors;
	const auto hr = D3DCompile(text, std::strlen(text), nullptr, nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
	if (errors)
		std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
	DX::ThrowIfFailed(hr);
	return code;
}
void AoPrecisionAndFill(Device& device, ID3D11DeviceContext* context)
{
	auto vertex = Compile("float4 main(uint id:SV_VertexID):SV_Position { return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1); }", "vs_5_0");
	auto pixel = Compile("float4 main():SV_Target { return float4(0.12345, 100.0/255.0, 0, 0.75); }", "ps_5_0");
	auto compute = Compile("RWTexture2D<float2> masks:register(u0); [numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) { masks[uint2(1,0)] = masks[uint2(0,0)]; }", "cs_5_0");
	winrt::com_ptr<ID3D11VertexShader> vs;
	winrt::com_ptr<ID3D11PixelShader> ps;
	winrt::com_ptr<ID3D11ComputeShader> cs;
	DX::ThrowIfFailed(device.real->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, vs.put()));
	DX::ThrowIfFailed(device.real->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, ps.put()));
	DX::ThrowIfFailed(device.real->CreateComputeShader(compute->GetBufferPointer(), compute->GetBufferSize(), nullptr, cs.put()));
	Util::SetResourceName(vs.get(), "DeferredMaskTest::AO VS");
	Util::SetResourceName(ps.get(), "DeferredMaskTest::AO PS");
	Util::SetResourceName(cs.get(), "DeferredMaskTest::Fill CS");
	uint16_t narrow = 0;
	for (DXGI_FORMAT format : { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM }) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = 2;
		desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = format;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
		winrt::com_ptr<ID3D11Texture2D> texture, readback;
		winrt::com_ptr<ID3D11RenderTargetView> rtv;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		DX::ThrowIfFailed(device.real->CreateTexture2D(&desc, nullptr, texture.put()));
		DX::ThrowIfFailed(device.real->CreateRenderTargetView(texture.get(), nullptr, rtv.put()));
		DX::ThrowIfFailed(device.real->CreateUnorderedAccessView(texture.get(), nullptr, uav.put()));
		Util::SetResourceName(texture.get(), "DeferredMaskTest::AO");
		Util::SetResourceName(rtv.get(), "DeferredMaskTest::AO RTV");
		Util::SetResourceName(uav.get(), "DeferredMaskTest::AO UAV");
		auto* view = rtv.get();
		context->OMSetRenderTargets(1, &view, nullptr);
		D3D11_VIEWPORT viewport{ 0, 0, 2, 1, 0, 1 };
		context->RSSetViewports(1, &viewport);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(vs.get(), nullptr, 0);
		context->PSSetShader(ps.get(), nullptr, 0);
		context->Draw(3, 0);
		context->OMSetRenderTargets(0, nullptr, nullptr);
		auto* output = uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
		context->CSSetShader(cs.get(), nullptr, 0);
		context->Dispatch(1, 1, 1);
		context->ClearState();
		desc.BindFlags = 0;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		DX::ThrowIfFailed(device.real->CreateTexture2D(&desc, nullptr, readback.put()));
		Util::SetResourceName(readback.get(), "DeferredMaskTest::Readback");
		context->CopyResource(readback.get(), texture.get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		DX::ThrowIfFailed(context->Map(readback.get(), 0, D3D11_MAP_READ, 0, &mapped));
		const auto* values = static_cast<const uint16_t*>(mapped.pData);
		const auto channels = format == DXGI_FORMAT_R16_UNORM ? 1u : 2u;
		const auto value = values[0];
		const auto copied = values[channels];
		context->Unmap(readback.get(), 0);
		Require(value == copied && value != 0 && value % 257 != 0, "Typed UAV fill lost 16-bit vertex AO precision");
		if (format == DXGI_FORMAT_R16_UNORM)
			narrow = value;
		else
			Require(value == narrow, "R16/RG16 changed vertex AO");
	}
}
void ShaderOutputs(const std::filesystem::path& root)
{
	unsigned permutations = 0;
	struct Variant
	{
		const char* family;
		std::vector<const char*> defines;
	};
	const std::vector<Variant> variants{
		{ "Sky", { "TEX" } }, { "Sky", { "TEX", "CLOUDS" } }, { "Sky", { "TEX", "DITHER" } },
		{ "DistantTree", {} }, { "DistantTree", { "DO_ALPHA_TEST" } },
		{ "Effect", {} }, { "Effect", { "MULTBLEND_DECAL" } }, { "Effect", { "TEXTURE", "ALPHA_TEST" } },
		{ "Lighting", {} }, { "Lighting", { "FACEGEN" } }, { "Lighting", { "FACEGEN_RGB_TINT" } },
		{ "Lighting", { "HAIR" } }, { "Lighting", { "EYE" } }, { "Lighting", { "TREE_ANIM" } },
		{ "Lighting", { "LANDSCAPE" } }, { "Lighting", { "LODLANDSCAPE" } },
		{ "RunGrass", {} }, { "RunGrass", { "GRASS_LIGHTING" } },
		{ "RunGrass", { "GRASS_LIGHTING", "GRASS_OPTIMIZATIONS" } }, { "RunGrass", { "GRASS_LIGHTING", "TRUE_PBR" } }
	};
	for (const auto& variant : variants) {
		const auto* family = variant.family;
		for (bool vr : { false, true })
			for (bool categories : { false, true }) {
				std::vector<D3D_SHADER_MACRO> macros{ { "PSHADER", "1" } };
				if (std::string_view(family) != "RunGrass")
					macros.push_back({ "DEFERRED", "1" });
				for (auto* define : variant.defines)
					macros.push_back({ define, "1" });
				if (vr)
					macros.push_back({ "VR", "1" });
				if (categories)
					macros.push_back({ "MATERIAL_CATEGORY", "1" });
				macros.push_back({ nullptr, nullptr });
				winrt::com_ptr<ID3DBlob> code, errors;
				const auto path = root / (std::string(family) + ".hlsl");
				PackageIncludes includes(root, root.parent_path().parent_path() / "features/Grass Lighting/Shaders");
				const auto hr = D3DCompileFromFile(path.c_str(), macros.data(), &includes, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
				if (FAILED(hr)) {
					std::cerr << family << (vr ? " VR" : " flat") << (categories ? " categories" : " AO");
					for (auto* define : variant.defines)
						std::cerr << ' ' << define;
					std::cerr << '\n';
				}
				if (errors)
					std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
				DX::ThrowIfFailed(hr);
				winrt::com_ptr<ID3D11ShaderReflection> reflection;
				DX::ThrowIfFailed(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), __uuidof(ID3D11ShaderReflection), reflection.put_void()));
				D3D11_SHADER_DESC desc{};
				DX::ThrowIfFailed(reflection->GetDesc(&desc));
				bool writesMask = false;
				for (UINT i = 0; i < desc.OutputParameters; ++i) {
					D3D11_SIGNATURE_PARAMETER_DESC output{};
					DX::ThrowIfFailed(reflection->GetOutputParameterDesc(i, &output));
					writesMask |= output.SystemValueType == D3D_NAME_TARGET && output.SemanticIndex == 7;
				}
				const bool ao = std::string_view(family) == "Lighting" || std::string_view(family) == "RunGrass";
				Require(writesMask == (categories || ao), std::format("{} exported the wrong mask layout", family));
				++permutations;
			}
	}
	std::cout << "Deferred shader outputs: " << permutations << " flat/VR layout permutations passed\n";
}
int wmain(int argc, wchar_t** argv)
{
	try {
		Require(argc == 2, "Pass the package shader directory");
		Renderer renderer;
		Device device;
		winrt::com_ptr<ID3D11DeviceContext> context;
		globals::game::renderer = &renderer;
		globals::d3d::device = &device;
		DX::ThrowIfFailed(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device.real.put(), nullptr, context.put()));
		AllocationCases(renderer, device, false);
		AllocationCases(renderer, device, true);
		Require(logger::errors == 14, "Allocation failures were not reported exactly once");
		AdmissionCases();
		PrewarmCases();
		FallbackNotifications();
		AoPrecisionAndFill(device, context.get());
		ShaderOutputs(argv[1]);
		std::cout << "Deferred mask: 40 layout transitions, 14 allocation rollbacks, bounded retries, current-frame admission, AO precision and typed UAV fill passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
