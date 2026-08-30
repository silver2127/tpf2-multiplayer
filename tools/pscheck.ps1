<#
.SYNOPSIS
Flag two bug shapes in the harness scripts that PowerShell accepts silently.

.DESCRIPTION
Both of these have already cost a debugging round in autotest.ps1, and both are
invisible at the point of the mistake -- the script runs, it just does the
wrong thing:

  1. CASE-COLLIDING VARIABLES. Variable names are case-INSENSITIVE, so
         $Ovl = "C:\Sandbox\...\out"     # a directory
         $ovl = Read-Identity $Ovl       # an object -- SAME VARIABLE
     silently replaced the path with an object. Get-SimTicks then handed that
     object to Join-Path and died with DriveNotFoundException, which killed the
     run right after the verdict AND meant the instance-B wedge check had been
     reading $null the whole time. A watchdog that cannot fire is worse than no
     watchdog: the harness reported healthy runs it had not checked.

  2. NESTED ARRAY LITERALS WITHOUT A UNARY COMMA. @( @(a,b), @(c,d) ) FLATTENS
     to a,b,c,d, so $pair[0]/$pair[1] quietly become scalars. Write
     @( ,@(a,b) ; ,@(c,d) ) instead.

    powershell -File tools/pscheck.ps1            # check tools/*.ps1
    powershell -File tools/pscheck.ps1 a.ps1 b.ps1
#>
param([string[]]$Path)

$ErrorActionPreference = "Stop"
if (-not $Path) {
    $Path = @(Get-ChildItem (Join-Path $PSScriptRoot "*.ps1") | ForEach-Object FullName)
}

$bad = 0
foreach ($p in $Path) {
    $errs = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($p, [ref]$null, [ref]$errs)
    if ($errs) {
        foreach ($e in $errs) { Write-Host "FAIL ${p}: $($e.Message)" -ForegroundColor Red }
        $bad++
        continue
    }

    # ---- 1. case-colliding variable names ----
    # Only ASSIGNED names count. Reading $foo when $Foo was assigned is the same
    # variable and is fine (if ugly); it is two different SPELLINGS being
    # assigned that means the author believed they were separate slots.
    $assigns = $ast.FindAll({
        param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and
                  $n.Left -is [System.Management.Automation.Language.VariableExpressionAst]
    }, $true)

    # A plain @{} is NO GOOD for the inner map: PowerShell hashtable keys are
    # case-insensitive too, so "Ovl" and "ovl" collapse into one entry and the
    # collision this function exists to find becomes invisible. (Caught by
    # self-test -- the checker fell into its own trap.) Use an ordinal
    # Dictionary, which compares keys byte-for-byte.
    $byLower = @{}
    foreach ($a in $assigns) {
        $name = $a.Left.VariablePath.UserPath
        $k = $name.ToLowerInvariant()
        if (-not $byLower.ContainsKey($k)) {
            $byLower[$k] = New-Object 'System.Collections.Generic.Dictionary[string,int]' (
                [System.StringComparer]::Ordinal)
        }
        if (-not $byLower[$k].ContainsKey($name)) {
            $byLower[$k].Add($name, $a.Extent.StartLineNumber)
        }
    }
    foreach ($k in $byLower.Keys) {
        if ($byLower[$k].Count -gt 1) {
            $where = ($byLower[$k].GetEnumerator() | Sort-Object Value |
                      ForEach-Object { "`$$($_.Key) (line $($_.Value))" }) -join " and "
            Write-Host ("FAIL ${p}: $where differ only in case -- PowerShell treats them as ONE " +
                        "variable, so the second assignment overwrites the first") -ForegroundColor Red
            $bad++
        }
    }

    # ---- 2. nested array literal that will flatten ----
    # @(...) parses as an ArrayExpression; a bare a,b list inside it is an
    # ArrayLiteralAst. A nested ArrayExpression as a direct element is the
    # flattening shape unless it is preceded by a unary comma, which the parser
    # records as its own single-element ArrayLiteralAst wrapper.
    $arrays = $ast.FindAll({
        param($n) $n -is [System.Management.Automation.Language.ArrayLiteralAst]
    }, $true)
    foreach ($arr in $arrays) {
        $nested = @($arr.Elements | Where-Object {
            $_ -is [System.Management.Automation.Language.ArrayExpressionAst] -or
            $_ -is [System.Management.Automation.Language.ArrayLiteralAst]
        })
        if ($nested.Count -ge 2) {
            $ln = $arr.Extent.StartLineNumber
            Write-Host ("FAIL ${p}:${ln}: nested array literals inside @( ) FLATTEN -- " +
                        "each element indexes as a scalar. Use @( ,@(a,b) ; ,@(c,d) )") -ForegroundColor Red
            $bad++
        }
    }

    if ($bad -eq 0) { Write-Host "ok   $(Split-Path $p -Leaf)" }
}

if ($bad -gt 0) { Write-Host "`n$bad problem(s)" -ForegroundColor Red; exit 1 }
Write-Host "`nall clean"
