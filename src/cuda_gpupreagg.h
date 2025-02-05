/*
 * cuda_gpupreagg.h
 *
 * Preprocess of aggregate using GPU acceleration, to reduce number of
 * rows to be processed by CPU; including the Sort reduction.
 * --
 * Copyright 2011-2021 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2021 (C) PG-Strom Developers Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the PostgreSQL License.
 */
#ifndef CUDA_GPUPREAGG_H
#define CUDA_GPUPREAGG_H

struct kern_gpupreagg
{
	kern_errorbuf	kerror;				/* kernel error information */
	cl_uint			num_group_keys;		/* nogroup reduction, if 0 */
	cl_uint			read_slot_pos;		/* offset to read kds_slot */
	cl_uint			grid_sz;			/* grid-size of setup/join kernel */
	cl_uint			block_sz;			/* block-size of setup/join kernel */
	cl_bool			setup_slot_done;	/* setup stage is done, if true */
	cl_bool			final_buffer_modified; /* true, if kds_final is modified */
	/* -- suspend/resume (KDS_FORMAT_BLOCK) */
	cl_bool			resume_context;		/* resume kernel, if true */
	cl_uint			suspend_count;		/* number of suspended blocks */
	cl_uint			suspend_size;		/* offset to suspend buffer, if any */
	/* -- runtime statistics -- */
	cl_uint			nitems_real;		/* out: # of outer input rows */
	cl_uint			nitems_filtered;	/* out: # of removed rows by quals */
	cl_uint			num_groups;			/* out: # of new groups */
	cl_uint			extra_usage;		/* out: size of new allocation */
	/* -- debug counter -- */
	cl_ulong		tv_stat_debug1;		/* out: debug counter 1 */
	cl_ulong		tv_stat_debug2;		/* out: debug counter 2 */
	cl_ulong		tv_stat_debug3;		/* out: debug counter 3 */
	cl_ulong		tv_stat_debug4;		/* out: debug counter 4 */
	/* -- kernel parameters buffer -- */
	kern_parambuf	kparams;
	/* <-- gpupreaggSuspendContext[], if any --> */
};
typedef struct kern_gpupreagg	kern_gpupreagg;

/*
 * gpupreaggSuspendContext is used to suspend gpupreagg_setup_block kernel.
 * Because KDS_FORMAT_BLOCK can have more items than estimation, so we cannot
 * avoid overflow of @kds_slot buffer preliminary. If @nitems exceeds @nrooms,
 * gpupreagg_setup_block will exit immediately, and save the current context
 * on the gpupreagg_suspend_context array to resume later.
 */
typedef union
{
	struct {
		size_t		src_base;
	} r;	/* row-format */
	struct {
		cl_uint		part_index;
		cl_uint		line_index;
	} b;	/* block-format */
	struct {
		size_t		src_base;
	} c;	/* arrow- and column-format */
} gpupreaggSuspendContext;

/* macro definitions to reference packed values */
#define KERN_GPUPREAGG_PARAMBUF(kgpreagg)				\
	(&(kgpreagg)->kparams)
#define KERN_GPUPREAGG_PARAMBUF_LENGTH(kgpreagg)		\
	((kgpreagg)->kparams.length)
#define KERN_GPUPREAGG_LENGTH(kgpreagg)					\
	(offsetof(kern_gpupreagg, kparams) +				\
	 KERN_GPUPREAGG_PARAMBUF_LENGTH(kgpreagg))
/* suspend/resume buffer for KDS_FORMAT_BLOCK */
#define KERN_GPUPREAGG_SUSPEND_CONTEXT(kgpreagg,group_id)	\
	((kgpreagg)->suspend_size > 0							\
	 ? ((gpupreaggSuspendContext *)							\
		((char *)KERN_GPUPREAGG_PARAMBUF(kgpreagg) +		\
		 KERN_GPUPREAGG_PARAMBUF_LENGTH(kgpreagg))) + (group_id) \
	 : NULL)

/*
 * preagg_hash_item
 */
typedef struct
{
	cl_uint		index;		/* index onto kds_slot/kds_final */
	cl_uint		hash;		/* hash value of the entry */
	cl_uint		next;		/* next index, or UINT_MAX if not */
} preagg_hash_item;

#define GPUPREAGG_LOCAL_HASH_NSLOTS		1153
typedef struct
{
	cl_uint		nitems;		/* current usage of hitems[] */
	cl_uint		l_hslots[GPUPREAGG_LOCAL_HASH_NSLOTS];
} preagg_local_hashtable;

