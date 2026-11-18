# Third-party notices

16bit Forge is MIT-licensed (see LICENSE). It uses or contains the components below. Their
licenses are reproduced as they require.

| Component | Used as | License |
|---|---|---|
| [SDL 3](https://github.com/libsdl-org/SDL) (release-3.4.20) | statically linked into the program | zlib |
| ... inside SDL: yuv2rgb (Adrien Descamps) | part of the SDL build | BSD-3-Clause |
| ... inside SDL: HIDAPI | part of the SDL build (gamepads) | BSD-3-Clause (chosen from HIDAPI's license options) |
| [Dear ImGui](https://github.com/ocornut/imgui) v1.93.0 | statically linked into the program | MIT |
| Hyllian's xBR-lv2 shader | ported to HLSL in `shaders/xbr_lv2.frag.hlsl` | MIT |
| ScaleFX (Sp00kyFox) | ported to HLSL in `shaders/scalefx_pass0.frag.hlsl` ... `scalefx_pass4.frag.hlsl` | MIT |
| [font8x8](https://github.com/dhepper/font8x8) (Daniel Hepper, from Marcel Sondaar's font8x8 / IBM VGA fonts) | 8x8 glyphs for on-screen messages in `src/frontend/font8x8.hpp` | public domain |
| CRT shader by Timothy Lottes | ported to HLSL in `shaders/crt_lottes.frag.hlsl` (its curvature replaced by our own) | public domain |
| [gilyon/snes-tests](https://github.com/gilyon/snes-tests) | SPC700 test vectors in `tests/apu_tests.cpp` (tests only, not in the program) | MIT |
| [SDL_shadercross](https://github.com/libsdl-org/SDL_shadercross), DirectXShaderCompiler, SPIRV-Cross, SPIRV-Tools | build-time tool only; nothing from them is shipped | zlib / University of Illinois NCSA / Apache-2.0 |
| Emerald engine (Ervin Ashley) | `cmake/Shaders.cmake`, `tools/shadercross/`, parts of the shaders and the GPU backend | MIT, same author |

Hardware data tables (the S-DSP Gaussian table, envelope rates, the S-DD1 and SPC7110
probability tables, the Cx4 math table formulas) are facts about the hardware, taken from the
public documentation credited in README.md.

The 64-byte SPC700 boot ROM (IPL) in `src/snes/apu/ipl_rom.hpp` is a dump of the sound chip's
mask ROM (Sony). It is included for compatibility, as in other SNES emulators, and is not
covered by the MIT license.

The NTSC composite / S-Video filter (`shaders/ntsc.frag.hlsl`), the screen curvature and
rounded corners in `shaders/crt_lottes.frag.hlsl`, and the filter chain itself are original
code (MIT), written from the published NTSC signal math and SNES video timing. No code from
other NTSC, CRT-geometry or pixel-art filters is used; in particular nothing from blargg's
snes_ntsc (LGPL), crt-geom, GTU or OmniScale (GPL).

The 16bit Forge logo (`assets/branding/16bitForge_logo_512.png`) and the icons made from it
(`assets/icons/`) are project artwork provided by Ervin Ashley, (c) 2026 Ervin Ashley.

No game ROMs and no coprocessor firmware are included.

---

## SDL 3 (zlib)

```
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## yuv2rgb, part of SDL (BSD-3-Clause)

```
Copyright (c) 2016, Adrien Descamps
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of yuv2rgb nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## HIDAPI, part of SDL (BSD-3-Clause)

```
Copyright (c) 2010, Alan Ott, Signal 11 Software
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Signal 11 Software nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

## Dear ImGui (MIT)

```
The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Hyllian's xBR-lv2 shader (MIT)

```
Hyllian's xBR-lv2 Shader

Copyright (C) 2011-2022 Hyllian - sergiogdb@gmail.com

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the
following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

Incorporates some of the ideas from SABR shader. Thanks to Joshua Street.
```

## ScaleFX (MIT)

From libretro slang-shaders, `edge-smoothing/scalefx/shaders/scalefx-pass0.slang` to
`scalefx-pass4.slang` (each pass carries the same notice):

```
ScaleFX
by Sp00kyFox, 2017-03-01

Copyright (c) 2016 Sp00kyFox - ScaleFX@web.de

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## CRT shader by Timothy Lottes (public domain)

From libretro slang-shaders, `crt/shaders/crt-lottes.slang`. No license terms; its notice:

```
PUBLIC DOMAIN CRT STYLED SCAN-LINE SHADER

  by Timothy Lottes

This is more along the style of a really good CGA arcade monitor.
With RGB inputs instead of NTSC.
The shadow mask example has the mask rotated 90 degrees for less chromatic aberration.

Left it unoptimized to show the theory behind the algorithm.

It is an example what I personally would want as a display option for pixel art games.
Please take and use, change, or whatever.
```

## gilyon/snes-tests (MIT)

```
MIT License

Copyright (c) 2023 gilyon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
