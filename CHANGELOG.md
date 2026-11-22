# Changelog

## 0.1.1-dev.1

- D3D11 dijagnostika ugrađena je u `dinput8.dll`. Za ručno postavljanje dovoljan je taj DLL uz sačuvani original Modern Tweaks-a.
- Izostavljen je instalater iz ZIP-a. Paket radi bez posebne konfiguracije. Postojeća opcija `Enabled=0` i dalje može da ugasi dijagnostiku.

## 0.1.0-dev.1

- Nativna DXBC dijagnostika za SSR i cubemap/probe prolaze uz verzijski ograničen DirectInput proxy.
- RenderDoc izvoz resursa i poređenje stvarnih dimenzija između dva snimka.
- Odvojeni D3D11 MSAA render cilj sa proverenim sample countom, depth/stencil i single-sample resolve izlazom.
- Windows x64 CMake i Visual Studio projekat, instalater sa SHA-256 proverom i oporavak originalnog moda.
