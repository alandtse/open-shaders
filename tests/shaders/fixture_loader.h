// YAML Fixture Loader for HLSL Tests
// Loads fixture definitions from YAML and creates C++ fixture instances
#pragma once

#include "test_common.h"
#include "test_fixtures.h"
#include <Framework/HLSLFramework.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ShaderTest
{
	// ============================================================================
	// SIMPLE YAML PARSER (No external dependencies)
	// ============================================================================
	// We use a very simple YAML parser to avoid adding yaml-cpp dependency
	// This handles the minimal subset we need for fixtures

	struct YAMLFixtureData
	{
		std::string name;
		std::string bufferName;
		std::string format;
		uint32_t stride = 0;
		std::vector<float> data;
		uint32_t zeroInitSize = 0;
		std::string dataFile;  // Path to external binary file
	};

	inline std::vector<YAMLFixtureData> ParseYAMLFixtures(const std::filesystem::path& yamlPath)
	{
		std::vector<YAMLFixtureData> fixtures;
		std::ifstream file(yamlPath);
		if (!file.is_open()) {
			return fixtures;
		}

		YAMLFixtureData currentFixture;
		bool inDocument = false;
		bool inBuffers = false;
		bool inData = false;

		std::string line;
		while (std::getline(file, line)) {
			// Trim whitespace
			line.erase(0, line.find_first_not_of(" \t\r\n"));
			line.erase(line.find_last_not_of(" \t\r\n") + 1);

			// Document separator
			if (line == "---") {
				if (inDocument && !currentFixture.name.empty()) {
					fixtures.push_back(currentFixture);
				}
				currentFixture = YAMLFixtureData{};
				inDocument = true;
				inBuffers = false;
				inData = false;
				continue;
			}

			if (line == "...") {
				if (!currentFixture.name.empty()) {
					fixtures.push_back(currentFixture);
				}
				currentFixture = YAMLFixtureData{};
				inDocument = false;
				inBuffers = false;
				inData = false;
				continue;
			}

			// Skip comments and empty lines
			if (line.empty() || line[0] == '#') {
				continue;
			}

			// Parse key-value pairs
			size_t colonPos = line.find(':');
			if (colonPos != std::string::npos) {
				std::string key = line.substr(0, colonPos);
				std::string value = line.substr(colonPos + 1);

				// Trim key and value
				key.erase(0, key.find_first_not_of(" \t"));
				key.erase(key.find_last_not_of(" \t") + 1);
				value.erase(0, value.find_first_not_of(" \t"));
				value.erase(value.find_last_not_of(" \t") + 1);

				if (key == "Name") {
					currentFixture.name = value;
				} else if (key == "Buffers") {
					inBuffers = true;
				} else if (inBuffers) {
					if (key == "- Name") {
						value.erase(0, value.find_first_not_of(" \t"));
						currentFixture.bufferName = value;
					} else if (key == "Format") {
						currentFixture.format = value;
					} else if (key == "Stride") {
						currentFixture.stride = static_cast<uint32_t>(std::stoi(value));
					} else if (key == "ZeroInitSize") {
						currentFixture.zeroInitSize = static_cast<uint32_t>(std::stoi(value));
					} else if (key == "DataFile") {
						currentFixture.dataFile = value;
					} else if (key == "Data") {
						inData = true;
						// Check if data is inline on same line
						if (value.find('[') != std::string::npos) {
							// Parse inline array
							size_t startBracket = value.find('[');
							size_t endBracket = value.find(']');
							if (endBracket != std::string::npos) {
								std::string dataStr = value.substr(startBracket + 1, endBracket - startBracket - 1);
								std::istringstream ss(dataStr);
								std::string token;
								while (std::getline(ss, token, ',')) {
									token.erase(0, token.find_first_not_of(" \t"));
									token.erase(token.find_last_not_of(" \t") + 1);
									if (!token.empty() && token != "#") {  // Skip comments
										currentFixture.data.push_back(std::stof(token));
									}
								}
								inData = false;
							}
						}
					}
				}
			} else if (inData) {
				// Multi-line data array
				if (line.find('[') != std::string::npos || line.find(']') != std::string::npos) {
					// Remove brackets
					line.erase(std::remove(line.begin(), line.end(), '['), line.end());
					line.erase(std::remove(line.begin(), line.end(), ']'), line.end());
				}

				// Parse comma-separated values
				std::istringstream ss(line);
				std::string token;
				while (std::getline(ss, token, ',')) {
					token.erase(0, token.find_first_not_of(" \t"));
					token.erase(token.find_last_not_of(" \t") + 1);

					// Stop at comments
					size_t commentPos = token.find('#');
					if (commentPos != std::string::npos) {
						token = token.substr(0, commentPos);
						token.erase(token.find_last_not_of(" \t") + 1);
					}

					if (!token.empty()) {
						try {
							currentFixture.data.push_back(std::stof(token));
						} catch (...) {
							// Skip invalid numbers
						}
					}
				}
			}
		}

		// Add last fixture if any
		if (!currentFixture.name.empty()) {
			fixtures.push_back(currentFixture);
		}

		return fixtures;
	}

	// ============================================================================
	// FIXTURE FACTORY FROM YAML
	// ============================================================================

	// Load binary data from file
	inline std::vector<uint8_t> LoadBinaryFile(const std::filesystem::path& filePath)
	{
		std::ifstream file(filePath, std::ios::binary | std::ios::ate);
		if (!file.is_open()) {
			throw std::runtime_error("Failed to open binary file: " + filePath.string());
		}

		std::streamsize size = file.tellg();
		file.seekg(0, std::ios::beg);

		std::vector<uint8_t> buffer(size);
		if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
			throw std::runtime_error("Failed to read binary file: " + filePath.string());
		}

		return buffer;
	}

	inline std::unique_ptr<BindingFixture> CreateFixtureFromYAML(const YAMLFixtureData& yamlData)
	{
		// Handle external binary file
		if (!yamlData.dataFile.empty()) {
			class YAMLBinaryFileFixture : public BindingFixture
			{
				std::string m_name;
				std::string m_filePath;

			public:
				YAMLBinaryFileFixture(const std::string& name, const std::string& filePath) :
					m_name(name), m_filePath(filePath) {}

				const char* GetName() const override { return m_name.c_str(); }

				void Apply(stf::DescriptorTableBuilder& builder, uint32_t bindingSlot) override
				{
					auto data = LoadBinaryFile(m_filePath);
					builder.AddSRV(bindingSlot, stf::Buffer(data.data(), data.size()));
				}
			};

			return std::make_unique<YAMLBinaryFileFixture>(yamlData.name, yamlData.dataFile);
		}

		// Handle inline data
		if (!yamlData.data.empty()) {
			// Create a custom fixture with the YAML data
			class YAMLDataFixture : public BindingFixture
			{
				std::string m_name;
				std::vector<float> m_data;
				uint32_t m_stride;

			public:
				YAMLDataFixture(const std::string& name, const std::vector<float>& data, uint32_t stride) :
					m_name(name), m_data(data), m_stride(stride) {}

				const char* GetName() const override { return m_name.c_str(); }

				void Apply(stf::DescriptorTableBuilder& builder, uint32_t bindingSlot) override
				{
					// Determine if this is a texture or buffer based on stride
					// For simplicity, treat as SRV buffer for now
					builder.AddSRV(bindingSlot, stf::Buffer(m_data.data(), m_data.size() * sizeof(float)));
				}
			};

			return std::make_unique<YAMLDataFixture>(yamlData.name, yamlData.data, yamlData.stride);
		}

		// Zero-initialized buffer
		if (yamlData.zeroInitSize > 0) {
			class YAMLZeroFixture : public BindingFixture
			{
				std::string m_name;
				uint32_t m_size;

			public:
				YAMLZeroFixture(const std::string& name, uint32_t size) :
					m_name(name), m_size(size) {}

				const char* GetName() const override { return m_name.c_str(); }

				void Apply(stf::DescriptorTableBuilder& builder, uint32_t bindingSlot) override
				{
					std::vector<uint8_t> zeroData(m_size, 0);
					builder.AddSRV(bindingSlot, stf::Buffer(zeroData.data(), m_size));
				}
			};

			return std::make_unique<YAMLZeroFixture>(yamlData.name, yamlData.zeroInitSize);
		}

		return nullptr;
	}

	// ============================================================================
	// FIXTURE REGISTRY WITH YAML SUPPORT
	// ============================================================================

	class FixtureRegistry
	{
		std::unordered_map<std::string, std::unique_ptr<BindingFixture>> m_fixtures;

	public:
		static FixtureRegistry& Instance()
		{
			static FixtureRegistry instance;
			return instance;
		}

		void LoadFromYAML(const std::filesystem::path& yamlPath)
		{
			auto yamlFixtures = ParseYAMLFixtures(yamlPath);
			for (const auto& yamlData : yamlFixtures) {
				auto fixture = CreateFixtureFromYAML(yamlData);
				if (fixture) {
					m_fixtures[yamlData.name] = std::move(fixture);
				}
			}
		}

		void RegisterBuiltIn()
		{
			// Register built-in C++ fixtures
			m_fixtures["GradientTexture8x8"] = std::make_unique<GradientTexture2D>();
			m_fixtures["ConstantTexture8x8"] = std::make_unique<ConstantTexture2D>();
			m_fixtures["OutputTexture8x8"] = std::make_unique<OutputTexture2D>();
			m_fixtures["CommonCBuffer"] = std::make_unique<CommonCBuffer>();
			m_fixtures["LinearClampSampler"] = std::make_unique<LinearClampSampler>();
			m_fixtures["PointClampSampler"] = std::make_unique<PointClampSampler>();
		}

		BindingFixture* Get(const std::string& name)
		{
			auto it = m_fixtures.find(name);
			return (it != m_fixtures.end()) ? it->second.get() : nullptr;
		}
	};
}
