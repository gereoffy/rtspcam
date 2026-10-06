# rtspcam - mozgásvezérelt RTSP rögzítő service C-ben (ffmpeg/libav nélkül)

## Működése
A H.264 streamből csak a minimálisan szükséges részt dekódolja (az ffmpeg/libavcodec alapján, LGPL): a fejléceket
(időbélyeg, kockatípus I/P, referenciák) és a CABAC entrópia-dekódolással a mozgásvektorokat (MV). Nincs IDCT, predikció,
deblocking, képet nem állít elő, ezért nagyon gyors: az 1,8 GHz-es Celeron 1037U-n 9 kamera (FHD, 1–2 Mbit/s) együtt
egy mag ~29%-át használja, azaz kameránként ~3%-ot. Csak CABAC-os (Main/High profil), progresszív, B-kocka nélküli
streamet elemez; ami nem ilyen, azon nem érzékel mozgást.

A mozgásdetektálás a kiolvasott MV-k alapján, P-kockánként történik:
1. Mozgónak számít egy 16×16-os cella, ha van benne legalább `--mv-min` pixel hosszú vektor (az `--ignore` zónákban levőket
   kihagyja).
2. A szomszédos (átlósan is) mozgó cellákból klasztereket (összefüggő foltokat) képez; a kocka akkor „mozgó”, ha a
   legnagyobb klaszter legalább `--min-cluster` cella (ha `--min-cluster` 0: az összes mozgó terület legalább `--min-blocks`
   blokk), és a kép legfeljebb `--global-limit` része mozog (afölött pl. IR-váltás, kameramozgás: nem számít).
3. Riasztás, ha az utolsó `--window` elemzett kockából legalább `--trigger-frames` mozgó (hiszterézis az egyedi téves
   kockák ellen).

Riasztáskor indul a felvétel, `--pre-roll` másodperccel korábbról (a pufferből, a megelőző kulcskockától), és az utolsó
riasztás után még `--post-roll` másodpercig tart; a fájlt a következő kulcskockánál zárja (legfeljebb `--max-tail`
várakozással), a hosszú eseményeket `--max-segment`-enként új fájlba vágja. Kulcskockán (I) nincs vektor, azt nem elemzi;
`--skip-after-key N` a kulcskocka utáni N kockát sem (blokkos képminőség miatti téves riasztás ellen), `--max-ref-dist`
(alapból 1) pedig a távoli referenciájú kockákat (hierarchikus P-kódolás, pl. Intellio: ott a mozgó tárgy intra blokk,
vektor nélkül).

## Használat
```bash
make -C src
src/rtspcam rtsp://10.1.55.35/primary/h264 -n kapu -o /srv/cameras --mv-min 5 --min-cluster 40 --ignore 0,0,1,0.08
```
Mozgástérkép generálása meglévő felvételekhez (pl. ZoneMinder exportok, régi felvételek): URL helyett MP4/MOV fájl(ok):
```bash
src/rtspcam MINTAK/*.mp4 --config configs/kapu.json          # -> MINTAK/<név>.mvmap, a megadott beállításokkal
```
Nem rögzít, csak a fájl mellé írja a `.mvmap`-et (`--vectors` esetén a `.mvvec`-et is); mindig csak a hiányzót írja meg
(pl. meglévő `.mvmap` mellé csak a `.mvvec`-et), a meglévőt csak `--force`-szal írja felül; a klasszikus (moov) és a
fragmentált (moof) MP4/MOV is megy (`mp4_reader.c`), a térkép ideje a fájl ideje. A `-n` itt elhagyható.

