# 每次修改代码后运行此脚本，自动同步到 C:\DevApp\android
Set-Location $PSScriptRoot

Write-Host "[1/3] Building Vite..." -ForegroundColor Cyan
npm run build
if ($LASTEXITCODE -ne 0) { Write-Host "Build failed!" -ForegroundColor Red; exit 1 }

Write-Host "[2/3] Capacitor sync..." -ForegroundColor Cyan
npx cap sync android
if ($LASTEXITCODE -ne 0) { Write-Host "Cap sync failed!" -ForegroundColor Red; exit 1 }

Write-Host "[3/3] Copying to C:\DevApp\android..." -ForegroundColor Cyan
$targetMain = "C:\DevApp\android\app\src\main"
Remove-Item "$targetMain\assets" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$targetMain\java" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$targetMain\res" -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item -Recurse "android\app\src\main\assets" "$targetMain\" -Force
Copy-Item -Recurse "android\app\src\main\java"   "$targetMain\" -Force
Copy-Item -Recurse "android\app\src\main\res"    "$targetMain\" -Force
Copy-Item "android\app\src\main\AndroidManifest.xml" "$targetMain\AndroidManifest.xml" -Force
Copy-Item "android\app\build.gradle" "C:\DevApp\android\app\build.gradle" -Force
Copy-Item "android\build.gradle" "C:\DevApp\android\build.gradle" -Force

Write-Host "Done! Now rebuild configuration assistant APK in Android Studio." -ForegroundColor Green
