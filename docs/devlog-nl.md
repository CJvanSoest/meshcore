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

---

## Update — tekstinvoer + protocol-archeologie (mei 2026)

📝 **Eén tekstinvoer-helper, twee velden.** De Owner-naam en de nieuwe LoRa-advert-naam wilden allebei bewerkbaar zijn. In plaats van twee parallelle bewerkmodi te bouwen heb ik één state (`field_editing_text`) en één edit-buffer, met twee thin helpers `settings_begin_text_edit(field)` en `settings_commit_text_edit(field)` die kiezen welk veld geladen/opgeslagen wordt. De toetsenbord-handler kent het concept "veld" niet — alleen "er wordt tekst getypt, schrijf naar de buffer". Resultaat: een derde bewerkbaar veld toevoegen is nu één case-statement, geen nieuwe input-modus. Les: shared edit-state met per-veld load/commit is een goedkoper patroon dan per-veld-modus, en blijft schoon als het aantal velden groeit.

🔗 **Eén naam-bron, drie zichtbare plekken.** Na het toevoegen van een aparte advert-naam liet de QR-code nog steeds de oude `owner_name` ("CJ") zien — terwijl scanners de advert-naam ("NL-MET-TANMATSU") verwachten. Dezelfde fallback-logica (`advert_name[0] ? advert_name : owner_name`) zat alleen in `send_advert()` ingebakken. De fix was drie call-sites synchroniseren: send_advert, QR-overlay (URL `name=` + label), én channel-chat-prefix. Les: zodra een afgeleide waarde door meer dan één pad bepaald wordt, is dat een symptoom — extraheer de keuze één keer en gebruik 'm overal. Anders krijg je gegarandeerd één plek die later vergeten wordt.

📤 **Reverse-paden moeten je peers ook leren.** Bij elke binnenkomende DM stuurde Tanmatsu een PATH_RETURN met `path_length=0` — ook als het inkomende pakket via 2 of 3 repeaters was gekomen. De remote (T-Beam) kon daardoor geen reverse-pad leren en bleef bij élk bericht opnieuw flooden. Fix: het inkomende `mc_msg.path[]` in omgekeerde volgorde meesturen in de encrypted inner-data van PATH_RETURN (gepad naar 16/32-byte AES-blok, HMAC erover). Les: in een mesh-netwerk is "ack sturen" niet hetzelfde als "de andere kant de route teruggeven". Beide kanten van de verbinding moeten een werkend pad opbouwen, anders blijven adverts en flood-traffic je airtime opvreten.

🕵️ **Hex-dump is sneller dan documentatie.** T-Beam-naar-Tanmatsu DMs faalden zodra ik de T-Beam op "2-byte path hash" zette. Onze parser zag `path_length = 0x40 = 64` — invalid, drop. Door de RX-log tijdelijk uit te breiden naar volledige hex-dump zag ik dat de **rest** van de packet identiek was aan een werkende 1-byte DM (zelfde dest, src, MAC, ciphertext-positie). Conclusie zonder de protocol-specs te raadplegen: `0x40` is een flag-bit in het path_length byte, geen letterlijke hop-count. Mask werd `& 0x3F` (lower 6 bits = lengte, hoogste 2 bits genegeerd). Les: als je vermoedt dat een veld misgelezen wordt, dump het ruwe wireformaat en vergelijk met een werkende variant — dat is sneller en zekerder dan upstream-source archaeology.

⚠️ **Een fix die "lijkt te kloppen" is niet hetzelfde als gefixt.** De mask-fix hierboven was elegant, geverifieerd in de hex-dump en gebouwd zonder build-errors. In de praktijk blijven 2-byte DMs nog steeds falen — er zit blijkbaar méér in dat protocol-verschil dan alleen byte 1. Item op de backlog gezet ipv het te verbergen achter een halve oplossing. Les: als de end-to-end test niet werkt, je fix is niet af. Beter een expliciete openstaande issue dan een silently-broken pad.

---

## Update — refactor, zelf-herstellende historie en documentatie (mei 2026)

Twee weken na de v2-release: een grote opruimactie, één hardnekkige bug die alleen na een NVS-erase tevoorschijn kwam, en een docs-sprint om alle veranderingen vast te leggen.

🧹 **Een file die niet meer reviewbaar is, is een ontwerp-fout.** `main.c` was naar ~3000 regels gegroeid: protocol-parsing, NVS-helpers, rendering, input-handling, SD-historie, identity-management, alles in één file. Opgesplitst in negen modules met een duidelijk thema per file (`history`, `contacts`, `identity`, `settings_nvs`, `nodes`, `chat`, `radio`, `input`, `render`) plus een `ui_state.h` voor gedeelde `extern`-declaraties. Zero gedragswijziging — pure code-organisatie. Resultaat: `main.c` is 320 regels die alleen nog `app_main()` doet (boot, init, event loop), de rest is per module navigeerbaar. Les: als je file zo groot is dat je niet meer weet waar iets staat zonder grep, is dat het signaal voor refactoring. Wacht niet op een functionele aanleiding.

