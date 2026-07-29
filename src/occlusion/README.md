# Occlusion module

This directory owns visibility tests, depth pyramids, and occlusion-specific
batching. It may depend on `raster`, `math`, and `platform`, but it must not
depend on the ABI or C# binding layers.
