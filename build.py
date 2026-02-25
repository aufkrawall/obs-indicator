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
        fail(f"'{ICON_FILE}' not found in repository root.")

def generate_compile_commands():
    # Create compile_commands.json for LSP support
    # We use clang++ so clangd can parse flags reliably.
    
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
    
    cmd_str = (
        'clang++ '
        "--driver-mode=g++ "
        "-target x86_64-windows-gnu "
        "-std=c++17 "
        "-Os "
        "-DNDEBUG "
        "-D_WIN32_WINNT=0x0A00 "
        "-DWINVER=0x0A00 "
        "-DWIN32_LEAN_AND_MEAN "
        "-D_WINSOCK_DEPRECATED_NO_WARNINGS "
        "-D_CRT_SECURE_NO_WARNINGS "
        "-Wall -Wextra -Wpedantic "
        "-Wformat -Wformat-security "
        "-Wno-missing-field-initializers "
        f"{include_flags} "
        f"-c {SOURCE_FILE}"
    )
    
    data = [
        {
            "directory": str(Path.cwd()),
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
    rc_content = f'101 ICON "{ICON_FILE}"\n'
    rc_path = Path(RC_FILE)
    if not rc_path.exists() or rc_path.read_text(encoding="utf-8") != rc_content:
        rc_path.write_text(rc_content, encoding="utf-8")

    # Use zig rc (llvm-rc) to compile resources
    # zig rc command: zig rc [options] <file>
    # Note: As of Zig 0.11+, 'zig rc' is available.
    res_cmd = [str(ZIG_EXE), "rc", "/fo", RES_FILE, RC_FILE]
    
    # Run resource compiler
    result = subprocess.run(res_cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr)
        fail("Resource compilation failed.")

    # 2. Compile C++ Application
    log("Compiling C++ application...")
    
    cmd = [
        str(ZIG_EXE), "c++",
        "-target", "x86_64-windows-gnu",
        "-std=c++17",
        "-Os",
        "-DNDEBUG",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Wformat",
        "-Wformat-security",
        "-Wno-missing-field-initializers",
        "-fstack-protector-strong",
        "-mwindows",
        "-Wl,--subsystem,windows",
        "-Wl,--dynamicbase",
        "-Wl,--high-entropy-va",
        "-Wl,--nxcompat",
        "-static",
        "-s",
        "-ffunction-sections",
        "-fdata-sections",
        SOURCE_FILE,
        RES_FILE,
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
    
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        if result.stdout:
            print(result.stdout)
        if result.stderr:
            print(result.stderr)
        fail("Compilation failed.")
        
    log(f"Success! Created {EXE_NAME}")
    
    # Generate LSP file
    generate_compile_commands()

def main():
    check_requirements()
    ensure_zig()
    build()

if __name__ == "__main__":
    main()
