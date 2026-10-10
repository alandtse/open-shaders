#include "Effect.h"
#include "../SettingValueParser.h"
#include "GpuPass.h"
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>

#include <DirectXTex.h>

#include "../ENBExtender.h"
#include "../EffectManager.h"
#include "../EffectSourceCompatibility.h"
#include "../PresetManager.h"
#include "../TextureManager.h"
#include "Features/Effects11/SettingsPatches.h"
#include "Features/Effects11/ShaderPatches.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/ShaderInclude.h"

std::filesystem::path Effect::GetFilePath() const
{
	return PresetManager::GetSingleton().GetENBSeriesPath() / GetName();
}

bool Effect::Load()
{
	std::filesystem::path iniPath = PresetManager::GetSingleton().GetENBSeriesPath() / (GetName() + ".ini");

	if (!std::filesystem::exists(iniPath)) {
		logger::info("[EFFECTS11] Could not find ini file '{}' for effect '{}', using defaults", iniPath.string(), GetName());
		Util::SettingsPatches::Apply(*this);
		CaptureBaseValues();
		return true;
	}

	std::string section = GetName();
	std::transform(section.begin(), section.end(), section.begin(), ::toupper);

	// D3DPreprocess and ENB stringify dots differently, so saved keys need normalized spacing.
	std::unordered_map<std::string, std::string> normalizedIniKeys;
	{
		std::vector<char> keysBuf(65536);
		DWORD keysLen = GetPrivateProfileStringA(section.c_str(), nullptr, "", keysBuf.data(), static_cast<DWORD>(keysBuf.size()), iniPath.string().c_str());
		const char* p = keysBuf.data();
		while (p < keysBuf.data() + keysLen && *p) {
			std::string iniKey(p);
			std::string normalized;
			for (size_t ci = 0; ci < iniKey.size(); ++ci) {
				if (iniKey[ci] == ' ' && ci + 1 < iniKey.size() && iniKey[ci + 1] == '.')
					continue;
				if (iniKey[ci] == '.' && ci + 1 < iniKey.size() && iniKey[ci + 1] == ' ') {
					normalized += '.';
					++ci;
					continue;
				}
				normalized += iniKey[ci];
			}
			normalizedIniKeys[normalized] = iniKey;
			p += iniKey.size() + 1;
		}
	}

	auto findIniKey = [&](const std::string& key) -> const std::string* {
		auto it = normalizedIniKeys.find(key);
		return (it != normalizedIniKeys.end()) ? &it->second : nullptr;
	};

	for (auto& uiVar : uiVariables) {
		if (uiVar.isLabel)
			continue;
		if (!uiVar.effectVariable && !uiVar.isDefine)
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
				auto* realKey = findIniKey(compKey);
				if (!realKey)
					realKey = &compKey;
				std::vector<char> valueBuffer(1024);
				DWORD result = GetPrivateProfileStringA(section.c_str(), realKey->c_str(), "", valueBuffer.data(), 1024, iniPath.string().c_str());
				if (result > 0) {
					Effects11Settings::TryParseFloat(valueBuffer.data(), uiVar.vectorValue[i]);
				}
			}
			if (uiVar.effectVariable)
				uiVar.effectVariable->AsVector()->SetFloatVector(uiVar.vectorValue);
		} else {
			auto* realKey = findIniKey(iniKey);
			if (!realKey)
				realKey = &iniKey;
			std::vector<char> valueBuffer(1024);
			DWORD result = GetPrivateProfileStringA(section.c_str(), realKey->c_str(), "", valueBuffer.data(), 1024, iniPath.string().c_str());
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
	CaptureBaseValues();

	logger::debug("[EFFECTS11] Loaded settings from '{}' for effect '{}'", iniPath.string(), GetName());
	return true;
}

void Effect::CaptureBaseValue(UIVariable& uiVar)
{
	if (uiVar.type == UIVariableType::Float)
		uiVar.baseFloatValue = uiVar.floatValue;
	std::copy(std::begin(uiVar.vectorValue), std::end(uiVar.vectorValue), std::begin(uiVar.baseVectorValue));
}

