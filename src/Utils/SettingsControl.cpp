#include "SettingsControl.h"

#include <cassert>

namespace
{
	SettingsControl::Proxy proxy;
	SettingsControl::Callbacks callbacks;
}

namespace SettingsControl
{
	ProxyScope::ProxyScope(const void* member, float displayScale) :
		previous(proxy)
	{
		assert(displayScale != 0.0f);
		proxy = { member, displayScale };
	}

	ProxyScope::~ProxyScope()
	{
		proxy = previous;
	}

	void SetCallbacks(const Callbacks& value)
	{
		callbacks = value;
	}

	const Proxy* GetProxy()
	{
		return proxy.member ? &proxy : nullptr;
	}

	bool IsReplicating()
	{
		return callbacks.isReplicating && callbacks.isReplicating();
	}

	bool InterceptInt(const char* label, int& value, const IntDrawer& draw)
	{
		return callbacks.interceptInt ? callbacks.interceptInt(label, value, draw) : draw(value);
	}

	bool DrawPathControls(const SettingIdentity& setting, std::string& path, std::string& error, const PathValidator& validate)
	{
		return callbacks.drawPath && callbacks.drawPath(setting, path, error, validate);
	}

	void RecordBaselineEdit(const SettingIdentity& setting, const nlohmann::json& value)
	{
		if (callbacks.recordBaselineEdit)
			callbacks.recordBaselineEdit(setting, value);
	}
}