Mozgásmező (`--vectors`, élőben és fájlmódban is): a `.mvmap` mellé `<név>.mvvec` kerül (2. verzió, `RCMVVEC2`),
csomagonként egy rekorddal (ugyanazzal a `t_ms`-sel, mint a térkép). Minden rekordban benne van a kocka mérete is:
`frame_bytes` (ahogy az MP4-ben van) és `vcl_bytes` (amiből a dekóder dolgozik: a képszeletek RBSP-bájtjai, NAL-fejléc,
start code/hosszmező, SPS/PPS/SEI, emulation-prevention bájtok és a záró nullák nélkül).
Utána elemzett kockánként 16×16-os cellánként 1 bájt – a cella **leghosszabb** vektora (küszöb és zóna nélkül,
úgy mérve, mint a detektor): alsó 4 bit a hossza egész pixelre lefelé csonkolva (0–15),
felső 4 bit az iránya 22,5°-onként (0 = jobbra, 4 = le, 8 = balra, 12 = fel; trigonometria nélkül, 4 összehasonlítással).
Így egész `--mv-min M` esetén a `hossz >= M` cellák pontosan a detektor mozgó cellái, a klaszterek (és a riasztás)
újraszámolhatók belőle – ellenőrizve: 675/675 kockán azonos cellák és klaszterméret. Közelítő marad a blokk-mód
(`--min-blocks`: a vektorok területe kellene) és a cellán belül húzódó zónahatár. Kulcskockánál és nem elemzett
kockánál nincs rács. Olvasó: `mvmap.read_vec()`, `mvmap.decode_vec()`. Méret: ~1,5–7 KB/s (kb. a videó 1%-a).

Élőkép (`--live DIR`): a felvevő a `DIR/<név>.sock` Unix socketen kiadja a kamera streamjét (fragmentált MP4, kockánként
egy fragmens, SPS-javítással; üzenetek: `u8 típus` (1 init, 2 kulcskocka, 3 egyéb, 4 státusz: riasztás, rögzítés, kamera
elérhető – 3 bájt, 2 s-enként és változáskor; a kamera kiesése alatt is megy, 0 „elérhető” értékkel), `u32` hossz, adat). Kapcsolódáskor az
init szegmenst és az utolsó kulcskocka óta jött kockákat kapja a néző (azonnal indul), utána élőben; a lassú nézőt eldobja,
a felvételt ez nem zavarja. A `../liveview.py` (csak standard könyvtár) kameránként EGY kapcsolattal olvassa (csak amíg
valaki néz), és osztja szét HTTP-n a böngészőknek; az oldal rácsban mutatja a kamerákat (9 kamerával 3×3), kattintásra egy nagyban.
A böngésző dekódol (MSE), a szerver nem kódol át. Mért késés ~1 s. Az oldal: `/live` = `../liveview.html` (rács)
(a liveview.py mellett kell lennie; minden kéréskor újraolvassa, így módosítás után elég a böngészőt frissíteni).
```bash
src/rtspcam rtsp://... -n kapu -o /srv/cameras --live /run/rtspcam        # kameránként egy processz
python3 liveview.py --live-dir /run/rtspcam --host 0.0.0.0 --port 8781  # http://<gép>:8781/
```

A kapcsolók ugyanazok, mint az `rtspcam.py`-nál (`--config` JSON is megy), kivéve:
- csak TCP (RTP a vezérlő kapcsolaton, interleaved), `--transport` kapcsoló nincs;
- hitelesítés: az URL-ben megadott `rtsp://felhasználó:jelszó@...` alapján Basic vagy Digest (MD5, MD5-sess, qop=auth),
  amelyiket a kamera kéri; a naplóban a jelszó helyén `***` áll;
- `--fix inline` (alapértelmezett) vagy `--fix off`; a `remux` nincs;
- új: `--viewonly`: nem ír fájlt (se `.mp4`, se `.mvmap`/`.mvvec`, könyvtárat sem hoz létre), minden más ugyanúgy megy:
  elemzés, riasztás, a felvétel indítása/leállítása „elvben” (a naplóban `EVENT start/stop`), és a `--live` élőkép a
  piros/narancs kerettel. Olyan kamerákhoz, amelyeknek csak az élőképe kell (pl. forgalmas utca).
- új: `--skip-after-key N` (alapértelmezett 0): a kulcskocka utáni első N képet nem elemzi (16-os jelző, nincs
  `.mvvec`-rács). Alacsony bitrátájú kameráknál (Kinai-Mini) a kódoló a kulcskocka után „átfesti” a képet, és az első
  P-kocka szinte mindig mozgást mutat (mért: 69% vs. 0–4%), ami felvétel közben 2 s-onként meghosszabbította a post-rollt.
- új: `--max-ref-dist N` (alapértelmezett 1): csak azokat a kockákat elemzi, amelyek referenciája legfeljebb N kockával
  korábbi; 0 = mindet (mint az `rtspcam.py`). Lásd lent: „Hierarchikus P-kockák”.
