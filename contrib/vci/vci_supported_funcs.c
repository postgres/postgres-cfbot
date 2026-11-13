/*-------------------------------------------------------------------------
 *
 * vci_supported_func.c
 *	  Function that VCI supports that can be called with FuncExpr
 *
 * vci_supported_func_table[] is created with the following SQL and then examined individually.
 *
 *   SELECT oid, proname FROM pg_proc WHERE prokind = 'f' AND NOT proretset
 *       AND NOT EXISTS (SELECT funcoid FROM sys_func_table WHERE pg_proc.oid = sys_func_table.funcoid)
 *       AND (SELECT bool_and(i IN (SELECT typeoid FROM safe_types)) FROM unnest(array_prepend(prorettype, proargtypes)) AS t(i))
 *       AND oid < 16384 ORDER BY oid;
 *
 * - prokind = 'f' is to include only normal functions (e.g. exclude aggregate functions and window functions).
 * - NOT proretset is to exclude SRF
 * - NOT EXISTS (SELECT ...) is to exclude system related functions
 * - (SELECT bool_and( ...) is to exclude the appearance of unauthorized types in return values and arguments.
 * - oid < 16384 is to exclude user-defined types
 *
 * sys_func_table and safe_types are in reference to vci_supported_funcs.sql.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/vci_supported_funcs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/transam.h"
#include "c.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"	/* for ProcedureRelationId, Form_pg_proc */
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#include "vci.h"
#include "vci_mem.h"
#include "vci_supported_oid.h"

/**
 * Smallest OID among functions supported by VCI
 */
#define VCI_SUPPORTED_FUNC_MIN (77)

/**
 * Biggest OID among functions supported by VCI
 */
#define VCI_SUPPORTED_FUNC_MAX (6204)

/**
 * Data to record special user defined functions
 */
vci_special_udf_info_t vci_special_udf_info;

/**
 * Array of information about functions supported by VCI
 * Note when modifying vci_supported_func_table array:
 *  1.OIDs are in ascending order.
 *  2.Don't forget to change the macro value of VCI_SUPPORTED_FUNC_MIN/VCI_SUPPORTED_FUNC_MAX.
 */
