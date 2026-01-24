import os
import sys
import subprocess
import urllib.request
import zipfile
import shutil
import json
from pathlib import Path

# --- CONFIGURATION ---
ZIG_VERSION = "0.13.0"
ZIG_URL = f"https://ziglang.org/download/{ZIG_VERSION}/zig-windows-x86_64-{ZIG_VERSION}.zip"
TOOLS_DIR = Path("tools")
ZIG_DIR = TOOLS_DIR / "zig"
ZIG_EXE = ZIG_DIR / "zig.exe"

SOURCE_FILE = "OBSIndicator.cpp"
EXE_NAME = "OBSIndicator.exe"
ICON_FILE = "icon.ico"
RC_FILE = "resource.rc"
RES_FILE = "resource.res"

def log(msg):
    print(f"[BUILD] {msg}")

def fail(msg):
    print(f"[ERROR] {msg}")
    sys.exit(1)

def ensure_zig():
    if ZIG_EXE.exists():
        return

    log(f"Zig compiler not found. Downloading Zig {ZIG_VERSION}...")
    TOOLS_DIR.mkdir(exist_ok=True)
    
    zip_path = TOOLS_DIR / "zig.zip"
    try:
        urllib.request.urlretrieve(ZIG_URL, zip_path)
    except Exception as e:
        fail(f"Failed to download Zig: {e}")

    log("Extracting Zig...")
    try:
        with zipfile.ZipFile(zip_path, 'r') as zip_ref:
            zip_ref.extractall(TOOLS_DIR)
        
        # Move extracted folder to 'zig'
        extracted_folder = next(TOOLS_DIR.glob(f"zig-windows-x86_64-{ZIG_VERSION}"))
        if ZIG_DIR.exists():
            shutil.rmtree(ZIG_DIR)
        extracted_folder.rename(ZIG_DIR)
        
    except Exception as e:
        fail(f"Failed to extract Zig: {e}")
    finally:
        if zip_path.exists():
            zip_path.unlink()

def check_requirements():
    if not Path(ICON_FILE).exists():
        fail(f"'{ICON_FILE}' not found! Please run 'python generate_icon.py' first.")

def generate_compile_commands(zig_exe_path):
    # Create compile_commands.json for LSP support
    # We use clang++ as the command to ensure standard clangd picks it up,
    # but we add explicit -isystem paths to Zig's bundled headers.
    
    # Use relative paths to avoid absolute paths/usernames in the generated file
    # Paths are relative to the workspace root (where compile_commands.json resides)
    zig_lib = Path("tools") / "zig" / "lib"
    
    includes = [
        zig_lib / "libcxx" / "include",
        zig_lib / "libcxxabi" / "include",
        zig_lib / "libunwind" / "include",
        zig_lib / "include",
        zig_lib / "libc" / "include" / "any-windows-any"
    ]
    
    include_flags = " ".join([f'-isystem "{p}"'.replace("\\", "/") for p in includes])
    
    # Note: We use clang++ here because clangd recognizes it better than zig.exe
    cmd_str = (
        'clang++ '
        "--driver-mode=g++ "
        "-target x86_64-windows-gnu "
        "-std=c++17 "
        "-Os "
        "-DNDEBUG "
        f"{include_flags} "
        f"-c {SOURCE_FILE}"
    )
    
    data = [
        {
            "directory": ".",
            "command": cmd_str,
            "file": SOURCE_FILE
        }
    ]
    
    with open("compile_commands.json", "w") as f:
        json.dump(data, f, indent=2)
    
    log("Generated compile_commands.json")

def build():
    # 1. Generate Resource File
    log("Compiling resources...")
    # Create simple RC file if it doesn't exist or update it
    with open(RC_FILE, "w") as f:
        f.write(f'101 ICON "{ICON_FILE}"\n')

    # Use zig rc (llvm-rc) to compile resources
    # zig rc command: zig rc [options] <file>
    # Note: As of Zig 0.11+, 'zig rc' is available.
    res_cmd = [str(ZIG_EXE), "rc", "/fo", RES_FILE, RC_FILE]
    
    # Run resource compiler
    result = subprocess.run(res_cmd, capture_output=True, text=True)
    if result.returncode != 0:
        # Fallback: try compiling .rc directly with zig c++ if zig rc fails or behaves unexpectedly
        # But standard way is 'zig rc'. Let's see if we need special flags.
        # Windows rc.exe /fo outputs .res. llvm-rc also supports /fo.
        print(result.stderr)
        fail("Resource compilation failed.")

    # 2. Compile C++ Application
    log("Compiling C++ application...")
    
    # Zig c++ acts as a clang driver
    # We use -target x86_64-windows-gnu to link against MinGW/libstdc++ equivalent or 
    # -target x86_64-windows-msvc to link against MSVC CRT.
    # The original build.bat used UCRT64 (MinGW-w64 UCRT).
    # Zig's default for windows is usually native-msvc if MSVC is installed, or gnu.
    # To be self-contained and match 'static' linking, we might want 'x86_64-windows-gnu'.
    # However, Zig ships its own libc/libc++ wrappers.
    # Let's try default target first, or force gnu.
    
    # Flags from build.bat:
    # -std=c++17 -Os -DNDEBUG -mwindows -static -s -ffunction-sections -fdata-sections
    # -fuse-ld=lld -Wl,--gc-sections
    # Libraries: -lws2_32 -lgdi32 -luser32 -lshell32 -ldwmapi -ladvapi32 -lcrypt32 -luxtheme
    
    cmd = [
        str(ZIG_EXE), "c++",
        "-target", "x86_64-windows-gnu", # Force GNU ABI to ensure static linking works as expected with MinGW-style libs if needed
        "-std=c++17",
        "-Os",
        "-DNDEBUG",
        "-mwindows",
        "-Wl,--subsystem,windows",
        "-static",
        "-s", # strip
        "-ffunction-sections",
        "-fdata-sections",
        SOURCE_FILE,
        RES_FILE, # Link the compiled resource
        "-o", EXE_NAME,
        "-lws2_32",
        "-lgdi32",
        "-luser32",
        "-lshell32",
        "-ldwmapi",
        "-ladvapi32",
        "-lcrypt32",
        "-luxtheme",
        "-Wl,--gc-sections"
    ]
    
    # Note: Zig's clang driver might not need -fuse-ld=lld as it uses lld by default.
    
    result = subprocess.run(cmd)
    if result.returncode != 0:
        fail("Compilation failed.")
        
    log(f"Success! Created {EXE_NAME}")
    
    # Generate LSP file
    generate_compile_commands(ZIG_EXE)

def main():
    check_requirements()
    ensure_zig()
    build()

if __name__ == "__main__":
    main()
