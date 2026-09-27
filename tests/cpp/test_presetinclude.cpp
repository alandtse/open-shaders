#include "Features/Effects11/PresetInclude.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <winrt/base.h>

namespace
{
	struct PresetFiles
	{
		std::filesystem::path root = std::filesystem::temp_directory_path() /
		                             std::format("effects11_includes_{}_{}", ::GetCurrentProcessId(), std::chrono::steady_clock::now().time_since_epoch().count());

		PresetFiles() { std::filesystem::create_directories(root / "enbseries"); }
		~PresetFiles()
		{
			std::error_code error;
			std::filesystem::remove_all(root, error);
		}

		void Write(const std::filesystem::path& a_name, std::string_view a_source) const
		{
			const auto path = root / "enbseries" / a_name;
			std::filesystem::create_directories(path.parent_path());
			std::ofstream file(path, std::ios::binary);
			file << a_source;
			REQUIRE(file.good());
		}
	};

	struct Compilation
	{
		HRESULT result;
		winrt::com_ptr<ID3DBlob> code;
		winrt::com_ptr<ID3DBlob> messages;

		std::string Messages() const
		{
			return messages ? std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize()) : "";
		}
	};

	Compilation Compile(const std::filesystem::path& a_file, std::string_view a_source, ID3DInclude* a_includes)
	{
		Compilation compilation;
		compilation.result = D3DCompile(a_source.data(), a_source.size(), a_file.string().c_str(), nullptr, a_includes,
			nullptr, "fx_5_0", 0, 0, compilation.code.put(), compilation.messages.put());
		return compilation;
	}

	constexpr std::string_view effect = R"(
float4 PS() : SV_Target { return 1.0; }
technique11 Test { pass P0 { SetPixelShader(CompileShader(ps_5_0, PS())); } }
)";
}

TEST_CASE("Absent unused preset helpers preserve compiled effect bytecode", "[effects11]")
{
	PresetFiles files;
	const auto path = files.root / "enbseries/enbeffectpostpass.fx";
	const auto source = std::string("#include \"/Modular Shaders/msHelpers.fxh\"\n") + std::string(effect);
	const auto original = Compile(path, source, D3D_COMPILE_STANDARD_FILE_INCLUDE);
	REQUIRE(FAILED(original.result));
	REQUIRE(original.Messages().find("X1507") != std::string::npos);

	EffectSourceCompatibility::PresetInclude includes(path.parent_path());
	const auto compatible = Compile(path, source, &includes);
	INFO(compatible.Messages());
	REQUIRE(SUCCEEDED(compatible.result));
	REQUIRE(includes.GetMissingIncludes() == std::vector<std::string>{ "/Modular Shaders/msHelpers.fxh" });
	const auto expected = Compile(path, effect, D3D_COMPILE_STANDARD_FILE_INCLUDE);
	REQUIRE(SUCCEEDED(expected.result));
	REQUIRE(compatible.code->GetBufferSize() == expected.code->GetBufferSize());
	CHECK(std::memcmp(compatible.code->GetBufferPointer(), expected.code->GetBufferPointer(), expected.code->GetBufferSize()) == 0);
}

TEST_CASE("Missing preset definitions still fail compilation", "[effects11]")
{
	PresetFiles files;
	EffectSourceCompatibility::PresetInclude includes(files.root / "enbseries");
	const auto compiled = Compile(files.root / "enbseries/enbeffect.fx",
		"#include \"missing.fxh\"\nfloat value = MissingHelper();\n" + std::string(effect), &includes);
	REQUIRE(FAILED(compiled.result));
	CHECK(compiled.Messages().find("MissingHelper") != std::string::npos);
}