void Effect::CaptureDefaultValue(UIVariable& uiVar)
{
	// #define-backed values are read from the preset ini while preprocessing, so they have no shader default
	if (uiVar.isLabel || uiVar.isDefine || !uiVar.effectVariable)
		return;
	switch (uiVar.type) {
	case UIVariableType::Float:
		uiVar.defaultFloatValue = uiVar.floatValue;
		break;
	case UIVariableType::Int:
		uiVar.defaultIntValue = uiVar.intValue;
		break;
	case UIVariableType::Bool:
		uiVar.defaultBoolValue = uiVar.boolValue;
		break;
	default:
		std::copy(std::begin(uiVar.vectorValue), std::end(uiVar.vectorValue), std::begin(uiVar.defaultVectorValue));
		break;
	}
	uiVar.hasDefaultValue = true;
}

bool Effect::RestoreDefaultValue(UIVariable& uiVar)
{
	if (!uiVar.hasDefaultValue)
		return false;
	switch (uiVar.type) {
	case UIVariableType::Float:
		uiVar.floatValue = uiVar.defaultFloatValue;
		break;
	case UIVariableType::Int:
		uiVar.intValue = uiVar.defaultIntValue;
		break;
	case UIVariableType::Bool:
		uiVar.boolValue = uiVar.defaultBoolValue;
		break;
	default:
		std::copy(std::begin(uiVar.defaultVectorValue), std::end(uiVar.defaultVectorValue), std::begin(uiVar.vectorValue));
		break;
	}
	return true;
}

