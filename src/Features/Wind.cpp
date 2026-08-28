#include "Wind.h"

#include "CSUtility.h"
#include "Globals.h"

void Wind::DrawSettings()
{
	globals::features::csUtility.DrawWindSettings();
}

void Wind::RestoreCurrentPageDefaultSettings()
{
	globals::features::csUtility.RestoreCurrentPageDefaultSettings();
}

bool Wind::ReapplyCurrentPageOverrideSettings()
{
	return globals::features::csUtility.ReapplyCurrentPageOverrideSettings();
}
