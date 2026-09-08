# scrcpy Server Patch

The bundled server is built from upstream scrcpy v4.1, commit
`2926c06c5dc3064ae6d8db706f1a98a37cfcf3f0`, under its Apache-2.0 license.
`manual-clipboard.patch` makes an explicit GET_CLIPBOARD with COPY_KEY_NONE
return the current clipboard even when clipboard autosync is enabled.
COPY/CUT shortcuts keep their existing autosync behavior.

The desktop uses this reply to confirm manual reads and verify writes. No Agent
APK, IME selection or clipboard polling is needed. A null/non-text Android
clipboard may not produce a reply; the desktop reports a timeout in that case.

Rebuild on Windows with JDK 17 (`JAVA_HOME`), Android platform 36 and Build Tools
35 or newer. Clone the pinned upstream revision into a disposable source folder,
then run `build-server.ps1` with `SourceDir`, `AndroidJar`, `FrameworkAidl`,
`BuildToolsDir`, and a new `BuildDir`. It applies the patch, generates AIDL stubs,
compiles Java and DEX, and replaces `src/third_party/scrcpy-server`.
Commit the patch, builder and generated binary together.