void Effect::CaptureBaseValues()
{
	for (auto& uiVar : uiVariables)
		CaptureBaseValue(uiVar);
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
		if (uiVar.isLabel || uiVar.isPatched)
			continue;
		if (!uiVar.effectVariable && !uiVar.isDefine)
			continue;
		std::string iniKey = GetVariableIniKey(uiVar);
		if (iniKey.empty())
			continue;

		std::string value;

		const bool useBase = IsWeatherSeparated(uiVar);
		const float* vectorValue = useBase ? uiVar.baseVectorValue : uiVar.vectorValue;

		switch (uiVar.type) {
		case UIVariableType::Float:
			value = std::to_string(useBase ? uiVar.baseFloatValue : uiVar.floatValue);
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
					std::string compValue = std::to_string(vectorValue[i]);
					BOOL compResult = WritePrivateProfileStringA(section.c_str(), compKey.c_str(), compValue.c_str(), iniPath.string().c_str());
					if (!compResult)
						logger::warn("[EFFECTS11] Failed to write key '{}' to ini file '{}'", compKey, iniPath.string());
				}
				continue;
			} else {
				std::ostringstream oss;
				int numComponents = (uiVar.type == UIVariableType::Float2) ? 2 : (uiVar.type == UIVariableType::Float3) ? 3 :
				                                                                                                          4;

				std::copy(vectorValue, vectorValue + numComponents - 1,
					std::ostream_iterator<float>(oss, ", "));
				oss << vectorValue[numComponents - 1];

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

	SaveWeatherOverrides();

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
		errors.push_back(T("feature.effects11.error.settings", "Failed to load settings"));
		logger::error("[EFFECTS11] Failed to load settings for effect '{}'", GetName());
		return false;
	}

	try {
		CreateEffectTextures();

		for (const auto& entry : techniques)
			for (const auto& info : entry.second)
				if (!info.renderTargetName.empty() && !effectTextureCache.contains(info.renderTargetName))
					GetCachedCommonTexture(info.renderTargetName);
	} catch (const std::exception& error) {
		logger::error("[EFFECTS11] Failed to create resources for '{}': {}", GetName(), error.what());
		effect = nullptr;
		errors.push_back(T("feature.effects11.error.resources", "Failed to create effect resources. See the log for details."));
		return false;
	}

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
	separators.clear();
	externBindings.clear();
	effectTextureCache.clear();
	uiTechniques.clear();
	selectedTechniqueIndex = 0;
	groupMeta.clear();
	techniqueDropdown = {};
	sourceGroupMap.clear();
	sourceOrderMap.clear();

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

	Util::ShaderInclude::File file;
	Util::ShaderInclude::ReadError readError;
	if (!Util::ShaderInclude::Read(filePath, file, readError)) {
		errors.push_back(I18n::GetSingleton()->Format("feature.effects11.error.read_effect", { { "file", filePath.string() } }, "Failed to read effect file: {file}"));
		return false;
	}
	std::string sourceCode(file.data.get(), file.size);

	if (ENBExtender::IsEncryptedSource(sourceCode)) {
		uiDefines.clear();
		std::string error;
		if (!ENBExtender::CreateEncryptedEffect(GetName(), effect, error)) {
			errors.push_back(error);
			return false;
		}
		ReflectCompiledEffect();
		ENBExtender::ResolveCompiledGroups(*this, filePath.parent_path() / (GetName() + ".ini"));
		logger::info("[EFFECTS11] Loaded encrypted FX file through ENB Extender: {}", filePath.string());
		return true;
	}
	EffectSourceCompatibility::PatchInteriorTimeOfDayMacro(sourceCode);

	auto enbseriesPath = filePath.parent_path();
	auto iniFilePath = enbseriesPath / (GetName() + ".ini");
	std::string iniPathStr = iniFilePath.string();
	std::string iniSection = GetName();
	std::transform(iniSection.begin(), iniSection.end(), iniSection.begin(), ::toupper);

	uiDefines.clear();
	Util::ShaderPatches::Apply(GetName().c_str(), sourceCode);
	ENBExtender::ConvertExtenderSyntax(sourceCode, enbseriesPath, uiDefines, iniPathStr, iniSection);

	std::vector<std::string> stringifyMacros;
	ENBExtender::StripStringifyDefines(sourceCode, stringifyMacros);

	auto filePathStr = filePath.string();

	auto compile = [&](const std::string& source, ID3DInclude* include) -> bool {
		winrt::com_ptr<ID3DBlob> compiled, err;
		HRESULT hr = D3DCompile(source.c_str(), source.size(), filePathStr.c_str(),
			nullptr, include, nullptr, "fx_5_0", 0, 0, compiled.put(), err.put());
		if (FAILED(hr)) {
			if (err) {
				std::string raw(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
				std::string filtered;
				std::istringstream stream(raw);
				std::string line;
				while (std::getline(stream, line))
					if (!line.empty() && line.find("warning X4717") == std::string::npos)
						filtered += line + "\n";
				if (!filtered.empty())
					logger::warn("[EFFECTS11] D3DCompile failed for '{}': {}", filePathStr, filtered);
			}
			return false;
		}
		return SUCCEEDED(D3DX11CreateEffectFromMemory(compiled->GetBufferPointer(),
			compiled->GetBufferSize(), 0, globals::d3d::device, effect.put()));
	};

	auto preprocess = [&](const std::string& source, ID3DInclude* include) -> std::string {
		winrt::com_ptr<ID3DBlob> blob, err;
		if (FAILED(D3DPreprocess(source.c_str(), source.size(), filePathStr.c_str(),
				nullptr, include, blob.put(), err.put())) ||
			!blob)
			return {};
		return { static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize() };
	};

	bool compiled = false;

	{
		ENBExtender::PresetInclude ppInclude(enbseriesPath, uiDefines, iniPathStr, iniSection);
		auto pp = preprocess(sourceCode, &ppInclude);
		if (!pp.empty()) {
			auto allMacros = stringifyMacros;
			auto& inclMacros = ppInclude.GetStringifyMacros();
			allMacros.insert(allMacros.end(), inclMacros.begin(), inclMacros.end());
			if (!allMacros.empty())
				ENBExtender::ExpandStringifyMacros(pp, allMacros);
			ENBExtender::ParseSourceGroupScopes(pp, *this);
			ENBExtender::StripLineDirectives(pp);
			compiled = compile(pp, nullptr);
		}
	}

	if (!compiled) {
		ENBExtender::PresetInclude includeHandler(enbseriesPath, uiDefines, iniPathStr, iniSection);
		winrt::com_ptr<ID3DBlob> compiledShader, errorBlob;
		HRESULT hr = D3DCompile(sourceCode.c_str(), sourceCode.size(), filePathStr.c_str(),
			nullptr, &includeHandler, nullptr, "fx_5_0", 0, 0, compiledShader.put(), errorBlob.put());
		if (FAILED(hr)) {
			std::string errorMsg = T("feature.effects11.error.compile", "Compilation failed");
			if (errorBlob) {
				std::string raw(static_cast<const char*>(errorBlob->GetBufferPointer()), errorBlob->GetBufferSize());
				errorMsg.clear();
				logger::error("[EFFECTS11] Effect compilation failed for '{}'", filePathStr);
				std::istringstream errorStream(raw);
				std::string errorLine;
				while (std::getline(errorStream, errorLine))
					if (!errorLine.empty() && errorLine.find("warning X4717") == std::string::npos) {
						logger::error("[EFFECTS11]   {}", errorLine);
						errorMsg += errorLine + "\n";
					}
				if (errorMsg.empty())
					errorMsg = T("feature.effects11.error.compile", "Compilation failed");
			}
			errors.push_back(errorMsg);
			return false;
		}
		if (FAILED(D3DX11CreateEffectFromMemory(compiledShader->GetBufferPointer(),
				compiledShader->GetBufferSize(), 0, globals::d3d::device, effect.put()))) {
			errors.push_back(T("feature.effects11.error.create", "Failed to create effect from compiled shader"));
			return false;
		}
	}

	ReflectCompiledEffect();

	logger::info("[EFFECTS11] Successfully loaded FX file: {}", filePathStr);
	return true;
}

