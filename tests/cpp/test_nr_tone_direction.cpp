// Regression coverage for the directional bounds on the production NR composite.

#include "Features/Upscaling/NeuralRendering/ContextProfile.h"
#include "Features/Upscaling/NeuralRendering/Tuning.h"
#include "Utils/ShaderInclude.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>
#include <winrt/base.h>

// ColorContract.hlsli is deliberately scalar-only. These wrappers let the
// compiler exercise that exact production HLSL function as native CPU C++.
inline float clamp(float value, float lower, float upper) { return std::clamp(value, lower, upper); }
using std::exp2;
inline bool isfinite(float value) { return std::isfinite(value); }
static bool ENABLE_LL = true;
namespace Color
{
	inline constexpr float GameGamma = 1.6f;
	inline float LinearToSkyrimGamma(float value) { return std::pow(std::abs(value), 1.0f / GameGamma); }
}
#include "Upscaling/NeuralRendering/ColorContract.hlsli"

static_assert(NR::Tuning::kMaxToneStops == NR::kMaxToneStops,
	"The host settings and shader must retain the same hard two-stop composite cap");

namespace
{
	struct WorkingDirectory
	{
		std::filesystem::path previous = std::filesystem::current_path();
		std::filesystem::path root;

		WorkingDirectory()
		{
			wchar_t temp[MAX_PATH];
			wchar_t file[MAX_PATH];
			REQUIRE(GetTempPathW(MAX_PATH, temp) != 0);
			REQUIRE(GetTempFileNameW(temp, L"nrt", 0, file) != 0);
			root = file;
			std::filesystem::remove(root);
			std::filesystem::create_directories(root / "Data/Shaders/Upscaling/NeuralRendering");
			std::filesystem::current_path(root);
		}

		~WorkingDirectory()
		{
			std::error_code error;
			std::filesystem::current_path(previous, error);
			std::filesystem::remove_all(root, error);
		}
	};

	std::string ReadFile(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input)
			throw std::runtime_error("cannot read production shader source: " + path.string());
		return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
	}

	void CopyTree(const std::filesystem::path& source, const std::filesystem::path& destination)
	{
		for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
			const auto relative = std::filesystem::relative(entry.path(), source);
			const auto target = destination / relative;
			if (entry.is_directory())
				std::filesystem::create_directories(target);
			else {
				std::filesystem::create_directories(target.parent_path());
				std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing);
			}
		}
	}

	std::string ExtractCbuffer(std::string_view source, std::string_view name)
	{
		const auto declaration = source.find("cbuffer " + std::string(name));
		if (declaration == std::string_view::npos)
			throw std::runtime_error("production shader cbuffer declaration is missing");
		const auto open = source.find('{', declaration);
		if (open == std::string_view::npos)
			throw std::runtime_error("production shader cbuffer has no opening brace");
		size_t depth = 0;
		for (size_t i = open; i < source.size(); ++i) {
			if (source[i] == '{')
				++depth;
			else if (source[i] == '}' && --depth == 0)
				return std::string(source.substr(declaration, i - declaration + 1)) + ";";
		}
		throw std::runtime_error("production shader cbuffer is unterminated");
	}

	struct CompiledShader
	{
		winrt::com_ptr<ID3DBlob> bytecode;
		winrt::com_ptr<ID3DBlob> diagnostics;
	};

	CompiledShader CompileWithProductionInclude(std::string source)
	{
		Util::CustomInclude include("Data/Shaders/ToneDirectionTest.hlsl");
		CompiledShader result;
		const HRESULT compiled = D3DCompile(source.data(), source.size(), "ToneDirectionTest.hlsl", nullptr, &include,
			"main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, result.bytecode.put(), result.diagnostics.put());
		const std::string diagnostic = result.diagnostics ? static_cast<const char*>(result.diagnostics->GetBufferPointer()) : "No shader diagnostics";
		INFO(diagnostic);
		REQUIRE(SUCCEEDED(compiled));
		return result;
	}

	CompiledShader CompileShaderFileWithProductionInclude(const std::filesystem::path& path, const char* entryPoint, const D3D_SHADER_MACRO* defines = nullptr)
	{
		Util::CustomInclude include(path);
		CompiledShader result;
		const HRESULT compiled = D3DCompileFromFile(path.c_str(), defines, &include, entryPoint, "cs_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS, 0, result.bytecode.put(), result.diagnostics.put());
		const std::string diagnostic = result.diagnostics ? static_cast<const char*>(result.diagnostics->GetBufferPointer()) : "No shader diagnostics";
		INFO(diagnostic);
		REQUIRE(SUCCEEDED(compiled));
		return result;
	}

	D3D11_SHADER_BUFFER_DESC ReflectConstantBuffer(const CompiledShader& shader, ID3D11ShaderReflection*& reflection,
		ID3D11ShaderReflectionConstantBuffer*& buffer)
	{
		REQUIRE(SUCCEEDED(D3DReflect(shader.bytecode->GetBufferPointer(), shader.bytecode->GetBufferSize(),
			__uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&reflection))));
		buffer = reflection->GetConstantBufferByName("ColorTransfer");
		REQUIRE(buffer != nullptr);
		D3D11_SHADER_BUFFER_DESC description{};
		REQUIRE(SUCCEEDED(buffer->GetDesc(&description)));
		return description;
	}
}

