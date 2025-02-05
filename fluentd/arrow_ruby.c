/*
 * arrow_ruby.c
 *
 * A Ruby language extension to write out data as Apache Arrow files.
 * --
 * Copyright 2011-2021 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2021 (C) PG-Strom Developers Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the PostgreSQL License.
 */
#include <ruby.h>
#include <ctype.h>
#include <libgen.h>
#include <sys/file.h>
#include "float2.h"
#define Elog(fmt,...)							\
	rb_raise(rb_eException, "%s:%d " fmt,		\
			 __FILE__, __LINE__, ##__VA_ARGS__)
#include "arrow_ipc.h"

/*
 * Misc definitions
 */
#define SECS_PER_DAY	86400UL
#define IP4ADDR_LEN		4
#define IP6ADDR_LEN		16

static inline char *
trim_cstring(char *str)
{
	char   *end;

	while (isspace(*str))
		str++;
	end = str + strlen(str) - 1;
	while (end >= str && isspace(*end))
		*end-- = '\0';

	return str;
}

static VALUE
rb_puts(VALUE obj)
{
	return rb_funcall(rb_mKernel, rb_intern("puts"), 1, obj);
}

/*
 * context based memory allocation tracker
 */
typedef struct mallocContext
{
	struct mallocContext   *prev;
	struct mallocContext   *next;
	char		data[0];
} mallocContext;

#define mallocContextOfChunk(ptr)				\
	((mallocContext *)((char *)(ptr) - offsetof(mallocContext, data)))

static mallocContext	defaultMallocContext = { .prev = &defaultMallocContext,
												 .next = &defaultMallocContext };
static mallocContext   *currentMallocContext = &defaultMallocContext;

static inline void
mallocContextInit(mallocContext *mcxt)
{
	mcxt->prev = mcxt;
	mcxt->next = mcxt;
}

static inline mallocContext *
mallocContextSwitchTo(mallocContext *mcxt_new)
{
	mallocContext  *mcxt_old = currentMallocContext;

	currentMallocContext = mcxt_new;

	return mcxt_old;
}

static void
mallocContextRelease(mallocContext *mcxt)
{
	mallocContext  *curr;
	mallocContext  *next;

	for (curr = mcxt->next; curr != mcxt; curr = next)
	{
		next = curr->next;
		free(curr);
	}
}

void *
palloc(size_t sz)
{
	mallocContext  *next = currentMallocContext->next;
	mallocContext  *curr;

	curr = malloc(offsetof(mallocContext, data) + sz);
	if (!curr)
		rb_memerror();
	currentMallocContext->next = curr;
	curr->prev = currentMallocContext;
	curr->next = next;
	next->prev = curr;

	return curr->data;
}

void *
palloc0(size_t sz)
{
	void   *ptr = palloc(sz);

	memset(ptr, 0, sz);

	return ptr;
}

char *
pstrdup(const char *str)
{
	void   *dst = palloc(strlen(str) + 1);

	strcpy(dst, str);

	return dst;
}

void *
repalloc(void *old, size_t sz)
{
	mallocContext  *curr;

	if (!old)
		return palloc(sz);
	curr = mallocContextOfChunk(old);
	curr = realloc(curr, offsetof(mallocContext, data) + sz);
	if (curr)
	{
		curr->prev->next = curr;
		curr->next->prev = curr;
		return curr->data;
	}
	rb_memerror();
}

void
pfree(void *ptr)
{
	mallocContext  *curr = mallocContextOfChunk(ptr);
	curr->prev->next = curr->next;
	curr->next->prev = curr->prev;
	free(curr);
}

/* ----------------------------------------------------------------
 *
 * Put values handler for Ruby VALUE
 *
 * ----------------------------------------------------------------
 */
static inline void
__put_inline_null_value(SQLfield *column, size_t row_index, int sz)
{
	column->nullcount++;
	sql_buffer_clrbit(&column->nullmap, row_index);
	sql_buffer_append_zero(&column->values, sz);
}

#define STAT_UPDATES(COLUMN,FIELD,VALUE)					\
	do {													\
		if ((COLUMN)->stat_enabled)							\
		{													\
			if (!(COLUMN)->stat_datum.is_valid)				\
			{												\
				(COLUMN)->stat_datum.min.FIELD = VALUE;     \
				(COLUMN)->stat_datum.max.FIELD = VALUE;     \
				(COLUMN)->stat_datum.is_valid = true;		\
			}												\
			else											\
			{												\
				if ((COLUMN)->stat_datum.min.FIELD > VALUE) \
					(COLUMN)->stat_datum.min.FIELD = VALUE; \
				if ((COLUMN)->stat_datum.max.FIELD < VALUE) \
					(COLUMN)->stat_datum.max.FIELD = VALUE; \
			}												\
		}													\
	} while(0)

/*
 * Bool
 */
static int
__ruby_fetch_bool_value(VALUE datum)
{
	if (datum == Qnil)
		return -1;		/* null */
	if (datum == Qtrue)
		return 1;
	else if (datum == Qfalse)
		return 0;
	else
	{
		if (CLASS_OF(datum) == rb_cString)
		{
			const char *ptr = RSTRING_PTR(datum);
			size_t		len = RSTRING_LEN(datum);

			if ((len == 4 && (memcmp(ptr, "true", 4) == 0 ||
							  memcmp(ptr, "True", 4) == 0 ||
							  memcmp(ptr, "TRUE", 4) == 0)) ||
				(len == 1 && (memcmp(ptr, "t", 1) == 0 ||
							  memcmp(ptr, "T", 1) == 0)))
				return 1;
			if ((len == 5 && (memcmp(ptr, "false", 5) == 0 ||
							  memcmp(ptr, "False", 5) == 0 ||
							  memcmp(ptr, "FALSE", 5) == 0)) ||
				(len == 1 && (memcmp(ptr, "f", 1) == 0 ||
							  memcmp(ptr, "F", 1) == 0)))
				return 0;
			/* elsewhere, try to convert to Integer */
			datum = rb_funcall(datum, rb_intern("to_i"), 0);
		}

		if (CLASS_OF(datum) == rb_cInteger ||
			CLASS_OF(datum) == rb_cFloat ||
			CLASS_OF(datum) == rb_cRational)
		{
			int		ival = NUM2INT(datum);

			return (ival == 0 ? 0 : 1);
		}
	}
	Elog("unable to convert to boolean value");
}

static size_t
ruby_put_bool_value(SQLfield *column, const char *addr, int sz)
{
	size_t	row_index = column->nitems++;
	int		bval = __ruby_fetch_bool_value((VALUE)addr);

	if (bval < 0)
	{
		/* null */
		column->nullcount++;
		sql_buffer_clrbit(&column->nullmap, row_index);
		sql_buffer_clrbit(&column->values,  row_index);
	}
	else
	{
		sql_buffer_setbit(&column->nullmap, row_index);
		if (bval != 0)
			sql_buffer_setbit(&column->values,  row_index);
		else
			sql_buffer_clrbit(&column->values,  row_index);		
	}
	return __buffer_usage_inline_type(column);
}

/*
 * IntXX/UintXX
 */
static size_t
put_ruby_int8_value(SQLfield *column, const char *addr, int sz)
{
	VALUE		datum = (VALUE)addr;
	size_t		row_index = column->nitems++;

	if (datum == Qnil)
		__put_inline_null_value(column, row_index, sizeof(int8_t));
	else
	{
		int8_t		value = NUM2CHR(datum);

		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(int8_t));

		STAT_UPDATES(column,i8,value);
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_int16_value(SQLfield *column, const char *addr, int sz)
{
	VALUE		datum = (VALUE)addr;
	size_t		row_index = column->nitems++;

	if (datum == Qnil)
		__put_inline_null_value(column, row_index, sizeof(int16_t));
	else
	{
		int16_t		value = NUM2SHORT(datum);

		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(int16_t));

		STAT_UPDATES(column,i16,value);
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_int32_value(SQLfield *column, const char *addr, int sz)
{
	VALUE		datum = (VALUE)addr;
	size_t		row_index = column->nitems++;

	if (datum == Qnil)
		__put_inline_null_value(column, row_index, sizeof(int32_t));
	else
	{
		int32_t		value = NUM2INT(datum);

		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(int32_t));

		STAT_UPDATES(column,i32,value);
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_int64_value(SQLfield *column, const char *addr, int sz)
{
	VALUE		datum = (VALUE)addr;
	size_t		row_index = column->nitems++;

	if (datum == Qnil)
		__put_inline_null_value(column, row_index, sizeof(int64_t));
	else
	{
		int64_t		value = NUM2LONG(datum);

		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(int64_t));

		STAT_UPDATES(column,i64,value);
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_uint8_value(SQLfield *column, const char *addr, int sz)
{
	VALUE		datum = (VALUE)addr;
	size_t		row_index = column->nitems++;

	if (datum == Qnil)
		__put_inline_null_value(column, row_index, sizeof(uint8_t));
	else
	{
		uint32_t	value = NUM2UINT(datum);

		if (value > UCHAR_MAX)
			rb_raise(rb_eRangeError, "Uint8 out of range (%u)", value);
		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(uint8_t));

		STAT_UPDATES(column,u8,value);
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_uint16_value(SQLfield *column, const char *addr, int sz)
{
	VALUE		datum = (VALUE)addr;
	size_t		row_index = column->nitems++;

	if (datum == Qnil)
		__put_inline_null_value(column, row_index, sizeof(uint16_t));
	else
	{
		uint32_t	value = NUM2UINT(datum);

		if (value > USHRT_MAX)
			rb_raise(rb_eRangeError, "Uint16 out of range (%u)", value);
		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(uint16_t));

		STAT_UPDATES(column,u16,value);
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_uint32_value(SQLfield *column, const char *addr, int sz)
{
	VALUE		datum = (VALUE)addr;
	size_t		row_index = column->nitems++;

	if (datum == Qnil)
		__put_inline_null_value(column, row_index, sizeof(uint32_t));
	else
	{
		uint32_t	value = NUM2UINT(datum);

		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(uint32_t));

		STAT_UPDATES(column,u32,value);
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_uint64_value(SQLfield *column, const char *addr, int sz)
{
	VALUE		datum = (VALUE)addr;
	size_t		row_index = column->nitems++;

	if (datum == Qnil)
		__put_inline_null_value(column, row_index, sizeof(uint64_t));
	else
	{
		uint64_t	value = NUM2UINT(datum);

		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(uint64_t));

		STAT_UPDATES(column,u64,value);
	}
	return __buffer_usage_inline_type(column);
}

/*
 * FloatingPointXX
 */
static size_t
put_ruby_float16_value(SQLfield *column, const char *addr, int sz)
{
	VALUE		datum = (VALUE)addr;
	size_t		row_index = column->nitems++;

	if (datum == Qnil)
		__put_inline_null_value(column, row_index, sizeof(half_t));
	else
	{
		double	fval = NUM2DBL(datum);
		half_t	value = fp64_to_fp16(fval);

		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(half_t));

		STAT_UPDATES(column,f64,fval);
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_float32_value(SQLfield *column, const char *addr, int sz)
{
	VALUE		datum = (VALUE)addr;
	size_t		row_index = column->nitems++;

	if (datum == Qnil)
		__put_inline_null_value(column, row_index, sizeof(float));
	else
	{
		float	value = (float)NUM2DBL(datum);

		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(float));

		STAT_UPDATES(column,f32,value);
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_float64_value(SQLfield *column, const char *addr, int sz)
{
	VALUE		datum = (VALUE)addr;
	size_t		row_index = column->nitems++;

	if (datum == Qnil)
		__put_inline_null_value(column, row_index, sizeof(double));
	else
	{
		double	value = NUM2DBL(datum);

		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(double));

		STAT_UPDATES(column,f64,value);
	}
	return __buffer_usage_inline_type(column);
}

/*
 * Decimal
 */
static bool
__ruby_fetch_decimal_value(VALUE datum, int128_t *p_value, int scale)
{
	VALUE		datum_klass;
	bool		retry = false;

	if (datum == Qnil)
		return false;
retry_again:
	datum_klass = CLASS_OF(datum);
	if (datum_klass == rb_cInteger ||
		datum_klass == rb_cFloat ||
		datum_klass == rb_cRational)
	{
		VALUE	ival;

		if (scale > 0)
		{
			ival = rb_funcall(INT2NUM(10), rb_intern("**"),
							  1, INT2NUM(scale));
			datum = rb_funcall(datum, rb_intern("*"), 1, ival);
		}
		else if (scale < 0)
		{
			ival = rb_funcall(INT2NUM(10), rb_intern("**"),
							  1, INT2NUM(-scale));
			datum = rb_funcall(datum, rb_intern("/"), 1, ival);
		}
		/* convert to integer */
		if (CLASS_OF(datum) != rb_cInteger)
			datum = rb_funcall(datum, rb_intern("to_i"), 0);
		/* overflow check */
		ival = rb_funcall(datum, rb_intern("bit_length"), 0);
		if (NUM2INT(ival) > 128)
			Elog("decimal value out of range");
		rb_integer_pack(datum, p_value, sizeof(int128_t), 1, 0,
						INTEGER_PACK_LITTLE_ENDIAN);
		return true;
	}
	else if (!retry)
	{
		/* convert to String once, if not yet */
		if (datum_klass != rb_cString)
			datum = rb_funcall(datum, rb_intern("to_s"), 0);
		/* then, convert to Retional */
		datum = rb_Rational1(datum);
		retry = true;
		goto retry_again;
	}
	Elog("cannot convert to decimal value");
}

static size_t
put_ruby_decimal_value(SQLfield *column, const char *addr, int sz)
{
	size_t		row_index = column->nitems++;
	int128_t	value = 0;

	if (!__ruby_fetch_decimal_value((VALUE)addr, &value,
									column->arrow_type.Decimal.scale))
		__put_inline_null_value(column, row_index, sizeof(int128_t));
	else
	{
		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(double));

		STAT_UPDATES(column,i128,value);
	}
	return __buffer_usage_inline_type(column);
}

/*
 * common routine to fetch date/time value
 */
static bool
__ruby_fetch_timestamp_value(VALUE datum,
							 uint64_t *p_sec,	/* seconds from UTC */
							 uint64_t *p_nsec,	/* nano-seconds in the day */
							 bool convert_to_utc)
{
	bool	retry = false;
	VALUE	sec;
	VALUE	nsec;

	if (datum == Qnil)
		return false;	/* NULL */

	/* Is EventTime object? */
	if (rb_respond_to(datum, rb_intern("sec")) &&
		rb_respond_to(datum, rb_intern("nsec")))
	{
		sec = rb_funcall(datum, rb_intern("sec"), 0);
		nsec = rb_funcall(datum, rb_intern("nsec"), 0);

		*p_sec = NUM2ULONG(sec);
		*p_nsec = NUM2ULONG(nsec);

		return true;
	}
try_again:
	/* convertible to Time? (maybe String or Date) */
	if (rb_respond_to(datum, rb_intern("to_time")))
		datum = rb_funcall(datum, rb_intern("to_time"), 0);

	/* Is Time object? */
	if (rb_respond_to(datum, rb_intern("tv_sec")) &&
		rb_respond_to(datum, rb_intern("tv_nsec")) &&
		(!convert_to_utc || (rb_respond_to(datum, rb_intern("utc?")) &&
							 rb_respond_to(datum, rb_intern("getutc")))))
	{
		if (convert_to_utc)
		{
			VALUE	is_utc = rb_funcall(datum, rb_intern("utc?"), 0);

			if (is_utc != Qtrue)
				datum = rb_funcall(datum, rb_intern("getutc"), 0);
		}
		sec = rb_funcall(datum, rb_intern("tv_sec"), 0);
		nsec = rb_funcall(datum, rb_intern("tv_nsec"), 0);

		*p_sec = NUM2ULONG(sec);
		*p_nsec = NUM2ULONG(nsec);
		return true;
	}
	/* elsewhere, try to convert to String once, then retry */
	if (!retry)
	{
		retry = true;
		datum = rb_funcall(datum, rb_intern("to_s"), 0);
		goto try_again;
	}
	Elog("unable to extract sec/nsec from the supplied object");
}

/*
 * Date
 */
static size_t
put_ruby_date_day_value(SQLfield *column, const char *addr, int sz)
{
	size_t		row_index = column->nitems++;
	uint64_t	sec;
	uint64_t	nsec;

	if (!__ruby_fetch_timestamp_value((VALUE)addr, &sec, &nsec, false))
		__put_inline_null_value(column, row_index, sizeof(int32_t));
	else
	{
		uint32_t	value = sec / SECS_PER_DAY;

		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(uint32_t));
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_date_ms_value(SQLfield *column, const char *addr, int sz)
{
	size_t		row_index = column->nitems++;
	uint64_t	sec;
	uint64_t	nsec;

	if (!__ruby_fetch_timestamp_value((VALUE)addr, &sec, &nsec, false))
		__put_inline_null_value(column, row_index, sizeof(int64_t));
	else
	{
		uint64_t	value = (sec * 1000000L) + (nsec / 1000L);

		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(uint64_t));
	}
	return __buffer_usage_inline_type(column);
}

/*
 * Time
 */
static size_t
put_ruby_time_sec_value(SQLfield *column, const char *addr, int sz)
{
	size_t		row_index = column->nitems++;
	uint64_t	sec;
	uint64_t	nsec;
	uint32_t	value;

	if (!__ruby_fetch_timestamp_value((VALUE)addr, &sec, &nsec, false))
		__put_inline_null_value(column, row_index, sizeof(uint32_t));
	else
	{
		value = sec % SECS_PER_DAY;
		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(uint32_t));
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_time_ms_value(SQLfield *column, const char *addr, int sz)
{
	size_t		row_index = column->nitems++;
	uint64_t	sec;
	uint64_t	nsec;
	uint32_t	value;

	if (!__ruby_fetch_timestamp_value((VALUE)addr, &sec, &nsec, false))
		__put_inline_null_value(column, row_index, sizeof(uint32_t));
	else
	{
		value = (sec % SECS_PER_DAY) * 1000 + (nsec / 1000000);
		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(uint32_t));
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_time_us_value(SQLfield *column, const char *addr, int sz)
{
	size_t		row_index = column->nitems++;
	uint64_t	sec;
	uint64_t	nsec;
	uint64_t	value;

	if (!__ruby_fetch_timestamp_value((VALUE)addr, &sec, &nsec, false))
		__put_inline_null_value(column, row_index, sizeof(uint64_t));
	else
	{
		value = (sec % SECS_PER_DAY) * 1000000L + (nsec / 1000L);
		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(uint64_t));
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_time_ns_value(SQLfield *column, const char *addr, int sz)
{
	size_t		row_index = column->nitems++;
	uint64_t	sec;
	uint64_t	nsec;
	uint64_t	value;

	if (!__ruby_fetch_timestamp_value((VALUE)addr, &sec, &nsec, false))
		__put_inline_null_value(column, row_index, sizeof(int64_t));
	else
	{
		value = (sec % SECS_PER_DAY) * 1000000000L + nsec;
		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(uint64_t));
	}
	return __buffer_usage_inline_type(column);
}

/*
 * Timestamp
 */
static size_t
put_ruby_timestamp_sec_value(SQLfield *column, const char *addr, int sz)
{
	size_t		row_index = column->nitems++;
	uint64_t	sec;
	uint64_t	nsec;

	if (!__ruby_fetch_timestamp_value((VALUE)addr, &sec, &nsec, true))
		__put_inline_null_value(column, row_index, sizeof(int64_t));
	else
	{
		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &sec, sizeof(uint64_t));
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_timestamp_ms_value(SQLfield *column, const char *addr, int sz)
{
	size_t		row_index = column->nitems++;
	uint64_t	sec;
	uint64_t	nsec;
	uint64_t	value;

	if (!__ruby_fetch_timestamp_value((VALUE)addr, &sec, &nsec, true))
		__put_inline_null_value(column, row_index, sizeof(int64_t));
	else
	{
		value = sec * 1000L + nsec / 1000000L;
		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(uint64_t));
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_timestamp_us_value(SQLfield *column, const char *addr, int sz)
{
	size_t		row_index = column->nitems++;
	uint64_t	sec;
	uint64_t	nsec;
	uint64_t	value;

	if (!__ruby_fetch_timestamp_value((VALUE)addr, &sec, &nsec, true))
		__put_inline_null_value(column, row_index, sizeof(int64_t));
	else
	{
		value = sec * 1000000L + nsec / 1000L;
		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(uint64_t));
	}
	return __buffer_usage_inline_type(column);
}

static size_t
put_ruby_timestamp_ns_value(SQLfield *column, const char *addr, int sz)
{
	size_t		row_index = column->nitems++;
	uint64_t	sec;
	uint64_t	nsec;
	uint64_t	value;

	if (!__ruby_fetch_timestamp_value((VALUE)addr, &sec, &nsec, true))
		__put_inline_null_value(column, row_index, sizeof(int64_t));
	else
	{
		value = sec * 1000000000L + nsec;
		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, &value, sizeof(uint64_t));
	}
	return __buffer_usage_inline_type(column);
}

/*
 * Utf8 (String)
 */
static size_t
put_ruby_utf8_value(SQLfield *column, const char *addr, int sz)
{
	static VALUE utf8_encoding = Qnil;
	VALUE		datum = (VALUE)addr;
	size_t		row_index = column->nitems++;

	if (row_index == 0)
		sql_buffer_append_zero(&column->values, sizeof(uint32_t));
	if (datum == Qnil)
	{
		column->nullcount++;
		sql_buffer_clrbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values,
						  &column->extra.usage, sizeof(uint32_t));
	}
	else
	{
		VALUE		encoding;

		if (TYPE(datum) != T_STRING)
			datum = rb_funcall(datum, rb_intern("to_s"), 0);
		if (utf8_encoding == Qnil)
		{
			VALUE	klass = rb_path2class("Encoding");

			utf8_encoding = rb_const_get(klass, rb_intern("UTF_8"));
		}
		/* force to convert UTF-8 string, if needed */
		encoding = rb_funcall(datum, rb_intern("encoding"), 0);
		if (encoding != utf8_encoding)
			datum = rb_funcall(datum, rb_intern("encode"), 1, utf8_encoding);

		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->extra,
						  RSTRING_PTR(datum),
						  RSTRING_LEN(datum));
		sql_buffer_append(&column->values,
						  &column->extra.usage, sizeof(uint32_t));
	}
	return __buffer_usage_varlena_type(column);
}

/*
 * common routine to fetch ip address
 */
static bool
__ruby_fetch_ipaddr_value(VALUE datum, unsigned char *buf, int ip_version)
{
	bool		retry = false;

	if (datum == Qnil)
		return false;	/* NULL */

retry_again:
	/* Is IPAddr object? */
	if (rb_respond_to(datum, rb_intern("ipv4?")) &&
		rb_respond_to(datum, rb_intern("ipv6?")) &&
		rb_respond_to(datum, rb_intern("to_i")))
	{
		VALUE	bval;
		VALUE	ival;

		if ((ip_version == 4 || ip_version < 0) &&
			(bval = rb_funcall(datum, rb_intern("ipv4?"), 0)) == Qtrue)
		{
			ival = rb_funcall(datum, rb_intern("to_i"), 0);

			rb_integer_pack(ival, buf, IP4ADDR_LEN, 1, 0,
							INTEGER_PACK_LITTLE_ENDIAN);
			return true;
		}
		if ((ip_version == 6 || ip_version < 0) &&
			(bval = rb_funcall(datum, rb_intern("ipv6?"), 0)) == Qtrue)
		{
			ival = rb_funcall(datum, rb_intern("to_i"), 0);

			rb_integer_pack(ival, buf, IP6ADDR_LEN, 1, 0,
							INTEGER_PACK_LITTLE_ENDIAN);
			return true;
		}
		Elog("IPAddr is not IPv%d format", ip_version);
	}

	/* Elsewhere try to convert to IPAddr */
	if (!retry)
	{
		static VALUE ipaddr_klass = Qnil;

		/* Load 'ipaddr' module once */
		if (ipaddr_klass == Qnil)
		{
			rb_require("ipaddr");
			ipaddr_klass = rb_path2class("IPAddr");
		}
		if (TYPE(datum) != T_STRING)
			datum = rb_funcall(datum, rb_intern("to_s"), 0);
		datum = rb_class_new_instance(1, &datum, ipaddr_klass);
		retry = true;
		goto retry_again;
	}
	Elog("unable to convert datum to logical Arrow::Ipaddr4/6");
}


/*
 * Ipaddr4
 */
static size_t
put_ruby_logical_ip4addr_value(SQLfield *column, const char *addr, int sz)
{
	size_t	row_index = column->nitems++;
	unsigned char buf[IP6ADDR_LEN];

	if (!__ruby_fetch_ipaddr_value((VALUE)addr, buf, 4))
		__put_inline_null_value(column, row_index, IP4ADDR_LEN);
	else
	{
		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, buf, IP4ADDR_LEN);
	}
	return __buffer_usage_inline_type(column);
}

/*
 * Ipaddr6
 */
static size_t
put_ruby_logical_ip6addr_value(SQLfield *column, const char *addr, int sz)
{
	size_t	row_index = column->nitems++;
	unsigned char buf[IP6ADDR_LEN];

	if (!__ruby_fetch_ipaddr_value((VALUE)addr, buf, 6))
		__put_inline_null_value(column, row_index, IP6ADDR_LEN);
	else
	{
		sql_buffer_setbit(&column->nullmap, row_index);
		sql_buffer_append(&column->values, buf, IP6ADDR_LEN);
	}
	return __buffer_usage_inline_type(column);
}

/* ----------------------------------------------------------------
 *
 * Write out min/max statistics handler
 *
 * ---------------------------------------------------------------- */
static int
write_ruby_int8_stat(SQLfield *column, char *buf, size_t len,
					 const SQLstat__datum *stat_datum)
{
	return snprintf(buf, len, "%d", (int32_t)stat_datum->i8);
}

static int
write_ruby_int16_stat(SQLfield *column, char *buf, size_t len,
					  const SQLstat__datum *stat_datum)
{
	return snprintf(buf, len, "%d", (int32_t)stat_datum->i16);
}

static int
write_ruby_int32_stat(SQLfield *column, char *buf, size_t len,
					  const SQLstat__datum *stat_datum)
{
	return snprintf(buf, len, "%d", stat_datum->i32);
}

static int
write_ruby_int64_stat(SQLfield *column, char *buf, size_t len,
					  const SQLstat__datum *stat_datum)
{
	return snprintf(buf, len, "%ld", stat_datum->i64);
}

static int
write_ruby_int128_stat(SQLfield *column, char *buf, size_t len,
					   const SQLstat__datum *stat_datum)
{
	int128_t	ival = stat_datum->i128;
	char		temp[64];
	char	   *pos = temp + sizeof(temp) - 1;
	bool		is_minus = false;

	/* special case handling if INT128 min value */
	if (~ival == (int128_t)0)
		return snprintf(buf, len, "-170141183460469231731687303715884105728");
	if (ival < 0)
	{
		is_minus = true;
		ival = -ival;
	}

	*pos = '\0';
	do {
		int		dig = ival % 10;

		*--pos = ('0' + dig);
		ival /= 10;
	} while (ival != 0);

	return snprintf(buf, len, "%s%s", (is_minus ? "-" : ""), pos);
}

static int
write_ruby_uint8_stat(SQLfield *column, char *buf, size_t len,
					  const SQLstat__datum *stat_datum)
{
	return snprintf(buf, len, "%u", (uint32_t)stat_datum->i8);
}

static int
write_ruby_uint16_stat(SQLfield *column, char *buf, size_t len,
					   const SQLstat__datum *stat_datum)
{
	return snprintf(buf, len, "%u", (uint32_t)stat_datum->i16);
}

static int
write_ruby_uint32_stat(SQLfield *column, char *buf, size_t len,
					   const SQLstat__datum *stat_datum)
{
	return snprintf(buf, len, "%u", (uint32_t)stat_datum->i32);
}


static int
write_ruby_uint64_stat(SQLfield *column, char *buf, size_t len,
					   const SQLstat__datum *stat_datum)
{
	return snprintf(buf, len, "%lu", (uint64_t)stat_datum->i64);
}

/* ----------------------------------------------------------------
 *
 * Routines related to initializer
 *
 * ----------------------------------------------------------------
 */
static void
__arrowFilePathnameValidator(VALUE self, VALUE __pathname)
{
	const char *str;
	uint32_t	len, i;

	if (CLASS_OF(__pathname) != rb_cString)
		Elog("pathname must be String");
	str = RSTRING_PTR(__pathname);
	len = RSTRING_LEN(__pathname);
	if (len == 0)
		Elog("pathname must not be empty");
	if (*str != '/')
		Elog("pathname must be absolute path: %.*s", len, str);

	for (i=0; i < len; i++)
	{
		if (str[i] != '%')
			continue;
		if (++i >= len)
			Elog("invalid pathname configuration: %.*s", len, str);
		switch (str[i])
		{
			case 'Y':	/* Year (4-digits) */
			case 'y':	/* Year (2-digits) */
			case 'm':	/* month (01-12) */
			case 'd':	/* day (01-31) */
			case 'H':	/* hour (00-23) */
			case 'M':	/* minute (00-59) */
			case 'S':	/* second (00-59) */
			case 'p':	/* process's PID */
			case 'q':	/* sequence number */
				break;
			default:
				Elog("unknown format character: '%c' in '%.*s'",
					 str[i], len, str);
		}
	}
	rb_ivar_set(self, rb_intern("pathname"),
				rb_str_new(RSTRING_PTR(__pathname),
						   RSTRING_LEN(__pathname)));
}

static void
arrowFieldAddCustomMetadata(SQLfield *column,
                            const char *key,
                            const char *value)
{
	ArrowKeyValue *kv;
	size_t		sz;

	sz = sizeof(ArrowKeyValue) * (column->numCustomMetadata + 1);
	column->customMetadata = repalloc(column->customMetadata, sz);
	kv = &column->customMetadata[column->numCustomMetadata++];
	kv->key = pstrdup(key);
	kv->_key_len = strlen(key);
	kv->value = pstrdup(value);
	kv->_value_len = strlen(value);
}

static int
__assignFieldTypeBool(SQLfield *column)
{
	initArrowNode(&column->arrow_type, Bool);
	column->put_value = ruby_put_bool_value;

	return 2;		/* nullmap + values */
}

static int
__assignFieldTypeInt(SQLfield *column, const char *extra)
{
	initArrowNode(&column->arrow_type, Int);
	column->arrow_type.Int.is_signed = true;
	if (strcmp(extra, "8") == 0)
	{
		column->arrow_type.Int.bitWidth = 8;
		column->put_value = put_ruby_int8_value;
		column->write_stat = write_ruby_int8_stat;
	}
	else if (strcmp(extra, "16") == 0)
	{
		column->arrow_type.Int.bitWidth = 16;
		column->put_value = put_ruby_int16_value;
		column->write_stat = write_ruby_int16_stat;
	}
	else if (strcmp(extra, "32") == 0)
	{
		column->arrow_type.Int.bitWidth = 32;
		column->put_value = put_ruby_int32_value;
		column->write_stat = write_ruby_int32_stat;
	}
	else if (strcmp(extra, "64") == 0)
	{
		column->arrow_type.Int.bitWidth = 64;
		column->put_value = put_ruby_int64_value;
		column->write_stat = write_ruby_int64_stat;
	}
	else
		Elog("Not a supported Int width (%s)", extra);

	return 2;	/* nullmap + values */
}

static int
__assignFieldTypeUint(SQLfield *column, const char *extra)
{
	initArrowNode(&column->arrow_type, Int);
	column->arrow_type.Int.is_signed = false;
	if (strcmp(extra, "8") == 0)
	{
		column->arrow_type.Int.bitWidth = 8;
		column->put_value = put_ruby_uint8_value;
		column->write_stat = write_ruby_uint8_stat;
	}
	else if (strcmp(extra, "16") == 0)
	{
		column->arrow_type.Int.bitWidth = 16;
		column->put_value = put_ruby_uint16_value;
		column->write_stat = write_ruby_uint16_stat;
	}
	else if (strcmp(extra, "32") == 0)
	{
		column->arrow_type.Int.bitWidth = 32;
		column->put_value = put_ruby_uint32_value;
		column->write_stat = write_ruby_uint32_stat;
	}
	else if (strcmp(extra, "64") == 0)
	{
		column->arrow_type.Int.bitWidth = 64;
		column->put_value = put_ruby_uint64_value;
		column->write_stat = write_ruby_uint64_stat;
	}
	else
		Elog("Not a supported Uint width (%s)", extra);

	return 2;	/* nullmap + values */
}

static int
__assignFieldTypeFloatingPoint(SQLfield *column, const char *extra)
{
	initArrowNode(&column->arrow_type, FloatingPoint);
	if (strcmp(extra, "16") == 0)
	{
		column->arrow_type.FloatingPoint.precision
			= ArrowPrecision__Half;
		column->put_value = put_ruby_float16_value;
		column->write_stat = write_ruby_int16_stat;
	}
	else if (strcmp(extra, "32") == 0)
	{
		column->arrow_type.FloatingPoint.precision
			= ArrowPrecision__Single;
		column->put_value = put_ruby_float32_value;
		column->write_stat = write_ruby_int32_stat;
	}
	else if (strcmp(extra, "64") == 0)
	{
		column->arrow_type.FloatingPoint.precision
			= ArrowPrecision__Double;
		column->put_value = put_ruby_float64_value;
		column->write_stat = write_ruby_int64_stat;
	}
	else
		Elog("Not a supported FloatingPoint width (%s)", extra);

	return 2;	/* nullmap + values */
}

static int
__assignFieldTypeDecimal(SQLfield *column, const char *extra)
{
	int		bitWidth = 128;
	int		precision = 30;
	int		scale = 8;

	if (strncmp(extra, "128", 3) == 0)
		extra += 3;
	if (*extra == '(')
	{
		const char *tail = extra + strlen(extra) - 1;

		if (*tail != ')')
			Elog("Arrow::Decimal definition syntax error");
		if (sscanf(extra, "(%u,%u)", &precision, &scale) != 2)
		{
			precision = 30;		/* revert */
			if (sscanf(extra, "(%u)", &scale) != 1)
				Elog("Arrow::Decimal definition syntax error");
		}
	}
	else if (*extra != '\0')
		Elog("Arrow::Decimal definition syntax error");

	initArrowNode(&column->arrow_type, Decimal);
	column->arrow_type.Decimal.precision = precision;
	column->arrow_type.Decimal.scale = scale;
	column->arrow_type.Decimal.bitWidth = bitWidth;
	column->put_value = put_ruby_decimal_value;
	column->write_stat = write_ruby_int128_stat;

	return 2;	/* nullmap + values */
}

static int
__assignFieldTypeDate(SQLfield *column, const char *extra)
{
	initArrowNode(&column->arrow_type, Date);
	if (strcmp(extra, "[day]") == 0 || strcmp(extra, "") == 0)
	{
		column->arrow_type.Date.unit = ArrowDateUnit__Day;
		column->put_value = put_ruby_date_day_value;
		column->write_stat = write_ruby_int32_stat;
	}
	else if (strcmp(extra, "[ms]") == 0)
	{
		column->arrow_type.Date.unit = ArrowDateUnit__MilliSecond;
		column->put_value = put_ruby_date_ms_value;
		column->write_stat = write_ruby_int64_stat;
	}
	else
		Elog("Arrow::Date - not a supported unit size: %s", extra);

	return 2;
}

static int
__assignFieldTypeTime(SQLfield *column, const char *extra)
{
	initArrowNode(&column->arrow_type, Time);
	if (strcmp(extra, "[sec]") == 0 || strcmp(extra, "") == 0)
	{
		column->arrow_type.Time.bitWidth = 32;
		column->put_value = put_ruby_time_sec_value;
		column->write_stat = write_ruby_int32_stat;
	}
	else if (strcmp(extra, "[ms]") == 0)
	{
		column->arrow_type.Time.bitWidth = 32;
		column->put_value = put_ruby_time_ms_value;
		column->write_stat = write_ruby_int32_stat;
	}
	else if (strcmp(extra, "[us]") == 0)
	{
		column->arrow_type.Time.bitWidth = 64;
		column->put_value = put_ruby_time_us_value;
		column->write_stat = write_ruby_int64_stat;
	}
	else if (strcmp(extra, "[ns]") == 0)
	{
		column->arrow_type.Time.bitWidth = 64;
		column->put_value = put_ruby_time_ns_value;
		column->write_stat = write_ruby_int64_stat;
	}
	else
		Elog("Arrow::Time - not a supported unit size: %s", extra);

	return 2;	/* nullmap + values */
}

static int
__assignFieldTypeTimestamp(SQLfield *column, const char *extra)
{
	initArrowNode(&column->arrow_type, Timestamp);
	/* with timezone? */
	if (strncmp(extra, "Tz", 2) == 0)
	{
		struct tm	tm;
		time_t		t;

		t = time(NULL);
		localtime_r(&t, &tm);
		if (tm.tm_zone)
		{
			column->arrow_type.Timestamp.timezone = pstrdup(tm.tm_zone);
			column->arrow_type.Timestamp._timezone_len = strlen(tm.tm_zone);
		}
		extra += 2;
	}

	if (strcmp(extra, "[sec]") == 0)
	{
		column->arrow_type.Timestamp.unit = ArrowTimeUnit__Second;
		column->put_value = put_ruby_timestamp_sec_value;
		column->write_stat = write_ruby_int64_stat;
	}
	else if (strcmp(extra, "[ms]") == 0)
	{
		column->arrow_type.Timestamp.unit = ArrowTimeUnit__MilliSecond;
		column->put_value = put_ruby_timestamp_ms_value;
		column->write_stat = write_ruby_int64_stat;
	}
	else if (strcmp(extra, "[us]") == 0)
	{
		column->arrow_type.Timestamp.unit = ArrowTimeUnit__MicroSecond;
		column->put_value = put_ruby_timestamp_us_value;
		column->write_stat = write_ruby_int64_stat;
	}
	else if (strcmp(extra, "[ns]") == 0)
	{
		column->arrow_type.Timestamp.unit = ArrowTimeUnit__NanoSecond;
        column->put_value = put_ruby_timestamp_ns_value;
        column->write_stat = write_ruby_int64_stat;
	}
	else
		Elog("Arrow::Time - not a supported unit size: %s", extra);

	return 2;
}

static int
__assignFieldTypeInterval(SQLfield *column, const char *extra)
{
	Elog("Arrow::Interval - not implemented yet");
}

static int
__assignFieldTypeUtf8(SQLfield *column)
{
	initArrowNode(&column->arrow_type, Utf8);
	column->put_value = put_ruby_utf8_value;

	return 3;	/* nullmap + index + extra */
}

static int
__assignFieldTypeIpaddr4(SQLfield *column)
{
	initArrowNode(&column->arrow_type, FixedSizeBinary);
	column->arrow_type.FixedSizeBinary.byteWidth = IP4ADDR_LEN;
	column->put_value = put_ruby_logical_ip4addr_value;
	arrowFieldAddCustomMetadata(column, "pg_type", "pg_catalog.inet");

	return 2;	/* nullmap + values */
}

static int
__assignFieldTypeIpaddr6(SQLfield *column)
{
	initArrowNode(&column->arrow_type, FixedSizeBinary);
	column->arrow_type.FixedSizeBinary.byteWidth = IP6ADDR_LEN;
	column->put_value = put_ruby_logical_ip6addr_value;
	arrowFieldAddCustomMetadata(column, "pg_type", "pg_catalog.inet");

	return 2;	/* nullmap + values */
}

static int
__arrowFileAssignFieldType(SQLfield *column,
						   const char *field_name,
						   const char *field_type,
						   bool stat_enabled)
{
	column->field_name = pstrdup(field_name);
	column->stat_enabled = stat_enabled;

	if (strcmp(field_type, "Bool") == 0)
		return __assignFieldTypeBool(column);
	else if (strncmp(field_type, "Int", 3) == 0)
		return  __assignFieldTypeInt(column, field_type + 3);
	else if (strncmp(field_type, "Uint", 4) == 0)
		return  __assignFieldTypeUint(column, field_type + 4);
	else if (strncmp(field_type, "Float", 5) == 0)
		return  __assignFieldTypeFloatingPoint(column, field_type + 5);
	else if (strncmp(field_type, "Decimal", 7) == 0)
		return  __assignFieldTypeDecimal(column, field_type + 7);
	else if (strncmp(field_type, "Date", 4) == 0)
		return  __assignFieldTypeDate(column, field_type + 4);
	else if (strncmp(field_type, "Time", 4) == 0)
		return  __assignFieldTypeTime(column, field_type + 4);
	else if (strncmp(field_type, "Timestamp", 9) == 0)
		return  __assignFieldTypeTimestamp(column, field_type + 9);
	else if (strncmp(field_type, "Interval", 8) == 0)
		return  __assignFieldTypeInterval(column, field_type + 8);
    else if (strcmp(field_type, "Utf8") == 0)
		return  __assignFieldTypeUtf8(column);
	else if (strcmp(field_type, "Ipaddr4") == 0)
		return  __assignFieldTypeIpaddr4(column);
	else if (strcmp(field_type, "Ipaddr6") == 0)
		return  __assignFieldTypeIpaddr6(column);

	Elog("ArrowFile: not a supported type");
}

/*
 * Parsing the Schema Definition
 */
typedef struct
{
	const char *field_name;
	const char *field_type;
	bool		stat_enabled;
} field_def;

static VALUE
__arrowFileInitTable(VALUE arg)
{
	void	  **__args = (void **)arg;
	SQLtable   *table = __args[0];
	field_def  *fields = __args[1];
	uint32_t	j, nbuffers = 0;

	table->fdesc = -1;
	for (j=0; j < table->nfields; j++)
	{
		nbuffers += __arrowFileAssignFieldType(&table->columns[j],
											   fields[j].field_name,
											   fields[j].field_type,
											   fields[j].stat_enabled);
	}
	table->numFieldNodes = table->nfields;
	table->numBuffers = nbuffers;

	return Qnil;
}

static SQLtable *
__arrowFileParseSchemaDefs(VALUE self, VALUE __schema_defs)
{
	VALUE		threshold = rb_ivar_get(self, rb_intern("record_batch_threshold"));
	mallocContext *mcxt;
	mallocContext *mcxt_saved;
	char	   *schema_defs;
	char	   *tok, *saveptr;
	SQLtable   *table = NULL;
	field_def  *fields = NULL;
	const char *str;
	int			len;
	int			nrooms = 0;
	uintptr_t	nfields = 0;
	int			status;
	void	   *__args[3];

	if (CLASS_OF(__schema_defs) != rb_cString)
		Elog("schema_defs must be a String");
	str = RSTRING_PTR(__schema_defs);
	len = RSTRING_LEN(__schema_defs);
	schema_defs = alloca(len+1);
	memcpy(schema_defs, str, len);
	schema_defs[len] = '\0';

	for (tok = strtok_r(schema_defs, ",", &saveptr);
		 tok != NULL;
		 tok = strtok_r(NULL, ",", &saveptr))
	{
		/* <column_name>=<column_type>[;<column_attr>;...] */
		char	   *field_name = tok;
		char	   *field_type;
		char	   *__extra;
		char	   *__tok, *__saveptr;
		bool		stat_enabled = false;

		field_type = strchr(field_name, '=');
		if (!field_type)
			Elog("syntax error in schema definition: %.*s", len, str);
		*field_type++ = '\0';

		__extra = strchr(field_type, ';');
		if (__extra)
		{
			*__extra++ = '\0';
			for (__tok = strtok_r(__extra, ";", &__saveptr);
				 __tok != NULL;
				 __tok = strtok_r(NULL, ";", &__saveptr))
			{
				char   *attr = trim_cstring(__tok);

				if (strcmp(attr, "stat_enabled") == 0)
				{
					if (stat_enabled)
						Elog("duplicated column attribute: %s", attr);
					stat_enabled = true;
				}
				else
				{
					Elog("unknown column attribute: %s", attr);
				}
			}
		}
		/* save the definition temporary */
		if (nfields >= nrooms)
		{
			field_def  *__fields;
			int			__nrooms = 100 + 2 * nrooms;

			__fields = alloca(sizeof(field_def) * __nrooms);
			if (nfields > 0)
				memcpy(__fields, fields, sizeof(field_def) * nfields);
			fields = __fields;
			nrooms = __nrooms;
		}
		fields[nfields].field_name = trim_cstring(field_name);
		fields[nfields].field_type = trim_cstring(field_type);
		fields[nfields].stat_enabled = stat_enabled;
		nfields++;
	}

	/* construction of SQLtable */
	mcxt = calloc(1, (offsetof(mallocContext, data) +
					  offsetof(SQLtable, columns[nfields])));
	if (!mcxt)
		Elog("out of memory");
	table = (SQLtable *)mcxt->data;
	table->segment_sz = NUM2LONG(threshold);
	table->nfields = nfields;
	mallocContextInit(mcxt);
	mcxt_saved = mallocContextSwitchTo(mcxt);
	__args[0] = table;
	__args[1] = fields;
	rb_protect(__arrowFileInitTable, (VALUE)__args, &status);
	if (status != 0)
	{
		mallocContextSwitchTo(mcxt_saved);
		mallocContextRelease(mcxt);
		free(mcxt);
		rb_jump_tag(status);	//throw again
	}
	mallocContextSwitchTo(mcxt_saved);

	return table;
}

static void
__arrowFileParseParams(VALUE self,
					   VALUE hash,
					   VALUE *p_ts_column,
					   VALUE *p_tag_column)
{
	VALUE		datum;
	size_t		r_threshold = 240;
	size_t		f_threshold = 10000;

	if (hash == Qnil)
		goto out;
	if (CLASS_OF(hash) != rb_cHash)
		Elog("ArrowFile: parameters must be Hash");

	datum = rb_hash_fetch(hash, rb_str_new_cstr("ts_column"));
	if (datum != Qnil)
	{
		if (CLASS_OF(datum) != rb_cString)
			datum = rb_funcall(datum, rb_intern("to_s"), 0);
		*p_ts_column = datum;
	}

	datum = rb_hash_fetch(hash, rb_str_new_cstr("tag_column"));
	if (datum != Qnil)
	{
		if (CLASS_OF(datum) != rb_cString)
			datum = rb_funcall(datum, rb_intern("to_s"), 0);
		*p_tag_column = datum;
	}

	datum = rb_hash_fetch(hash, rb_str_new_cstr("record_batch_threshold"));
	if (datum != Qnil)
	{
		r_threshold = NUM2LONG(datum);
		if (r_threshold < 16 || r_threshold > 2048)
			Elog("record_batch_threshold must be [16...2048]");
	}

	datum = rb_hash_fetch(hash, rb_str_new_cstr("filesize_threshold"));
	if (datum != Qnil)
	{
		f_threshold = NUM2LONG(datum);
		if (f_threshold < 16 || f_threshold > 1048576)
			Elog("filesize_threshold must be [16...1048576]");
	}
out:
	rb_ivar_set(self, rb_intern("record_batch_threshold"),
				LONG2NUM(r_threshold << 20));
	rb_ivar_set(self, rb_intern("filesize_threshold"),
				LONG2NUM(f_threshold << 20));
}

static VALUE
rb_ArrowFile__initialize(VALUE self,
						 VALUE __pathname,
						 VALUE __schema_defs,
						 VALUE __params)
{
	SQLtable   *table;
	VALUE		ts_column = Qnil;
	VALUE		tag_column = Qnil;

	__arrowFilePathnameValidator(self, __pathname);
	__arrowFileParseParams(self, __params,
						   &ts_column,
						   &tag_column);
	table = __arrowFileParseSchemaDefs(self, __schema_defs);

	/* lookup ts_column/tag_column if any */
	if (ts_column != Qnil || tag_column != Qnil)
	{
		int		__ts_column = -1;
		int		__tag_column = -1;
		int		j;

		for (j=0; j < table->nfields; j++)
		{
			SQLfield   *column = &table->columns[j];

			if (ts_column != Qnil &&
				__ts_column < 0 &&
				strlen(column->field_name) == RSTRING_LEN(ts_column) &&
				memcmp(column->field_name,
					   RSTRING_PTR(ts_column),
					   RSTRING_LEN(ts_column)) == 0)
				__ts_column = j;

			if (tag_column != Qnil &&
				__tag_column < 0 &&
				strlen(column->field_name) == RSTRING_LEN(tag_column) &&
				memcmp(column->field_name,
					   RSTRING_PTR(tag_column),
					   RSTRING_LEN(tag_column)) == 0)
				__tag_column = j;
		}
		if (ts_column != Qnil)
			ts_column = (__ts_column < 0 ? Qnil : INT2NUM(__ts_column));
		if (tag_column != Qnil)
			tag_column = (__tag_column < 0 ? Qnil : INT2NUM(__tag_column));
	}
	/* instance variables */
	rb_ivar_set(self, rb_intern("schema_defs"),
				rb_str_new(RSTRING_PTR(__schema_defs),
						   RSTRING_LEN(__schema_defs)));
	rb_ivar_set(self, rb_intern("table"), (VALUE)table);
	rb_ivar_set(self, rb_intern("ts_column"), ts_column);
	rb_ivar_set(self, rb_intern("tag_column"), tag_column);

	return self;
}

/* ----------------------------------------------------------------
 *
 * Routines related to ArrowFile::open / close
 *
 * ----------------------------------------------------------------
 */
static bool
__arrowFileSwitchNext(int fdesc, const char *filename, struct stat *st_buf_new)
{
	char	   *d_name, *__d_buf;
	char	   *b_name, *__b_buf;
	struct stat	st_buf_cur;
	int			d_desc = -1;

	__d_buf = alloca(strlen(filename) + 1);
	strcpy(__d_buf, filename);
	d_name = dirname(__d_buf);

	__b_buf = alloca(strlen(filename) + 1);
	strcpy(__b_buf, filename);
	b_name = basename(__b_buf);

	d_desc = open(d_name, O_RDONLY | O_DIRECTORY);
	if (d_desc < 0)
		goto error_0;

	if (flock(d_desc, LOCK_EX) != 0)
		goto error_1;

	/* <-- exclusive lock on the directory --> */
	if (stat(filename, &st_buf_cur) != 0)
		goto error_1;

	/* pathname is not renamed yet? */
	if (st_buf_cur.st_dev == st_buf_new->st_dev &&
		st_buf_cur.st_ino == st_buf_new->st_ino)
	{
		char   *n_name = alloca(strlen(b_name) + 100);
		int		suffix;

		for (suffix = 1; ; suffix++)
		{
			sprintf(n_name, "%s.%d", b_name, suffix);
			if (faccessat(d_desc, n_name, F_OK, 0) != 0)
			{
				if (errno == ENOENT &&
					renameat(d_desc, b_name,
							 d_desc, n_name) == 0)
					break;

				goto error_1;
			}
		}
	}
	close(d_desc);
	return true;

error_1:
	close(d_desc);
error_0:
	return false;
}

static void
__arrowFileClose(SQLtable *table)
{
	if (table->fdesc >= 0)
	{
		close(table->fdesc);
		free((void *)table->filename);
		table->fdesc = -1;
		table->filename = NULL;
	}
}

static void
arrowFileOpen(VALUE self, bool force_next_file)
{
	SQLtable   *table = (SQLtable *)rb_ivar_get(self, rb_intern("table"));
	VALUE		pathname = rb_ivar_get(self, rb_intern("pathname"));
	const char *str;
	char	   *buf = alloca(2000);
	uint32_t	bufsz = 2000;
	uint32_t	i, j, len;
	int			fdesc;
	time_t		__time;
	struct tm	tm;

	__time = time(NULL);
	localtime_r(&__time, &tm);

	assert(CLASS_OF(pathname) == rb_cString);
	str = RSTRING_PTR(pathname);
	len = RSTRING_LEN(pathname);
	for (i=0, j=0; i < len; i++)
	{
		int		c = str[i];

		assert(j + 20 < bufsz);
		if (c != '%')
			buf[j++] = c;
		else if (++i < len)
		{
			switch (str[i])
			{
				case 'Y':
					j += snprintf(buf+j, bufsz-j, "%04u", tm.tm_year + 1900);
					break;
				case 'y':
					j += snprintf(buf+j, bufsz-j, "%02u", tm.tm_year % 100);
					break;
				case 'm':
					j += snprintf(buf+j, bufsz-j, "%02u", tm.tm_mon + 1);
					break;
				case 'd':
					j += snprintf(buf+j, bufsz-j, "%02u", tm.tm_mday);
					break;
				case 'H':
					j += snprintf(buf+j, bufsz-j, "%02u", tm.tm_hour);
					break;
				case 'M':
					j += snprintf(buf+j, bufsz-j, "%02u", tm.tm_min);
					break;
				case 'S':
					j += snprintf(buf+j, bufsz-j, "%02u", tm.tm_sec);
					break;
				case 'p':
					j += snprintf(buf+j, bufsz-j, "%u", getpid());
					break;
				default:
					Elog("unknown format character at: %.*s", len, str);
			}
		}
		else
		{
			Elog("Bug? unclosed format charaster at: %.*s", len, str);
		}

		/* expand the buffer if little margin */
		if (j + 20 >= bufsz)
		{
			size_t	__bufsz = 2 * bufsz + 1000;
			char   *__buf = alloca(__bufsz);

			memcpy(__buf, buf, j);
			bufsz = __bufsz;
			buf = __buf;
		}
	}
	buf[j] = '\0';
retry_again:
	/* ok, try to open the output file */
	fdesc = open(buf, O_RDWR | O_CREAT, 0644);
	if (fdesc < 0)
		Elog("ArrowWrite: failed to open '%s': %m", buf);
	/* close the previous output file, if any */
	if (table->fdesc >= 0)
	{
		/*
		 * Ensure the above file is not the previous one, even if
		 * pathname is generated from an identical configuration.
		 */
		if (force_next_file)
		{
			struct stat	st_buf_old;
			struct stat	st_buf_new;

			if (fstat(table->fdesc, &st_buf_old) != 0 ||
				fstat(fdesc, &st_buf_new) != 0)
			{
				close(fdesc);
				Elog("failed on fstat: %m");
			}
			if (st_buf_old.st_dev == st_buf_new.st_dev &&
				st_buf_old.st_ino == st_buf_new.st_ino)
			{
				if (!__arrowFileSwitchNext(fdesc, buf, &st_buf_new))
				{
					close(fdesc);
					Elog("failed on to switch output file [%s]", buf);
				}
				close(fdesc);
				goto retry_again;
			}
		}
		__arrowFileClose(table);
	}
	table->filename = strdup(buf);
	if (!table->filename)
	{
		close(fdesc);
		rb_memerror();
	}
	table->fdesc = fdesc;
}

static VALUE
__arrowFileWriteRecordBatch(VALUE self)
{
	SQLtable   *table = (SQLtable *)rb_ivar_get(self, rb_intern("table"));
	size_t		threshold;
	struct stat	stat_buf;
	int			j;

	threshold = NUM2LONG(rb_ivar_get(self, rb_intern("filesize_threshold")));
	if (fstat(table->fdesc, &stat_buf) != 0)
		Elog("failed on fstat(2): %m");
	if (stat_buf.st_size >= threshold)
		return Qfalse;

	/* cleanup temporary buffers during the writing out */
	table->numRecordBatches = 0;
	table->recordBatches = NULL;
	table->numCustomMetadata = 0;
	table->customMetadata = NULL;
	for (j=0; j < table->numFieldNodes; j++)
	{
		SQLfield   *column = &table->columns[j];

		column->customMetadata = NULL;
		column->numCustomMetadata = 0;
	}

	if (stat_buf.st_size == 0)
	{
		/* case of an empty file */
		arrowFileWrite(table, "ARROW1\0\0", 8);
		writeArrowSchema(table);
	}
	else
	{
		ArrowFileInfo af_info;
		uint32_t	nitems;
		size_t		nbytes;
		size_t		offset;
		char		buffer[80];

		readArrowFileDesc(table->fdesc, &af_info);
		//TODO: check Schema compatibility

		/* restore RecordBatches already in the file */
		nitems = af_info.footer._num_recordBatches;
		table->numRecordBatches = nitems;
		table->recordBatches = palloc(sizeof(ArrowBlock) * nitems);
		memcpy(table->recordBatches,
			   af_info.footer.recordBatches,
			   sizeof(ArrowBlock) * nitems);

		//TODO: if cleanup context, last chunk shall be
		//      merged to the current buffer.

		/* move to the file offset in front of the Footer */
		nbytes = sizeof(int32_t) + 6;	/* strlen("ARROW1") */
		offset = stat_buf.st_size - nbytes;
		if (pread(table->fdesc, buffer, nbytes, offset) != nbytes)
			Elog("failed on pread(2): %m");
		offset -= *((uint32_t *)buffer);
		if (lseek(table->fdesc, offset, SEEK_SET) < 0)
			Elog("failed on lseek(2): %m");
		table->f_pos = offset;
	}
	writeArrowRecordBatch(table);
	writeArrowFooter(table);

	return Qtrue;
}

static void
arrowFileWriteRecordBatch(VALUE self)
{
	SQLtable	   *table = (SQLtable *)rb_ivar_get(self, rb_intern("table"));
	VALUE			retval;
	int				status;
	mallocContext	mcxt;
	mallocContext  *mcxt_saved;

	assert(table->nitems > 0);
	for (;;)
	{
		/* BEGIN critical section */
		if (flock(table->fdesc, LOCK_EX) != 0)
			Elog("failed on flock('%s', LOCK_EX): %m", table->filename);

		mallocContextInit(&mcxt);
		mcxt_saved = mallocContextSwitchTo(&mcxt);
		retval = rb_protect(__arrowFileWriteRecordBatch, self, &status);
		if (status != 0)
		{
			flock(table->fdesc, LOCK_UN);
			mallocContextSwitchTo(mcxt_saved);
			mallocContextRelease(&mcxt);
			rb_jump_tag(status);
		}
		/* END critical section */
		flock(table->fdesc, LOCK_UN);
		mallocContextSwitchTo(mcxt_saved);
		mallocContextRelease(&mcxt);

		if (retval == Qtrue)
			break;
		/* switch the output file again */
		arrowFileOpen(self, true);
	}
}

/*
 * ArrowFile::writeRow method handler
 */
static VALUE
__arrowFile__writeRow(VALUE __arg)
{
	VALUE	   *args = (VALUE *)__arg;
	VALUE		self	= args[0];
	VALUE		tag		= args[1];
	VALUE		ts		= args[2];
	VALUE		record	= args[3];
	SQLtable   *table	= (SQLtable *)rb_ivar_get(self, rb_intern("table"));
	int			tag_column = -1;
	int			ts_column = -1;
	VALUE		datum;
	long		threshold;
	size_t		len;
	int			j;

	datum = rb_ivar_get(self, rb_intern("ts_column"));
	if (datum != Qnil)
		ts_column = NUM2INT(datum);
	datum = rb_ivar_get(self, rb_intern("tag_column"));
	if (datum != Qnil)
		tag_column = NUM2INT(datum);
	datum = rb_ivar_get(self, rb_intern("record_batch_threshold"));
	threshold = NUM2LONG(datum);

	if (table->fdesc < 0)
		arrowFileOpen(self, false);

	for (j=0, len=0; j < table->nfields; j++)
	{
		SQLfield   *column = &table->columns[j];
		const char *cname = column->field_name;

		if (j == ts_column)
			datum = ts;
		else if (j == tag_column)
			datum = tag;
		else
			datum = rb_hash_fetch(record, rb_str_new_cstr(cname));

		len += column->put_value(column, (const char *)datum, -1);
	}
	table->nitems++;

	if (len >= threshold)
	{
		arrowFileWriteRecordBatch(self);
		sql_table_clear(table);
	}
	return Qnil;
}

static VALUE
rb_ArrowFile__writeRow(VALUE self,
					   VALUE __tag,
					   VALUE __ts,
					   VALUE __record)
{
	SQLtable   *table = (SQLtable *)rb_ivar_get(self, rb_intern("table"));
	size_t		nitems_saved = table->nitems;
	VALUE		__args[4];
	VALUE		retval;
	int			j, status;
	mallocContext *mcxt;
	mallocContext *mcxt_saved;

	__args[0] = self;
	__args[1] = __tag;
	__args[2] = __ts;
	__args[3] = __record;
	mcxt = mallocContextOfChunk(table);
	mcxt_saved = mallocContextSwitchTo(mcxt);
	retval = rb_protect(__arrowFile__writeRow, (VALUE)__args, &status);
	if (status != 0)
	{
		/* revert current buffer usage, on errors */
		for (j=0; j < table->numFieldNodes; j++)
			table->columns[j].nitems = nitems_saved;
		table->nitems = nitems_saved;

		mallocContextSwitchTo(mcxt_saved);
		rb_jump_tag(status);
	}
	mallocContextSwitchTo(mcxt_saved);

	return retval;
}

static VALUE
callback_test(VALUE yield_value, VALUE private_datum, int argc, VALUE *argv)
{
	int		i;

	puts("yield_value:");
	rb_puts(yield_value);
	puts("private_datum:");
	rb_puts(private_datum);
	printf("argc = %d\n", argc);
	for (i=0; i < argc; i++)
		rb_puts(argv[i]);

	return Qnil;
}

static VALUE
rb_ArrowFile__writeChunk(VALUE self,
						 VALUE chunk)
{
	rb_block_call(chunk,
				  rb_intern("each"),
				  0,
				  NULL,
				  callback_test,
				  self);
	return Qnil;
}

#if 0
static VALUE
rb_ArrowFile__nextChunk(VALUE self)
{
	SQLtable   *table = (SQLtable *)rb_ivar_get(self, rb_intern("table"));

	if (table->nitems > 0)
		arrowFileWriteRecordBatch(self);
	return Qnil;
}
#endif

static VALUE
rb_ArrowFile__cleanup(VALUE self)
{
	mallocContext *mcxt;
	SQLtable   *table = (SQLtable *)rb_ivar_get(self, rb_intern("table"));

	if (table->nitems > 0)
		arrowFileWriteRecordBatch(self);
	__arrowFileClose(table);
	
	mcxt = mallocContextOfChunk(table);
	mallocContextRelease(mcxt);
	free(mcxt);

	return Qnil;
}

#if 0
static VALUE
rb_ArrowFile__test(VALUE self, VALUE datum)
{
	int128_t	value;

	printf("classname [%s]  ", rb_class2name(CLASS_OF(datum)));
	if (!__ruby_fetch_decimal_value(datum, &value, 5))
		printf(" --> NULL\n");
	else
		printf(" --> %ld\n", (int64_t)value);

	return Qnil;
}
#endif

void
Init_ArrowFile(void)
{
	VALUE	klass;

	klass = rb_define_class("ArrowFile",  rb_cObject);
	rb_define_method(klass, "initialize", rb_ArrowFile__initialize, 3);
	rb_define_method(klass, "writeRow",   rb_ArrowFile__writeRow, 3);
	rb_define_method(klass, "writeChunk", rb_ArrowFile__writeChunk, 1);
//	rb_define_method(klass, "nextChunk",  rb_ArrowFile__nextChunk, 0);
	rb_define_method(klass, "cleanup",    rb_ArrowFile__cleanup, 0);

	//rb_define_method(klass, "test", rb_ArrowFile__test, 1);
}
