#include "Features/LightLimitFix/FormulaHelper.h"

#include <cstring>

#include <exprtk.hpp>

namespace ShadowCasterManager
{
	struct FormulaWrapper
	{
		exprtk::expression<double> expression;
		exprtk::parser<double> parser;
	};

	static double s_formulaParams[kFormulaParam_Max];
	static exprtk::symbol_table<double> s_symbolTable;
	static bool s_formulaInited = false;

	static void InitFormulaSystem()
	{
		if (s_formulaInited)
			return;
		s_formulaInited = true;

		memset(s_formulaParams, 0, sizeof(double) * kFormulaParam_Max);

		for (const auto& v : kFormulaVars)
			s_symbolTable.add_variable(v.name, s_formulaParams[v.index]);
	}

	FormulaHelper::FormulaHelper() :
		_ptr(nullptr) { InitFormulaSystem(); }

	FormulaHelper::~FormulaHelper()
	{
		if (_ptr)
			delete static_cast<FormulaWrapper*>(_ptr);
	}

	bool FormulaHelper::Parse(const std::string& input)
	{
		if (_ptr)
			return false;
		auto* w = new FormulaWrapper();
		w->expression.register_symbol_table(s_symbolTable);
		// Defer the _ptr assignment until compile succeeds. Otherwise a
		// failed compile leaves the helper in a "parsed" state (Calculate
		// would evaluate an uncompiled expression and the early-return
		// guard above would block subsequent Parse retries).
		if (!w->parser.compile(input, w->expression)) {
			delete w;
			return false;
		}
		_ptr = w;
		return true;
	}

	double FormulaHelper::Calculate()
	{
		auto* w = static_cast<FormulaWrapper*>(_ptr);
		return w ? w->expression.value() : 0.0;
	}

	bool FormulaHelper::Reparse(const std::string& input)
	{
		std::string err;
		if (!Validate(input, err))
			return false;
		if (_ptr)
			delete static_cast<FormulaWrapper*>(_ptr);
		_ptr = nullptr;
		return Parse(input);
	}

	bool FormulaHelper::Validate(const std::string& input, std::string& errorOut)
	{
		InitFormulaSystem();
		FormulaWrapper tmp;
		tmp.expression.register_symbol_table(s_symbolTable);
		if (tmp.parser.compile(input, tmp.expression))
			return true;
		if (tmp.parser.error_count() > 0)
			errorOut = tmp.parser.get_error(0).diagnostic;
		else
			errorOut = "Unknown parse error";
		return false;
	}

	void FormulaHelper::SetParam(int32_t index, double value) { s_formulaParams[index] = value; }
	double FormulaHelper::GetParam(int32_t index) { return s_formulaParams[index]; }
}
