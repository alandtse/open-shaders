#include "ENBExtender.h"
#include "EffectSourceCompatibility.h"
#include "I18n/I18n.h"
#include "SettingValueParser.h"
#include "Utils/ShaderInclude.h"

#include <d3dcompiler.h>
#include <sstream>
#include <unordered_set>

#include "Features/Effects11/ShaderPatches.h"

namespace ENBExtender
{
	static bool IsTruthy(const std::string& s)
	{
		return !s.empty() && s != "0" && s != "false";
	}

	static bool IsIdentChar(char c)
	{
		return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
	}

	static void Trim(std::string& s, const char* chars = " \t")
	{
		s.erase(0, s.find_first_not_of(chars));
		s.erase(s.find_last_not_of(chars) + 1);
	}

	static std::string BuildGroupPath(const std::vector<std::string>& stack)
	{
		std::string path;
		for (size_t i = 0; i < stack.size(); ++i) {
			if (i > 0)
				path += ".";
			path += stack[i];
		}
		return path;
	}

	static std::string ResolveGroup(const std::string& varName, const std::string& explicitGroup,
		const std::vector<std::string>& groupStack, const Effect& effect)
	{
		if (!explicitGroup.empty())
			return explicitGroup;
		if (!groupStack.empty())
			return BuildGroupPath(groupStack);
		auto it = effect.sourceGroupMap.find(varName);
		return (it != effect.sourceGroupMap.end()) ? it->second : std::string{};
	}

	static int GetSourceOrder(const std::string& varName, const Effect& effect)
	{
		auto it = effect.sourceOrderMap.find(varName);
		return (it != effect.sourceOrderMap.end()) ? it->second : INT_MAX;
	}

	static std::string GetStringVariableValue(ID3DX11EffectVariable* variable)
	{
		auto* strVar = variable->AsString();
		if (strVar && strVar->IsValid()) {
			LPCSTR val = nullptr;
			if (SUCCEEDED(strVar->GetString(&val)) && val && val[0] != '\0')
				return val;
		}
		return {};
	}

	int SafeStoi(const std::string& s, int fallback)
	{
		try {
			return std::stoi(s);
		} catch (...) {
			if (!s.empty())
				logger::warn("[ENBExtender] Failed to parse int from '{}'", s);
			return fallback;
		}
	}

	float SafeStof(const std::string& s, float fallback)
	{
		float value;
		if (Effects11Settings::TryParseHlslFloat(s, value))
			return value;
		if (!s.empty())
			logger::warn("[ENBExtender] Failed to parse float from '{}'", s);
		return fallback;
	}

	static Effect::UIWidgetType ParseWidgetType(const std::string& widget)
	{
		std::string lower = widget;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
		if (lower == "dropdown")
			return Effect::UIWidgetType::Dropdown;
		if (lower == "vector")
			return Effect::UIWidgetType::Vector;
		if (lower == "quality")
			return Effect::UIWidgetType::Quality;
		if (lower == "color")
			return Effect::UIWidgetType::Color;
		return Effect::UIWidgetType::Default;
	}

	static std::vector<std::string> ParseDropdownList(const std::string& list)
	{
		std::vector<std::string> items;
		std::stringstream ss(list);
		std::string item;
		while (std::getline(ss, item, ',')) {
			Trim(item);
			items.push_back(item);
		}
		return items;
	}

	static std::string ExtractAnnotation(const std::string& annotations, const std::string& name)
	{
		for (size_t pos = 0;;) {
			pos = annotations.find(name, pos);
			if (pos == std::string::npos)
				return {};
			size_t end = pos + name.size();
			if ((pos > 0 && IsIdentChar(annotations[pos - 1])) ||
				(end < annotations.size() && IsIdentChar(annotations[end]))) {
				pos = end;
				continue;
			}
			size_t eq = annotations.find('=', end);
			if (eq == std::string::npos)
				return {};
			size_t vs = annotations.find_first_not_of(" \t", eq + 1);
			if (vs == std::string::npos)
				return {};
			if (annotations[vs] == '"') {
				std::string combined;
				size_t cur = vs;
				while (cur < annotations.size() && annotations[cur] == '"') {
					size_t qe = annotations.find('"', cur + 1);
					if (qe == std::string::npos)
						return combined;
					combined += annotations.substr(cur + 1, qe - cur - 1);
					cur = qe + 1;
					while (cur < annotations.size() && (annotations[cur] == ' ' || annotations[cur] == '\t'))
						++cur;
				}
				return combined;
			}
			size_t ve = annotations.find_first_of(";>", vs);
			std::string val = (ve != std::string::npos) ? annotations.substr(vs, ve - vs) : annotations.substr(vs);
			Trim(val);
			return val;
		}
	}

	// ConvertExtenderSyntax

