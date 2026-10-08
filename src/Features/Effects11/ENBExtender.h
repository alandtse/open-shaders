#pragma once

#include <d3dcompiler.h>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include "Effects/Effect.h"
#include "PresetInclude.h"

namespace ENBExtender
{
	// Shared helpers
	/** @brief Parses an integer annotation, returning the fallback on failure. */
	int SafeStoi(const std::string& s, int fallback = 0);
	/** @brief Parses a finite float annotation, returning the fallback on failure. */
	float SafeStof(const std::string& s, float fallback = 0.0f);

	// Source preprocessing
	/** @brief Converts extender directives and collects editable compile-time definitions. */
	void ConvertExtenderSyntax(std::string& content, const std::filesystem::path& enbseriesPath, std::vector<Effect::UIDefineInfo>& uiDefines, const std::string& iniPath = "", const std::string& iniSection = "");
	/** @brief Collects and removes stringification definitions before preprocessing. */
	void StripStringifyDefines(std::string& source, std::vector<std::string>& macroNames);
	/** @brief Expands invocations of the collected stringification macros. */
	void ExpandStringifyMacros(std::string& source, const std::vector<std::string>& macroNames);

	// File preprocessing
	/** @brief Removes preprocessor line markers from the rewritten source. */
	void StripLineDirectives(std::string& source);
	class PresetInclude : public ID3DInclude
	{
	public:
		/** @brief Wraps the preset include resolver with extender source conversion. */
		PresetInclude(const std::filesystem::path& a_basePath, std::vector<Effect::UIDefineInfo>& a_uiDefines, const std::string& a_iniPath = "", const std::string& a_iniSection = "");
		/** @brief Returns an owned, converted include buffer while preserving parent paths. */
		HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR pFileName, LPCVOID, LPCVOID* ppData, UINT* pBytes) override;
		/** @brief Releases the converted buffer and its original include. */
		HRESULT __stdcall Close(LPCVOID pData) override;

		/** @brief Returns stringification macros collected from opened includes. */
		const std::vector<std::string>& GetStringifyMacros() const { return stringifyMacros; }

	private:
		std::filesystem::path basePath;
		std::vector<Effect::UIDefineInfo>& uiDefines;
		std::string iniPath;
		std::string iniSection;
		struct IncludeData
		{
			std::unique_ptr<char[]> source;
			LPCVOID original;
		};
		EffectSourceCompatibility::PresetInclude sourceIncludes;
		std::unordered_map<LPCVOID, IncludeData> openIncludes;
		std::vector<std::string> stringifyMacros;
	};

	// UI variable processing
	/** @brief Records source ordering and groups for reflected variables. */
	void ParseSourceGroupScopes(const std::string& preprocessedSource, Effect& effect);
	/** @brief Collects labels, separators and group directives from string variables. */
	bool ProcessExtenderStringVariable(ID3DX11EffectVariable* variable, const D3DX11_EFFECT_VARIABLE_DESC& varDesc, std::vector<std::string>& groupStack, Effect& effect);
	/** @brief Builds a validated editor parameter from a reflected effect variable. */
	bool CreateUIVariable(Effect::UIVariable& out, ID3DX11EffectVariable* variable, const D3DX11_EFFECT_VARIABLE_DESC& varDesc, const D3DX11_EFFECT_TYPE_DESC& typeDesc, const std::vector<std::string>& groupStack, Effect& effect);
	/** @brief Adds editable compile-time definitions to the reflected parameter list. */
	void InsertUIDefines(Effect& effect);

	// Post-load processing
	/** @brief Reads the preset technique selector annotations. */
	void LoadTechniqueDropdownMetadata(Effect& effect);

	/** @brief Recognizes the encrypted preset source signature. */
	bool IsEncryptedSource(std::string_view source);
	/** @brief Creates an effect from bytecode supplied by the loaded ENB Extender. */
	bool CreateEncryptedEffect(const std::string& effectName, winrt::com_ptr<ID3DX11Effect>& effect, std::string& error);
	/** @brief Recovers encrypted preset groups from reflected names and saved settings. */
	void ResolveCompiledGroups(Effect& effect, const std::filesystem::path& iniPath);
}
