# Feature notes

## Light Limit Fix / Particle Lights
- Particle light tuning includes a clustering threshold, a per-emitter sample cap, and a max distance (game units).
- These settings trade lighting fidelity for CPU cost and default to the previous behavior.

## VRS/FFR alpha test fix
- TREE_ANIM uses a fixed low alpha threshold (0.1) to avoid edge artifacts under coarse shading.

## Heat warp scale
- "Refraction Scale" in Advanced settings scales ImageSpace refraction via SharedData.RefractionScale.
- Default is 1.0 (vanilla Community Shaders behavior).

## Terrain Blending (VR)
- VR TB learns the main depth prepass signature and only binds blended depth during the TB pass.
- Performance options include signature lock after N frames and a terrain cull distance.
- See docs/TB_VR_NOTES.md for pipeline details.