	static bool HandlePragmaUIDefine(std::string& line, std::istringstream& stream,
		const std::string& iniPath, const std::string& iniSection, std::string& result, std::vector<Effect::UIDefineInfo>& uiDefines)
	{
		size_t pragmaPos = line.find("pragma");
		if (pragmaPos == std::string::npos)
			return false;
		size_t uidefPos = line.find("uidefine", pragmaPos + 6);
		if (uidefPos == std::string::npos)
			return false;

		for (auto t = line.find_last_not_of(" \t\r");
			t != std::string::npos && line[t] == '\\';
			t = line.find_last_not_of(" \t\r")) {
			line.erase(t);
			std::string next;
			if (!std::getline(stream, next))
				break;
			line += next;
		}

		size_t openParen = line.find('(', uidefPos);
		size_t closeParen = line.rfind(')');
		if (openParen == std::string::npos || closeParen == std::string::npos || closeParen <= openParen)
			return false;

		std::string inner = line.substr(openParen + 1, closeParen - openParen - 1);

		size_t typeEnd = inner.find_first_of(" \t");
		if (typeEnd == std::string::npos)
			return false;
		std::string typeName = inner.substr(0, typeEnd);
		Trim(typeName);

		size_t nameStart = inner.find_first_not_of(" \t", typeEnd);
		if (nameStart == std::string::npos)
			return false;
		size_t nameEnd = inner.find_first_of("<=", nameStart);
		std::string defineName = (nameEnd != std::string::npos) ? inner.substr(nameStart, nameEnd - nameStart) : inner.substr(nameStart);
		Trim(defineName);

		std::string annotations;
		size_t angleOpen = inner.find('<', nameStart);
		size_t angleClose = inner.rfind('>');
		if (angleOpen != std::string::npos && angleClose != std::string::npos && angleClose > angleOpen)
			annotations = inner.substr(angleOpen + 1, angleClose - angleOpen - 1);

		size_t equalsPos = (angleClose != std::string::npos) ? inner.find('=', angleClose) : inner.rfind('=');
		std::string defaultVal = "0";
		if (equalsPos != std::string::npos) {
			defaultVal = inner.substr(equalsPos + 1);
			Trim(defaultVal, " \t;");
			if (defaultVal == "false")
				defaultVal = "0";
			else if (defaultVal == "true")
				defaultVal = "1";
		}

		auto ann = [&](const char* name) { return ExtractAnnotation(annotations, name); };
		std::string uiName = ann("UIName");
		std::string uiGroup = ann("UIGroup");

		std::string finalVal = defaultVal;
		if (!iniPath.empty() && !iniSection.empty() && !uiName.empty()) {
			std::string iniKey = ann("UniqueName");
			if (iniKey.empty())
				iniKey = uiGroup.empty() ? uiName : (uiGroup + "." + uiName);
			char buf[1024];
			if (GetPrivateProfileStringA(iniSection.c_str(), iniKey.c_str(), "", buf, sizeof(buf), iniPath.c_str()) > 0) {
				std::string iniVal(buf);
				Trim(iniVal);
				if (iniVal == "false")
					iniVal = "0";
				else if (iniVal == "true")
					iniVal = "1";
				finalVal = iniVal;
			}
		}

		if (!uiName.empty()) {
			bool alreadyExists = std::any_of(uiDefines.begin(), uiDefines.end(),
				[&](const Effect::UIDefineInfo& existing) { return existing.defineName == defineName; });
			if (!alreadyExists) {
				bool isInt = (typeName == "int");
				Effect::UIDefineInfo info;
				info.defineName = defineName;
				info.displayName = uiName;
				info.group = uiGroup;
				info.type = typeName;
				info.value = finalVal;
				info.widget = ann("UIWidget");
				info.list = ann("UIList");

				auto minStr = ann("UIMin"), maxStr = ann("UIMax");
				if (!minStr.empty()) {
					if (isInt)
						info.intMin = SafeStoi(minStr);
					else
						info.floatMin = SafeStof(minStr);
				}
				if (!maxStr.empty()) {
					if (isInt)
						info.intMax = SafeStoi(maxStr);
					else
						info.floatMax = SafeStof(maxStr);
				}
				auto orderStr = ann("UIOrdering");
				if (!orderStr.empty()) {
					info.ordering = SafeStoi(orderStr);
					info.hasExplicitOrdering = true;
				}
				info.annotations = annotations;
				uiDefines.push_back(std::move(info));
			}
		}

		result += "#define " + defineName + " " + finalVal + "\n";
		result += "string __uidef_" + defineName + ";\n";
		return true;
	}

	void ConvertExtenderSyntax(std::string& content, const std::filesystem::path& enbseriesPath, std::vector<Effect::UIDefineInfo>& uiDefines, const std::string& iniPath, const std::string& iniSection)
	{
		std::string result;
		result.reserve(content.size());
		result += "#define ENB_EXT_VER 1\n";
		std::istringstream stream(content);
		std::string line;

		while (std::getline(stream, line)) {
			size_t pragmaPos = line.find("#pragma");
			if (pragmaPos != std::string::npos) {
				size_t existsPos = line.find("exists", pragmaPos + 7);
				if (existsPos != std::string::npos) {
					size_t op = line.find('(', existsPos), cp = line.rfind(')');
					if (op != std::string::npos && cp != std::string::npos && cp > op) {
						std::string args = line.substr(op + 1, cp - op - 1);
						size_t q1 = args.find('"');
						size_t q2 = (q1 != std::string::npos) ? args.find('"', q1 + 1) : std::string::npos;
						size_t comma = (q2 != std::string::npos) ? args.find(',', q2 + 1) : std::string::npos;
						if (q1 != std::string::npos && q2 != std::string::npos && comma != std::string::npos) {
							std::string defName = args.substr(comma + 1);
							Trim(defName);
							bool exists = std::filesystem::exists(enbseriesPath / args.substr(q1 + 1, q2 - q1 - 1));
							result += "#define " + defName + (exists ? " 1" : " 0") + "\n";
							continue;
						}
					}
				}
			}

			if (HandlePragmaUIDefine(line, stream, iniPath, iniSection, result, uiDefines))
				continue;
			result += line + "\n";
		}

		content = std::move(result);
	}

