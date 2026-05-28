# PlatformIO: C++ only flags (--std gnu++17, exceptions/RTTI) must not be passed to C compilers.
Import("env")

env.Append(CXXFLAGS=["-std=gnu++17", "-fno-exceptions", "-fno-rtti"])
