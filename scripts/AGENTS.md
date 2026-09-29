# scripts/ — manual entry points

These are human-facing Python/native launchers and shell bootstrap helpers.
Automated implementations live in tools/build, tools/lint and tools/audit.
Keep existing entry commands as thin forwarding wrappers; never duplicate a
build recipe here. Native bootstrap helpers may normalize the environment and
finish an interactive run, but must not encode consumer gameplay or assets.
Project-specific defaults and shortcuts belong to examples/<project>/scripts.
Use relative paths and explicit toolchain inputs. BAT/CMD use CRLF on disk;
other first-party scripts use LF. Tool/runtime source is never copied here.