	// D3DPreprocess leaves stringification tokens unexpanded, so they need a separate conversion.

	static std::optional<std::string> ParseStringifyDefine(const std::string& line)
	{
		std::string trimmed = line;
		if (!trimmed.empty() && trimmed.back() == '\r')
			trimmed.pop_back();

		auto hash = trimmed.find_first_not_of(" \t");
		if (hash == std::string::npos || trimmed[hash] != '#')
			return std::nullopt;

		auto keyword = trimmed.find_first_not_of(" \t", hash + 1);
		if (keyword == std::string::npos || trimmed.compare(keyword, 6, "define") != 0)
			return std::nullopt;

		auto nameStart = trimmed.find_first_not_of(" \t", keyword + 6);
		if (nameStart == std::string::npos)
			return std::nullopt;

		auto nameEnd = trimmed.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", nameStart);
		if (nameEnd == std::string::npos || nameEnd == nameStart)
			return std::nullopt;

		auto openParen = trimmed.find('(', nameEnd);
		if (openParen == std::string::npos || trimmed.find_first_not_of(" \t", nameEnd) != openParen)
			return std::nullopt;

		auto closeParen = trimmed.find(')', openParen);
		if (closeParen == std::string::npos)
			return std::nullopt;

		std::string param = trimmed.substr(openParen + 1, closeParen - openParen - 1);
		Trim(param);

		auto bodyStart = trimmed.find_first_not_of(" \t", closeParen + 1);
		if (bodyStart == std::string::npos || trimmed[bodyStart] != '#')
			return std::nullopt;

		std::string body = trimmed.substr(bodyStart + 1);
		Trim(body, " \t\r");

		if (body != param)
			return std::nullopt;

		return trimmed.substr(nameStart, nameEnd - nameStart);
	}

	static size_t SkipSourceLiteralOrComment(std::string_view source, size_t pos)
	{
		const char quote = source[pos];
		if (quote == '"' || quote == '\'') {
			for (size_t end = pos + 1; end < source.size(); ++end) {
				if (source[end] == '\\')
					++end;
				else if (source[end] == quote)
					return end + 1;
			}
			return source.size();
		}
		if (source.substr(pos, 2) == "//") {
			const auto end = source.find('\n', pos + 2);
			return end == std::string_view::npos ? source.size() : end;
		}
		if (source.substr(pos, 2) == "/*") {
			const auto end = source.find("*/", pos + 2);
			return end == std::string_view::npos ? source.size() : end + 2;
		}
		return pos;
	}

	static std::string StringifyArgument(std::string_view argument)
	{
		std::string normalized;
		for (size_t pos = 0; pos < argument.size();) {
			const auto end = SkipSourceLiteralOrComment(argument, pos);
			const bool comment = end != pos && argument[pos] == '/';
			if (comment || std::isspace(static_cast<unsigned char>(argument[pos]))) {
				if (!normalized.empty() && normalized.back() != ' ')
					normalized += ' ';
				pos = comment ? end : pos + 1;
			} else if (end != pos) {
				normalized.append(argument.substr(pos, end - pos));
				pos = end;
			} else {
				normalized += argument[pos++];
			}
		}
		Trim(normalized);

		std::string result = "\"";
		for (size_t pos = 0; pos < normalized.size();) {
			const auto end = SkipSourceLiteralOrComment(normalized, pos);
			if (end != pos) {
				for (; pos < end; ++pos) {
					if (normalized[pos] == '\\' || normalized[pos] == '"')
						result += '\\';
					result += normalized[pos];
				}
			} else if (normalized.compare(pos, 3, "| -") == 0) {
				result += "|-";
				pos += 3;
			} else if (normalized.compare(pos, 2, " .") == 0) {
				++pos;
			} else if (normalized.compare(pos, 2, ". ") == 0) {
				result += '.';
				pos += 2;
			} else {
				result += normalized[pos++];
			}
		}
		return result + '"';
	}

