#include "Effect.h"
#include <d3dcompiler.h>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>

#include <DirectXTex.h>

#include "../EffectManager.h"
#include "../EffectSourceCompatibility.h"
#include "../PresetInclude.h"
#include "../PresetManager.h"
#include "../TextureManager.h"
#include "Features/Effects11/SettingsPatches.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Utils/D3D.h"

namespace
{
	void Trim(std::string& a_value, const char* a_characters = " \t\r")
	{
		const auto first = a_value.find_first_not_of(a_characters);
		if (first == std::string::npos) {
			a_value.clear();
			return;
		}
		const auto last = a_value.find_last_not_of(a_characters);
		a_value = a_value.substr(first, last - first + 1);
	}

	bool IsTruthy(const std::string& a_value)
	{
		return !a_value.empty() && a_value != "0" && a_value != "false";
	}

	int ParseInt(const std::string& a_value, int a_fallback = 0)
	{
		try {
			return std::stoi(a_value);
		} catch (...) {
			if (!a_value.empty())
				logger::warn("[EFFECTS11] Failed to parse integer from '{}'", a_value);
			return a_fallback;
		}
	}

	float ParseFloat(const std::string& a_value, float a_fallback = 0.0f)
	{
		try {
			return std::stof(a_value);
		} catch (...) {
			if (!a_value.empty())
				logger::warn("[EFFECTS11] Failed to parse float from '{}'", a_value);
			return a_fallback;
		}
	}

	Effect::UIWidgetType ParseWidgetType(const std::string& a_widget)
	{
		std::string widget = a_widget;
		std::transform(widget.begin(), widget.end(), widget.begin(), ::tolower);
		if (widget == "dropdown")
			return Effect::UIWidgetType::Dropdown;
		if (widget == "vector")
			return Effect::UIWidgetType::Vector;
		if (widget == "quality")
			return Effect::UIWidgetType::Quality;
		if (widget == "color")
			return Effect::UIWidgetType::Color;
		return Effect::UIWidgetType::Default;
	}

	std::vector<std::string> ParseDropdownList(const std::string& a_list)
	{
		std::vector<std::string> items;
		std::istringstream stream(a_list);
		std::string item;
		while (std::getline(stream, item, ',')) {
			Trim(item);
			items.push_back(item);
		}
		return items;
	}

	bool CreateUIVariable(Effect::UIVariable& a_output, ID3DX11EffectVariable* a_variable,
		const D3DX11_EFFECT_VARIABLE_DESC& a_variableDescription, const D3DX11_EFFECT_TYPE_DESC& a_typeDescription,
		Effect& a_effect)
	{
		std::string uiName = a_effect.GetUIAnnotation(a_variable, "UIName");
		Trim(uiName);
		if (uiName.empty())
			return false;

		a_output = {};
		a_output.name = a_variableDescription.Name;
		a_output.displayName = std::move(uiName);
		a_output.effectVariable.copy_from(a_variable);

		if (a_typeDescription.Class == D3D_SVC_SCALAR) {
			switch (a_typeDescription.Type) {
			case D3D_SVT_FLOAT:
				a_output.type = Effect::UIVariableType::Float;
				break;
			case D3D_SVT_INT:
				a_output.type = Effect::UIVariableType::Int;
				break;
			case D3D_SVT_BOOL:
				a_output.type = Effect::UIVariableType::Bool;
				break;
			default:
				return false;
			}
		} else if (a_typeDescription.Class == D3D_SVC_VECTOR && a_typeDescription.Type == D3D_SVT_FLOAT && a_typeDescription.Elements == 0) {
			if (a_typeDescription.Columns == 2)
				a_output.type = Effect::UIVariableType::Float2;
			else if (a_typeDescription.Columns == 3)
				a_output.type = Effect::UIVariableType::Float3;
			else if (a_typeDescription.Columns == 4)
				a_output.type = Effect::UIVariableType::Float4;
			else
				return false;
		} else {
			return false;
		}

		a_output.widgetType = ParseWidgetType(a_effect.GetUIAnnotation(a_variable, "UIWidget"));
		if (a_output.type == Effect::UIVariableType::Int) {
			const auto minimum = a_effect.GetUIAnnotation(a_variable, "UIMin");
			const auto maximum = a_effect.GetUIAnnotation(a_variable, "UIMax");
			if (!minimum.empty())
				a_output.intMin = ParseInt(minimum, a_output.intMin);
			if (!maximum.empty())
				a_output.intMax = ParseInt(maximum, a_output.intMax);
			if (a_output.widgetType == Effect::UIWidgetType::Dropdown)
				a_output.dropdownItems = ParseDropdownList(a_effect.GetUIAnnotation(a_variable, "UIList"));
			else if (a_output.widgetType == Effect::UIWidgetType::Quality) {
				a_output.dropdownItems = {
					T("feature.effects11.quality_very_high", "Very High"),
					T("feature.effects11.quality_high", "High"),
					T("feature.effects11.quality_medium", "Medium"),
					T("feature.effects11.quality_low", "Low"),
					T("feature.effects11.quality_very_low", "Very Low")
				};
				a_output.intMin = -1;
				a_output.intMax = 3;
			}
		} else if (a_output.type != Effect::UIVariableType::Bool) {
			const auto minimum = a_effect.GetUIAnnotation(a_variable, "UIMin");
			const auto maximum = a_effect.GetUIAnnotation(a_variable, "UIMax");
			if (!minimum.empty())
				a_output.floatMin = ParseFloat(minimum, a_output.floatMin);
			if (!maximum.empty())
				a_output.floatMax = ParseFloat(maximum, a_output.floatMax);
		}

		a_output.isHidden = IsTruthy(a_effect.GetUIAnnotation(a_variable, "UIHidden"));
		a_output.isLabel =
			(a_output.type == Effect::UIVariableType::Float && a_output.floatMin == 0.0f && a_output.floatMax == 0.0f) ||
			(a_output.type == Effect::UIVariableType::Int && a_output.intMin == 0 && a_output.intMax == 0);
		a_output.isReadOnly =
			(a_output.type == Effect::UIVariableType::Int && a_output.intMin == a_output.intMax) ||
			(a_output.type != Effect::UIVariableType::Int && a_output.type != Effect::UIVariableType::Bool && a_output.floatMin == a_output.floatMax);
		return true;
	}
}