/*
 * kern_global_hashslot
 *
 * An array of pagg_datum and its usage statistics, to be placed on
 * global memory area. Usage counter is used to break a loop to find-
 * out an empty slot if hash-slot is already filled-up.
 */
typedef struct
{
	size_t		length;		/* max length of the buffer */
	cl_uint		lock;		/* shared/exclusive lock */
	cl_uint		usage;		/* current usage of the hash item in the tail */
	cl_uint		nslots;		/* current size of the hash slots */
	cl_uint		slots[FLEXIBLE_ARRAY_MEMBER];
} kern_global_hashslot;

#define GLOBAL_HASHSLOT_GETITEM(f_hash, index)							\
	((preagg_hash_item *)((char *)(f_hash) + (f_hash)->length -			\
						  sizeof(preagg_hash_item) * ((index) + 1)))

#ifndef __CUDACC__
/*
 * gpupreagg_reset_kernel_task - reset kern_gpupreagg status prior to resume
 */
STATIC_INLINE(void)
gpupreagg_reset_kernel_task(kern_gpupreagg *kgpreagg, bool resume_context)
{
	memset(&kgpreagg->kerror, 0, sizeof(kern_errorbuf));
	kgpreagg->read_slot_pos   = 0;
	kgpreagg->setup_slot_done = false;
	kgpreagg->final_buffer_modified = false;
	kgpreagg->resume_context  = resume_context;
	kgpreagg->suspend_count   = 0;
}
#else	/* __CUDACC__ */
/*
 * gpupreagg_quals_eval(_arrow) - qualifier of outer scan
 */
DEVICE_FUNCTION(cl_bool)
gpupreagg_quals_eval(kern_context *kcxt,
					 kern_data_store *kds,
					 ItemPointerData *t_self,
					 HeapTupleHeaderData *htup);
DEVICE_FUNCTION(cl_bool)
gpupreagg_quals_eval_arrow(kern_context *kcxt,
						   kern_data_store *kds,
						   cl_uint row_index);
DEVICE_FUNCTION(cl_bool)
gpupreagg_quals_eval_column(kern_context *kcxt,
							kern_data_store *kds,
							kern_data_extra *extra,
							cl_uint row_index);
/*
 * hash value calculation function - to be generated by PG-Strom on the fly
 */
DEVICE_FUNCTION(cl_uint)
gpupreagg_hashvalue(kern_context *kcxt,
					cl_char *slot_dclass,
					Datum   *slot_values);

/*
 * comparison function - to be generated by PG-Strom on the fly
 *
 * It compares two records indexed by 'x_index' and 'y_index' on the supplied
 * kern_data_store, then returns -1 if record[X] is less than record[Y],
 * 0 if record[X] is equivalent to record[Y], or 1 if record[X] is greater
 * than record[Y].
 * (auto generated function)
 */
extern __device__ cl_bool		GPUPREAGG_ATTR_IS_GROUPBY_KEY[];

DEVICE_FUNCTION(cl_bool)
gpupreagg_keymatch(kern_context *kcxt,
				   kern_data_store *x_kds, size_t x_index,
				   kern_data_store *y_kds, size_t y_index);

/*
 * merge functions - to be generated by PG-Strom on the fly
 */
extern __device__ cl_int		GPUPREAGG_NUM_ACCUM_VALUES;
extern __device__ cl_int		GPUPREAGG_ACCUM_EXTRA_BUFSZ;
extern __device__ cl_int		GPUPREAGG_LOCAL_HASH_NROOMS;
extern __device__ cl_int		GPUPREAGG_HLL_REGISTER_BITS;
extern __device__ cl_short		GPUPREAGG_ACCUM_MAP_LOCAL[];
extern __device__ cl_short		GPUPREAGG_ACCUM_MAP_GLOBAL[];
extern __device__ cl_bool		GPUPREAGG_ATTR_IS_ACCUM_VALUES[];

DEVICE_FUNCTION(void)
gpupreagg_init_slot(cl_char  *dst_dclass,
					Datum    *dst_values,
					char     *dst_extras,
					cl_short *dst_attmap);



DEVICE_FUNCTION(void)
gpupreagg_init_local_slot(cl_char  *dst_dclass,
						  Datum    *dst_values,
						  char     *dst_extras);
DEVICE_FUNCTION(void)
gpupreagg_init_final_slot(cl_char  *dst_dclass,
						  Datum    *dst_values,
						  char     *dst_extras);
/*
 * merge operation by shuffle
 *
 * Its dclass/values must be private array built according to
 * the tlist_part definition.
 */
DEVICE_FUNCTION(void)
gpupreagg_merge_shuffle(cl_char  *priv_dclass,	// private
						Datum    *priv_values,	// private
						cl_short *priv_attmap,
						int       lane_id);
