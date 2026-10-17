#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <source_location>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

struct ID3D11PixelShader
{};
struct ID3D11VertexShader
{};
namespace RE
{
	namespace BSGraphics
	{
		struct Renderer
		{};
		enum
		{
			DIRTY_VERTEX_DESC
		};
		struct VertexShader
		{
			ID3D11VertexShader* shader = nullptr;
		};
		struct PixelShader
		{
			ID3D11PixelShader* shader = nullptr;
			uint32_t id = 0;
		};
	}
	struct BSShader
	{
		enum class Type
		{
			Lighting,
			Grass,
			DistantTree,
			Sky,
			Effect,
			ImageSpace,
			Total
		};
		struct TypeValue
		{
			Type value = Type::Lighting;
			Type get() const { return value; }
			bool operator==(Type other) const { return value == other; }
			unsigned underlying() const { return static_cast<unsigned>(value); }
		} shaderType;
		const char* fxpFilename = "Lighting";
	};
}
namespace REL
{
	template <class T>
	struct Relocation
	{};
}
namespace logger
{
	template <class... T>
	void debug(const char*, T&&...)
	{}
}
struct TestState
{
	bool developer = false, enablePShaders = true, settingCustomShader = false;
	bool deferredPixelShaderMissing = false;
	RE::BSShader* currentShader = nullptr;
	uint32_t modifiedPixelDescriptor = 0;
	uint32_t currentVertexDescriptor = 0, currentPixelDescriptor = 0, modifiedVertexDescriptor = 0;
	uint32_t lastModifiedVertexDescriptor = 0, lastModifiedPixelDescriptor = 0;
	bool updateShader = false;
	struct
	{
		uint32_t VertexShaderDescriptor = 0, PixelShaderDescriptor = 0;
	} permutationData;
	void ModifyShaderLookup(const RE::BSShader&, uint32_t&, uint32_t&) {}
	bool IsDeveloperMode() const { return developer; }
	bool IsShaderEnabled(const RE::BSShader&) const { return true; }
	bool ShaderEnabled(RE::BSShader::Type) const { return true; }
};
struct TestDeferred
{
	bool deferredPass = true, categories = true, captureReady = true;
	unsigned invalidations = 0;
	static bool MaterialCategoriesRequested();
	bool IsMaterialCategoriesEnabled() const { return categories; }
	void InvalidateMaterialCategories()
	{
		captureReady = false;
		++invalidations;
	}
};
struct TestContext
{
	ID3D11PixelShader* bound = nullptr;
	unsigned binds = 0;
	ID3D11VertexShader* vertex = nullptr;
	void VSSetShader(ID3D11VertexShader* shader, void*, unsigned) { vertex = shader; }
	void PSSetShader(ID3D11PixelShader* shader, void*, unsigned)
	{
		bound = shader;
		++binds;
	}
};
namespace SIE
{
	struct ShaderCache;
}
namespace globals
{
	TestState* state;
	TestDeferred* deferred;
	SIE::ShaderCache* shaderCache;
	namespace game
	{
		bool isVR = true;
		RE::BSGraphics::PixelShader** currentPixelShader;
		RE::BSGraphics::VertexShader* vertex = nullptr;
		RE::BSGraphics::VertexShader** currentVertexShader = &vertex;
		struct Flags
		{
			void set(int) {}
		} flags;
		auto* stateUpdateFlags = &flags;
	}
	namespace d3d
	{
		TestContext* context;
	}
}
bool TestDeferred::MaterialCategoriesRequested() { return globals::deferred->categories; }
using Deferred = TestDeferred;
namespace Util
{
	template <class T>
	T* AsReal(T* shader)
	{
		return shader;
	}
}
struct Feature
{
	void OnPixelShaderFallback(RE::BSShader::Type) {}
	template <class Callback>
	static void ForEachLoadedFeature(const char*, Callback callback)
	{
		Feature feature;
		callback(&feature);
	}
};
void NormalizeLegacyUtilityDescriptors(const RE::BSShader&, uint32_t&, uint32_t&) {}

