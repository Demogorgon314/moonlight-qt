# Agent Build Notes

These notes apply to the entire repository.

## Windows x64 release build

The repository is a qmake project. On this workstation, use the MSVC Qt kit, not
the MinGW kit.

Required tools and known paths:

- Qt: `C:\Qt\6.11.2\msvc2022_64`
- Visual Studio 2022: `D:\Softwares\VisualStudio`
- Working Python: `C:\Users\kwang\AppData\Local\Programs\Python\Python312\python.exe`
- 7-Zip: `C:\Program Files\7-Zip\7z.exe`

The default `python` command may resolve to a broken pyenv shim. Put the working
Python directory first in `PATH`, or invoke that `python.exe` explicitly.

Initialize submodules in a fresh checkout:

```powershell
git submodule update --init --recursive
```

Download the prebuilt Windows dependencies when `libs\windows` is missing or
outdated:

```powershell
.\setup-deps.ps1
```

Be aware that `setup-deps.ps1` clears `libs\windows` before downloading both the
x64 and ARM64 dependency archives.

The build script's actual argument order is configuration first, architecture
second. The correct x64 release command is:

```cmd
scripts\build-arch.bat release x64
```

Do not follow the reversed `x64 release` order shown in some README revisions.

## Codex duplicate PATH pitfall

The Codex desktop process on this workstation may contain two distinct Windows
environment entries named `PATH` and `Path`. `vcvarsall.bat` updates one entry,
while jom child processes may read the other. The symptom is:

```text
[vcvarsall.bat] Environment initialized for: 'x64'
'cl' is not recognized as an internal or external command
```

Seeing `cl.exe` from `where cl` in the parent shell does not rule out this issue.
Launch the build through a sanitized `ProcessStartInfo` environment:

```powershell
$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = 'cmd.exe'
$psi.UseShellExecute = $false
$psi.Environment.Remove('Path') | Out-Null
$psi.Environment['PATH'] = 'C:\Qt\6.11.2\msvc2022_64\bin;C:\Users\kwang\AppData\Local\Programs\Python\Python312;C:\Program Files\7-Zip;C:\Windows\System32;C:\Windows;C:\Program Files\Git\cmd'
$psi.WorkingDirectory = (Get-Location).Path
$psi.ArgumentList.Add('/d')
$psi.ArgumentList.Add('/c')
$psi.ArgumentList.Add('scripts\build-arch.bat release x64')
$process = [System.Diagnostics.Process]::Start($psi)
$process.WaitForExit()
if ($process.ExitCode -ne 0) { throw "Build failed with exit code $($process.ExitCode)" }
```

In an ordinary Developer Command Prompt without duplicate environment entries,
adding the Qt MSVC bin directory to `PATH` and running the build script directly
should be sufficient.

## Git ownership warning

Git may reject this checkout as having dubious ownership. Do not modify global
Git configuration just to inspect it. For one-off read-only commands, use:

```cmd
git -c safe.directory=G:/Github/moonlight-qt status
```

## Expected artifacts

The main compiled executable is normally written to:

```text
build\build-x64-release\app\release\Moonlight.exe
```

The deployed application directory and portable package are normally written to:

```text
build\deploy-x64-release
build\installer-x64-release\Moonlight-VPlus-Portable-x64-<version>.zip
```

Verify the portable ZIP with `7z t` before reporting success. Build outputs,
downloaded libraries, and generated Makefiles are ignored; do not commit them.

## Tests and documentation

- Add tests for realistic observable regressions, non-trivial invariants or
  boundaries, and concrete bugs. Code changing or coverage increasing is not
  sufficient justification by itself.
- Prefer existing coverage at the behavior boundary. Avoid tests that mirror
  literals, mappings, obvious control flow, implementation details, or removed
  features unless absence is itself a contract. For concurrency, prefer
  deterministic coordination or controlled scheduling over sleeps when
  practical.
- Comments should explain non-obvious rationale, invariants, safety constraints,
  or external quirks rather than restating code. Public API documentation should
  describe observable contracts, not incidental implementation details.
