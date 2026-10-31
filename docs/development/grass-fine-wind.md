# Continuous Fine Wind for grass

This change builds on the Open Wind provider extraction in PR #1041.
The provider already supplies continuous world-space transport through its
public renderer interface. No interface revision or provider source change
is required.

The grass spring pass samples the slow fine band at undeformed world
positions and adds it to ambient and gust velocity before spring response.
The detail band supplies signed flutter, with positive gusts increasing its
amplitude. Grass travel rate uses the provider's separately integrated grass
transport; changing that rate does not multiply an accumulated clock.
Detail fades when a field's cell spacing cannot represent its wavelength.

Flutter rotates around the wind direction and follows the bent pose.
Symmetric angular headroom keeps the tilt limit from pushing the average
pose away from the spring response. Vertex alpha weights flutter, and
existing broad-bend weighting is retained pending the independent authored
alpha change in PR #1039. Transient flutter remains separately controlled.
Current and previous spring textures feed their respective deformations;
normals include the flutter rotation in both grass rendering paths.

Wind Field exposes strength, baseline amplitude, ambient coupling, gust
coupling, and travel rate. These settings participate in save/load, page
reset, and Scene Manager overrides. Existing grass controls adjust response,
detail scale, detail travel, and gust influence. Devbench's Wind feature
command `setFineWind` accepts optional numeric arguments `strength`,
`baseline`, `ambientCoupling`, `gustCoupling`, and `travelRate`; the feature
diagnostics report the five configured settings.

Tree response, shelter, mesh authoring, LOD, and presets are outside this
change. Fine Wind is sampled procedurally in the existing grass simulation;
there is no additional texture or CPU simulation. Open Wind's absence keeps
the existing ordinary-wind fallback.
