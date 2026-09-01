# Apple Screen Sharing AAC-ELD dependency

High Performance Apple Screen Sharing audio uses AAC-ELD with explicit SBR.
The FFmpeg build used by Moonlight does not decode this profile, so the optional
Windows x64 Apple feature loads FDK-AAC 2.0.3 through its public C ABI at
runtime. Normal Moonlight builds do not link or deploy FDK-AAC.

When `MOONLIGHT_ENABLE_APPLE_SCREEN_SHARING=1`, `setup-deps.ps1` downloads the
source for the pinned upstream commit, verifies its SHA-256 digest, builds a
shared x64 library with the Visual Studio 2022 toolchain, and keeps it outside
the normal dependency wildcard. `scripts/build-arch.bat` copies the DLL only
for an enabled x64 build. The deployed application also contains the exact
source archive and FDK-AAC `NOTICE` file.

FDK-AAC's copyright license permits source and binary redistribution subject to
the conditions in its `NOTICE`, including providing the complete source with a
binary redistribution. It expressly grants no AAC patent license. Anyone
shipping an Apple-enabled build is responsible for confirming that its use and
distribution are covered by the necessary patent rights.

Upstream: <https://github.com/mstorsjo/fdk-aac>

Pinned source commit: `716f4394641d53f0d79c9ddac3fa93b03a49f278`

Source archive SHA-256:
`C8DB4B4C335D0A6711EA82B0A577AB62FC1578187C391CFE02B2C64AB29F6D2A`
