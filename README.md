# rtspcam – mozgásvezérelt RTSP rögzítő és lejátszó

A **rögzítő** a `src/` könyvtárban van (C, ffmpeg nélkül; működés, kapcsolók és fájlformátumok: [`src/README.md`](src/README.md)). Mozgásra
felvételt ír kameránként (`<kamera>/<dátum>/<kamera>_<óraperc>.mp4`), mellé a `.mvmap` (riasztási térkép) és a `--vectors` kapcsolóval a
`.mvvec` (cellánként a leghosszabb vektor) fájlt. A **lejátszó** (`player.py`) ezeket mutatja a böngészőben, és kiszolgálja az élőképet is.

## Lejátszó (`player.py`)
```bash
python3 player.py /srv/cameras                           # a rögzítő -o könyvtára
python3 player.py /srv/cameras --live-dir /run/rtspcam   # + élőkép
```

| Cím | Mit ad |
|---|---|
| `/` | `index.html`, a menü/tájékoztató oldal |
| `/favicon.ico` | a `favicon.ico` a szkript mellől |
| `/player` | a lejátszó, az összes kamerával (alapból az utolsó 1 óra) |
| `/player/<kamera>` vagy `/player#<kamera>` | a lejátszó úgy nyílik, hogy a kamera-szűrő az adott kamerán áll (alapból az utolsó 24 óra); a szűrőben más kamera vagy „Mind" is választható |
| `/player#<felvétel-azonosító>` | közvetlen link egy felvételre (`<kamera>/<dátum>/<név>`) |
| `/live` | az élőkép rácsa (csak `--live-dir`-rel; nélküle „Hiba: nem fut a rögzítő") |
| `/live/<kamera>`, `/status` | az élőkép nyers fMP4 streamje, illetve a kamerák riasztás/rögzítés állapota (JSON) |
| `/video?id=…`, `/api/list`, `/api/map`, `/api/vec`, `/api/vecinfo`, `/api/tuneconfig` | a lejátszó adatai (a lista: `?since=<epoch>`, `?camera=` szűréssel) |

Kapcsolók:

| Kapcsoló | Jelentés |
|---|---|
| `root` | a felvételek gyökere (a rögzítő `-o` könyvtára) |
| `--port` | alapból 8780 |
| `--host` | alapból `127.0.0.1`; `0.0.0.0` csak megbízható hálón |
| `--config-dir` | a hangoló `<kamera>.json` konfigjainak mappája (alapból `configs/`) |
| `--live-dir` | a rögzítők `--live` könyvtára: az élőkép (`/live`, `/status`) is a lejátszóból szolgálódik ki |
| `--cameras a,b,c` | `--live-dir`-rel: a kamerák sorrendje az élőkép rácsában (alapból a socketek ábécésorrendje) |
| `--rtspcam` | a rögzítő binárisa (alapból `src/rtspcam`): a hangolás ezzel generálja a hiányzó `.mvvec`-et |

A `player.py` mellett kell lennie a `player.html`, `index.html`, `liveview.html`, `liveview.py`, `mvmap.py` és `favicon.ico` fájloknak. Az oldalakat
minden kérésnél újraolvassa (módosítás után nem kell újraindítani), és `no-cache` fejlécet kapnak. A `.mvvec`-et a lejátszó csak akkor kéri, ha vektor-overlayt vagy hangolást használsz. Ha a felvételhez nincs, a **szerver** az első kérésnél legenerálja: lefuttatja a `src/rtspcam --vectors FILE.mp4`-et (csak a hiányzó `.mvvec`-et írja, a `.mvmap`-et nem bántja; kb. 0,3 s egy 20 s-os 2K-s felvételre), a kamera `configs/<kamera>.json` fájljával (a `max_ref_dist` miatt). A felvételek könyvtárának írhatónak kell lennie. Egy fájlra egyszerre egy, összesen legfeljebb két generálás fut. A `.mvmap` nélkül a felvétel overlay nélkül is lejátszható.

Hitelesítés nincs: alapból csak `127.0.0.1`-en figyel; `--host 0.0.0.0` csak megbízható hálón. Javasolt VPN vagy reverse proxy (nginx stb) használata.
A `/player/<kamera>` csak a listát szűri, nem korlátozza a hozzáférést.

Az összes kamera felvételeit listázza (szűrés: kamera, dátum-tartomány, napszak, „csak térképpel"; a lista 30 mp-enként magától frissül,
kézzel a „↻ Frissítés" gombbal), a teljes felbontású H.264 videót streameli a böngészőbe, és rárajzolja a `.mvmap` mozgástérképét
(sárga/narancs/piros blokkok; „zaj elrejtése" kapcsolóval a sárgák eltüntethetők). Alatta idővonal a mérőszámmal és a riasztással
(kattintással/húzással lehet tekerni). Lejátszás/szünet (szóköz), sebesség (0,5–8×), „autoplay": a felvétel végén ugyanannak a kamerának a
következő felvétele indul. A kép és az idővonal elfér az ablak magasságában. **Hangolás mód** (a vezérlősor „Rögzítő (.mvmap) | ⚙ Hangolás (.mvvec)" kapcsolója, csak `.mvvec`-es felvételnél): a videó alatt megjelenik a hangoló panel (csúszkák a konfigfájlban és a parancssorban használt nevekkel: `mv_min`, `min_cluster`, `global_limit`, `window`, `trigger_frames`, `pre_roll`, `post_roll`, a magyar magyarázat a tooltipben; zónarajzolás a képen húzással). A panel közvetlenül az idővonal és a lejátszásvezérlők alatt van, a többi beállítás (mozgástérkép, zaj elrejtése, overlay-mód, autoplay) alatta. Ilyenkor a **felvétel `.mvmap`-ját egyáltalán nem használja**: az idővonal, a kockák (piros = a küszöböt elérő összefüggő csoportok, narancs = a legnagyobb a küszöb alatt, sárga = a többi) és a riasztás is csak a `.mvvec`-ből és a csúszkákból számolódik, a rögzítő állapotgépével azonos logikával (cellánként a leghosszabb vektor ≥ `mv_min`, 8-szomszédos csoportok, ablak + trigger, pre/post-roll). A `global_limit` hatása látszik: az idővonalon a limitet elérő kockák oszlopa **lila** (kis jelmagyarázattal), a kijelző mutatja, hogy a kép hány %-a mozog a limithez képest („mozog 48% / limit 30% · global_limit: nem számít"), az összegzés pedig kiírja, hány kockát utasított el a limit. A skála felső határa 500 (a `min_cluster` csúszka 400-ig megy), az ennél nagyobb értékű oszlop teteje fehér sapkát kap. A képen egy jelző („HANGOLÁS · .mvvec + csúszkák" / „rögzítő · .mvmap") és az idővonal sárga kerete mutatja, melyik adat látszik. Három beállításkészleten ellenőrizve a C-s rögzítő `.mvmap`-jával kockánként azonos (klaszter, mozgásos kocka, riasztás; zónával és globális limittel is). Különbségek: csak klaszter módban számol (nincs `min_blocks`), nincs kulcskocka-igazítás (a felvétel hossza egy GOP-nyit eltérhet), a zónát cellaközéppel teszteli. A Mentés a `configs/<kamera>.json`-t írja (a nem ismert kulcsokat, pl. `max_ref_dist`, megtartja; mappa: `--config-dir`), a Betöltés beolvassa; ugyanezt olvassák a rögzítők a `--config` kapcsolóval: a fájl erősebb a parancssornál, és a futó rögzítő percenként ránéz, így a mentett értékek legfeljebb egy percen belül, újraindítás nélkül élesben is érvényesek (a következő képkockától; lásd `src/README.md`).

**Overlay-mód** (a „zaj elrejtése" mellett): *kockák* (a sárga/narancs/piros blokkok), *vektorok* vagy *kockák + vektorok*. A vektorokat a C-s rögzítő `--vectors` kapcsolóval írt `.mvvec` fájlja adja (cellánként 1 bájt: a cella **leghosszabb vektora**, alsó 4 bit a hossz pixelben lefelé kerekítve, felső 4 bit az irány 22,5°-onként; így a küszöbös döntés és a klaszterek a `.mvvec`-ből pontosan visszaszámolhatók); ha a felvételhez nincs `.mvvec`, a mód le van tiltva, és a kockák látszanak. Egy vektor egy cella közepén kezdődő, ponttal jelölt vonal: a **hossza** a vektor hossza (képpontonként 2 képpontnyi vonal), a **színe** az irány (piros = jobbra, 90° = le, 180° = balra, 270° = fel). A fájl kicsomagolva nagy (kb. 3 MB / 10 mp, a videónál is több), ezért a szerver az eredeti zlib-folyamot küldi `Content-Encoding: deflate` fejléccel, amit a böngésző magától kibont (a hálózaton a videó ~1–2%-a). Csak akkor tölti be, ha vektor módot választasz. **Léptetés:** a „◀ kocka / kocka ▶" gombokkal vagy a ←/→ billentyűkkel kockánként (Shift+nyíl: 10 kocka; lejátszás közben megállítja a videót; a kockaidőket a `.mvmap` adja, térkép nélkül 25 fps-sel számol). **Nagyítás:** görgővel a kép fölött (az egérkurzor alatti pont helyben marad, max. 12×), húzással mozgatható, dupla kattintásra visszaáll; a mozgástérkép együtt nagyítódik a képpel.

**Kockaméret-grafikon** (`.mvvec` 2-es formátum, az idővonal alatt): kockánként a `frame_bytes` vagy a `vcl_bytes`, a medián (alapból az egész klipé, az I-kockák/kiugrók — > 10× medián — nélkül; az „ablak” mezőbe írt N-nel az előző N kocka mozgómediánja), skála 3×/5×/10× medián. A **méretarány-küszöb** csúszka (0 = ki) a hangolásban VAGY-kapcsolatban második mozgásjelzés: az a P-kocka is „mozgásos”, amelyik a medián küszöbszorosánál nagyobb (a `global_limit` vétó erre is él); az idővonalon kék, a méretgrafikonon piros. Ez még csak a hangolóban létezik és **nem mentődik a konfigba** (a C rögzítő ismeretlen konfigkulcsra hibával áll le), a rögzítő nem ismeri.

**Autozoom** (alul, az autoplay előtt, alapból ki): lejátszás közben magától belenagyít a riasztó (piros) képrészletbe és követi. Kockánként a piros cellák befoglaló téglalapjából (rögzítő módban a `.mvmap`-ből, hangolásnál a `.mvvec`-ből és a csúszkákból) a kb. kétszeres méretű képkivágat lesz (legalább a kép szélességének 25%-a), a kivágatok sorozata – a mozgás előtt és után a teljes képre kitávolítással, két esemény között viszont nem távolít ki, hanem lassan halad a következő felé – 40 ms-os mintákra bontva Gauss-szűrővel (σ = 0,5 s) simítva, idő szerint játssza vissza (a teljes felvétel ismert, ezért „előre néz”). Kézi nagyítás vagy húzás kikapcsolja. A hangoló a `FOLLOW` konstansokkal (`margin`, `minFrac`, `sigma`) a `player.html`-ben állítható.

**Nagy könyvtár:** a felvétel-index csak memóriában él (indulásnál a háttérben épül fel, lemezre nem ír). A lista az „Időszak” szűrő szerint (a `/player` alapból az utolsó 1 óra, a `/player/<kamera>` 24 óra; a kamera- és az időszak-szűrő nem öröklődik az előző látogatásból, a többi szűrő igen) csak a szükséges részt kéri le (`/api/list?since=<epoch>`, gzip-pel), az automatikus frissítés csak az utolsó óra utáni részt kérdezi újra (minden 20.-nál és kézi frissítésnél a teljes betöltött tartományt), a napfejlécre kattintva a nap összecsukható (hosszú időszaknál a legújabb nap kivételével alapból összecsukva).

**Függőségek:** csak a Python standard könyvtára (a lejátszó és az élőkép is). Az `av` (PyAV) opcionális: csak akkor kell, ha egy felvételhez nincs `.mvmap`
(a hosszát ilyenkor a konténerből olvassa). A `numpy` a lejátszóhoz nem kell.

## Élőkép (`/live`)
A lejátszó fejlécében az `● Élő` gomb ide visz (mindig az összes kamerához). A rács a kamerákat egymás mellett mutatja (9 kameránál 3×3), a böngésző maga
dekódolja a H.264-et (Media Source Extensions), a szerver nem kódol át. A kamerák **piros kerete** riasztást, a **narancs** a riasztás utáni (post-roll)
rögzítést jelzi; a **szürkített kép** „kamera nem elérhető” felirattal azt, hogy a rögzítő nem éri el a kamerát, vagy a kamera nem küld képet (az utolsó képek
maradnak a csempén), „nem fut a rögzítő” felirattal pedig azt, hogy annak a kamerának a rögzítője nem fut (a socketje nem érhető el). A csempe fölé víve megjelenik egy sáv (a képen alapból nincs felirat): balra a kamera neve, mellette az állapot (felbontás, fps, késés,
újracsatlakozások), jobbra a **▶ Felvételek** gomb, ami a kamera lejátszó-oldalára (`/player/<kamera>`) visz. A képre kattintva egy kamera nagyban látszik
(újabb kattintás: vissza a rácsba); ilyenkor **görgővel** az egérkurzornál nagyítható (max. 12×), húzással mozgatható, nagyítva egy kattintás 1×-re állít vissza.
Ha nem fut a rögzítő (nincs socket, vagy nem jelentkezik), az oldal „Hiba: nem fut a rögzítő" üzenetet ír.

A rögzítőt `--live DIR`-rel kell indítani (kameránként egy `DIR/<kamera>.sock`). Ugyanez önálló szerverként is megy a lejátszó nélkül:
`python3 liveview.py --live-dir DIR [--host 0.0.0.0] [--port 8781] [--cameras a,b,c]` (közös rész: `liveview.Live`).
