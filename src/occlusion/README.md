# Occlusion module

This directory owns visibility tests, depth pyramids, and occlusion-specific
batching. It may depend on `raster`, `math`, and `platform`, but it must not
depend on the ABI or C# binding layers.

AABB visibility is conservative. Valid projectable world-space bounds are
covered by a conservatively selected Hi-Z level and are reported occluded only
when the complete projection is strictly behind occluder depth. The comparison
honors forward or reversed Z and both supported clip depth ranges. Invalid,
non-finite, near-plane-crossing, and otherwise unprojectable bounds are
fail-open (`UNKNOWN`); valid bounds without a strict occlusion proof are
`VISIBLE`.