namespace SIE
{
	enum class ShaderClass
	{
		Pixel
	};
	namespace SShaderCache
	{
		bool ResolveImageSpaceDescriptor(const RE::BSShader&, uint32_t&) { return true; }
		std::string GetShaderString(ShaderClass, const RE::BSShader&, uint32_t descriptor, bool)
		{
			return std::to_string(descriptor);
		}
	}
	struct ShaderCache
	{
#include "deferred_flag_under_test.h"
#include "lighting_flags_under_test.h"
#include "pixel_lookup_mode.h"
		std::array<std::unordered_map<uint32_t, std::unique_ptr<RE::BSGraphics::PixelShader>>,
			static_cast<unsigned>(RE::BSShader::Type::Total)>
			pixelShaders;
		std::mutex pixelShadersMutex;
		int blockedKeyIndex = -1;
		std::string blockedKey;
		std::vector<uint32_t> blockedIDs;
		bool async = true, compilationSucceeds = true;
		unsigned compiles = 0;
		RE::BSGraphics::VertexShader* vertex = nullptr;
		auto* GetVertexShader(const RE::BSShader&, uint32_t) { return vertex; }
		struct Task
		{
			ShaderClass stage;
			const RE::BSShader& shader;
			uint32_t descriptor;
		};
		struct Queue
		{
			std::set<uint32_t> requests;
			void Add(Task task) { requests.insert(task.descriptor); }
		} compilationSet;
		static bool IsSupportedShader(const RE::BSShader&) { return true; }
		bool IsEnabled() const { return true; }
		bool IsTrackingActiveShaders() const { return false; }
		bool IsAsync() const { return async; }
		void TrackActiveShader(ShaderClass, const RE::BSShader&, uint32_t) {}
		RE::BSGraphics::PixelShader* Add(const RE::BSShader& shader, uint32_t descriptor)
		{
			auto& entry = pixelShaders[shader.shaderType.underlying()][descriptor];
			entry = std::make_unique<RE::BSGraphics::PixelShader>();
			entry->id = descriptor;
			entry->shader = reinterpret_cast<ID3D11PixelShader*>(entry.get());
			return entry.get();
		}
		RE::BSGraphics::PixelShader* MakeAndAddPixelShader(const RE::BSShader& shader, uint32_t descriptor)
		{
			++compiles;
			return compilationSucceeds ? Add(shader, descriptor) : nullptr;
		}
		void PrewarmDeferredPixelShaders(const RE::BSShader&, uint32_t, bool);
		RE::BSGraphics::PixelShader* GetPixelShader(const RE::BSShader&, uint32_t, PixelShaderLookup = PixelShaderLookup::Compile);
	};
#include "deferred_prewarm_under_test.h"
#include "pixel_lookup.h"
}
namespace Permutations
{
	std::vector<uint32_t> pixelPermutations;
	const auto& GeneratePBRLightingPixelPermutations() { return pixelPermutations; }
}
struct TruePBR
{
	void GenerateShaderPermutations(RE::BSShader*);
};
#include "pbr_prewarm_under_test.h"
namespace Hooks
{
#include "pixel_binding.h"
	struct BSShader_BeginTechnique
	{
		static bool thunk(RE::BSShader*, uint32_t, uint32_t, bool);
		static inline bool (*func)(RE::BSShader*, uint32_t, uint32_t, bool) = nullptr;
	};
}
#include "begin_technique.h"
unsigned assertions = 0;
void Require(bool condition, const std::source_location& location = std::source_location::current())
{
	++assertions;
	if (!condition)
		throw std::runtime_error("Category fallback check failed at line " + std::to_string(location.line()));
}
struct Fixture
{
	TestState state;
	TestDeferred deferred;
	TestContext context;
	SIE::ShaderCache cache;
	RE::BSShader shader;
	RE::BSGraphics::PixelShader native;
	RE::BSGraphics::PixelShader* current = nullptr;
	Fixture()
	{
		globals::state = &state;
		globals::deferred = &deferred;
		globals::shaderCache = &cache;
		globals::d3d::context = &context;
		globals::game::currentPixelShader = &current;
		state.currentShader = &shader;
		native.shader = reinterpret_cast<ID3D11PixelShader*>(&native);
	}
	void Bind(uint32_t descriptor)
	{
		state.deferredPixelShaderMissing = false;
		state.modifiedPixelDescriptor = descriptor;
		Hooks::BSShader__BeginTechnique_SetPixelShader::thunk(nullptr, &native);
	}
};
void TestLayoutTransitions(RE::BSShader::Type type, bool categories)
{
	Fixture f;
	f.shader.shaderType.value = type;
	f.deferred.categories = categories;
	const uint32_t flag = SIE::ShaderCache::GetMaterialCategoryFlag(type);
	const uint32_t requested = 0x10001u | (categories ? flag : 0u);
	const uint32_t previous = requested ^ flag;
	auto* fallback = f.cache.Add(f.shader, previous);
	f.Bind(requested);
	Require(f.context.bound == fallback->shader && f.current == &f.native);
	Require(!f.state.deferredPixelShaderMissing && !f.deferred.captureReady);
	Require(f.cache.compilationSet.requests == std::set<uint32_t>{ requested });
	Require(f.cache.compiles == 0);
	auto* ready = f.cache.Add(f.shader, requested);
	f.deferred.captureReady = true;
	const auto invalidations = f.deferred.invalidations;
	for (int frame = 0; frame < 100; ++frame) f.Bind(requested);
	Require(f.context.bound == ready->shader && f.deferred.captureReady);
	Require(f.deferred.invalidations == invalidations && f.cache.compiles == 0);
}
void TestColdAndFailedVariants()
{
	Fixture f;
	const uint32_t requested = 0x10021u, plain = 0x10001u;
	f.Bind(requested);
	Require(f.state.deferredPixelShaderMissing && f.context.binds == 0 && f.current == &f.native);
	Require(f.cache.compilationSet.requests == std::set<uint32_t>{ requested });
	auto* fallback = f.cache.Add(f.shader, plain);
	Require(f.cache.GetPixelShader(f.shader, requested, SIE::ShaderCache::PixelShaderLookup::Draw) == fallback);
	Require(!f.state.deferredPixelShaderMissing);
	f.Bind(requested);
	Require(!f.state.deferredPixelShaderMissing && f.context.bound == fallback->shader);
	f.cache.async = false;
	f.cache.compilationSucceeds = false;
	f.Bind(requested);
	Require(!f.state.deferredPixelShaderMissing && f.context.bound == fallback->shader);
	Require(f.cache.compiles == 1);
	f.cache.pixelShaders[0].clear();
	f.context.binds = 0;
	f.Bind(requested);
	Require(f.state.deferredPixelShaderMissing && f.context.binds == 0);
}
void TestSynchronousAndCachedLookups()
{
	Fixture f;
	constexpr uint32_t requested = 0x10021u;
	f.cache.async = false;
	Require(!f.cache.GetPixelShader(f.shader, requested, SIE::ShaderCache::PixelShaderLookup::Cached));
	Require(f.cache.compiles == 0 && f.deferred.captureReady);
	f.Bind(requested);
	Require(f.cache.compiles == 1 && f.cache.compilationSet.requests.empty());
	Require(f.context.bound != f.native.shader && !f.state.deferredPixelShaderMissing);
	Require(f.deferred.captureReady);
	auto* exact = f.cache.GetPixelShader(f.shader, requested, SIE::ShaderCache::PixelShaderLookup::Cached);
	Require(exact && exact->shader == f.context.bound && f.cache.compiles == 1);
	f.state.deferredPixelShaderMissing = true;
	Require(f.cache.GetPixelShader(f.shader, requested) == exact);
	Require(f.state.deferredPixelShaderMissing);
}
void TestVROcclusionBypass()
{
	Fixture f;
	f.shader.fxpFilename = "OBBOcclusionTesting";
	f.Bind(0x10021u);
	if (globals::game::isVR) {
		Require(f.context.bound == f.native.shader && !f.state.deferredPixelShaderMissing);
		Require(f.cache.compilationSet.requests.empty());
	} else {
		Require(f.context.binds == 0 && f.state.deferredPixelShaderMissing);
		Require(f.cache.compilationSet.requests == std::set<uint32_t>{ 0x10021u });
	}
}
void TestExactAndProtectedLookups()
{
	Fixture f;
	constexpr uint32_t requested = 0x10021u, plain = 0x10001u;
	auto* fallback = f.cache.Add(f.shader, plain);
	Require(!f.cache.GetPixelShader(f.shader, requested));
	Require(f.deferred.captureReady && !f.state.deferredPixelShaderMissing);
	f.cache.compilationSet.requests.clear();
	f.Bind(requested);
	Require(f.context.bound == fallback->shader && f.cache.compilationSet.requests == std::set<uint32_t>{ requested });
	f.deferred.deferredPass = false;
	f.Bind(requested);
	Require(f.context.bound == f.native.shader);
	f.deferred.deferredPass = true;
	f.state.enablePShaders = false;
	f.Bind(requested);
	Require(f.context.bound == f.native.shader);
	f.state.enablePShaders = true;
	f.state.developer = true;
	f.cache.blockedKeyIndex = 0;
	f.cache.blockedKey = std::to_string(requested);
	f.Bind(requested);
	Require(f.context.bound == f.native.shader);
	f.cache.blockedKeyIndex = -1;
	f.state.developer = false;
	f.shader.shaderType.value = RE::BSShader::Type::ImageSpace;
	f.Bind(requested);
	Require(f.context.bound == f.native.shader && !f.state.deferredPixelShaderMissing);
}
void TestUnusedCategoriesStayCold()
{
	Fixture f;
	f.deferred.categories = false;
	f.Bind(0x10001u);
	Require(f.state.deferredPixelShaderMissing && f.context.binds == 0);
	Require(f.cache.compilationSet.requests == std::set<uint32_t>{ 0x10001u });
	f.cache.compilationSet.requests.clear();
	Require(!f.cache.GetPixelShader(f.shader, 0x10021u, SIE::ShaderCache::PixelShaderLookup::Cached));
	Require(f.cache.compilationSet.requests.empty());
}
bool nativeTechniqueReady = true;
RE::BSGraphics::PixelShader* nativeTechniquePixel = nullptr;
bool NativeTechnique(RE::BSShader*, uint32_t, uint32_t, bool skipPixel)
{
	if (nativeTechniqueReady)
		Hooks::BSShader__BeginTechnique_SetPixelShader::thunk(nullptr, skipPixel ? nullptr : nativeTechniquePixel);
	return nativeTechniqueReady;
}
void TestTechniqueAdmission()
{
	Fixture f;
	constexpr uint32_t requested = 0x10021u, plain = 0x10001u;
	Hooks::BSShader_BeginTechnique::func = NativeTechnique;
	nativeTechniquePixel = &f.native;
	nativeTechniqueReady = true;
	// Native shader availability must not admit a draw with incompatible outputs.
	Require(!Hooks::BSShader_BeginTechnique::thunk(&f.shader, plain, requested, false));
	Require(f.context.binds == 0 && f.state.deferredPixelShaderMissing);
	// A new depth-only technique must not inherit the rejected pixel stage.
	Require(Hooks::BSShader_BeginTechnique::thunk(&f.shader, plain, requested, true));
	Require(!f.state.deferredPixelShaderMissing && f.context.binds == 1 && f.context.bound == nullptr);
	auto* fallback = f.cache.Add(f.shader, plain);
	Require(Hooks::BSShader_BeginTechnique::thunk(&f.shader, plain, requested, false));
	Require(f.context.bound == fallback->shader);
	nativeTechniqueReady = false;
	Require(!Hooks::BSShader_BeginTechnique::thunk(&f.shader, plain, requested, false));
	ID3D11VertexShader nativeVS;
	RE::BSGraphics::VertexShader vertex{ &nativeVS };
	f.cache.vertex = &vertex;
	Require(Hooks::BSShader_BeginTechnique::thunk(&f.shader, plain, requested, false));
	Require(f.current == fallback && f.context.vertex == &nativeVS && !f.state.settingCustomShader);
	// Depth-only fallback requires a vertex stage but never a pixel variant.
	f.cache.pixelShaders[0].clear();
	f.cache.compilationSet.requests.clear();
	Require(Hooks::BSShader_BeginTechnique::thunk(&f.shader, plain, requested, true));
	Require(f.current == nullptr && !f.state.deferredPixelShaderMissing);
	Require(f.context.bound == nullptr && f.cache.compilationSet.requests.empty());
	// Effect techniques retain their native admission rules.
	f.shader.shaderType.value = RE::BSShader::Type::Effect;
	Require(!Hooks::BSShader_BeginTechnique::thunk(&f.shader, plain, requested, false));
}
void TestNullPixelBinding()
{
	for (bool cached : { false, true }) {
		Fixture f;
		f.context.bound = f.native.shader;
		f.current = &f.native;
		f.state.modifiedPixelDescriptor = 0x10021u;
		if (cached)
			f.cache.Add(f.shader, f.state.modifiedPixelDescriptor);
		Hooks::BSShader__BeginTechnique_SetPixelShader::thunk(nullptr, nullptr);
		Require(f.context.bound == nullptr && f.current == nullptr);
		Require(!f.state.deferredPixelShaderMissing && f.deferred.captureReady);
		Require(f.cache.compilationSet.requests.empty());
	}
}
void TestPBRPrewarming()
{
	constexpr uint32_t forward = 0x10009u;
	constexpr uint32_t deferred = forward | static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::Deferred);
	const auto flag = SIE::ShaderCache::GetMaterialCategoryFlag(RE::BSShader::Type::Lighting);
	Permutations::pixelPermutations = { forward, deferred };
	for (bool categories : { false, true }) {
		Fixture f;
		f.deferred.categories = categories;
		TruePBR feature;
		feature.GenerateShaderPermutations(&f.shader);
		std::set<uint32_t> expected{ forward, deferred };
		if (categories)
			expected.insert(deferred | flag);
		Require(f.cache.compilationSet.requests == expected);
		Require(f.deferred.captureReady && !f.state.deferredPixelShaderMissing);
		f.cache.compilationSet.requests.clear();
		f.shader.shaderType.value = RE::BSShader::Type::Effect;
		feature.GenerateShaderPermutations(&f.shader);
		Require(f.cache.compilationSet.requests.empty());
	}
}
int main()
{
	try {
		for (bool vr : { false, true }) {
			globals::game::isVR = vr;
			for (auto type : { RE::BSShader::Type::Lighting, RE::BSShader::Type::Grass,
					 RE::BSShader::Type::DistantTree, RE::BSShader::Type::Sky, RE::BSShader::Type::Effect })
				for (bool categories : { false, true }) TestLayoutTransitions(type, categories);
			TestColdAndFailedVariants();
			TestSynchronousAndCachedLookups();
			TestExactAndProtectedLookups();
			TestUnusedCategoriesStayCold();
			TestNullPixelBinding();
			TestTechniqueAdmission();
			TestPBRPrewarming();
			TestVROcclusionBypass();
		}
		std::cout << "Deferred shader fallback: " << assertions << " checks passed (flat and VR)\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