std::filesystem::path Effect::GetFilePath() const
{
	return PresetManager::GetSingleton().GetENBSeriesPath() / GetName();
}

bool Effect::Load()
{
	std::filesystem::path iniPath = PresetManager::GetSingleton().GetENBSeriesPath() / (GetName() + ".ini");

	if (!std::filesystem::exists(iniPath)) {
		logger::info("[EFFECTS11] Could not find ini file '{}' for effect '{}', using defaults", iniPath.string(), GetName());
		return true;
	}

	std::string section = GetName();
	std::transform(section.begin(), section.end(), section.begin(), ::toupper);

	for (auto& uiVar : uiVariables) {
		if (uiVar.isLabel)
			continue;
		if (!uiVar.effectVariable)
			continue;
		std::string iniKey = GetVariableIniKey(uiVar);
		if (iniKey.empty())
			continue;

		bool isPerComponent = IsPerComponentVector(uiVar);
		if (isPerComponent) {
			static const char* suffixes[] = { "X", "Y", "Z", "W" };
			int numComponents = (uiVar.type == UIVariableType::Float2) ? 2 : (uiVar.type == UIVariableType::Float3) ? 3 :
			                                                                                                          4;
			for (int i = 0; i < numComponents; ++i) {
				std::string compKey = iniKey + suffixes[i];
				std::vector<char> valueBuffer(1024);
				DWORD result = GetPrivateProfileStringA(section.c_str(), compKey.c_str(), "", valueBuffer.data(), 1024, iniPath.string().c_str());
				if (result > 0) {
					try {
						uiVar.vectorValue[i] = std::stof(std::string(valueBuffer.data()));
					} catch (...) {}
				}
			}
			if (uiVar.effectVariable)
				uiVar.effectVariable->AsVector()->SetFloatVector(uiVar.vectorValue);
		} else {
			std::vector<char> valueBuffer(1024);
			DWORD result = GetPrivateProfileStringA(section.c_str(), iniKey.c_str(), "", valueBuffer.data(), 1024, iniPath.string().c_str());
			if (result > 0) {
				std::string value(valueBuffer.data());
				LoadVariableFromString(uiVar, value);
			}
		}
	}

	if (!uiTechniques.empty()) {
		uint32_t techniqueFromIni = static_cast<uint32_t>(GetPrivateProfileIntA(section.c_str(), "TECHNIQUE", selectedTechniqueIndex + 1, iniPath.string().c_str()));
		if (techniqueFromIni > 0) {
			uint32_t maxIndex = static_cast<uint32_t>(uiTechniques.size() - 1);
			selectedTechniqueIndex = (techniqueFromIni - 1 < maxIndex) ? (techniqueFromIni - 1) : maxIndex;
		} else {
			selectedTechniqueIndex = 0;
		}
	}

	Util::SettingsPatches::Apply(*this);

	logger::debug("[EFFECTS11] Loaded settings from '{}' for effect '{}'", iniPath.string(), GetName());
	return true;
}

void Effect::Save()
{
	// Nothing loaded means nothing to persist; writing would leave stub ini files in the preset
	if (!IsCompiled())
		return;

	std::filesystem::path iniPath = PresetManager::GetSingleton().GetENBSeriesPath() / (GetName() + ".ini");

	std::string section = GetName();
	std::transform(section.begin(), section.end(), section.begin(), ::toupper);

	for (const auto& uiVar : uiVariables) {
		if (uiVar.isLabel)
			continue;
		if (!uiVar.effectVariable)
			continue;
		std::string iniKey = GetVariableIniKey(uiVar);
		if (iniKey.empty())
			continue;

		std::string value;

		switch (uiVar.type) {
		case UIVariableType::Float:
			value = std::to_string(uiVar.floatValue);
			break;
		case UIVariableType::Int:
			value = std::to_string(uiVar.intValue);
			break;
		case UIVariableType::Bool:
			value = uiVar.boolValue ? "true" : "false";
			break;
		case UIVariableType::Float2:
		case UIVariableType::Float3:
		case UIVariableType::Float4:
			if (IsPerComponentVector(uiVar)) {
				static const char* suffixes[] = { "X", "Y", "Z", "W" };
				int numComponents = (uiVar.type == UIVariableType::Float2) ? 2 : (uiVar.type == UIVariableType::Float3) ? 3 :
				                                                                                                          4;
				for (int i = 0; i < numComponents; ++i) {
					std::string compKey = iniKey + suffixes[i];
					std::string compValue = std::to_string(uiVar.vectorValue[i]);
					BOOL compResult = WritePrivateProfileStringA(section.c_str(), compKey.c_str(), compValue.c_str(), iniPath.string().c_str());
					if (!compResult)
						logger::warn("[EFFECTS11] Failed to write key '{}' to ini file '{}'", compKey, iniPath.string());
				}
				continue;
			} else {
				std::ostringstream oss;
				int numComponents = (uiVar.type == UIVariableType::Float2) ? 2 : (uiVar.type == UIVariableType::Float3) ? 3 :
				                                                                                                          4;

				std::copy(uiVar.vectorValue, uiVar.vectorValue + numComponents - 1,
					std::ostream_iterator<float>(oss, ", "));
				oss << uiVar.vectorValue[numComponents - 1];

				value = oss.str();
			}
			break;
		}

		BOOL result = WritePrivateProfileStringA(section.c_str(), iniKey.c_str(), value.c_str(), iniPath.string().c_str());
		if (!result) {
			logger::warn("[EFFECTS11] Failed to write key '{}' to ini file '{}'", iniKey, iniPath.string());
		}
	}

	std::string techniqueValue = std::to_string(selectedTechniqueIndex + 1u);
	BOOL techniqueResult = WritePrivateProfileStringA(section.c_str(), "TECHNIQUE", techniqueValue.c_str(), iniPath.string().c_str());
	if (!techniqueResult) {
		logger::warn("[EFFECTS11] Failed to write TECHNIQUE key to ini file '{}'", iniPath.string());
	}

	WritePrivateProfileStringA(NULL, NULL, NULL, iniPath.string().c_str());

	logger::info("[EFFECTS11] Saved settings to '{}' for effect '{}'", iniPath.string(), GetName());
}

