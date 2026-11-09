param(
    [string]$Port = "COM69",
    # 并行编译任务数。默认 2，不要用 ninja 的"按核心数自动"。
    # 本机 7.7 GB 物理内存 / 8 逻辑核心，引入 LVGL 后工程有 ~1900 个编译目标，
    # 其中 nvs_flash/mbedtls 的 C++ 单元单个就要 400~700 MB，
    # -j8 时提交内存（物理+页面文件）会被打穿，报 "cc1plus.exe: out of memory"。
    # 内存宽裕时可 -Jobs 4 加速。
    [int]$Jobs = 2,
    [switch]$Flash,
    [switch]$Monitor
)

$ErrorActionPreference = "Stop"

# 本机 PowerShell 执行策略是 Restricted，dot-source IDF 激活脚本会被拒。
# 这里对当前进程放开，不修改系统策略。
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force -ErrorAction SilentlyContinue

# The machine has 3 IDF installs (v5.5.5 / v6.0.2 / v6.1). Only this one has a
# matching python venv + riscv32-esp-elf toolchain that the board demos were built with.
$eimProfile = "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
if (-not (Test-Path $eimProfile)) {
    throw "ESP-IDF v6.1 activation script not found: $eimProfile"
}

Write-Host "[env] activating ESP-IDF v6.1 (IDF_PATH=D:\esp\.espressif\v6.1\esp-idf)" -ForegroundColor Cyan
. $eimProfile

Set-Location -Path $PSScriptRoot

# set-target wipes sdkconfig and build/, so only run it when the target is wrong.
$target = $null
if (Test-Path "sdkconfig") {
    $hit = Select-String -Path "sdkconfig" -Pattern '^CONFIG_IDF_TARGET="(.+)"' | Select-Object -First 1
    if ($hit) { $target = $hit.Matches[0].Groups[1].Value }
}

if ($target -ne "esp32p4") {
    Write-Host "[target] sdkconfig target='$target' -> switching to esp32p4 (regenerates sdkconfig + build)" -ForegroundColor Yellow
    idf.py set-target esp32p4
    if ($LASTEXITCODE -ne 0) { throw "idf.py set-target esp32p4 failed (exit $LASTEXITCODE)" }
} else {
    Write-Host "[target] esp32p4 already configured" -ForegroundColor Green
}

Write-Host "[build] idf.py build (并行 $Jobs 个编译任务)" -ForegroundColor Cyan

# 编译前先看一眼提交内存余量：低于 1.5 GB 时 -j2 也可能失败，
# 与其等 20 分钟后在某个随机文件上 OOM，不如现在提示。
$os = Get-CimInstance Win32_OperatingSystem
$freeCommitMB = [math]::Round($os.FreeVirtualMemory / 1KB, 0)
if ($freeCommitMB -lt 1536) {
    Write-Host ("[warn] 提交内存只剩 $freeCommitMB MB（物理内存 7.7 GB 已用满，页面文件也接近上限）。") -ForegroundColor Yellow
    Write-Host "       建议先关掉几个大户再编译：多余的 VS Code 窗口、Edge 标签页、微信、Acrobat。" -ForegroundColor Yellow
    Write-Host "       若仍然 OOM，用 .\build.bat -Jobs 1 串行编译（慢，但一定过得去）。" -ForegroundColor Yellow
} else {
    Write-Host "[mem] 提交内存余量 $freeCommitMB MB" -ForegroundColor Green
}

# IDF 通过这个环境变量给 ninja 传 -j，不设就是 CPU 核心数（这里必然 OOM）
$env:IDF_PY_BUILD_JOBS = $Jobs

idf.py build
if ($LASTEXITCODE -ne 0) { throw "idf.py build failed (exit $LASTEXITCODE)" }

if ($Flash) {
    Write-Host "[flash] idf.py -p $Port flash monitor" -ForegroundColor Cyan
    idf.py -p $Port flash monitor
} elseif ($Monitor) {
    Write-Host "[monitor] idf.py -p $Port monitor" -ForegroundColor Cyan
    idf.py -p $Port monitor
} else {
    Write-Host "[done] build ok. Use -Flash to burn, -Monitor to open serial." -ForegroundColor Green
}