TEST_CASE("Directional tone controls default neutral, sanitize, and copy with context tuning", "[nr][tone]")
{
	STATIC_REQUIRE(NR::Tuning::kMaxToneStops == 2.0f);
	NR::Tuning tuning;
	CHECK(tuning.brightening == 1.0f);
	CHECK(tuning.darkening == 1.0f);
	CHECK(tuning.maxBrighteningStops == 2.0f);
	CHECK(tuning.maxDarkeningStops == 2.0f);

	SECTION("finite inputs clamp to the supported range and non-finite inputs return to neutral")
	{
		tuning.brightening = -1.0f;
		tuning.darkening = std::numeric_limits<float>::infinity();
		tuning.maxBrighteningStops = -std::numeric_limits<float>::infinity();
		tuning.maxDarkeningStops = std::numeric_limits<float>::quiet_NaN();
		tuning.Sanitize();
		CHECK(tuning.brightening == 0.0f);
		CHECK(tuning.darkening == 1.0f);
		CHECK(tuning.maxBrighteningStops == 2.0f);
		CHECK(tuning.maxDarkeningStops == 2.0f);

		tuning.brightening = std::numeric_limits<float>::quiet_NaN();
		tuning.darkening = 3.0f;
		tuning.maxBrighteningStops = 3.0f;
		tuning.maxDarkeningStops = -1.0f;
		tuning.Sanitize();
		CHECK(tuning.brightening == 1.0f);
		CHECK(tuning.darkening == 2.0f);
		CHECK(tuning.maxBrighteningStops == 2.0f);
		CHECK(tuning.maxDarkeningStops == 0.0f);
	}

	SECTION("the full-range fields survive effective tuning in normal and dialogue contexts")
	{
		tuning.brightening = 0.35f;
		tuning.darkening = 1.7f;
		tuning.maxBrighteningStops = 0.75f;
		tuning.maxDarkeningStops = 1.25f;
		NR::Context::Profiles profiles;
		for (const auto kind : { NR::Context::Kind::kNormal, NR::Context::Kind::kDialogue }) {
			const auto effective = NR::Context::EffectiveTuning(tuning, profiles, kind);
			CHECK(effective.brightening == Catch::Approx(0.35f));
			CHECK(effective.darkening == Catch::Approx(1.7f));
			CHECK(effective.maxBrighteningStops == Catch::Approx(0.75f));
			CHECK(effective.maxDarkeningStops == Catch::Approx(1.25f));
		}
	}
}

TEST_CASE("Production HLSL composite preserves neutral legacy gain and applies directional limits", "[nr][tone]")
{
	const std::vector<float> tones{ -std::numeric_limits<float>::infinity(), -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f,
		std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() };
	for (const bool linearLighting : { true, false }) {
		ENABLE_LL = linearLighting;
		for (const float tone : tones) {
			const float sanitizedTone = std::isfinite(tone) ? tone : 0.0f;
			const float oldLinearGain = std::exp2(std::clamp(sanitizedTone, -2.0f, 2.0f));
			const float oldGain = linearLighting ? oldLinearGain : Color::LinearToSkyrimGamma(oldLinearGain);
			CHECK(NR::CompositeGain(tone, 1.0f, 1.0f, 2.0f, 2.0f) == Catch::Approx(oldGain));
		}
		CHECK(NR::CompositeGain(0.0f, 0.0f, 0.0f, 0.0f, 0.0f) == Catch::Approx(1.0f));
	}

	ENABLE_LL = true;
	CHECK(NR::CompositeGain(1.0f, 0.0f, 1.0f, 2.0f, 2.0f) == Catch::Approx(1.0f));
	CHECK(NR::CompositeGain(-1.0f, 1.0f, 0.0f, 2.0f, 2.0f) == Catch::Approx(1.0f));
	CHECK(NR::CompositeGain(1.0f, 2.0f, 0.0f, 2.0f, 2.0f) == Catch::Approx(4.0f));
	CHECK(NR::CompositeGain(-1.0f, 0.0f, 2.0f, 2.0f, 2.0f) == Catch::Approx(0.25f));
	CHECK(NR::CompositeGain(2.0f, 2.0f, 0.0f, 0.5f, 2.0f) == Catch::Approx(std::exp2(0.5f)));
	CHECK(NR::CompositeGain(-2.0f, 0.0f, 2.0f, 2.0f, 0.5f) == Catch::Approx(std::exp2(-0.5f)));
	CHECK(NR::CompositeGain(2.0f, 2.0f, 0.0f, 10.0f, 2.0f) == Catch::Approx(4.0f));
	CHECK(NR::CompositeGain(-2.0f, 0.0f, 2.0f, 2.0f, 10.0f) == Catch::Approx(0.25f));
	CHECK(NR::CompositeGain(1.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f, 2.0f, 2.0f) == Catch::Approx(2.0f));
	CHECK(NR::CompositeGain(-1.0f, 1.0f, std::numeric_limits<float>::infinity(), 2.0f, 2.0f) == Catch::Approx(0.5f));
	CHECK(NR::CompositeGain(2.0f, 1.0f, 1.0f, std::numeric_limits<float>::quiet_NaN(), 2.0f) == Catch::Approx(4.0f));
	CHECK(NR::CompositeGain(-2.0f, 1.0f, 1.0f, 2.0f, std::numeric_limits<float>::infinity()) == Catch::Approx(0.25f));
}