	static std::string ReplaceStringifyInvocations(const std::string& source, const std::string& macroName)
	{
		std::string result;
		result.reserve(source.size());
		size_t pos = 0;
		while (pos < source.size()) {
			const auto tokenEnd = SkipSourceLiteralOrComment(source, pos);
			if (tokenEnd != pos) {
				result.append(source, pos, tokenEnd - pos);
				pos = tokenEnd;
				continue;
			}
			if (!IsIdentChar(source[pos])) {
				result += source[pos++];
				continue;
			}
			size_t nameEnd = pos + 1;
			while (nameEnd < source.size() && IsIdentChar(source[nameEnd]))
				++nameEnd;
			size_t afterName = nameEnd;
			while (afterName < source.size() && std::isspace(static_cast<unsigned char>(source[afterName])))
				++afterName;
			if (source.compare(pos, nameEnd - pos, macroName) != 0 || afterName == source.size() || source[afterName] != '(') {
				result.append(source, pos, nameEnd - pos);
				pos = nameEnd;
				continue;
			}

			int depth = 1;
			size_t argEnd = afterName + 1;
			while (argEnd < source.size() && depth > 0) {
				const auto end = SkipSourceLiteralOrComment(source, argEnd);
				if (end != argEnd) {
					argEnd = end;
					continue;
				}
				if (source[argEnd] == '(')
					++depth;
				else if (source[argEnd] == ')')
					--depth;
				++argEnd;
			}
			if (depth != 0) {
				result.append(source, pos);
				break;
			}
			result += StringifyArgument(std::string_view(source).substr(afterName + 1, argEnd - afterName - 2));
			pos = argEnd;
		}
		return result;
	}

	void StripStringifyDefines(std::string& source, std::vector<std::string>& macroNames)
	{
		std::string stripped;
		stripped.reserve(source.size());

		std::istringstream stream(source);
		std::string line;
		size_t lineStart = 0, scanPos = 0;
		while (std::getline(stream, line)) {
			bool directiveIsCode = false;
			const auto first = line.find_first_not_of(" \t\r");
			if (first != std::string::npos) {
				const auto directivePos = lineStart + first;
				while (scanPos <= directivePos) {
					const auto end = SkipSourceLiteralOrComment(source, scanPos);
					if (end != scanPos) {
						scanPos = end;
					} else {
						directiveIsCode = scanPos == directivePos;
						++scanPos;
					}
				}
			}
			if (auto name = directiveIsCode ? ParseStringifyDefine(line) : std::nullopt) {
				macroNames.push_back(*name);
				stripped += "\n";
			} else {
				stripped += line + "\n";
			}
			lineStart += line.size() + 1;
		}

		source = std::move(stripped);
	}

	void ExpandStringifyMacros(std::string& source, const std::vector<std::string>& macroNames)
	{
		for (auto& name : macroNames) {
			logger::debug("[ENBEXTENDER] Expanding stringification macro: {}", name);
			source = ReplaceStringifyInvocations(source, name);
		}
	}

	// Source-based group scoping (compiled effect reorders variable types, so source text is the ground truth for declaration order)

	static bool IsHLSLType(const std::string& s)
	{
		static const std::unordered_set<std::string> types = { "float", "float2", "float3", "float4", "int", "bool", "string" };
		return types.count(s) > 0;
	}

	void ParseSourceGroupScopes(const std::string& preprocessedSource, Effect& effect)
	{
		effect.sourceGroupMap.clear();
		effect.sourceOrderMap.clear();
		std::vector<std::string> groupStack;
		int declarationIndex = 0;

		std::istringstream stream(preprocessedSource);
		std::string line;

		while (std::getline(stream, line)) {
			size_t firstNonSpace = line.find_first_not_of(" \t");
			if (firstNonSpace == std::string::npos || line[firstNonSpace] == '#')
				continue;

			if (line.find("UIGroupBegin") != std::string::npos) {
				std::string groupName;
				size_t gt = line.rfind('>');
				size_t q1 = (gt != std::string::npos) ? line.find('"', gt) : std::string::npos;
				size_t q2 = (q1 != std::string::npos) ? line.find('"', q1 + 1) : std::string::npos;
				if (q2 != std::string::npos)
					groupName = line.substr(q1 + 1, q2 - q1 - 1);
				if (groupName.empty())
					groupName = ExtractAnnotation(line, "UIGroup");
				if (!groupName.empty()) {
					groupStack.push_back(groupName);
					auto groupPath = BuildGroupPath(groupStack);
					auto& gm = effect.groupMeta[groupPath];
					auto orderStr = ExtractAnnotation(line, "UIOrdering");
					if (!orderStr.empty()) {
						gm.ordering = SafeStoi(orderStr);
						gm.hasOrdering = true;
					}
				}
				continue;
			}

			if (line.find("UIGroupEnd") != std::string::npos) {
				if (!groupStack.empty())
					groupStack.pop_back();
				continue;
			}

			std::string trimmed = line.substr(firstNonSpace);
			size_t spacePos = trimmed.find_first_of(" \t");
			if (spacePos == std::string::npos || !IsHLSLType(trimmed.substr(0, spacePos)))
				continue;

			size_t nameStart = trimmed.find_first_not_of(" \t", spacePos);
			if (nameStart == std::string::npos)
				continue;
			size_t nameEnd = nameStart;
			while (nameEnd < trimmed.size() && IsIdentChar(trimmed[nameEnd]))
				nameEnd++;

			if (nameEnd > nameStart) {
				std::string varName = trimmed.substr(nameStart, nameEnd - nameStart);
				if (varName.find("UIGroupBegin") == std::string::npos && varName.find("UIGroupEnd") == std::string::npos) {
					auto [it, inserted] = effect.sourceOrderMap.try_emplace(varName, declarationIndex);
					if (inserted) {
						declarationIndex++;
						if (!groupStack.empty())
							effect.sourceGroupMap[varName] = BuildGroupPath(groupStack);
					}
				}
			}
		}
	}

