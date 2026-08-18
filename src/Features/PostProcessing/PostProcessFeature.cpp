#include "PostProcessFeature.h"

void PostProcessFeature::CompileComputeShadersAsync(std::wstring_view sourceDir, std::span<const ComputeShaderCompileInfo> infos)
{
	for (const auto& info : infos) {
		auto path = std::filesystem::path(sourceDir) / info.filename;
		globals::shaderCache->EnqueueComputeShaderCompile(
			path.wstring(), info.entry, info.defines,
			[this, ptr = info.programPtr](ID3D11ComputeShader* shader) {
				if (shader) {
					std::lock_guard lock(shaderMutex);
					ptr->attach(shader);
				}
			});
	}
}

bool PostProcessFeature::AllShadersReady(std::initializer_list<const winrt::com_ptr<ID3D11ComputeShader>*> shaders) const
{
	std::lock_guard lock(shaderMutex);
	for (auto* shader : shaders) {
		if (!*shader)
			return false;
	}
	return true;
}
