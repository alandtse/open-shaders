#pragma once

#include <algorithm>
#include <cmath>

// Engine-free lit-region math, unit-testable without the game runtime.
namespace LightLimitFixLitRegion
{
	/// Lower bound on the distance from world point `p` to the frustum whose clip-space box is
	/// x,y in [-w,w], z in [0,w]; 0 when `p` is inside. `viewProj` is row-major, row-vector
	/// convention (`clip4 = [p,1] * viewProj`). A plane with no normal (an infinite far plane) is skipped;
	/// returns a negative value when the matrix is unusable.
	inline float DistanceToFrustum(const float viewProj[16], const float p[3])
	{
		const auto col = [&](int c, int r) { return viewProj[r * 4 + c]; };
		float farthestOutside = 0.0f;
		int usablePlanes = 0;
		for (int plane = 0; plane < 6; ++plane) {
			float coeff[4];
			for (int r = 0; r < 4; ++r) {
				switch (plane) {
				case 0:
					coeff[r] = col(3, r) + col(0, r);
					break;
				case 1:
					coeff[r] = col(3, r) - col(0, r);
					break;
				case 2:
					coeff[r] = col(3, r) + col(1, r);
					break;
				case 3:
					coeff[r] = col(3, r) - col(1, r);
					break;
				case 4:
					coeff[r] = col(2, r);
					break;
				default:
					coeff[r] = col(3, r) - col(2, r);
					break;
				}
			}
			const float len = std::sqrt(coeff[0] * coeff[0] + coeff[1] * coeff[1] + coeff[2] * coeff[2]);
			if (!std::isfinite(len))
				return -1.0f;
			if (len < 1e-6f)
				continue;
			++usablePlanes;
			const float signedDistance = (coeff[0] * p[0] + coeff[1] * p[1] + coeff[2] * p[2] + coeff[3]) / len;
			if (!std::isfinite(signedDistance))
				return -1.0f;
			farthestOutside = std::max(farthestOutside, -signedDistance);
		}
		return usablePlanes > 0 ? farthestOutside : -1.0f;
	}
}