🔑 **Een sleutel die je niet meer hebt blokkeert je voor altijd.** Na een NVS-erase (per ongeluk veroorzaakt door een partition-table flash) startte de app met een nieuwe Ed25519 identity. De chat-historie op SD was versleuteld met een AES-key afgeleid van de oude private key. Symptoom: nieuwe DMs kwamen binnen, LED knipperde, maar de tekst verscheen niet in de chat-view en bleef na een restart ook weg. Diagnose via seriële log: `load` faalde bij record 0 ("bad magic") en stopte de loop — dus ook nieuwe records werden nooit geladen, want het oude vuil bleef in het bestand staan. Fix: een `fatal`-vlag in de load-loop; als de file *vanaf record 0* onleesbaar is, verwijder hem. Volgende append begint dan een fresh log onder de huidige key. Het behoudt readable prefixes (corrupte staart laat ik staan) — alleen volledig-onleesbare files worden gewist. Les: persistente data versleuteld met een key die op een ander persistentie-vlak ligt, heeft een herstelpad nodig wanneer die key verdwijnt — anders staat oud vuil voor altijd in de weg.

🔌 **Een launcher-instelling die je verwacht aan te staan, staat soms uit.** Tussen de v2-tests door werd de launcher geüpdatet en kreeg de Tanmatsu een nieuw WiFi-symptoom: na elke reboot moest ik handmatig opnieuw verbinden. Oorzaak in de upstream launcher: `wifi_connect_try_all()` werd alleen aangeroepen als NTP enabled was. Dat is voor velen acceptabel (geen NTP = geen WiFi-noodzaak), maar voor mij was het stilletjes de default-waarde gewijzigd door een launcher-upgrade. Lokale patch in onze launcher-checkout: WiFi altijd proberen, NTP-pad onafhankelijk. Niet upstream gepushed (te lokale aanname); gedocumenteerd in een memory-file zodat ik bij elke launcher-update kan checken of de patch nog nodig is. Les: bij upstream-dependencies die je gedrag bepalen, hou bij welke patches lokaal zijn en waarom — anders verbaas je jezelf maandenlang met "gisteren werkte het nog".

📚 **Docs zijn een momentopname, niet een aflevering.** Na de refactor + self-heal-fix waren de bestaande SVG-screenshots en README niet meer correct. Nieuwe ronde: 6 SVGs (boot, settings, nodes, dm, channel, qr, radio-bootloader) in het huidige Tokyo Night palette, README bijgewerkt met de module-tabel + alle nieuwe Settings-velden + Makefile-quickstart, en een `docs/wiki/` opgezet met 8 markdown-pagina's per onderwerp (architectuur, protocol, UI, NVS, SD, C6-radio, build). Markdown-pagina's in plaats van een echte `.wiki.git` — ze renderen direct op Gitea/GitHub en zijn met `cp` te promoveren als ik later wel een aparte wiki-repo wil. Les: documentatie groeit niet vanzelf mee — plan na een grote refactor een vaste doc-sprint in voordat je verder bouwt, anders staat de wiki maanden later op een UI die niet meer bestaat.


---

## Update — Radio v3.0.0 + handle-based LoRa API + Gitea-forks (mei 2026)

Nicolai bracht `tanmatsu-radio v3.0.0` uit met een breaking change: de SDIO-commands tussen P4 en C6 zijn gewijzigd, en het bijbehorende `tanmatsu-lora` component is overgegaan op een **handle-based API** (`lora_init_remote(&handle, ...)`, alle calls krijgen `lora_handle_t*` als eerste argument). Nieuwe upstream API ondersteunt ook **meerdere LoRa-radio's tegelijk** — futureproofing waar we nu nog geen gebruik van maken.

🔄 **Een breaking API is een uitnodiging om je integratiepunten te tellen.** Migratie raakte 17 call-sites verspreid over `main.c`, `radio.c` en `settings_nvs.c` — exact de plekken waar de oude lora-globals werden geraakt. Het verbouwen tot één `lora_handle_t lora_handle = {0};` global plus extern in `radio.h` was triviaal; ze allemaal vinden via grep was het echte werk. Les: een refactor die "naar handle-based" gaat is een goeie aanleiding om te zien hoe diep een library je codebase in zit. Hoe meer handle-args je moet doorgeven, hoe meer onzichtbare koppeling er was.

⚠️ **Build vanuit de verkeerde working copy zonder het te merken.** Eerste poging gebouwd vanuit `/Volumes/Projects/Tanmatsu/meshcore-settings` — een pré-refactor copy waar `main.c` nog 3000 regels was. Geuploadt naar de badge, UI toonde de oude monolithische look. Vijf minuten verbazing voordat ik realiseerde dat de juiste source op `~/stack/Projects/Tanmatsu/meshcore-settings` stond met de modulaire 320-regel `main.c` + `chat.c`/`render.c`/etc. Les: meerdere checkouts van dezelfde repo is een footgun; check `git status` + bestandsdatum op de file die je net hebt gewijzigd, niet alleen `pwd`.

🧪 **Local fork bewaren tijdens een upstream-bump.** Onze fork van `tanmatsu-lora` (PACKET_RX_V2 + GET_RSSI_INST patches voor RSSI/SNR + noise floor) is **niet** mee-geüpgrade door de upstream-bump — die API was nog niet handle-based. Compromis: tijdelijk uit `components/` gehaald en de upstream HEAD gebruikt, met stubs in de app zodat het bouwde maar zonder RSSI/SNR. Later op de dag de patches opnieuw aangebracht op de nieuwe handle-based base — als feature-branch op een Gitea-fork (`CJ/tanmatsu-radio` + `CJ/tanmatsu-lora`). App's `idf_component.yml` wijst nu naar `http://192.168.2.25:3000/CJ/tanmatsu-lora.git` (Gitea url). Les: bij upstream-API-breaks is "tijdelijk uitschakelen + later opnieuw aanbrengen" sneller dan "blokkeren tot de patches gemigreerd zijn". De stubs vingen één compile-cyclus op, daarna kon ik de patches opnieuw schrijven met een rustig hoofd.

