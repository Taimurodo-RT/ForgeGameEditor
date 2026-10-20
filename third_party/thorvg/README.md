Two files of ThorVG 1.1.2 (https://github.com/thorvg/thorvg, tag `v1.1.2`), MIT license (LICENSE): its CPU renderer's
raster of pictures, `src/renderer/cpu_engine/tvgSwRaster.cpp` (as it is) and `tvgSwRasterTexmap.h`, which it includes.
The rest of ThorVG is fetched (cmake/Dependencies.cmake), which builds `tvgSwRaster.cpp` from here instead of its own.

Changes made for Forge are marked with `// Forge:` comments: a turned, slanted or mirrored picture (ThorVG's texture
mapping) is sampled at the centres of the pixels and between the centres of its texels, as a GPU samples it and as
ThorVG's own path for a picture only moved and scaled already did. ThorVG sampled it at each pixel's far corner, between
texel corners, so a turned picture was drawn half a pixel and half a texel up and left of its place: in «Векторная» a
coin of 32 × 32 pixels zoomed 13 times stood 7 pixels off where the game draws it (part 6г2 of
docs/models/сцены-модель.md).
