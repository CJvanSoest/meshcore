# LinkedIn post (NL)

---

Afgelopen tijd heb ik een **MeshCore-client gebouwd voor de Tanmatsu badge** — een open-source embedded device met een eigen LoRa-radio voor mesh-communicatie.

Het eindresultaat: node discovery, groepschat en versleutelde directe berichten met bezorgbevestiging, direct op de badge. Maar wat ik er vooral van meeneem is wat ik onderweg heb geleerd.

**Wat ik heb geleerd:**

📖 **Lees eerst, code daarna.** Mijn eerste les was vertragen. Veel bugs uit de eerste week waren dingen die al gedocumenteerd waren — op een plek die ik nog niet had gelezen.

🔩 **Hardware-abstractie verbergt echte complexiteit.** De badge gebruikt twee chips die samenwerken. De grens daartussen heeft regels die niet zichtbaar zijn in de API. Dat merkte ik pas toen de applicatie processor crashte omdat ik een verkeerde volgorde van afsluiten aanhield. Drie regels fix, maar je moet wel begrijpen *waarom*.

🔐 **Cryptografie tolereert geen aannames.** De encryptie-implementatie had een subtiele fout in de wiskundige berekeningen — geen crash, geen foutmelding, gewoon een verkeerde uitkomst. Het enige wat hielp was een wiskundige zelfcheck inbouwen: bereken hetzelfde getal op twee manieren en vergelijk. Dat is hoe ik het vond.

🔎 **Reverse engineering is een waardevolle vaardigheid.** Sommige protocol-details stonden nergens gedocumenteerd. Ik heb ze achterhaald door te kijken hoe de ontvanger (de MeshCore iPhone-app) reageerde op pakketten die ik stuurde. Toen de app voor het eerst "delivered" toonde was dat een mooi moment.

⏱️ **Tijd is een schaars goed op embedded systemen.** Zonder batterij-klok is de tijd na herstart altijd nul. Voor een messaging-app breekt dat de berichtsvolgorde. Een constraint die in web- of mobiele ontwikkeling simpelweg niet bestaat.

---

De broncode staat op GitHub: github.com/CJvanSoest/meshcore

#EmbeddedSystems #ESP32 #LoRa #MeshCore #Leren #Cryptography #Tanmatsu

---

## Update — week na launch (mei 2026)

Een paar extra lessen na een week verder bouwen:

🔢 **Namen liegen, registers niet.** Een chip-constante heette `BANDWIDTH_62`, dus dacht ik 62 kHz. Bleek 62.5 kHz te zijn — gewoon afgerond in de naam. Ik dacht een mismatch te zien met een ander apparaat in mijn netwerk. Eén blik in de register-tabel van de driver: er was geen mismatch. Les: ga bij vermeende afwijkingen altijd terug naar het laagste documentatie-niveau, niet naar de symbool-naam erbovenop.

🪞 **Wat een library NIET doet is net zo belangrijk als wat-ie wel doet.** De MeshCore-library is een protocol-codec. Geen deduplicatie. Pakketten die via meerdere routes binnenkwamen verschenen daardoor 2x of 3x in de chat. Fix was triviaal (ringbuffer van 32 hashes), maar je moet eerst beseffen dat het je verantwoordelijkheid is. Bij het kiezen van een library — bouw een mentaal model van wat-ie wel én niet voor je doet.

🔐 **Vingerafdruk op het stabiele deel.** Voor die dedup: niet de hele packet hashen — bij elke retransmit zit er een andere route in. Wél de versleutelde payload (deterministisch). De les generaliseert: voor elke cache of dedup geldt dat je sleutel afgeleid moet zijn van het deel dat invariant is onder de variatie die je wil negeren.

⏰ **Zet je timezone vóór ALLES dat tijd leest.** Eén `setenv("TZ", ...)` voorkomt een hele klasse aan bugs — mits hij op ELK pad uitgevoerd wordt voordat `localtime_r` ergens wordt aangeroepen. Niet alleen op het happy path met WiFi, ook op de offline NVS-restore branch. Klassieke "happy path getest, edge path stuk" bug.

🏷️ **Cosmetisch is OK, maar wees eerlijk.** Een instelling "Rol: Repeater" toegevoegd die alleen de advertentie verandert — het apparaat wordt geen echte repeater. Tooltip toegevoegd: "Affects advertised role only, does not enable repeater behavior." Vijf seconden schrijven voorkomt uren verwarde gebruikers. Waar een naam meer belooft dan een instelling levert, benoem dat gat expliciet in de UI.

---

## Update — UI restyle-week (mei 2026)

Een paar dagen besteed aan het visueel gelijktrekken van deze app met een ander project op een andere badge (WHY2025). Lessen die je niet uit codestijl-guides haalt:

🎨 **Een palet is een API tussen apps.** De vier tabs van de meshcore-app delen nu hetzelfde Tokyo Night kleurenpalet als mijn LoRa-info app op de andere badge. Hex-waarden één-op-één overgenomen, bestaande `COL_*` namen behouden, alleen de waarden vervangen. 99 call-sites bleven onaangetast. Les: als je twee apps consistent wilt laten voelen, behandel het kleurenpalet als een gedeelde definitie — niet als toevallige hex-waarden verspreid over de code.

🔢 **Een font kies je voor je meest getoonde glyph, niet voor je liefste woord.** Switch gemaakt van een monospace naar een proportional font (Saira). Mooi voor lopende tekst — maar de "1"-glyph is dun en op kleine sizes lastig te onderscheiden van een "l". In een app vol RSSI-waarden, SNR-getallen en pagina-tellertjes ("1-8/11") is dat een echte leesbaarheidsbug. Oplossing: getallen-velden één type-step omhoog. Les: als je veel cijfers toont, test je font op cijfers, niet op proza.

📥 **Persistente UI vraagt persistente data.** De DM-tab toonde na een herstart geen lijst van eerdere chats meer. Mijn eerste reflex: "ik moet de view fixen." Maar de view was correct — de data ontbrak. Contacts werden alleen opgeslagen als de gebruiker ze expliciet als favoriet markeerde, niet automatisch bij DM-verkeer. Fix: één idempotente helper (`contact_ensure`) die wordt aangeroepen vanuit elk pad dat een DM-relatie creëert (ontvangen, versturen, openen vanuit nodelijst). Les: als je UI iets blijvend wil tonen, los het op in de storage-laag, niet in de render-laag.

♻️ **Hergebruik vóór verbouwen.** Voor de DM-inbox stonden er twee opties op tafel: óf alle bestaande infrastructuur uitbreiden (per-contact buffers, per-contact files op SD) óf alleen een dunne UI-laag bovenop de bestaande globale buffer. De tweede haalt 80% van de UX-winst met 10% van het werk en blokkeert de eerste niet. Gekozen voor de dunne variant; volledige per-contact storage staat op de roadmap voor na deze release. Les: incrementele verbeteringen mogen — als ze de grote stap niet in de weg zitten.
