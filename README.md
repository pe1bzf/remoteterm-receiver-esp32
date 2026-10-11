# RemoteTerm receiver (ESP32-S3)

*English summary: a receive-only ESP32-S3 touch display that shows MeshCore
channel messages from a [Remote Terminal for MeshCore](https://github.com/jkingsman/Remote-Terminal-for-MeshCore)
server over HTTPS. Documentation below is in Dutch.*

Een losstaand 2,8" touchdisplay op basis van een ESP32-S3 dat de berichten
van MeshCore-kanalen toont. De berichten komen van een
[Remote Terminal for MeshCore](https://github.com/jkingsman/Remote-Terminal-for-MeshCore)
("RemoteTerm") server, bijvoorbeeld op een Raspberry Pi. Het display is
**alleen-ontvangen**: het verstuurt niets en draait geen webserver.

Gemaakt door PE1BZF.

## Functies

- Kanaalkeuze via het touchscreen, bladeren door berichten met `<` en `>`
- Berichtgeschiedenis per kanaal op de SD-kaart (blijft bewaard na herstart)
- Woordomloop met UTF-8; veel Europese letters (é, ł, č, ő, ...) en `°` worden
  netjes weergegeven, emoji via Noto Emoji (LGFX_Fonts)
- Meerdere WiFi-profielen: kiest automatisch het sterkste bekende netwerk in
  bereik en wisselt als de verbinding wegvalt (`WiFiMulti`)
- HTTPS met vastgepind certificaat en Basic Auth: het display praat alleen met
  jouw server
- Wachtwoorden en adressen staan in een apart secrets-bestand, buiten de sketch

## Hardware

- ES3C28P / ES3N28P (LCDWiki) 2,8" ESP32-S3 board
  - ILI9341 240×320 via SPI
  - Capacitieve touch (I²C, adres 0x38)
  - MicroSD via SDMMC (1-bit)

Andere ESP32-S3 boards werken ook, mits je de pinnen bovenin de sketch aanpast.

## Benodigdheden

- Arduino IDE met **esp32 by Espressif 3.3.x** (getest met 3.3.12)
- Libraries:
  - [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
  - LGFX_Fonts (Noto Emoji voor LovyanGFX)
  - [ArduinoJson](https://arduinojson.org/) 7.x
- Een RemoteTerm-server met daarvoor een Apache reverse proxy (zie onder)

### Bordinstellingen (Arduino IDE)

| Instelling | Waarde |
|---|---|
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | naar behoefte (voor de seriële monitor) |
| Partition Scheme | een schema met een grote app-partitie (de emoji-fonts zijn groot) |
| Core Debug Level | None (Debug alleen bij foutzoeken) |

## Installatie

1. Kopieer de map `remoteterm_receiver/` naar je Arduino-sketchmap.
2. Kopieer `libraries/RemoteTermSecrets/` naar `<Arduino-map>/libraries/`.
3. Maak in die map een kopie van `remoteterm_secrets.example.h` met de naam
   `remoteterm_secrets.h` en vul in:
   - je WiFi-netwerken (alleen 2,4 GHz)
   - het publieke adres van je Apache-proxy
   - gebruiker en wachtwoord (Basic Auth)
   - het servercertificaat (PEM)
4. Herstart de Arduino IDE (nieuwe library-mappen worden pas dan gevonden),
   compileer en flash.

`remoteterm_secrets.h` staat in `.gitignore`: commit dat bestand nooit.

## Server: Apache als HTTPS-proxy voor RemoteTerm

RemoteTerm zelf heeft geen echte toegangscontrole en hoort niet direct op
internet. Zet er daarom Apache voor met HTTPS:

- **Display**: Basic Auth (herkend aan de User-Agent `ESP32HTTPClient`)
- **Browsers**: een inlogformulier met een versleutelde sessie-cookie van
  90 dagen (`mod_auth_form` + `mod_session_crypto`)
- RemoteTerm luistert alleen nog via Apache; poort 8000 niet openzetten in de
  router, alleen 443

Voorbeeldbestanden staan in [`server/apache/`](server/apache/).

### Kort stappenplan (Raspberry Pi OS / Debian)

```bash
# Certificaat (vervang de IP-adressen door die van jou)
sudo mkdir -p /etc/ssl/remoteterm
sudo openssl req -x509 -newkey rsa:2048 -sha256 -days 825 -nodes \
  -keyout /etc/ssl/remoteterm/remoteterm.key \
  -out    /etc/ssl/remoteterm/remoteterm.crt \
  -subj "/CN=203.0.113.10" \
  -addext "subjectAltName=IP:203.0.113.10,IP:192.168.1.10" \
  -addext "extendedKeyUsage=serverAuth"
sudo chmod 600 /etc/ssl/remoteterm/remoteterm.key

# Modules, wachtwoord en sessiesleutel
sudo a2enmod ssl proxy proxy_http proxy_wstunnel auth_form request session session_cookie session_crypto
sudo htpasswd -c /etc/apache2/remoteterm.htpasswd gebruiker
openssl rand -base64 32 | sudo tee /etc/apache2/remoteterm-session.key > /dev/null
sudo chmod 600 /etc/apache2/remoteterm-session.key

# Inlogpagina en site
sudo mkdir -p /var/www/remoteterm-login
sudo cp server/apache/login/index.html /var/www/remoteterm-login/
sudo cp server/apache/remoteterm-ssl.conf.example /etc/apache2/sites-available/remoteterm-ssl.conf
sudo nano /etc/apache2/sites-available/remoteterm-ssl.conf   # ServerName aanpassen
sudo a2ensite remoteterm-ssl
sudo apachectl configtest && sudo systemctl reload apache2
```

Gebruik `-days 825` of korter: Safari en iOS weigeren servercertificaten die
langer geldig zijn, ook als je ze vertrouwt.

## Bekende eigenaardigheden

- **RemoteTerm negeert `after_id`/`before_id`** op `/api/messages` en stuurt
  altijd de nieuwste `limit` berichten. De sketch vraagt daarom normaal
  `limit=20` en filtert zelf op id. Terugbladeren voorbij de 100 nieuwste
  berichten werkt (nog) niet.
- **ESP32-core 3.3 + HTTPS**: `NetworkClientSecure::read()` geeft -1 zolang er
  nog geen data is, en `NetworkClient::readBytes()` (gebruikt door
  ArduinoJson) ziet dat als fout. De sketch gebruikt daarom een eigen
  `TimedBodyStream` die wel wacht.
- **WiFi-wissel**: `setAutoReconnect(true)` blijft een onbereikbaar netwerk
  proberen en blokkeert zo een wissel; de sketch gebruikt `WiFiMulti`.

Zie [CHANGELOG.md](CHANGELOG.md) voor de volledige geschiedenis.

## Licentie

MIT — zie [LICENSE](LICENSE).
