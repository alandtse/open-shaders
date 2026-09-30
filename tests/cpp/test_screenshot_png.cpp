#include "Features/Screenshot/SequenceImage.h"
#include "Features/Screenshot/Service.h"
#include "Features/Screenshot/Storage.h"
#include "Utils/FileDigest.h"
#include <Windows.h>
#include <catch2/catch_test_macros.hpp>

namespace
{
	struct PngTestResources
	{
		HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		std::filesystem::path path = std::filesystem::temp_directory_path() / ("os-png-" + OS::Capture::ServiceFoundation::NewId());
		~PngTestResources()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
			if (SUCCEEDED(comResult))
				CoUninitialize();
		}
	};

	void CheckDecodedPixels(const std::filesystem::path& path, const DirectX::Image& expected)
	{
		DirectX::ScratchImage decoded;
		REQUIRE(SUCCEEDED(DirectX::LoadFromWICFile(path.c_str(), DirectX::WIC_FLAGS_FORCE_RGB, nullptr, decoded)));
		const auto* actual = decoded.GetImage(0, 0, 0);
		REQUIRE(actual);
		REQUIRE(actual->width == expected.width);
		REQUIRE(actual->height == expected.height);
		REQUIRE((actual->format == DXGI_FORMAT_R8G8B8A8_UNORM || actual->format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
		for (size_t row = 0; row < expected.height; ++row)
			CHECK(std::memcmp(actual->pixels + row * actual->rowPitch, expected.pixels + row * expected.rowPitch, expected.width * 4) == 0);
	}
}

TEST_CASE("Native PNGs survive Unicode publication and decode without pixel changes", "[screenshot-sequence][png]")
{
	using namespace OS::Capture;
	using namespace OS::CaptureStorage;
	PngTestResources resources;
	REQUIRE(SUCCEEDED(resources.comResult));
	const auto lease = DirectoryLease::CreateExclusive(resources.path, "png-regression");
	std::vector<uint8_t> left(17 * 11 * 4), right(left.size());
	for (size_t i = 0; i < left.size(); i += 4) {
		left[i] = static_cast<uint8_t>(i);
		left[i + 1] = static_cast<uint8_t>(i / 17);
		left[i + 2] = 0;
		left[i + 3] = 255;
		right[i] = 0;
		right[i + 1] = static_cast<uint8_t>(i / 11);
		right[i + 2] = static_cast<uint8_t>(255 - i);
		right[i + 3] = 255;
	}
	const std::array<NativePlane, 2> planes{ NativePlane{ 17, 11, left }, NativePlane{ 17, 11, right } };
	const auto image = CombinePlanes(planes);
	const auto* expected = image.GetImage(0, 0, 0);
	REQUIRE(expected->width == 34);
	for (size_t row = 0; row < 11; ++row) {
		CHECK(std::memcmp(expected->pixels + row * expected->rowPitch, left.data() + row * 17 * 4, 17 * 4) == 0);
		CHECK(std::memcmp(expected->pixels + row * expected->rowPitch + 17 * 4, right.data() + row * 17 * 4, 17 * 4) == 0);
	}
	const auto blob = EncodeImage(*expected, true);
	for (size_t index = 0; index < 64; ++index) {
		const auto path = lease->Path() / (L"st\u00E9r\u00E9o-\u56FE-" + std::to_wstring(index) + std::wstring(index, L'x') + L".png");
		INFO(index);
		lease->VerifyDirectChild(path);
		const auto temporary = path.wstring() + L".writing";
		const auto artifact = CommittedFile::WriteAtomically(temporary, path, blob.GetBufferPointer(), blob.GetBufferSize(), false);
		CHECK(artifact.bytes == blob.GetBufferSize());
		CHECK(Util::FileDigest::Sha256FileHex(path) == artifact.sha256);
		CHECK_FALSE(std::filesystem::exists(temporary));
		CheckDecodedPixels(path, *expected);
	}
	const auto reference = resources.path / L"golden-\u56FE.png";
	lease->VerifyDestinationChild(reference);
	const auto referenceArtifact = CommittedFile::WriteAtomically(reference.wstring() + L".writing", reference, blob.GetBufferPointer(), blob.GetBufferSize(), false);
	CHECK(Util::FileDigest::Sha256FileHex(reference) == referenceArtifact.sha256);
	CheckDecodedPixels(reference, *expected);
	CHECK_THROWS(CommittedFile::WriteAtomically(reference.wstring() + L".writing", reference, blob.GetBufferPointer(), blob.GetBufferSize(), false));
	CheckDecodedPixels(reference, *expected);
	const auto still = lease->Path() / L"still-\u56FE.png";
	REQUIRE(SUCCEEDED(DirectX::SaveToWICFile(*expected, DirectX::WIC_FLAGS_FORCE_SRGB, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), still.c_str())));
	CheckDecodedPixels(still, *expected);
	const auto mono = CombinePlanes(std::span(planes).first(1));
	CHECK(mono.GetMetadata().width == 17);
	CHECK_THROWS(CombinePlanes(std::span(planes).first(0)));
	auto mismatched = planes;
	mismatched[1].width = 16;
	CHECK_THROWS(CombinePlanes(mismatched));
}
