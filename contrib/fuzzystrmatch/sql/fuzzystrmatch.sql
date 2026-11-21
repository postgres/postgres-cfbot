CREATE EXTENSION fuzzystrmatch;


SELECT soundex('hello world!');

SELECT soundex('Anne'), soundex('Ann'), difference('Anne', 'Ann');
SELECT soundex('Anne'), soundex('Andrew'), difference('Anne', 'Andrew');
SELECT soundex('Anne'), soundex('Margaret'), difference('Anne', 'Margaret');
SELECT soundex(''), difference('', '');

-- Leading non-alphabetic characters are skipped
SELECT soundex('-hello');
SELECT soundex('3M');
SELECT soundex('!@#');


SELECT levenshtein('GUMBO', 'GAMBOL');
SELECT levenshtein('GUMBO', 'GAMBOL', 2, 1, 1);
SELECT levenshtein('GUMBO', 'GAMBOL', 1, 1, 2000000000);
SELECT levenshtein('GUMBO', 'GAMBOL', 2000000000, 2000000000, 2000000000);
SELECT levenshtein_less_equal('extensive', 'exhaustive', 2);
SELECT levenshtein_less_equal('extensive', 'exhaustive', 4);
SELECT levenshtein_less_equal('aaa', 'aaaaa', 1073741824, 0, 1073741824, 10);


SELECT metaphone('GUMBO', 4);

-- Edge cases and errors
SELECT metaphone('', 4);
SELECT metaphone('-', 10);
SELECT metaphone(repeat('a', 256), 4);
SELECT metaphone('gumbo', 256);
SELECT metaphone('gumbo', 0);
SELECT metaphone('gumbo', -1);

-- The first letter of the word is handled specially
SELECT w, metaphone(w, 10) FROM unnest(ARRAY[
  'aero',       -- AE becomes E
  'anne',       -- other leading vowels are preserved
  'gnome',      -- GN becomes N
  'knight',     -- KN becomes N
  'pneumatic',  -- PN becomes N
  'white',      -- WH becomes H
  'wrong',      -- WR becomes R
  'water',      -- W before a vowel is kept
  'wto',        -- W not before a vowel is dropped
  'xray',       -- X becomes S
  'eat', 'igloo', 'open', 'under'
]) AS w;

-- The rest of the word
SELECT w, metaphone(w, 10) FROM unnest(ARRAY[
  'bob',        -- B
  'dumb',       -- B in MB is dropped
  'special',    -- CIA becomes X
  'science',    -- C in SCI is dropped, C before E becomes S
  'christ',     -- CH before R becomes K
  'school',     -- CH after S becomes K
  'anchor',     -- CH elsewhere in the word becomes X
  'chair',      -- CH otherwise becomes X
  'judge',      -- DG before E becomes J
  'daughter',   -- GH after D is silent
  'knight',     -- GH otherwise becomes F
  'alright',    -- GH far enough into the word to look four letters back
  'sign',       -- GN at the end of a word is dropped
  'signed',     -- GNED is dropped
  'magnet',     -- GN otherwise becomes K
  'george',     -- G before E becomes J
  'behave',     -- H before a vowel is kept, V becomes F
  'phone',      -- PH becomes F, H after P is dropped
  'black',      -- K after C is dropped
  'kite',       -- K otherwise is kept
  'queen',      -- Q becomes K
  'vision',     -- SIO becomes X
  'shoe',       -- SH becomes X
  'schwa',      -- SCHW becomes X
  'nation',     -- TIO becomes X
  'thing',      -- TH becomes 0
  'boxer',      -- X becomes KS
  'yellow',     -- Y before a vowel is kept
  'zebra',      -- Z becomes S
  'o''brien'    -- non-alphabetic characters are ignored
]) AS w;


SELECT dmetaphone('gumbo');
SELECT dmetaphone_alt('gumbo');

SELECT dmetaphone(''), dmetaphone_alt('');

SELECT w, dmetaphone(w) AS dm, dmetaphone_alt(w) AS alt FROM unnest(ARRAY[
  'Smith', 'Schmidt', 'Wagner', 'Kowalski', 'Czerny', 'McCarthy',
  'Caesar', 'Chianti', 'Michael', 'Chorus', 'Architect', 'Orchestra',
  'Bacher', 'Wachter', 'Ghislaine', 'Hugh', 'Laugh', 'Gnome', 'Judge',
  'Sugar', 'Island', 'Jose', 'Jackson', 'Villa', 'Pfeiffer', 'Thomas',
  'Xavier', 'Zhao', 'Bordeaux', 'Dumb', 'Knight', 'Wright', 'Gonzalez',
  'Sanchez', 'Nowakowski', 'Anne', 'Breaux', 'Van Gogh', 'Schwarzenegger',
  'Campbell', 'MacGregor', 'Tchaikovsky', 'Dvorak', 'Niece', 'Resnais',
  'Arnow', 'Filipowicz', 'Rogier', 'Hochmeier', 'Vasquez', 'Yankelovich'
]) AS w;

