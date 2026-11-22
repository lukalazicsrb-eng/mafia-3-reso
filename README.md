# Mafia III Definitive Edition — Reflections

Windows x64 razvojni paket za originalni Modern Tweaks 2.5.1. Paket meri D3D11 prolaze za refleksije u igri. Uključuje i odvojeni test pravog MSAA renderovanja, sa multisampled color/depth parom, resolve operacijom i proverom piksela.

| Funkcija | Stanje |
| --- | --- |
| Nativna dijagnostika | Prati odabrane DXBC shadere i beleži tip, format, mipove, dimenzije i sample count vezanih resursa. |
| Ultra Resolution Reflections | Postojeća F8 opcija ostaje dostupna. Uzrok mutnih refleksija i stvarna promena rezolucije zahtevaju A/B snimak. |
| MSAA 2x, 4x i 8x | Implementirani su provera podrške, renderovanje i resolve na izolovanom D3D11 raster cilju. U igri nema uključenog MSAA za vozila. |
| Stabilnost i filtriranje, nove F8 opcije | Potrebna je potvrda SSR i cubemap prolaza iz stvarne igre. |

## Dijagnostika

1. Raspakuj ZIP u zaseban folder i zatvori Mafia III. Potreban je originalni Modern Tweaks 2.5.1 u direktorijumu sa `mafia3definitiveedition.exe`.
2. Pokreni **Instaliraj dijagnostiku.cmd** i izaberi folder igre. Instalater proverava nastavak postojeće instalacije i SHA-256 originalnog DLL-a, proverava tri fajla paketa i čuva original kao `ModernTweaks_reflections_original.dll`. Ako nađe fajlove FSR beta instalacije, prekida bez izmena.
3. Pokreni igru preko Steam-a. Dijagnostika zapisuje `MafiaReflections-loader.log` i `MafiaReflections.log` u folder igre. Podrazumevano se koristi originalni F8 meni i njegovo podešavanje refleksija.
4. Posle testa zatvori igru i pokreni **Vrati originalni mod.cmd** iz raspakovanog ZIP-a. Skripta proverava instalirane fajlove pre oporavka i čuva zamene i logove.

Dijagnostika može da se ugasi preko `Enabled=0` u `MafiaReflections.ini`. Loader je ograničen na originalni mod sa SHA-256 `529b16daec59e2ce5c735e5c8e7974662b647591da2847c53a16f165a479e900`. Praćenje D3D11 zahteva podržanu verziju EXE-a i dovoljno ranu inicijalizaciju. Prazan izveštaj bez shader događaja ne govori ništa o kvalitetu refleksija.

## Provera rezolucije refleksija

Za isti parkiran automobil i mokar asfalt u 2560 × 1440 napravi dva D3D11 RenderDoc `.rdc` snimka, jedan sa isključenim, drugi sa uključenim **Ultra Resolution Reflections**. Koristi isti sejv, kameru, vreme, stanje retrovizora i osnovnu grafičku postavku. Ponovno pokretanje i učitavanje između dva snimka omogućava poređenje ponovnih alokacija.

Uz RenderDoc 1.47, `Snimanje/Obradi snimak refleksija.cmd` pretvara svaki snimak u `.reflection-report.json`. Poređenje zahteva isti DXBC SHA-256 i isti slot u oba snimka. Izdvojene varijante i prolazi koji nedostaju ostaju vidljivi u izveštaju.

```powershell
python Snimanje/compare_reflections.py vanilla.reflection-report.json ultra.reflection-report.json --output Poredjenje.txt
```

## Build

`Izvorni-kod/MafiaIIIReflections.sln` otvara CMake build u Visual Studio 2022, sa Windows SDK-om i MSVC v143. Projekt može da se izgradi i direktno iz `Izvorni-kod` foldera.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python -B -m unittest discover -s tests -p 'test_*.py'
```

Rezultati za paket su u `PROVERE.txt`. Grafički test renderuje izolovanu scenu izvan igre. FPS, 1% low, vreme GPU kadra, promena scena i kvalitet refleksija na RX 6700 XT još nisu mereni. Dodatni tehnički kontekst je u `Izvorni-kod/docs/reflections.md`.
