"""Generated CMake compatibility policy; never modify dependency sources."""
import shlex

_WINDOWS_XP_CONF = "user.jce:windows_xp"
_PHYSFS_FLOOR_CONF = "user.jce:physfs_win32_floor"
_PHYSFS_FLOOR_CONFIG_CONF = "user.jce:physfs_win32_floor_config"
_PHYSFS_FLOOR = "0x0601"
_PHYSFS_FLOOR_CONFIG = "directory-flags-v1"
_PHYSFS_XP_FLOOR_CONFIG = "directory-flags-xp-v1"
_PHYSFS_MACROS = ("_WIN32_WINNT", "WINVER")
_FLECS_XP_CONFIG_CONF = "user.jce:flecs_xp_config"
_FLECS_XP_CONFIG = "no-http-rest-default-os-v1"
_FLECS_XP_MACROS = ("FLECS_NO_HTTP", "FLECS_NO_REST", "FLECS_NO_OS_API_IMPL")
_FLECS_XP_FORBIDDEN = ("FLECS_HTTP", "FLECS_REST", "FLECS_OS_API_IMPL", "FLECS_CUSTOM_BUILD")


def _cmake_value(value):
    return value.get("value") if isinstance(value, dict) else value


def _physfs_windows_desktop(conanfile, settings):
    if conanfile.name != "physfs" or str(settings.get_safe("os")) != "Windows":
        return False
    variables = conanfile.conf.get("tools.cmake.cmaketoolchain:extra_variables",
                                   default={}, check_type=dict)
    system = _cmake_value(variables.get("CMAKE_SYSTEM_NAME",
        conanfile.conf.get("tools.cmake.cmaketoolchain:system_name", default="Windows")))
    return system == "Windows"


def _physfs_floor_configuration(conanfile, settings):
    if conanfile.name != "physfs":
        return None
    xp = conanfile.conf.get(_WINDOWS_XP_CONF, default=False)
    if not isinstance(xp, bool):
        raise ValueError("user.jce:windows_xp requires a boolean")
    if xp:
        if not _physfs_windows_desktop(conanfile, settings):
            raise ValueError("PhysFS Windows XP floor requires desktop Windows")
        # The C-only PhysFS recipe removes cppstd/libcxx before package_id.
        # Full GNU/libc++ profile admission belongs to the shared build driver.
        if (str(settings.get_safe("compiler")) != "clang"
                or settings.get_safe("compiler.libcxx") not in (None, "", "libc++")):
            raise ValueError("PhysFS Windows XP floor requires GNU Clang with libc++")
        frontend = settings.get_safe("compiler.frontend")
        if frontend not in (None, "") and str(frontend).lower() != "gnu":
            raise ValueError("PhysFS Windows XP floor requires the GNU Clang frontend")
        for key in ("compiler.runtime", "compiler.runtime_type", "compiler.runtime_version"):
            if settings.get_safe(key) not in (None, ""):
                raise ValueError("PhysFS Windows XP floor refuses the MSVC runtime ABI")
        arch = str(settings.get_safe("arch"))
        if arch not in ("x86", "x86_64"):
            raise ValueError("PhysFS Windows XP floor requires x86 or x86_64")
        floor = {"x86": "0x0501", "x86_64": "0x0502"}[arch]
        config = _PHYSFS_XP_FLOOR_CONFIG
    else:
        if not _physfs_windows_desktop(conanfile, settings):
            return None
        # Windows on ARM64 starts at Windows 10. Keep the ARM64 profile's
        # existing 0x0A00 compile floor; other desktop targets retain 0x0601.
        # This is PhysFS-scoped, not a relaxation of all Conan flags.
        floor, config = (("0x0A00", "directory-flags-arm64-v1")
                         if str(settings.get_safe("arch")) == "armv8"
                         else (_PHYSFS_FLOOR, _PHYSFS_FLOOR_CONFIG))
    for key, expected in ((_PHYSFS_FLOOR_CONF, floor), (_PHYSFS_FLOOR_CONFIG_CONF, config)):
        if conanfile.conf.get(key, default=expected) != expected:
            raise ValueError("PhysFS Windows floor conflicts with " + key)
    return floor, config


