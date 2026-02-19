## fdk-aac prebuilt package

- Source: `https://github.com/mstorsjo/fdk-aac`
- Library version: `2.0.3` (from upstream `CMakeLists.txt`)
- Platform: `win-x64`
- Note: only `win-x64` is bundled currently.
- Artifacts:
  - `win-x64/bin/fdk-aac.dll`
  - `win-x64/lib/fdk-aac.lib`
  - headers in `include/`

### Rebuild

Clone and build:

```powershell
git clone --depth 1 https://github.com/mstorsjo/fdk-aac.git C:\temp\fdk-aac

& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" `
  -S C:\temp\fdk-aac `
  -B C:\temp\fdk-aac\build-win-x64 `
  -G "Visual Studio 17 2022" `
  -A x64 `
  -DBUILD_SHARED_LIBS=ON `
  -DBUILD_PROGRAMS=OFF `
  -DFDK_AAC_INSTALL_CMAKE_CONFIG_MODULE=OFF `
  -DFDK_AAC_INSTALL_PKGCONFIG_MODULE=OFF

& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" `
  --build C:\temp\fdk-aac\build-win-x64 `
  --config Release `
  --target fdk-aac
```
