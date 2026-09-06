"""Keep OTA authentication out of normal PlatformIO logs."""

Import("env")

if env.subst("$UPLOAD_PROTOCOL") == "espota":
    env.Replace(
        UPLOADERFLAGS=[
            flag for flag in env.get("UPLOADERFLAGS", []) if flag != "--debug"
        ]
    )