🪪 **De protocol-versie die je leest is niet altijd wat je dacht.** Voor de "Radio firmware" weergave in Settings: de huidige `lora_get_status` retourneert een `version_string` van 16 bytes. Aangenomen dat dit de `tanmatsu-radio` app-versie zou zijn (v3.0.0 etc) — maar het bleek de **silicon chip ID** te zijn (`sx1261 V20 2002`). De C6-app-versie wordt vandaag de dag niet via dit protocol blootgesteld. Compromis: één read-only "Radio chip" rij voor de silicon-versie, en een tweede "Radio firmware" rij met een hand-onderhouden `#define TANMATSU_RADIO_FW_LABEL` in `app_config.h`. Bumpen bij elke re-flash. Eerlijke labels > misleidende auto-detect. Les: voor je een geadverteerd veld in je UI toont, lees de bron — niet de naam.

📚 **Gitea als fork-host is goedkoper dan GitHub voor work-in-progress.** Voor onze patches op `tanmatsu-radio` (C6 firmware) en `tanmatsu-lora` (P4 component) twee Gitea-repos aangemaakt via de Gitea-API (token uit Infisical). App's `idf_component.yml` wijst nu naar `git: http://192.168.2.25:3000/CJ/...` — de IDF component manager kan rechtstreeks zo'n git-URL hanteren. Voordeel: feature-branch live houden zonder een GitHub-PR-cycle nodig te hebben. De upstream PR naar Nicolai-Electronics komt later. Les: een privé fork-mirror op je eigen Gitea is een ontbrekend stuk gereedschap tussen "lokaal werken" en "publiek PR'en" — geen pressure on PR cadence, wél versiecontrole.


## Update — Upstream PRs voor RSSI/SNR (mei 2026)

Onze fork-patches naar Nicolai-Electronics doorgezet als twee gekoppelde PRs:
- `tanmatsu-radio` (C6 firmware): PACKET_RX_V2 + GET_RSSI_INST handlers
- `esp32-component-tanmatsu-lora` (P4 component): stats fields + lora_get_rssi_inst()

Beide PRs gepaird in description; bevatten screenshots van werkende RSSI/SNR/noise-floor in Settings-footer en Nodes-tab. KRITIEK in de PR-tekst: noise-floor poll interval moet 60s blijven (5s breekt SX1262 preamble→header transitie — diagnose 2026-05-23).

🪞 **Fork-mirror als WIP-host, GitHub als PR-host.** Onze ontwikkel-cyclus liep via Gitea (snel, privé, geen review-druk). Voor de PRs naar Nicolai zijn de patches gespiegeld naar GitHub-forks via een tweede `github` remote. Op die manier krijgen we beide werelden: rustig itereren op Gitea, publiek PR'en vanaf GitHub. Les: een privé fork-mirror tussen "lokaal" en "publiek PR" voorkomt premature feedback-rondes en houdt je hoofdrepo schoon.

🔒 **GitHub email-privacy blokkeert pushes met je echte email.** Eerste push faalde met "push declined due to email privacy restrictions" omdat ons commit-author de privé-email had. Fix: zowel author als committer email zetten op `<username>@users.noreply.github.com`. Voor alle TOEKOMSTIGE Gitea→GitHub mirrors: meteen `git config user.email "...@users.noreply.github.com"` zetten in elke checkout van een GitHub-fork. Les: GitHub's privacy-policy is een onzichtbare push-gate; check je committer-email voordat je een mirror opzet.

📸 **Screenshots in PR-description zijn geen luxe.** GitHub PR-description accepteert drag-drop van afbeeldingen (auto-upload naar githubusercontent CDN). Twee foto's geplaatst: één van Settings-footer met `RX:-31 SNR:+12 N:-101`, één van Nodes-tab met RSSI/SNR-kolommen gevuld. Voor een reviewer (Renze) bewijs: "dit werkt op echte hardware, niet alleen theoretisch in mijn code". Les: een feature-PR zonder beeld is een belofte; mét beeld is het een bewijsstuk.

---

## Update — multi-channel feature data-laag (mei 2026)

Eerste iteratie van #channels: data-storage + RX brute-force + TX active-channel. Nog geen UI om channels te beheren, maar de pipeline werkt eind-tot-eind.

🪪 **Channel-key uit Discord-conversatie als spec.** Renze bevestigde in DM: voor non-Public kanalen is de 16-byte AES-key gewoon de eerste 16 bytes van `SHA256(channel_name_including_'#')`. Dat klinkt te simpel om waar te zijn — maar end-to-end test bevestigde het meteen. Een T-Beam stuurde `#test` chat, onze `channels_add_by_name("#test")` afgeleid via dezelfde formule, en de brute-force HMAC matchte. Les: voor undocumenteerde protocol-details, vraag de auteur — de echte spec is vaak één zin lang, niet drie pagina's reverse engineering.

