# Kamerabeállítás ONVIF-en (`onvif_encoder.py`)
A kamera kódolóbeállításai és órája ONVIF-en, webes felület nélkül is (csak standard könyvtár). A `KAMERA` egy `rtsp://user:jelszó@host/...` URL,
`user:jelszó@ip`, egy név a `cameras.url`-ből (`<név> <rtsp url>` soronként), vagy csak az IP-cím (vagy DNS-név): ilyenkor a nevet és a jelszót
a `--user`/`--password` adja (nélkülük azonosítás nélkül kérdez; ha a kamera azonosítást kér, ezt kiírja). A `cameras.url` nem kötelező.
A portot magától keresi (80, 8899, 6688); a HS-Camera firmware-ek azonosítás
nélkül válaszolnak (ezt stderr-en jelzi).

## RTSP URL egy új kameránál

A gyártók ritkán adják meg az RTSP URL-t, csak annyit, hogy „kérdezd le ONVIF-en”:

    python3 onvif_encoder.py admin:jelszo@192.168.1.64 url

streamenként (fő- és alstream) kiírja a felbontást, a kódolást, az fps-t és a teljes `rtsp://` URL-t a megadott névvel és jelszóval együtt,
így egyből mehet a `cameras.url`-be. Ha a kamera a gyári címét írja bele (olcsó firmware-eknél előfordul), azt arra a címre cseréli, amin
elértük (stderr-en jelzi). Név/jelszó nélkül (`192.168.1.64 url`) azonosítás nélkül kérdez; ha a kamera azonosítást kér, ezt kiírja.

## Kódoló

- `onvif_encoder.py KAMERA get` – a streamek beállításai (felbontás, fps, bitráta, minőség, GOP, profil).
- `onvif_encoder.py KAMERA options` – mit enged a kamera streamenként.
- `onvif_encoder.py KAMERA set [--token ...] [--width --height] [--fps] [--bitrate] [--quality] [--gop] [--profile]` – csak azt írja, ami eltér
  (az írás újraindítja a kamera kódolóját), így cronból is futtatható, ha a telefonos app visszaállítja a beállításokat. A kamerák többsége
  VBR: a `--bitrate` csak felső korlát (egy 2048-ra állított Imou csendes képnél 240 kbit/s-ot küldött), a képminőséget (nagyításnál a
  kockásodást) a `--quality` adja: Dahua/Imou 1–6 (nagyobb = jobb), a kínai IR kamera 0–100; a megengedett tartományt az `options` mutatja.
  A tényleges bitrátát az élőkép csempéjének állapotsora mutatja („kamera … kbit/s”), így egy-egy beállítás hatása ott lemérhető.

## Óra és időzóna
 A kamerák a nyári időszámítást gyakran rosszul kezelik.

- `onvif_encoder.py KAMERA time` – a kamera módja (NTP/kézi), nyári idő jelzője, időzónája, UTC és helyi ideje, és ezek eltérése ettől a géptől
  másodpercben. Ha a kamera nem ad helyi időt, a zónából és a nyári idő jelzőből számolja („computed from the zone”) – ez nem feltétlenül az,
  amit a kamera a képre ír, azt a képen kell ellenőrizni.
- `onvif_encoder.py KAMERA settime --fixed` – **ajánlott**: kézi mód, ennek a gépnek a mostani eltolása fix zónaként (télen +1, nyáron +2),
  nyári idő jelző ki, és beírja ennek a gépnek a pontos idejét; minden futáskor ír. Naponta cronból, az átállás utáni órában:
  `5 3 * * * cd /home/rtspcam && python3 onvif_encoder.py Utca settime --fixed`. A zónát a kamera saját formájában írja (`GMT+02:00` vagy
  POSIX `CEST-2`); ha a kamera nem ismer zónát (csak helyi időt ad), zóna nélkül a helyi időt küldi.
- `onvif_encoder.py KAMERA settime --tz "CET-1CEST,M3.5.0,M10.5.0/3"` – zóna nyári idő szabályokkal, azoknak a kameráknak, amelyek követik;
  `--dst on|off` a nyári idő jelző. Csak akkor ír, ha valami eltér; kézi módú kameránál a pontos időt is beírja.

Kiírja az előtte/utána állapotot. Ismert esetek: az Imou a nyári idő jelzőt nem engedi kikapcsolni (marad `GMT+01:00` + jelző be); a kínai
mini (HS-Camera firmware, ONVIF a 8899-en) az időt ONVIF-en nem tudja beállítani („SetSystemDateAndTime not implemented”) – ott a telefonos
appban az ország fix eltolás nyári idő nélkül (pl. „Greece” = UTC+2 nyáron, „Hungary” = UTC+1 télen), évente kétszer kézzel kell átállítani.