	// Group metadata tracking

	static void CollectGroupMeta(const std::string& groupPath, ID3DX11EffectVariable* variable, Effect& effect)
	{
		if (groupPath.empty())
			return;
		auto& gm = effect.groupMeta[groupPath];
		auto get = [&](const char* name) { return effect.GetUIAnnotation(variable, name); };
		if (gm.displayName.empty()) {
			auto s = get("UIGroupName");
			if (!s.empty())
				gm.displayName = s;
		}
		if (!gm.defaultOpen) {
			auto s = get("UIGroupOpen");
			if (!s.empty())
				gm.defaultOpen = IsTruthy(s);
		}
		if (!gm.hasOrdering) {
			auto s = get("UIOrdering");
			if (!s.empty()) {
				gm.ordering = SafeStoi(s);
				gm.hasOrdering = true;
			}
		}
		if (!gm.isTopLevel) {
			auto s = get("UITopLevel");
			if (!s.empty())
				gm.isTopLevel = IsTruthy(s);
		}
	}

	// Shared annotation application (file-local, used by CreateUIVariable and ProcessExtenderStringVariable)

	static void ApplyAnnotations(Effect::UIVariable& uiVar, ID3DX11EffectVariable* variable,
		const std::vector<std::string>& groupStack, Effect& effect)
	{
		auto get = [&](const char* name) { return effect.GetUIAnnotation(variable, name); };

		auto explicitGroup = get("UIGroup");
		uiVar.group = ResolveGroup(uiVar.name, explicitGroup, groupStack, effect);
		uiVar.sourceOrder = GetSourceOrder(uiVar.name, effect);

		auto orderStr = get("UIOrdering");
		if (!orderStr.empty())
			uiVar.ordering = SafeStoi(orderStr);

		uiVar.isReadOnly = IsTruthy(get("UIReadOnly"));
		auto hiddenStr = get("UIHidden"), visibleStr = get("UIVisible");
		uiVar.isHidden = IsTruthy(hiddenStr) || (!visibleStr.empty() && !IsTruthy(visibleStr));

		CollectGroupMeta(uiVar.group, variable, effect);
		uiVar.isTopLevel = IsTruthy(get("UITopLevel"));
		if (uiVar.isTopLevel && explicitGroup.empty())
			uiVar.group.clear();

		uiVar.uniqueName = get("UniqueName");

		auto readBindings = [&](const char* prefix, bool inverted) {
			auto target = get(prefix);
			if (!target.empty()) {
				Effect::UIBindingInfo b;
				b.target = target;
				b.file = get("UIBindingFile");
				b.property = get("UIBindingProperty");
				b.condition = get("UIBindingCondition");
				b.inverted = inverted;
				uiVar.uiBindings.push_back(std::move(b));
			}
			for (int i = 0; i < 16; ++i) {
				auto iStr = std::to_string(i);
				target = get((std::string(prefix) + iStr).c_str());
				if (target.empty())
					continue;
				Effect::UIBindingInfo b;
				b.target = target;
				b.file = get(("UIBindingFile" + iStr).c_str());
				b.property = get(("UIBindingProperty" + iStr).c_str());
				b.condition = get(("UIBindingCondition" + iStr).c_str());
				b.inverted = inverted;
				uiVar.uiBindings.push_back(std::move(b));
			}
		};
		readBindings("UIBinding", false);
		readBindings("UIInvBinding", true);

		uiVar.ignorePerfMode = IsTruthy(get("UIIgnorePerfMode"));
		uiVar.isWeatherString = IsTruthy(get("UIWeatherString"));
		uiVar.isWeatherOnlyString = IsTruthy(get("UIWeatherOnlyString"));
		if (uiVar.isWeatherOnlyString)
			uiVar.isWeatherString = true;
		if (uiVar.type == Effect::UIVariableType::Float || uiVar.type == Effect::UIVariableType::Float2 ||
			uiVar.type == Effect::UIVariableType::Float3 || uiVar.type == Effect::UIVariableType::Float4)
			uiVar.separation = get("Separation");
	}

	static void ParseTimePeriod(Effect::UIVariable& uiVar)
	{
		if (uiVar.separation.empty() || uiVar.separation == "None")
			return;
		static const std::string_view periods[] = { "Dawn", "Sunrise", "Day", "Sunset", "Dusk", "Night", "Interior" };
		for (auto period : periods) {
			auto suffix = std::string(period) + " - ";
			if (uiVar.displayName.compare(0, 3 + suffix.size(), "|- " + suffix) == 0 ||
				uiVar.displayName.compare(0, suffix.size(), suffix) == 0) {
				uiVar.timePeriod = period;
				return;
			}
		}
	}

	// CreateUIVariable - consolidated annotation reading for compiled effect variables

