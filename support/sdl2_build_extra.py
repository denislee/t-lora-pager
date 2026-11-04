Import("env")

exec_name = "${BUILD_DIR}/${PROGNAME}${PROGSUFFIX}"

# Override unused "upload" to execute compiled binary
from SCons.Script import AlwaysBuild
AlwaysBuild(env.Alias("upload", exec_name, exec_name))

# Add custom target to explorer
env.AddTarget(
    name = "execute",
    dependencies = exec_name,
    actions = '"{}"'.format(exec_name),
    title = "Execute",
    description = "Build and execute",
    group="General"
)