void Effect::ReflectCompiledEffect()
{
	EnumerateAllVariables();
	SetupCustomTextures();
	LoadTechniques();
	LoadUITechniques();

	LoadUIVariables();
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
	bool wroteChain = false;
	bool targetInOutput = false;
	bool targetInTemp = false;

	ID3D11ShaderResourceView* inputSRV = nullptr;
	ID3D11RenderTargetView* outputRTV = nullptr;

	for (size_t i = 0; i < sequence.size(); ++i) {
		auto& techniqueInfo = sequence[i];

		if (!techniqueInfo.technique)
			continue;

		if (!IsTechniqueEnabled(techniqueInfo))
			continue;

		if (sequence.size() == 1 || swapCounter == 0) {
			inputSRV = a_input;
			outputRTV = a_output.rtv.get();
		} else {
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

		if (sourceTexture && sourceTexture->IsValid())
			sourceTexture->AsShaderResource()->SetResource(inputSRV);

		RenderPasses(techniqueInfo.technique.get(), outputRTV, passOffset);
		passOffset += techniqueInfo.passCount;

		if (outputRTV == a_output.rtv.get() || outputRTV == a_temp.rtv.get()) {
			wroteChain = true;
			targetInOutput = (outputRTV == a_output.rtv.get());
			targetInTemp = !targetInOutput;
		}
	}

	return { wroteChain, targetInOutput, targetInTemp };
}

bool Effect::ExecuteTechnique(const std::string& techniqueName, TextureManager::Texture& output)
{
	if (!IsCompiled() || !effect)
		return false;

	auto technique = effect->GetTechniqueByName(techniqueName.c_str());
	if (!technique || !technique->IsValid())
		return false;

	RenderPasses(technique, output.rtv.get());
	return true;
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

			auto readBindings = [&](const std::string& suffix) {
				std::string bindVal = GetTechniqueAnnotation(technique, "UIBinding" + suffix);
				if (!bindVal.empty())
					info.bindings.push_back({ bindVal, false });
				std::string invVal = GetTechniqueAnnotation(technique, "UIInvBinding" + suffix);
				if (!invVal.empty())
					info.bindings.push_back({ invVal, true });
			};
			readBindings("");
			for (int bi = 0; bi < 16; ++bi)
				readBindings(std::to_string(bi));

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

	ENBExtender::LoadTechniqueDropdownMetadata(*this);

	uint32_t defaultIndex = 0;

	for (UINT g = 0; g < effectDesc.Groups; ++g) {
		auto group = effect->GetGroupByIndex(g);
		if (!group || !group->IsValid())
			continue;

		D3DX11_GROUP_DESC groupDesc;
		if (FAILED(group->GetDesc(&groupDesc)))
			continue;

		bool isNamedGroup = groupDesc.Name && groupDesc.Name[0];

		if (isNamedGroup) {
			std::string uiName = GetGroupAnnotation(group, "UIName");
			if (uiName.empty())
				continue;

			std::string isDefault = GetGroupAnnotation(group, "UIDefault");
			if (!isDefault.empty() && isDefault != "0" && isDefault != "false")
				defaultIndex = static_cast<uint32_t>(uiTechniques.size());

			uiTechniques.push_back({ std::string(groupDesc.Name), uiName });
		} else {
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

				std::string techName = techDesc.Name ? std::string(techDesc.Name) : "";

				std::string isDefault = GetTechniqueAnnotation(technique, "UIDefault");
				if (!isDefault.empty() && isDefault != "0" && isDefault != "false")
					defaultIndex = static_cast<uint32_t>(uiTechniques.size());

				uiTechniques.push_back({ techName, uiName });
			}
		}
	}

	if (defaultIndex < uiTechniques.size())
		selectedTechniqueIndex = defaultIndex;
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

	std::vector<std::string> groupStack;

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

		if (typeDesc.Class == D3D_SVC_OBJECT && typeDesc.Type == D3D_SVT_STRING) {
			ENBExtender::ProcessExtenderStringVariable(variable, varDesc, groupStack, *this);
			continue;
		}

		auto externBinding = GetUIAnnotation(variable, "ExternBinding");
		if (!externBinding.empty()) {
			ExternBindingInfo eb;
			eb.bindingName = externBinding;
			eb.variable.copy_from(variable);
			externBindings.push_back(std::move(eb));
			continue;
		}

		UIVariable uiVar = {};
		if (ENBExtender::CreateUIVariable(uiVar, variable, varDesc, typeDesc, groupStack, *this)) {
			LoadUIVariableValue(uiVar);
			CaptureDefaultValue(uiVar);
			uiVariables.push_back(std::move(uiVar));
		}
	}

	ENBExtender::InsertUIDefines(*this);

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
	if (!uiVar.uniqueName.empty())
		return uiVar.uniqueName;
	return uiVar.group.empty() ? uiVar.displayName : uiVar.group + "." + uiVar.displayName;
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
			if (!Effects11Settings::TryParseFloat(value, uiVar.floatValue))
				return;
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
					float component;
					if (!(ss >> component))
						break;
					if (std::isfinite(component))
						uiVar.vectorValue[i] = component;
					if (ss.peek() == ',')
						ss.get();
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
	return TextureManager::CreateTexture(width, height, format, debugName);
}