- csak `rtsp://` bemenet (fájl nincs); `--cheap` elfogadott, de nincs hatása (nincs mit kihagyni).
Leállítás: SIGINT/SIGTERM (a nyitott felvételt lezárja). Szakadáskor újracsatlakozik (2, 4, … 60 s várakozás);
akkor is, ha a kapcsolat él, de `--timeout` ideig (10 s) nem jön videó (lefagyott kamera, ami csak RTCP-t küld);
induláskor a megmaradt `.part` fájlokat átnevezi.

Üzemeltetési részletek:
- a fájlnév a felvétel első (pre-roll) képének **faliórás** ideje (a kamera órájának csúszása, időbélyeg-ugrása nem
  tolja el); ha a név foglalt (pl. az őszi óraátállítás ismétlődő órája), `_1`, `_2`… utótagot kap;
- **tele lemez**: felvétel indítása előtt megnézi a szabad helyet (`--min-free MB`, alapból 100; 0 = nincs
  ellenőrzés); ha kevesebb van, vagy a könyvtár/fájl nem hozható létre, vagy írás közben hiba jön, a rögzítő egy
  hibasorral **view-only módba vált** (mint a `--viewonly`: elemzés, riasztás, élőkép megy, de se `.mp4`, se
  `.mvmap`/`.mvvec` nem íródik), és abban is marad: a hely felszabadítása után a service-t újra kell indítani. A
  félbemaradt fájlt azzal zárja le, ami kiment; ha egy teljes fragmens sem került ki, törli;
- **diagnosztika** (egyszer naplózott figyelmeztetések): 30 s-ig nincs kulcskocka (IDR); a stream nem elemezhető
  (B-kocka, CAVLC/Baseline, FMO, interlace, 4:2:2, vagy nincs SPS/PPS se az SDP-ben, se a streamben) – ilyenkor csak
  `--always`-zel van felvétel; 50 egymás utáni kocka nem dekódolható; nincs `sprop-parameter-sets` az SDP-ben (info);
- a hiányos (csomagvesztéses) képet rögzíti, de nem elemzi (a térképben „nincs adat”), hogy a szemét vektor ne
  riasszon.

Mért erőforrás (élő kamera, 60 s egymás mellett, ugyanazokkal a beállításokkal, fejlesztői gépen):
Python 7,6% CPU / 535 MB RSS, C 1,5% CPU / 8,5 MB RSS.

Kipróbált kamerák: Intellio ILD-420E (hierarchikus P), Dahua (2 db, Digest), Hikvision (Digest/Basic), YGTek/kínai
(Basic, ill. hitelesítés nélkül). Szűk sávszélességnél egyes kamerák (Dahua PTZ wifin, YGTek) a valós időnél lassabban
küldenek, a YGTek ilyenkor a nagy IDR-képek tartalmát a kamerában elrontja – ez a ZoneMindernél is így van.

## Fájlok
- `mvparse.c/.h` – mozgásvektorok kiolvasása képdekódolás nélkül (csak CABAC entrópia-dekódolás; az együtthatókat
  kiolvassa és eldobja). Az ffmpeg H.264 dekóderéből portolva → **LGPL-2.1+**. `h264_cabac_tables.c`: az ffmpeg táblái.
- `options.c/.h` – az `rtspcam.py`-jal azonos parancssor és `--config` JSON (a fájl adja az alapértékeket,
  a parancssori kapcsoló felülírja, a `--ignore` hozzáadódik).
- `motion.c/.h` – a `MotionDetector` megfelelője (zónák, klaszterezés, hiszterézis, térkép-szintek).
- `mvmap_writer.c/.h` – `.mvmap` író (formátum: `../mvmap.py`).
- `session.c/.h` – a `run_session()` állapotgépe (pre-roll puffer, post-roll, `--max-segment`, `--max-tail`);
  a videóíró cserélhető (`rc_video_ops`: MP4 író, a tesztben csomagnaplózó).
