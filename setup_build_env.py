import os
import sys
import subprocess
import urllib.request
import tarfile
import shutil
import time

# Configuration
MSYS2_URL = "https://repo.msys2.org/distrib/x86_64/msys2-base-x86_64-20240113.tar.xz"
INSTALL_DIR = os.path.join(os.getcwd(), "env")
MSYS2_DIR = os.path.join(INSTALL_DIR, "msys64")
BASH = os.path.join(MSYS2_DIR, "usr", "bin", "bash.exe")

def log(msg):
    print(f"[SETUP] {msg}")

def run_bash(cmd, check=True):
    full_cmd = [BASH, "-lc", cmd]
    return subprocess.run(full_cmd, check=check, env=os.environ.copy())

def main():
    if not os.path.exists(INSTALL_DIR):
        os.makedirs(INSTALL_DIR)

    # 1. Check if MSYS2 is already installed
    if os.path.exists(MSYS2_DIR) and os.path.exists(BASH):
        log("MSYS2 environment found.")
    else:
        # 2. Download MSYS2
        tar_path = os.path.join(INSTALL_DIR, "msys2.tar.xz")
        if not os.path.exists(tar_path):
            log(f"Downloading MSYS2 from {MSYS2_URL}...")
            try:
                urllib.request.urlretrieve(MSYS2_URL, tar_path)
            except Exception as e:
                log(f"Failed to download: {e}")
                sys.exit(1)
        
        # 3. Extract MSYS2
        log("Extracting MSYS2 (this may take a while)...")
        try:
            with tarfile.open(tar_path, "r:xz") as tar:
                tar.extractall(path=INSTALL_DIR)
        except Exception as e:
            log(f"Failed to extract: {e}")
            sys.exit(1)
            
        # Cleanup tar
        os.remove(tar_path)
        log("Extraction complete.")

    # 4. Initialize and Update
    # First run to initialize keys and basic setup
    log("Initializing MSYS2...")
    run_bash("exit") # dummy run to invoke first-time setup
    
    # Update package database
    log("Updating package database...")
    run_bash("pacman -Sy --noconfirm")

    # 5. Install Dependencies
    pkgs = [
        "mingw-w64-ucrt-x86_64-clang",
        "mingw-w64-ucrt-x86_64-lld",
        "mingw-w64-ucrt-x86_64-toolchain", # Includes make, etc
        "base-devel",
    ]
    
    log(f"Installing packages: {', '.join(pkgs)}...")
    # Using --needed to skip already installed
    run_bash(f"pacman -S --needed --noconfirm {' '.join(pkgs)}")

    log("Environment setup complete.")

if __name__ == "__main__":
    main()