static const struct
{
	Oid			oid;
	const char *name;
	bool		is_support;
}			vci_supported_func_table[] = {
	{77, "int4", true},			/* immutable, internal(12) {char,int4} */
	{78, "char", true},			/* immutable, internal(12) {char,int4} */
	{89, "version", false},		/* stable,    internal(12) {text} */
	{228, "dround", true},		/* immutable, internal(12) {float8,float8} */
	{229, "dtrunc", true},		/* immutable, internal(12) {float8,float8} */
	{233, "dexp", true},		/* immutable, internal(12) {float8,float8} */
	{234, "dlog1", true},		/* immutable, internal(12) {float8,float8} */
	{235, "float8", true},		/* immutable, internal(12) {int2,float8} */
	{236, "float4", true},		/* immutable, internal(12) {int2,float4} */
	{237, "int2", true},		/* immutable, internal(12) {int2,float8} */
	{238, "int2", true},		/* immutable, internal(12) {int2,float4} */
	{274, "timeofday", true},	/* volatile,  internal(12) {text} */
	{311, "float8", true},		/* immutable, internal(12) {float4,float8} */
	{312, "float4", true},		/* immutable, internal(12) {float4,float8} */
	{313, "int4", true},		/* immutable, internal(12) {int2,int4} */
	{314, "int2", true},		/* immutable, internal(12) {int2,int4} */
	{316, "float8", true},		/* immutable, internal(12) {int4,float8} */
	{317, "int4", true},		/* immutable, internal(12) {int4,float8} */
	{318, "float4", true},		/* immutable, internal(12) {int4,float4} */
	{319, "int4", true},		/* immutable, internal(12) {int4,float4} */
	{320, "width_bucket", true},	/* immutable, internal(12)
									 * {int4,int4,float8,float8,float8} */
	{376, "string_to_array", false},	/* immutable, internal(12)
										 * {text,text,text,_text} */
	{384, "array_to_string", false},	/* stable,    internal(12)
										 * {text,text,text,anyarray} */
	{394, "string_to_array", false},	/* immutable, internal(12)
										 * {text,text,_text} */
	{395, "array_to_string", false},	/* stable,    internal(12)
										 * {text,text,anyarray} */
	{401, "text", true},		/* immutable, internal(12) {text,bpchar} */
	{406, "text", true},		/* immutable, internal(12) {name,text} */
	{407, "name", true},		/* immutable, internal(12) {name,text} */
	{408, "bpchar", true},		/* immutable, internal(12) {name,bpchar} */
	{409, "name", true},		/* immutable, internal(12) {name,bpchar} */
	{480, "int4", true},		/* immutable, internal(12) {int8,int4} */
	{481, "int8", true},		/* immutable, internal(12) {int8,int4} */
	{482, "float8", true},		/* immutable, internal(12) {int8,float8} */
	{483, "int8", true},		/* immutable, internal(12) {int8,float8} */
	{652, "float4", true},		/* immutable, internal(12) {int8,float4} */
	{653, "int8", true},		/* immutable, internal(12) {int8,float4} */
	{668, "bpchar", true},		/* immutable, internal(12)
								 * {bool,int4,bpchar,bpchar} */
	{710, "getpgusername", false},	/* stable,    internal(12) {name} */
	{714, "int2", true},		/* immutable, internal(12) {int8,int2} */
	{720, "octet_length", true},	/* immutable, internal(12) {bytea,int4} */
	{721, "get_byte", true},	/* immutable, internal(12) {bytea,int4,int4} */
	{722, "set_byte", true},	/* immutable, internal(12)
								 * {bytea,bytea,int4,int4} */
	{723, "get_bit", true},		/* immutable, internal(12) {bytea,int8,int4} */
	{724, "set_bit", true},		/* immutable, internal(12)
								 * {bytea,bytea,int8,int4} */
	{745, "current_user", false},	/* stable,    internal(12) {name} */
	{746, "session_user", false},	/* stable,    internal(12) {name} */
	{747, "array_dims", false}, /* immutable, internal(12) {text,anyarray} */
	{748, "array_ndims", false},	/* immutable, internal(12) {int4,anyarray} */
	{749, "overlay", true},		/* immutable, internal(12)
								 * {bytea,bytea,bytea,int4,int4} */
	{752, "overlay", true},		/* immutable, internal(12)
								 * {bytea,bytea,bytea,int4} */
	{754, "int8", true},		/* immutable, internal(12) {int8,int2} */
	{766, "int4inc", true},		/* immutable, internal(12) {int4,int4} */
	{810, "pg_client_encoding", true},	/* stable,    internal(12) {name} */
	{817, "current_query", false},	/* volatile,  internal(12) {text} */
	{849, "position", true},	/* immutable, internal(12) {int4,text,text} */
	{860, "bpchar", true},		/* immutable, internal(12) {char,bpchar} */
	{861, "current_database", false},	/* stable,    internal(12) {name} */
	{868, "strpos", true},		/* immutable, internal(12) {int4,text,text} */
	{870, "lower", true},		/* immutable, internal(12) {text,text} */
	{871, "upper", true},		/* immutable, internal(12) {text,text} */
	{872, "initcap", true},		/* immutable, internal(12) {text,text} */
	{873, "lpad", true},		/* immutable, internal(12)
								 * {int4,text,text,text} */
	{874, "rpad", true},		/* immutable, internal(12)
								 * {int4,text,text,text} */
	{875, "ltrim", true},		/* immutable, internal(12) {text,text,text} */
	{876, "rtrim", true},		/* immutable, internal(12) {text,text,text} */
	{877, "substr", true},		/* immutable, internal(12)
								 * {int4,int4,text,text} */
	{878, "translate", true},	/* immutable, internal(12)
								 * {text,text,text,text} */
	{879, "lpad", true},		/* immutable, sql(14)      {int4,text,text} */
	{880, "rpad", true},		/* immutable, sql(14)      {int4,text,text} */
	{881, "ltrim", true},		/* immutable, internal(12) {text,text} */
	{882, "rtrim", true},		/* immutable, internal(12) {text,text} */
	{883, "substr", true},		/* immutable, internal(12) {int4,text,text} */
	{884, "btrim", true},		/* immutable, internal(12) {text,text,text} */
	{885, "btrim", true},		/* immutable, internal(12) {text,text} */
	{935, "cash_words", true},	/* immutable, internal(12) {text,money} */
	{936, "substring", true},	/* immutable, internal(12)
								 * {int4,int4,text,text} */
	{937, "substring", true},	/* immutable, internal(12) {int4,text,text} */
	{940, "mod", true},			/* immutable, internal(12) {int2,int2,int2} */
	{941, "mod", true},			/* immutable, internal(12) {int4,int4,int4} */
	{944, "char", true},		/* immutable, internal(12) {char,text} */
	{946, "text", true},		/* immutable, internal(12) {char,text} */
	{947, "mod", true},			/* immutable, internal(12) {int8,int8,int8} */
	{1026, "timezone", true},	/* immutable, internal(12)
								 * {timestamp,timestamptz,interval} */
	{1039, "getdatabaseencoding", false},	/* stable,    internal(12) {name} */
	{1158, "to_timestamp", true},	/* immutable, sql(14) {float8,timestamptz} */
	{1159, "timezone", true},	/* immutable, internal(12)
								 * {text,timestamp,timestamptz} */
	{1171, "date_part", true},	/* stable,    internal(12)
								 * {text,float8,timestamptz} */
	{1172, "date_part", true},	/* immutable, internal(12)
								 * {text,float8,interval} */
	{1174, "timestamptz", true},	/* stable,    internal(12)
									 * {date,timestamptz} */
	{1175, "justify_hours", true},	/* immutable, internal(12)
									 * {interval,interval} */
	{1176, "timestamptz", true},	/* stable,    sql(14)
									 * {date,time,timestamptz} */
	{1178, "date", true},		/* stable,    internal(12) {date,timestamptz} */
	{1193, "array_fill", false},	/* immutable, internal(12)
									 * {_int4,anyarray,anyelement} */
	{1199, "age", true},		/* immutable, internal(12)
								 * {timestamptz,timestamptz,interval} */
	{1200, "interval", true},	/* immutable, internal(12)
								 * {int4,interval,interval} */
	{1217, "date_trunc", true}, /* stable,    internal(12)
								 * {text,timestamptz,timestamptz} */
	{1218, "date_trunc", true}, /* immutable, internal(12)
								 * {text,interval,interval} */
	{1257, "textlen", true},	/* immutable, internal(12) {int4,text} */
	{1264, "pg_char_to_encoding", true},	/* stable,    internal(12)
											 * {name,int4} */
	{1269, "pg_column_size", false},	/* stable,    internal(12) {int4,any} */
	{1271, "overlaps", true},	/* immutable, internal(12)
								 * {bool,timetz,timetz,timetz,timetz} */
	{1273, "date_part", true},	/* immutable, internal(12)
								 * {text,float8,timetz} */
	{1282, "quote_ident", true},	/* immutable, internal(12) {text,text} */
	{1283, "quote_literal", true},	/* immutable, internal(12) {text,text} */
	{1285, "quote_literal", true},	/* stable,    sql(14) {text,anyelement} */
	{1286, "array_fill", false},	/* immutable, internal(12)
									 * {_int4,_int4,anyarray,anyelement} */
	{1289, "quote_nullable", true}, /* immutable, internal(12) {text,text} */
	{1290, "quote_nullable", true}, /* stable,    sql(14) {text,anyelement} */
	{1295, "justify_days", true},	/* immutable, internal(12)
									 * {interval,interval} */
	{1299, "now", true},		/* stable,    internal(12) {timestamptz} */
	{1304, "overlaps", true},	/* immutable, internal(12)
								 * {bool,timestamptz,timestamptz,timestamptz,timestamptz} */
	{1305, "overlaps", true},	/* stable,    sql(14)
								 * {bool,timestamptz,timestamptz,interval,interval} */
	{1306, "overlaps", true},	/* stable,    sql(14)
								 * {bool,timestamptz,timestamptz,timestamptz,interval} */
	{1307, "overlaps", true},	/* stable,    sql(14)
								 * {bool,timestamptz,timestamptz,timestamptz,interval} */
	{1308, "overlaps", true},	/* immutable, internal(12)
								 * {bool,time,time,time,time} */
	{1309, "overlaps", true},	/* immutable, sql(14)
								 * {bool,time,time,interval,interval} */
	{1310, "overlaps", true},	/* immutable, sql(14)
								 * {bool,time,time,time,interval} */
	{1311, "overlaps", true},	/* immutable, sql(14)
								 * {bool,time,time,time,interval} */
	{1316, "time", true},		/* immutable, internal(12) {time,timestamp} */
	{1317, "length", true},		/* immutable, internal(12) {int4,text} */
	{1318, "length", true},		/* immutable, internal(12) {int4,bpchar} */
	{1339, "dlog10", true},		/* immutable, internal(12) {float8,float8} */
	{1340, "log", true},		/* immutable, internal(12) {float8,float8} */
	{1341, "ln", true},			/* immutable, internal(12) {float8,float8} */
	{1342, "round", true},		/* immutable, internal(12) {float8,float8} */
	{1343, "trunc", true},		/* immutable, internal(12) {float8,float8} */
	{1344, "sqrt", true},		/* immutable, internal(12) {float8,float8} */
	{1345, "cbrt", true},		/* immutable, internal(12) {float8,float8} */
	{1346, "pow", true},		/* immutable, internal(12)
								 * {float8,float8,float8} */
	{1347, "exp", true},		/* immutable, internal(12) {float8,float8} */
	{1359, "timestamptz", true},	/* immutable, internal(12)
									 * {date,timestamptz,timetz} */
	{1367, "character_length", true},	/* immutable, internal(12)
										 * {int4,bpchar} */
	{1368, "power", true},		/* immutable, internal(12)
								 * {float8,float8,float8} */
	{1369, "character_length", true},	/* immutable, internal(12) {int4,text} */
	{1370, "interval", true},	/* immutable, internal(12) {time,interval} */
	{1372, "char_length", true},	/* immutable, internal(12) {int4,bpchar} */
	{1373, "isfinite", true},	/* immutable, internal(12) {bool,date} */
	{1374, "octet_length", true},	/* immutable, internal(12) {int4,text} */
	{1375, "octet_length", true},	/* immutable, internal(12) {int4,bpchar} */
	{1376, "factorial", true},	/* immutable, internal(12) {int8,numeric} */
	{1381, "char_length", true},	/* immutable, internal(12) {int4,text} */
	{1384, "date_part", true},	/* immutable, sql(14)      {text,float8,date} */
	{1385, "date_part", true},	/* immutable, internal(12) {text,float8,time} */
	{1386, "age", true},		/* stable,    sql(14) {timestamptz,interval} */
	{1388, "timetz", true},		/* stable,    internal(12)
								 * {timestamptz,timetz} */
	{1389, "isfinite", true},	/* immutable, internal(12) {bool,timestamptz} */
	{1390, "isfinite", true},	/* immutable, internal(12) {bool,interval} */
	{1394, "abs", true},		/* immutable, internal(12) {float4,float4} */
	{1395, "abs", true},		/* immutable, internal(12) {float8,float8} */
	{1396, "abs", true},		/* immutable, internal(12) {int8,int8} */
	{1397, "abs", true},		/* immutable, internal(12) {int4,int4} */
	{1398, "abs", true},		/* immutable, internal(12) {int2,int2} */
	{1402, "current_schema", false},	/* stable,    internal(12) {name} */
	{1403, "current_schemas", false},	/* stable,    internal(12)
										 * {bool,_name} */
	{1404, "overlay", true},	/* immutable, internal(12)
								 * {int4,int4,text,text,text} */
	{1405, "overlay", true},	/* immutable, internal(12)
								 * {int4,text,text,text} */
	{1419, "time", true},		/* immutable, internal(12) {time,interval} */
	{1569, "like", true},		/* immutable, internal(12) {bool,text,text} */
	{1570, "notlike", true},	/* immutable, internal(12) {bool,text,text} */
	{1571, "like", true},		/* immutable, internal(12) {bool,name,text} */
	{1572, "notlike", true},	/* immutable, internal(12) {bool,name,text} */
	{1597, "pg_encoding_to_char", true},	/* stable,    internal(12)
											 * {name,int4} */
	{1598, "random", false},	/* volatile,  internal(12) {float8} */
	{1599, "setseed", false},	/* volatile,  internal(12) {float8,void} */
	{1600, "asin", true},		/* immutable, internal(12) {float8,float8} */
	{1601, "acos", true},		/* immutable, internal(12) {float8,float8} */
	{1602, "atan", true},		/* immutable, internal(12) {float8,float8} */
	{1603, "atan2", true},		/* immutable, internal(12)
								 * {float8,float8,float8} */
	{1604, "sin", true},		/* immutable, internal(12) {float8,float8} */
	{1605, "cos", true},		/* immutable, internal(12) {float8,float8} */
	{1606, "tan", true},		/* immutable, internal(12) {float8,float8} */
	{1607, "cot", true},		/* immutable, internal(12) {float8,float8} */
	{1608, "degrees", true},	/* immutable, internal(12) {float8,float8} */
	{1609, "radians", true},	/* immutable, internal(12) {float8,float8} */
	{1610, "pi", true},			/* immutable, internal(12) {float8} */
	{1620, "ascii", true},		/* immutable, internal(12) {int4,text} */
	{1621, "chr", true},		/* immutable, internal(12) {int4,text} */
	{1622, "repeat", true},		/* immutable, internal(12) {int4,text,text} */
	{1623, "similar_escape", true}, /* immutable, internal(12)
									 * {text,text,text} */
	{1637, "like_escape", true},	/* immutable, internal(12)
									 * {text,text,text} */
	{1640, "pg_get_viewdef", false},	/* stable,    internal(12) {text,text} */
	{1665, "pg_get_serial_sequence", false},	/* stable,    internal(12)
												 * {text,text,text} */
	{1680, "substring", true},	/* immutable, internal(12) {int4,int4,bit,bit} */
	{1681, "length", true},		/* immutable, internal(12) {int4,bit} */
	{1682, "octet_length", true},	/* immutable, internal(12) {int4,bit} */
	{1683, "bit", true},		/* immutable, internal(12) {int4,int4,bit} */
	{1684, "int4", true},		/* immutable, internal(12) {int4,bit} */
	{1685, "bit", true},		/* immutable, internal(12) {bool,int4,bit,bit} */
	{1698, "position", true},	/* immutable, internal(12) {int4,bit,bit} */
	{1699, "substring", true},	/* immutable, internal(12) {int4,bit,bit} */
	{1703, "numeric", true},	/* immutable, internal(12)
								 * {int4,numeric,numeric} */
	{1705, "abs", true},		/* immutable, internal(12) {numeric,numeric} */
	{1706, "sign", true},		/* immutable, internal(12) {numeric,numeric} */
	{1707, "round", true},		/* immutable, internal(12)
								 * {int4,numeric,numeric} */
	{1708, "round", true},		/* immutable, sql(14)      {numeric,numeric} */
	{1709, "trunc", true},		/* immutable, internal(12)
								 * {int4,numeric,numeric} */
	{1710, "trunc", true},		/* immutable, sql(14)      {numeric,numeric} */
	{1711, "ceil", true},		/* immutable, internal(12) {numeric,numeric} */
	{1712, "floor", true},		/* immutable, internal(12) {numeric,numeric} */
	{1713, "length", true},		/* stable,    internal(12) {bytea,name,int4} */
	{1714, "convert_from", true},	/* stable,    internal(12)
									 * {bytea,name,text} */
	{1717, "convert_to", true}, /* stable,    internal(12) {bytea,name,text} */
	{1728, "mod", true},		/* immutable, internal(12)
								 * {numeric,numeric,numeric} */
	{1730, "sqrt", true},		/* immutable, internal(12) {numeric,numeric} */
	{1731, "numeric_sqrt", true},	/* immutable, internal(12)
									 * {numeric,numeric} */
	{1732, "exp", true},		/* immutable, internal(12) {numeric,numeric} */
	{1733, "numeric_exp", true},	/* immutable, internal(12)
									 * {numeric,numeric} */
	{1734, "ln", true},			/* immutable, internal(12) {numeric,numeric} */
	{1735, "numeric_ln", true}, /* immutable, internal(12) {numeric,numeric} */
	{1736, "log", true},		/* immutable, internal(12)
								 * {numeric,numeric,numeric} */
	{1737, "numeric_log", true},	/* immutable, internal(12)
									 * {numeric,numeric,numeric} */
	{1738, "pow", true},		/* immutable, internal(12)
								 * {numeric,numeric,numeric} */
	{1740, "numeric", true},	/* immutable, internal(12) {int4,numeric} */
	{1741, "log", true},		/* immutable, sql(14)      {numeric,numeric} */
	{1742, "numeric", true},	/* immutable, internal(12) {float4,numeric} */
	{1743, "numeric", true},	/* immutable, internal(12) {float8,numeric} */
	{1744, "int4", true},		/* immutable, internal(12) {int4,numeric} */
	{1745, "float4", true},		/* immutable, internal(12) {float4,numeric} */
	{1746, "float8", true},		/* immutable, internal(12) {float8,numeric} */
	{1764, "numeric_inc", true},	/* immutable, internal(12)
									 * {numeric,numeric} */
	{1768, "to_char", true},	/* stable,    internal(12)
								 * {text,text,interval} */
	{1770, "to_char", true},	/* stable,    internal(12)
								 * {text,text,timestamptz} */
	{1772, "to_char", true},	/* stable,    internal(12) {text,text,numeric} */
	{1773, "to_char", true},	/* stable,    internal(12) {int4,text,text} */
	{1774, "to_char", true},	/* stable,    internal(12) {int8,text,text} */
	{1775, "to_char", true},	/* stable,    internal(12) {text,text,float4} */
	{1776, "to_char", true},	/* stable,    internal(12) {text,text,float8} */
	{1777, "to_number", true},	/* stable,    internal(12) {text,text,numeric} */
	{1778, "to_timestamp", true},	/* stable,    internal(12)
									 * {text,text,timestamptz} */
	{1779, "int8", true},		/* immutable, internal(12) {int8,numeric} */
	{1780, "to_date", true},	/* stable,    internal(12) {text,text,date} */
	{1781, "numeric", true},	/* immutable, internal(12) {int8,numeric} */
	{1782, "numeric", true},	/* immutable, internal(12) {int2,numeric} */
	{1783, "int2", true},		/* immutable, internal(12) {int2,numeric} */
	{1810, "bit_length", true}, /* immutable, sql(14)      {bytea,int4} */
	{1811, "bit_length", true}, /* immutable, sql(14)      {int4,text} */
	{1812, "bit_length", true}, /* immutable, sql(14)      {int4,bit} */
	{1813, "convert", true},	/* stable,    internal(12)
								 * {bytea,bytea,name,name} */
	{1842, "int8_sum", true},	/* immutable, internal(12)
								 * {int8,numeric,numeric} */
	{1845, "to_ascii", true},	/* immutable, internal(12) {text,text} */
	{1846, "to_ascii", true},	/* immutable, internal(12) {int4,text,text} */
	{1847, "to_ascii", true},	/* immutable, internal(12) {name,text,text} */
	{1946, "encode", true},		/* immutable, internal(12) {bytea,text,text} */
	{1947, "decode", true},		/* immutable, internal(12) {bytea,text,text} */
	{1961, "timestamp", true},	/* immutable, internal(12)
								 * {int4,timestamp,timestamp} */
	{1967, "timestamptz", true},	/* immutable, internal(12)
									 * {int4,timestamptz,timestamptz} */
	{1968, "time", true},		/* immutable, internal(12) {int4,time,time} */
	{1969, "timetz", true},		/* immutable, internal(12)
								 * {int4,timetz,timetz} */
	{1973, "div", true},		/* immutable, internal(12)
								 * {numeric,numeric,numeric} */
	{1980, "numeric_div_trunc", true},	/* immutable, internal(12)
										 * {numeric,numeric,numeric} */
	{1986, "similar_to_escape", true},	/* immutable, internal(12)
										 * {text,text,text} */
	{1987, "similar_to_escape", true},	/* immutable, internal(12) {text,text} */
	{2007, "like", true},		/* immutable, internal(12) {bool,bytea,bytea} */
	{2008, "notlike", true},	/* immutable, internal(12) {bool,bytea,bytea} */
	{2009, "like_escape", true},	/* immutable, internal(12)
									 * {bytea,bytea,bytea} */
	{2010, "length", true},		/* immutable, internal(12) {bytea,int4} */
	{2012, "substring", true},	/* immutable, internal(12)
								 * {bytea,bytea,int4,int4} */
	{2013, "substring", true},	/* immutable, internal(12) {bytea,bytea,int4} */
	{2014, "position", true},	/* immutable, internal(12) {bytea,bytea,int4} */
	{2015, "btrim", true},		/* immutable, internal(12) {bytea,bytea,bytea} */
	{2019, "time", true},		/* stable,    internal(12) {time,timestamptz} */
	{2020, "date_trunc", true}, /* immutable, internal(12)
								 * {text,timestamp,timestamp} */
	{2021, "date_part", true},	/* immutable, internal(12)
								 * {text,float8,timestamp} */
	{2024, "timestamp", true},	/* immutable, internal(12) {date,timestamp} */
	{2025, "timestamp", true},	/* immutable, internal(12)
								 * {date,time,timestamp} */
	{2026, "pg_backend_pid", false},	/* stable,    internal(12) {int4} */
	{2027, "timestamp", true},	/* stable,    internal(12)
								 * {timestamp,timestamptz} */
	{2028, "timestamptz", true},	/* stable,    internal(12)
									 * {timestamp,timestamptz} */
	{2029, "date", true},		/* immutable, internal(12) {date,timestamp} */
	{2034, "pg_conf_load_time", false}, /* stable,    internal(12)
										 * {timestamptz} */
	{2037, "timezone", true},	/* volatile,  internal(12)
								 * {text,timetz,timetz} */
	{2038, "timezone", true},	/* immutable, internal(12)
								 * {interval,timetz,timetz} */
	{2041, "overlaps", true},	/* immutable, internal(12)
								 * {bool,timestamp,timestamp,timestamp,timestamp} */
	{2042, "overlaps", true},	/* immutable, sql(14)
								 * {bool,timestamp,timestamp,interval,interval} */
	{2043, "overlaps", true},	/* immutable, sql(14)
								 * {bool,timestamp,timestamp,timestamp,interval} */
	{2044, "overlaps", true},	/* immutable, sql(14)
								 * {bool,timestamp,timestamp,timestamp,interval} */
	{2046, "time", true},		/* immutable, internal(12) {time,timetz} */
	{2047, "timetz", true},		/* stable,    internal(12) {time,timetz} */
	{2048, "isfinite", true},	/* immutable, internal(12) {bool,timestamp} */
	{2049, "to_char", true},	/* stable,    internal(12)
								 * {text,text,timestamp} */
	{2058, "age", true},		/* immutable, internal(12)
								 * {timestamp,timestamp,interval} */
	{2059, "age", true},		/* stable,    sql(14) {timestamp,interval} */
	{2069, "timezone", true},	/* immutable, internal(12)
								 * {text,timestamp,timestamptz} */
	{2070, "timezone", true},	/* immutable, internal(12)
								 * {timestamp,timestamptz,interval} */
	{2073, "substring", true},	/* immutable, internal(12) {text,text,text} */
	{2074, "substring", true},	/* immutable, sql(14) {text,text,text,text} */
	{2075, "bit", true},		/* immutable, internal(12) {int8,int4,bit} */
	{2076, "int8", true},		/* immutable, internal(12) {int8,bit} */
	{2077, "current_setting", false},	/* stable,    internal(12) {text,text} */
	{2078, "set_config", false},	/* volatile,  internal(12)
									 * {bool,text,text,text} */
	{2085, "substr", true},		/* immutable, internal(12)
								 * {bytea,bytea,int4,int4} */
	{2086, "substr", true},		/* immutable, internal(12) {bytea,bytea,int4} */
	{2087, "replace", true},	/* immutable, internal(12)
								 * {text,text,text,text} */
	{2088, "split_part", true}, /* immutable, internal(12)
								 * {int4,text,text,text} */
	{2089, "to_hex", true},		/* immutable, internal(12) {int4,text} */
	{2090, "to_hex", true},		/* immutable, internal(12) {int8,text} */
	{2091, "array_lower", true},	/* immutable, internal(12)
									 * {int4,int4,anyarray} */
	{2092, "array_upper", true},	/* immutable, internal(12)
									 * {int4,int4,anyarray} */
	{2167, "ceiling", true},	/* immutable, internal(12) {numeric,numeric} */
	{2169, "power", true},		/* immutable, internal(12)
								 * {numeric,numeric,numeric} */
	{2170, "width_bucket", true},	/* immutable, internal(12)
									 * {int4,int4,numeric,numeric,numeric} */
	{2176, "array_length", false},	/* immutable, internal(12)
									 * {int4,int4,anyarray} */
	{2284, "regexp_replace", true}, /* immutable, internal(12)
									 * {text,text,text,text} */
	{2285, "regexp_replace", true}, /* immutable, internal(12)
									 * {text,text,text,text,text} */
	{2288, "pg_size_pretty", false},	/* volatile,  internal(12) {int8,text} */
	{2308, "ceil", true},		/* immutable, internal(12) {float8,float8} */
	{2309, "floor", true},		/* immutable, internal(12) {float8,float8} */
	{2310, "sign", true},		/* immutable, internal(12) {float8,float8} */
	{2311, "md5", true},		/* immutable, internal(12) {text,text} */
	{2319, "pg_encoding_max_length", false},	/* immutable, internal(12)
												 * {int4,int4} */
	{2320, "ceiling", true},	/* immutable, internal(12) {float8,float8} */
	{2321, "md5", true},		/* immutable, internal(12) {bytea,text} */
	{2557, "bool", true},		/* immutable, internal(12) {bool,int4} */
	{2558, "int4", true},		/* immutable, internal(12) {bool,int4} */
	{2559, "lastval", false},	/* volatile,  internal(12) {int8} */
	{2560, "pg_postmaster_start_time", false},	/* stable,    internal(12)
												 * {timestamptz} */
	{2621, "pg_reload_conf", false},	/* volatile,  internal(12) {bool} */
	{2622, "pg_rotate_logfile", false}, /* volatile,  internal(12) {bool} */
	{2623, "pg_stat_file", false},	/* volatile,  internal(12) {text,record} */
	{2624, "pg_read_file", false},	/* volatile,  internal(12)
									 * {int8,int8,text,text} */
	{2626, "pg_sleep", false},	/* volatile,  internal(12) {float8,void} */
	{2647, "transaction_timestamp", false}, /* stable,    internal(12)
											 * {timestamptz} */
	{2648, "statement_timestamp", false},	/* stable,    internal(12)
											 * {timestamptz} */
	{2649, "clock_timestamp", true},	/* volatile,  internal(12)
										 * {timestamptz} */
	{2705, "pg_has_role", false},	/* stable,    internal(12)
									 * {bool,name,name,text} */
	{2709, "pg_has_role", false},	/* stable,    internal(12)
									 * {bool,name,text} */
	{2711, "justify_interval", true},	/* immutable, internal(12)
										 * {interval,interval} */
	{2767, "regexp_split_to_array", false}, /* immutable, internal(12)
											 * {text,text,_text} */
	{2768, "regexp_split_to_array", false}, /* immutable, internal(12)
											 * {text,text,text,_text} */
	{2971, "text", true},		/* immutable, internal(12) {bool,text} */
	{3030, "overlay", true},	/* immutable, internal(12)
								 * {int4,int4,bit,bit,bit} */
	{3031, "overlay", true},	/* immutable, internal(12) {int4,bit,bit,bit} */
	{3032, "get_bit", true},	/* immutable, internal(12) {int4,int4,bit} */
	{3033, "set_bit", true},	/* immutable, internal(12) {int4,int4,bit,bit} */
	{3036, "pg_notify", false}, /* volatile,  internal(12) {text,text,void} */
	{3051, "xml_is_well_formed", false},	/* stable,    internal(12)
											 * {bool,text} */
	{3052, "xml_is_well_formed_document", false},	/* immutable, internal(12)
													 * {bool,text} */
	{3053, "xml_is_well_formed_content", false},	/* immutable, internal(12)
													 * {bool,text} */
	{3058, "concat", true},		/* stable,    internal(12) {text,any} */
	{3059, "concat_ws", true},	/* stable,    internal(12) {text,text,any} */
	{3060, "left", true},		/* immutable, internal(12) {int4,text,text} */
	{3061, "right", true},		/* immutable, internal(12) {int4,text,text} */
	{3062, "reverse", true},	/* immutable, internal(12) {text,text} */
	{3162, "pg_collation_for", false},	/* stable,    internal(12) {text,any} */
	{3166, "pg_size_pretty", false},	/* volatile,  internal(12)
										 * {text,numeric} */
	{3167, "array_remove", false},	/* immutable, internal(12)
									 * {anyarray,anyarray,anyelement} */
	{3168, "array_replace", false}, /* immutable, internal(12)
									 * {anyarray,anyarray,anyelement,anyelement} */
	{3179, "cardinality", false},	/* immutable, internal(12) {int4,anyarray} */
	{3461, "make_timestamp", true}, /* immutable, internal(12)
									 * {int4,int4,int4,int4,int4,float8,timestamp} */
	{3462, "make_timestamptz", true},	/* stable,    internal(12)
										 * {int4,int4,int4,int4,int4,float8,timestamptz} */
	{3463, "make_timestamptz", true},	/* stable,    internal(12)
										 * {int4,int4,int4,int4,int4,text,float8,timestamptz} */
	{3464, "make_interval", true},	/* immutable, internal(12)
									 * {int4,int4,int4,int4,int4,int4,float8,interval} */
	{3528, "enum_first", false},	/* stable,    internal(12)
									 * {anyenum,anyenum} */
	{3529, "enum_last", false}, /* stable,    internal(12) {anyenum,anyenum} */
	{3530, "enum_range", false},	/* stable,    internal(12)
									 * {anyarray,anyenum,anyenum} */
	{3531, "enum_range", false},	/* stable,    internal(12)
									 * {anyarray,anyenum} */
	{3533, "enum_send", false}, /* stable,    internal(12) {bytea,anyenum} */
	{3539, "format", false},	/* stable,    internal(12) {text,text,any} */
	{3540, "format", true},		/* stable,    internal(12) {text,text} */
	{3811, "money", true},		/* stable,    internal(12) {int4,money} */
	{3812, "money", true},		/* stable,    internal(12) {int8,money} */
	{3823, "numeric", true},	/* stable,    internal(12) {money,numeric} */
	{3824, "money", true},		/* stable,    internal(12) {money,numeric} */
	{3846, "make_date", true},	/* immutable, internal(12)
								 * {int4,int4,int4,date} */
	{3847, "make_time", true},	/* immutable, internal(12)
								 * {int4,int4,float8,time} */
	{3935, "pg_sleep_for", false},	/* volatile,  sql(14)      {interval,void} */
	{3936, "pg_sleep_until", false},	/* volatile,  sql(14)
										 * {timestamptz,void} */
	{4350, "normalize", true},	/* immutable, internal(12) {text,text,text} */
	{4351, "is_normalized", true},	/* immutable, internal(12)
									 * {bool,text,text} */
	{5044, "gcd", true},		/* immutable, internal(12) {int4,int4,int4} */
	{5045, "gcd", true},		/* immutable, internal(12) {int8,int8,int8} */
	{5046, "lcm", true},		/* immutable, internal(12) {int4,int4,int4} */
	{5047, "lcm", true},		/* immutable, internal(12) {int8,int8,int8} */
	{5048, "gcd", true},		/* immutable, internal(12)
								 * {numeric,numeric,numeric} */
	{5049, "lcm", true},		/* immutable, internal(12)
								 * {numeric,numeric,numeric} */
	{6162, "bit_count", true},	/* immutable, internal(12) {int8,bit} */
	{6163, "bit_count", true},	/* immutable, internal(12) {bytea,int8} */
	{6177, "date_bin", true},	/* immutable, internal(12)
								 * {timestamp,timestamp,timestamp,interval} */
	{6178, "date_bin", true},	/* immutable, internal(12)
								 * {timestamptz,timestamptz,timestamptz,interval} */
	{6195, "ltrim", true},		/* immutable, internal(12) {bytea,bytea,bytea} */
	{6196, "rtrim", true},		/* immutable, internal(12) {bytea,bytea,bytea} */
	{6198, "unistr", true},		/* immutable, internal(12) {text,text} */
	{6199, "extract", true},	/* immutable, internal(12) {text,date,numeric} */
	{6200, "extract", true},	/* immutable, internal(12) {text,time,numeric} */
	{6201, "extract", true},	/* immutable, internal(12)
								 * {text,timetz,numeric} */
	{6202, "extract", true},	/* immutable, internal(12)
								 * {text,timestamp,numeric} */
	{6203, "extract", true},	/* stable,    internal(12)
								 * {text,timestamptz,numeric} */
	{6204, "extract", true},	/* immutable, internal(12)
								 * {text,interval,numeric} */
};