	bool CreateUIVariable(Effect::UIVariable& out, ID3DX11EffectVariable* variable,
		const D3DX11_EFFECT_VARIABLE_DESC& varDesc, const D3DX11_EFFECT_TYPE_DESC& typeDesc,
		const std::vector<std::string>& groupStack, Effect& effect)
	{
		auto get = [&](const char* name) { return effect.GetUIAnnotation(variable, name); };

		std::string uiName = get("UIName");
		if (uiName.empty())
			return false;
		if (uiName.find_first_not_of(" \t") != std::string::npos)
			Trim(uiName);

		out = {};
		out.name = varDesc.Name;
		out.displayName = uiName;
		out.effectVariable.copy_from(variable);

		if (typeDesc.Class == D3D_SVC_SCALAR) {
			switch (typeDesc.Type) {
			case D3D_SVT_FLOAT:
				out.type = Effect::UIVariableType::Float;
				break;
			case D3D_SVT_INT:
				out.type = Effect::UIVariableType::Int;
				break;
			case D3D_SVT_BOOL:
				out.type = Effect::UIVariableType::Bool;
				break;
			default:
				return false;
			}
		} else if (typeDesc.Class == D3D_SVC_VECTOR && typeDesc.Type == D3D_SVT_FLOAT && typeDesc.Elements == 0) {
			if (typeDesc.Columns == 2)
				out.type = Effect::UIVariableType::Float2;
			else if (typeDesc.Columns == 3)
				out.type = Effect::UIVariableType::Float3;
			else if (typeDesc.Columns == 4)
				out.type = Effect::UIVariableType::Float4;
			else
				return false;
		} else {
			return false;
		}

		out.widgetType = ParseWidgetType(get("UIWidget"));

		if (out.type == Effect::UIVariableType::Float || out.type == Effect::UIVariableType::Float2 ||
			out.type == Effect::UIVariableType::Float3 || out.type == Effect::UIVariableType::Float4) {
			auto s = get("UIMin");
			if (!s.empty())
				out.floatMin = SafeStof(s, out.floatMin);
			s = get("UIMax");
			if (!s.empty())
				out.floatMax = SafeStof(s, out.floatMax);
		} else if (out.type == Effect::UIVariableType::Int) {
			auto s = get("UIMin");
			if (!s.empty())
				out.intMin = SafeStoi(s, out.intMin);
			s = get("UIMax");
			if (!s.empty())
				out.intMax = SafeStoi(s, out.intMax);
			if (out.widgetType == Effect::UIWidgetType::Dropdown) {
				s = get("UIList");
				if (!s.empty())
					out.dropdownItems = ParseDropdownList(s);
			} else if (out.widgetType == Effect::UIWidgetType::Quality) {
				out.dropdownItems = { T("feature.effects11.quality_very_high", "Very High"), T("feature.effects11.quality_high", "High"), T("feature.effects11.quality_medium", "Medium"), T("feature.effects11.quality_low", "Low"), T("feature.effects11.quality_very_low", "Very Low") };
				out.intMin = -1;
				out.intMax = 3;
			}
		}

		if (out.floatMin > out.floatMax)
			std::swap(out.floatMin, out.floatMax);
		if (out.intMin > out.intMax)
			std::swap(out.intMin, out.intMax);

		ApplyAnnotations(out, variable, groupStack, effect);

		if (!out.isReadOnly) {
			if ((out.type == Effect::UIVariableType::Float || out.type == Effect::UIVariableType::Float2 ||
					out.type == Effect::UIVariableType::Float3 || out.type == Effect::UIVariableType::Float4) &&
				out.floatMin == out.floatMax)
				out.isReadOnly = true;
			else if (out.type == Effect::UIVariableType::Int && out.intMin == out.intMax)
				out.isReadOnly = true;
		}

		ParseTimePeriod(out);
		return true;
	}

	// ProcessExtenderStringVariable

	bool ProcessExtenderStringVariable(ID3DX11EffectVariable* variable, const D3DX11_EFFECT_VARIABLE_DESC& varDesc,
		std::vector<std::string>& groupStack, Effect& effect)
	{
		auto get = [&](const char* name) { return effect.GetUIAnnotation(variable, name); };

		if (IsTruthy(get("UIGroupBegin"))) {
			std::string groupName = GetStringVariableValue(variable);
			if (groupName.empty())
				groupName = get("UIGroup");
			if (!groupName.empty()) {
				groupStack.push_back(groupName);
				CollectGroupMeta(BuildGroupPath(groupStack), variable, effect);
			}
			return true;
		}

		if (IsTruthy(get("UIGroupEnd"))) {
			if (!groupStack.empty())
				groupStack.pop_back();
			return true;
		}

		if (IsTruthy(get("UISeparator"))) {
			Effect::SeparatorInfo sep;
			sep.name = varDesc.Name;
			sep.group = ResolveGroup(varDesc.Name, get("UIGroup"), groupStack, effect);
			sep.sourceOrder = GetSourceOrder(varDesc.Name, effect);
			auto orderStr = get("UIOrdering");
			if (!orderStr.empty()) {
				sep.ordering = SafeStoi(orderStr);
				sep.hasOrdering = true;
			}
			sep.isTopLevel = IsTruthy(get("UITopLevel"));
			effect.separators.push_back(sep);
			return true;
		}

		std::string labelText = get("UIName");
		if (labelText.empty())
			labelText = GetStringVariableValue(variable);
		Trim(labelText);
		if (labelText.empty())
			return true;

		Effect::UIVariable labelVar = {};
		labelVar.name = varDesc.Name;
		labelVar.displayName = labelText;
		labelVar.isLabel = true;
		ApplyAnnotations(labelVar, variable, groupStack, effect);
		if (!labelVar.isHidden)
			effect.uiVariables.push_back(labelVar);
		return true;
	}

