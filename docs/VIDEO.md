# Video: the native renderer

How the picture is drawn is satrecomp's own: `satrecomp/video/`. The renderer knows no machine: it reads
the video chips' memory where the machine keeps it, is told every register write (and decodes the registers
itself, `regs.cpp`), and is called where the hardware does something: the VDP1 erasing and swapping its
framebuffers and going through its command table, the VDP2 showing a line. On satrecomp's machine
(`machine/vdp.cpp`) that is all there is. On the reference build (`make ref`) `ymir_adapter.*` connects it
to Ymir's video chips and offers Ymir's software renderer as the reference to compare with
(`SATRECOMP_VIDEO=check`; `--video-stats` exists there only).

## What a game asks for

`--video-stats` sums up what the game asked of the video hardware over a run: VDP1 commands by kind
and drawing mode, VDP2 screens and settings. `SATRECOMP_VDP1_INFO=1` prints the VDP1's framebuffer
setup, clipping and a frame's command totals every 500 frames.

Fighting Vipers, 15,000 frames of menus and fights:

- VDP1: a 352x224 framebuffer, erased and swapped every frame. About 544 commands a frame (most 663):
  88% distorted sprites with RGB textures and Gouraud shading (the characters and stages are textured
  quads), 8% flat polygons, 3% 16-colour sprites (colour bank), a few mesh sprites, scaled sprites and
  lines. Transparent pixels and end codes on, high speed shrink on. No half transparency, no shadow or
  half-luminance modes.
- VDP2: 704x448 interlaced (double density). Cell scroll screens NBG0 (16 colours), NBG1, NBG2 (NBG3
  in menus), and the rotation screen RBG0 with a coefficient table (the floor), rotation parameter
  mode 3. Sprite type 4, mixed RGB and palette. One window (WCTLB 0300). Fixed priorities (sprites 5
  and 6, NBG 1 to 7, RBG0 3 or 5). No colour calculation, no colour offset, no line or vertical cell
  scroll, no mosaic. (Colour offset is used after all: by the title screen and the opening movie.)

## The parts

- `renderer.{h,cpp}`: the renderer the hardware model drives. Its modes:
  - **GPU** (the default in a window; `--video gpu`): nothing is rasterised on the CPU but the VDP2's
    scroll screens. Each frame goes to a GPU backend as a `Frame` (`frame.h`).
  - **software** (the default headless; `--video software`): everything drawn here at the original
    resolution, pixel for pixel what the reference renderer (the hardware model's software renderer)
    gives. The reference draws nothing in this mode.
  - `SATRECOMP_VIDEO=check`: software, with the reference doing everything too, and every layer of
    every line and every frame compared. `=passthrough`: every call handed to the reference.
- `layers.cpp`: the VDP2's scroll screens a line at a time: NBG0-3 (cells or bitmap, scroll, zoom,
  line scroll), RBG0 (both rotation parameter sets, coefficient tables per line or per dot, the
  screen-over modes), and the windows (two rectangles, line window tables, the sprite window, AND/OR).
- `renderer.cpp`, software: the sprite layer from the VDP1 framebuffer (the 16 sprite data formats,
  mixed RGB and palette, priorities, colour RAM, the sprite window) and the composition (highest
  priority shows; ties: sprites, RBG0, NBG0..NBG3; the back screen; sprite shadows; colour offset,
  which the game fades with).
- `raster.cpp`, software: the VDP1's commands rasterised into its framebuffer as the hardware does
  it (quads as lines between two edges, the extra pixel at each step of a slanted line, texture
  stepping with end codes and high speed shrink, Gouraud shading, the four colour calculation modes,
  mesh, MSB on, clipping, erase). The counters that step along edges, lines, textures and shading are
  in `counters.h`: satrecomp's own, counting by the hardware's rules as the reference does.
- `vdp1.cpp`: the VDP1's commands as quads for a GPU: sprites (normal, scaled, distorted), polygons,
  lines and polylines, with system and user clipping and local coordinates; framebuffer erases and
  swaps as events in order.