TEST_CASE("Plain text preset libraries retain root and nested include definitions", "[effects11]")
{
	PresetFiles files;
	files.Write("Library/Entry.fxh", "#include \"Values.fxh\"\n#include \"/Root.fxh\"\n#include \"\\Root.fxh\"\n");
	files.Write("Library/Values.fxh", "#define LIBRARY_VALUE 7\n");
	files.Write("Library/Later.fxh", "#define LATER_VALUE 11\n");
	files.Write("Root.fxh", "#define ROOT_VALUE 3\n");
	files.Write("Values.fxh", "#error Wrong parent directory\n");
	files.Write("Library/Root.fxh", "#error Wrong preset root\n");
	files.Write("Empty.fxh", "");
	const auto source = std::string(R"(
#include "/Library/Entry.fxh"
#include "Later.fxh"
#include <Empty.fxh>
#include "Empty.fxh"
#if LIBRARY_VALUE != 7 || ROOT_VALUE != 3 || LATER_VALUE != 11
#error Lost preset definitions
#endif
)") + std::string(effect);
	files.Write("enbeffect.fx", source);
	const auto path = files.root / "enbseries/enbeffect.fx";

	SECTION("Memory compilation")
	{
		EffectSourceCompatibility::PresetInclude includes(path.parent_path());
		const auto compiled = Compile(path, source, &includes);
		INFO(compiled.Messages());
		REQUIRE(SUCCEEDED(compiled.result));
		CHECK(includes.GetMissingIncludes().empty());
	}
	SECTION("File compilation")
	{
		EffectSourceCompatibility::PresetInclude includes(path.parent_path());
		Compilation compiled;
		compiled.result = D3DCompileFromFile(path.c_str(), nullptr, &includes, nullptr, "fx_5_0", 0, 0,
			compiled.code.put(), compiled.messages.put());
		INFO(compiled.Messages());
		REQUIRE(SUCCEEDED(compiled.result));
		CHECK(includes.GetMissingIncludes().empty());
	}
}

TEST_CASE("Preset includes accept parent-relative paths within the preset", "[effects11]")
{
	PresetFiles files;
	files.Write("Library/Entry.fxh", "#include \"../Shared.fxh\"\n");
	files.Write("Shared.fxh", "#define SHARED_VALUE 17\n");
	const auto source = std::string(R"(
#include "Library/Entry.fxh"
#include "../ENBSERIES/Shared.fxh"
#if SHARED_VALUE != 17
#error Lost shared definition
#endif
)") + std::string(effect);
	const auto path = files.root / "enbseries/enbeffect.fx";
	EffectSourceCompatibility::PresetInclude includes(path.parent_path());
	const auto compiled = Compile(path, source, &includes);
	INFO(compiled.Messages());
	REQUIRE(SUCCEEDED(compiled.result));
	CHECK(includes.GetMissingIncludes().empty());
}

TEST_CASE("Unreadable preset includes and paths outside the preset remain errors", "[effects11]")
{
	PresetFiles files;
	files.Write("Locked.fxh", "#define VALUE 1\n");
	winrt::file_handle locked(::CreateFileW((files.root / "enbseries/Locked.fxh").c_str(), GENERIC_READ, 0,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	REQUIRE(static_cast<bool>(locked));
	EffectSourceCompatibility::PresetInclude includes(files.root / "enbseries");
	for (const auto* name : { "Locked.fxh", "../outside.fxh", "C:/outside.fxh" }) {
		INFO(name);
		LPCVOID data = nullptr;
		UINT bytes = 0;
		CHECK(FAILED(includes.Open(D3D_INCLUDE_LOCAL, name, nullptr, &data, &bytes)));
		CHECK(data == nullptr);
		CHECK(bytes == 0);
	}
	CHECK(includes.GetMissingIncludes().empty());
}

TEST_CASE("Preset reload reads newly installed helpers", "[effects11]")
{
	PresetFiles files;
	const auto path = files.root / "enbseries/enbeffect.fx";
	const auto source = std::string("#include \"Optional.fxh\"\n#include \"Optional.fxh\"\n") + std::string(effect);
	{
		EffectSourceCompatibility::PresetInclude includes(path.parent_path());
		REQUIRE(SUCCEEDED(Compile(path, source, &includes).result));
		CHECK(includes.GetMissingIncludes().size() == 1);
	}
	files.Write("Optional.fxh", "#error Newly installed helper was read\n");
	EffectSourceCompatibility::PresetInclude includes(path.parent_path());
	const auto reloaded = Compile(path, source, &includes);
	REQUIRE(FAILED(reloaded.result));
	CHECK(reloaded.Messages().find("Newly installed helper was read") != std::string::npos);
	CHECK(includes.GetMissingIncludes().empty());
}