def _reserved_flag_names(flags, floor=_PHYSFS_FLOOR,
                         macros=_PHYSFS_MACROS, label="PhysFS Windows floor"):
    if any(not isinstance(flag, str) for flag in flags):
        raise ValueError(label + " requires string compiler flags")
    tokens = shlex.split(" ".join(flags), posix=False)
    found = set()
    for index, token in enumerate(tokens):
        token = token.strip("\"'")
        if token[:2] not in ("-D", "/D", "-U", "/U"):
            continue
        operand = token[2:] or (tokens[index + 1] if index + 1 < len(tokens) else "")
        name, equals, value = operand.strip("\"'").partition("=")
        if name in macros:
            # Exact tokens avoid introducing redefinition warnings alongside
            # caller flags with a different textual spelling of the same value.
            if token[1] != "D" or not equals or value != floor:
                raise ValueError(label + " conflicts with " + name)
            found.add(name)
    return found


def _validate_physfs_floor(conanfile, floor=_PHYSFS_FLOOR):
    for key in ("tools.build:cflags", "tools.build:cxxflags"):
        _reserved_flag_names(conanfile.conf.get(key, default=[], check_type=list), floor)
    for definition in conanfile.conf.get("tools.build:defines", default=[], check_type=list):
        if not isinstance(definition, str):
            raise ValueError("PhysFS Windows floor requires string directory defines")
        name, equals, value = definition.partition("=")
        if name.strip() in _PHYSFS_MACROS and (not equals or value != floor):
            raise ValueError("PhysFS Windows floor conflicts with directory define " + name)
    variables = conanfile.conf.get("tools.cmake.cmaketoolchain:extra_variables",
                                   default={}, check_type=dict)
    for name, value in variables.items():
        value = _cmake_value(value)
        if name.startswith(("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS")):
            _reserved_flag_names([value], floor)
        elif name in _PHYSFS_MACROS and value != floor:
            raise ValueError("PhysFS Windows floor conflicts with CMake variable " + name)


def _flecs_xp_configuration(conanfile, settings):
    if conanfile.name != "flecs":
        return False
    xp = conanfile.conf.get(_WINDOWS_XP_CONF, default=False)
    if not isinstance(xp, bool):
        raise ValueError("user.jce:windows_xp requires a boolean")
    if not xp:
        return False
    variables = conanfile.conf.get("tools.cmake.cmaketoolchain:extra_variables",
                                   default={}, check_type=dict)
    system = _cmake_value(variables.get("CMAKE_SYSTEM_NAME",
        conanfile.conf.get("tools.cmake.cmaketoolchain:system_name", default="Windows")))
    if str(settings.get_safe("os")) != "Windows" or system != "Windows":
        raise ValueError("Flecs XP configuration requires desktop Windows")
    # The C-only recipe removes libcxx/cppstd before package_id.
    if (str(settings.get_safe("compiler")) != "clang"
            or settings.get_safe("compiler.libcxx") not in (None, "", "libc++")):
        raise ValueError("Flecs XP configuration requires GNU Clang with libc++")
    frontend = settings.get_safe("compiler.frontend")
    if frontend not in (None, "") and str(frontend).lower() != "gnu":
        raise ValueError("Flecs XP configuration requires the GNU frontend")
    for key in ("compiler.runtime", "compiler.runtime_type", "compiler.runtime_version"):
        if settings.get_safe(key) not in (None, ""):
            raise ValueError("Flecs XP configuration refuses the MSVC runtime ABI")
    if str(settings.get_safe("arch")) not in ("x86", "x86_64"):
        raise ValueError("Flecs XP configuration requires x86 or x86_64")
    if conanfile.conf.get(_FLECS_XP_CONFIG_CONF, default=_FLECS_XP_CONFIG) != _FLECS_XP_CONFIG:
        raise ValueError("Flecs XP configuration revision conflicts")
    return True


def _flecs_xp_flag_names(flags):
    _reserved_flag_names(flags, None, _FLECS_XP_FORBIDDEN, "Flecs XP configuration")
    return _reserved_flag_names(flags, "1", _FLECS_XP_MACROS, "Flecs XP configuration")


