// Test-only stub for I18n::Get.
//
// Some units-under-test compiled directly into cpp_tests (e.g. Subrect.cpp's
// DrawEditor) now call T(), which resolves to I18n::GetSingleton()->Get(...).
// Linking the real src/I18n/I18n.cpp would drag the translation loader (file
// I/O, locale discovery, spdlog) into the test binary. The tests only exercise
// non-UI logic, so stub Get to return the inline English default -- identical
// to the runtime fallback when no translation is loaded.
#include "I18n/I18n.h"

const char* I18n::Get(std::string_view, const char* defaultText) const
{
	return defaultText ? defaultText : "";
}