🔄 **Brute-force MAC-verify > hash-shortcut alleen.** MeshCore's channel_hash byte in de payload is *advisory* — meerdere kanalen kunnen op één hash-byte botsen, en sommige nodes (geobserveerd: oude T-Beam configuraties) zenden zelfs `chash=0x00` ongeacht hun werkelijke key. Onze decode probeert eerst de hash-gematchte channel als optimisatie, en valt bij MAC-mismatch terug op alle andere channels. Les: in een protocol waarbij meerdere keys hetzelfde formaat delen, is "MAC verifieert" de enige bron van waarheid; hash is een hint, geen filter.

🧪 **Debug-bootstrap is een legitieme test-stap.** In `channels_init` voegen we bij eerste boot automatisch een `#test` channel toe — niet als productie-feature, maar zodat ik kon valideren dat de brute-force loop werkt zonder eerst een UI te bouwen. Eén regel code, idempotent (skip als al in NVS), en weg te halen zodra er een echte Add-channel UI is. Les: hardcoded testdata in init-paden is OK als het idempotent + verwijderbaar is — soms is dat het kortste pad tussen "code geschreven" en "feature bewezen werkend".

🏷️ **Visuele disambiguatie zonder data-schema-change.** Met één gedeelde `ch_msgs` ring zou je niet kunnen zien of een bericht uit `Public` of `#test` kwam. Snelste fix: prepend `[name]` aan de tekst voordat-ie de ring in gaat. Vijf minuten code, geen struct-changes, geen render-changes. Volledige per-channel ring + cursor + filter komt later — als deze inline prefix te druk wordt. Les: een struct uitbreiden is werk; een tekstprefix toevoegen is een commit. Kies welke pijn je vandaag wil dragen.

---

## Update — Schaal-factor-archeologie + NVS-migratie valkuil (mei 2026)

Een afstand-kolom toegevoegd voor de Nodes-tab, want we hebben nu eigen GPS-coords + per-node positie uit adverts. Eerste test: T-Beam op zelfde locatie als Tanmatsu → 5211 km. Wat ging er mis?

📏 **Comments liegen, broncode niet.** Mijn `nodes.h` had `// degrees × 1e7` als comment naast de int32 lat/lon field. Daar bouwde ik de hele input/output-pipeline op. Eén `grep "/1000000" upstream/` later: MeshCore upstream gebruikt `× 1e6` (`AdvertDataHelpers.h::getLat() = _lat / 1000000.0`). De comment was ooit fout gezet en daarna door iedereen — inclusief mijzelf — vertrouwd. Les: bij protocol-velden NIET op een comment in je eigen header bouwen — verifieer in upstream src of in een sniffer-dump. Een fout aangenomen schaal blijft niet bij decimal-formatting; het cascadert door alle calculaties.

🔧 **Hernoem-by-sed is goedkoop, scale-mismatches duur.** Toen ik de fix doorvoerde, ging het in 6 plekken om: text-edit parse + display, settings-row display, advert TX, en de Nodes-Dist haversine. Sed-rename `gps_lat_e7` → `gps_lat_e6` was triviaal; de échte mentale belasting was elke literal `1e7` / `1e-7` opzoeken. Les: als je een schaal hernoemt, hernoem dan ook de constanten meteen, niet alleen het symbool. Anders blijft de fout in numerieke vorm staan, onzichtbaar voor compiler.

💀 **Half-gemigreerde NVS is erger dan fout-bevroren NVS.** Mijn eerste migratiecode deed: "als lat > 90M, deel lat door 10". Maar de gebruiker zat op (51.87°N, 5.29°E). In ×1e7 was lat = 518718190 (triggert!) maar lon = 52919140 (< 180M lon-grens, triggert NIET). Resultaat: lat correct gemigreerd naar 51.87°, lon bleef op 52.92° = ergens in Centraal-Azië → 3269 km. Tweede poging "als lat > 90M, deel BEIDE door 10" loste *nieuwe* halve-migraties op, maar de NVS was al gecorrumpeerd in een visueel valide staat (beide < 180M). Geen heuristiek kon dat nog detecteren. Eindoplossing: sentinel NVS-key `lora.gps.sv` (u8 versie). Bij boot zonder of < huidige versie → wipe en force re-entry. save_gps_coords schrijft altijd huidige versie. Les: schaal-migraties die per-veld trigger-condities hebben kunnen halve-migraties veroorzaken; gebruik een sentinel-versie i.p.v. waarde-heuristiek.

🎯 **Een user-typo onder een schaal-bug versterkt verwarring.** Nadat de migratie-bug "opgelost" leek, zag de gebruiker nog steeds 3269km. Mijn reflex: nog meer migratiecode toevoegen. Pas na een nieuwe screenshot bleek dat de Settings-waarde van longitude per ongeluk 52.919140 was ingevoerd (typo: 52.9 ipv 5.29). De waarde die ik had gediagnosticeerd als "halve-migratie corruption" was eigenlijk een handmatige typo achteraf. Les: voordat je doorgaat met een tweede ronde correctie-code, vraag actief om de huidige waarde (Settings-tab screenshot) — anders bouw je oplossingen voor een probleem dat niet meer bestaat.

---

## Update — v2.1 sprint: unread, GPS, ACK-tracking, channel-UI, region-scope (mei 2026)

Eén lange sessie met een serie kleine features en één pijnlijke schaal-bug. Geleerd:

