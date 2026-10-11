# RemoteTerm receiver – wijzigingslog

## 1.4.11 – 2026-10-09
- **Bug opgelost: geen wissel naar een ander WiFi-profiel buiten bereik van
  profiel 1** (gemerkt op locatie, 2026-10-07).
  Oorzaak: `setAutoReconnect(true)` bleef het onbereikbare profiel 1 proberen;
  zolang de ESP32 "aan het verbinden" is weigert hij een nieuwe configuratie,
  dus `WiFi.begin()` voor profiel 2/3 mislukte stil (elke 10 s opnieuw).
- WiFi nu via `WiFiMulti`: scan van netwerken in bereik, verbinden met het
  sterkste bekende profiel (`WIFI_`-placeholders worden overgeslagen). Bij
  verlies van de verbinding wordt opnieuw gescand (elke 10 s, connect-timeout
  8 s). Auto-reconnect staat uit.
- Bij een (nieuwe) WiFi-verbinding wordt de oude HTTPS-verbinding gesloten.
- Display toont nu ook de SSID naast het IP-adres; seriële log meldt SSID,
  IP en signaalsterkte.
- Versienummer naar 1.4.11.
- Getest 2026-10-09: display verbindt en ontvangt via iPhone-hotspot.

## 1.4.10 – 2026-10-07
- **Bug opgelost: `EmptyInput` bij elke berichten-poll** (1.4.9; vermoedelijk
  ook de echte oorzaak in 1.4.8 — het vrije geheugen bleek ruim 150 KB).
  Oorzaak in ESP32-core 3.3: `NetworkClientSecure::read()` geeft -1 als er nog
  geen data is; `NetworkClient::readBytes()` (gebruikt door ArduinoJson) ziet
  dat als fout en stopt meteen. Kanalen (1,5 KB) kwamen mee met de headers en
  werkten; berichten (~19 KB) kwamen iets later en faalden na 2 ms.
  Gevonden met Core Debug Level "Debug".
- Nieuwe klasse `TimedBodyStream`: leest via `Stream::timedRead()` (wacht tot
  5 s op data), met 512-byte buffer, en nooit meer dan `Content-Length` bytes
  (veilig voor keep-alive).
- Op de PC gesimuleerd met vertraagde TLS-data: oud → EmptyInput na 0 ms,
  nieuw → 100 berichten OK.
- Versienummer naar 1.4.10.
- Getest 2026-10-07 (thuis): berichten in #pi4fld komen weer binnen via HTTPS.
- Let op: bij compileren board "ESP32S3 Dev Module" kiezen (stond per ongeluk
  op ESP32C3 → fouten 'File'/'SD_MMC' not declared).

