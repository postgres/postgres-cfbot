CREATE TABLE xmltest (
    id int,
    data xml
);

INSERT INTO xmltest VALUES (1, '<value>one</value>');
INSERT INTO xmltest VALUES (2, '<value>two</value>');
INSERT INTO xmltest VALUES (3, '<value>three</wrong>  ');

-- If no XML data could be inserted, skip the tests as the server has been
-- compiled without libxml support.
SELECT count(*) = 0 AS skip_test FROM xmltest \gset
\if :skip_test
\quit
\endif

SELECT * FROM xmltest;

-- test non-throwing API, too
SELECT pg_input_is_valid('<value>one</value>', 'xml');
SELECT pg_input_is_valid('<value>one</', 'xml');
SELECT message FROM pg_input_error_info('<value>one</', 'xml');
SELECT pg_input_is_valid('<?xml version="1.0" standalone="y"?><foo/>', 'xml');
SELECT message FROM pg_input_error_info('<?xml version="1.0" standalone="y"?><foo/>', 'xml');


SELECT xmlcomment('test');
SELECT xmlcomment('-test');
SELECT xmlcomment('test-');
SELECT xmlcomment('--test');
SELECT xmlcomment('te st');


SELECT xmlconcat(xmlcomment('hello'),
                 xmlelement(NAME qux, 'foo'),
                 xmlcomment('world'));

SELECT xmlconcat('hello', 'you');
SELECT xmlconcat(1, 2);
SELECT xmlconcat('bad', '<wrong></syntax>  ');
SELECT xmlconcat('<foo/>', NULL, '<?xml version="1.1" standalone="no"?><bar/>');
SELECT xmlconcat('<?xml version="1.1"?><foo/>', NULL, '<?xml version="1.1" standalone="no"?><bar/>');
SELECT xmlconcat(NULL);
SELECT xmlconcat(NULL, NULL);


SELECT xmlelement(name element,
                  xmlattributes (1 as one, 'deuce' as two),
                  'content');

SELECT xmlelement(name element,
                  xmlattributes ('unnamed and wrong'));

SELECT xmlelement(name element, xmlelement(name nested, 'stuff'));

SELECT xmlelement(name employee, xmlforest(name, age, salary as pay)) FROM emp;

SELECT xmlelement(name duplicate, xmlattributes(1 as a, 2 as b, 3 as a));

SELECT xmlelement(name num, 37);
SELECT xmlelement(name foo, text 'bar');
SELECT xmlelement(name foo, xml 'bar');
SELECT xmlelement(name foo, text 'b<a/>r');
SELECT xmlelement(name foo, xml 'b<a/>r');
SELECT xmlelement(name foo, array[1, 2, 3]);
SET xmlbinary TO base64;
SELECT xmlelement(name foo, bytea 'bar');
SET xmlbinary TO hex;
SELECT xmlelement(name foo, bytea 'bar');

SELECT xmlelement(name foo, xmlattributes(true as bar));
SELECT xmlelement(name foo, xmlattributes('2009-04-09 00:24:37'::timestamp as bar));
SELECT xmlelement(name foo, xmlattributes('infinity'::timestamp as bar));
SELECT xmlelement(name foo, xmlattributes('<>&"''' as funny, xml 'b<a/>r' as funnier));


SELECT xmlparse(content '');
SELECT xmlparse(content '  ');
SELECT xmlparse(content 'abc');
SELECT xmlparse(content '<abc>x</abc>');
SELECT xmlparse(content '<invalidentity>&</invalidentity>');
SELECT xmlparse(content '<undefinedentity>&idontexist;</undefinedentity>');
SELECT xmlparse(content '<invalidns xmlns=''&lt;''/>');
SELECT xmlparse(content '<relativens xmlns=''relative''/>');
SELECT xmlparse(content '<twoerrors>&idontexist;</unbalanced>  ');
SELECT xmlparse(content '<nosuchprefix:tag/>');

SELECT xmlparse(document '!');
SELECT xmlparse(document 'abc');
SELECT xmlparse(document '<abc>x</abc>');
SELECT xmlparse(document '<invalidentity>&</abc>  ');
SELECT xmlparse(document '<undefinedentity>&idontexist;</abc>  ');
SELECT xmlparse(document '<invalidns xmlns=''&lt;''/>');
SELECT xmlparse(document '<relativens xmlns=''relative''/>');
SELECT xmlparse(document '<twoerrors>&idontexist;</unbalanced>  ');
SELECT xmlparse(document '<nosuchprefix:tag/>');


SELECT xmlpi(name foo);
SELECT xmlpi(name xml);
SELECT xmlpi(name xmlstuff);
SELECT xmlpi(name foo, 'bar');
SELECT xmlpi(name foo, 'in?>valid');
SELECT xmlpi(name foo, null);
SELECT xmlpi(name xml, null);
SELECT xmlpi(name xmlstuff, null);
SELECT xmlpi(name "xml-stylesheet", 'href="mystyle.css" type="text/css"');
SELECT xmlpi(name foo, '   bar');


SELECT xmlroot(xml '<foo/>', version no value, standalone no value);
SELECT xmlroot(xml '<foo/>', version '2.0');
SELECT xmlroot(xml '<foo/>', version no value, standalone yes);
SELECT xmlroot(xml '<?xml version="1.1"?><foo/>', version no value, standalone yes);
SELECT xmlroot(xmlroot(xml '<foo/>', version '1.0'), version '1.1', standalone no);
SELECT xmlroot('<?xml version="1.1" standalone="yes"?><foo/>', version no value, standalone no);
SELECT xmlroot('<?xml version="1.1" standalone="yes"?><foo/>', version no value, standalone no value);
SELECT xmlroot('<?xml version="1.1" standalone="yes"?><foo/>', version no value);


SELECT xmlroot (
  xmlelement (
    name gazonk,
    xmlattributes (
      'val' AS name,
      1 + 1 AS num
    ),
    xmlelement (
      NAME qux,
      'foo'
    )
  ),
  version '1.0',
  standalone yes
);


SELECT xmlserialize(content data as character varying(20)) FROM xmltest;
SELECT xmlserialize(content 'good' as char(10));
SELECT xmlserialize(document 'bad' as text);

