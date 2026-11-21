# VR Eye Handling

VR renders both eyes side by side in one target: x in [0, 0.5) is the left eye and [0.5, 1) the right. A pass that treats that target as one wide image puts its centre on the seam and blurs, convolves or ghosts across it. Flat builds are the one-eye case (`kEyeCount = 1`), so the same code serves both.

## Rules

1. **Per-pixel passes need nothing.** Tone mapping, LUTs and grading read and write the same texel.
2. **Anything that uses screen position or neighbours works per eye.** That means centre or radius masks, vignettes, blur or gather taps, distortion, convolution and FFTs.
3. **In shaders** include `Common/StereoEye.hlsli`: `EyeLocalUV` turns a texel into an eye-local UV, `SampleEye` returns black outside the eye, `SampleEyeClamped` clamps blur taps to the eye edge; `Stereo::ClampToEyeUV` does the same for raw UV taps. Use `Stereo::ConvertToStereoUV` / `ConvertFromStereoUV` (`Common/VR.hlsli`) for other conversions and `Stereo::GetEyeIndexFromTexCoord` when you only have the packed UV.
4. **In C++** (any feature) use `Util::EyeCount()` and `Util::EyeWidth()` from `Utils/Game.h`; never write `isVR ? 2 : 1`. For post processing, also size every resource from the pass input description, never from a different render target (`kMAIN_COPY` is render resolution, the input is output resolution). Use the same helpers for the eye count and per-eye width, and rebuild resources when the input size changes.
5. **Two ways to go per eye.** Either one dispatch whose threads resolve their eye from the output texel (blur and gather passes), or a loop over eyes on eye-local copies with per-eye buffer and texture views (Physical Lens). A whole-target FFT needs the second form.
6. **Compare in game, not by reading.** Toggle the effect with the camera static and compare each half of the frame; a per-eye feature should change both halves the same way.

`tests/test_architecture_lints.py` fails when hand-rolled `isVR ? 2 : 1` eye math appears in C++, when a shader redefines `eyeCount = 2`, a Post Processing shader folder is not classified below, when a folder marked `stereo` never converts eye UVs, and when a `per-pixel` or `global` folder starts using stereo helpers.

## Post Processing status

| Shader folder                                                              | Class     | VR state                                                                                                                                |
| -------------------------------------------------------------------------- | --------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| Border, Camera, DoF, LocalExposure, MotionBlur, Vignette, Composite        | stereo    | Per eye.                                                                                                                                |
| Lens Flare                                                                 | stereo    | All three ghost modes are per eye; the FFT modes run the whole chain once per eye, so Quality and Ultra cost about twice as much in VR. |
| Bloom (`CODBloom`)                                                         | stereo    | Blur taps clamp to the eye of the pixel being filtered (`Stereo::ClampToEyeUV`).                                                        |
| Physical Lens (`PhysicalLensFlare`, `PhysicalLensImaging`), Physical Glare | eye-local | Run once per eye on eye-local data, with `ScreenWidth` one eye wide.                                                                    |
| ColorGrading, LUT                                                          | per-pixel | Nothing to do.                                                                                                                          |
| HistogramAutoExposure                                                      | global    | One histogram over both eyes, so both eyes adapt to the same exposure.                                                                  |
