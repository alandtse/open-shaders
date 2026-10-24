#include "Features/GrassCollision.h"
#include "Features/GrassOptimizations.h"
#include "Features/Wind/Wind.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/Game.h"

static void ComputeCameraRelativeFrustumPlanes(float (*out)[4], const Matrix& viewProj)
{
	// Matches GrassCullingCS.hlsl's divide-by-zero guard convention (e.g. max(length(dvC), 1e-4)).
	constexpr float kPlaneNormalizeEpsilon = 1e-4f;

	const float rows[4][4] = {
		{ viewProj._11, viewProj._12, viewProj._13, viewProj._14 },
		{ viewProj._21, viewProj._22, viewProj._23, viewProj._24 },
		{ viewProj._31, viewProj._32, viewProj._33, viewProj._34 },
		{ viewProj._41, viewProj._42, viewProj._43, viewProj._44 }
	};

	const auto setPlane = [&](uint32_t index, uint32_t rowA, float signA, uint32_t rowB, float signB) {
		float plane[4];
		for (uint32_t component = 0; component < 4; ++component)
			plane[component] = rows[rowA][component] * signA + rows[rowB][component] * signB;
		const float invLength = 1.0f / std::max(std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]), kPlaneNormalizeEpsilon);
		out[index][0] = plane[0] * invLength;
		out[index][1] = plane[1] * invLength;
		out[index][2] = plane[2] * invLength;
		out[index][3] = -plane[3] * invLength;
	};

	setPlane(0, 2, 1.0f, 2, 0.0f);
	setPlane(1, 3, 1.0f, 2, -1.0f);
	setPlane(2, 3, 1.0f, 0, 1.0f);
	setPlane(3, 3, 1.0f, 0, -1.0f);
	setPlane(4, 3, 1.0f, 1, -1.0f);
	setPlane(5, 3, 1.0f, 1, 1.0f);
}

void GrassOptimizations::CullGrass(const RE::NiCamera* camera)
{
	auto* device = globals::d3d::device;
	auto* ctx = globals::d3d::context;

	const uint32_t eyeCount = globals::game::isVR ? 2u : 1u;
	RE::NiFrustumPlanes frustums[2]{};
	FrustumSoA frustumSoAs[2];
	for (uint32_t eye = 0; eye < eyeCount; ++eye) {
		auto transform = camera->world;
		if (globals::game::isVR)
			transform.translate = Util::GetEyePosition(eye);
		const auto& viewFrustum = globals::game::isVR ? camera->GetVRRuntimeData().viewFrustumArray[eye] : camera->GetRuntimeData2().viewFrustum;
		ComputeFrustumPlanes(frustums[eye], viewFrustum, transform);
		BuildFrustumSoA(frustumSoAs[eye], frustums[eye]);
	}
	const RE::NiPoint3 cameraTranslation = camera->world.translate;
	const __m128 cameraPosition = _mm_setr_ps(cameraTranslation.x, cameraTranslation.y, cameraTranslation.z, 0.0f);
	const auto& frameBuffer = globals::game::frameBufferCached;

	if (settings.EnableOcclusionCulling)
		hiZ.Build(device, ctx);
	else
		hiZ.Invalidate();

	{
		CullParamsCB params{};

		params.eyeCount = eyeCount;
		for (uint32_t eye = 0; eye < eyeCount; ++eye) {
			const auto& viewProj = frameBuffer.GetCameraViewProjUnjittered(eye);
			const auto& position = frameBuffer.GetCameraPosAdjust(eye);
			std::memcpy(params.cameraViewProj[eye], &viewProj, sizeof(params.cameraViewProj[eye]));
			params.cameraPos[eye][0] = position.x;
			params.cameraPos[eye][1] = position.y;
			params.cameraPos[eye][2] = position.z;
			if (globals::game::isVR) {
				ComputeCameraRelativeFrustumPlanes(params.frustumPlanes + eye * 6, viewProj);
			} else {
				for (uint32_t plane = 0; plane < 6; ++plane) {
					const auto& source = frustums[eye].cullingPlanes[plane];
					params.frustumPlanes[plane][0] = source.normal.x;
					params.frustumPlanes[plane][1] = source.normal.y;
					params.frustumPlanes[plane][2] = source.normal.z;
					params.frustumPlanes[plane][3] = source.constant;
				}
			}
			const auto* rowW = params.cameraViewProj[eye][3];
			params.projectionWLength[eye] = std::sqrt(rowW[0] * rowW[0] + rowW[1] * rowW[1] + rowW[2] * rowW[2]);
		}

		params.minPixelSize = settings.MinPixelSize;
		params.fullDetailPixelSize = settings.FullDetailPixelSize;
		params.lodMinKeep = settings.MinDensity;
		params.lodFadeBand = 0.15f;

		const auto& viewFrustum = globals::game::isVR ? camera->GetVRRuntimeData().viewFrustumArray[0] : camera->GetRuntimeData2().viewFrustum;
		const float screenHeight = Util::ConvertToDynamic(globals::state->screenSize).y;
		params.meshCostBias = settings.MeshCostBias;
		params.projScale = screenHeight / (2.0f * std::abs(viewFrustum.fTop));
		params.maxDistSq = maxDistSq;
		params.edgeFadeStart = std::clamp(settings.EdgeFadeStart, 0.0f, 1.0f);

		params.alphaParam1 = grassStartFadeDistance;
		params.alphaParam2 = maxGrassDistance;
		params.fadeNow = timeAccum;
		params.fadeInTimeRcp = fadeInTimeRcp;

		params.invisibleFadeCull = settings.InvisibleFadeCull;
		params.simpleShadingPixelSize = std::max(0.0f, settings.SimpleShadingPixelSize);
		const auto& projection = frameBuffer.GetCameraProj();
		params.reverseProjection = projection._33 * projection._43 < 0.0f ? 1.0f : 0.0f;
		params.midLODPixelSize = settings.MidLODPixelSize;
		params.farLODPixelSize = settings.EnableMidLOD ? std::min(settings.FarLODPixelSize, settings.MidLODPixelSize) : settings.FarLODPixelSize;
		params.densityDistanceRcp = 1.0f / std::max(vanillaMaxDistance, 1e-4f);

		params.meshLODBandPx = std::max(0.0f, settings.MeshLODBandPixels);
		params.hiZEnabled = hiZ.IsValid() ? 1.0f : 0.0f;
		params.hiZSizeX = (float)hiZ.GetWidth();
		params.hiZSizeY = (float)hiZ.GetHeight();

		params.hiZMipCount = (float)hiZ.GetMipCount();
		params.occlusionBias = std::max(0.0f, settings.OcclusionBias);
		params.costBiasStartDist = std::max(0.0f, settings.CostBiasStartDistance);

		cullParamsCB->Update(params);
	}

	visibleBuckets.clear();
	sliceTableCPU.clear();

	{
		CS_GPU_PASS("GrassOptimizations::SliceCull");
		for (auto& [key, bucket] : bucketStore.buckets) {
			bucket.ResetCullState();
			const auto* texture = bucket.asset.diffuseTexture.get();
			if (!bucket.totalInstances || !bucket.source.instances || !bucket.uploadedSlices || !texture || !texture->rendererTexture) {
				bucketStore.SetOptimizedReady(bucket, false);
				continue;
			}

			bucketStore.PrepareBucket(bucket);

			CullBucketStereoSlices(bucket, frustumSoAs, eyeCount, cameraPosition);
			if (bucket.sliceTableCount)
				visibleBuckets.push_back(&bucket);
			else
				bucketStore.SetOptimizedReady(bucket, true);
		}

		std::erase_if(visibleBuckets, [&](GrassBucket* bucket) {
			bucket->complexSRV = bucketStore.meshLibrary.GetComplexGrass(bucket->asset.diffuseTexture.get(), ctx);

			if (bucket->cullGroups.size() <= kMaxRecordGroups && bucketStore.UploadCullGroups(*bucket, ctx))
				return false;

			// Use engine draws if the bucket cannot supply valid cull resources.
			bucket->ResetCullState();
			bucketStore.SetOptimizedReady(*bucket, false);

			return true;
		});
	}

	CS_GPU_PASS("GrassOptimizations::InstanceCull");
	UploadCullState(ctx);
}

