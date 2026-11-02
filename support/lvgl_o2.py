# KF.31: build LVGL's software renderer at -O2 instead of the framework's -Os.
# build_src_flags doesn't reach libraries, so this is a build middleware. It takes
# the env of whatever is being built, because LVGL finds lv_conf.h through its
# library env's include paths, which the project env doesn't have.
Import("env")


def lvgl_draw_sw_o2(build_env, node):
    flags = [f for f in build_env["CCFLAGS"] if f not in ("-Os", "-Og", "-O0", "-O1")] + ["-O2"]
    return build_env.Object(node, CCFLAGS=flags)


env.AddBuildMiddleware(lvgl_draw_sw_o2, "*/lvgl/src/draw/sw/*")
