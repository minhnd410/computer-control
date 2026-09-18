# winget packaging

The manifests here are the source of truth for the
`minhnd410.computer-control` package. They are submitted to
[microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs) by opening a
PR that copies this directory to
`manifests/m/minhnd410/computer-control/<version>/`.

**Not yet submitted.** Submission requires a tagged release with a stable
download URL and a SHA256, and there is no release yet. Until then, install on
Windows by building from source — see the main README.

To validate locally once a release exists:

```powershell
winget validate --manifest packaging\winget
winget install --manifest packaging\winget   # installs from the local manifest
```

The `InstallerSha256` must be regenerated per release:

```powershell
(Get-FileHash .\computer-control-windows-x86_64.zip -Algorithm SHA256).Hash
```