-- indent
SELECT xmlserialize(DOCUMENT '<foo><bar><val x="y">42</val></bar></foo>' AS text INDENT);
SELECT xmlserialize(CONTENT  '<foo><bar><val x="y">42</val></bar></foo>' AS text INDENT);
-- no indent
SELECT xmlserialize(DOCUMENT '<foo><bar><val x="y">42</val></bar></foo>' AS text NO INDENT);
SELECT xmlserialize(CONTENT  '<foo><bar><val x="y">42</val></bar></foo>' AS text NO INDENT);
-- indent non singly-rooted xml
SELECT xmlserialize(DOCUMENT '<foo>73</foo><bar><val x="y">42</val></bar>' AS text INDENT);
SELECT xmlserialize(CONTENT  '<foo>73</foo><bar><val x="y">42</val></bar>' AS text INDENT);
-- indent non singly-rooted xml with mixed contents
SELECT xmlserialize(DOCUMENT 'text node<foo>73</foo>text node<bar><val x="y">42</val></bar>' AS text INDENT);
SELECT xmlserialize(CONTENT  'text node<foo>73</foo>text node<bar><val x="y">42</val></bar>' AS text INDENT);
-- indent singly-rooted xml with mixed contents
SELECT xmlserialize(DOCUMENT '<foo><bar><val x="y">42</val><val x="y">text node<val>73</val></val></bar></foo>' AS text INDENT);
SELECT xmlserialize(CONTENT  '<foo><bar><val x="y">42</val><val x="y">text node<val>73</val></val></bar></foo>' AS text INDENT);
-- indent empty string
SELECT xmlserialize(DOCUMENT '' AS text INDENT);
SELECT xmlserialize(CONTENT  '' AS text INDENT);
-- whitespaces
SELECT xmlserialize(DOCUMENT '  ' AS text INDENT);
SELECT xmlserialize(CONTENT  '  ' AS text INDENT);
-- indent null
SELECT xmlserialize(DOCUMENT NULL AS text INDENT);
SELECT xmlserialize(CONTENT  NULL AS text INDENT);
-- indent with XML declaration
SELECT xmlserialize(DOCUMENT '<?xml version="1.0" encoding="UTF-8"?><foo><bar><val>73</val></bar></foo>' AS text INDENT);
SELECT xmlserialize(CONTENT  '<?xml version="1.0" encoding="UTF-8"?><foo><bar><val>73</val></bar></foo>' AS text INDENT);
-- indent containing DOCTYPE declaration
SELECT xmlserialize(DOCUMENT '<!DOCTYPE a><a/>' AS text INDENT);
SELECT xmlserialize(CONTENT  '<!DOCTYPE a><a/>' AS text INDENT);
-- indent xml with empty element
SELECT xmlserialize(DOCUMENT '<foo><bar></bar></foo>' AS text INDENT);
SELECT xmlserialize(CONTENT  '<foo><bar></bar></foo>' AS text INDENT);
-- 'no indent' = not using 'no indent'
SELECT xmlserialize(DOCUMENT '<foo><bar><val x="y">42</val></bar></foo>' AS text) = xmlserialize(DOCUMENT '<foo><bar><val x="y">42</val></bar></foo>' AS text NO INDENT);
SELECT xmlserialize(CONTENT  '<foo><bar><val x="y">42</val></bar></foo>' AS text) = xmlserialize(CONTENT '<foo><bar><val x="y">42</val></bar></foo>' AS text NO INDENT);
-- indent xml strings containing blank nodes
SELECT xmlserialize(DOCUMENT '<foo>   <bar></bar>    </foo>' AS text INDENT);
SELECT xmlserialize(CONTENT  'text node<foo>    <bar></bar>   </foo>' AS text INDENT);

SELECT xml '<foo>bar</foo>' IS DOCUMENT;
SELECT xml '<foo>bar</foo><bar>foo</bar>' IS DOCUMENT;
SELECT xml '<abc/>' IS NOT DOCUMENT;
SELECT xml 'abc' IS NOT DOCUMENT;
SELECT '<>' IS NOT DOCUMENT;


SELECT xmlagg(data) FROM xmltest;
SELECT xmlagg(data) FROM xmltest WHERE id > 10;
SELECT xmlelement(name employees, xmlagg(xmlelement(name name, name))) FROM emp;


-- Check mapping SQL identifier to XML name

SELECT xmlpi(name ":::_xml_abc135.%-&_");
SELECT xmlpi(name "123");


PREPARE foo (xml) AS SELECT xmlconcat('<foo/>', $1);

SET XML OPTION DOCUMENT;
EXECUTE foo ('<bar/>');
EXECUTE foo ('bad');
SELECT xml '<!DOCTYPE a><a/><b/>';

SET XML OPTION CONTENT;
EXECUTE foo ('<bar/>');
EXECUTE foo ('good');
SELECT xml '<!-- in SQL:2006+ a doc is content too--> <?y z?> <!DOCTYPE a><a/>';
SELECT xml '<?xml version="1.0"?> <!-- hi--> <!DOCTYPE a><a/>';
SELECT xml '<!DOCTYPE a><a/>';
SELECT xml '<!-- hi--> oops <!DOCTYPE a><a/>';
SELECT xml '<!-- hi--> <oops/> <!DOCTYPE a><a/>';
SELECT xml '<!DOCTYPE a><a/><b/>';


-- Test backwards parsing

CREATE VIEW xmlview1 AS SELECT xmlcomment('test');
CREATE VIEW xmlview2 AS SELECT xmlconcat('hello', 'you');
CREATE VIEW xmlview3 AS SELECT xmlelement(name element, xmlattributes (1 as ":one:", 'deuce' as two), 'content&');
CREATE VIEW xmlview4 AS SELECT xmlelement(name employee, xmlforest(name, age, salary as pay)) FROM emp;
CREATE VIEW xmlview5 AS SELECT xmlparse(content '<abc>x</abc>');
CREATE VIEW xmlview6 AS SELECT xmlpi(name foo, 'bar');
CREATE VIEW xmlview7 AS SELECT xmlroot(xml '<foo/>', version no value, standalone yes);
CREATE VIEW xmlview8 AS SELECT xmlserialize(content 'good' as char(10));
CREATE VIEW xmlview9 AS SELECT xmlserialize(content 'good' as text);
CREATE VIEW xmlview10 AS SELECT xmlserialize(document '<foo><bar>42</bar></foo>' AS text indent);
CREATE VIEW xmlview11 AS SELECT xmlserialize(document '<foo><bar>42</bar></foo>' AS character varying no indent);

SELECT table_name, view_definition FROM information_schema.views
  WHERE table_name LIKE 'xmlview%' ORDER BY 1;

-- Text XPath expressions evaluation

SELECT xpath('/value', data) FROM xmltest;
SELECT xpath(NULL, NULL) IS NULL FROM xmltest;
SELECT xpath('', '<!-- error -->');
SELECT xpath('//text()', '<local:data xmlns:local="http://127.0.0.1"><local:piece id="1">number one</local:piece><local:piece id="2" /></local:data>');
SELECT xpath('//loc:piece/@id', '<local:data xmlns:local="http://127.0.0.1"><local:piece id="1">number one</local:piece><local:piece id="2" /></local:data>', ARRAY[ARRAY['loc', 'http://127.0.0.1']]);
SELECT xpath('//loc:piece', '<local:data xmlns:local="http://127.0.0.1"><local:piece id="1">number one</local:piece><local:piece id="2" /></local:data>', ARRAY[ARRAY['loc', 'http://127.0.0.1']]);
SELECT xpath('//loc:piece', '<local:data xmlns:local="http://127.0.0.1" xmlns="http://127.0.0.2"><local:piece id="1"><internal>number one</internal><internal2/></local:piece><local:piece id="2" /></local:data>', ARRAY[ARRAY['loc', 'http://127.0.0.1']]);
SELECT xpath('//b', '<a>one <b>two</b> three <b>etc</b></a>');
SELECT xpath('//text()', '<root>&lt;</root>');
SELECT xpath('//@value', '<root value="&lt;"/>');
SELECT xpath('''<<invalid>>''', '<root/>');
SELECT xpath('count(//*)', '<root><sub/><sub/></root>');
SELECT xpath('count(//*)=0', '<root><sub/><sub/></root>');
SELECT xpath('count(//*)=3', '<root><sub/><sub/></root>');
SELECT xpath('name(/*)', '<root><sub/><sub/></root>');
SELECT xpath('/nosuchtag', '<root/>');
SELECT xpath('root', '<root/>');
SELECT xpath('//namespace::foo', '<root xmlns:foo="http://127.0.0.1"/>');

-- Round-trip non-ASCII data through xpath().
DO $$
DECLARE
  xml_declaration text := '<?xml version="1.0" encoding="ISO-8859-1"?>';
  degree_symbol text;
  res xml[];
BEGIN
  -- Per the documentation, except when the server encoding is UTF8, xpath()
  -- may not work on non-ASCII data.  The untranslatable_character and
  -- undefined_function traps below, currently dead code, will become relevant
  -- if we remove this limitation.
  IF current_setting('server_encoding') <> 'UTF8' THEN
    RAISE LOG 'skip: encoding % unsupported for xpath',
      current_setting('server_encoding');
    RETURN;
  END IF;

  degree_symbol := convert_from('\xc2b0', 'UTF8');
  res := xpath('text()', (xml_declaration ||
    '<x>' || degree_symbol || '</x>')::xml);
  IF degree_symbol <> res[1]::text THEN
    RAISE 'expected % (%), got % (%)',
      degree_symbol, convert_to(degree_symbol, 'UTF8'),
      res[1], convert_to(res[1]::text, 'UTF8');
  END IF;
