"""
check_sdkconfig_local.py -- PlatformIO pre-build script.

Ensures sdkconfig.defaults.local exists before ESP-IDF's CMake tries to
read it.  If the file is missing, an empty one is created and the user is
warned to fill it in.  This prevents a hard CMake error on first checkout
while still making the missing-credentials problem obvious at build time
rather than silently at runtime.
"""

import os
Import("env")  # noqa: F821  (PlatformIO SConstruct global)

project_dir  = env["PROJECT_DIR"]
local_file   = os.path.join(project_dir, "sdkconfig.defaults.local")
example_file = local_file + ".example"

if not os.path.exists(local_file):
    print()
    print("=" * 70)
    print("  WARNING: sdkconfig.defaults.local not found.")
    print()
    print("  WiFi credentials and EFM URLs are missing.")
    print("  The firmware will compile but the device won't connect at runtime.")
    print()
    print("  To fix:")
    print(f"    cp sdkconfig.defaults.local.example sdkconfig.defaults.local")
    print(f"    # then edit sdkconfig.defaults.local with your SSID, password,")
    print(f"    # and EFM endpoint URLs.")
    print("=" * 70)
    print()
    # Create an empty file so CMake doesn't error on the missing path.
    open(local_file, "w").close()