def _validate_flecs_xp(conanfile):
    for key in ("tools.build:cflags", "tools.build:cxxflags"):
        _flecs_xp_flag_names(conanfile.conf.get(key, default=[], check_type=list))
    for definition in conanfile.conf.get("tools.build:defines", default=[], check_type=list):
        if not isinstance(definition, str):
            raise ValueError("Flecs XP configuration requires string directory defines")
        name, equals, value = definition.partition("=")
        if name.strip() in _FLECS_XP_FORBIDDEN or (name.strip() in _FLECS_XP_MACROS
                and (not equals or value != "1")):
            raise ValueError("Flecs XP configuration conflicts with directory define " + name)
    variables = conanfile.conf.get("tools.cmake.cmaketoolchain:extra_variables",
                                   default={}, check_type=dict)
    for name, value in variables.items():
        value = _cmake_value(value)
        if name.startswith(("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS")):
            _flecs_xp_flag_names([value])
        elif name in _FLECS_XP_FORBIDDEN or (name in _FLECS_XP_MACROS and value != "1"):
            raise ValueError("Flecs XP configuration conflicts with CMake variable " + name)


def _apply_flecs_xp(conanfile):
    for key in ("tools.build:cflags", "tools.build:cxxflags"):
        compiler_flags = list(conanfile.conf.get(key, default=[], check_type=list))
        present = _flecs_xp_flag_names(compiler_flags)
        for name in _FLECS_XP_MACROS:
            flag = "-D" + name + "=1"
            if name not in present:
                compiler_flags.append(flag)
        conanfile.conf.define(key, compiler_flags)
    directory_macros = list(conanfile.conf.get("tools.build:defines", default=[], check_type=list))
    for name in _FLECS_XP_MACROS:
        definition = name + "=1"
        if definition not in directory_macros:
            directory_macros.append(definition)
    conanfile.conf.define("tools.build:defines", directory_macros)


def post_package_id(conanfile):
    # Conan forbids access to live self.settings at this lifecycle boundary.
    if conanfile.name == "flecs":
        if _flecs_xp_configuration(conanfile, conanfile.info.settings):
            _validate_flecs_xp(conanfile)
            conanfile.info.conf.define(_FLECS_XP_CONFIG_CONF, _FLECS_XP_CONFIG)
        return
    if conanfile.name != "physfs":
        return
    selection = _physfs_floor_configuration(conanfile, conanfile.info.settings)
    if selection is None:
        return
    floor, config = selection
    _validate_physfs_floor(conanfile, floor)
    conanfile.info.conf.define(_PHYSFS_FLOOR_CONF, floor)
    conanfile.info.conf.define(_PHYSFS_FLOOR_CONFIG_CONF, config)


def pre_generate(conanfile):
    flecs_xp = _flecs_xp_configuration(conanfile, conanfile.settings)
    if flecs_xp:
        _validate_flecs_xp(conanfile)
    selection = _physfs_floor_configuration(conanfile, conanfile.settings)
    if selection is not None:
        _validate_physfs_floor(conanfile, selection[0])
    key = "tools.cmake.cmaketoolchain:extra_variables"
    variables = dict(conanfile.conf.get(key, default={}, check_type=dict))
    variables.setdefault("CMAKE_POLICY_VERSION_MINIMUM", "3.5")
    conanfile.conf.define(key, variables)
    if flecs_xp:
        _apply_flecs_xp(conanfile)
    if selection is None:
        return
    floor, _ = selection
    for key in ("tools.build:cflags", "tools.build:cxxflags"):
        flags = list(conanfile.conf.get(key, default=[], check_type=list))
        present = _reserved_flag_names(flags, floor)
        for name in _PHYSFS_MACROS:
            definition = "-D" + name + "=" + floor
            if name not in present:
                flags.append(definition)
        conanfile.conf.define(key, flags)
    defines = list(conanfile.conf.get("tools.build:defines", default=[], check_type=list))
    for name in _PHYSFS_MACROS:
        definition = name + "=" + floor
        if definition not in defines:
            defines.append(definition)
    conanfile.conf.define("tools.build:defines", defines)


def post_package_info(conanfile):
    if conanfile.name != "flecs" or not _flecs_xp_configuration(conanfile, conanfile.settings):
        return
    _validate_flecs_xp(conanfile)
    targets = [conanfile.cpp_info]
    targets.extend(conanfile.cpp_info.components.values())
    for target in targets:
        for name in _FLECS_XP_MACROS:
            define = name + "=1"
            if define not in target.defines:
                target.defines.append(define)