/*
 * normal update operation (not atomic operations)
 *
 * The source dclass/values must be built according to the tlist_prep
 * (thus, slots on kds_slots should be given). On the other hands, the
 * destination dclass/values must be built according to the tlist_part;
 * like kds_final, or local/private array.
 */
DEVICE_FUNCTION(void)
gpupreagg_update_normal(cl_char  *dst_dclass,	/* tlist_part */
						Datum    *dst_values,	/* tlist_part */
						cl_short *dst_attmap,
						cl_char  *src_dclass,	/* tlist_prep (kds_slot) */
						Datum    *src_values,	/* tlist_prep (kds_slot) */
						cl_short *src_attmap);
/*
 * atomic merge operation
 *
 * Both of source/destination dclass/values must be built according to
 * the tlist_part. So, slots on kds_slot should not be used.
 */
DEVICE_FUNCTION(void)
gpupreagg_merge_atomic(cl_char  *dst_dclass,
					   Datum    *dst_values,
					   cl_short *dst_attmap,
					   cl_char  *src_dclass,
					   Datum    *src_values,
					   cl_short *src_attmap);
/*
 * atomic update operations
 *
 * The source dclass/values must be built according to the tlist_prep
 * (thus, slots on kds_slots should be given). On the other hands, the
 * destination dclass/values must be built according to the tlist_part;
 * like kds_final, or local/private array.
 */
DEVICE_FUNCTION(void)
gpupreagg_update_atomic(cl_char *dst_dclass,
						Datum    *dst_values,
						cl_short *dst_attmap,
						cl_char  *src_dclass,	/* tlist_prep(kds_slot) */
						Datum    *src_values,	/* tlist_prep(kds_slot) */
						cl_short *src_attmap);

/*
 * translate a kern_data_store (input) into an output form
 * (auto generated function)
 */
DEVICE_FUNCTION(void)
gpupreagg_projection_row(kern_context *kcxt,
						 kern_data_store *kds_src,	/* in */
						 HeapTupleHeaderData *htup,	/* in */
						 cl_char *dst_dclass,		/* out */
						 Datum   *dst_values);		/* out */
DEVICE_FUNCTION(void)
gpupreagg_projection_arrow(kern_context *kcxt,
						   kern_data_store *kds_src,	/* in */
						   cl_uint src_index,			/* out */
						   cl_char *dst_dclass,			/* out */
						   Datum   *dst_values);		/* out */
DEVICE_FUNCTION(void)
gpupreagg_projection_column(kern_context *kcxt,
							kern_data_store *kds,	/* in */
							kern_data_extra *extra,	/* in */
							cl_uint rowid,			/* in */
							cl_char *dst_dclass,	/* out */
							Datum   *dst_values);	/* out */				
/*
 * GpuPreAgg initial projection
 */
DEVICE_FUNCTION(void)
gpupreagg_setup_row(kern_context *kcxt,
					kern_gpupreagg *kgpreagg,
					kern_data_store *kds_src,		/* in: KDS_FORMAT_ROW */
					kern_data_store *kds_slot);		/* out: KDS_FORMAT_SLOT */
DEVICE_FUNCTION(void)
gpupreagg_setup_block(kern_context *kcxt,
					  kern_gpupreagg *kgpreagg,
					  kern_data_store *kds_src,
					  kern_data_store *kds_slot);
DEVICE_FUNCTION(void)
gpupreagg_setup_arrow(kern_context *kcxt,
					  kern_gpupreagg *kgpreagg,
					  kern_data_store *kds_src,		/* in: KDS_FORMAT_ARROW */
					  kern_data_store *kds_slot);	/* out: KDS_FORMAT_SLOT */
DEVICE_FUNCTION(void)
gpupreagg_setup_column(kern_context *kcxt,
					   kern_gpupreagg *kgpreagg,
					   kern_data_store *kds_src,	/* in: KDS_FORMAT_COLUMN */
					   kern_data_extra *kds_extra,
					   kern_data_store *kds_slot);	/* out: KDS_FORMAT_SLOT */
/*
 * GpuPreAgg reduction functions
 */
DEVICE_FUNCTION(void)
gpupreagg_nogroup_reduction(kern_context *kcxt,
							kern_gpupreagg *kgpreagg,		/* in/out */
							kern_errorbuf *kgjoin_errorbuf,	/* in */
							kern_data_store *kds_slot,		/* in */
							kern_data_store *kds_final,		/* global out */
							cl_char *p_dclass,		/* __private__ */
							Datum   *p_values,		/* __private__ */
							char    *p_extras);		/* __private__ */