bool Effect::Apply()
{
	logger::info("[EFFECTS11] Applying effect '{}'", GetName());

	Unload();

	if (!LoadFXFile()) {
		if (!filePresent) {
			// A missing optional effect is normal, a missing required one means there is no preset
			if (IsRequired()) {
				logger::error("[EFFECTS11] Required effect file not found: '{}'", GetFilePath().string());
				return false;
			}
			logger::info("[EFFECTS11] Effect file not found, skipping: '{}'", GetFilePath().string());
			return true;
		}
		// The file exists but could not be read or compiled; errors holds the specific reason
		logger::error("[EFFECTS11] Failed to load '{}': {}", GetFilePath().string(), errors.empty() ? "unknown error" : errors.back());
		return false;
	}

	if (!Load()) {
		errors.push_back("Failed to load settings");
		logger::error("[EFFECTS11] Failed to load settings for effect '{}'", GetName());
		return false;
	}

	CreateEffectTextures();

	logger::info("[EFFECTS11] Successfully applied effect '{}'", GetName());
	return true;
}

void Effect::Unload()
{
	effect = nullptr;

	techniques.clear();
	variables.clear();
	customTextureCache.clear();
	uiVariables.clear();
	effectTextureCache.clear();
	uiTechniques.clear();
	selectedTechniqueIndex = 0;

	ClearVariableCache();

	filePresent = false;
	errors.clear();

	logger::info("[EFFECTS11] Unloaded effect '{}'", GetName());
}

bool Effect::LoadFXFile()
{
	auto filePath = GetFilePath();

	if (!std::filesystem::exists(filePath)) {
		filePresent = false;
		return false;
	}
	filePresent = true;

	auto filePathStr = filePath.string();
	std::string patchedSource;
	if (GetName() == "enbeffect.fx") {
		std::ifstream sourceFile(filePath, std::ios::binary);
		if (sourceFile) {
			patchedSource.assign(std::istreambuf_iterator<char>(sourceFile), std::istreambuf_iterator<char>());
			if (EffectSourceCompatibility::PatchInteriorTimeOfDayMacro(patchedSource))
				logger::debug("[EFFECTS11] Applied interior time-of-day compatibility patch to '{}'", filePathStr);
			else
				patchedSource.clear();
		}
	}

	std::optional<EffectSourceCompatibility::PresetInclude> includes;
	try {
		includes.emplace(filePath.parent_path());
	} catch (const std::filesystem::filesystem_error& error) {
		errors.emplace_back(error.what());
		logger::error("[EFFECTS11] Failed to resolve include directory for '{}': {}", filePathStr, error.what());
		return false;
	}
	winrt::com_ptr<ID3DBlob> compileMessages;
	HRESULT hr;
	if (!patchedSource.empty()) {
		hr = D3DX11CompileEffectFromMemory(
			patchedSource.data(),
			patchedSource.size(),
			filePathStr.c_str(),
			nullptr,
			&*includes,
			0,
			0,
			globals::d3d::device,
			effect.put(),
			compileMessages.put());
	} else {
		hr = D3DX11CompileEffectFromFile(
			filePath.c_str(),
			nullptr,
			&*includes,
			0,
			0,
			globals::d3d::device,
			effect.put(),
			compileMessages.put());
	}
	for (const auto& missing : includes->GetMissingIncludes())
		logger::warn("[EFFECTS11] Missing include '{}' in '{}'; treated as empty source for preset compatibility", missing, filePathStr);
	if (FAILED(hr)) {
		std::string errorMessage = "Compilation failed";
		if (compileMessages) {
			errorMessage.clear();
			std::istringstream stream(std::string(
				static_cast<const char*>(compileMessages->GetBufferPointer()), compileMessages->GetBufferSize()));
			std::string line;
			while (std::getline(stream, line)) {
				if (!line.empty() && line.find("warning X4717") == std::string::npos)
					errorMessage += line + '\n';
			}
			if (errorMessage.empty())
				errorMessage = "Compilation failed";
		}
		errors.push_back(errorMessage);
		logger::error("[EFFECTS11] Effect compilation failed for '{}': {}", filePathStr, errorMessage);
		return false;
	}
	Util::LogShaderCompileWarnings(compileMessages.get(), filePathStr);

	EnumerateAllVariables();
	SetupCustomTextures();
	LoadTechniques();
	LoadUITechniques();

	LoadUIVariables();

	logger::info("[EFFECTS11] Successfully loaded FX file: {}", filePathStr);
	return true;
}

