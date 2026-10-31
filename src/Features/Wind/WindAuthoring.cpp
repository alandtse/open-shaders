#include "Wind.h"

#include "Trees/TreeWindPatcher.h"

namespace
{
	template <class F>
	bool GuardAuthoring(F&& a_operation) noexcept
	{
		try {
			return a_operation();
		} catch (...) {
			return false;
		}
	}

	template <class T>
	OpenWind::TreeAuthoringValues TreeValues(const T& a_rule)
	{
		return { a_rule.bend, a_rule.leafAmbient, a_rule.upperBendRange, a_rule.maximumDisplacementPercent,
			a_rule.trunkGustInfluence, a_rule.leafGustInfluence, a_rule.transientWindInfluence,
			a_rule.leafTransientWindInfluence, a_rule.leafTransientFlutterMaximum, a_rule.transientMaximumBendMultiplier };
	}

	TreeWindPatcher::Sensitivities ReadTreeValues(const OpenWind::TreeAuthoringValues& a_values)
	{
		TreeWindPatcher::Sensitivities values;
		values.bend = a_values.bend;
		values.leafAmbient = a_values.leafAmbient;
		values.upperBendRange = a_values.upperBendRange;
		values.maximumDisplacementPercent = a_values.maximumDisplacementPercent;
		values.trunkGustInfluence = a_values.trunkGustInfluence;
		values.leafGustInfluence = a_values.leafGustInfluence;
		values.transientWindInfluence = a_values.transientWindInfluence;
		values.leafTransientWindInfluence = a_values.leafTransientWindInfluence;
		values.leafTransientFlutterMaximum = a_values.leafTransientFlutterMaximum;
		values.transientMaximumBendMultiplier = a_values.transientMaximumBendMultiplier;
		return values;
	}

	bool VisitRules(void*, bool a_grass, void* a_visitor, bool (*a_visit)(void*, const OpenWind::AuthoringRule*) noexcept) noexcept
	{
		return GuardAuthoring([&] {
			if (a_grass || !a_visit)
				return false;
			for (size_t index = 0; index < TreeWindPatcher::GetRuleCount(); ++index) {
				const auto rule = TreeWindPatcher::GetRule(index);
				const std::string mesh(rule.mesh);
				const OpenWind::AuthoringRule entry{ index, mesh.c_str(), rule.unsaved, TreeValues(rule), 0.0f, 0.0f, rule.bendEnabled };
				if (!a_visit(a_visitor, &entry))
					return false;
			}
			return true;
		});
	}

	bool VisitPaths(void*, bool a_grass, bool a_unconfigured, void* a_visitor, bool (*a_visit)(void*, const char*) noexcept) noexcept
	{
		return GuardAuthoring([&] {
			if (a_grass || a_unconfigured || !a_visit)
				return false;
			for (const auto& path : TreeWindPatcher::GetConflictingFiles()) {
				if (!a_visit(a_visitor, path.c_str()))
					return false;
			}
			return true;
		});
	}

	bool GetUniversal(void*, bool* a_enabled, OpenWind::TreeAuthoringValues* a_values) noexcept
	{
		return GuardAuthoring([&] {
			if (!a_enabled || !a_values)
				return false;
			const auto [enabled, values] = TreeWindPatcher::GetUniversalOverride();
			*a_enabled = enabled;
			*a_values = TreeValues(values);
			return true;
		});
	}

	bool SaveRules(void*, bool a_grass, void* a_visitor, bool (*a_visit)(void*, const OpenWind::AuthoringSaveResult*) noexcept) noexcept
	{
		return GuardAuthoring([&] {
			if (a_grass || !a_visit)
				return false;
			const auto saved = TreeWindPatcher::SaveRules();
			const OpenWind::AuthoringSaveResult result{ saved.success, saved.savedRuleCount, saved.path.c_str(), saved.error.c_str() };
			return a_visit(a_visitor, &result);
		});
	}

	bool RevertRules(void*, bool a_grass) noexcept
	{
		return GuardAuthoring([&] {
			if (a_grass)
				return false;
			TreeWindPatcher::RevertUnsavedChanges();
			return true;
		});
	}

	bool SetTreeRule(void*, const char* a_mesh, const OpenWind::TreeAuthoringValues* a_values) noexcept
	{
		return GuardAuthoring([&] {
			if (!a_mesh || !a_values)
				return false;
			const auto& v = *a_values;
			return TreeWindPatcher::SetRule(a_mesh, v.bend, v.leafAmbient, v.upperBendRange,
				v.maximumDisplacementPercent, v.trunkGustInfluence, v.leafGustInfluence,
				v.transientWindInfluence, v.leafTransientWindInfluence,
				v.leafTransientFlutterMaximum, v.transientMaximumBendMultiplier);
		});
	}

	bool SetUniversal(void*, bool a_enabled, const OpenWind::TreeAuthoringValues* a_values) noexcept
	{
		return GuardAuthoring([&] {
			TreeWindPatcher::SetUniversalOverride(a_enabled, a_values ? ReadTreeValues(*a_values) : TreeWindPatcher::Sensitivities{});
			return true;
		});
	}

	bool SetTreeBendEnabled(void*, const char* a_mesh, bool a_enabled) noexcept
	{
		return GuardAuthoring([&] { return a_mesh && TreeWindPatcher::SetBendEnabled(a_mesh, a_enabled); });
	}
}

const OpenWind::AuthoringHost& Wind::GetAuthoringHost()
{
	static const OpenWind::AuthoringHost host{
		.VisitRules = VisitRules,
		.VisitPaths = VisitPaths,
		.GetUniversal = GetUniversal,
		.SaveRules = SaveRules,
		.RevertRules = RevertRules,
		.SetTreeRule = SetTreeRule,
		.SetUniversal = SetUniversal,
		.SetTreeBendEnabled = SetTreeBendEnabled
	};
	return host;
}
