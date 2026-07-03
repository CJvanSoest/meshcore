#!/usr/bin/env python3
# Emits SPECIAL_SET rows for special_table.c. Frequency-ordered, 4 cols x 10 rows.
chars = [
    # row 1 — German lowercase (most used)
    ('ä','a-umlaut'), ('ö','o-umlaut'), ('ü','u-umlaut'), ('ß','sharp-s'),
    # row 2 — German uppercase + degree
    ('Ä','A-umlaut'), ('Ö','O-umlaut'), ('Ü','U-umlaut'), ('°','degree'),
    # row 3 — currency / section / micro / spanish n
    ('€','euro'), ('§','section'), ('µ','micro'), ('ñ','n-tilde'),
    # row 4 — french e variants
    ('é','e-acute'), ('è','e-grave'), ('ê','e-circ'), ('ë','e-diaer'),
    # row 5 — a variants + cedilla
    ('à','a-grave'), ('â','a-circ'), ('á','a-acute'), ('ç','c-cedilla'),
    # row 6 — i/o variants
    ('î','i-circ'), ('ï','i-diaer'), ('í','i-acute'), ('ô','o-circ'),
    # row 7 — u/o variants
    ('û','u-circ'), ('ù','u-grave'), ('ú','u-acute'), ('ó','o-acute'),
    # row 8 — nordic a/ae
    ('å','a-ring'), ('Å','A-ring'), ('æ','ae'), ('Æ','AE'),
    # row 9 — nordic o + spanish inverted
    ('ø','o-slash'), ('Ø','O-slash'), ('¿','inv-question'), ('¡','inv-exclaim'),
    # row 10 — guillemets + dashes
    ('«','laquo'), ('»','raquo'), ('–','en-dash'), ('—','em-dash'),
]
assert len(chars) == 40, len(chars)
for ch, name in chars:
    b = ch.encode('utf-8')
    esc = ''.join('\\x%02X' % x for x in b)
    cp = ord(ch)
    print('    {0x%04X, "%s", %d},  // %s' % (cp, esc, len(b), name))