ID3D11ComputeShader* GrassOptimizations::GetCullCS()
{
	std::vector<std::pair<const char*, const char*>> defines;
	if (globals::features::grassCollision.loaded)
		defines.emplace_back("GRASS_COLLISION", "1");
	return cullCS.Get(L"Data\\Shaders\\GrassOptimizations\\GrassCullingCS.hlsl", defines,
		"cs_5_0", "main", "GrassOptimizations::CullCS");
}

bool GrassOptimizations::IsMainCameraPerFrame(const RE::NiPoint3& mainCameraPos)
{
	const auto& posAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();
	const RE::NiPoint3 eyeOffset{ posAdjust.x - mainCameraPos.x, posAdjust.y - mainCameraPos.y, posAdjust.z - mainCameraPos.z };

	return eyeOffset.SqrLength() < 64.0f * 64.0f;
}

bool GrassOptimizations::HasCurrentCull() const
{
	return lastFrame == globals::game::graphicsState->GetFrameCount();
}

void GrassOptimizations::ComputeFrustumPlanes(RE::NiFrustumPlanes& out, const RE::NiFrustum& viewFrustum, const RE::NiTransform& transform)
{
	const __m128 forward = _mm_set_ps(0.0f, transform.rotate.entry[2][0], transform.rotate.entry[1][0], transform.rotate.entry[0][0]);
	const __m128 up = _mm_set_ps(0.0f, transform.rotate.entry[2][1], transform.rotate.entry[1][1], transform.rotate.entry[0][1]);
	const __m128 right = _mm_set_ps(0.0f, transform.rotate.entry[2][2], transform.rotate.entry[1][2], transform.rotate.entry[0][2]);
	const __m128 translation = _mm_set_ps(0.0f, transform.translate.z, transform.translate.y, transform.translate.x);

	const __m128 nearPoint = _mm_add_ps(translation, _mm_mul_ps(forward, _mm_set1_ps(viewFrustum.fNear)));
	const __m128 farPoint = _mm_add_ps(translation, _mm_mul_ps(forward, _mm_set1_ps(viewFrustum.fFar)));

	auto setPlane = [&](int planeIndex, __m128 normal, __m128 point) {
		alignas(16) float components[4];
		_mm_store_ps(components, normal);
		out.cullingPlanes[planeIndex].normal = { components[0], components[1], components[2] };
		out.cullingPlanes[planeIndex].constant = _mm_cvtss_f32(_mm_dp_ps(normal, point, 0x71));
	};

	setPlane(0, forward, nearPoint);
	const __m128 backward = _mm_xor_ps(forward, _mm_set1_ps(-0.0f));
	setPlane(1, backward, farPoint);

	if (viewFrustum.bOrtho) {
		setPlane(2, right, _mm_add_ps(translation, _mm_mul_ps(right, _mm_set1_ps(viewFrustum.fLeft))));
		setPlane(3, _mm_xor_ps(right, _mm_set1_ps(-0.0f)), _mm_add_ps(translation, _mm_mul_ps(right, _mm_set1_ps(viewFrustum.fRight))));
		setPlane(4, _mm_xor_ps(up, _mm_set1_ps(-0.0f)), _mm_add_ps(translation, _mm_mul_ps(up, _mm_set1_ps(viewFrustum.fTop))));
		setPlane(5, up, _mm_add_ps(translation, _mm_mul_ps(up, _mm_set1_ps(viewFrustum.fBottom))));
	} else {
		// Normalize each side plane from the camera axis and frustum slope.
		auto setSidePlane = [&](int planeIndex, __m128 axis, float slope, float forwardSign, float axisSign) {
			const float inverseLength = 1.0f / std::sqrt(slope * slope + 1.0f);
			const __m128 normal = _mm_add_ps(
				_mm_mul_ps(forward, _mm_set1_ps(forwardSign * slope * inverseLength)),
				_mm_mul_ps(axis, _mm_set1_ps(axisSign * inverseLength)));
			setPlane(planeIndex, normal, translation);
		};

		setSidePlane(2, right, viewFrustum.fLeft, -1.0f, +1.0f);
		setSidePlane(3, right, viewFrustum.fRight, +1.0f, -1.0f);
		setSidePlane(4, up, viewFrustum.fTop, +1.0f, -1.0f);
		setSidePlane(5, up, viewFrustum.fBottom, -1.0f, +1.0f);
	}

	out.activePlanes = RE::NiFrustumPlanes::ActivePlane(0x3F);

	constexpr float edgePadding = 128.0f;
	out.cullingPlanes[2].constant -= edgePadding;
	out.cullingPlanes[3].constant -= edgePadding;
	out.cullingPlanes[4].constant -= edgePadding;
	out.cullingPlanes[5].constant -= edgePadding;
}

