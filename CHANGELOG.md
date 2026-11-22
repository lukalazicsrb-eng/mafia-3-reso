# Changelog

## 0.1.3-dev.1 — reflection capture integrity

- Ispravljena greška u kojoj neuspešno presretanje `CreatePixelShader`
  zaustavlja i `Draw` dijagnostiku, iako je izvršni D3D11 import povezan.
- Brojači uređaja, shader potpisa, render poziva, dispatch i Present potvrđuju
  da smo zaista zakačili Mafia III renderer.
- Do 32 nepoznata shader potpisa beleže se strogo ograničeno, ali **nisu
  automatski potvrđeni SSR** (unapred zadati CRC nije dokaz potpisa).
- Dodat test `reflection_settings_test` za `HOOK_HEALTH` izlaz i F8 toggle.
- Bezbedna nadogradnja sa poznatog prethodnog reflection proxy-a,
  originalni Modern Tweaks i High ostaju sačuvani.
- **Ultra refleksije nisu još popravljene niti su u igri potvrđene.**

## 0.1.2-dev.1

- Prekidač **Reflection Diagnostics** dodat je u postojeći Modern Tweaks F8 → Visuals. On/Off se čuva odvojeno u `MafiaReflections.ini`.
- DLL proverava originalni Modern Tweaks i instrukciju na poznatom Visuals pozivu pre uključivanja nove kontrole. Originalni anti-aliasing ostaje dostupan.

## 0.1.1-dev.1

- D3D11 dijagnostika ugrađena je u `dinput8.dll`. Za ručno postavljanje dovoljan je taj DLL uz sačuvani original Modern Tweaks-a.
- Izostavljen je instalater iz ZIP-a. Paket radi bez posebne konfiguracije. Postojeća opcija `Enabled=0` i dalje može da ugasi dijagnostiku.

## 0.1.0-dev.1

- Nativna DXBC dijagnostika za SSR i cubemap/probe prolaze uz verzijski ograničen DirectInput proxy.
- RenderDoc izvoz resursa i poređenje stvarnih dimenzija između dva snimka.
- Odvojeni D3D11 MSAA render cilj sa proverenim sample countom, depth/stencil i single-sample resolve izlazom.
- Windows x64 CMake i Visual Studio projekat, instalater sa SHA-256 proverom i oporavak originalnog moda.