	// InsertUIDefines

	void InsertUIDefines(Effect& effect)
	{
		int fallbackOrder = -static_cast<int>(effect.uiDefines.size());
		for (const auto& def : effect.uiDefines) {
			Effect::UIVariable uiVar = {};
			uiVar.name = def.defineName;
			uiVar.displayName = def.displayName;
			uiVar.group = def.group;
			uiVar.ordering = def.ordering;
			uiVar.isDefine = true;

			auto it = effect.sourceOrderMap.find("__uidef_" + def.defineName);
			uiVar.sourceOrder = (it != effect.sourceOrderMap.end()) ? it->second : fallbackOrder++;

			if (def.type == "bool") {
				uiVar.type = Effect::UIVariableType::Bool;
				uiVar.boolValue = (def.value != "0");
			} else if (def.type == "int") {
				uiVar.type = Effect::UIVariableType::Int;
				uiVar.intValue = SafeStoi(def.value);
				uiVar.intMin = def.intMin;
				uiVar.intMax = def.intMax;
				uiVar.widgetType = ParseWidgetType(def.widget);
				if (uiVar.widgetType == Effect::UIWidgetType::Dropdown && !def.list.empty())
					uiVar.dropdownItems = ParseDropdownList(def.list);
				else if (uiVar.widgetType == Effect::UIWidgetType::Quality) {
					uiVar.dropdownItems = { T("feature.effects11.quality_very_high", "Very High"), T("feature.effects11.quality_high", "High"), T("feature.effects11.quality_medium", "Medium"), T("feature.effects11.quality_low", "Low"), T("feature.effects11.quality_very_low", "Very Low") };
					uiVar.intMin = -1;
					uiVar.intMax = 3;
				}
			} else {
				uiVar.type = Effect::UIVariableType::Float;
				uiVar.floatValue = SafeStof(def.value);
				uiVar.floatMin = def.floatMin;
				uiVar.floatMax = def.floatMax;
			}

			if (!def.group.empty() && def.hasExplicitOrdering) {
				auto& gm = effect.groupMeta[def.group];
				if (!gm.hasOrdering) {
					gm.ordering = def.ordering;
					gm.hasOrdering = true;
				}
			}

			if (!def.annotations.empty()) {
				auto ann = [&](const char* name) { return ExtractAnnotation(def.annotations, name); };

				uiVar.isReadOnly = IsTruthy(ann("UIReadOnly"));
				auto hiddenStr = ann("UIHidden"), visibleStr = ann("UIVisible");
				uiVar.isHidden = IsTruthy(hiddenStr) || (!visibleStr.empty() && !IsTruthy(visibleStr));
				uiVar.uniqueName = ann("UniqueName");
				uiVar.isTopLevel = IsTruthy(ann("UITopLevel"));
				uiVar.ignorePerfMode = IsTruthy(ann("UIIgnorePerfMode"));
				uiVar.isWeatherString = IsTruthy(ann("UIWeatherString"));
				uiVar.isWeatherOnlyString = IsTruthy(ann("UIWeatherOnlyString"));
				if (uiVar.isWeatherOnlyString)
					uiVar.isWeatherString = true;
				if (uiVar.isTopLevel && uiVar.group.empty())
					uiVar.group.clear();
				if (uiVar.type == Effect::UIVariableType::Float || uiVar.type == Effect::UIVariableType::Float2 ||
					uiVar.type == Effect::UIVariableType::Float3 || uiVar.type == Effect::UIVariableType::Float4)
					uiVar.separation = ann("Separation");

				auto readDefBindings = [&](const char* prefix, bool inverted) {
					auto target = ann(prefix);
					if (!target.empty()) {
						Effect::UIBindingInfo b;
						b.target = target;
						b.file = ann("UIBindingFile");
						b.property = ann("UIBindingProperty");
						b.condition = ann("UIBindingCondition");
						b.inverted = inverted;
						uiVar.uiBindings.push_back(std::move(b));
					}
					for (int i = 0; i < 16; ++i) {
						auto iStr = std::to_string(i);
						target = ann((std::string(prefix) + iStr).c_str());
						if (target.empty())
							continue;
						Effect::UIBindingInfo b;
						b.target = target;
						b.file = ann(("UIBindingFile" + iStr).c_str());
						b.property = ann(("UIBindingProperty" + iStr).c_str());
						b.condition = ann(("UIBindingCondition" + iStr).c_str());
						b.inverted = inverted;
						uiVar.uiBindings.push_back(std::move(b));
					}
				};
				readDefBindings("UIBinding", false);
				readDefBindings("UIInvBinding", true);
			}

			effect.uiVariables.push_back(uiVar);
		}
	}