Effect::TechniqueSequenceResult Effect::ExecuteTechniqueSequence(const std::string& a_baseTechniqueName, ID3D11ShaderResourceView* a_input, TextureManager::Texture& a_output, TextureManager::Texture& a_temp)
{
	if (!IsCompiled() || !effect)
		return {};

	if (a_baseTechniqueName.empty())
		return {};

	auto sequenceIt = techniques.find(a_baseTechniqueName);
	if (sequenceIt == techniques.end())
		return {};

	auto& sequence = sequenceIt->second;
	if (sequence.empty())
		return {};

	auto* cachedVar = GetCachedVariable("TextureColor");
	auto sourceTexture = cachedVar ? cachedVar->AsShaderResource() : nullptr;

	uint32_t swapCounter = 0;
	uint32_t passOffset = 0;
	bool targetInOutput = false;

	ID3D11ShaderResourceView* inputSRV = nullptr;
	ID3D11RenderTargetView* outputRTV = nullptr;

	for (size_t i = 0; i < sequence.size(); ++i) {
		auto& techniqueInfo = sequence[i];

		if (!techniqueInfo.technique)
			continue;

		if (sequence.size() == 1 || swapCounter == 0) {
			inputSRV = a_input;
			outputRTV = a_output.rtv.get();
		} else {
			// GetEyeCroppedSRV: a prior technique in this sequence wrote a_output/a_temp
			// under a per-eye viewport crop, so this read-back needs the same crop.
			bool useTemp = (swapCounter & 1) == 0;
			if (useTemp) {
				inputSRV = EffectManager::GetSingleton().GetEyeCroppedSRV(a_temp);
				outputRTV = a_output.rtv.get();
			} else {
				inputSRV = EffectManager::GetSingleton().GetEyeCroppedSRV(a_output);
				outputRTV = a_temp.rtv.get();
			}
		}

		if (!techniqueInfo.renderTargetName.empty()) {
			outputRTV = GetRenderTargetView(techniqueInfo.renderTargetName, outputRTV);
		} else {
			swapCounter++;
		}

		targetInOutput = (outputRTV == a_output.rtv.get());

		if (sourceTexture && sourceTexture->IsValid())
			sourceTexture->AsShaderResource()->SetResource(inputSRV);

		RenderPasses(techniqueInfo.technique.get(), outputRTV, passOffset);
		passOffset += techniqueInfo.passCount;
	}

	return { true, targetInOutput };
}

void Effect::ExecuteTechnique(const std::string& techniqueName, TextureManager::Texture& output)
{
	if (!IsCompiled() || !effect)
		return;

	auto technique = effect->GetTechniqueByName(techniqueName.c_str());
	if (!technique || !technique->IsValid())
		return;

	RenderPasses(technique, output.rtv.get());
}

void Effect::SetupCustomTextures()
{
	for (auto& [varName, effectVar] : variables) {
		std::string resourceName = GetUIAnnotation(effectVar.get(), "ResourceName");
		if (resourceName.empty())
			continue;

		auto srv = LoadTextureFromFile(resourceName);
		if (srv) {
			auto shaderResourceVar = effectVar->AsShaderResource();
			if (shaderResourceVar && shaderResourceVar->IsValid())
				shaderResourceVar->SetResource(srv);
		}
	}
}

