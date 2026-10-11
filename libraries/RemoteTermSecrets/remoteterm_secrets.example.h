/*
 * Voorbeeld-secrets voor RemoteTerm receiver.
 *
 * Kopieer dit bestand naar:
 *   <Arduino-map>/libraries/RemoteTermSecrets/remoteterm_secrets.h
 * en vul je eigen gegevens in. Dat bestand staat buiten de sketch en hoort
 * NIET in git (zie .gitignore). Alle versies van de sketch gebruiken het.
 */
#pragma once
#include <stdint.h>

struct WifiProfile {
  const char* ssid;
  const char* password;
};

// Bekende WiFi-netwerken. Het display kiest het sterkste dat in bereik is.
// Profielen waarvan de SSID met "WIFI_" begint worden overgeslagen.
// Let op: de ESP32 werkt alleen op 2,4 GHz (iPhone-hotspot: zet
// "Maximaliseer compatibiliteit" aan).
static WifiProfile wifiProfiles[] = {
  {"WIFI_1_SSID", "WIFI_1_WACHTWOORD"},
  {"WIFI_2_SSID", "WIFI_2_WACHTWOORD"},
  {"WIFI_3_SSID", "WIFI_3_WACHTWOORD"},
  {"WIFI_4_SSID", "WIFI_4_WACHTWOORD"},
  {"WIFI_5_SSID", "WIFI_5_WACHTWOORD"}
};

// Publiek adres van de Apache reverse proxy voor RemoteTerm (HTTPS).
// Thuis werkt dit via NAT-loopback van de router.
static const char* REMOTETERM_HOST = "203.0.113.10";
static const uint16_t REMOTETERM_PORT = 443;

// Gebruiker en wachtwoord uit Apache Basic Auth (htpasswd).
static const char* REMOTETERM_USER = "gebruiker";
static const char* REMOTETERM_PASS = "wachtwoord";

// Het (self-signed) servercertificaat van je Apache, of het CA-certificaat
// dat het ondertekende. Het display accepteert alleen een server die
// hiermee te verifiëren is. Dit is openbare informatie, niet geheim.
// Inhoud van: /etc/ssl/remoteterm/remoteterm.crt
static const char* REMOTETERM_CA_CERT =
"-----BEGIN CERTIFICATE-----\n"
"...plak hier je eigen certificaat, regel voor regel...\n"
"-----END CERTIFICATE-----\n";
