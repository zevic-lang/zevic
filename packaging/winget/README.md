# winget

`winget install zevic-lang.Zevic` installs the compiler and, through `Dependencies`, `LLVM.LLVM` (clang).
The templates here (`*.yaml`) hold the placeholders `__VERSION__`, `__SHA256__`, `__DATE__`; `render.sh` fills them in:

    sh packaging/winget/render.sh 0.4.1 <sha256 of the setup exe> out-dir

The result goes to `manifests/z/zevic-lang/Zevic/<version>/` in https://github.com/microsoft/winget-pkgs.
Check it locally with `winget validate --manifest <dir>` and `winget install --manifest <dir>`.

LLVM alone is not enough to link on Windows: clang also needs the MSVC libraries (Visual Studio Build Tools).
winget has no stable package id for that, so `zevic` prints the instruction when it cannot link.
