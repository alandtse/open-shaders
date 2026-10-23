#pragma once

namespace GrassWindSettings
{
	struct Range
	{
		float minimum;
		float maximum;
	};

	inline constexpr Range kBendSensitivity{ 0.0f, 4.0f };
	inline constexpr Range kFlutterSensitivity{ 0.0f, 4.0f };
}