📸 **Een screenshot is een betere spec dan een tekstuele beschrijving.** De gebruiker (= ik) deelde foto's van zijn eigen backlog-lijst plus van de iPhone MeshCore-app chat-header. Zonder die referenties had ik ofwel verkeerd geraden over UI-hiërarchie, ofwel uren in protocol-RE gestoken om "iets dergelijks" te bouwen. Bij volgende sessies: vraag actief om visuele referenties als de gebruiker een doel-look beschrijft. Het is veel minder werk dan reverse-engineering en het bevriest het verwachtingsmanagement vooraf.

🔍 **Bekijk altijd de upstream-broncode vóórdat je "reverse engineering" plant.** Voor de region-scope wire-format dacht ik aan een sniffer-setup met de T-Beam — totdat één `grep -rn "RegionMap"` in de upstream MeshCore-repo het complete mechanisme onthulde: `helpers/RegionMap.h` + `TransportKeyStore.cpp::calcTransportCode` + `companion_radio/MyMesh.cpp::sendFloodScoped`. De wire-layout was `ROUTE_TYPE_TRANSPORT_FLOOD + transport_codes[2]` met `code = HMAC-SHA256(SHA256(region_name)[0..15], type || payload)[0..1]`. Implementatie was één helper aanroepen vóór serialize. Les: "ik weet niet hoe dit werkt" is meestal "ik heb de bron niet gelezen".

⌨️ **Elke input-event-route moet text-input-modes ondersteunen.** Een gebruiker meldde "Enter doet niks bij Add channel". Mijn handler werkte voor `\r/\n` via de keyboard-route, maar `BSP_INPUT_NAVIGATION_KEY_RETURN` (D-pad center) ging een aparte handler in die met `!channel_adding` als guard de save-pad oversloeg. Les: elke modus die tekstinvoer doet, MOET worden geregistreerd in álle event-paden (keyboard + nav). Eén grep op je modus-flag zou hierop moeten matchen — als de flag op één plek staat en niet op de ander, dan klopt iets niet.

🪞 **Header-context > inline tekstprefix.** Vorige sessie had ik `[#test] name: text` prefixen toegevoegd om channels te disambiguëren in de chat-ring. Zodra de chat-header echte channel-context kreeg (channel-naam + `Region: nl` onder elkaar — gemodelleerd naar iPhone-screenshot), werd de inline prefix overbodig en visueel rommelig. Verwijderd uit 4 plekken (TX×2, RX×2). Les: redundante visualisaties stapelen zich op tijdens iteratie — periodiek schoonmaken hoort erbij. Of beter: stel jezelf bij iedere nieuwe display-affordance de vraag "welke oude is hierdoor overbodig?".

🤝 **ACK-matching via CRC-binding aan de sender.** Voor de "ack" indicator op eigen DMs: bij send_dm_message bereken ik de ACK-CRC die de receiver straks zal teruggeven (`SHA256(plaintext[0..5] || dm_text || OUR_pubkey)[0..4]`), bewaar die op de chat_msg, en voeg een PATH_RETURN inbound handler toe die de inner-block ontsleutelt met de sender's shared secret en matched op die CRC. Twee dingen geleerd: (1) het binnenste ACK-mechanisme van MeshCore is afgeleid uit de plaintext + ontvanger's pubkey, dus we kunnen het deterministisch vooraf berekenen. (2) Een sender bouwen die PATH_RETURN STUURT is niet hetzelfde als een sender die PATH_RETURN ONTVANGT — die paden zijn gescheiden, en ik had alleen de TX-kant.

---

## Update — Emoji RX+TX + app-icon (mei 2026)

🎨 **Programmatisch tekenen versus echte bitmaps.** Eerste poging: emoji handgetekend met `pax_simple_circle` + `pax_simple_arc`. Werkte technisch, maar het resultaat zag eruit als gele blobs zonder duidelijke features. Tweede poging: Twemoji 32×32 ARGB-bitmaps embedded als `static const uint32_t[]` in flash. `pax_buf_init` met een non-NULL pointer wrapt die in een `pax_buf_t`, `pax_draw_image_sized` schaalt het naar de inline-tekstgrootte. Resultaat: herkenbaar, professioneel, en kosteloos op rendering (PAX heeft hardware-acceleratie voor image-draw). Les: voor pixel-art met expressie is een asset-pipeline (download + downscale + embed) goedkoper dan programmatisch tekenen. Programmatic werkt voor schema-iconen (pijlen, vinkjes); voor karakter-glyphs niet.

🔁 **Cross-platform UTF-8 is gratis als je het ook echt UTF-8 doet.** De wire-format voor emoji is gewoon de Unicode codepoint als 4-byte UTF-8. Geen custom protocol, geen tokens, geen aparte payload-type. Een ontvanger die emoji-aware is (iPhone-app) toont het emoji; een ontvanger die het niet kent (T-Beam zonder display) toont een `?`-fallback of niets — maar het bericht *blijft correct*. Les: voor protocol-features die optioneel zijn aan ontvangerkant, kies een wire-format dat zonder feature degradeert naar "niets doen" in plaats van "kapot bericht".

🧹 **Twee event-paden voor één modus = twee handlers.** Bij eerste flash-test: emoji-picker selectie via D-pad werkte niet, en de picker bleef hangen bij volgende typ-sessie. Diagnose: keyboard Enter levert `\r` aan `handle_key`, D-pad center levert `BSP_INPUT_NAVIGATION_KEY_RETURN` aan `handle_nav`. Mijn handler zat alleen in handle_key. Les: elke modus die tekstinvoer/navigatie heeft, MOET geregistreerd worden in álle event-paden die de mode aan/uit kunnen zetten. Eén `grep` op de mode-flag moet matchen in beide handlers — als één pad ontbreekt, is de modus alleen via één input-methode bedienbaar.

