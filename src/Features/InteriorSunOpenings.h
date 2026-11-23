#pragma once

#include "Buffer.h"

namespace InteriorSunOpenings
{
	struct Projection
	{
		UINT target{}, slice{};
		std::array<float4, 2> gridFromShadow{};
		float2 shadowDepthFromGrid{};
	};

	struct ShellShape
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		RE::NiTransform transform;
		bool operator==(const ShellShape&) const = default;
	};

	struct Detector
	{
		/** Updates the structural geometry used to find openings in the loaded interior. */
		void SetShell(std::vector<ShellShape> shapes);
		/** Maps a native shadow cascade into the cell's shared sun-space opening grid. */
		bool Prepare(Projection& projection, UINT target, UINT slice, const RE::BSGraphics::ViewData& eye, const RE::NiPoint3& adjust, float minimumWidth, float depthDrop, int rimCoverage, bool doubleSided);
		/** Combines admitted openings with authored panes and closes the remaining native shadow texels. */
		void Draw(const Projection& projection, ID3D11ShaderResourceView* panes, ID3D11RenderTargetView* mergedTarget, ID3D11ShaderResourceView* mergedEntries,
			ID3D11VertexShader* fullscreen, ID3D11RasterizerState* raster, ID3D11DepthStencilState* writeDepth,
			const std::array<winrt::com_ptr<ID3D11SamplerState>, 4>& materialSamplers);

	private:
		struct Grid
		{
			struct Level
			{
				std::unique_ptr<Texture2D> depth, rim;
			};
			std::vector<Level> levels;
			std::unique_ptr<Texture2D> candidates, centers, entries;
			std::unique_ptr<Buffer> regions;
		};
		bool Initialize();
		void FitGrid(float3 ray, float minimumWidth);
		void RasterizeShell(ID3D11RasterizerState* backCull, const std::array<winrt::com_ptr<ID3D11SamplerState>, 4>& materialSamplers);
		Grid grid;
		std::vector<ShellShape> shell;
		bool failed{}, dirty{ true }, fitDirty{ true }, forceDoubleSided{};
		float width{}, drop{}, pitch{}, depthRange{};
		UINT coverage{};
		float3 u{}, v{}, direction{}, gridOrigin{};
		float4x4 worldToClip{};
		std::unique_ptr<ConstantBuffer> constants, shapeConstants;
		winrt::com_ptr<ID3DBlob> shellBytecode;
		winrt::com_ptr<ID3D11VertexShader> shellVS;
		winrt::com_ptr<ID3D11PixelShader> depthPS, cutoutPS;
		winrt::com_ptr<ID3D11RasterizerState> twoSidedRaster;
		winrt::com_ptr<ID3D11BlendState> nearestDepth;
		std::unordered_map<UINT, winrt::com_ptr<ID3D11InputLayout>> layouts;
		winrt::com_ptr<ID3D11ComputeShader> reduceDepth, findRims, findCenters, makeEntries;
		winrt::com_ptr<ID3D11ComputeShader> seedRegions, joinRegions, measureRims, classifyOpenings;
		winrt::com_ptr<ID3D11PixelShader> merge, closeDepth;
	};
}