	void LoadTechniqueDropdownMetadata(Effect& effect)
	{
		if (!effect.IsCompiled())
			return;
		auto* fx = effect.GetEffect();
		if (!fx)
			return;
		D3DX11_EFFECT_DESC effectDesc;
		if (FAILED(fx->GetDesc(&effectDesc)))
			return;

		ID3DX11EffectGroup* annotationGroup = nullptr;
		ID3DX11EffectTechnique* annotationTechnique = nullptr;

		for (UINT g = 0; g < effectDesc.Groups; ++g) {
			auto* group = fx->GetGroupByIndex(g);
			if (!group || !group->IsValid())
				continue;
			D3DX11_GROUP_DESC groupDesc;
			if (FAILED(group->GetDesc(&groupDesc)))
				continue;

			if (groupDesc.Name && groupDesc.Name[0]) {
				annotationGroup = group;
				break;
			} else if (!annotationTechnique && groupDesc.Techniques > 0) {
				auto* tech = group->GetTechniqueByIndex(0);
				if (tech && tech->IsValid())
					annotationTechnique = tech;
			}
		}

		if (!annotationGroup && !annotationTechnique)
			return;

		auto get = [&](const char* name) -> std::string {
			if (annotationGroup)
				return Effect::GetGroupAnnotation(annotationGroup, name);
			return Effect::GetTechniqueAnnotation(annotationTechnique, name);
		};

		auto str = [&](const char* name, std::string& out) { auto s = get(name); if (!s.empty()) out = s; };
		auto flag = [&](const char* name, bool& out) { auto s = get(name); if (!s.empty()) out = IsTruthy(s); };

		auto& td = effect.techniqueDropdown;
		str("UIDropdownName", td.name);
		str("UIDropdownGroup", td.group);
		str("UIDropdownGroupName", td.groupName);
		flag("UIDropdownGroupOpen", td.groupOpen);
		flag("UIDropdownVisible", td.visible);
		flag("UIDropdownTopLevel", td.topLevel);
		auto s = get("UIDropdownOrdering");
		if (!s.empty())
			td.ordering = SafeStoi(s, td.ordering);
	}

	// File preprocessing

	void StripLineDirectives(std::string& source)
	{
		std::string result;
		result.reserve(source.size());
		std::istringstream stream(source);
		std::string line;
		while (std::getline(stream, line)) {
			size_t firstNonSpace = line.find_first_not_of(" \t");
			if (firstNonSpace != std::string::npos && line[firstNonSpace] == '#') {
				auto directive = line.substr(firstNonSpace + 1);
				size_t dirStart = directive.find_first_not_of(" \t");
				if (dirStart != std::string::npos && directive.compare(dirStart, 4, "line") == 0) {
					result += "\n";
					continue;
				}
			}
			result += line;
			result += "\n";
		}
		source = std::move(result);
	}

	PresetInclude::PresetInclude(const std::filesystem::path& a_basePath, std::vector<Effect::UIDefineInfo>& a_uiDefines, const std::string& a_iniPath, const std::string& a_iniSection) :
		basePath(a_basePath), uiDefines(a_uiDefines), iniPath(a_iniPath), iniSection(a_iniSection), sourceIncludes(a_basePath) {}

	HRESULT PresetInclude::Open(D3D_INCLUDE_TYPE type, LPCSTR fileName, LPCVOID parent, LPCVOID* data, UINT* bytes)
	{
		*data = nullptr;
		*bytes = 0;
		const auto parentIt = openIncludes.find(parent);
		LPCVOID original = nullptr;
		UINT size = 0;
		const auto result = sourceIncludes.Open(type, fileName, parentIt != openIncludes.end() ? parentIt->second.original : parent, &original, &size);
		if (FAILED(result))
			return result;
		try {
			std::string content(static_cast<const char*>(original), size);
			Util::ShaderPatches::Apply(fileName, content);
			EffectSourceCompatibility::PatchInteriorTimeOfDayMacro(content);
			ConvertExtenderSyntax(content, basePath, uiDefines, iniPath, iniSection);
			StripStringifyDefines(content, stringifyMacros);
			if (content.size() > UINT_MAX) {
				sourceIncludes.Close(original);
				return E_OUTOFMEMORY;
			}
			auto buffer = std::make_unique<char[]>(content.size() + 1);
			memcpy(buffer.get(), content.data(), content.size());
			buffer[content.size()] = '\0';
			const auto pointer = buffer.get();
			openIncludes.emplace(pointer, IncludeData{ std::move(buffer), original });
			*data = pointer;
			*bytes = static_cast<UINT>(content.size());
			return S_OK;
		} catch (...) {
			sourceIncludes.Close(original);
			return E_FAIL;
		}
	}

	HRESULT PresetInclude::Close(LPCVOID data)
	{
		const auto it = openIncludes.find(data);
		if (it == openIncludes.end())
			return E_INVALIDARG;
		sourceIncludes.Close(it->second.original);
		openIncludes.erase(it);
		return S_OK;
	}
}
