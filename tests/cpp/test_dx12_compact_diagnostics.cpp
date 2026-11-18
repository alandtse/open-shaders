#include "Features/DX12Resources/CompactDiagnosticRetention.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <format>

namespace fs = std::filesystem;

namespace
{
	struct TestDirectory
	{
		fs::path root;
		TestDirectory()
		{
			static std::atomic<uint64_t> sequence{ 0 };
			root = fs::temp_directory_path() / std::format("dx12-compact-diag-{}-{}",
				std::chrono::steady_clock::now().time_since_epoch().count(), sequence++);
			fs::create_directories(root);
		}
		~TestDirectory()
		{
			std::error_code ignored;
			fs::remove_all(root, ignored);
		}
	};

	void WriteReport(const fs::path& path, std::string_view value, int ageSeconds)
	{
		fs::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary);
		output.write(value.data(), static_cast<std::streamsize>(value.size()));
		output.close();
		fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::seconds(ageSeconds));
	}
}

TEST_CASE("DX12 compact diagnostics accept only bounded CELL report names", "[dx12][diagnostics]")
{
	using DX12ResourceService::IsCompactDiagnosticFile;
	CHECK(IsCompactDiagnosticFile("0000AF.json"));
	CHECK_FALSE(IsCompactDiagnosticFile("0000af.json"));
	CHECK_FALSE(IsCompactDiagnosticFile("0000AF.json.old"));
	CHECK_FALSE(IsCompactDiagnosticFile("cell-meshes.json"));
	CHECK_FALSE(IsCompactDiagnosticFile("../diagnostics.json"));
}

TEST_CASE("DX12 compact diagnostics prune oldest reports across plugins", "[dx12][diagnostics]")
{
	TestDirectory temporary;
	const auto old = temporary.root / "cells/Test.esm/000001.json";
	const auto middle = temporary.root / "worldspaces/World.esm/000002.json";
	const auto recent = temporary.root / "cells/Test.esm/000003.json";
	const auto historical = temporary.root / "cells/Test.esm/000001-12345/cell-meshes.json";
	WriteReport(old, "111111", 30);
	WriteReport(middle, "222222", 20);
	WriteReport(recent, "333333", 10);
	WriteReport(historical, "historical", 40);

	CHECK(DX12ResourceService::PruneCompactDiagnostics(temporary.root, 12, 2) == 1);
	CHECK_FALSE(fs::exists(old));
	CHECK(fs::exists(middle));
	CHECK(fs::exists(recent));
	CHECK(fs::exists(historical));

	CHECK(DX12ResourceService::PruneCompactDiagnostics(temporary.root, 6, 3) == 1);
	CHECK_FALSE(fs::exists(middle));
	CHECK(fs::exists(recent));
}

TEST_CASE("DX12 compact diagnostics do not traverse redirected roots", "[dx12][diagnostics]")
{
	TestDirectory temporary;
	const auto report = temporary.root / "cells/Test.esm/000001.json";
	WriteReport(report, "report", 0);
	CHECK(DX12ResourceService::PruneCompactDiagnostics(temporary.root / "not-a-directory", 0, 0) == 0);
	CHECK(fs::exists(report));
}

TEST_CASE("DX12 companion logs are bounded without removing unrelated files", "[dx12][diagnostics]")
{
	TestDirectory temporary;
	const auto oldest = temporary.root / "logs/prepare-1.log";
	const auto next = temporary.root / "logs/interiors-2.log";
	const auto newest = temporary.root / "logs/prepare-3.log";
	const auto other = temporary.root / "logs/user-note.log";
	WriteReport(oldest, "one", 30);
	WriteReport(next, "two", 20);
	WriteReport(newest, "three", 10);
	WriteReport(other, "preserve", 100);
	CHECK(DX12ResourceService::PruneGeneratedJobFiles(temporary.root / "logs", 6, 2) == 1);
	CHECK_FALSE(fs::exists(oldest));
	CHECK(fs::exists(next));
	CHECK(fs::exists(newest));
	CHECK(fs::exists(other));
}