/** Maximum number of arguments for specially treated user defined function */
#define VCI_MAX_APPLICABLE_UDF_NARGS (2)

/**
 * Template to specify a specially treated user defined function
 */
typedef struct
{
	const char *name;			/* Function name */
	Oid			namespace;		/* Namespace */
	int16		nargs;			/* Number of arguments */
	Oid			rettype;		/* Function return type */
	/** Function argument types. The number of elements is specified by nargs */
	Oid			argtypes[VCI_MAX_APPLICABLE_UDF_NARGS];
} vci_applicable_udf_template;

/*
 * Index numbers that are treated specially among applicable_udf_table[]
 */
#define APPLICABLE_UDF_TABLE_VCI_RUNS_IN_PLAN_INDEX (0)
#define APPLICABLE_UDF_TABLE_VCI_ALWAYS_RETURN_TRUE (1)

/**
 * Template table for specially treated user defined function
 *
 * However the top 2 functions are treated specially and have fixed array positions.
 */
static vci_applicable_udf_template applicable_udf_table[] = {

	{"vci_runs_in_plan", PG_PUBLIC_NAMESPACE, 0, BOOLOID, {0, 0}},
	{"vci_always_return_true", PG_PUBLIC_NAMESPACE, 0, BOOLOID, {0, 0}},

	{"vci_runs_in_query", PG_PUBLIC_NAMESPACE, 0, BOOLOID, {0, 0}},
	{"hamming_distance", PG_PUBLIC_NAMESPACE, 2, INT4OID, {BITOID, BITOID}}
};