DEVICE_FUNCTION(void)
gpupreagg_groupby_reduction(kern_context *kcxt,
							kern_gpupreagg *kgpreagg,		/* in/out */
							kern_errorbuf *kgjoin_errorbuf,	/* in */
							kern_data_store *kds_slot,		/* in */
							kern_data_store *kds_final,		/* shared out */
							kern_global_hashslot *f_hash,	/* shared out */
							preagg_hash_item *l_hitems,		/* __shared__ */
							cl_char    *l_dclass,			/* __shared__ */
							Datum      *l_values,			/* __shared__ */
							char       *l_extras);			/* __shared__ */
#endif /* __CUDACC__ */

/* ----------------------------------------------------------------
 *
 * A thin abstraction layer for atomic functions
 *
 * ---------------------------------------------------------------- */
#ifdef __CUDACC__
#define AGGCALC_INIT_TEMPLATE(NAME,INIT_VALUE)							\
	STATIC_INLINE(void)													\
	aggcalc_init_##NAME(cl_char *p_accum_dclass,						\
						Datum   *p_accum_datum)							\
	{																	\
		*p_accum_dclass = DATUM_CLASS__NULL;							\
		*p_accum_datum = (INIT_VALUE);									\
	}

AGGCALC_INIT_TEMPLATE(null, 0)
AGGCALC_INIT_TEMPLATE(min_int, (Datum)INT_MAX)
AGGCALC_INIT_TEMPLATE(max_int, (Datum)INT_MIN)
AGGCALC_INIT_TEMPLATE(add_int,  0)
AGGCALC_INIT_TEMPLATE(min_long, (Datum)LONG_MAX)
AGGCALC_INIT_TEMPLATE(max_long, (Datum)LONG_MIN)
AGGCALC_INIT_TEMPLATE(add_long, 0)
AGGCALC_INIT_TEMPLATE(min_float, __float_as_int(FLT_MAX))
AGGCALC_INIT_TEMPLATE(max_float, __float_as_int(-FLT_MAX))
AGGCALC_INIT_TEMPLATE(add_float, __float_as_int(0.0))
AGGCALC_INIT_TEMPLATE(min_double, __double_as_longlong(DBL_MAX))
AGGCALC_INIT_TEMPLATE(max_double, __double_as_longlong(-DBL_MAX))
AGGCALC_INIT_TEMPLATE(add_double, __double_as_longlong(0.0))
#undef AGGCALC_INIT_TEMPLATE