-- Rarer branches of the encoder.  Most of these are letter combinations that
-- only show up in names of a particular origin.
SELECT w, dmetaphone(w) AS dm, dmetaphone_alt(w) AS alt FROM unnest(ARRAY[
  'McHugh',         -- CH preceded by MC
  'Chopin',         -- CH treated as K rather than X
  'Fuccia',         -- CC followed by IA
  'Accede',         -- CC followed by E
  'Bellocchio',     -- CC followed by H
  'Ecclesiastes',   -- CC followed by a consonant, and SIA
  'Wojciech',       -- CI in a Slavic name, and a trailing CH
  'Judgment',       -- DG followed by a consonant
  'Burgh',          -- GH preceded by a consonant
  'Ghana',          -- GH at the start of a word
  'Agnes',          -- GN after an initial vowel
  'Lasagna',        -- GN inside a word
  'Gigli',          -- GLI in an Italian name
  'Berger',         -- GER at the end of a word
  'Gergen',         -- G before E, not at the start
  'Schlegel',       -- G before E in a word starting with SCH
  'Gersheim',       -- G before E, and SH followed by HEIM
  'Josephine',      -- initial JOSE not followed by a space, and PH
  'Ajax',           -- J inside a word, and X
  'Raj',            -- J at the end of a word
  'Shipwright',     -- SH inside a word, and WR not at the start
  'Stensholm',      -- SH followed by HOLM
  'Kasian',         -- SIAN in a Slavic name
  'Schenker',       -- SCH followed by a consonant
  'Schooner',       -- SCH followed by OO
  'Brescia',        -- SC followed by I
  'Scot',           -- SC followed by a consonant
  'Nation',         -- TION
  'Wheeler'         -- W followed by H
]) AS w;

-- A long input
SELECT dmetaphone(repeat('schwarzenegger', 20));

-- Repeated GLI adds two characters to the primary code but only one to the
-- secondary one, which is what makes the code outgrow its initial buffer
SELECT dmetaphone('agliagliaglia');

-- Wovels
SELECT daitch_mokotoff('Augsburg');
SELECT daitch_mokotoff('Breuer');
SELECT daitch_mokotoff('Freud');

-- The letter "H"
SELECT daitch_mokotoff('Halberstadt');
SELECT daitch_mokotoff('Mannheim');

-- Adjacent sounds
SELECT daitch_mokotoff('Chernowitz');

-- Adjacent letters with identical adjacent code digits
SELECT daitch_mokotoff('Cherkassy');
SELECT daitch_mokotoff('Kleinman');

-- More than one word
SELECT daitch_mokotoff('Nowy Targ');

-- Padded with "0"
SELECT daitch_mokotoff('Berlin');

-- Other examples from https://www.avotaynu.com/soundex.htm
SELECT daitch_mokotoff('Ceniow');
SELECT daitch_mokotoff('Tsenyuv');
SELECT daitch_mokotoff('Holubica');
SELECT daitch_mokotoff('Golubitsa');
SELECT daitch_mokotoff('Przemysl');
SELECT daitch_mokotoff('Pshemeshil');
SELECT daitch_mokotoff('Rosochowaciec');
SELECT daitch_mokotoff('Rosokhovatsets');

-- Ignored characters
SELECT daitch_mokotoff('''OBrien');
SELECT daitch_mokotoff('O''Brien');

-- Input with no encodable characters
SELECT daitch_mokotoff('');
SELECT daitch_mokotoff('123');

-- ASCII characters reserved for internal conversions
SELECT daitch_mokotoff('a[b');

-- "Difficult" cases, likely to cause trouble for other implementations.
SELECT daitch_mokotoff('CJC');
SELECT daitch_mokotoff('BESST');
SELECT daitch_mokotoff('BOUEY');
SELECT daitch_mokotoff('HANNMANN');
SELECT daitch_mokotoff('MCCOYJR');
SELECT daitch_mokotoff('ACCURSO');
SELECT daitch_mokotoff('BIERSCHBACH');
