# Removes Gridline with its own uninstaller (silent).
$ErrorActionPreference = 'Stop'
[array]$keys = Get-UninstallRegistryKey -SoftwareName 'Gridline*'
foreach ($key in $keys) {
  if ($key.UninstallString) {
    Uninstall-ChocolateyPackage -PackageName $env:ChocolateyPackageName -FileType 'exe' `
      -SilentArgs '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' -File ($key.UninstallString -replace '"', '')
  }
}