STATIC_INLINE(void)
aggcalc_normal_min_int(cl_char *p_accum_dclass,
					   Datum   *p_accum_datum,
					   cl_char  newval_dclass,
					   Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		cl_int		newval_int = (cl_int)(newval_datum & 0xffffffffU);

		*((cl_int *)p_accum_datum) = Min(*((cl_int *)p_accum_datum),
										 newval_int);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_normal_max_int(cl_char *p_accum_dclass,
					   Datum   *p_accum_datum,
					   cl_char  newval_dclass,
					   Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		cl_int		newval_int = (cl_int)(newval_datum & 0xffffffffU);

		*((cl_int *)p_accum_datum) = Max(*((cl_int *)p_accum_datum),
										 newval_int);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}


STATIC_INLINE(void)
aggcalc_normal_add_int(cl_char *p_accum_dclass,
					   Datum   *p_accum_datum,
					   cl_char  newval_dclass,
					   Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		*((cl_int *)p_accum_datum) += (cl_int)(newval_datum & 0xffffffff);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_normal_min_long(cl_char *p_accum_dclass,
						Datum   *p_accum_datum,
						cl_char  newval_dclass,
						Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		*((cl_long *)p_accum_datum) = Min(*((cl_long *)p_accum_datum),
										  (cl_long)newval_datum);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_normal_max_long(cl_char *p_accum_dclass,
						Datum   *p_accum_datum,
						cl_char  newval_dclass,
						Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		*((cl_long *)p_accum_datum) = Max(*((cl_long *)p_accum_datum),
										  (cl_long)newval_datum);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_normal_add_long(cl_char *p_accum_dclass,
						Datum   *p_accum_datum,
						cl_char  newval_dclass,
						Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		*((cl_long *)p_accum_datum) += (cl_long)newval_datum;
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_normal_min_float(cl_char *p_accum_dclass,
						 Datum   *p_accum_datum,
						 cl_char  newval_dclass,
						 Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		*((cl_float *)p_accum_datum)
			= Min(*((cl_float *)p_accum_datum),
				  __int_as_float(newval_datum & 0xffffffff));
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_normal_max_float(cl_char *p_accum_dclass,
						 Datum   *p_accum_datum,
						 cl_char  newval_dclass,
						 Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		*((cl_float *)p_accum_datum)
			= Max(*((cl_float *)p_accum_datum),
				  __int_as_float(newval_datum & 0xffffffff));
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_normal_add_float(cl_char *p_accum_dclass,
						 Datum   *p_accum_datum,
						 cl_char  newval_dclass,
						 Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		*((cl_float *)p_accum_datum)
			+= __int_as_float(newval_datum & 0xffffffff);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_normal_min_double(cl_char *p_accum_dclass,
						  Datum   *p_accum_datum,
						  cl_char  newval_dclass,
						  Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		*((cl_double *)p_accum_datum)
			= Min(*((cl_double *)p_accum_datum),
				  __longlong_as_double((cl_ulong)newval_datum));
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_normal_max_double(cl_char *p_accum_dclass,
						  Datum   *p_accum_datum,
						  cl_char  newval_dclass,
						  Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		*((cl_double *)p_accum_datum)
			= Max(*((cl_double *)p_accum_datum),
				  __longlong_as_double((cl_ulong)newval_datum));
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_normal_add_double(cl_char *p_accum_dclass,
						  Datum   *p_accum_datum,
						  cl_char  newval_dclass,
						  Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		*((cl_double *)p_accum_datum)
			+= __longlong_as_double((cl_ulong)newval_datum);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

#define AGGCALC_SHUFFLE_TEMPLATE(name)									\
	STATIC_INLINE(void)													\
	aggcalc_shuffle_##name(cl_char *p_accum_dclass,						\
						   Datum   *p_accum_datum,						\
						   int     lane_id)								\
	{																	\
		cl_char		newval_dclass;										\
		Datum		newval_datum;										\
																		\
		assert(__activemask() == ~0U);									\
		newval_dclass = __shfl_sync(__activemask(), *p_accum_dclass, lane_id); \
		newval_datum  = __shfl_sync(__activemask(), *p_accum_datum,  lane_id); \
		aggcalc_normal_##name(p_accum_dclass,							\
							  p_accum_datum,							\
							  newval_dclass,							\
							  newval_datum);							\
	}

AGGCALC_SHUFFLE_TEMPLATE(min_int)
AGGCALC_SHUFFLE_TEMPLATE(max_int)
AGGCALC_SHUFFLE_TEMPLATE(add_int)
AGGCALC_SHUFFLE_TEMPLATE(min_long)
AGGCALC_SHUFFLE_TEMPLATE(max_long)
AGGCALC_SHUFFLE_TEMPLATE(add_long)
AGGCALC_SHUFFLE_TEMPLATE(min_float)
AGGCALC_SHUFFLE_TEMPLATE(max_float)
AGGCALC_SHUFFLE_TEMPLATE(add_float)
AGGCALC_SHUFFLE_TEMPLATE(min_double)
AGGCALC_SHUFFLE_TEMPLATE(max_double)
AGGCALC_SHUFFLE_TEMPLATE(add_double)
#undef AGGCALC_SHUFFLE_TEMPLATE

STATIC_INLINE(void)
aggcalc_merge_min_int(cl_char *p_accum_dclass,
					  Datum   *p_accum_datum,
					  cl_char  newval_dclass,
					  Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		cl_int	newval_int = (cl_int)(newval_datum & 0xffffffffU);

		atomicMin((cl_int *)p_accum_datum, newval_int);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_merge_max_int(cl_char *p_accum_dclass,
					   Datum   *p_accum_datum,
					   cl_char  newval_dclass,
					   Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		cl_int	newval_int = (cl_int)(newval_datum & 0xffffffffU);

		atomicMax((cl_int *)p_accum_datum, newval_int);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_merge_add_int(cl_char *p_accum_dclass,
					  Datum   *p_accum_datum,
					  cl_char  newval_dclass,
					  Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		cl_int		newval_int = (cl_int)(newval_datum & 0xffffffff);

		atomicAdd((cl_int *)p_accum_datum, newval_int);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_merge_min_long(cl_char *p_accum_dclass,
					   Datum   *p_accum_datum,
					   cl_char  newval_dclass,
					   Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		atomicMin((cl_long *)p_accum_datum, (cl_long)newval_datum);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}


STATIC_INLINE(void)
aggcalc_merge_max_long(cl_char *p_accum_dclass,
					   Datum   *p_accum_datum,
					   cl_char  newval_dclass,
					   Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		atomicMax((cl_long *)p_accum_datum, (cl_long)newval_datum);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_merge_add_long(cl_char *p_accum_dclass,
					   Datum   *p_accum_datum,
					   cl_char  newval_dclass,
					   Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		atomicAdd((cl_ulong *)p_accum_datum, (cl_ulong)newval_datum);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_merge_min_float(cl_char *p_accum_dclass,
						 Datum   *p_accum_datum,
						 cl_char  newval_dclass,
						 Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		cl_uint		curval = *((cl_uint *)p_accum_datum);
		cl_uint		newval = (newval_datum & 0xffffffff);
		cl_uint		oldval;

		do {
			oldval = curval;
			if (__int_as_float(oldval) < __int_as_float(newval))
				break;
		} while ((curval = atomicCAS((cl_uint *)p_accum_datum,
									 oldval, newval)) != oldval);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_merge_max_float(cl_char *p_accum_dclass,
						 Datum   *p_accum_datum,
						 cl_char  newval_dclass,
						 Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		cl_uint		curval = *((cl_uint *)p_accum_datum);
		cl_uint		newval = (newval_datum & 0xffffffff);
		cl_uint		oldval;

		do {
			oldval = curval;
			if (__int_as_float(oldval) > __int_as_float(newval))
				break;
		} while ((curval = atomicCAS((cl_uint *)p_accum_datum,
									 oldval, newval)) != oldval);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_merge_add_float(cl_char *p_accum_dclass,
						 Datum   *p_accum_datum,
						 cl_char  newval_dclass,
						 Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		atomicAdd((cl_float *)p_accum_datum,
				  __int_as_float(newval_datum & 0xffffffff));
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_merge_min_double(cl_char *p_accum_dclass,
						  Datum   *p_accum_datum,
						  cl_char  newval_dclass,
						  Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		cl_ulong	curval = *((cl_ulong *)p_accum_datum);
		cl_ulong	newval = (cl_ulong)newval_datum;
		cl_ulong	oldval;

		do {
			oldval = curval;
			if (__longlong_as_double(oldval) < __longlong_as_double(newval))
				break;
		} while ((curval = atomicCAS((cl_ulong *)p_accum_datum,
									 oldval, newval)) != oldval);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_merge_max_double(cl_char *p_accum_dclass,
						  Datum   *p_accum_datum,
						  cl_char  newval_dclass,
						  Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		cl_ulong	curval = *((cl_ulong *)p_accum_datum);
		cl_ulong	newval = (cl_ulong)newval_datum;
		cl_ulong	oldval;

		do {
			oldval = curval;
			if (__longlong_as_double(oldval) > __longlong_as_double(newval))
				break;
		} while ((curval = atomicCAS((cl_ulong *)p_accum_datum,
									 oldval, newval)) != oldval);
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

STATIC_INLINE(void)
aggcalc_merge_add_double(cl_char *p_accum_dclass,
						  Datum   *p_accum_datum,
						  cl_char  newval_dclass,
						  Datum    newval_datum)
{
	if (newval_dclass == DATUM_CLASS__NORMAL)
	{
		atomicAdd((cl_double *)p_accum_datum,
				  __longlong_as_double(newval_datum));
		*p_accum_dclass = DATUM_CLASS__NORMAL;
	}
	else
		assert(newval_dclass == DATUM_CLASS__NULL);
}

#define aggcalc_update_min_int(a,b,c,d)		aggcalc_merge_min_int((a),(b),(c),(d))
#define aggcalc_update_max_int(a,b,c,d)		aggcalc_merge_max_int((a),(b),(c),(d))
#define aggcalc_update_add_int(a,b,c,d)		aggcalc_merge_add_int((a),(b),(c),(d))
#define aggcalc_update_min_long(a,b,c,d)	aggcalc_merge_min_long((a),(b),(c),(d))
#define aggcalc_update_max_long(a,b,c,d)	aggcalc_merge_max_long((a),(b),(c),(d))
#define aggcalc_update_add_long(a,b,c,d)	aggcalc_merge_add_long((a),(b),(c),(d))
#define aggcalc_update_min_float(a,b,c,d)	aggcalc_merge_min_float((a),(b),(c),(d))
#define aggcalc_update_max_float(a,b,c,d)	aggcalc_merge_max_float((a),(b),(c),(d))
#define aggcalc_update_add_float(a,b,c,d)	aggcalc_merge_add_float((a),(b),(c),(d))
#define aggcalc_update_min_double(a,b,c,d)	aggcalc_merge_min_double((a),(b),(c),(d))
#define aggcalc_update_max_double(a,b,c,d)	aggcalc_merge_max_double((a),(b),(c),(d))
#define aggcalc_update_add_double(a,b,c,d)	aggcalc_merge_add_double((a),(b),(c),(d))

/*
 * aggcalc operations for hyper-log-log support
 */
DEVICE_FUNCTION(void)
aggcalc_init_hll_sketch(cl_char *p_accum_dclass,
						Datum   *p_accum_datum,
						char    *extra_buffer);
DEVICE_FUNCTION(void)
aggcalc_shuffle_hll_sketch(cl_char *p_accum_dclass,
						   Datum   *p_accum_datum,
						   int      lane_id);
DEVICE_FUNCTION(void)
aggcalc_normal_hll_sketch(cl_char *p_accum_dclass,
						  Datum   *p_accum_datum,
						  cl_char  newval_dclass,
						  Datum    newval_datum);
DEVICE_FUNCTION(void)
aggcalc_merge_hll_sketch(cl_char *p_accum_dclass,
						 Datum   *p_accum_datum,
						 cl_char  newval_dclass,
						 Datum    newval_datum);
DEVICE_FUNCTION(void)
aggcalc_update_hll_sketch(cl_char *p_accum_dclass,
						  Datum   *p_accum_datum,
						  cl_char  newval_dclass,
						  Datum    newval_datum);

#endif	/* __CUDACC__ */

#ifdef __CUDACC_RTC__
/*
 * GPU kernel entrypoint - valid only NVRTC
 */
KERNEL_FUNCTION(void)
kern_gpupreagg_setup_row(kern_gpupreagg *kgpreagg,
						 kern_data_store *kds_src,
						 kern_data_extra *__always_null__,
						 kern_data_store *kds_slot)
{
	kern_parambuf *kparams = KERN_GPUPREAGG_PARAMBUF(kgpreagg);
	DECL_KERNEL_CONTEXT(u);

	INIT_KERNEL_CONTEXT(&u.kcxt, kparams);
	gpupreagg_setup_row(&u.kcxt, kgpreagg, kds_src, kds_slot);
	kern_writeback_error_status(&kgpreagg->kerror, &u.kcxt);
}

KERNEL_FUNCTION(void)
kern_gpupreagg_setup_block(kern_gpupreagg *kgpreagg,
						   kern_data_store *kds_src,
						   kern_data_extra *__always_null__,
						   kern_data_store *kds_slot)
{
	kern_parambuf *kparams = KERN_GPUPREAGG_PARAMBUF(kgpreagg);
	DECL_KERNEL_CONTEXT(u);

	INIT_KERNEL_CONTEXT(&u.kcxt, kparams);
	gpupreagg_setup_block(&u.kcxt, kgpreagg, kds_src, kds_slot);
	kern_writeback_error_status(&kgpreagg->kerror, &u.kcxt);
}

KERNEL_FUNCTION(void)
kern_gpupreagg_setup_arrow(kern_gpupreagg *kgpreagg,
						   kern_data_store *kds_src,
						   kern_data_extra *__always_null__,
						   kern_data_store *kds_slot)
{
	kern_parambuf *kparams = KERN_GPUPREAGG_PARAMBUF(kgpreagg);
	DECL_KERNEL_CONTEXT(u);

	INIT_KERNEL_CONTEXT(&u.kcxt, kparams);
	gpupreagg_setup_arrow(&u.kcxt, kgpreagg, kds_src, kds_slot);
	kern_writeback_error_status(&kgpreagg->kerror, &u.kcxt);
}

KERNEL_FUNCTION(void)
kern_gpupreagg_setup_column(kern_gpupreagg *kgpreagg,
							kern_data_store *kds_src,
							kern_data_extra *kds_extra,
							kern_data_store *kds_slot)
{
	kern_parambuf *kparams = KERN_GPUPREAGG_PARAMBUF(kgpreagg);
	DECL_KERNEL_CONTEXT(u);

	INIT_KERNEL_CONTEXT(&u.kcxt, kparams);
	gpupreagg_setup_column(&u.kcxt, kgpreagg, kds_src, kds_extra, kds_slot);
	kern_writeback_error_status(&kgpreagg->kerror, &u.kcxt);
}

KERNEL_FUNCTION(void)
kern_gpupreagg_nogroup_reduction(kern_gpupreagg *kgpreagg,
								 kern_errorbuf *kgjoin_errorbuf,
								 kern_data_store *kds_slot,
								 kern_data_store *kds_final,
								 kern_global_hashslot *f_hash)
{
	kern_parambuf *kparams = KERN_GPUPREAGG_PARAMBUF(kgpreagg);
	DECL_KERNEL_CONTEXT(u);
#if __GPUPREAGG_NUM_ACCUM_VALUES > 0
	cl_char		nogroup_p_dclass[__GPUPREAGG_NUM_ACCUM_VALUES];
	Datum		nogroup_p_values[__GPUPREAGG_NUM_ACCUM_VALUES];
#else
#define nogroup_p_dclass	NULL
#define nogroup_p_values	NULL
#endif
#if __GPUPREAGG_ACCUM_EXTRA_BUFSZ > 0
	char		nogroup_p_extras[__GPUPREAGG_ACCUM_EXTRA_BUFSZ]
				__attribute__ ((aligned(16)));
#else
#define nogroup_p_extras	NULL
#endif
	INIT_KERNEL_CONTEXT(&u.kcxt, kparams);
	/*
	 * nogroup reduction has no grouping-key, so GPUPREAGG_NUM_ACCUM_VALUES has
	 * identical to the width of kds_slot/kds_final
	 */
	gpupreagg_nogroup_reduction(&u.kcxt,
								kgpreagg,
								kgjoin_errorbuf,
								kds_slot,
								kds_final,
								nogroup_p_dclass,
								nogroup_p_values,
								nogroup_p_extras);
	kern_writeback_error_status(&kgpreagg->kerror, &u.kcxt);
}

KERNEL_FUNCTION(void)
kern_gpupreagg_groupby_reduction(kern_gpupreagg *kgpreagg,
								 kern_errorbuf *kgjoin_errorbuf,
								 kern_data_store *kds_slot,
								 kern_data_store *kds_final,
								 kern_global_hashslot *f_hash)
{
	kern_parambuf *kparams = KERN_GPUPREAGG_PARAMBUF(kgpreagg);
	DECL_KERNEL_CONTEXT(u);
#if __GPUPREAGG_LOCAL_HASH_NROOMS > 0
	__shared__ preagg_hash_item groupby_l_hitems[__GPUPREAGG_LOCAL_HASH_NROOMS];
#if __GPUPREAGG_NUM_ACCUM_VALUES > 0
	__shared__ cl_char	groupby_l_dclass[__GPUPREAGG_NUM_ACCUM_VALUES *
										 __GPUPREAGG_LOCAL_HASH_NROOMS];
	__shared__ Datum	groupby_l_values[__GPUPREAGG_NUM_ACCUM_VALUES *
										 __GPUPREAGG_LOCAL_HASH_NROOMS];
#else	/* __GPUPREAGG_NUM_ACCUM_VALUES */
#define groupby_l_dclass	NULL
#define groupby_l_values	NULL
#endif	/* __GPUPREAGG_NUM_ACCUM_VALUES */
#if __GPUPREAGG_ACCUM_EXTRA_BUFSZ > 0
	__shared__ char		groupby_l_extras[__GPUPREAGG_ACCUM_EXTRA_BUFSZ *
										 __GPUPREAGG_LOCAL_HASH_NROOMS]
						__attribute__((aligned(16)));
#else	/* __GPUPREAGG_ACCUM_EXTRA_BUFSZ */
#define groupby_l_extras	NULL
#endif	/* __GPUPREAGG_ACCUM_EXTRA_BUFSZ */
#else	/* __GPUPREAGG_LOCAL_HASH_NROOMS */
#define groupby_l_hitems	NULL
#define groupby_l_dclass	NULL
#define groupby_l_values	NULL
#define groupby_l_extras	NULL
#endif	/* __GPUPREAGG_LOCAL_HASH_NROOMS */
	INIT_KERNEL_CONTEXT(&u.kcxt, kparams);

	gpupreagg_groupby_reduction(&u.kcxt,
								kgpreagg,
								kgjoin_errorbuf,
								kds_slot,
								kds_final,
								f_hash,
								groupby_l_hitems,
								groupby_l_dclass,
								groupby_l_values,
								groupby_l_extras);
	kern_writeback_error_status(&kgpreagg->kerror, &u.kcxt);
}

/* public variables */
__device__ cl_int	GPUPREAGG_NUM_ACCUM_VALUES  = __GPUPREAGG_NUM_ACCUM_VALUES;
__device__ cl_int	GPUPREAGG_ACCUM_EXTRA_BUFSZ = __GPUPREAGG_ACCUM_EXTRA_BUFSZ;
__device__ cl_int	GPUPREAGG_LOCAL_HASH_NROOMS = __GPUPREAGG_LOCAL_HASH_NROOMS;
__device__ cl_int	GPUPREAGG_HLL_REGISTER_BITS = __GPUPREAGG_HLL_REGISTER_BITS;

#endif /* __CUDACC_RTC__ */
#endif /* CUDA_GPUPREAGG_H */