ID3D11ShaderResourceView* Effect::LoadTextureFromFile(const std::string& filename)
{
	auto device = globals::d3d::device;

	auto cacheIt = customTextureCache.find(filename);
	if (cacheIt != customTextureCache.end())
		return cacheIt->second.get();

	std::filesystem::path filepath = PresetManager::GetSingleton().GetENBSeriesPath() / filename;

	winrt::com_ptr<ID3D11ShaderResourceView> srv;

	DirectX::ScratchImage image;
	HRESULT hr = DirectX::LoadFromDDSFile(filepath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image);
	if (FAILED(hr))
		hr = DirectX::LoadFromWICFile(filepath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
	if (SUCCEEDED(hr))
		hr = DirectX::CreateShaderResourceView(device, image.GetImages(), image.GetImageCount(), image.GetMetadata(), srv.put());

	if (FAILED(hr)) {
		logger::error("[EFFECTS11] Failed to load texture file: {} (HRESULT: 0x{:08X})", filepath.string(), static_cast<uint32_t>(hr));
		return nullptr;
	}

	customTextureCache[filename] = srv;
	return srv.get();
}

void Effect::LoadTechniques()
{
	D3DX11_EFFECT_DESC effectDesc;
	if (FAILED(effect->GetDesc(&effectDesc)))
		return;

	for (UINT g = 0; g < effectDesc.Groups; ++g) {
		auto group = effect->GetGroupByIndex(g);
		if (!group || !group->IsValid())
			continue;

		D3DX11_GROUP_DESC groupDesc;
		if (FAILED(group->GetDesc(&groupDesc)))
			continue;

		bool isNamedGroup = groupDesc.Name && groupDesc.Name[0];

		for (UINT t = 0; t < groupDesc.Techniques; ++t) {
			auto technique = group->GetTechniqueByIndex(t);
			if (!technique || !technique->IsValid())
				continue;

			D3DX11_TECHNIQUE_DESC techDesc;
			if (FAILED(technique->GetDesc(&techDesc)))
				continue;

			std::string key;
			if (isNamedGroup) {
				key = std::string(groupDesc.Name);
			} else {
				std::string techName = techDesc.Name ? std::string(techDesc.Name) : ("technique" + std::to_string(t));

				// ENB convention: numbered follow-up techniques (Name1, Name2, ...) belong to the base technique (Name)
				std::string baseName = techName;
				while (!baseName.empty() && std::isdigit(static_cast<unsigned char>(baseName.back())))
					baseName.pop_back();

				if (!baseName.empty() && baseName != techName && techniques.contains(baseName))
					key = baseName;
				else
					key = techName;
			}

			TechniqueInfo info;
			info.technique.copy_from(technique);
			info.renderTargetName = GetTechniqueAnnotation(technique, "RenderTarget");
			info.passCount = techDesc.Passes;

			techniques[key].push_back(std::move(info));
		}
	}
}

void Effect::LoadUITechniques()
{
	uiTechniques.clear();
	selectedTechniqueIndex = 0;

	D3DX11_EFFECT_DESC effectDesc;
	if (FAILED(effect->GetDesc(&effectDesc)))
		return;

	for (UINT g = 0; g < effectDesc.Groups; ++g) {
		auto group = effect->GetGroupByIndex(g);
		if (!group || !group->IsValid())
			continue;

		D3DX11_GROUP_DESC groupDesc;
		if (FAILED(group->GetDesc(&groupDesc)))
			continue;

		const bool isNamedGroup = groupDesc.Name && groupDesc.Name[0];
		if (isNamedGroup) {
			std::string uiName = GetGroupAnnotation(group, "UIName");
			if (!uiName.empty())
				uiTechniques.push_back({ std::string(groupDesc.Name), uiName });
			continue;
		}

		for (UINT t = 0; t < groupDesc.Techniques; ++t) {
			auto technique = group->GetTechniqueByIndex(t);
			if (!technique || !technique->IsValid())
				continue;

			D3DX11_TECHNIQUE_DESC techDesc;
			if (FAILED(technique->GetDesc(&techDesc)))
				continue;

			std::string uiName = GetTechniqueAnnotation(technique, "UIName");
			if (uiName.empty())
				continue;

			std::string sequenceName = techDesc.Name ? std::string(techDesc.Name) : "";
			uiTechniques.push_back({ sequenceName, uiName });
		}
	}
}

ID3D11RenderTargetView* Effect::GetRenderTargetView(const std::string& renderTargetName, ID3D11RenderTargetView* fallback)
{
	if (renderTargetName.empty())
		return fallback;

	auto it = effectTextureCache.find(renderTargetName);
	if (it != effectTextureCache.end() && it->second.rtv)
		return it->second.rtv.get();

	auto* texture = GetCachedCommonTexture(renderTargetName);
	if (texture && texture->rtv)
		return texture->rtv.get();

	return fallback;
}

void Effect::LoadUIVariables()
{
	D3DX11_EFFECT_DESC effectDesc;
	if (FAILED(effect->GetDesc(&effectDesc)))
		return;

	uiVariables.clear();

	for (UINT i = 0; i < effectDesc.GlobalVariables; ++i) {
		auto variable = effect->GetVariableByIndex(i);
		if (!variable || !variable->IsValid())
			continue;

		D3DX11_EFFECT_VARIABLE_DESC varDesc;
		if (FAILED(variable->GetDesc(&varDesc)))
			continue;

		D3DX11_EFFECT_TYPE_DESC typeDesc;
		auto effectType = variable->GetType();
		if (FAILED(effectType->GetDesc(&typeDesc)))
			continue;

		if (typeDesc.Class == D3D_SVC_OBJECT && typeDesc.Type == D3D_SVT_STRING)
			continue;

		UIVariable uiVar = {};
		if (CreateUIVariable(uiVar, variable, varDesc, typeDesc, *this)) {
			LoadUIVariableValue(uiVar);
			uiVariables.push_back(std::move(uiVar));
		}
	}

	logger::info("[EFFECTS11] Loaded {} UI variables for effect '{}'", uiVariables.size(), GetName());
}

static std::string ReadAnnotationValue(ID3DX11EffectVariable* annotation)
{
	if (!annotation || !annotation->IsValid())
		return "";

	auto stringVar = annotation->AsString();
	if (stringVar && stringVar->IsValid()) {
		LPCSTR value = nullptr;
		if (SUCCEEDED(stringVar->GetString(&value)) && value)
			return std::string(value);
	}

	auto scalarVar = annotation->AsScalar();
	if (scalarVar && scalarVar->IsValid()) {
		auto annType = annotation->GetType();
		D3DX11_EFFECT_TYPE_DESC typeDesc;
		if (annType && SUCCEEDED(annType->GetDesc(&typeDesc))) {
			switch (typeDesc.Type) {
			case D3D_SVT_INT:
				{
					int v;
					if (SUCCEEDED(scalarVar->GetInt(&v)))
						return std::to_string(v);
					break;
				}
			case D3D_SVT_FLOAT:
				{
					float v;
					if (SUCCEEDED(scalarVar->GetFloat(&v)))
						return std::to_string(v);
					break;
				}
			case D3D_SVT_BOOL:
				{
					bool v;
					if (SUCCEEDED(scalarVar->GetBool(&v)))
						return std::to_string(v ? 1 : 0);
					break;
				}
			default:
				break;
			}
		}
		int intValue;
		if (SUCCEEDED(scalarVar->GetInt(&intValue)))
			return std::to_string(intValue);
	}
	return "";
}

std::string Effect::GetUIAnnotation(ID3DX11EffectVariable* variable, const std::string& annotationName)
{
	if (!variable)
		return "";

	auto annotation = variable->GetAnnotationByName(annotationName.c_str());
	if (annotation && annotation->IsValid()) {
		auto result = ReadAnnotationValue(annotation);
		if (!result.empty())
			return result;
	}

	D3DX11_EFFECT_VARIABLE_DESC varDesc;
	if (FAILED(variable->GetDesc(&varDesc)))
		return "";
	for (UINT i = 0; i < varDesc.Annotations; ++i) {
		auto ann = variable->GetAnnotationByIndex(i);
		if (!ann || !ann->IsValid())
			continue;
		D3DX11_EFFECT_VARIABLE_DESC annDesc;
		if (FAILED(ann->GetDesc(&annDesc)))
			continue;
		if (_stricmp(annDesc.Name, annotationName.c_str()) == 0)
			return ReadAnnotationValue(ann);
	}
	return "";
}

std::string Effect::GetTechniqueAnnotation(ID3DX11EffectTechnique* technique, const std::string& annotationName)
{
	if (!technique)
		return "";
	auto annotation = technique->GetAnnotationByName(annotationName.c_str());
	return ReadAnnotationValue(annotation);
}

std::string Effect::GetGroupAnnotation(ID3DX11EffectGroup* group, const std::string& annotationName)
{
	if (!group)
		return "";
	auto annotation = group->GetAnnotationByName(annotationName.c_str());
	return ReadAnnotationValue(annotation);
}

bool Effect::IsPerComponentVector(const UIVariable& uiVar)
{
	return (uiVar.type == UIVariableType::Float2 || uiVar.type == UIVariableType::Float3 || uiVar.type == UIVariableType::Float4) &&
	       uiVar.widgetType != UIWidgetType::Color;
}

std::string Effect::GetVariableIniKey(const UIVariable& uiVar)
{
	return uiVar.displayName;
}

void Effect::LoadUIVariableValue(UIVariable& uiVar)
{
	switch (uiVar.type) {
	case UIVariableType::Float:
		uiVar.effectVariable->AsScalar()->GetFloat(&uiVar.floatValue);
		break;
	case UIVariableType::Int:
		uiVar.effectVariable->AsScalar()->GetInt(&uiVar.intValue);
		break;
	case UIVariableType::Bool:
		uiVar.effectVariable->AsScalar()->GetBool(&uiVar.boolValue);
		break;
	case UIVariableType::Float2:
	case UIVariableType::Float3:
	case UIVariableType::Float4:
		uiVar.effectVariable->AsVector()->GetFloatVector(uiVar.vectorValue);
		break;
	}
}

void Effect::LoadVariableFromString(UIVariable& uiVar, const std::string& value)
{
	try {
		switch (uiVar.type) {
		case UIVariableType::Float:
			uiVar.floatValue = std::stof(value);
			if (uiVar.effectVariable)
				uiVar.effectVariable->AsScalar()->SetFloat(uiVar.floatValue);
			break;
		case UIVariableType::Int:
			uiVar.intValue = std::stoi(value);
			if (uiVar.effectVariable)
				uiVar.effectVariable->AsScalar()->SetInt(uiVar.intValue);
			break;
		case UIVariableType::Bool:
			{
				std::string lowerValue = value;
				std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(), ::tolower);
				if (lowerValue == "true" || lowerValue == "1" || lowerValue == "yes" || lowerValue == "on")
					uiVar.boolValue = true;
				else if (lowerValue == "false" || lowerValue == "0" || lowerValue == "no" || lowerValue == "off")
					uiVar.boolValue = false;
				else
					uiVar.boolValue = std::stoi(value) != 0;
				if (uiVar.effectVariable)
					uiVar.effectVariable->AsScalar()->SetBool(uiVar.boolValue);
			}
			break;
		case UIVariableType::Float2:
		case UIVariableType::Float3:
		case UIVariableType::Float4:
			{
				std::istringstream ss(value);
				int numComponents = (uiVar.type == UIVariableType::Float2) ? 2 : (uiVar.type == UIVariableType::Float3) ? 3 :
				                                                                                                          4;
				for (int i = 0; i < numComponents; ++i) {
					char sep;
					ss >> uiVar.vectorValue[i];
					if (ss.peek() == ',')
						ss >> sep;
				}
				if (uiVar.effectVariable)
					uiVar.effectVariable->AsVector()->SetFloatVector(uiVar.vectorValue);
			}
			break;
		}
	} catch (const std::exception& e) {
		logger::warn("[EFFECTS11] Failed to parse value '{}' for variable '{}': {}", value, uiVar.name, e.what());
	}
}