🔘 **Vraag de hardware, niet je intuïtie.** Eerste keuze voor emoji-shortcut: `BSP_INPUT_NAVIGATION_KEY_GAMEPAD_A`. Build groen, niets gebeurde op de badge. Bleek dat Tanmatsu's BSP de gekleurde knoppen niet als GAMEPAD_* mapt maar als `F1..F6`. Launcher source `home.c` regel 355 bevestigt het: F4 is de groene cirkel. Les: BSP-key-mappings verschillen per badge-target. Sniff een leverende app (de launcher) om de mapping te leren, niet de generieke BSP-enum-namen.

🏷️ **App-icon: AppFS heeft geen icon-slot.** Custom tile-icoon werkt niet via `badgelink appfs upload` — dat protocol heeft alleen slug/title/version/size. De launcher's `app_metadata_parser` leest icoon en metadata vanuit `<slug>/metadata.json` op SD. Pad: een SD-bundle (binary + metadata.json + icon32.png in een map per slug). Tegelijk is dat ook precies de bundle die een toekomstige appstore-distributie nodig heeft. Les: een tweede install-route hebben (naast snelle-dev AppFS) opent niet alleen het tile-icoon, maar legt ook de fundering voor latere distributie. Niet duplicatie, maar evolutie.

---

## Update — Regelgeving aan boord + radio-gevoeligheid (mei 2026)

📡 **Regelgeving is geen vinkje, maar een laag.** Een gebruiker uit de community wees op boetes voor zenden buiten de toegestane band/power — in sommige landen tot in de tienduizenden euro's. Oplossing: een `Country`-veld dat de juiste limieten oplaadt. Cruciaal detail: de EU 863-870 MHz-band is geen één-regel-limiet maar **zes sub-banden** (g/g1/g1'/g2/g3/g4), elk met eigen power- en duty-cycle-grenzen. Meshtastic plet elke regio tot één sub-band; ik hou de volle granulariteit aan zodat de check klopt op de frequentie die je écht gebruikt. Les: regelgeving in software is een datalaag met de granulariteit van de echte regels — niet één enkele "max power"-constante.

⚖️ **Soft waarschuwen waar het jouw keuze is, hard blokkeren waar het de mesh schaadt.** Frequentie buiten band of een paar dB te veel → rood kader + waarschuwing, maar je mag het (je hebt misschien een vergunning of richtantenne). Duty cycle → hard afgedwongen: een rollend 1-uurs airtime-budget (Semtech time-on-air formule, 60 minuut-buckets), en TX wordt **geblokkeerd** als het budget op is. Het verschil: airtime-vreten degradeert actief de gedeelde mesh, dus dáár ligt de grens niet bij de operator.

🔆 **Een meetwaarde meet niet altijd wat je denkt.** RxBoost aanzetten (`0x96` naar RxGain-register `0x08AC`) gaf "stukken betere" ontvangst in het veld — maar de noise floor bleef exact gelijk. Waarom? De SX1262 rapporteert RSSI gekalibreerd/absoluut, gerefereerd aan de antenne-ingang, dus de extra LNA-gain wordt door de chip weggecompenseerd. De winst zit in zwak-signaal-decodering (pakketten over tijd), niet in de noise floor. Les: verifieer een feature met de metric die 'm écht meet, niet de eerste die plausibel lijkt — anders concludeer je "werkt niet" terwijl het wél werkt.

---

## Update — Upstream merge + Settings-opschoning (mei 2026)

🔀 **Een fork is een delta, geen eindbestemming.** Mijn RSSI/SNR-bijdrage is gemerged bij Nicolai Electronics (upstream). In plaats van mijn Gitea-forks te laten divergeren heb ik ze ge-rebased op de gemergede upstream, zodat er nog maar **twee** commits delta overblijven (rx_boost + firmware-versie-query) — features die nog niet upstream zijn. Hoe dunner de delta, hoe triviaal de volgende upstream-bump. Les: behandel een fork als tijdelijke overlay op upstream, niet als een parallel universum.

🗂️ **Volgorde is gratis structuur.** Settings was naar 22 velden gegroeid en werd onoverzichtelijk. Ik vreesde een navigatie-refactor voor sectie-headings, maar de truc bleek: houd **enum-volgorde gelijk aan display-volgorde** en laat de cursor een veld-index blijven. Headings worden dan puur render-artefacten (niet-selecteerbaar) — de input-code hoeft niet te veranderen. Alleen de scroll moest naar pixel-based (met clipping) zodat de kortere heading-rijen meescrollen. Les: koppel weergave los van navigatie door de volgorde gelijk te houden, niet door extra state toe te voegen.

💾 **Efemere UI-state hoort geen formaat-migratie te forceren.** Per-gesprek ongelezen-tellers en scroll-positie: ik bewaar ze in RAM-only parallelle arrays i.p.v. in de NVS-blobs van contacts/channels. Anders had het toevoegen van één veld de oude opslag gecorrumpeerd — die code leidt het aantal items af uit de blob-grootte. Les: niet alles wat per-item is hoeft persistent; weeg een formaat-migratie af tegen simpelweg "opnieuw beginnen bij reboot".

