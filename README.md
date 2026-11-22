# Mafia III Definitive Edition — Reflections

Windows x64 razvojni paket za Modern Tweaks 2.5.1. U ZIP-u su `dinput8.dll` i uputstvo za ručno postavljanje. Izvorni kod uključuje odvojeni test pravog MSAA renderovanja, sa multisampled color/depth parom, resolve operacijom i proverom piksela.

| Funkcija | Stanje |
| --- | --- |
| Nativna dijagnostika | Prati odabrane DXBC shadere i beleži tip, format, mipove, dimenzije i sample count vezanih resursa. |
| Ultra Resolution Reflections | Postojeća F8 opcija ostaje dostupna. Uzrok mutnih refleksija i stvarna promena rezolucije zahtevaju A/B snimak. |
| MSAA 2x, 4x i 8x | Implementirani su provera podrške, renderovanje i resolve na izolovanom D3D11 raster cilju. U igri nema uključenog MSAA za vozila. |
| Stabilnost i filtriranje, nove F8 opcije | Potrebna je potvrda SSR i cubemap prolaza iz stvarne igre. |

## Ručno postavljanje

1. Zatvori Mafia III. U folderu pored `mafia3definitiveedition.exe` sačuvaj originalni Modern Tweaks 2.5.1 `dinput8.dll` kao `ModernTweaks_reflections_original.dll`.
2. Kopiraj `dinput8.dll` iz novog ZIP-a u folder igre i pokreni igru preko Steam-a.
3. Za povratak zatvori igru, skloni DLL iz ZIP-a i kopiraj sačuvani original nazad kao `dinput8.dll`.

Ako već imaš sačuvan `ModernTweaks_reflections_original.dll`, zadrži ga. Ako je aktivan stariji FSR proxy, skloni ga u rezervni folder i koristi svoju kopiju originalnog Modern Tweaks DLL-a.

Dijagnostika beleži događaje u `MafiaReflections-loader.log` i `MafiaReflections.log`. Za originalni mod očekuje SHA-256 `529b16daec59e2ce5c735e5c8e7974662b647591da2847c53a16f165a479e900`. Praćenje D3D11 zahteva podržanu verziju EXE-a i dovoljno ranu inicijalizaciju.

## Provera rezolucije refleksija

Za isti parkiran automobil i mokar asfalt u 2560 × 1440 napravi dva D3D11 RenderDoc `.rdc` snimka, jedan sa isključenim, drugi sa uključenim **Ultra Resolution Reflections**. Koristi isti sejv, kameru, vreme, stanje retrovizora i osnovnu grafičku postavku. Ponovno pokretanje i učitavanje između dva snimka omogućava poređenje ponovnih alokacija.

Uz RenderDoc 1.47, `capture/Export-ReflectionCapture.ps1` iz izvornog repozitorijuma pretvara svaki snimak u `.reflection-report.json`. Poređenje zahteva isti DXBC SHA-256 i isti slot u oba snimka. Izdvojene varijante i prolazi koji nedostaju ostaju vidljivi u izveštaju.

```powershell
python capture/compare_reflections.py vanilla.reflection-report.json ultra.reflection-report.json --output Poredjenje.txt
```

## Build

`MafiaIIIReflections.sln` otvara CMake build u Visual Studio 2022, sa Windows SDK-om i MSVC v143. Projekat se izgrađuje iz korena repozitorijuma.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python -B -m unittest discover -s tests -p 'test_*.py'
```

Rezultati su u GitHub Actions. Grafički test renderuje izolovanu scenu izvan igre. FPS, 1% low, vreme GPU kadra, promena scena i kvalitet refleksija na RX 6700 XT još nisu mereni. Dodatni tehnički kontekst je u `docs/reflections.md`.