EXCEPTION
  -- character with byte sequence 0xc2 0xb0 in encoding "UTF8" has no equivalent in encoding "LATIN8"
  WHEN undefined_function
  -- unsupported XML feature
  OR feature_not_supported THEN
    RAISE LOG 'skip: %', SQLERRM;
END
$$;

-- Test xmlexists and xpath_exists
SELECT xmlexists('//town[text() = ''Toronto'']' PASSING BY REF '<towns><town>Bidford-on-Avon</town><town>Cwmbran</town><town>Bristol</town></towns>');
SELECT xmlexists('//town[text() = ''Cwmbran'']' PASSING BY REF '<towns><town>Bidford-on-Avon</town><town>Cwmbran</town><town>Bristol</town></towns>');
SELECT xmlexists('count(/nosuchtag)' PASSING BY REF '<root/>');
SELECT xpath_exists('//town[text() = ''Toronto'']','<towns><town>Bidford-on-Avon</town><town>Cwmbran</town><town>Bristol</town></towns>'::xml);
SELECT xpath_exists('//town[text() = ''Cwmbran'']','<towns><town>Bidford-on-Avon</town><town>Cwmbran</town><town>Bristol</town></towns>'::xml);
SELECT xpath_exists('count(/nosuchtag)', '<root/>'::xml);

INSERT INTO xmltest VALUES (4, '<menu><beers><name>Budvar</name><cost>free</cost><name>Carling</name><cost>lots</cost></beers></menu>'::xml);
INSERT INTO xmltest VALUES (5, '<menu><beers><name>Molson</name><cost>free</cost><name>Carling</name><cost>lots</cost></beers></menu>'::xml);
INSERT INTO xmltest VALUES (6, '<myns:menu xmlns:myns="http://myns.com"><myns:beers><myns:name>Budvar</myns:name><myns:cost>free</myns:cost><myns:name>Carling</myns:name><myns:cost>lots</myns:cost></myns:beers></myns:menu>'::xml);
INSERT INTO xmltest VALUES (7, '<myns:menu xmlns:myns="http://myns.com"><myns:beers><myns:name>Molson</myns:name><myns:cost>free</myns:cost><myns:name>Carling</myns:name><myns:cost>lots</myns:cost></myns:beers></myns:menu>'::xml);

SELECT COUNT(id) FROM xmltest WHERE xmlexists('/menu/beer' PASSING data);
SELECT COUNT(id) FROM xmltest WHERE xmlexists('/menu/beer' PASSING BY REF data BY REF);
SELECT COUNT(id) FROM xmltest WHERE xmlexists('/menu/beers' PASSING BY REF data);
SELECT COUNT(id) FROM xmltest WHERE xmlexists('/menu/beers/name[text() = ''Molson'']' PASSING BY REF data);

SELECT COUNT(id) FROM xmltest WHERE xpath_exists('/menu/beer',data);
SELECT COUNT(id) FROM xmltest WHERE xpath_exists('/menu/beers',data);
SELECT COUNT(id) FROM xmltest WHERE xpath_exists('/menu/beers/name[text() = ''Molson'']',data);
SELECT COUNT(id) FROM xmltest WHERE xpath_exists('/myns:menu/myns:beer',data,ARRAY[ARRAY['myns','http://myns.com']]);
SELECT COUNT(id) FROM xmltest WHERE xpath_exists('/myns:menu/myns:beers',data,ARRAY[ARRAY['myns','http://myns.com']]);
SELECT COUNT(id) FROM xmltest WHERE xpath_exists('/myns:menu/myns:beers/myns:name[text() = ''Molson'']',data,ARRAY[ARRAY['myns','http://myns.com']]);

CREATE TABLE query ( expr TEXT );
INSERT INTO query VALUES ('/menu/beers/cost[text() = ''lots'']');
SELECT COUNT(id) FROM xmltest, query WHERE xmlexists(expr PASSING BY REF data);

-- Test xml_is_well_formed and variants

SELECT xml_is_well_formed_document('<foo>bar</foo>');
SELECT xml_is_well_formed_document('abc');
SELECT xml_is_well_formed_content('<foo>bar</foo>');
SELECT xml_is_well_formed_content('abc');

SET xmloption TO DOCUMENT;
SELECT xml_is_well_formed('abc');
SELECT xml_is_well_formed('<>');
SELECT xml_is_well_formed('<abc/>');
SELECT xml_is_well_formed('<foo>bar</foo>');
SELECT xml_is_well_formed('<foo>bar</foo');
SELECT xml_is_well_formed('<foo><bar>baz</foo>');
SELECT xml_is_well_formed('<local:data xmlns:local="http://127.0.0.1"><local:piece id="1">number one</local:piece><local:piece id="2" /></local:data>');
SELECT xml_is_well_formed('<pg:foo xmlns:pg="http://postgresql.org/stuff">bar</my:foo>');
SELECT xml_is_well_formed('<pg:foo xmlns:pg="http://postgresql.org/stuff">bar</pg:foo>');
SELECT xml_is_well_formed('<invalidentity>&</abc>');
SELECT xml_is_well_formed('<undefinedentity>&idontexist;</abc>');
SELECT xml_is_well_formed('<invalidns xmlns=''&lt;''/>');
SELECT xml_is_well_formed('<relativens xmlns=''relative''/>');
SELECT xml_is_well_formed('<twoerrors>&idontexist;</unbalanced>');

SET xmloption TO CONTENT;
SELECT xml_is_well_formed('abc');

-- Since xpath() deals with namespaces, it's a bit stricter about
-- what's well-formed and what's not. If we don't obey these rules
-- (i.e. ignore namespace-related errors from libxml), xpath()
-- fails in subtle ways. The following would for example produce
-- the xml value
--   <invalidns xmlns='<'/>
-- which is invalid because '<' may not appear un-escaped in
-- attribute values.
-- Since different libxml versions emit slightly different
-- error messages, we suppress the DETAIL in this test.
\set VERBOSITY terse
SELECT xpath('/*', '<invalidns xmlns=''&lt;''/>');
\set VERBOSITY default

-- Again, the XML isn't well-formed for namespace purposes
SELECT xpath('/*', '<nosuchprefix:tag/>');

-- XPath deprecates relative namespaces, but they're not supposed to
-- throw an error, only a warning.
SELECT xpath('/*', '<relativens xmlns=''relative''/>');

-- External entity references should not leak filesystem information.
SELECT XMLPARSE(DOCUMENT '<!DOCTYPE foo [<!ENTITY c SYSTEM "/etc/passwd">]><foo>&c;</foo>');
SELECT XMLPARSE(DOCUMENT '<!DOCTYPE foo [<!ENTITY c SYSTEM "/etc/no.such.file">]><foo>&c;</foo>');
-- This might or might not load the requested DTD, but it mustn't throw error.
SELECT XMLPARSE(DOCUMENT '<!DOCTYPE chapter PUBLIC "-//OASIS//DTD DocBook XML V4.1.2//EN" "http://www.oasis-open.org/docbook/xml/4.1.2/docbookx.dtd"><chapter>&nbsp;</chapter>');

