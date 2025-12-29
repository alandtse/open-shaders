// Reusable Binding Fixtures for HLSL Tests
// Provides common mock resources (textures, cbuffers, samplers) that can be
// referenced by HLSL tests via simple annotations
#pragma once

#include "test_common.h"
#include <D3D12/Raytracing/ShaderBindingTable.h>
#include <Framework/HLSLFramework.h>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>

namespace ShaderTest
{
	// ============================================================================
	// FIXTURE REGISTRY
	// ============================================================================
	// Central registry of all available binding fixtures
	// HLSL tests reference these by name via annotations

	class BindingFixture
	{
	public:
		virtual ~BindingFixture() = default;
		virtual void Apply(stf::DescriptorTableBuilder& builder, uint32_t bindingSlot) = 0;
		virtual const char* GetName() const = 0;
	};

	// ============================================================================
	// COMMON FIXTURES
	// ============================================================================

	/// Simple 8x8 test texture filled with gradient values (0.0 to 1.0)
	class GradientTexture2D : public BindingFixture
	{
	public:
		const char* GetName() const override { return "GradientTexture2D"; }

		void Apply(stf::DescriptorTableBuilder& builder, uint32_t bindingSlot) override
		{
			constexpr uint32_t Width = 8;
			constexpr uint32_t Height = 8;

			// Create gradient data: linear interpolation from 0.0 to 1.0
			std::array<float, Width * Height> data;
			for (uint32_t y = 0; y < Height; ++y) {
				for (uint32_t x = 0; x < Width; ++x) {
					float u = static_cast<float>(x) / (Width - 1);
					float v = static_cast<float>(y) / (Height - 1);
					data[y * Width + x] = (u + v) * 0.5f;  // Average of u and v
				}
			}

			builder.AddSRV(bindingSlot, stf::Tex2D(Width, Height, DXGI_FORMAT_R32_FLOAT, data.data()));
		}
	};

	/// Simple 8x8 test texture filled with constant value
	class ConstantTexture2D : public BindingFixture
	{
		float m_value;

	public:
		explicit ConstantTexture2D(float value = 1.0f) :
			m_value(value) {}

		const char* GetName() const override { return "ConstantTexture2D"; }

		void Apply(stf::DescriptorTableBuilder& builder, uint32_t bindingSlot) override
		{
			constexpr uint32_t Width = 8;
			constexpr uint32_t Height = 8;
			std::array<float, Width * Height> data;
			data.fill(m_value);

			builder.AddSRV(bindingSlot, stf::Tex2D(Width, Height, DXGI_FORMAT_R32_FLOAT, data.data()));
		}
	};

	/// Simple UAV (RWTexture2D) for output
	class OutputTexture2D : public BindingFixture
	{
		uint32_t m_width;
		uint32_t m_height;

	public:
		OutputTexture2D(uint32_t width = 8, uint32_t height = 8) :
			m_width(width), m_height(height) {}

		const char* GetName() const override { return "OutputTexture2D"; }

		void Apply(stf::DescriptorTableBuilder& builder, uint32_t bindingSlot) override
		{
			builder.AddUAV(bindingSlot, stf::Tex2D(m_width, m_height, DXGI_FORMAT_R32G32B32A32_FLOAT));
		}
	};

	/// Simple constant buffer with common shader data
	struct CommonCBufferData
	{
		float time;
		float deltaTime;
		float frameIndex;
		float _padding;
	};

	class CommonCBuffer : public BindingFixture
	{
		CommonCBufferData m_data;

	public:
		CommonCBuffer(float time = 0.0f, float deltaTime = 0.016f, float frameIndex = 0.0f)
		{
			m_data.time = time;
			m_data.deltaTime = deltaTime;
			m_data.frameIndex = frameIndex;
			m_data._padding = 0.0f;
		}

		const char* GetName() const override { return "CommonCBuffer"; }

		void Apply(stf::DescriptorTableBuilder& builder, uint32_t bindingSlot) override
		{
			builder.AddCBV(bindingSlot, stf::ConstantBuffer(&m_data, sizeof(m_data)));
		}
	};

	/// Linear clamp sampler (most common)
	class LinearClampSampler : public BindingFixture
	{
	public:
		const char* GetName() const override { return "LinearClampSampler"; }

		void Apply(stf::DescriptorTableBuilder& builder, uint32_t bindingSlot) override
		{
			D3D12_SAMPLER_DESC desc = {};
			desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			desc.MipLODBias = 0.0f;
			desc.MaxAnisotropy = 1;
			desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			desc.MinLOD = 0.0f;
			desc.MaxLOD = D3D12_FLOAT32_MAX;

			builder.AddStaticSampler(bindingSlot, desc);
		}
	};

	/// Point clamp sampler
	class PointClampSampler : public BindingFixture
	{
	public:
		const char* GetName() const override { return "PointClampSampler"; }