static bool is_supported_udf(Oid oid);

/**
 * Determine if the given oid is a function that VCI can support
 *
 * @param[in] oid OID (pg_proc.oid) indicating the function to be determined
 * @return true if supported, false otherwise
 */
bool
vci_is_supported_function(Oid oid)
{
	int			min,
				max,
				pivot;

	if (FirstNormalObjectId <= oid)
		return is_supported_udf(oid);

	if ((oid < VCI_SUPPORTED_FUNC_MIN) || (VCI_SUPPORTED_FUNC_MAX < oid))
		return false;

	/* 2 minute search */

	min = 0;
	max = lengthof(vci_supported_func_table);	/* exclusive */

	while (max - min > 1)
	{
		Oid			comp;

		pivot = (min + max) / 2;

		comp = vci_supported_func_table[pivot].oid;

		if (comp == oid)
			return vci_supported_func_table[pivot].is_support;
		else if (oid < comp)
			max = pivot;
		else					/* comp < oid */
			min = pivot;
	}

	if (max - min == 1)
		if (oid == vci_supported_func_table[min].oid)
			return vci_supported_func_table[min].is_support;

	return false;
}

/**
 * Determine if the given user-defined functions indicated by the oid is treated specially by VCI
 *
 * @param[in] oid OID (pg_proc.oid) indicating the function to be determined
 * @return true if supported, false otherwise
 */