💬 **Zet hulp op de plek van de twijfel.** De footer toont nu per geselecteerd veld een korte uitleg — Sync word en Preamble leggen zichzelf uit, en op Country/Frequentie/TX power/Duty cycle verschijnen de limieten van de actieve sub-band (range, max dBm ERP/EIRP, % duty). Les: contextuele micro-uitleg op het veld zelf is effectiever dan een losse handleiding die niemand opzoekt.

---

## Update — Upstream-firmware sync: twee één-regel-bugs gejaagd (mei 2026)

De upstream-maintainer refactorde de radio-firmware ingrijpend (de lora-driver werd een component, nieuwe protocol-servers). Meegaan kostte twee dagen jagen op twee één-regel-firmwarebugs.

🔌 **`esp_err_t` kaal in een `if` is een omgekeerde-logica-val.** Na de overstap kwam er niks meer binnen. De C6 ontving wél (console: "preamble → header → Data available!"), maar de host kreeg geen pakket. De dader: `if (lora_receive_packet(...))`, terwijl die functie `esp_err_t` teruggeeft — en `ESP_OK` is `0`. Dus bij een geslaagde ontvangst was de conditie `if(0)` → onwaar → nooit doorsturen. Eén `== ESP_OK` en RX werkte. Les: een functie met een "0 = succes"-conventie hoort niet kaal in een `if` — het leest als "if success" maar betekent "if failure".

🪡 **Capture op de juiste laag wijst de bug aan.** Ik wist alleen "geen RX" — dat kon overal zitten (radio, transport, host-parsing). Door de C6-console mee te lezen terwijl een buurnode zond, zag ik dat de radio + interrupt + read-queue prima werkten. Dat sneed de zoekruimte in één klap weg: alles vóór de forward klopte, dus de bug zat ín de forward. Les: meet zo dicht mogelijk bij je vermoeden, en bevestig wat wél werkt — dat is net zo informatief als wat niet werkt.

📇 **`ESP_ERR_NO_MEM` is niet altijd de heap.** De eerste blokkade was subtieler: de esp-hosted-slave heeft een vaste callback-tabel (`MAX_CUSTOM_MSG_HANDLERS=3`), maar de nieuwe firmware registreert er 6 (echo/ir/badgelink/system/ieee802154/lora). De eerste 3 pasten; de rest — incl. LoRa, als laatste geregistreerd — viel eraf met `ESP_ERR_NO_MEM`. De bootlog las als "geen geheugen", maar het was een volle vaste array. Les: lees de regel erboven (`No space for callback (max 3)`) vóór je heap gaat debuggen.

🤝 **Synchroniseren met een bewegend doel.** De maintainer was diezelfde dag volop aan het refactoren (v3.1.0, system-protocol, launcher-updates). Onze fixes zijn één-regelaars: lokaal in onze fork toegepast om door te kunnen, én meteen als issue + reproductie + bootlog teruggemeld zodat het upstream gefixt wordt i.p.v. permanent in onze fork te leven. Les: bij een actief-bewegende upstream — fix lokaal om verder te komen, maar rapporteer direct met genoeg detail dat de maintainer het kan overnemen.

---

## Update — Issue #18 gemerged: de fork krimpt (mei 2026)

Vervolg op het vorige: de maintainer mergede beide gemelde firmwarebugs upstream (radio **v3.1.1**: de RX-fix als `0ca17e3 "Fix receiving LoRa packets (issue #18)"`, de callback-limiet als `MAX_CUSTOM_MSG_HANDLERS=6`). Tijd om de forks op te ruimen.

📉 **Een geslaagde bugmelding maakt je eigen patch overbodig — en dat is het doel.** Beide onze fix-commits waren nu redundant. De radio-fork ging van 3 delta-commits terug naar **één** (alleen nog de redirect naar onze lora-fork voor rx_boost), ge-rebased op v3.1.1. Les: het succes van een upstream-bijdrage meet je niet in regels die je toevoegt, maar in regels die je weer kunt wéghalen.

🔢 **Gelijke bestandsgrootte verslaat naïeve mismatch-detectie.** De app-binary veranderde alleen in één versie-string — exact even lang, dus dezelfde grootte. De launcher detecteert een nieuwe versie op revisie-óf-grootte; beide ongewijzigd → hij bleef de oude cache draaien. Fix: AppFS-cache expliciet wissen zodat hij vers vanaf SD herinstalleert. Les: een cache-invalidatie op "grootte of revisie" mist precies de wijziging die geen van beide raakt.

🏷️ **Hardcoded versie-checks verouderen stilletjes.** De launcher checkte de radio hard op `"v3.1.0"`; onze git-described radio meldt `v3.1.1-1-g…` → valse mismatch én de gevaarlijke "Update radio" downgrade-tegel verscheen. In onze launcher-fork prefix-matchen we nu `v3.1.` — dat verbergt de waarschuwing én de tegel. Les: vergelijk firmwareversies op een betekenis-niveau (lijn/range), niet op een exacte string die elke build verandert.

🍴 **Drie forks, drie groottes delta.** Eindstaat: radio = upstream + 1 (rx_boost-redirect), lora = upstream + 1 (rx_boost), launcher = upstream + 3 (WiFi-autoconnect, versie-accept, `[C]`-tag verbergen). Alles wat kón convergeren is geconvergeerd; wat rest zijn bewuste features. Les: documenteer per fork exact wat de delta is en waaróm — dan blijft "kan dit terug naar upstream?" een beantwoordbare vraag i.p.v. een archeologische.

---

## Update — Docs, wiki-links & appstore-inzending (mei 2026)