- `log.c/.h` – naplózás az `rtspcam.py` formátumában.
- `rtsp.c/.h` – RTSP-kliens (OPTIONS, DESCRIBE, SETUP, PLAY, GET_PARAMETER keepalive, TEARDOWN), SDP, sprop-parameter-sets.
- `rtp_h264.c/.h` – RTP → Annex B képkockák (single NAL, STAP-A, FU-A). Képhatár az időbélyeg váltásánál vagy markernél
  (az Intellio kamera nem mindig állítja a marker bitet, a YGTek minden csomagra ráteszi); a kép típusát a start code-ok
  alapján nézi (egyes kínai kamerák a teljes képet start code-okkal egy FU-A-ba teszik); a kép nélküli SPS/PPS/SEI a
  következő képhez kerül; ha egy időbélyeggel több kép jön (lemaradó kamera), a képkezdő szeleteknél szétvágja és +1 tickkel
  szigorúan növekvő időt ad; csomagvesztésnél a kocka „hiányos” jelzést kap. Session végén statisztika a naplóban.
- `md5.c/.h` – MD5 a Digest hitelesítéshez.
- `mp4_writer.c/.h` – fragmentált MP4 (`ftyp`, üres `moov`, GOP-onként `moof`+`mdat`, a végén `mfra`), mint az ffmpeg
  `frag_keyframe+empty_moov+default_base_moof`; `.part`-ként íródik, lezáráskor átnevezi. `h264_sps.c`: az SPS-javítás
  (`max_num_ref_frames` → 4, mint a `h264fix.py`), az avcC-ben és a kulcskockák SPS-ében.
- `mp4_reader.c/.h` – MP4/MOV index-olvasó (első H.264 videósáv; moov táblák és moof/trun fragmentek) a fájlmódhoz.
- `live.c/.h` – élőkép-socket (nem blokkoló, kliensenkénti sor, utolsó GOP a gyors induláshoz); az `mp4_writer.c` élő
  módja (`mp4_live_*`) készíti a kockánkénti fragmenseket.
- `main.c` – főprogram.
- `mvdump.c` – parancssori eszköz: MV-k egy Annex B (`.h264`) fájlból, időméréssel.

## Fordítás és tesztek
```bash
make -C src                                                   # cc + zlib
.venv/bin/python src/test_mvparse.py tune_clips/*.mp4         # MV-k: C vs ffmpeg export_mvs + sebesség
.venv/bin/python src/test_session.py tune_clips/*.mp4         # felvételek + .mvmap: C vs rtspcam.py + sebesség
```
A `test_session.py` az igazi `rtspcam.main()`-t futtatja (csak az MP4 író helyett egy naplózó csonkkal és befagyasztott
órával), a C oldal ugyanazokat a csomagokat kapja; a felvételek listájának, csomagjainak és a `.mvmap` tartalmának egyeznie kell.
Egyetlen engedett eltérés: ha több egyforma méretű „legnagyobb” csoport van, a két implementáció másikat jelölheti meg (ezt
a teszt „tie frames”-ként számolja).

## Hierarchikus P-kockák (a „fésűminta”)
Az Intellio kamerák időbeli rétegezést használnak: minden kockának egy referenciája van, de a távolság 8 kockánként
1, 2, 1, 4, 1, 2, 1, 8. A 4–8 kockás távolságnál a mozgó tárgyat a kódoló intrában kódolja (700–2200 intra MB/kocka a
szokásos 100–300 helyett), így ezekben a kockákban a mozgásnak nincs vektora – ez adta a fésűs grafikont. A parser követi a
referenciákat (POC, sliding window, MMCO 1–6, listaátrendezés; a DPB-állapot képről képre egyezik az ffmpeg
`-debug mmco` kimenetével), és a túl távoli referenciájú kockák szelet-adatát nem is dekódolja (kb. 2,5× gyorsabb).
Ezek a `.mvmap`-ben a 16-os jelzőt kapják (a kulcskockák is: 1 | 16, mert vektoruk nincs); a lejátszó a grafikonról kihagyja őket, a képen az előző elemzett kocka
térképét mutatja. A `--window`/`--trigger-frames` az *elemzett* kockákat számolja (ennél a kameránál kb. a felét).
Megjegyzés: ha a kamera kihagy egy képet, az ffmpeg „nem létező” helyőrző képeket tesz a DPB-be; ezeket itt nem
követjük (a távolságot nem befolyásolják, mert hivatkozni nem lehet rájuk).

Nem támogatott (a kamerák nem használják): B-szelet, CAVLC, interlace/MBAFF, FMO. Az ilyen kockák „nincs adat”-ként
kerülnek a térképbe, a hibás (nem dekódolható) kockák szintén; a következő kockát ez nem befolyásolja.