void Effect::UpdateUIVariables()
{
	for (auto& uiVar : uiVariables) {
		if (!uiVar.effectVariable)
			continue;

		switch (uiVar.type) {
		case UIVariableType::Float:
			uiVar.effectVariable->AsScalar()->SetFloat(uiVar.floatValue);
			break;
		case UIVariableType::Int:
			uiVar.effectVariable->AsScalar()->SetInt(uiVar.intValue);
			break;
		case UIVariableType::Bool:
			uiVar.effectVariable->AsScalar()->SetBool(uiVar.boolValue);
			break;
		case UIVariableType::Float2:
		case UIVariableType::Float3:
		case UIVariableType::Float4:
			uiVar.effectVariable->AsVector()->SetFloatVector(uiVar.vectorValue);
			break;
		}
	}
}

void Effect::RenderImGui()
{
	bool valuesChanged = false;

	if (uiTechniques.size() > 1) {
		selectedTechniqueIndex = std::min(selectedTechniqueIndex, static_cast<uint32_t>(uiTechniques.size() - 1));
		ImGui::TextUnformatted(T("feature.effects11.technique", "Technique"));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.0f);
		const char* currentTechnique = uiTechniques[selectedTechniqueIndex].displayName.c_str();
		if (ImGui::BeginCombo(("##Technique_" + GetName()).c_str(), currentTechnique)) {
			for (uint32_t i = 0; i < uiTechniques.size(); ++i) {
				const bool selected = selectedTechniqueIndex == i;
				if (ImGui::Selectable(uiTechniques[i].displayName.c_str(), selected)) {
					selectedTechniqueIndex = i;
					valuesChanged = true;
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
	}

	if (ImGui::BeginTable(("##EffectVariables_" + GetName()).c_str(), 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn(T("feature.effects11.parameter", "Parameter"), ImGuiTableColumnFlags_WidthStretch, 0.45f);
		ImGui::TableSetupColumn(T("feature.effects11.value", "Value"), ImGuiTableColumnFlags_WidthStretch, 0.55f);

		for (size_t i = 0; i < uiVariables.size(); ++i) {
			auto& variable = uiVariables[i];
			if (variable.isHidden || variable.displayName.empty())
				continue;

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextWrapped("%s", variable.displayName.c_str());
			if (variable.isLabel)
				continue;

			ImGui::TableSetColumnIndex(1);
			ImGui::BeginDisabled(variable.isReadOnly);
			const std::string id = "##Variable_" + std::to_string(i) + "_" + GetName();
			bool changed = false;
			switch (variable.type) {
			case UIVariableType::Float:
				changed = ImGui::SliderFloat(id.c_str(), &variable.floatValue, variable.floatMin, variable.floatMax, "%.3f");
				break;
			case UIVariableType::Int:
				if ((variable.widgetType == UIWidgetType::Dropdown || variable.widgetType == UIWidgetType::Quality) && !variable.dropdownItems.empty()) {
					const int dropdownIndex = variable.widgetType == UIWidgetType::Quality ? variable.intValue + 1 : variable.intValue;
					const char* currentItem = dropdownIndex >= 0 && dropdownIndex < static_cast<int>(variable.dropdownItems.size()) ?
					                              variable.dropdownItems[dropdownIndex].c_str() :
					                              "";
					if (ImGui::BeginCombo(id.c_str(), currentItem)) {
						for (int item = 0; item < static_cast<int>(variable.dropdownItems.size()); ++item) {
							const int value = variable.widgetType == UIWidgetType::Quality ? item - 1 : item;
							if (ImGui::Selectable(variable.dropdownItems[item].c_str(), variable.intValue == value)) {
								variable.intValue = value;
								changed = true;
							}
						}
						ImGui::EndCombo();
					}
				} else {
					changed = ImGui::SliderInt(id.c_str(), &variable.intValue, variable.intMin, variable.intMax);
				}
				break;
			case UIVariableType::Bool:
				changed = ImGui::Checkbox(id.c_str(), &variable.boolValue);
				break;
			case UIVariableType::Float2:
				changed = ImGui::SliderFloat2(id.c_str(), variable.vectorValue, variable.floatMin, variable.floatMax, "%.3f");
				break;
			case UIVariableType::Float3:
				if (variable.widgetType == UIWidgetType::Color)
					changed = ImGui::ColorEdit3(id.c_str(), variable.vectorValue);
				else if (variable.widgetType == UIWidgetType::Vector)
					changed = ImGui::SliderFloat3(id.c_str(), variable.vectorValue, -1.0f, 1.0f, "%.3f");
				else
					changed = ImGui::SliderFloat3(id.c_str(), variable.vectorValue, variable.floatMin, variable.floatMax, "%.3f");
				break;
			case UIVariableType::Float4:
				if (variable.widgetType == UIWidgetType::Color)
					changed = ImGui::ColorEdit4(id.c_str(), variable.vectorValue);
				else
					changed = ImGui::SliderFloat4(id.c_str(), variable.vectorValue, variable.floatMin, variable.floatMax, "%.3f");
				break;
			}
			ImGui::EndDisabled();
			valuesChanged |= changed;
		}

		ImGui::EndTable();
	}

	if (valuesChanged)
		UpdateUIVariables();
}

void Effect::EnumerateAllVariables()
{
	D3DX11_EFFECT_DESC effectDesc;
	if (FAILED(effect->GetDesc(&effectDesc)))
		return;

	variables.clear();

	for (UINT i = 0; i < effectDesc.GlobalVariables; ++i) {
		auto variable = effect->GetVariableByIndex(i);
		if (!variable || !variable->IsValid())
			continue;

		D3DX11_EFFECT_VARIABLE_DESC varDesc;
		if (FAILED(variable->GetDesc(&varDesc)))
			continue;

		variables[varDesc.Name].copy_from(variable);
	}
}

ID3DX11EffectVariable* Effect::GetCachedVariable(const std::string& name)
{
	if (!effect)
		return nullptr;

	auto it = variableCache.find(name);
	if (it != variableCache.end())
		return it->second;

	auto variable = effect->GetVariableByName(name.c_str());
	variableCache[name] = variable;
	return variable;
}

TextureManager::Texture* Effect::GetCachedCommonTexture(const std::string& name)
{
	auto it = commonTexturePointerCache.find(name);
	if (it != commonTexturePointerCache.end())
		return it->second;

	auto* texture = TextureManager::GetSingleton().GetCommonTexture(name);
	commonTexturePointerCache[name] = texture;
	return texture;
}

void Effect::ClearVariableCache()
{
	variableCache.clear();
	commonTexturePointerCache.clear();
	rtvDimensionCache.clear();
}

bool Effect::SetShaderResourceVariable(const std::string& variableName, ID3D11ShaderResourceView* resource)
{
	auto variable = GetCachedVariable(variableName);
	if (variable) {
		auto srVar = variable->AsShaderResource();
		if (srVar && srVar->IsValid()) {
			srVar->SetResource(resource);
			return true;
		}
	}
	return false;
}

bool Effect::SetShaderResourceVariable(ID3DX11Effect* effect, const std::string& variableName, ID3D11ShaderResourceView* resource)
{
	if (!effect)
		return false;

	auto variable = effect->GetVariableByName(variableName.c_str())->AsShaderResource();
	if (variable && variable->IsValid()) {
		variable->SetResource(resource);
		return true;
	}
	return false;
}

bool Effect::SetVectorVariable(ID3DX11Effect* effect, const std::string& variableName, const void* data, uint32_t size)
{
	if (!effect)
		return false;

	auto variable = effect->GetVariableByName(variableName.c_str());
	if (variable && variable->IsValid()) {
		variable->SetRawValue(data, 0, size);
		return true;
	}
	return false;
}

bool Effect::SetVectorVariable(const std::string& variableName, const void* data, uint32_t size)
{
	auto variable = GetCachedVariable(variableName);
	if (variable && variable->IsValid()) {
		variable->SetRawValue(data, 0, size);
		return true;
	}
	return false;
}

TextureManager::Texture Effect::CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, const std::string& debugName)
{
	auto device = globals::d3d::device;

	D3D11_TEXTURE2D_DESC texDesc = {};
	texDesc.Width = width;
	texDesc.Height = height;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.Format = format;
	texDesc.SampleDesc.Count = 1;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	TextureManager::Texture texture{};
	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, texture.texture.put()));
	DX::ThrowIfFailed(device->CreateRenderTargetView(texture.texture.get(), nullptr, texture.rtv.put()));
	DX::ThrowIfFailed(device->CreateShaderResourceView(texture.texture.get(), nullptr, texture.srv.put()));

	if (!debugName.empty()) {
		Util::SetResourceName(texture.texture.get(), (debugName).c_str());
		Util::SetResourceName(texture.rtv.get(), (debugName + " RTV").c_str());
		Util::SetResourceName(texture.srv.get(), (debugName + " SRV").c_str());
	}

	return texture;
}

