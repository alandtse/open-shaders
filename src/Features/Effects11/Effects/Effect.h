#pragma once

#include <Effects11/d3dx11effect.h>
#include <filesystem>
#include <winrt/base.h>

#include "../TextureManager.h"
#include "Profiler.h"

class Effect
{
public:
	Effect() = default;
	virtual ~Effect() = default;

	// UI technique structure (defined early for use in method declarations)
	struct UITechnique
	{
		std::string techniqueName;  // Actual technique name
		std::string displayName;    // UIName annotation
	};

	// Settings methods
	bool Load();
	void Save();

	// Effect lifecycle
	virtual bool Apply();   // Clear resources, load settings, recompile, create resources
	virtual void Unload();  // Clear all resources

	/** @brief Drops texture bindings while retaining the compiled effect and UI values. */
	virtual void ReleaseResources();
	/** @brief Restores textures without recompiling or reloading settings. */
	void RestoreResources();

	bool IsCompiled() const { return filePresent && errors.empty(); }
	bool IsFilePresent() const { return filePresent; }
	const std::vector<std::string>& GetErrors() const { return errors; }

	/** @brief Absolute path of this effect's .fx file inside the resolved preset folder. */
	std::filesystem::path GetFilePath() const;

	virtual void Execute() = 0;
	virtual void UpdateEffectVariables() {}

	// Virtual texture creation function for derived classes to override
	virtual void CreateEffectTextures() {}

	// UI System
	virtual void RenderImGui();
	void LoadUIVariables();
	void UpdateUIVariables();

	// Technique selection
	std::string GetSelectedTechnique() const;

	// Pure virtual methods for derived classes to implement
	virtual std::string GetName() const = 0;
	virtual bool IsRequired() const { return false; }

	struct TechniqueInfo
	{
		winrt::com_ptr<ID3DX11EffectTechnique> technique;
		std::string renderTargetName;
		uint32_t passCount = 0;
	};

	Profiler* profiler = nullptr;

	winrt::com_ptr<ID3DX11Effect> effect;
	std::unordered_map<std::string, std::vector<TechniqueInfo>> techniques;
	std::unordered_map<std::string, winrt::com_ptr<ID3DX11EffectVariable>> variables;

	std::unordered_map<std::string, TextureManager::Texture> effectTextureCache;
	std::unordered_map<std::string, winrt::com_ptr<ID3D11ShaderResourceView>> customTextureCache;

	// UI Variable System
	enum class UIVariableType
	{
		Float,
		Float2,
		Int,
		Bool,
		Float3,
		Float4
	};

	enum class UIWidgetType
	{
		Default,
		Dropdown,
		Vector,
		Quality,
		Color
	};

	struct UIVariable
	{
		UIVariableType type = UIVariableType::Float;
		UIWidgetType widgetType = UIWidgetType::Default;
		std::string name;
		std::string displayName;
		winrt::com_ptr<ID3DX11EffectVariable> effectVariable;

		// Value storage
		union
		{
			float floatValue = 0.0f;
			int intValue;
			bool boolValue;
		};

		float vectorValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

		// UI properties
		float floatMin = 0.0f;
		float floatMax = 1.0f;
		int intMin = 0;
		int intMax = 100;
		bool isLabel = false;
		bool isReadOnly = false;
		bool isHidden = false;

		std::vector<std::string> dropdownItems;
	};

	std::vector<UIVariable> uiVariables;

	// UI technique selection (indexed by uint, only includes annotated techniques)
	std::vector<UITechnique> uiTechniques;
	uint32_t selectedTechniqueIndex = 0;

	// Error tracking
	bool filePresent = false;
	std::vector<std::string> errors;

	struct TechniqueSequenceResult
	{
		bool executed = false;
		bool inOutput = false;
	};

	// Execute a technique sequence with ping-pong rendering
	TechniqueSequenceResult ExecuteTechniqueSequence(const std::string& a_baseTechniqueName, ID3D11ShaderResourceView* a_input, TextureManager::Texture& a_output, TextureManager::Texture& a_temp);

	// Execute a single technique
	void ExecuteTechnique(const std::string& techniqueName, TextureManager::Texture& output);

	// Allow EffectManager to setup common variables
	ID3DX11Effect* GetEffect() const { return effect.get(); }

	// Helper function to set shader resource variables (non-static version for this effect)
	bool SetShaderResourceVariable(const std::string& variableName, ID3D11ShaderResourceView* resource);

	// Texture creation helper
	static TextureManager::Texture CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, const std::string& debugName);

	// Static helper functions for any effect
	static bool SetShaderResourceVariable(ID3DX11Effect* effect, const std::string& variableName, ID3D11ShaderResourceView* resource);
	static bool SetVectorVariable(ID3DX11Effect* effect, const std::string& variableName, const void* data, uint32_t size);

	// Helper function for safe vector variable access
	bool SetVectorVariable(const std::string& variableName, const void* data, uint32_t size);

	void RenderPasses(ID3DX11EffectTechnique* technique, ID3D11RenderTargetView* outputRTV, uint32_t passOffset = 0);

	// UI annotation helpers
	std::string GetUIAnnotation(ID3DX11EffectVariable* variable, const std::string& annotationName);
	static std::string GetTechniqueAnnotation(ID3DX11EffectTechnique* technique, const std::string& annotationName);
	static std::string GetGroupAnnotation(ID3DX11EffectGroup* group, const std::string& annotationName);

	ID3DX11EffectVariable* GetCachedVariable(const std::string& name);
	TextureManager::Texture* GetCachedCommonTexture(const std::string& name);
	void ClearVariableCache();

protected:
	static bool IsPerComponentVector(const UIVariable& uiVar);
	std::string GetVariableIniKey(const UIVariable& uiVar);

private:
	bool LoadFXFile();

	std::unordered_map<std::string, ID3DX11EffectVariable*> variableCache;
	std::unordered_map<std::string, TextureManager::Texture*> commonTexturePointerCache;
	struct RTVDimensionEntry
	{
		winrt::com_ptr<ID3D11Resource> resource;
		uint32_t width;
		uint32_t height;
	};
	// resource lets a lookup detect a released RTV's address reused for a new view (routine
	// with EffectManager::EnsureCropTarget's per-eye targets) instead of trusting a stale size.
	std::unordered_map<ID3D11RenderTargetView*, RTVDimensionEntry> rtvDimensionCache;

	void EnumerateAllVariables();

	void SetupCustomTextures();
	ID3D11ShaderResourceView* LoadTextureFromFile(const std::string& filename);

	void LoadTechniques();
	void LoadUITechniques();
	ID3D11RenderTargetView* GetRenderTargetView(const std::string& renderTargetName, ID3D11RenderTargetView* fallback);

	void LoadUIVariableValue(UIVariable& uiVar);
	void LoadVariableFromString(UIVariable& uiVar, const std::string& value);
};