static bool
is_supported_udf(Oid oid)
{
	bool		result;

	result = false;

	for (int i = 0; i < vci_special_udf_info.num_applicable_udfs; i++)
	{
		if (oid == vci_special_udf_info.applicable_udfs[i])
		{
			result = true;
			break;
		}
	}

	return result;
}

/**
 * Register user defined function for special handling
 *
 * @param[in] snapshot Current snapshot
 *
 * This function is called every time before attempting to rewrite the VCI plan,
 * but the actual registration process is only called once within the PostgreSQL instance.
 */
void
vci_register_applicable_udf(Snapshot snapshot)
{
	bool		already_registerd;
	MemoryContext tmpcontext,
				oldcontext;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tuple;

	already_registerd = (vci_special_udf_info.num_applicable_udfs > 0);

	if (already_registerd)
		return;

	/*
	 * To use fmgr_info, a temporary memory context is needed, but since
	 * CurrentMemoryContext is SMC here, create child memory context from
	 * MessageContext.
	 */
	tmpcontext = AllocSetContextCreate(MessageContext,
									   "Register Applicable UDF",
									   ALLOCSET_DEFAULT_SIZES);

	oldcontext = MemoryContextSwitchTo(tmpcontext);

	rel = table_open(ProcedureRelationId, AccessShareLock);
	scan = table_beginscan(rel, snapshot, 0, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid			funcoid;
		Form_pg_proc procform;

		funcoid = ((Form_pg_proc) GETSTRUCT(tuple))->oid;

		/*
		 * UDF always takes an OID greater than or equal to
		 * FirstNormalObjectId
		 */
		if (funcoid < FirstNormalObjectId)
			continue;

		procform = (Form_pg_proc) GETSTRUCT(tuple);

		/*
		 * Check if tuple matches an entry in the template table
		 */
		for (int i = 0; i < lengthof(applicable_udf_table); i++)
		{
			vci_applicable_udf_template *entry = &applicable_udf_table[i];

			if ((procform->pronamespace != entry->namespace) ||
				(procform->pronargs != entry->nargs) ||
				(procform->prorettype != entry->rettype) ||
				(strcmp(NameStr(procform->proname), entry->name) != 0))
				goto next;

			for (int j = 0; j < Min(entry->nargs, VCI_MAX_APPLICABLE_UDF_NARGS); j++)
				if (procform->proargtypes.values[j] != entry->argtypes[j])
					goto next;

			vci_special_udf_info.applicable_udfs[vci_special_udf_info.num_applicable_udfs++]
				= funcoid;

			if (i == APPLICABLE_UDF_TABLE_VCI_RUNS_IN_PLAN_INDEX)
				vci_special_udf_info.vci_runs_in_plan_funcoid = funcoid;
			else if (i == APPLICABLE_UDF_TABLE_VCI_ALWAYS_RETURN_TRUE)
				vci_special_udf_info.vci_always_return_true_funcoid = funcoid;

			break;

	next:
			;
		}
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(tmpcontext);
}

/*==========================================================================*/
/* Implementation of PG function to check supported functions at CREATE EXTENSION */
/*==========================================================================*/

PG_FUNCTION_INFO_V1(vci_check_supported_functions);

Datum
vci_check_supported_functions(PG_FUNCTION_ARGS)
{
	Relation	rel;

	rel = table_open(ProcedureRelationId, AccessShareLock);

	for (int i = 0; i < lengthof(vci_supported_func_table); i++)
	{
		HeapTuple	tuple;
		Form_pg_proc procform;

		if (!vci_supported_func_table[i].is_support)
			continue;

		tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(vci_supported_func_table[i].oid));
		if (!HeapTupleIsValid(tuple))
			goto error;

		procform = (Form_pg_proc) GETSTRUCT(tuple);

		if (strcmp(vci_supported_func_table[i].name, NameStr(procform->proname)) != 0)
			goto error;

		ReleaseSysCache(tuple);
	}

	table_close(rel, AccessShareLock);

	PG_RETURN_VOID();

error:
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("extension \"%s\" cannot be installed under this version of PostgreSQL", VCI_STRING)));

	PG_RETURN_VOID();
}
