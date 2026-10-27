Import("env")
import sys
import os
import glob
import subprocess

if sys.platform.startswith("darwin"):
    # Homebrew's old `sdl2` formula (Cellar/sdl2) or its replacement
    # `sdl2-compat` (Cellar/sdl2-compat, SDL2 API over SDL3).
    sdl_include = glob.glob("/opt/homebrew/Cellar/sdl2*/*/include")
    sdl_libdir = [os.path.dirname(p) for p in
                  glob.glob("/opt/homebrew/Cellar/sdl2*/*/lib/libSDL2.*")]
    if not sdl_include:
        # Anything else that ships sdl2-config (MacPorts, a custom prefix).
        try:
            prefix = subprocess.check_output(["sdl2-config", "--prefix"], text=True).strip()
            sdl_include = [os.path.join(prefix, "include")]
            sdl_libdir = [os.path.join(prefix, "lib")]
        except (OSError, subprocess.CalledProcessError):
            pass
    if sdl_include:
        print(f"Found SDL include path: {sdl_include[0]}")
        env.Append(
            CPPPATH=sdl_include[0]
        )
    if sdl_libdir:
        print(f"Found SDL lib path: {sdl_libdir[0]}")
        env.Append(
            LIBPATH=sdl_libdir[0]
        )
