$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$py = Join-Path $PSScriptRoot ".venv\Scripts\python.exe"
if (-not (Test-Path $py)) {
    $pythonLauncher = Get-Command py -ErrorAction SilentlyContinue
    if ($pythonLauncher) {
        & $pythonLauncher.Source -3.12 -m venv .venv
    } else {
        $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
        if (-not $pythonCommand) {
            throw "Python 3.12 is required. Install Python and reopen PowerShell."
        }
        & $pythonCommand.Source -m venv .venv
    }
    if ($LASTEXITCODE -ne 0) { throw "Could not create the Python virtual environment." }
}

& $py -m pip install -r requirements.txt
if ($LASTEXITCODE -ne 0) { throw "Dependency installation failed." }
& $py ml_api\train_model.py
exit $LASTEXITCODE
