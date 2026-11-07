#pragma once

class WeatherPickerWindow
{
public:
	static WeatherPickerWindow* GetSingleton()
	{
		static WeatherPickerWindow singleton;
		return &singleton;
	}

	/** @brief Draw the floating weather picker (anchored above the palette until moved). */
	void Draw();
	/** @brief Picker body: weather combo, lock and transition controls. */
	void DrawContents(bool expanded = false);
	/** @brief Draw the shared weather list and filter buttons within the supplied bounds. */
	static void DrawSelection(float width, float height, bool fullWidthSearch = false);

	bool open = true;
};
