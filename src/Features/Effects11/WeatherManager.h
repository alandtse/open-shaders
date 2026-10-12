#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

class WeatherManager
{
public:
	static WeatherManager& GetSingleton();

	struct WeatherEntry
	{
		std::string fileName;
		std::vector<uint32_t> weatherIDs;
	};

	void Initialize();
	void LoadWeatherList();
	void LoadLocationWeather();

	WeatherEntry* FindWeatherEntry(uint32_t weatherID);

	/// @brief Gets the effective weather ID, checking for location-based overrides first.
	/// @param actualWeatherID The real weather form ID from the game
	/// @return Location-mapped weather ID if applicable, otherwise the actual weather ID
	/// @note Interior cells have no worldspace, so they resolve through the `[00000000]`
	/// section of _locationweather.ini (ENB's convention for worldspace-less locations),
	/// keyed the same way as exterior worldspace sections -- see GetEffectiveWeatherID's
	/// implementation and _locationweather.ini's own documented format.
	uint32_t GetEffectiveWeatherID(uint32_t actualWeatherID);

	uint32_t GetWeatherIndex(uint32_t weatherID) const;

	const std::unordered_map<std::string, WeatherEntry>& GetWeatherEntries() const { return weatherEntries; }

	std::unordered_map<std::string, std::string> GetWeatherFiles() const;

	/** @brief Drops the load-order index; ENB weather list and location keys are matched without it. */
	static constexpr uint32_t LocalFormIDMask = 0x00FFFFFF;

private:
	std::unordered_map<std::string, WeatherEntry> weatherEntries;
	std::unordered_map<uint32_t, std::string> weatherIDMap;

	// Location weather: worldSpaceID -> (locationID -> fakeWeatherID)
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> locationWeatherMap;
};