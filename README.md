# djdriver – Treiber für den Reloop Digital Jockey 2 Master Edition

Ein quelloffener Treiber für den **Reloop Digital Jockey 2 Master Edition**
(USB-ID `200c:1009`), der auf aktuellen Macs **mit Apple Silicon (M1/M2/M3/…)**
läuft. Reloop liefert seit Jahren keinen passenden Treiber mehr.

> **Status: experimentell, noch nicht an echter Hardware getestet.**
> Der Treiber beruht auf öffentlich dokumentiertem Reverse Engineering des
> Ploytec-USB-Protokolls (siehe [Hintergrund](#hintergrund)). Das Protokoll
> ist für verwandte Geräte belegt, für den DJ2 ME aber noch nicht bestätigt.
> Rückmeldungen mit der Ausgabe von `djdriver probe` sind sehr willkommen.

## Was der Treiber kann

> **Hinweis zum DJ2 ME:** Tests an echter Hardware haben gezeigt, dass der
> Controller abstürzt, sobald sein Audioteil gestartet wird (Streaming-Bit,
> Samplerate-Anfragen, Interface 1). Der Treiber läuft deshalb standardmäßig
> im **Nur-Eingabe-Modus**: Bedienelemente → DJ-Software funktionieren, die
> LEDs bleiben aus. Für die LEDs gibt es `--leds` (kann den Controller zum
> Absturz bringen).

| Funktion | Status |
| --- | --- |
| Tasten, Fader, Drehregler, Jogwheels → DJ-Software (MIDI IN) | ✅ implementiert |
| LEDs ← DJ-Software (MIDI OUT) | ⚠️ standardmäßig aus (`--leds`, experimentell) |
| Automatisches Wiederverbinden beim Ab- und Anstecken (inkl. LED-Zustand) | ✅ |
| Eingebaute Soundkarte (Audio-Ausgang/-Eingang) | ❌ noch nicht |

Der Treiber läuft komplett im **Userspace** (libusb + CoreMIDI).
Du brauchst also **keine Kernel-Extension, musst SIP nicht abschalten** und
brauchst kein Apple-Entwicklerzertifikat. In macOS erscheint ein virtuelles
MIDI-Gerät **„Reloop DJ2 ME“**, das jede DJ-Software (Mixxx, Traktor,
VirtualDJ, Serato, rekordbox, djay …) wie einen normalen MIDI-Controller sieht.

Für den Ton nimmst du bis auf Weiteres den Kopfhörer-/Line-Ausgang des Macs
oder ein anderes Audio-Interface.

## Installation (macOS)

1. [Homebrew](https://brew.sh) installieren, falls noch nicht vorhanden.
2. Terminal öffnen und:

   ```sh
   git clone https://github.com/launischeforelle/djdriver.git
   cd djdriver
   ./macos/install.sh
   ```

Das Skript installiert `libusb`, baut den Treiber, testet ihn, installiert
ihn nach `/usr/local/bin/djdriver` und richtet einen LaunchAgent ein. Der
Treiber startet dann bei jeder Anmeldung automatisch im Hintergrund und
wartet auf den Controller. Log: `~/Library/Logs/djdriver.log`.

Deinstallation: `./macos/uninstall.sh`

**Wichtig:** Falls noch ein alter Reloop-/Ploytec-Treiber installiert ist,
diesen vorher entfernen – sonst kann `djdriver` das Gerät nicht öffnen.

## Erste Schritte / Fehlersuche

```sh
djdriver probe      # Zeigt USB-Deskriptoren, Firmware und Status des Geräts
djdriver monitor    # Treiber starten und jede MIDI-Nachricht anzeigen
djdriver ledtest    # Lässt die LEDs (Noten 0–127, Kanal 1) einzeln aufleuchten
djdriver --help
```

Falls schon der LaunchAgent läuft, vor `monitor`/`ledtest` erst stoppen:

```sh
launchctl bootout gui/$(id -u)/com.github.djdriver
# ... testen ...
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.github.djdriver.plist
```

Wenn sich nichts tut, bitte die Ausgabe von `djdriver probe` und
`djdriver -v monitor` (während du ein paar Tasten drückst) in einem Issue
posten. Die Endpunkte lassen sich bei Bedarf mit `--ep-out`, `--ep-midi` und
`--ep-pcm` überschreiben.

## Nutzung mit Mixxx

[Mixxx](https://mixxx.org) ist kostenlos und bringt ein Mapping für die
Digital-Jockey-2-Familie mit:

1. `djdriver` laufen lassen, Controller anschließen.
2. Mixxx → Einstellungen → Controller → **„Reloop DJ2 ME“** aktivieren.
3. Als Mapping **„Reloop Digital Jockey 2 Controller“** wählen.

Sollte das Mapping nicht 1:1 passen, kann man in Mixxx (und jeder anderen
DJ-Software) per *MIDI Learn* selbst zuweisen; `djdriver monitor` zeigt,
welche Nachricht jedes Bedienelement sendet.

## Bauen von Hand

```sh
brew install libusb pkg-config   # macOS
make && make test
./djdriver monitor
```

Unter Linux lässt sich das Programm ebenfalls bauen (`libusb-1.0-0-dev`), dort
gibt es aber noch kein virtuelles MIDI-Gerät – die Nachrichten werden nur
ausgegeben. Das ist zum Testen des Protokolls gedacht.

## Hintergrund

Der DJ2 ME ist **nicht USB-class-compliant**. Er benutzt einen USB-Chip mit
Firmware der Firma **Ploytec GmbH**, so wie auch der Reloop Jockey 3 und die
Allen & Heath Xone:DB4/DB2/4D. Das Protokoll in Kürze:

* **Steuerung (EP0):** Herstelleranfragen zum Lesen der Firmware (`0x56`),
  Lesen/Schreiben eines Statusregisters (`0x49`, Bit 5 = Streaming an) und
  zum Setzen der Samplerate (`SET_CUR` an die Endpunkte `0x86` und `0x05`).
* **EP 0x05 OUT:** Audio-Ausgabestrom. MIDI zum Gerät (LEDs) steckt als
  einzelnes Byte an festen Stellen im Audiostrom; freie Stellen enthalten
  `0xFD`. Der Strom muss deshalb ständig laufen, auch wenn nur Stille
  gesendet wird.
* **EP 0x83 IN:** MIDI vom Gerät als roher Bytestrom, aufgefüllt mit
  Füllbytes `0xF0`–`0xFF`.
* **EP 0x86 IN:** Audio-Eingabestrom (wird derzeit nur geleert).

Die Firmware versteht kein *Running Status*, deshalb schickt der Treiber
jede Nachricht vollständig. MIDI zum Gerät ist auf ca. 1100 Byte/s
begrenzt – genug für ~360 LED-Änderungen pro Sekunde.

Das Protokollwissen stammt aus diesen Projekten – vielen Dank:

* [alsa-jockey3](https://github.com/antoon-derijcke/alsa-jockey3) (Frank van de Pol) –
  Linux-Treiber für den Reloop Jockey 3, mit USB-Mitschnitten der Original-Treiber
* [Ozzy](https://github.com/mischa85/Ozzy) (Marcel Bierling) –
  Treiber für Ploytec-basierte Allen-&-Heath-Mixer

Der Code in diesem Repository ist eine eigenständige Implementierung.

### Ausblick: Audio

Die eingebaute Soundkarte nutzbar zu machen, ist möglich, aber deutlich
aufwendiger: Dafür braucht es ein CoreAudio-Plug-in (AudioServerPlugIn), das
mit diesem Prozess Audiodaten austauscht, plus den Ploytec-Audio-Codec
(24-Bit-Samples werden bitweise über 24 Bytes verteilt). Das Ozzy-Projekt
zeigt, wie das geht. Sobald MIDI an echter Hardware bestätigt ist, wäre das
der nächste Schritt.

## Lizenz

MIT, siehe [LICENSE](LICENSE).
