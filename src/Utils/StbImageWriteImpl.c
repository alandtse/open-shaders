/* Single translation unit that emits the stb_image_write implementation, including
   ReShade's HDR-PNG extension (stb_image_write_hdr_png.h). Keep this the ONLY place
   STB_IMAGE_WRITE_IMPLEMENTATION is defined.

   Compiled as C (not C++), matching upstream ReShade's deps/stb_impl.c: the HDR-PNG
   extension passes `unsigned char*` where stb's stbiw__encode_png_line takes
   `signed char*`, which is legal in C but a hard error in C++. CMake skips the C++ PCH
   and quiets warnings for this third-party TU; the vendored header stays byte-identical
   to upstream so updates remain a clean drop-in (see extern/.../PROVENANCE.md).

   stb_image.h precedes the others for the stbi_us typedef; stb_image.h/stb_image_write.h
   come from vcpkg `stb`, stb_image_write_hdr_png.h is vendored under extern/. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>
#include <stb_image_write_hdr_png.h>
