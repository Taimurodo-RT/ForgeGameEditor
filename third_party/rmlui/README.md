RmlUi 6.3 (https://github.com/mikke89/RmlUi, tag `6.3`), MIT license (LICENSE.txt).

Kept here so the engine can extend it toward the web's styling (see
docs/ui-web-styling.md): only `Include/`, `Source/Core`, `Source/Debugger`,
the SVG plugin (`Source/SVG`, `Include/RmlUi/SVG`, built on lunasvg) and the
CMake files. Backends, samples, tests, Lua and Lottie were left out.
Changes made for Forge are marked with `// Forge:` comments. Among them: `px`
is a logical pixel, scaled by the context's dp-ratio like `dp`, and `devpx` is
a pixel of the window (step 3 of docs/models/вектор-модель.md); `<img>`,
`decorator: image(...)` and `border-image` draw an SVG picture through the
plugin at its size on screen, in the element's `color` where the picture uses
`currentColor`. Step 4: an element can be made live (`RmlUi/SVG/LivePicture.h`)
and draws its SVG pictures from documents of its own that a script changes;
SVG pictures are rasterized at the scale of the element's transforms and its
ancestors' (`RmlUi/Core/PictureScale.h`), by steps of 2^(1/4) while that scale
keeps changing.