🔗 **Dezelfde markdown, twee renderers, twee link-conventies.** Een nieuwe wiki-pagina opende niet: een kale link `(Firmware-Versions)` werkt in een wiki, maar in de GitHub/Gitea **repo-bestandsbrowser** resolvet 'ie naar een pad zónder extensie → 404. De `.md`-vorm (`(Firmware-Versions.md)`) werkt in béíde. Bonus-valkuil: de gerenderde Gitea-wiki bleek een aparte git-repo die handmatig gesynct moet worden — de bron-`docs/wiki/` was al maanden vóór de wiki uit. Les: weet via welke renderer je docs bekeken worden, en test een link in díe context — "het rendert" is niet "de links werken".

📦 **Het schema is de waarheid, niet de README.** Bij het inpakken voor de Tanmatsu-appstore (PR naar `Nicolai-Electronics/app-repository`) zei de README `license` en `target`, maar de echte validator (`.validator/schema.json`, Ajv) eiste `license_type` en `targets` — en de naam/description-regex verbood `:` en `,`, die in onze beschrijving zaten. `additionalProperties:false` betekende dat een extra `repository`-veld de boel zou breken. Door lokaal tegen dat schema te valideren (Python `jsonschema`) vóór de PR ving ik alles in één keer i.p.v. via een CI-faalronde. Les: valideer tegen de machine-leesbare bron (schema + een werkend voorbeeld), niet tegen het proza eromheen.

🚪 **Een eerste PR van een nieuwe contributor wacht aan de poort.** De PR stond meteen open, maar de metadata-CI draaide niet — GitHub houdt Actions van een first-time contributor tegen tot een maintainer ze goedkeurt (de CLA-check liep wél automatisch). Geen fout, gewoon de regel. Les: als checks "ontbreken" i.p.v. "falen" op een fork-PR, is het vaak approval-gating, geen kapotte CI — wacht op de maintainer i.p.v. te debuggen wat er niet stuk is.

---

## Update — Region scope HMAC en veld-diagnose (mei 2026)

Na de v2.1.0-release kwam een veld-symptoom binnen: berichten van Tanmatsu naar het publieke `#test`-kanaal verschenen niet op mc-radar (een externe relay-dashboard), terwijl exact dezelfde berichten vanaf een Heltec wél werden geïndexeerd. Een dag jagen leidde naar een één-karakter-bug in mijn HMAC-key-derivation én een paar lessen over veld-diagnose.

🔬 **De spec zit soms in een upstream-cpp-file, niet in de documentatie.** Mijn `apply_region_scope` derivede de transport-key uit `SHA256("nl")`, conform de protocol-docs. Maar upstream's `RegionMap::getTransportKeysFor` prepent stilletjes een `#` vóór SHA256 — *"implicit auto hashtag region"*. Mijn HMAC-codes mismatchten dus elk scope-aware relay, en die droppen wat ze niet kunnen verifiëren. Side-by-side van mijn 9 regels HMAC-code naast upstream's 14 regels onthulde 't in seconden. Les: voor protocol-keys is de definitieve referentie de regenerator-implementatie, niet de menselijke beschrijving — een proza-spec is altijd een samenvatting.

📐 **Een direct-vs-relay test splitst je foutruimte in twee.** Voor ik 't probleem aan de HMAC kon vastpinnen, moest ik weten: zit het in mijn TX-bytes of in het relay-pad? Eén test scheidt die: een Heltec staat 2m van Tanmatsu, geen repeater nodig. Stuur naar `#test`, kijk of Heltec 'm in z'n channel-view krijgt. Werkte → mijn bytes zijn correct → het probleem zit gegarandeerd in de relay. Werkte niet → het probleem zit in de packet-bouw. Zonder die scheiding had ik uren in `apply_region_scope` kunnen zitten zonder zekerheid dat dáár de bug zat. Les: als een diagnose meerdere lagen omvat, ontwerp een test die precies één laag uitschakelt — bekrachtigde isolatie verslaat speculatieve breedte.

🎙️ **De firmware-console is een orakel als je app-code zwijgt.** De radio-co-processor logde tijdens iedere send `Can not set TX mode directly` — mijn app riep een `lora_set_mode(TX)` aan vóór `lora_send_packet`, een patroon dat in een vorige firmware-versie nog werkte maar in de huidige geweigerd wordt. Mijn app checkte de return value niet en de TX ging alsnog door via de nieuwere API, dus niets brak zichtbaar — alleen de console was rumoerig. Zonder een tap op die seriële poort had ik die obsolete wrapper-call nooit gezien. Les: een radio of co-processor met z'n eigen log is een gratis tweede getuige van je gedrag — sluit 'm aan, ook (of juist) als de app zelf stil is.

🔇 **Een silent API-breuk is een tikkende klok.** De `set_mode(TX)`-weigering kwam met een duidelijke ERROR-regel in de console, maar omdat mijn app de return-code negeerde en de daadwerkelijke TX via een ándere call ging, leek alles te werken. Pas toen ik op zoek ging naar een tweede, minder verklaarbare error (een SPI-timeout een seconde ná elke RPC) sprong die set_mode-spam in beeld. Les: een error in een log die je niet leest is praktisch geen error — pas zodra een tweede probleem je dwingt te lezen, blijken de eerste opeens uit te schreeuwen wat ook nog stuk is. Negeer return values niet, ook als de happy path doorgaat.