std::string Effect::GetSelectedTechnique() const
{
	if (selectedTechniqueIndex < uiTechniques.size())
		return uiTechniques[selectedTechniqueIndex].techniqueName;
	if (!techniques.empty())
		return techniques.begin()->first;
	return "";
}

void Effect::RenderPasses(ID3DX11EffectTechnique* technique, ID3D11RenderTargetView* outputRTV, uint32_t passOffset)
{
	if (!technique || !outputRTV || !effect)
		return;

	auto context = globals::d3d::context;

	context->OMSetRenderTargets(1, &outputRTV, nullptr);

	uint32_t outputWidth = 0, outputHeight = 0;

	winrt::com_ptr<ID3D11Resource> outputResource;
	outputRTV->GetResource(outputResource.put());

	auto cacheIt = rtvDimensionCache.find(outputRTV);
	if (cacheIt != rtvDimensionCache.end() && cacheIt->second.resource == outputResource) {
		outputWidth = cacheIt->second.width;
		outputHeight = cacheIt->second.height;
	} else {
		D3D11_TEXTURE2D_DESC outputDesc{};
		if (Util::GetTexture2DDesc(outputRTV, outputDesc)) {
			outputWidth = outputDesc.Width;
			outputHeight = outputDesc.Height;
		}
		rtvDimensionCache[outputRTV] = { outputResource, outputWidth, outputHeight };
	}

	if (outputWidth == 0 || outputHeight == 0)
		return;

	// Crop only a full-SBS-width destination, not a fixed-size canvas (e.g. TextureBloom).
	auto& effectManager = EffectManager::GetSingleton();
	const bool cropToEye = effectManager.currentEyeIndex >= 0 && outputWidth == effectManager.currentMainWidth;
	const uint32_t viewportWidth = cropToEye ? outputWidth / 2 : outputWidth;
	const uint32_t viewportOffsetX = cropToEye ? static_cast<uint32_t>(effectManager.currentEyeIndex) * viewportWidth : 0;

	float aspect = static_cast<float>(viewportWidth) / static_cast<float>(outputHeight);
	float screenSize[4] = { static_cast<float>(viewportWidth), 1.0f / viewportWidth, aspect, 1.0f / aspect };
	SetVectorVariable("ScreenSize", screenSize, sizeof(screenSize));

	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = static_cast<float>(viewportOffsetX);
	viewport.Width = static_cast<float>(viewportWidth);
	viewport.Height = static_cast<float>(outputHeight);
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);

	D3DX11_TECHNIQUE_DESC techDesc;
	technique->GetDesc(&techDesc);

	for (UINT p = 0; p < techDesc.Passes; p++) {
		if (profiler)
			profiler->BeginPass(std::format("Effects11::{} Pass {}", GetName(), passOffset + p));
		technique->GetPassByIndex(p)->Apply(0, context);
		context->Draw(4, 0);
		if (profiler)
			profiler->EndPass();
	}
}