void GrassOptimizations::BuildFrustumSoA(FrustumSoA& out, const RE::NiFrustumPlanes& frustum)
{
	constexpr RE::NiFrustumPlanes::ActivePlane planeBits[RE::NiFrustumPlanes::Planes::kTotal] = {
		RE::NiFrustumPlanes::ActivePlane::kNear, RE::NiFrustumPlanes::ActivePlane::kFar,
		RE::NiFrustumPlanes::ActivePlane::kLeft, RE::NiFrustumPlanes::ActivePlane::kRight,
		RE::NiFrustumPlanes::ActivePlane::kTop, RE::NiFrustumPlanes::ActivePlane::kBottom
	};

	// Inactive planes always pass, keeping slice tests branch-free.
	alignas(16) float nx[8], ny[8], nz[8], d[8];
	for (uint32_t i = 0; i < 8; ++i) {
		nx[i] = ny[i] = nz[i] = 0.0f;
		d[i] = -1.0f;
	}

	for (uint32_t i = 0; i < 6; ++i) {
		if (!frustum.activePlanes.any(planeBits[i]))
			continue;
		const auto& plane = frustum.cullingPlanes[i];
		nx[i] = plane.normal.x;
		ny[i] = plane.normal.y;
		nz[i] = plane.normal.z;
		d[i] = plane.constant;
	}

	for (uint32_t planeBatch = 0; planeBatch < 2; ++planeBatch) {
		out.nx[planeBatch] = _mm_load_ps(nx + planeBatch * 4);
		out.ny[planeBatch] = _mm_load_ps(ny + planeBatch * 4);
		out.nz[planeBatch] = _mm_load_ps(nz + planeBatch * 4);
		out.d[planeBatch] = _mm_load_ps(d + planeBatch * 4);
	}

	for (uint32_t i = 0; i < 6; ++i) {
		out.planes[i] = { _mm_set1_ps(nx[i]), _mm_set1_ps(ny[i]), _mm_set1_ps(nz[i]), _mm_set1_ps(d[i]) };
	}
}