TEST_CASE("Production tone shader include compiles through the runtime handler and exposes the expected cbuffer layout", "[nr][tone][shader]")
{
	WorkingDirectory working;
	const std::filesystem::path contractSource = NR_TONE_DIRECTION_CONTRACT_SOURCE;
	const std::filesystem::path cbufferSource = NR_TONE_DIRECTION_CBUFFER_SOURCE;
	const std::filesystem::path shaderSource = NR_TONE_DIRECTION_SHADER_SOURCE;
	const auto contractPath = working.root / "Data/Shaders/Upscaling/NeuralRendering/ColorContract.hlsli";
	std::filesystem::copy_file(contractSource, contractPath);

	const auto cbuffer = ExtractCbuffer(ReadFile(cbufferSource), "ColorTransfer");
	// Verify the production helper and the extracted production parameter block
	// compile together with the same include handler used by runtime shaders.
	const auto source = std::string("#define ENABLE_LL 1\nnamespace Color { float LinearToSkyrimGamma(float value) { return value; } }\n") +
	                    "#include \"Upscaling/NeuralRendering/ColorContract.hlsli\"\n" + cbuffer +
	                    "\nRWByteAddressBuffer Sink : register(u0);\n[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID) { "
	                    "Sink.Store(0, Width + Height + asuint(BrighteningWeight + DarkeningWeight + MaxBrighteningStops + MaxDarkeningStops + "
	                    "NR::CompositeGain(0.25, 1, 1, 2, 2))); }\n";
	const auto compiled = CompileWithProductionInclude(source);

	ID3D11ShaderReflection* reflection = nullptr;
	ID3D11ShaderReflectionConstantBuffer* buffer = nullptr;
	const auto description = ReflectConstantBuffer(compiled, reflection, buffer);
	CHECK(description.Size == 288u);
	D3D11_SHADER_VARIABLE_DESC variable{};
	auto* reflected = buffer->GetVariableByName("BrighteningWeight");
	REQUIRE(reflected != nullptr);
	REQUIRE(SUCCEEDED(reflected->GetDesc(&variable)));
	CHECK(variable.StartOffset == 136u);
	reflected = buffer->GetVariableByName("DarkeningWeight");
	REQUIRE(reflected != nullptr);
	REQUIRE(SUCCEEDED(reflected->GetDesc(&variable)));
	CHECK(variable.StartOffset == 140u);
	reflected = buffer->GetVariableByName("MaxBrighteningStops");
	REQUIRE(reflected != nullptr);
	REQUIRE(SUCCEEDED(reflected->GetDesc(&variable)));
	CHECK(variable.StartOffset == 272u);
	reflected = buffer->GetVariableByName("MaxDarkeningStops");
	REQUIRE(reflected != nullptr);
	REQUIRE(SUCCEEDED(reflected->GetDesc(&variable)));
	CHECK(variable.StartOffset == 276u);
	reflection->Release();

	// Compile actual production ColorTransfer and GainReuse entrypoints with all
	// runtime-relative includes resolved by Util::CustomInclude. D3DCompile is
	// CPU-only; this does not create a D3D device or dispatch a shader.
	const auto shaderRoot = working.root / "Data/Shaders";
	CopyTree(NR_TONE_DIRECTION_PACKAGE_SHADER_ROOT, shaderRoot);
	CopyTree(NR_TONE_DIRECTION_FEATURE_SHADER_ROOT, shaderRoot);
	const D3D_SHADER_MACRO vrDefines[]{ { "VR", "1" }, { nullptr, nullptr } };
	for (const auto* defines : { static_cast<const D3D_SHADER_MACRO*>(nullptr), vrDefines }) {
		const auto colorTransfer = shaderRoot / "Upscaling/NeuralRendering/ColorTransferCS.hlsl";
		for (const char* entryPoint : { "PrepareToneData", "Prepare", "Composite" }) {
			const auto productionShader = CompileShaderFileWithProductionInclude(colorTransfer, entryPoint, defines);
			CHECK(productionShader.bytecode->GetBufferSize() > 0);
		}
		const auto gainReuse = shaderRoot / "Upscaling/NeuralRendering/GainReuseCS.hlsl";
		for (const char* entryPoint : { "StoreGain", "ReuseGain" }) {
			const auto productionShader = CompileShaderFileWithProductionInclude(gainReuse, entryPoint, defines);
			CHECK(productionShader.bytecode->GetBufferSize() > 0);
		}
	}
}
