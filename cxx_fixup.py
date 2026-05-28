# PlatformIO post-hook: GCC applies `-Wno-old-style-declaration` to C++, but GCC 15 rejects
# that switch for C++. ESP-IDF / toolchain merges it into CXXFLAGS — strip before link.
Import("env")


def _strip_c_only_warning_opts(flags):
    if not flags:
        return []
    bad = ("old-style-declaration",)
    result = []
    for f in flags:
        sf = str(f)
        if any(b in sf for b in bad):
            continue
        result.append(f)
    return result


env.Replace(CXXFLAGS=_strip_c_only_warning_opts(env.get("CXXFLAGS")))
