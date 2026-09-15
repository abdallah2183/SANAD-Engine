@echo off
chcp 65001 >nul
title NOVAForge Engine - Push to GitHub
echo ====================================================================
echo        🚀 NOVAForge Engine - رفع المحرك على جيت هب (GitHub)
echo ====================================================================
echo.

git status --short

echo.
echo Checking Git Remote Origin...
git remote -v
echo.

set /p REPO_URL="Enter your GitHub Repository URL (or press Enter for https://github.com/abdallah2183/NOVAForge-Engine.git): "
if "%REPO_URL%"=="" (
    set "REPO_URL=https://github.com/abdallah2183/NOVAForge-Engine.git"
)

echo.
echo Setting remote origin to: %REPO_URL%
git remote remove origin >nul 2>&1
git remote add origin %REPO_URL%

echo.
echo Ensuring all files are added and committed...
git add .
git commit -m "feat: Arab Game Engine launch - NOVAForge Engine with logo, web showcase, and community roadmap"

echo.
echo Pushing branch 'main' to GitHub (%REPO_URL%)...
git branch -M main
git push -u origin main

echo.
if %ERRORLEVEL% equ 0 (
    echo ====================================================================
    echo   🎉 تم رفع محرك NOVAForge على جيت هب بنجاح!
    echo ====================================================================
) else (
    echo ====================================================================
    echo   ⚠️ إذا طلب منك جيت هب تسجيل الدخول أو مفتاح الوصول (Personal Access Token):
    echo   1. تأكد من إنشاء المستودع على: https://github.com/new
    echo   2. يمكنك تسجيل الدخول في نافذة المتصفح التي ستظهر لك.
    echo ====================================================================
)
echo.
pause
