# Winget packaging

The manifests here are the source of truth for the
`minhnd410.computer-control` package. They are submitted to
[microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs) by opening a
PR that copies this directory to
`manifests/m/minhnd410/computer-control/<version>/`.

The release workflow renders these templates for each published release,
validates them with `winget validate`, updates the `minhnd410/winget-pkgs`
fork, and opens or updates a pull request against `microsoft/winget-pkgs`.

The workflow needs a repository secret named `WINGET_PKGS_TOKEN`. It must be
able to push branches to `minhnd410/winget-pkgs` and create pull requests in
`microsoft/winget-pkgs`. Without that secret, the release succeeds but the
Winget job records that it was skipped.

To validate the checked-in templates locally:

```powershell
winget validate --manifest packaging\winget
winget install --manifest packaging\winget
```

The release workflow regenerates `InstallerSha256` from the published Windows
archive. For a manual check:

```powershell
(Get-FileHash .\computer-control-windows-x86_64.zip -Algorithm SHA256).Hash
```
