# Things to Do This Week

-   [ ] Capture grass spring A/B GPU timings with ambient grass disabled and confirm whether `OSUtility::GrassWindSpringUpdate` still dispatches; investigate the profiler's stale timer values if needed.
-   [ ] Perform RenderDoc analysis of grass draw and shader workload for ambient-wind versus vanilla-flutter A/B captures.
-   [ ] Figure out a good spring algorithm for the non-compute grass-wind path.
-   [ ] Before the tree-wind PR, replace the hard-coded `NatureOfTheWildLands.json` loader with generic tree-wind patch discovery and merging.