bool GrassOptimizations::ClassifyAabb(const FrustumSoA& frustum, __m128 minimum, __m128 maximum, uint32_t& intersectingPlanes)
{
	intersectingPlanes = 0;
	const __m128 minX = _mm_shuffle_ps(minimum, minimum, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 minY = _mm_shuffle_ps(minimum, minimum, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 minZ = _mm_shuffle_ps(minimum, minimum, _MM_SHUFFLE(2, 2, 2, 2));
	const __m128 maxX = _mm_shuffle_ps(maximum, maximum, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 maxY = _mm_shuffle_ps(maximum, maximum, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 maxZ = _mm_shuffle_ps(maximum, maximum, _MM_SHUFFLE(2, 2, 2, 2));

	for (uint32_t planeBatch = 0; planeBatch < 2; ++planeBatch) {
		// Select the corner furthest along each plane normal using its sign bits.
		const __m128 positiveX = _mm_blendv_ps(maxX, minX, frustum.nx[planeBatch]);
		const __m128 positiveY = _mm_blendv_ps(maxY, minY, frustum.ny[planeBatch]);
		const __m128 positiveZ = _mm_blendv_ps(maxZ, minZ, frustum.nz[planeBatch]);

		const __m128 maxProjection = _mm_add_ps(
			_mm_add_ps(_mm_mul_ps(frustum.nx[planeBatch], positiveX), _mm_mul_ps(frustum.ny[planeBatch], positiveY)),
			_mm_mul_ps(frustum.nz[planeBatch], positiveZ));

		if (_mm_movemask_ps(_mm_cmplt_ps(_mm_sub_ps(maxProjection, frustum.d[planeBatch]), _mm_setzero_ps())))
			return false;

		// A plane containing the negative vertex contains the whole coarse box.
		const __m128 negativeX = _mm_blendv_ps(minX, maxX, frustum.nx[planeBatch]);
		const __m128 negativeY = _mm_blendv_ps(minY, maxY, frustum.ny[planeBatch]);
		const __m128 negativeZ = _mm_blendv_ps(minZ, maxZ, frustum.nz[planeBatch]);
		const __m128 minProjection = _mm_add_ps(_mm_add_ps(_mm_mul_ps(frustum.nx[planeBatch], negativeX), _mm_mul_ps(frustum.ny[planeBatch], negativeY)), _mm_mul_ps(frustum.nz[planeBatch], negativeZ));
		const uint32_t insideMask = _mm_movemask_ps(_mm_cmpge_ps(_mm_sub_ps(minProjection, frustum.d[planeBatch]), _mm_setzero_ps()));
		intersectingPlanes |= ((~insideMask) & 0xFu) << (planeBatch * 4);
	}

	intersectingPlanes &= 0x3Fu;

	return true;
}

uint32_t GrassOptimizations::LaneMask(uint32_t remaining)
{
	return (1u << std::min(4u, remaining)) - 1u;
}

void GrassOptimizations::CullBucketStereoSlices(GrassBucket& bucket, const FrustumSoA* frustums, uint32_t eyeCount, __m128 cameraPosition)
{
	if (eyeCount == 1) {
		CullBucketSlices(bucket, frustums[0], cameraPosition);
		return;
	}

	static thread_local std::vector<std::pair<uint32_t, uint32_t>> ranges;
	ranges.clear();
	const auto tableStart = sliceTableCPU.size();
	for (uint32_t eye = 0; eye < eyeCount; ++eye) {
		bucket.sliceTableCount = 0;
		bucket.groupCount = 0;
		CullBucketSlices(bucket, frustums[eye], cameraPosition);
		for (uint32_t run = 0; run < bucket.sliceTableCount; ++run) {
			const auto [source, visible] = sliceTableCPU[bucket.sliceTableOffset + run];
			const uint32_t end = run + 1 < bucket.sliceTableCount ? sliceTableCPU[bucket.sliceTableOffset + run + 1].second : bucket.groupCount;
			ranges.emplace_back(source, source + end - visible);
		}
		sliceTableCPU.resize(tableStart);
	}
	bucket.sliceTableOffset = (uint32_t)tableStart;
	bucket.sliceTableCount = 0;
	bucket.groupCount = 0;
	std::sort(ranges.begin(), ranges.end());
	for (size_t i = 0; i < ranges.size();) {
		const uint32_t first = ranges[i].first;
		uint32_t end = ranges[i++].second;
		while (i < ranges.size() && ranges[i].first <= end)
			end = std::max(end, ranges[i++].second);
		AppendVisibleGroups(bucket, first, end - first);
	}
}

void GrassOptimizations::CullBucketSlices(GrassBucket& bucket, const FrustumSoA& frustumSoA, __m128 cameraPosition)
{
	bucket.sliceTableOffset = (uint32_t)sliceTableCPU.size();

	const __m128 bucketMin = _mm_setr_ps(bucket.coarseMin.x, bucket.coarseMin.y, bucket.coarseMin.z, 0.0f);
	const __m128 bucketMax = _mm_setr_ps(bucket.coarseMax.x, bucket.coarseMax.y, bucket.coarseMax.z, 0.0f);
	const __m128 nearest = _mm_max_ps(_mm_max_ps(_mm_sub_ps(bucketMin, cameraPosition), _mm_sub_ps(cameraPosition, bucketMax)), _mm_setzero_ps());
	if (_mm_cvtss_f32(_mm_dp_ps(nearest, nearest, 0x71)) > maxDistSq)
		return;

	uint32_t planeMask;
	if (!ClassifyAabb(frustumSoA, bucketMin, bucketMax, planeMask))
		return;

	if (!bucket.clustersValid || bucket.clusteredSlices != bucket.uploadedSlices)
		bucketStore.MergeSlicesIntoRuns(bucket);

	const __m128 signBit = _mm_set1_ps(-0.0f);
	const __m128 farthest = _mm_max_ps(_mm_andnot_ps(signBit, _mm_sub_ps(bucketMin, cameraPosition)), _mm_andnot_ps(signBit, _mm_sub_ps(bucketMax, cameraPosition)));
	const bool allWithinDistance = _mm_cvtss_f32(_mm_dp_ps(farthest, farthest, 0x71)) <= maxDistSq;

	if (!planeMask && allWithinDistance) {
		// Every run is visible, and its groups are contiguous in their original order.
		if (!bucket.cullGroups.empty())
			AppendVisibleGroups(bucket, 0, (uint32_t)bucket.cullGroups.size());

	} else if (planeMask && bucket.sliceBlocksUseful) {
		CullBucketSliceBlocks(bucket, frustumSoA, cameraPosition, planeMask, allWithinDistance);
	} else {
		CullSliceRuns(bucket, frustumSoA, cameraPosition, 0, bucket.sliceRunCount, planeMask, allWithinDistance);
	}
}

void GrassOptimizations::CullBucketSliceBlocks(GrassBucket& bucket, const FrustumSoA& frustumSoA, __m128 cameraPosition, uint32_t planeMask, bool allWithinDistance)
{
	const __m128 zero = _mm_setzero_ps();
	const __m128 cameraX = _mm_shuffle_ps(cameraPosition, cameraPosition, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 cameraY = _mm_shuffle_ps(cameraPosition, cameraPosition, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 cameraZ = _mm_shuffle_ps(cameraPosition, cameraPosition, _MM_SHUFFLE(2, 2, 2, 2));
	const __m128 maxDistanceSq = _mm_set1_ps(maxDistSq);

	const uint32_t blockCount = (bucket.sliceRunCount + GrassBucket::kRunsPerCullBlock - 1) / GrassBucket::kRunsPerCullBlock;
	for (uint32_t firstBlock = 0; firstBlock < blockCount; firstBlock += 4) {
		const auto& batch = bucket.sliceBlockBatches[firstBlock / 4];
		const BoxBatch blocks(batch);
		uint32_t visibleMask = LaneMask(blockCount - firstBlock);
		uint32_t withinDistanceMask = visibleMask;

		if (!allWithinDistance) {
			visibleMask &= _mm_movemask_ps(_mm_cmple_ps(blocks.NearestDistanceSq(cameraX, cameraY, cameraZ), maxDistanceSq));
			if (!visibleMask)
				continue;
			withinDistanceMask = _mm_movemask_ps(_mm_cmple_ps(blocks.FarthestDistanceSq(cameraX, cameraY, cameraZ), maxDistanceSq));
		}

		// A plane that contains a whole block needs no test on that block's runs.
		__m128i childPlaneMasksSIMD = _mm_set1_epi32(planeMask);
		for (uint32_t remainingPlanes = planeMask; visibleMask && remainingPlanes; remainingPlanes &= remainingPlanes - 1) {
			const uint32_t planeIndex = std::countr_zero(remainingPlanes);

			const auto& plane = frustumSoA.planes[planeIndex];
			visibleMask &= ~_mm_movemask_ps(_mm_cmplt_ps(blocks.MaxPlaneDistance(plane), zero));
			const __m128i insideMask = _mm_castps_si128(_mm_cmpge_ps(blocks.MinPlaneDistance(plane), zero));
			childPlaneMasksSIMD = _mm_andnot_si128(_mm_and_si128(insideMask, _mm_set1_epi32(1 << planeIndex)), childPlaneMasksSIMD);
		}

		alignas(16) uint32_t childPlaneMasks[4];
		_mm_store_si128((__m128i*)childPlaneMasks, childPlaneMasksSIMD);

		for (; visibleMask; visibleMask &= visibleMask - 1) {
			const uint32_t lane = std::countr_zero(visibleMask);
			const bool withinDistance = (withinDistanceMask >> lane) & 1u;

			if (!childPlaneMasks[lane] && withinDistance) {
				AppendVisibleGroups(bucket, batch.firstGroup[lane], batch.groupCount[lane]);
				continue;
			}

			const uint32_t firstRun = (firstBlock + lane) * GrassBucket::kRunsPerCullBlock;
			const uint32_t endRun = std::min(firstRun + GrassBucket::kRunsPerCullBlock, bucket.sliceRunCount);
			CullSliceRuns(bucket, frustumSoA, cameraPosition, firstRun, endRun, childPlaneMasks[lane], withinDistance);
		}
	}
}

void GrassOptimizations::CullSliceRuns(GrassBucket& bucket, const FrustumSoA& frustumSoA, __m128 cameraPosition, uint32_t firstRun, uint32_t endRun, uint32_t planeMask, bool withinDistance)
{
	const __m128 zero = _mm_setzero_ps();
	const __m128 cameraX = _mm_shuffle_ps(cameraPosition, cameraPosition, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 cameraY = _mm_shuffle_ps(cameraPosition, cameraPosition, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 cameraZ = _mm_shuffle_ps(cameraPosition, cameraPosition, _MM_SHUFFLE(2, 2, 2, 2));
	const __m128 maxDistanceSq = _mm_set1_ps(maxDistSq);

	for (; firstRun < endRun; firstRun += 4) {
		const auto& batch = bucket.sliceRunBatches[firstRun / 4];
		const BoxBatch runs(batch);
		uint32_t visibleMask = LaneMask(endRun - firstRun);
		if (!withinDistance)
			visibleMask &= _mm_movemask_ps(_mm_cmple_ps(runs.NearestDistanceSq(cameraX, cameraY, cameraZ), maxDistanceSq));

		for (uint32_t remainingPlanes = planeMask; visibleMask && remainingPlanes; remainingPlanes &= remainingPlanes - 1)
			visibleMask &= ~_mm_movemask_ps(_mm_cmplt_ps(runs.MaxPlaneDistance(frustumSoA.planes[std::countr_zero(remainingPlanes)]), zero));

		// Consume lanes in order so adjacent visible groups still coalesce.
		for (; visibleMask; visibleMask &= visibleMask - 1) {
			const uint32_t lane = std::countr_zero(visibleMask);
			AppendVisibleGroups(bucket, batch.firstGroup[lane], batch.groupCount[lane]);
		}
	}
}

void GrassOptimizations::AppendVisibleGroups(GrassBucket& bucket, uint32_t firstGroup, uint32_t groupCount)
{
	bool extendsLast = false;
	if (bucket.sliceTableCount) {
		const auto& [sourceStart, visibleStart] = sliceTableCPU.back();
		const uint32_t previousCount = bucket.groupCount - visibleStart;
		extendsLast = sourceStart + previousCount == firstGroup;
	}
	if (!extendsLast) {
		sliceTableCPU.emplace_back(firstGroup, bucket.groupCount);
		++bucket.sliceTableCount;
	}

	bucket.groupCount += groupCount;
}

void GrassOptimizations::DisableOptimizedDraws()
{
	for (auto& [key, bucket] : bucketStore.buckets) {
		bucket.ResetCullState();
		bucketStore.SetOptimizedReady(bucket, false);
	}

	visibleBuckets.clear();
}

void GrassOptimizations::UploadCullState(ID3D11DeviceContext* ctx)
{
	if (visibleBuckets.empty()) {
		bucketStore.PrepareCullOutputs({});
		return;
	}

	ID3D11ShaderResourceView* nullInstanceView = nullptr;
	ctx->VSSetShaderResources(2, 1, &nullInstanceView);
	// All resources and uploads must succeed before any bucket is dispatched.
	if (!bucketStore.PrepareCullOutputs(visibleBuckets) || !UploadCullBucketConstants(ctx) || !UploadSliceTable(ctx)) {
		DisableOptimizedDraws();
		return;
	}

	// These fields must be updated before culling, regardless of SetupGeometry hook order.
	if (globals::features::grassCollision.loaded) {
		globals::features::grassCollision.Update();
		globals::features::grassCollision.BindDeformationResources(true);
	}
	if (globals::features::wind.loaded)
		globals::features::wind.UpdateGrassWindSpring(true);
	globals::state->UpdatePermutationBuffer();
	ID3D11Buffer* deformationBuffers[] = {
		globals::state->permutationCB->CB(), globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB()
	};
	ctx->CSSetConstantBuffers(4, ARRAYSIZE(deformationBuffers), deformationBuffers);

	ID3D11Buffer* frameBuffers[1]{ *globals::game::perFrame.get() };
	ctx->CSSetConstantBuffers(12, 1, frameBuffers);

	ID3D11Buffer* paramsCB = cullParamsCB->CB();
	ctx->CSSetConstantBuffers(0, 1, &paramsCB);
	ctx->CSSetShader(cullCS.get(), nullptr, 0);

	// Reset all bucket arguments with one upload before dispatching.
	const uint32_t eyeCount = globals::game::isVR ? 2u : 1u;
	const uint32_t argumentCount = (uint32_t)visibleBuckets.size() * eyeCount * kTierCount * kArgsStride / sizeof(uint32_t);
	cullArguments.assign(argumentCount, 0);
	for (const GrassBucket* bucket : visibleBuckets) {
		for (uint32_t eye = 0; eye < eyeCount; ++eye) {
			auto* arguments = cullArguments.data() + (bucket->cullSlot * eyeCount + eye) * kTierCount * kArgsStride / sizeof(uint32_t);
			arguments[0] = 3u * bucket->asset.key.triCount;
			for (uint32_t tier = 1; tier < kTierCount; ++tier) {
				if (const auto* lod = bucket->lodMeshes[tier - 1])
					arguments[tier * kArgsStride / sizeof(uint32_t)] = lod->indexCount;
			}
		}
	}
	const D3D11_BOX argumentBox{ 0, 0, 0, argumentCount * (UINT)sizeof(uint32_t), 1, 1 };
	ctx->UpdateSubresource(bucketStore.arguments->resource.get(), 0, &argumentBox, cullArguments.data(), 0, 0);

	ID3D11ShaderResourceView* sharedSRVs[2] = { hiZ.GetSRV(), sliceTable->srv.get() };
	ctx->CSSetShaderResources(2, 2, sharedSRVs);
	for (GrassBucket* bucket : visibleBuckets) {
		CullBucket(*bucket, ctx);
		bucketStore.SetOptimizedReady(*bucket, true);
	}

	ID3D11UnorderedAccessView* nullUAVs[2] = {};
	ctx->CSSetUnorderedAccessViews(0, (UINT)std::size(nullUAVs), nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRVs[4] = {};
	ctx->CSSetShaderResources(0, 4, nullSRVs);
	ID3D11ShaderResourceView* nullDeformationSRVs[11] = {};
	ctx->CSSetShaderResources(100, ARRAYSIZE(nullDeformationSRVs), nullDeformationSRVs);
	// Shared b5/b6 must stay bound for subsequent compute passes.
	ID3D11Buffer* nullDeformationBuffers[2] = {};
	ctx->CSSetConstantBuffers(3, ARRAYSIZE(nullDeformationBuffers), nullDeformationBuffers);
	ID3D11SamplerState* nullDeformationSamplers[2] = {};
	ctx->CSSetSamplers(14, ARRAYSIZE(nullDeformationSamplers), nullDeformationSamplers);
	ctx->CSSetShader(nullptr, nullptr, 0);

	// Removals at Present may destroy these buckets before the next frame.
	visibleBuckets.clear();
}

uint32_t GrassOptimizations::DispatchWidth(uint32_t groupCount)
{
	return std::min(groupCount, (uint32_t)D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION);
}

void GrassOptimizations::CullBucket(GrassBucket& bucket, ID3D11DeviceContext* ctx)
{
	static_assert(
		(size_t)GrassMeshLibrary::LODTier::kMiddle == 0 &&
			(size_t)GrassMeshLibrary::LODTier::kFar == 1 &&
			kTierCount == 3,
		"GrassCullingCS tiers must match LODTier");

	// Match the output and argument bindings at u0 and u1 in GrassCullingCS.
	ID3D11UnorderedAccessView* uavs[2] = { bucket.output->records->uav.get(), bucketStore.arguments->uav.get() };
	ctx->CSSetUnorderedAccessViews(0, (UINT)std::size(uavs), uavs, nullptr);

	// The Hi-Z pyramid and slice table at t2-t3 are shared and bound once by UploadCullState.
	ID3D11ShaderResourceView* srvs[2] = { bucket.source.instances->srv.get(), bucket.groups->srv.get() };
	ctx->CSSetShaderResources(0, 2, srvs);

	ID3D11Buffer* bucketCB = cullBucketCB->CB();
	const UINT firstConstant = bucket.cullSlot * (kSlotBytes / 16);
	const UINT constantCount = 16;
	ctx1->CSSetConstantBuffers1(1, 1, &bucketCB, &firstConstant, &constantCount);

	// Skipping the dispatch keeps the instance count at zero for the draw.
	if (bucket.groupCount) {
		const UINT width = DispatchWidth(bucket.groupCount);
		ctx->Dispatch(width, (bucket.groupCount + width - 1) / width, 1);
	}
}

void GrassBucketStore::PrepareBucket(GrassBucket& bucket)
{
	bucket.lodMeshes = {};
	const auto& draw = bucket.asset.key.draw;
	const auto center = DirectX::XMVectorSet(draw.boundCenter[0], draw.boundCenter[1], draw.boundCenter[2], 0.0f);
	bucket.boundExtent = DirectX::XMVectorGetX(DirectX::XMVector3Length(center)) + draw.modelRadius;

	if (frameParams.enableMeshLOD) {
		meshLibrary.EnsureLODMeshes(*bucket.asset.key.mesh);
		if (frameParams.enableMidLOD)
			bucket.lodMeshes[0] = meshLibrary.GetLODMesh(*bucket.asset.key.mesh, GrassMeshLibrary::LODTier::kMiddle);
		if (frameParams.enableFarLOD)
			bucket.lodMeshes[1] = meshLibrary.GetLODMesh(*bucket.asset.key.mesh, GrassMeshLibrary::LODTier::kFar);
		for (const auto* lod : bucket.lodMeshes) {
			if (lod)
				bucket.boundExtent = std::max(bucket.boundExtent, lod->boundExtent);
		}
	}

	const float padding = bucket.boundExtent * bucket.maxScale + 128.0f;
	if (bucket.boundsPadding != padding) {
		bucket.boundsPadding = padding;
		bucket.coarseValid = false;
		bucket.clustersValid = false;
	}

	if (!bucket.coarseValid)
		UpdateCoarseBounds(bucket);
}

void GrassBucketStore::UpdateCoarseBounds(GrassBucket& bucket)
{
	RE::NiPoint3 minimum{ FLT_MAX, FLT_MAX, FLT_MAX };
	RE::NiPoint3 maximum{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (const auto* slice : bucket.slices) {
		const auto& bounds = slice->bounds;
		minimum.x = std::min(minimum.x, bounds.lo[0]);
		minimum.y = std::min(minimum.y, bounds.lo[1]);
		minimum.z = std::min(minimum.z, bounds.lo[2]);
		maximum.x = std::max(maximum.x, bounds.hi[0]);
		maximum.y = std::max(maximum.y, bounds.hi[1]);
		maximum.z = std::max(maximum.z, bounds.hi[2]);
	}

	if (minimum.x > maximum.x) {
		minimum = { 0.0f, 0.0f, 0.0f };
		maximum = { 0.0f, 0.0f, 0.0f };
	}

	// Pad the instance bounds to retain grass along screen edges.
	const float padding = bucket.boundsPadding;
	minimum.x -= padding;
	minimum.y -= padding;
	minimum.z -= padding;
	maximum.x += padding;
	maximum.y += padding;
	maximum.z += padding;

	bucket.coarseMin = minimum;
	bucket.coarseMax = maximum;
	bucket.coarseValid = true;
}

void GrassBucketStore::MergeSlicesIntoRuns(GrassBucket& bucket)
{
	// Group slices by their instance bounds because several cells can share a shape origin.
	const auto cellOf = [](const SliceBounds& bounds) {
		constexpr float kCellSize = 4096.0f;
		const auto cellX = (int32_t)std::floor((bounds.lo[0] + bounds.hi[0]) * 0.5f / kCellSize);
		const auto cellY = (int32_t)std::floor((bounds.lo[1] + bounds.hi[1]) * 0.5f / kCellSize);
		return ((uint64_t)(uint32_t)cellX << 32) | (uint32_t)cellY;
	};

	if (!bucket.clustersValid) {
		bucket.sliceRunBatches.clear();
		bucket.sliceRunCount = 0;
		bucket.sliceBlockBatches.clear();
		bucket.sliceBlocksUseful = false;
		bucket.cullGroups.clear();
		bucket.clusteredSlices = 0;
		bucket.residentGroups = 0;
		bucket.deadGroups = 0;
		bucket.dirtyGroupRanges.clear();
	}

	const float padding = bucket.boundsPadding;
	const uint32_t uploadedSliceCount = bucket.uploadedSlices;
	for (uint32_t sliceIndex = bucket.clusteredSlices; sliceIndex < uploadedSliceCount; ++sliceIndex) {
		auto& slice = *bucket.slices[sliceIndex];
		const auto& bounds = slice.bounds;

		// Runs require a shared cell, but their groups may reference noncontiguous source records.
		const bool sameCellAsPrevious = sliceIndex && bucket.sliceRunCount &&
		                                cellOf(bounds) == cellOf(bucket.slices[sliceIndex - 1]->bounds);

		if (!sameCellAsPrevious) {
			if (bucket.sliceRunCount % 4 == 0)
				bucket.sliceRunBatches.emplace_back();
			++bucket.sliceRunCount;
		}

		auto& runBatch = bucket.sliceRunBatches.back();
		const uint32_t runLane = (bucket.sliceRunCount - 1) % 4;

		const uint32_t blockIndex = (bucket.sliceRunCount - 1) / GrassBucket::kRunsPerCullBlock;
		const uint32_t blockLane = blockIndex % 4;

		const bool startsBlock = !sameCellAsPrevious && (bucket.sliceRunCount - 1) % GrassBucket::kRunsPerCullBlock == 0;
		if (startsBlock && blockLane == 0)
			bucket.sliceBlockBatches.emplace_back();

		auto& block = bucket.sliceBlockBatches.back();
		block.IncludeBounds(blockLane, bounds, padding, startsBlock);
		runBatch.IncludeBounds(runLane, bounds, padding, !sameCellAsPrevious);

		if (startsBlock)
			block.firstGroup[blockLane] = (uint32_t)bucket.cullGroups.size();
		if (!sameCellAsPrevious)
			runBatch.firstGroup[runLane] = (uint32_t)bucket.cullGroups.size();

		// Groups never span slices, so the cull reads each instance's origin from its group.
		slice.firstCullGroup = (uint32_t)bucket.cullGroups.size();
		for (uint32_t firstInstance = 0; firstInstance < slice.count; firstInstance += kCullGroupSize) {
			bucket.cullGroups.push_back({ { slice.origin.x, slice.origin.y, slice.origin.z }, slice.fadeStart,
				slice.bufferOffset + firstInstance, std::min(kCullGroupSize, slice.count - firstInstance) });
			++runBatch.groupCount[runLane];
			++block.groupCount[blockLane];
		}
	}

	// Use parent bounds only for spatially coherent runs; scattered runs use the flat SSE path.
	bucket.sliceBlocksUseful = false;
	if (bucket.sliceRunCount >= GrassBucket::kRunsPerCullBlock * 2) {
		float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
		float totalBlockArea = 0.0f;
		const uint32_t blockCount = (bucket.sliceRunCount + GrassBucket::kRunsPerCullBlock - 1) / GrassBucket::kRunsPerCullBlock;

		for (uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
			const auto& block = bucket.sliceBlockBatches[blockIndex / 4];
			const uint32_t blockLane = blockIndex % 4;
			minX = std::min(minX, block.loX[blockLane]);
			minY = std::min(minY, block.loY[blockLane]);
			maxX = std::max(maxX, block.hiX[blockLane]);
			maxY = std::max(maxY, block.hiY[blockLane]);
			totalBlockArea += (block.hiX[blockLane] - block.loX[blockLane]) * (block.hiY[blockLane] - block.loY[blockLane]);
		}

		bucket.sliceBlocksUseful = totalBlockArea < (maxX - minX) * (maxY - minY) * (0.5f * blockCount);
	}

	bucket.clusteredSlices = uploadedSliceCount;
	bucket.clustersValid = true;
}