- `gpu.{h,cpp}`, `shaders/`: the SDL_GPU backend (Metal now; the shaders are generated as SPIR-V too,
  for Vulkan). `quad.frag` draws a quad's pixels into a target several times the original resolution
  that holds what the VDP1's framebuffer holds, a 16-bit value per pixel. It finds each pixel's place
  in the quad by the hardware's own rule (lines from edge A-D to edge B-C: a bilinear stretch) and
  picks the texel the hardware's stepping would (shrinking, stretching, high speed shrink).
  `compose.frag` reads that target as the VDP2 reads the framebuffer and puts it together with the
  VDP2's layers by priority, several samples a pixel where the target has more pixels than the screen.
  It also draws the rotation screen (the floor) itself, at the picture's resolution: the renderer
  gives it, per line of the original, where the line's first and last dots look in the map
  (`Renderer::FloorLine`), and the shader takes positions between the lines in between and reads
  pattern names, cells and colours from video memory. Lines it cannot do that way (a bitmap, RGB
  cells, per-dot coefficients or priorities, windows, both parameter sets) are drawn by the renderer
  into the layer's image as before; `SATRECOMP_FLOOR=image` does that for all of them.
  `tools/gen_shaders.py` (glslangValidator, spirv-cross) writes `shaders.inc`, which is committed.

## What is not done

Counted and reported at exit when a game asks for it.

- VDP2: colour calculation (transparency between layers), the line colour screen,
  mosaic, vertical cell scroll, RBG1, framebuffer rotation. Lines are drawn without the effect.
- GPU: the VDP1 drawing modes that read what is already in the framebuffer (shadow, half
  transparency, MSB on) draw as if replacing; end codes make a pixel transparent but do not cut the
  rest of the line; the game writing or reading the VDP1 framebuffer directly; the sprite window used
  as a window of other layers; 8-bit framebuffers.
- The VDP2's normal screens (NBG0-3) are drawn at the original resolution. Of an interlaced picture the GPU mode draws
  both fields' lines every frame (the normal screens' other lines half a line further on in their
  maps; the rotation screen's lines twice), so moving backgrounds are whole; `SATRECOMP_INTERLACE=1`
  gives a field per frame as the hardware does. Software mode keeps the other field's lines, as the
  original (and the reference) does.

## Checking

- `SATRECOMP_VIDEO=check`: the VDP1 framebuffer after every frame, every layer of every line and
  every picture against the reference. Fighting Vipers: nothing differs over 12,000 frames each of
  two movies (movies/native_71, native_74), and
  the state hashes (which cover the picture) are the same in software mode and passthrough.
- `SATRECOMP_GPU_CHECK=1 --headless --video gpu --internal-scale 1`: the GPU's VDP1 framebuffer
  against the reference rasteriser's, at the original resolution. They cannot be identical (the
  hardware draws slanted lines pixel by pixel, the GPU evaluates each pixel on its own); Fighting
  Vipers, 6,000 frames: of the pixels either drew, 60% identical, 12% within 2/31 per channel
  (shading rounding), 13% another texel, 15% drawn by only one of the two (edges).
  `SATRECOMP_GPU_DUMP=FRAME:PREFIX` writes both framebuffers of a frame.
- The floor drawn by the shader against the floor drawn by the renderer, both shown at the original
  size: 0.4% of pixels differ (the edges of the map's dots, which the shader places more finely).
- Headless GPU runs read the picture back, so `--shot-every N:PREFIX` works with `--video gpu`
  (`--output WxH` for the size). `tools/pngdiff.py DIR_A DIR_B [--field] [--sheet OUT.png]` compares
  two runs' screenshots.

## Widescreen

`--widescreen` (16:9; `--aspect W:H` for another shape; the launcher's "Widescreen" switch), with the GPU
renderer only. The picture keeps its height and shows more at both sides: for 16:9, 59 more of the VDP1's
pixels each side (470 instead of 352).

Nothing in the game had to change for the view itself. Its 3D code (the vertex transform at 0x0602331C and
the polygon list builders around 0x060230B4) culls by depth and facing only, not by the screen's left and
right edges, and leaves that to the VDP1's clipping; so the renderer widens the drawing area and everything
the game sends beyond the old edges simply shows:

- VDP1: every command is moved right by the extra columns; clipping at the hardware picture's left or right
  edge becomes clipping at the wider area's; an erase that covers the picture covers the wider one.
- Scroll screens: each line starts that much further left in its map and runs that much longer. Windows
  keep their place; beyond the picture's edges they are as at the edge.
- The floor (rotation screen): each line goes on at the same step beyond both ends, whether the GPU draws
  it from the map or it is drawn here dot by dot (a floor with one character repeated outside its map, say);
  a coefficient given for each dot stays beyond the ends as it is at them.
- A scene without the floor (menus, the opening movie, score tables) is not meant to be seen beyond the
  hardware's picture: it is shown as it is, with black at the sides.

Known limits: the gauges and timer stay where they are (in the middle 4:3); anything the game itself would
leave out because it is off the 4:3 picture would be missing at the sides (none seen so far); the software
renderer has no widescreen.
