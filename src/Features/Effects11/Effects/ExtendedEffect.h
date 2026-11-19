#pragma once

#include "../UITree.h"
#include "../WeatherManager.h"
#include "Effect.h"

#include <span>

#ifdef ENABLE_ENB_EXTENDER

class ExtendedEffect : public Effect
{
public:
	/** @brief Loads per-weather overrides and rebuilds the parsed-value cache. */
	void LoadWeatherData();
	/** @brief Blends cached weather values into the reflected shader variables. */
	void ApplyWeatherBlending(float blendFactor, uint32_t currentWeatherID, uint32_t lastWeatherID);
	/** @brief Updates the edited weather override and invalidates its saved value. */
	void SyncWeatherVarFromUI(size_t index, uint32_t weatherID);
	/** @brief Interpolates time-period parameters into their base shader variables. */
	void ApplyTimeOfDayInterpolation();
	/** @brief Writes modified per-weather shader settings. @return False when a weather file failed to write. */
	bool SaveWeatherOverrides() override;

	/** @brief Weather entry an edit of this variable is saved to, or null when it belongs to the base value. */
	WeatherManager::WeatherEntry* WeatherEntryForEdit(size_t index, uint32_t weatherID) const;
	/** @brief A weather file's stored text for an ini key, or null when that file does not define it. @param weatherIDs Every weather ID sharing the file. */
	const std::string* GetWeatherValue(std::span<const uint32_t> weatherIDs, const std::string& iniKey) const;
	/** @brief Sets, or with null removes, an ini key for every weather sharing one file. Not marked dirty, so it is never saved. */
	void SetWeatherValue(std::span<const uint32_t> weatherIDs, const std::string& iniKey, const std::string* value);
	/** @brief Re-parses the weather values after SetWeatherValue calls. */
	void RefreshWeatherCaches() { RebuildWeatherCaches(); }

	/** @brief Releases the effect and clears its dependent interpolation caches. */
	void Unload() override;
	/** @brief Evaluates the technique bindings against current parameter values. */
	bool IsTechniqueEnabled(TechniqueInfo& info) override;

	// Rendering
	/** @brief Draws this effect using the shared shader parameter editor. */
	void RenderImGui() override;
	/** @brief Draws the parameters of several effects as one annotation-ordered tree. */
	static void RenderMergedUI(std::span<Effect*> effects, UITree::FilterMode filter = UITree::FilterMode::All, UITree::ViewOptions* options = nullptr);

private:
	using WeatherValues = std::unordered_map<std::string, std::string>;
	std::unordered_map<uint32_t, WeatherValues> weatherData;

	// Cache parsed weather values so per-frame blending performs no string parsing.
	struct WeatherVarSlot
	{
		size_t index = 0;  ///< Index into uiVariables
		std::string iniKey;
		int components = 1;            ///< 1 for Float, 2-4 for vectors
		bool perComponent = false;     ///< Vector stored as KeyX/KeyY/... keys
		bool exteriorWeather = false;  ///< separation == "ExteriorWeather"
	};
	struct ParsedWeatherValue
	{
		float values[4] = {};
		uint8_t definedMask = 0;  ///< Bit c set when component c was present and parsed
	};
	std::vector<WeatherVarSlot> weatherVarSlots;
	std::vector<int> weatherSlotOfVariable;  ///< uiVariables index -> slot, or -1
	std::unordered_map<uint32_t, std::vector<ParsedWeatherValue>> parsedWeatherData;
	ID3DX11Effect* weatherCacheEffect = nullptr;
	size_t weatherCacheVariableCount = 0;

	void EnsureWeatherCaches();
	void RebuildWeatherCaches();
	void ParseWeatherValue(const WeatherValues& values, const WeatherVarSlot& slot, ParsedWeatherValue& out) const;

	// Time-of-day variables grouped by base variable, built once per compiled effect
	struct TimeOfDayEntry
	{
		size_t index = 0;  ///< Index into uiVariables
		int period = -1;   ///< Index into the period weight table, -1 for unknown periods
	};
	struct TimeOfDayGroup
	{
		ID3DX11EffectVariable* baseVariable = nullptr;
		int components = 1;  ///< 1 for Float, 2-4 for vectors
		bool exteriorWeather = false;
		std::vector<TimeOfDayEntry> entries;
	};
	std::vector<TimeOfDayGroup> timeOfDayGroups;
	ID3DX11Effect* timeOfDayCacheEffect = nullptr;
	size_t timeOfDayCacheVariableCount = 0;

	void EnsureTimeOfDayGroups();
	void RebuildTimeOfDayGroups();
	static int GetPeriodIndex(const std::string& period);

	/** @brief Dirty ini keys per weather file, each mapped to the weather ID whose values it was edited under.
		Several weatherlist sections may share one FileName, so the source weather is tracked per key. */
	using DirtyWeatherKeys = std::unordered_map<std::string, uint32_t>;
	std::unordered_map<std::string, DirtyWeatherKeys> dirtyWeatherFiles;

	std::unordered_map<std::string, int> bindingCache;

	int ResolveTechniqueBinding(const std::string& variableName);
};

using EffectBase = ExtendedEffect;

#else

using EffectBase = Effect;

#endif