-- XMLPATH tests
CREATE TABLE xmldata(data xml);
INSERT INTO xmldata VALUES('<ROWS>
<ROW id="1">
  <COUNTRY_ID>AU</COUNTRY_ID>
  <COUNTRY_NAME>Australia</COUNTRY_NAME>
  <REGION_ID>3</REGION_ID>
</ROW>
<ROW id="2">
  <COUNTRY_ID>CN</COUNTRY_ID>
  <COUNTRY_NAME>China</COUNTRY_NAME>
  <REGION_ID>3</REGION_ID>
</ROW>
<ROW id="3">
  <COUNTRY_ID>HK</COUNTRY_ID>
  <COUNTRY_NAME>HongKong</COUNTRY_NAME>
  <REGION_ID>3</REGION_ID>
</ROW>
<ROW id="4">
  <COUNTRY_ID>IN</COUNTRY_ID>
  <COUNTRY_NAME>India</COUNTRY_NAME>
  <REGION_ID>3</REGION_ID>
</ROW>
<ROW id="5">
  <COUNTRY_ID>JP</COUNTRY_ID>
  <COUNTRY_NAME>Japan</COUNTRY_NAME>
  <REGION_ID>3</REGION_ID><PREMIER_NAME>Sinzo Abe</PREMIER_NAME>
</ROW>
<ROW id="6">
  <COUNTRY_ID>SG</COUNTRY_ID>
  <COUNTRY_NAME>Singapore</COUNTRY_NAME>
  <REGION_ID>3</REGION_ID><SIZE unit="km">791</SIZE>
</ROW>
</ROWS>');

-- XMLTABLE with columns
SELECT  xmltable.*
   FROM (SELECT data FROM xmldata) x,
        LATERAL XMLTABLE('/ROWS/ROW'
                         PASSING data
                         COLUMNS id int PATH '@id',
                                  _id FOR ORDINALITY,
                                  country_name text PATH 'COUNTRY_NAME/text()' NOT NULL,
                                  country_id text PATH 'COUNTRY_ID',
                                  region_id int PATH 'REGION_ID',
                                  size float PATH 'SIZE',
                                  unit text PATH 'SIZE/@unit',
                                  premier_name text PATH 'PREMIER_NAME' DEFAULT 'not specified');

CREATE VIEW xmltableview1 AS SELECT  xmltable.*
   FROM (SELECT data FROM xmldata) x,
        LATERAL XMLTABLE('/ROWS/ROW'
                         PASSING data
                         COLUMNS id int PATH '@id',
                                  _id FOR ORDINALITY,
                                  country_name text PATH 'COUNTRY_NAME/text()' NOT NULL,
                                  country_id text PATH 'COUNTRY_ID',
                                  region_id int PATH 'REGION_ID',
                                  size float PATH 'SIZE',
                                  unit text PATH 'SIZE/@unit',
                                  premier_name text PATH 'PREMIER_NAME' DEFAULT 'not specified');

SELECT * FROM xmltableview1;

\sv xmltableview1

EXPLAIN (COSTS OFF) SELECT * FROM xmltableview1;
EXPLAIN (COSTS OFF, VERBOSE) SELECT * FROM xmltableview1;

-- errors
SELECT * FROM XMLTABLE (ROW () PASSING null COLUMNS v1 timestamp) AS f (v1, v2);

SELECT * FROM XMLTABLE (ROW () PASSING null COLUMNS v1 timestamp __pg__is_not_null 1) AS f (v1);

-- XMLNAMESPACES tests
SELECT * FROM XMLTABLE(XMLNAMESPACES('http://x.y' AS zz),
                      '/zz:rows/zz:row'
                      PASSING '<rows xmlns="http://x.y"><row><a>10</a></row></rows>'
                      COLUMNS a int PATH 'zz:a');

CREATE VIEW xmltableview2 AS SELECT * FROM XMLTABLE(XMLNAMESPACES('http://x.y' AS "Zz"),
                      '/Zz:rows/Zz:row'
                      PASSING '<rows xmlns="http://x.y"><row><a>10</a></row></rows>'
                      COLUMNS a int PATH 'Zz:a');

SELECT * FROM xmltableview2;

\sv xmltableview2

SELECT * FROM XMLTABLE(XMLNAMESPACES(DEFAULT 'http://x.y'),
                      '/rows/row'
                      PASSING '<rows xmlns="http://x.y"><row><a>10</a></row></rows>'
                      COLUMNS a int PATH 'a');

SELECT * FROM XMLTABLE('.'
                       PASSING '<foo/>'
                       COLUMNS a text PATH 'foo/namespace::node()');

-- used in prepare statements
PREPARE pp AS
SELECT  xmltable.*
   FROM (SELECT data FROM xmldata) x,
        LATERAL XMLTABLE('/ROWS/ROW'
                         PASSING data
                         COLUMNS id int PATH '@id',
                                  _id FOR ORDINALITY,
                                  country_name text PATH 'COUNTRY_NAME' NOT NULL,
                                  country_id text PATH 'COUNTRY_ID',
                                  region_id int PATH 'REGION_ID',
                                  size float PATH 'SIZE',
                                  unit text PATH 'SIZE/@unit',
                                  premier_name text PATH 'PREMIER_NAME' DEFAULT 'not specified');

EXECUTE pp;

SELECT xmltable.* FROM xmldata, LATERAL xmltable('/ROWS/ROW[COUNTRY_NAME="Japan" or COUNTRY_NAME="India"]' PASSING data COLUMNS "COUNTRY_NAME" text, "REGION_ID" int);
SELECT xmltable.* FROM xmldata, LATERAL xmltable('/ROWS/ROW[COUNTRY_NAME="Japan" or COUNTRY_NAME="India"]' PASSING data COLUMNS id FOR ORDINALITY, "COUNTRY_NAME" text, "REGION_ID" int);
SELECT xmltable.* FROM xmldata, LATERAL xmltable('/ROWS/ROW[COUNTRY_NAME="Japan" or COUNTRY_NAME="India"]' PASSING data COLUMNS id int PATH '@id', "COUNTRY_NAME" text, "REGION_ID" int);
SELECT xmltable.* FROM xmldata, LATERAL xmltable('/ROWS/ROW[COUNTRY_NAME="Japan" or COUNTRY_NAME="India"]' PASSING data COLUMNS id int PATH '@id');
SELECT xmltable.* FROM xmldata, LATERAL xmltable('/ROWS/ROW[COUNTRY_NAME="Japan" or COUNTRY_NAME="India"]' PASSING data COLUMNS id FOR ORDINALITY);
SELECT xmltable.* FROM xmldata, LATERAL xmltable('/ROWS/ROW[COUNTRY_NAME="Japan" or COUNTRY_NAME="India"]' PASSING data COLUMNS id int PATH '@id', "COUNTRY_NAME" text, "REGION_ID" int, rawdata xml PATH '.');
SELECT xmltable.* FROM xmldata, LATERAL xmltable('/ROWS/ROW[COUNTRY_NAME="Japan" or COUNTRY_NAME="India"]' PASSING data COLUMNS id int PATH '@id', "COUNTRY_NAME" text, "REGION_ID" int, rawdata xml PATH './*');

SELECT * FROM xmltable('/root' passing '<root><element>a1a<!-- aaaa -->a2a<?aaaaa?> <!--z-->  bbbb<x>xxx</x>cccc</element></root>' COLUMNS element text);
SELECT * FROM xmltable('/root' passing '<root><element>a1a<!-- aaaa -->a2a<?aaaaa?> <!--z-->  bbbb<x>xxx</x>cccc</element></root>' COLUMNS element text PATH 'element/text()'); -- should fail

-- CDATA test
select * from xmltable('d/r' passing '<d><r><c><![CDATA[<hello> &"<>!<a>foo</a>]]></c></r><r><c>2</c></r></d>' columns c text);

-- XML builtin entities
SELECT * FROM xmltable('/x/a' PASSING '<x><a><ent>&apos;</ent></a><a><ent>&quot;</ent></a><a><ent>&amp;</ent></a><a><ent>&lt;</ent></a><a><ent>&gt;</ent></a></x>' COLUMNS ent text);
SELECT * FROM xmltable('/x/a' PASSING '<x><a><ent>&apos;</ent></a><a><ent>&quot;</ent></a><a><ent>&amp;</ent></a><a><ent>&lt;</ent></a><a><ent>&gt;</ent></a></x>' COLUMNS ent xml);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT  xmltable.*
   FROM (SELECT data FROM xmldata) x,
        LATERAL XMLTABLE('/ROWS/ROW'
                         PASSING data
                         COLUMNS id int PATH '@id',
                                  _id FOR ORDINALITY,
                                  country_name text PATH 'COUNTRY_NAME' NOT NULL,
                                  country_id text PATH 'COUNTRY_ID',
                                  region_id int PATH 'REGION_ID',
                                  size float PATH 'SIZE',
                                  unit text PATH 'SIZE/@unit',
                                  premier_name text PATH 'PREMIER_NAME' DEFAULT 'not specified');

-- test qual
SELECT xmltable.* FROM xmldata, LATERAL xmltable('/ROWS/ROW[COUNTRY_NAME="Japan" or COUNTRY_NAME="India"]' PASSING data COLUMNS "COUNTRY_NAME" text, "REGION_ID" int) WHERE "COUNTRY_NAME" = 'Japan';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT f.* FROM xmldata, LATERAL xmltable('/ROWS/ROW[COUNTRY_NAME="Japan" or COUNTRY_NAME="India"]' PASSING data COLUMNS "COUNTRY_NAME" text, "REGION_ID" int) AS f WHERE "COUNTRY_NAME" = 'Japan';

EXPLAIN (VERBOSE, FORMAT JSON, COSTS OFF)
SELECT f.* FROM xmldata, LATERAL xmltable('/ROWS/ROW[COUNTRY_NAME="Japan" or COUNTRY_NAME="India"]' PASSING data COLUMNS "COUNTRY_NAME" text, "REGION_ID" int) AS f WHERE "COUNTRY_NAME" = 'Japan';

-- should to work with more data
INSERT INTO xmldata VALUES('<ROWS>
<ROW id="10">
  <COUNTRY_ID>CZ</COUNTRY_ID>
  <COUNTRY_NAME>Czech Republic</COUNTRY_NAME>
  <REGION_ID>2</REGION_ID><PREMIER_NAME>Milos Zeman</PREMIER_NAME>
</ROW>
<ROW id="11">
  <COUNTRY_ID>DE</COUNTRY_ID>
  <COUNTRY_NAME>Germany</COUNTRY_NAME>
  <REGION_ID>2</REGION_ID>
</ROW>
<ROW id="12">
  <COUNTRY_ID>FR</COUNTRY_ID>
  <COUNTRY_NAME>France</COUNTRY_NAME>
  <REGION_ID>2</REGION_ID>
</ROW>
</ROWS>');

INSERT INTO xmldata VALUES('<ROWS>
<ROW id="20">
  <COUNTRY_ID>EG</COUNTRY_ID>
  <COUNTRY_NAME>Egypt</COUNTRY_NAME>
  <REGION_ID>1</REGION_ID>
</ROW>
<ROW id="21">
  <COUNTRY_ID>SD</COUNTRY_ID>
  <COUNTRY_NAME>Sudan</COUNTRY_NAME>
  <REGION_ID>1</REGION_ID>
</ROW>
</ROWS>');

SELECT  xmltable.*
   FROM (SELECT data FROM xmldata) x,
        LATERAL XMLTABLE('/ROWS/ROW'
                         PASSING data
                         COLUMNS id int PATH '@id',
                                  _id FOR ORDINALITY,
                                  country_name text PATH 'COUNTRY_NAME' NOT NULL,
                                  country_id text PATH 'COUNTRY_ID',
                                  region_id int PATH 'REGION_ID',
                                  size float PATH 'SIZE',
                                  unit text PATH 'SIZE/@unit',
                                  premier_name text PATH 'PREMIER_NAME' DEFAULT 'not specified');

SELECT  xmltable.*
   FROM (SELECT data FROM xmldata) x,
        LATERAL XMLTABLE('/ROWS/ROW'
                         PASSING data
                         COLUMNS id int PATH '@id',
                                  _id FOR ORDINALITY,
                                  country_name text PATH 'COUNTRY_NAME' NOT NULL,
                                  country_id text PATH 'COUNTRY_ID',
                                  region_id int PATH 'REGION_ID',
                                  size float PATH 'SIZE',
                                  unit text PATH 'SIZE/@unit',
                                  premier_name text PATH 'PREMIER_NAME' DEFAULT 'not specified')
  WHERE region_id = 2;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT  xmltable.*
   FROM (SELECT data FROM xmldata) x,
        LATERAL XMLTABLE('/ROWS/ROW'
                         PASSING data
                         COLUMNS id int PATH '@id',
                                  _id FOR ORDINALITY,
                                  country_name text PATH 'COUNTRY_NAME' NOT NULL,
                                  country_id text PATH 'COUNTRY_ID',
                                  region_id int PATH 'REGION_ID',
                                  size float PATH 'SIZE',
                                  unit text PATH 'SIZE/@unit',
                                  premier_name text PATH 'PREMIER_NAME' DEFAULT 'not specified')
  WHERE region_id = 2;

-- should fail, NULL value
SELECT  xmltable.*
   FROM (SELECT data FROM xmldata) x,
        LATERAL XMLTABLE('/ROWS/ROW'
                         PASSING data
                         COLUMNS id int PATH '@id',
                                  _id FOR ORDINALITY,
                                  country_name text PATH 'COUNTRY_NAME' NOT NULL,
                                  country_id text PATH 'COUNTRY_ID',
                                  region_id int PATH 'REGION_ID',
                                  size float PATH 'SIZE' NOT NULL,
                                  unit text PATH 'SIZE/@unit',
                                  premier_name text PATH 'PREMIER_NAME' DEFAULT 'not specified');

-- if all is ok, then result is empty
-- one line xml test
WITH
   x AS (SELECT proname, proowner, procost::numeric, pronargs,
                array_to_string(proargnames,',') as proargnames,
                case when proargtypes <> '' then array_to_string(proargtypes::oid[],',') end as proargtypes
           FROM pg_proc WHERE proname = 'f_leak'),
   y AS (SELECT xmlelement(name proc,
                           xmlforest(proname, proowner,
                                     procost, pronargs,
                                     proargnames, proargtypes)) as proc
           FROM x),
   z AS (SELECT xmltable.*
           FROM y,
                LATERAL xmltable('/proc' PASSING proc
                                 COLUMNS proname name,
                                         proowner oid,
                                         procost float,
                                         pronargs int,
                                         proargnames text,
                                         proargtypes text))
   SELECT * FROM z
   EXCEPT SELECT * FROM x;

-- multi line xml test, result should be empty too
WITH
   x AS (SELECT proname, proowner, procost::numeric, pronargs,
                array_to_string(proargnames,',') as proargnames,
                case when proargtypes <> '' then array_to_string(proargtypes::oid[],',') end as proargtypes
           FROM pg_proc),
   y AS (SELECT xmlelement(name data,
                           xmlagg(xmlelement(name proc,
                                             xmlforest(proname, proowner, procost,
                                                       pronargs, proargnames, proargtypes)))) as doc
           FROM x),
   z AS (SELECT xmltable.*
           FROM y,
                LATERAL xmltable('/data/proc' PASSING doc
                                 COLUMNS proname name,
                                         proowner oid,
                                         procost float,
                                         pronargs int,
                                         proargnames text,
                                         proargtypes text))
   SELECT * FROM z
   EXCEPT SELECT * FROM x;

CREATE TABLE xmltest2(x xml, _path text);

INSERT INTO xmltest2 VALUES('<d><r><ac>1</ac></r></d>', 'A');
INSERT INTO xmltest2 VALUES('<d><r><bc>2</bc></r></d>', 'B');
INSERT INTO xmltest2 VALUES('<d><r><cc>3</cc></r></d>', 'C');
INSERT INTO xmltest2 VALUES('<d><r><dc>2</dc></r></d>', 'D');

SELECT xmltable.* FROM xmltest2, LATERAL xmltable('/d/r' PASSING x COLUMNS a int PATH '' || lower(_path) || 'c');
SELECT xmltable.* FROM xmltest2, LATERAL xmltable(('/d/r/' || lower(_path) || 'c') PASSING x COLUMNS a int PATH '.');
SELECT xmltable.* FROM xmltest2, LATERAL xmltable(('/d/r/' || lower(_path) || 'c') PASSING x COLUMNS a int PATH 'x' DEFAULT ascii(_path) - 54);

-- XPath result can be boolean or number too
SELECT * FROM XMLTABLE('*' PASSING '<a>a</a>' COLUMNS a xml PATH '.', b text PATH '.', c text PATH '"hi"', d boolean PATH '. = "a"', e integer PATH 'string-length(.)');
\x
SELECT * FROM XMLTABLE('*' PASSING '<e>pre<!--c1--><?pi arg?><![CDATA[&ent1]]><n2>&amp;deep</n2>post</e>' COLUMNS x xml PATH '/e/n2', y xml PATH '/');
\x

SELECT * FROM XMLTABLE('.' PASSING XMLELEMENT(NAME a) columns a varchar(20) PATH '"<foo/>"', b xml PATH '"<foo/>"');

SELECT xmltext(NULL);
SELECT xmltext('');
SELECT xmltext('  ');
SELECT xmltext('foo `$_-+?=*^%!|/\()[]{}');
SELECT xmltext('foo & <"bar">');
SELECT xmltext('x'|| '<P>73</P>'::xml || .42 || true || 'j'::char);

-- for xmlcast() tests
INSERT INTO xmltest
 VALUES (42,
'<?xml version="1.0" encoding="utf-8"?>
 <xmlcast>
  <period1>P1Y2M3DT4H5M6S</period1>
  <period2>1 year 2 mons 3 days 4 hours 5 minutes 6 seconds</period2>
  <date1>2002-09-24</date1>
  <date2>2002-09-24+06:00</date2>
  <time>09:30:10.5</time>
  <time_tz1>09:30:10Z</time_tz1>
  <time_tz2>09:30:10-06:00</time_tz2>
  <time_tz3>09:30:10+06:00</time_tz3>
  <timestamp1>2002-05-30T09:00:00</timestamp1>
  <timestamp2>2002-05-30T09:30:10.5</timestamp2>
  <timestamp_tz1>2002-05-30T09:30:10Z</timestamp_tz1>
  <timestamp_tz2>2002-05-30T09:30:10-06:00</timestamp_tz2>
  <timestamp_tz3>2002-05-30T09:30:10+06:00</timestamp_tz3>
  <text1>foo bar</text1>
  <text2>       foo bar     </text2>
  <text3>foo &amp; &lt;&quot;bar&quot;&gt;</text3>
  <decimal1>42.7312345678910</decimal1>
  <decimal2>+42.7312345678910</decimal2>
  <decimal3>-42.7312345678910</decimal3>
  <decimal4>INF</decimal4>
  <decimal5>-INF</decimal5>
  <decimal6>NaN</decimal6>
  <integer1>42</integer1>
  <integer2>+42</integer2>
  <integer3>-42</integer3>
  <long1>4273535420162021</long1>
  <long2>+4273535420162021</long2>
  <long3>-4273535420162021</long3>
  <bool1 att="true">42</bool1>
  <bool2 att="false">73</bool2>
  <empty></empty>
 </xmlcast>'::xml
);

-- This prevents the xmlcast regression tests from failing if the system's timezone has been changed.
SET timezone TO 'America/Los_Angeles';

-- xmlcast exceptions
\set VERBOSITY terse
SELECT xmlcast((xpath('//text1/text()', data))[1] AS text[]) FROM xmltest WHERE id = 42;
SELECT xmlcast((xpath('//text1/integer1()', data))[1] AS int[]) FROM xmltest WHERE id = 42;
SELECT xmlcast(NULL AS text);
SELECT xmlcast('foo'::text AS varchar);
SELECT xmlcast(42 AS text);
SELECT xmlcast(array['foo','bar'] AS xml);
SELECT xmlcast('not-a-number'::xml AS integer);
SELECT xmlcast('not-a-date'::xml AS date);
\set VERBOSITY default

-- xmlcast tests for "XML to non-XML" expressions
SELECT
  xmlcast((xpath('//date1/text()', data))[1] AS date), pg_typeof(xmlcast((xpath('//date1/text()', data))[1] AS date)),
  xmlcast((xpath('//date2/text()', data))[1] AS date), pg_typeof(xmlcast((xpath('//date2/text()', data))[1] AS date))
FROM xmltest WHERE id = 42;

SELECT
  xmlcast((xpath('//period1/text()', data))[1] AS interval), pg_typeof(xmlcast((xpath('//period1/text()', data))[1] AS interval)),
  xmlcast((xpath('//period2/text()', data))[1] AS interval), pg_typeof(xmlcast((xpath('//period2/text()', data))[1] AS interval))
FROM xmltest WHERE id = 42;

SELECT
  xmlcast((xpath('//time/text()', data))[1] AS time), pg_typeof(xmlcast((xpath('//time/text()', data))[1] AS time)),
  xmlcast((xpath('//time_tz1/text()', data))[1] AS time with time zone), pg_typeof(xmlcast((xpath('//time_tz1/text()', data))[1] AS time with time zone)),
  xmlcast((xpath('//time_tz2/text()', data))[1] AS time with time zone), pg_typeof(xmlcast((xpath('//time_tz2/text()', data))[1] AS time with time zone)),
  xmlcast((xpath('//time_tz3/text()', data))[1] AS time with time zone), pg_typeof(xmlcast((xpath('//time_tz3/text()', data))[1] AS time with time zone))
FROM xmltest WHERE id = 42;

SELECT
  xmlcast((xpath('//text1/text()', data))[1] AS text), pg_typeof(xmlcast((xpath('//text1/text()', data))[1] AS text)),
  xmlcast((xpath('//text2/text()', data))[1] AS text), pg_typeof(xmlcast((xpath('//text2/text()', data))[1] AS text)),
  xmlcast((xpath('//text3/text()', data))[1] AS text), pg_typeof(xmlcast((xpath('//text3/text()', data))[1] AS text))
FROM xmltest WHERE id = 42;

SELECT
  xmlcast((xpath('//text1/text()', data))[1] AS varchar), pg_typeof(xmlcast((xpath('//text1/text()', data))[1] AS varchar)),
  xmlcast((xpath('//text2/text()', data))[1] AS varchar), pg_typeof(xmlcast((xpath('//text2/text()', data))[1] AS varchar)),
  xmlcast((xpath('//text3/text()', data))[1] AS varchar), pg_typeof(xmlcast((xpath('//text3/text()', data))[1] AS varchar))
FROM xmltest WHERE id = 42;

SELECT
  xmlcast((xpath('//text1/text()', data))[1] AS name), pg_typeof(xmlcast((xpath('//text1/text()', data))[1] AS name)),
  xmlcast((xpath('//text2/text()', data))[1] AS name), pg_typeof(xmlcast((xpath('//text2/text()', data))[1] AS name)),
  xmlcast((xpath('//text3/text()', data))[1] AS name), pg_typeof(xmlcast((xpath('//text3/text()', data))[1] AS name))
FROM xmltest WHERE id = 42;

SELECT
  xmlcast((xpath('//text1/text()', data))[1] AS bpchar), pg_typeof(xmlcast((xpath('//text1/text()', data))[1] AS bpchar)),
  xmlcast((xpath('//text2/text()', data))[1] AS bpchar), pg_typeof(xmlcast((xpath('//text2/text()', data))[1] AS bpchar)),
  xmlcast((xpath('//text3/text()', data))[1] AS bpchar), pg_typeof(xmlcast((xpath('//text3/text()', data))[1] AS bpchar))
FROM xmltest WHERE id = 42;

SELECT
  xmlcast((xpath('//decimal1/text()', data))[1] AS numeric), pg_typeof(xmlcast((xpath('//decimal1/text()', data))[1] AS numeric)),
  xmlcast((xpath('//decimal2/text()', data))[1] AS numeric), pg_typeof(xmlcast((xpath('//decimal2/text()', data))[1] AS numeric)),
  xmlcast((xpath('//decimal3/text()', data))[1] AS numeric), pg_typeof(xmlcast((xpath('//decimal3/text()', data))[1] AS numeric)),
  xmlcast((xpath('//decimal4/text()', data))[1] AS numeric), pg_typeof(xmlcast((xpath('//decimal4/text()', data))[1] AS numeric)),
  xmlcast((xpath('//decimal5/text()', data))[1] AS numeric), pg_typeof(xmlcast((xpath('//decimal5/text()', data))[1] AS numeric)),
  xmlcast((xpath('//decimal6/text()', data))[1] AS numeric), pg_typeof(xmlcast((xpath('//decimal6/text()', data))[1] AS numeric))
FROM xmltest WHERE id = 42;

SELECT
  xmlcast((xpath('//decimal1/text()', data))[1] AS double precision), pg_typeof(xmlcast((xpath('//decimal1/text()', data))[1] AS double precision)),
  xmlcast((xpath('//decimal2/text()', data))[1] AS double precision), pg_typeof(xmlcast((xpath('//decimal2/text()', data))[1] AS double precision)),
  xmlcast((xpath('//decimal3/text()', data))[1] AS double precision), pg_typeof(xmlcast((xpath('//decimal3/text()', data))[1] AS double precision)),
  xmlcast((xpath('//decimal4/text()', data))[1] AS double precision), pg_typeof(xmlcast((xpath('//decimal4/text()', data))[1] AS double precision)),
  xmlcast((xpath('//decimal5/text()', data))[1] AS double precision), pg_typeof(xmlcast((xpath('//decimal5/text()', data))[1] AS double precision)),
  xmlcast((xpath('//decimal6/text()', data))[1] AS double precision), pg_typeof(xmlcast((xpath('//decimal6/text()', data))[1] AS double precision))
FROM xmltest WHERE id = 42;

SELECT
  xmlcast((xpath('//integer1/text()', data))[1] AS int), pg_typeof(xmlcast((xpath('//integer1/text()', data))[1] AS int)),
  xmlcast((xpath('//integer2/text()', data))[1] AS int), pg_typeof(xmlcast((xpath('//integer2/text()', data))[1] AS int)),
  xmlcast((xpath('//integer3/text()', data))[1] AS int), pg_typeof(xmlcast((xpath('//integer3/text()', data))[1] AS int))
FROM xmltest WHERE id = 42;

SELECT
  xmlcast((xpath('//long1/text()', data))[1] AS bigint), pg_typeof(xmlcast((xpath('//long1/text()', data))[1] AS bigint)),
  xmlcast((xpath('//long2/text()', data))[1] AS bigint), pg_typeof(xmlcast((xpath('//long2/text()', data))[1] AS bigint)),
  xmlcast((xpath('//long3/text()', data))[1] AS bigint), pg_typeof(xmlcast((xpath('//long3/text()', data))[1] AS bigint))
FROM xmltest WHERE id = 42;

SELECT
  xmlcast((xpath('//bool1/@att', data))[1] AS boolean), pg_typeof(xmlcast((xpath('//bool1/@att', data))[1] AS boolean)),
  xmlcast((xpath('//bool2/@att', data))[1] AS boolean), pg_typeof(xmlcast((xpath('//bool1/@att', data))[1] AS boolean))
FROM xmltest WHERE id = 42;

SELECT xmlcast((xpath('//empty/text()', data))[1] AS text), pg_typeof(xmlcast((xpath('//empty/text()', data))[1] AS text))
FROM xmltest WHERE id = 42;

-- xmlcast tests for "XML to XML" expressions
SELECT
  xmlcast((xpath('//text1/text()', data))[1] AS xml), pg_typeof(xmlcast((xpath('//text1/text()', data))[1] AS xml)),
  xmlcast((xpath('//text2/text()', data))[1] AS xml), pg_typeof(xmlcast((xpath('//text2/text()', data))[1] AS xml)),
  xmlcast((xpath('//text3/text()', data))[1] AS xml), pg_typeof(xmlcast((xpath('//text3/text()', data))[1] AS xml))
FROM xmltest WHERE id = 42;

SELECT
  xmlcast((xpath('//timestamp1/text()', data))[1] AS timestamp), pg_typeof(xmlcast((xpath('//timestamp1/text()', data))[1] AS timestamp)),
  xmlcast((xpath('//timestamp2/text()', data))[1] AS timestamp), pg_typeof(xmlcast((xpath('//timestamp2/text()', data))[1] AS timestamp))
FROM xmltest WHERE id = 42;

SELECT
  xmlcast((xpath('//timestamp_tz1/text()', data))[1] AS timestamp with time zone), pg_typeof(xmlcast((xpath('//timestamp_tz1/text()', data))[1] AS timestamp with time zone)),
  xmlcast((xpath('//timestamp_tz2/text()', data))[1] AS timestamp with time zone), pg_typeof(xmlcast((xpath('//timestamp_tz2/text()', data))[1] AS timestamp with time zone)),
  xmlcast((xpath('//timestamp_tz3/text()', data))[1] AS timestamp with time zone), pg_typeof(xmlcast((xpath('//timestamp_tz3/text()', data))[1] AS timestamp with time zone))
FROM xmltest WHERE id = 42;

-- xmlcast tests for "non-XML to XML" expressions
SELECT j, pg_typeof(j) FROM xmlcast(NULL AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('foo' AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(''::text AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(NULL::text AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(''::xml AS text) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(NULL::xml AS text) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('foo & <"bar">'::text AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('foo & <"bar">'::varchar AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('foo & <"bar">'::name AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(xmltext(E'foo & <"bar">\r') AS text) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(xmlcast(E'foo & <"bar">\r' AS xml) AS text) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(to_date('29/05/2024','dd/mm/yyyy') AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('2024-05-29 12:04:10.703585+02'::timestamp with time zone at time zone 'Europe/Berlin' AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('2024-05-29 12:04:10.703585+02'::timestamp without time zone AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('1 year 2 months 3 days 4 hours 5 minutes 6 seconds'::interval AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(42::smallint AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(427353542 AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(4273535420162021 AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(42.007312345678910 AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(42.007312345678910::double precision AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(42.0::real AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('infinity'::real AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('-infinity'::real AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('nan'::real AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('infinity'::double precision AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('-infinity'::double precision AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('nan'::double precision AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('infinity'::numeric AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('-infinity'::numeric AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('nan'::numeric AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(true AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(false AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(42 = 73 AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast(42 <> 73 AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('11:11:11.5'::time AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('11:11:11.5+01'::time with time zone AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('infinity'::interval AS xml) t(j);
SELECT j, pg_typeof(j) FROM xmlcast('-infinity'::interval AS xml) t(j);

-- Convert an XML string to bytea and back to xml
SELECT xmlcast(convert_from(xmlcast('&lt;&quot;foo&amp;bar&quot;&gt;'::xml AS bytea),'UTF8')::xml AS text);

SET xmlbinary TO hex;
SELECT xmlcast(E'\\xDEADBEEF'::bytea AS xml);
SET xmlbinary TO base64;
SELECT xmlcast(E'\\xDEADBEEF'::bytea AS xml);

-- The BY REF and BY VALUE clauses are accepted but ignored.
-- This checks if the results are indeed the same as without the clauses.
SELECT
  xmlcast('foo' AS xml)::text = xmlcast('foo' AS xml BY REF)::text,
  xmlcast('foo' AS xml)::text = xmlcast('foo' AS xml BY VALUE)::text,
  xmlcast('foo'::xml AS text) = xmlcast('foo'::xml AS text BY REF),
  xmlcast('foo'::xml AS text) = xmlcast('foo'::xml AS text BY VALUE);

SELECT
  xmlcast(42 AS xml)::text = xmlcast(42 AS xml BY REF)::text,
  xmlcast(42 AS xml)::text = xmlcast(42 AS xml BY VALUE)::text,
  xmlcast('42'::xml AS int) = xmlcast('42'::xml AS int BY REF),
  xmlcast('42'::xml AS int) = xmlcast('42'::xml AS int BY VALUE);

SELECT
  xmlcast(42.73 AS xml)::text = xmlcast(42.73 AS xml BY REF)::text,
  xmlcast(42.73 AS xml)::text = xmlcast(42.73 AS xml BY VALUE)::text,
  xmlcast('42.73'::xml AS numeric) = xmlcast('42.73'::xml AS numeric BY REF),
  xmlcast('42.73'::xml AS numeric) = xmlcast('42.73'::xml AS numeric BY VALUE);

SELECT
  xmlcast('2024-08-14'::date AS xml)::text = xmlcast('2024-08-14'::date AS xml BY REF)::text,
  xmlcast('2024-08-14'::date AS xml)::text = xmlcast('2024-08-14'::date AS xml BY VALUE)::text,
  xmlcast('2024-08-14'::xml AS date) = xmlcast('2024-08-14'::xml AS date BY REF),
  xmlcast('2024-08-14'::xml AS date) = xmlcast('2024-08-14'::xml AS date BY VALUE);

SELECT
  xmlcast('12:30:45'::time without time zone AS xml)::text = xmlcast('12:30:45'::time without time zone AS xml BY REF)::text,
  xmlcast('12:30:45'::time without time zone AS xml)::text = xmlcast('12:30:45'::time without time zone AS xml BY VALUE)::text,
  xmlcast('12:30:45'::xml AS time without time zone) = xmlcast('12:30:45'::xml AS time without time zone BY REF),
  xmlcast('12:30:45'::xml AS time without time zone) = xmlcast('12:30:45'::xml AS time without time zone BY VALUE);

SELECT
  xmlcast('09:30:10+06:00'::time with time zone AS xml)::text = xmlcast('09:30:10+06:00'::time with time zone AS xml BY REF)::text,
  xmlcast('09:30:10+06:00'::time with time zone AS xml)::text = xmlcast('09:30:10+06:00'::time with time zone AS xml BY VALUE)::text,
  xmlcast('09:30:10+06:00'::xml AS time with time zone) = xmlcast('09:30:10+06:00'::xml AS time with time zone BY REF),
  xmlcast('09:30:10+06:00'::xml AS time with time zone) = xmlcast('09:30:10+06:00'::xml AS time with time zone BY VALUE);

SELECT
  xmlcast('2002-05-30 09:30:10+06:00'::timestamp with time zone AS xml)::text = xmlcast('2002-05-30 09:30:10+06:00'::timestamp with time zone AS xml BY REF)::text,
  xmlcast('2002-05-30 09:30:10+06:00'::timestamp with time zone AS xml)::text = xmlcast('2002-05-30 09:30:10+06:00'::timestamp with time zone AS xml BY VALUE)::text,
  xmlcast('2002-05-30T09:30:10+06:00'::xml AS timestamp with time zone) = xmlcast('2002-05-30T09:30:10+06:00'::xml AS timestamp with time zone BY REF),
  xmlcast('2002-05-30T09:30:10+06:00'::xml AS timestamp with time zone) = xmlcast('2002-05-30T09:30:10+06:00'::xml AS timestamp with time zone BY VALUE);

SELECT
  xmlcast('1 year 2 mons'::interval AS xml)::text = xmlcast('1 year 2 mons'::interval AS xml BY REF)::text,
  xmlcast('1 year 2 mons'::interval AS xml)::text = xmlcast('1 year 2 mons'::interval AS xml BY VALUE)::text,
  xmlcast('1 year 2 mons'::xml AS interval) = xmlcast('1 year 2 mons'::xml AS interval BY REF),
  xmlcast('1 year 2 mons'::xml AS interval) = xmlcast('1 year 2 mons'::xml AS interval BY VALUE);

-- tests for xmlcast() with explicit length modifiers
SELECT xmlcast('hello world'::xml AS varchar(5));
SELECT xmlcast('42.7312'::xml AS numeric(5,2));

CREATE VIEW view_xmlcast_to_xml AS
SELECT
  xmlcast(NULL AS xml) AS c1,
  xmlcast('foo' AS xml) AS c2,
  xmlcast(''::text AS xml) AS c3,
  xmlcast(NULL::text AS xml) AS c4,
  xmlcast(''::xml AS text) AS c5,
  xmlcast(NULL::xml AS text) c6,
  xmlcast('foo & <"bar">'::text AS xml) AS c7,
  xmlcast('foo & <"bar">'::varchar AS xml) AS c8,
  xmlcast('foo & <"bar">'::name AS xml) AS c9,
  xmlcast(xmltext(E'foo & <"bar">\r') AS text) AS c10,
  xmlcast(xmlcast(E'foo & <"bar">\r' AS xml) AS text) AS c11,
  xmlcast(to_date('29/05/2024','dd/mm/yyyy') AS xml) AS c12,
  xmlcast('2024-05-29 12:04:10.703585+02'::timestamp with time zone at time zone 'Europe/Berlin' AS xml) AS c13,
  xmlcast('2024-05-29 12:04:10.703585+02'::timestamp without time zone AS xml) AS c14,
  xmlcast('1 year 2 months 3 days 4 hours 5 minutes 6 seconds'::interval AS xml) AS c15,
  xmlcast(427353542 AS xml) AS c16,
  xmlcast(4273535420162021 AS xml) AS c17,
  xmlcast(42.007312345678910 AS xml) AS c18,
  xmlcast(42.007312345678910::double precision AS xml) AS c19,
  xmlcast(true AS xml) AS c20,
  xmlcast(false AS xml) AS c21,
  xmlcast(42 = 73 AS xml) AS c22,
  xmlcast(42 <> 73 AS xml) AS c23,
  xmlcast('11:11:11.5'::time AS xml) AS c24,
  xmlcast('11:11:11.5+01'::time with time zone AS xml) AS c25;

\sv view_xmlcast_to_xml
SELECT * FROM view_xmlcast_to_xml;

CREATE VIEW view_xmlcast_from_xml AS
SELECT
  xmlcast('P1Y2M3DT4H5M6S'::xml AS interval) AS c1,
  xmlcast('1 year 2 mons 3 days 4 hours 5 minutes 6 seconds'::xml AS interval) AS c2,
  xmlcast('2002-09-24'::xml AS date) AS c3,
  xmlcast('2002-09-24+06:00'::xml AS date) AS c4,
  xmlcast('09:30:10Z'::xml AS time with time zone) AS c5,
  xmlcast('09:30:10-06:00'::xml AS time with time zone) AS c6,
  xmlcast('09:30:10+06:00'::xml AS time with time zone) AS c7,
  xmlcast('2002-05-30T09:30:10Z'::xml AS timestamp with time zone) at time zone 'Europe/Berlin' AS c8,
  xmlcast('2002-05-30T09:30:10-06:00'::xml AS timestamp with time zone) at time zone 'Europe/Berlin' AS c9,
  xmlcast('2002-05-30T09:30:10+06:00'::xml AS timestamp with time zone) at time zone 'Europe/Berlin' AS c10,
  xmlcast('foo bar'::xml AS text) AS c11,
  xmlcast('       foo bar     '::xml AS varchar) AS c12,
  xmlcast('foo &amp; &lt;&quot;bar&quot;&gt;'::xml AS text) AS c13,
  xmlcast('42.7312345678910'::xml AS numeric) AS c14,
  xmlcast('+42.7312345678910'::xml AS numeric) AS c15,
  xmlcast('-42.7312345678910'::xml AS numeric) AS c16,
  xmlcast('42'::xml AS integer) AS c17,
  xmlcast('+42'::xml AS integer) AS c18,
  xmlcast('-42'::xml AS integer) AS c19,
  xmlcast('4273535420162021'::xml AS bigint) AS c20,
  xmlcast('+4273535420162021'::xml AS bigint) AS c21,
  xmlcast('-4273535420162021'::xml AS bigint) AS c22,
  xmlcast('true'::xml AS boolean) AS c23,
  xmlcast('false'::xml AS boolean) AS c24,
  xmlcast(''::xml AS character varying) AS c25,
  xmlcast(NULL::xml AS character varying) AS c26,
  xmlcast('hello world'::xml AS varchar(5)) AS c27,
  xmlcast('42.7312'::xml AS numeric(5,2)) AS c28;

\sv view_xmlcast_from_xml
SELECT * FROM view_xmlcast_from_xml;