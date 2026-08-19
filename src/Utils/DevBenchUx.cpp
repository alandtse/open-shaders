#include "Utils/DevBenchUx.h"

namespace Util::DevBenchUx
{
	Registry& Registry::GetSingleton()
	{
		static Registry singleton;
		return singleton;
	}

	void Registry::RegisterCommand(std::string_view a_featureShortName, std::string_view a_name, std::string_view a_description, CommandFn a_fn)
	{
		commands[std::string(a_featureShortName)][std::string(a_name)] = { std::string(a_description), a_fn };
	}

	void Registry::RegisterQuery(std::string_view a_featureShortName, std::string_view a_name, std::string_view a_description, QueryFn a_fn)
	{
		queries[std::string(a_featureShortName)][std::string(a_name)] = { std::string(a_description), a_fn };
	}

	const CommandEntry* Registry::FindCommand(std::string_view a_featureShortName, std::string_view a_name) const
	{
		const auto featureIt = commands.find(std::string(a_featureShortName));
		if (featureIt == commands.end())
			return nullptr;
		const auto entryIt = featureIt->second.find(std::string(a_name));
		return entryIt == featureIt->second.end() ? nullptr : &entryIt->second;
	}

	const QueryEntry* Registry::FindQuery(std::string_view a_featureShortName, std::string_view a_name) const
	{
		const auto featureIt = queries.find(std::string(a_featureShortName));
		if (featureIt == queries.end())
			return nullptr;
		const auto entryIt = featureIt->second.find(std::string(a_name));
		return entryIt == featureIt->second.end() ? nullptr : &entryIt->second;
	}

	json Registry::ListCommands(std::string_view a_featureShortName) const
	{
		json out = json::array();
		const auto featureIt = commands.find(std::string(a_featureShortName));
		if (featureIt != commands.end())
			for (const auto& [name, entry] : featureIt->second)
				out.push_back(json{ { "name", name }, { "description", entry.description } });
		return out;
	}

	json Registry::ListQueries(std::string_view a_featureShortName) const
	{
		json out = json::array();
		const auto featureIt = queries.find(std::string(a_featureShortName));
		if (featureIt != queries.end())
			for (const auto& [name, entry] : featureIt->second)
				out.push_back(json{ { "name", name }, { "description", entry.description } });
		return out;
	}
}