std::string Effect::GetSelectedTechnique() const
{
	if (selectedTechniqueIndex < uiTechniques.size())
		return uiTechniques[selectedTechniqueIndex].techniqueName;
	if (!techniques.empty())
		return techniques.begin()->first;
	return "";
}

void Effect::UpdateExternBindings()
{
	if (externBindings.empty())
		return;

	auto& fb = globals::game::frameBufferCached;
	const auto eye = std::max(0, EffectManager::GetSingleton().currentEyeIndex);
	auto invView = fb.GetCameraViewInverse(eye);
	auto wvp = fb.GetCameraViewProj(eye);
	auto invWvp = fb.GetCameraViewProjInverse(eye);

	for (auto& eb : externBindings) {
		if (!eb.variable)
			continue;
		auto* vec = eb.variable->AsVector();
		if (!vec || !vec->IsValid())
			continue;

		if (eb.bindingName == "InvCamRotMatColumn0")
			vec->SetFloatVector(reinterpret_cast<const float*>(&invView.m[0]));
		else if (eb.bindingName == "InvCamRotMatColumn1")
			vec->SetFloatVector(reinterpret_cast<const float*>(&invView.m[1]));
		else if (eb.bindingName == "InvCamRotMatColumn2")
			vec->SetFloatVector(reinterpret_cast<const float*>(&invView.m[2]));
		else if (eb.bindingName == "WVPMatColumn0")
			vec->SetFloatVector(reinterpret_cast<const float*>(&wvp.m[0]));
		else if (eb.bindingName == "WVPMatColumn1")
			vec->SetFloatVector(reinterpret_cast<const float*>(&wvp.m[1]));
		else if (eb.bindingName == "WVPMatColumn2")
			vec->SetFloatVector(reinterpret_cast<const float*>(&wvp.m[2]));
		else if (eb.bindingName == "WVPMatColumn3")
			vec->SetFloatVector(reinterpret_cast<const float*>(&wvp.m[3]));
		else if (eb.bindingName == "InvWVPMatColumn0")
			vec->SetFloatVector(reinterpret_cast<const float*>(&invWvp.m[0]));
		else if (eb.bindingName == "InvWVPMatColumn1")
			vec->SetFloatVector(reinterpret_cast<const float*>(&invWvp.m[1]));
		else if (eb.bindingName == "InvWVPMatColumn2")
			vec->SetFloatVector(reinterpret_cast<const float*>(&invWvp.m[2]));
		else if (eb.bindingName == "InvWVPMatColumn3")
			vec->SetFloatVector(reinterpret_cast<const float*>(&invWvp.m[3]));
	}
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
		const auto passName = std::format("Effects11::{} Pass {}", GetName(), passOffset + p);
		CS_GPU_PASS_DYNAMIC(passName);
		technique->GetPassByIndex(p)->Apply(0, context);
		context->Draw(4, 0);
	}
}