		void Apply(stf::DescriptorTableBuilder& builder, uint32_t bindingSlot) override
		{
			D3D12_SAMPLER_DESC desc = {};
			desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
			desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			desc.MipLODBias = 0.0f;
			desc.MaxAnisotropy = 1;
			desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			desc.MinLOD = 0.0f;
			desc.MaxLOD = D3D12_FLOAT32_MAX;

			builder.AddStaticSampler(bindingSlot, desc);
		}
	};

	// ============================================================================
	// FIXTURE FACTORY
	// ============================================================================

	class FixtureFactory
	{
	public:
		using FactoryFunc = std::function<std::unique_ptr<BindingFixture>()>;

		static FixtureFactory& Instance()
		{
			static FixtureFactory instance;
			return instance;
		}

		void Register(const std::string& name, FactoryFunc factory)
		{
			m_factories[name] = factory;
		}

		std::unique_ptr<BindingFixture> Create(const std::string& name)
		{
			auto it = m_factories.find(name);
			if (it != m_factories.end()) {
				return it->second();
			}
			return nullptr;
		}

	private:
		FixtureFactory()
		{
			// Register built-in fixtures
			Register("GradientTexture2D", []() { return std::make_unique<GradientTexture2D>(); });
			Register("ConstantTexture2D", []() { return std::make_unique<ConstantTexture2D>(); });
			Register("OutputTexture2D", []() { return std::make_unique<OutputTexture2D>(); });
			Register("CommonCBuffer", []() { return std::make_unique<CommonCBuffer>(); });
			Register("LinearClampSampler", []() { return std::make_unique<LinearClampSampler>(); });
			Register("PointClampSampler", []() { return std::make_unique<PointClampSampler>(); });
		}

		std::unordered_map<std::string, FactoryFunc> m_factories;
	};

	// ============================================================================
	// FIXTURE ANNOTATION PARSER
	// ============================================================================

	struct BindingAnnotation
	{
		std::string fixtureName;  // e.g., "GradientTexture2D"
		uint32_t registerSlot;    // e.g., 0 for t0, b0, u0, s0
		char registerType;        // 't', 'b', 'u', 's'
	};

	/// Parse fixture annotations from HLSL comments
	/// Format: /// @fixture(t0) GradientTexture2D
	///         /// @fixture(b0) CommonCBuffer
	///         /// @fixture(s0) LinearClampSampler
	inline std::vector<BindingAnnotation> ParseFixtureAnnotations(const std::string& hlslSource)
	{
		std::vector<BindingAnnotation> annotations;
		std::regex fixturePattern(R"(@fixture\(([tbus])(\d+)\)\s+(\w+))");

		std::istringstream stream(hlslSource);
		std::string line;
		while (std::getline(stream, line)) {
			std::smatch match;
			if (std::regex_search(line, match, fixturePattern)) {
				BindingAnnotation annotation;
				annotation.registerType = match[1].str()[0];
				annotation.registerSlot = std::stoi(match[2].str());
				annotation.fixtureName = match[3].str();
				annotations.push_back(annotation);
			}
		}

		return annotations;
	}

	/// Apply fixtures to a descriptor table based on annotations
	inline void ApplyFixtures(
		const std::vector<BindingAnnotation>& annotations,
		stf::DescriptorTableBuilder& builder)
	{
		for (const auto& annotation : annotations) {
			auto fixture = FixtureFactory::Instance().Create(annotation.fixtureName);
			if (fixture) {
				fixture->Apply(builder, annotation.registerSlot);
			}
		}
	}

	// ============================================================================
	// ENHANCED TEST RUNNER (with fixture support)
	// ============================================================================

	struct FixtureTestDesc
	{
		std::filesystem::path ShaderPath;
		std::string TestName;
		std::array<uint32_t, 3> ThreadGroupCount = { 1, 1, 1 };
		std::vector<BindingAnnotation> Fixtures;  // Optional: can be auto-parsed from HLSL
	};

	/// Run a test with automatic fixture binding
	inline bool RunTestWithFixtures(const FixtureTestDesc& desc, std::string& errorMsg)
	{
		try {
			stf::ShaderTestFixture fixture(GetFixtureDesc());
			auto shaderDir = (GetExecutableDirectory() / "Shaders").wstring();

			// Build descriptor table with fixtures
			stf::DescriptorTableBuilder resourceBuilder;
			ApplyFixtures(desc.Fixtures, resourceBuilder);

			auto result = fixture.RunTest(stf::ShaderTestFixture::RuntimeTestDesc{
				.CompilationEnv{ .Source = desc.ShaderPath,
					.CompilationFlags = { L"-I", shaderDir } },
				.TestName = desc.TestName,
				.ThreadGroupCount{ desc.ThreadGroupCount[0], desc.ThreadGroupCount[1], desc.ThreadGroupCount[2] },
				.Resources = resourceBuilder.Build() });

			if (!result) {
				std::ostringstream oss;
				oss << result;
				errorMsg = oss.str();
				std::cout << "\n"
						  << errorMsg << "\n";
				return false;
			}
			return true;
		} catch (const std::exception& e) {
			errorMsg = e.what();
			std::cout << "\nException: " << errorMsg << "\n";
			return false;
		}
	}
}