## 1.4.9 – 2026-10-07
- **Bug opgelost (geïntroduceerd in 1.4.8): nieuwe berichten bleven weg** in
  drukke kanalen (bijv. #pi4fld). Oorzaak: RemoteTerm negeert `after_id`
  (en `before_id`/`since_id`) en stuurt altijd de laatste `limit` berichten
  (~48 KB bij 100). 1.4.8 las dat eerst volledig in een String; samen met TLS
  liep het geheugen vol en mislukte elke poll, waardoor `lastMessageId` bleef
  hangen (Apache-log: steeds `after_id=1415`).
- `httpsGet()` vervangen door `httpsGetJson()`: parse direct uit de stream
  met `DeserializationOption::Filter` (alleen id, text, sender_name,
  sender_key, channel_name, sender_timestamp). Bij een chunked antwoord valt
  hij terug op een buffer. Bij een JSON-fout wordt de verbinding gesloten en
  het vrije geheugen gelogd.
- Gewone poll vraagt `limit=20` (`POLL_LIMIT`); na start of kanaalwissel en
  bij terugbladeren `limit=100` (`FULL_LIMIT`, vlag `fullFetchPending`).
- Kanalen ook via filter (key, name); `DynamicJsonDocument` hier vervangen
  door `JsonDocument` (v7).
- Filter op de PC getest met RemoteTerm-achtig antwoord (100 berichten).
- Versienummer naar 1.4.9.

## 1.4.8 – 2026-10-07
- **Beveiliging: HTTPS met wachtwoord.** RemoteTerm wordt nu bereikt via
  Apache op de RPI4 (poort 443 → proxy naar 127.0.0.1:8000) met Basic Auth.
- `WiFiClientSecure` met vastgepind self-signed certificaat
  (`REMOTETERM_CA_CERT` in de secrets): het display vertrouwt alleen de RPI4.
- Nieuwe functie `httpsGet()`: één globale verbinding die tussen polls open
  blijft (keep-alive); bij een verbindingsfout één keer opnieuw met een verse
  verbinding. Fouten (HTTP-code, TLS-fout) gaan naar de seriële monitor.
- Antwoord wordt via `getString()` gelezen (ook veilig bij chunked transfer).
- NTP-tijdsynchronisatie (`configTzTime`, Europe/Amsterdam) voor de
  certificaatcontrole.
- Secrets: `REMOTETERM_PORT` 443, nieuw `REMOTETERM_USER`, `REMOTETERM_PASS`,
  `REMOTETERM_CA_CERT`.
- Serverkant (Raspberry Pi met RemoteTerm): certificaat in `/etc/ssl/remoteterm/`
  (geldig tot 2036-10-04), vhost `remoteterm-ssl.conf`, htpasswd in
  `/etc/apache2/remoteterm.htpasswd`; router: TCP 443 → de Pi.
- Versienummer naar 1.4.8.

## 1.4.7 – 2026-10-07
- **Beveiliging: wachtwoorden uit de broncode.** `wifiProfiles`,
  `REMOTETERM_HOST` en `REMOTETERM_PORT` staan nu in
  `Arduino/libraries/RemoteTermSecrets/remoteterm_secrets.h`. De sketch doet
  `#include <remoteterm_secrets.h>`. Eén bestand voor alle versies; de sketch
  bevat geen wachtwoorden meer en kan veilig gedeeld worden.
- `remoteterm_secrets.example.h` als voorbeeld met nep-gegevens.
- Versienummer naar 1.4.7.

## 1.4.6 – 2026-10-07
- **Bug 4 opgelost: kanaalwissel vergeleken op index.** `fetchChannels()`
  vergelijkt nu de key van het gekozen kanaal vóór en na het verversen. Een
  andere volgorde van de kanaallijst wist de history niet meer.
- Ongebruikte variabele `oldSelectedChannel` verwijderd.
- Versienummer naar 1.4.6.
- Test uitgesteld (lastig te testen; vereist andere kanaalvolgorde in RemoteTerm).

## 1.4.5 – 2026-10-07
- **Bug 3 opgelost: niet-Latijnse tekens werden dubbel `??`.** `displayText()`
  verwerkt nu elk 2-byte UTF-8-teken (lead `0xC0`–`0xDF` + vervolgbyte) als
  één teken. Cyrillisch/Grieks wordt één `?` per letter.
- Nieuw: `latinFallback()` met tabel `LATIN_EXT_A` (U+0100–U+017F) zet
  Poolse, Tsjechische, Hongaarse e.d. letters om naar ASCII (ł→l, ś→s, č→c,
  ő→o …). Ook `°`→`o`, harde spatie→spatie, `µ`→`u`.
- Op de PC getest met voorbeeldzinnen vóór het flashen.
- Getest op het display 2026-10-07: werkt.
- Versienummer naar 1.4.5.

## 1.4.4 – 2026-10-07
- **Bug 2 opgelost: status bleef hangen op "RemoteTerm niet bereikbaar".**
  Nieuwe vlag `remoteTermFailed` onthoudt of de vorige poll mislukte. Bij
  herstel wordt het scherm opnieuw getekend met "Verbonden".
- Bij een storing wordt het scherm nu één keer getekend in plaats van elke 5 s
  (minder flikkering). Overgangen worden ook in de seriële monitor gemeld.
- Versienummer naar 1.4.4.
- Getest 2026-10-07: RemoteTerm-services gestopt en gestart; melding verschijnt
  en herstelt naar "Verbonden".

## 1.4.3 – 2026-10-07
- **Bug 1 opgelost: volle history-buffer.** Bij 120 berichten werden nieuwe
  berichten weggegooid (en niet naar SD geschreven). `insertHistory()` laat nu
  het oudste bericht vallen bij een nieuw bericht, en het nieuwste bij
  terugbladeren. Na herstart worden de 120 nieuwste berichten van SD geladen.
- Versienummer naar 1.4.3.
- Getest 2026-10-07: berichten na de eerste 120 worden weer getoond.

## 1.4.2
- Uitgangsversie (vóór review).

## Openstaand (uit review)
- Optioneel: Cyrillisch omzetten naar Latijnse letters (Привет → Privet).
- Test 1.4.6 (kanaalwissel op key).
- Test 1.4.11 thuis, op een ander netwerk en wissel tussen profielen.
- Mini-CA voor het servercertificaat (Safari-waarschuwing; Apple max. 825 dagen).
- Terugbladeren voorbij de 100 nieuwste berichten werkt niet: RemoteTerm
  negeert `before_id`. Juiste parameter uitzoeken (bijv. `offset`).
- Naam staat dubbel bij eigen berichten (`text` begint al met afzender).
- Robuustheid: blokkerende HTTP(S), overbodige SPI.begin, groeiend SD-bestand, ArduinoJson v6/v7, `| 0` bij int64-id's, sender_timestamp.
- UI: kanaalmenu max 6, emoji-hoogte, flikkering, statusmelding "Kanalen geladen", spring naar nieuwste bij terugbladeren, kanaalnamen afkappen op bytes.
