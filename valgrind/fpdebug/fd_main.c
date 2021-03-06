
/*--------------------------------------------------------------------*/
/*--- FpDebug: Floating-point arithmetic debugger	     fd_main.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of FpDebug, a heavyweight Valgrind tool for
   detecting floating-point accuracy problems.

   Copyright (C) 2010-2011 Florian Benz 
      florianbenz1@gmail.com

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

/* 
   The first function called by Valgrind is fd_pre_clo_init.

   For each super block (similar to a basic block) Valgrind
   calls fd_instrument and here the instrumentation is done.
   This means that instructions needed for the analysis are added.

   The function fd_instrument does not add instructions itself
   but calls functions named instrumentX where X stands for the
   operation for which instructions should be added.

   For instance, the instructions for analysis a binary operation
   are added in instrumentBinOp. Bascially a call to processBinOp
   is added. So each time the client program performs a binary
   floating-point operation, processBinOp is called.
*/ 

#include "pub_tool_basics.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_machine.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_threadstate.h"
#include "pub_tool_stacktrace.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_debuginfo.h"
#include "pub_tool_oset.h"
#include "pub_tool_libcfile.h"
#include "pub_tool_vki.h"
#include "pub_tool_options.h"
#include "pub_tool_xarray.h"
#include "pub_tool_clientstate.h"
#include "pub_tool_redir.h"

#include "fd_include.h"
/* for client requests */
#include "fpdebug.h"

#include "opToString.c"

#define mkU1(_n)							IRExpr_Const(IRConst_U1(_n))
#define mkU32(_n)                			IRExpr_Const(IRConst_U32(_n))
#define mkU64(_n)                			IRExpr_Const(IRConst_U64(_n))

#define	MAX_STAGES							100
#define	MAX_TEMPS							1000
#define	MAX_REGISTERS						1000
#define	CANCEL_LIMIT						10
#define TMP_COUNT							4
#define CONST_COUNT   						4

/* 10,000 entries -> ~6 MB file */
#define MAX_ENTRIES_PER_FILE				10000
#define MAX_LEVEL_OF_GRAPH					10
#define MAX_DUMPED_GRAPHS					10

#define MPFR_BUFSIZE						100
#define FORMATBUF_SIZE						256
#define DESCRIPTION_SIZE					256
#define FILENAME_SIZE						256
#define FWRITE_BUFSIZE 						32000
#define FWRITE_THROUGH 						10000

#define PSO_SIZE							10000
#define PSO_INFLATION_THRESHOLD				(1.0e6)
#define PSO_OV_ZERO_BOUND					(1e-9)
#define PSO_SV_ZERO_BOUND					(1e-15)
#define PSO_PERCENTIGE_THRESHOLD			(0.7)
#define PSO_FALSEPOSITIVE_PERCENTAGE		(0.1)

/* standard rounding mode: round to nearest */
static mpfr_rnd_t 	STD_RND 				= MPFR_RNDN;

/* precision for float: 24, double: 53*/
static mpfr_prec_t	clo_precision 			= 120;
static Bool 		clo_computeMeanValue 	= True;
static Bool 		clo_ignoreLibraries 	= False;
static Bool 		clo_ignoreAccurate 		= True;
static Bool			clo_simulateOriginal	= False;
static Bool			clo_analyze				= True;
static Bool			clo_bad_cancellations	= True;
static Bool			clo_ignore_end			= False;
static Bool    		clo_error_localization  = False;
static Bool  		clo_print_every_error   = False;
static Bool         clo_detect_pso			= False;
static Bool 		clo_goto_shadow_branch	= False;
static Bool 		clo_track_int			= False;

static UInt activeStages 					= 0;
static ULong sbExecuted 					= 0;
static ULong fpOps 							= 0;
static Int fwrite_pos						= -1;
static Int fwrite_fd 						= -1;

static ULong sbCounter 						= 0;
static ULong totalIns 						= 0;
static UInt getCount 						= 0;
static UInt getsIgnored 					= 0;
static UInt storeCount 						= 0;
static UInt storesIgnored 					= 0;
static UInt loadCount 						= 0;
static UInt loadsIgnored					= 0;
static UInt putCount 						= 0;
static UInt putsIgnored 					= 0;
static UInt maxTemps 						= 0;

static Bool fd_process_cmd_line_option(Char* arg) {
	if VG_BINT_CLO(arg, "--precision", clo_precision, MPFR_PREC_MIN, MPFR_PREC_MAX) {}
	else if VG_BOOL_CLO(arg, "--mean-error", clo_computeMeanValue) {}
	else if VG_BOOL_CLO(arg, "--ignore-libraries", clo_ignoreLibraries) {}
	else if VG_BOOL_CLO(arg, "--ignore-accurate", clo_ignoreAccurate) {}
	else if VG_BOOL_CLO(arg, "--sim-original", clo_simulateOriginal) {}
	else if VG_BOOL_CLO(arg, "--analyze-all", clo_analyze) {}
    else if VG_BOOL_CLO(arg, "--ignore-end", clo_ignore_end) {}
    else if VG_BOOL_CLO(arg, "--error-localization", clo_error_localization) {}
    else if VG_BOOL_CLO(arg, "--print-every-error", clo_print_every_error) {}
    else if VG_BOOL_CLO(arg, "--detect-pso", clo_detect_pso) {}
    else if VG_BOOL_CLO(arg, "--goto-shadow-branch", clo_goto_shadow_branch) {}
    else if VG_BOOL_CLO(arg, "--track-int", clo_track_int) {}
	else 
		return False;
   
	return True;
}

static void fd_print_usage(void) {  
	VG_(printf)(
"    --precision=<number>      the precision of the shadow values [120]\n"
"    --mean-error=no|yes       compute mean and max error for each operation [yes]\n"
"    --ignore-libraries=no|yes libraries are not analyzed [no]\n"
"    --ignore-accurate=no|yes  do not show variables/lines without errors [yes]\n"
"    --sim-original=no|yes     simulate original precision [no]\n"
"    --analyze-all=no|yes      analyze everything [yes]\n"
"    --ignore-end=no|yes       ignore end requests [no]\n"
"    --error-localization=no|yes print large error and its location [no]\n"
"    --print-every-error=no|yes  print the error of every statement [no]\n"
"    --detect-pso=no|yes	   detect and fix precision-specific operations [no]\n"
"    --goto-shadow-branch=no|yes choose branch according to shadow vlaue (high-precision) [no]\n"
"    --track-int=no|yes		   continue track the shadow value for integers [no]\n"
	);
}

static void fd_print_debug_usage(void) {  
	VG_(printf)("    (none)\n");
}

/* hash tables for maping the addresses of the original floating-point values 
   to the shadow values */
static VgHashTable globalMemory 	= NULL;
static VgHashTable meanValues 		= NULL;
static OSet* originAddrSet 			= NULL;
static OSet* unsupportedOps			= NULL;

static Store* 			storeArgs 	= NULL;
static Mux0X* 			muxArgs 	= NULL;
static UnOp* 			unOpArgs 	= NULL;
static BinOp* 			binOpArgs 	= NULL;
static TriOp* 			triOpArgs 	= NULL;
static CircularRegs* 	circRegs	= NULL;

static ShadowValue* 	threadRegisters[VG_N_THREADS][MAX_REGISTERS];
static ShadowValue* 	localTemps[MAX_TEMPS];
static ShadowTmp* 		sTmp[TMP_COUNT];
static ShadowConst* 	sConst[CONST_COUNT];
static Stage* 			stages[MAX_STAGES];
static StageReport*		stageReports[MAX_STAGES];

static Char 			formatBuf[FORMATBUF_SIZE]; 
static Char 			description[DESCRIPTION_SIZE];
static Char 			filename[FILENAME_SIZE];
static Char 			fwrite_buf[FWRITE_BUFSIZE];

static mpfr_t meanOrg, meanRelError;
static mpfr_t stageOrg, stageDiff, stageRelError;
static mpfr_t dumpGraphOrg, dumpGraphRel, dumpGraphDiff, dumpGraphMeanError, dumpGraphErr1, dumpGraphErr2;
static mpfr_t endAnalysisOrg, endAnalysisRelError;
static mpfr_t introMaxError, introErr1, introErr2;
static mpfr_t compareIntroErr1, compareIntroErr2;
static mpfr_t writeSvOrg, writeSvDiff, writeSvRelError;
static mpfr_t cancelTemp;
static mpfr_t arg1tmpX, arg2tmpX, arg3tmpX;
static mpfr_t arg1midX, arg2midX, arg3midX;
static mpfr_t arg1oriX, arg2oriX, arg3oriX;

/* Detecting precision-specific operations*/
static VgHashTable errorMap			= NULL;
static VgHashTable detectedPSO		= NULL;
static Bool findFirstPSO			= False;
static Bool finishPSO				= False;
static Int defaultEmin				= 0;
static Int defaultEmax				= 0;

static Char* mpfrToStringShort(Char* str, mpfr_t* fp) {
	if (mpfr_cmp_ui(*fp, 0) == 0) {
		str[0] = '0'; str[1] = '\0';
		return str;
	}

	Int sgn = mpfr_sgn(*fp);
	if (sgn >= 0) {
		str[0] = ' '; str[1] = '0'; str[2] = '\0';
	} else {
		str[0] = '-'; str[1] = '\0';
	}

	Char mpfr_str[4]; /* digits + 1 */
	mpfr_exp_t exp;
	/* digits_base10 = log10 ( 2^(significant bits) ) */
	mpfr_get_str(mpfr_str, &exp, /* base */ 10, 3, *fp, STD_RND);
	exp--;
	VG_(strcat)(str, mpfr_str);

	if (sgn >= 0) {
		str[1] = str[2];
		str[2] = '.';
	} else {
		str[1] = str[2];
		str[2] = '.';
	}

	Char exp_str[10];
	VG_(sprintf)(exp_str, " * 10^%ld", exp);
	VG_(strcat)(str, exp_str);
	return str;
}

static Char* mpfrToString(Char* str, mpfr_t* fp) {
	Int sgn = mpfr_sgn(*fp);
	if (sgn >= 0) {
		str[0] = ' '; str[1] = '0'; str[2] = '\0';
	} else {
		str[0] = '-'; str[1] = '\0';
	}

	Char mpfr_str[100]; /* digits + 1 */
	mpfr_exp_t exp;
	/* digits_base10 = log10 ( 2^(significant bits) ) */
	mpfr_get_str(mpfr_str, &exp, /* base */ 10, /* digits, float: 7, double: 15 */ 60, *fp, STD_RND);
	//mpfr_get_str(mpfr_str, &exp, /* base */ 10, /* digits, float: 7, double: 15 */ 60, *fp, STD_RND);
	exp--;
	VG_(strcat)(str, mpfr_str);

	if (sgn >= 0) {
		str[1] = str[2];
		str[2] = '.';
	} else {
		str[1] = str[2];
		str[2] = '.';
	}

	Char exp_str[50];
	VG_(sprintf)(exp_str, " * 10^%ld", exp);
	VG_(strcat)(str, exp_str);

	mpfr_prec_t pre_min = mpfr_min_prec(*fp);
	mpfr_prec_t pre = mpfr_get_prec(*fp);
	Char pre_str[50];
	VG_(sprintf)(pre_str, ", %ld/%ld bit", pre_min, pre);
	VG_(strcat)(str, pre_str);
	return str;
}

static Char* mpfrToStringE(Char* str, mpfr_t* fp) {
	Int sgn = mpfr_sgn(*fp);
	if (sgn >= 0) {
		str[0] = ' '; str[1] = '0'; str[2] = '\0';
	} else {
		str[0] = '-'; str[1] = '\0';
	}

	Char mpfr_str[100]; /* digits + 1 */
	mpfr_exp_t exp;
	/* digits_base10 = log10 ( 2^(significant bits) ) */
	mpfr_get_str(mpfr_str, &exp, /* base */ 10, /* digits, float: 7, double: 15 */ 60, *fp, STD_RND);
	//mpfr_get_str(mpfr_str, &exp, /* base */ 10, /* digits, float: 7, double: 15 */ 60, *fp, STD_RND);
	exp--;
	VG_(strcat)(str, mpfr_str);

	if (sgn >= 0) {
		str[1] = str[2];
		str[2] = '.';
	} else {
		str[1] = str[2];
		str[2] = '.';
	}

	Char exp_str[50];
	VG_(sprintf)(exp_str, "e%ld", exp);
	VG_(strcat)(str, exp_str);
	return str;
}

static Bool ignoreFile(Char* desc) {
	if (!clo_ignoreLibraries) {
		return False;
	}
	/* simple patern matching - only for one short pattern */
	Char* pattern = ".so";
	Int pi = 0;
	Int i = 0;
	while (desc[i] != '\0' && i < 256) {
		if (desc[i] == pattern[pi]) {
			pi++;
		} else {
			pi = 0;
		}
		if (pattern[pi] == '\0') return True;
		i++;
	}
	return False;
}

static Bool isInLibrary(Addr64 addr) {
	DebugInfo* dinfo = VG_(find_DebugInfo)((Addr)addr);
	if (!dinfo) return False; /* be save if not sure */

	const UChar* soname = VG_(DebugInfo_get_soname)(dinfo);
	tl_assert(soname);
	if (0) VG_(printf)("%s\n", soname);

	return ignoreFile(soname);
}

static __inline__
mpfr_exp_t maxExp(mpfr_exp_t x, mpfr_exp_t y) {
	if (x > y) {
		return x;
	} else {
		return y;
	}
}

static __inline__
mpfr_exp_t getCanceledBits(mpfr_t* res, mpfr_t* arg1, mpfr_t* arg2) {
	/* consider zero, NaN and infinity */
	if (mpfr_regular_p(*arg1) == 0 || mpfr_regular_p(*arg2) == 0 || mpfr_regular_p(*res) == 0) {
		return 0;
	}

	mpfr_exp_t resExp = mpfr_get_exp(*res);
	mpfr_exp_t arg1Exp = mpfr_get_exp(*arg1);
	mpfr_exp_t arg2Exp = mpfr_get_exp(*arg2);

	mpfr_exp_t max = maxExp(arg1Exp, arg2Exp);
	if (resExp < max) {
		mpfr_exp_t diff = max - resExp;
		if (diff < 0) {
			return -diff;
		}
		return diff;
	}
	return 0;
}

static ULong avMallocs = 0;
static ULong avFrees = 0;

static __inline__
ShadowValue* initShadowValue(UWord key) {
	ShadowValue* sv = VG_(malloc)("fd.initShadowValue.1", sizeof(ShadowValue));
	sv->key = key;
	sv->active = True;
	sv->version = 0;
	sv->opCount = 0;
	sv->origin = 0;
	sv->cancelOrigin = 0;
	sv->orgType = Ot_INVALID;

	mpfr_init(sv->value);
	mpfr_init(sv->midValue);
	mpfr_init(sv->oriValue);

	avMallocs++;
	return sv;
}

static __inline__
void freeShadowValue(ShadowValue* sv, Bool freeSvItself) {
	tl_assert(sv != NULL);

	mpfr_clear(sv->value);
	mpfr_clear(sv->midValue);
	mpfr_clear(sv->oriValue);
	if (freeSvItself) {
		VG_(free)(sv);
	}
	avFrees++;
}

static __inline__
void copyShadowValue(ShadowValue* newSv, ShadowValue* sv) {
	tl_assert(newSv != NULL && sv != NULL);

	if (clo_simulateOriginal) {
		mpfr_set_prec(newSv->value, mpfr_get_prec(sv->value));
		mpfr_set_prec(newSv->midValue, mpfr_get_prec(sv->midValue));
		mpfr_set_prec(newSv->oriValue, mpfr_get_prec(sv->oriValue));
	}

	mpfr_set(newSv->value, sv->value, STD_RND);
	newSv->opCount = sv->opCount;
	newSv->origin = sv->origin;
	newSv->canceled = sv->canceled;
	newSv->cancelOrigin = sv->cancelOrigin;
	// newSv->orgType = Ot_INVALID;
	newSv->orgType = sv->orgType; // added by ran
	newSv->Org.db = sv->Org.db; // added by ran
	mpfr_set(newSv->midValue, sv->midValue, STD_RND);
	mpfr_set(newSv->oriValue, sv->oriValue, STD_RND);

	/* Do not overwrite active or version!
	   They should be set before. */
}

static __inline__
ShadowValue* getTemp(IRTemp tmp) {
	tl_assert(tmp >= 0 && tmp < MAX_TEMPS);

	if (localTemps[tmp] && localTemps[tmp]->version == sbExecuted) {
		return localTemps[tmp];
	} else {
		return NULL;
	}
}

static __inline__
ShadowValue* setTemp(IRTemp tmp) {
	tl_assert(tmp >= 0 && tmp < MAX_TEMPS);

	if (localTemps[tmp]) {
		localTemps[tmp]->active = True;
	} else {
		localTemps[tmp] = initShadowValue((UWord)tmp);
	}
	localTemps[tmp]->version = sbExecuted;

	return localTemps[tmp];
}

static void updateMeanValue(UWord key, IROp op, mpfr_t* shadow, mpfr_exp_t canceled, Addr arg1, Addr arg2, UInt cancellationBadness) {
	if (mpfr_cmp_ui(meanOrg, 0) != 0 || mpfr_cmp_ui(*shadow, 0) != 0) {
		mpfr_reldiff(meanRelError, *shadow, meanOrg, STD_RND);
		mpfr_abs(meanRelError, meanRelError, STD_RND);
	} else {
		mpfr_set_ui(meanRelError, 0, STD_RND);
	}

	MeanValue* val = VG_(HT_lookup)(meanValues, key);
	if (val == NULL) {
		val = VG_(malloc)("fd.updateMeanValue.1", sizeof(MeanValue));
		val->key = key;
		val->op = op;
		val->count = 1;
		val->visited = False;
		val->overflow = False;
		mpfr_init_set(val->sum, meanRelError, STD_RND);
		mpfr_init_set(val->max, meanRelError, STD_RND);
		val->canceledSum = canceled;
		val->canceledMax = canceled;
		val->cancellationBadnessSum = cancellationBadness;
		val->cancellationBadnessMax = cancellationBadness;
		VG_(HT_add_node)(meanValues, val);
		val->arg1 = arg1;
		val->arg2 = arg2;
	} else {
		val->count++;
		mpfr_add(val->sum, val->sum, meanRelError, STD_RND);

		mpfr_exp_t oldSum = val->canceledSum;
		val->canceledSum += canceled;
		/* check for overflow */
		if (oldSum > val->canceledSum) {
			val->overflow = True;
		}

		val->cancellationBadnessSum += cancellationBadness;

		if (mpfr_cmp(meanRelError, val->max) > 0) {
			mpfr_set(val->max, meanRelError, STD_RND);
			val->arg1 = arg1;
			val->arg2 = arg2;
		}

		if (canceled > val->canceledMax) {
			val->canceledMax = canceled;
		}

		if (cancellationBadness > val->cancellationBadnessMax) {
			val->cancellationBadnessMax = cancellationBadness;
		}
	}
}

static void stageClearVals(VgHashTable t) {
	if (t == NULL) {
		return;
	}

	VG_(HT_ResetIter)(t);
	StageValue* next;
	while (next = VG_(HT_Next)(t)) {
		mpfr_clears(next->val, next->relError, NULL);
	}
	VG_(HT_destruct)(t);
}

static void stageStart(Int num) {
	tl_assert(num < MAX_STAGES);
	
	if (stages[num]) {
		tl_assert(!stages[num]->active);
		stages[num]->active = True;
		stages[num]->count++;
	} else {
		stages[num] = VG_(malloc)("fd.stageStart.1", sizeof(Stage));
		stages[num]->active = True;
		stages[num]->count = 1;
		stages[num]->oldVals = NULL;
		stages[num]->limits = VG_(HT_construct)("Stage limits");
	}
	stages[num]->newVals = VG_(HT_construct)("Stage values");
	activeStages++;
}

static void stageEnd(Int num) {
	tl_assert(stages[num]);
	tl_assert(stages[num]->active);

	Int mateCount = -1;
	Int newNodes = 0;
	Int oldNodes = 0;

	if (stages[num]->newVals && stages[num]->oldVals) {
		mateCount = 0;

		VG_(HT_ResetIter)(stages[num]->newVals);
		StageValue* next;
		StageValue* mate;

		while (next = VG_(HT_Next)(stages[num]->newVals)) {
			mate = VG_(HT_lookup)(stages[num]->oldVals, next->key);
			if (!mate) {
				VG_(dmsg)("no mate: %d\n", num);
				continue;
			}

			mateCount++;
			StageLimit* sl = VG_(HT_lookup)(stages[num]->limits, next->key);

			mpfr_sub(stageDiff, mate->relError, next->relError, STD_RND);
			mpfr_abs(stageDiff, stageDiff, STD_RND);

			Char mpfrBuf[MPFR_BUFSIZE];
			if (sl) {
				if (mpfr_cmp(stageDiff, sl->limit) > 0) {
					mpfrToString(mpfrBuf, &(sl->limit));
					mpfrToString(mpfrBuf, &(stageDiff));

					/* adjust limit for the following iterations */
					mpfr_set(sl->limit, stageDiff, STD_RND);

					/* create stage report */
					StageReport* report = NULL;
					if (stageReports[num]) {
						report = VG_(HT_lookup)(stageReports[num], next->key);
					} else {
						stageReports[num] = VG_(HT_construct)("Stage reports");
					}
					if (report) {
						report->count++;
						report->iterMax = stages[num]->count;
					} else {
						report = VG_(malloc)("fd.stageEnd.1", sizeof(StageReport));
						report->key = next->key;
						report->count = 1;
						report->iterMin = stages[num]->count;
						report->iterMax = stages[num]->count;
						report->origin = 0;
						ShadowValue* sv = VG_(HT_lookup)(globalMemory, next->key);
						if (sv) {
							report->origin = sv->origin;
						}
						VG_(HT_add_node)(stageReports[num], report);
					}
				}
			} else {
				sl = VG_(malloc)("fd.stageEnd.1", sizeof(StageLimit));
				sl->key = next->key;
				mpfr_init_set(sl->limit, stageDiff, STD_RND);
				VG_(HT_add_node)(stages[num]->limits, sl);
			}
		}
	}

	stages[num]->active = False;
	stageClearVals(stages[num]->oldVals);
	stages[num]->oldVals = stages[num]->newVals;
	stages[num]->newVals = NULL;
	activeStages--;
}

static void updateStages(Addr addr, Bool isFloat) {
	if (isFloat) {
		Float f = *(Float*)addr;
		mpfr_set_flt(stageOrg, f, STD_RND);
	} else {
		Double d = *(Double*)addr;
		mpfr_set_d(stageOrg, d, STD_RND);
	}
	ShadowValue* svalue = VG_(HT_lookup)(globalMemory, addr);

	if (svalue && svalue->active) {
		mpfr_sub(stageDiff, svalue->value, stageOrg, STD_RND);

		if (mpfr_cmp_ui(svalue->value, 0) != 0 || mpfr_cmp_ui(stageOrg, 0) != 0) {
			mpfr_reldiff(stageRelError, svalue->value, stageOrg, STD_RND);
			mpfr_abs(stageRelError, stageRelError, STD_RND);
		} else {
			mpfr_set_ui(stageRelError, 0, STD_RND);
		}

		Int i;
		for (i = 0; i < MAX_STAGES; i++) {
			if (!stages[i] || !(stages[i]->active) || !(stages[i]->newVals)) {
				continue;
			}

			StageValue* sv = VG_(HT_lookup)(stages[i]->newVals, addr);
			if (sv) {
				if (mpfr_cmpabs(stageRelError, sv->relError) > 0) {
					mpfr_set(sv->val, svalue->value, STD_RND);
					mpfr_set(sv->relError, stageRelError, STD_RND);
				}
			} else {
				sv = VG_(malloc)("fd.updateStages.1", sizeof(StageValue));
				sv->key = addr;
				mpfr_init_set(sv->val, svalue->value, STD_RND);
				mpfr_init_set(sv->relError, stageRelError, STD_RND);
				VG_(HT_add_node)(stages[i]->newVals, sv);
			}
		}
	}
}

static void stageClear(Int num) {
	if (stages[num] == NULL) {
		return;
	}
	stageClearVals(stages[num]->oldVals);
	stageClearVals(stages[num]->newVals);
	if (stages[num]->limits != NULL) {
		VG_(HT_ResetIter)(stages[num]->limits);
		StageLimit* next;
		while ( (next = VG_(HT_Next)(stages[num]->limits)) ) {
			mpfr_clear(next->limit);
		}
		VG_(HT_destruct)(stages[num]->limits);
	}
	stages[num] = NULL;
}

static void writeSConst(IRSB* sb, IRConst* c, Int num) {
	IRConstTag tag = c->tag;

	IRExpr* addr = NULL;
	switch (tag) {
		case Ico_F64:
			addr = mkU64(&(sConst[num]->Val.F64));
			break;
		case Ico_V128:
			addr = mkU64(&(sConst[num]->Val.V128));
			break;
		default:
			break;
	}

	if (addr) {
		IRStmt* store = IRStmt_Store(Iend_LE, mkU64(&(sConst[num]->tag)), mkU32(tag));
		addStmtToIRSB(sb, store);

		IRExpr* expr_const = IRExpr_Const(c);
		store = IRStmt_Store(Iend_LE, addr, expr_const);
		addStmtToIRSB(sb, store);
	} else {
		VG_(tool_panic)("Unhandled case in writeSConst\n");
	}
}

static __inline__
void readSConst(Int num, mpfr_t* fp) {
	Int i;
	ULong v128 = 0;
	Double* db;

	switch (sConst[num]->tag) {
		case Ico_F64:
			mpfr_set_d(*fp, (Double)sConst[num]->Val.F64, STD_RND);
			break;
		case Ico_V128:
			/* 128-bit restricted vector constant with 1 bit (repeated 8 times)
			   for each of the 16 1-byte lanes */
			for (i = 7; i >= 0; i--) {
				if ((sConst[num]->Val.V128 >> (i + 8)) & 1) {
					v128 &= 0xFF;
				}
				v128 <<= 8;
			}
			db = &v128;
			mpfr_set_d(*fp, *db, STD_RND);
			break;
		default:		
			VG_(tool_panic)("Unhandled case in readSConst\n");
			break;
	}
}

static void writeSTemp(IRSB* sb, IRTypeEnv* env, IRTemp tmp, Int num) {
	IRType type = typeOfIRTemp(env, tmp);
	IRExpr* addr = NULL;
	switch (type) {
		case Ity_F32:
			addr = mkU64(&(sTmp[num]->Val.F32));
			break;
		case Ity_F64:
			addr = mkU64(&(sTmp[num]->Val.F64));
			break;
		case Ity_V128:
			addr = mkU64(sTmp[num]->U128);
			break;
		default:
			break;
	}

	if (addr) {
		IRStmt* store = IRStmt_Store(Iend_LE, mkU64(&(sTmp[num]->type)), mkU32(type));
		addStmtToIRSB(sb, store);

		IRExpr* rdTmp = IRExpr_RdTmp(tmp);
		store = IRStmt_Store(Iend_LE, addr, rdTmp);
		addStmtToIRSB(sb, store);
	} else {
		VG_(tool_panic)("Unhandled case in writeSTemp\n");
	}
}

static __inline__
void readSTemp(Int num, mpfr_t* fp) {
	IRType type = sTmp[num]->type;
	switch (type) {
		case Ity_F32:
			if (clo_simulateOriginal) mpfr_set_prec(*fp, 24);
			mpfr_set_flt(*fp, sTmp[num]->Val.F32, STD_RND);
			break;
		case Ity_F64:
			if (clo_simulateOriginal) mpfr_set_prec(*fp, 53);
			mpfr_set_d(*fp, sTmp[num]->Val.F64, STD_RND);
			break;
		case Ity_V128:
			/* Not a general solution, because this does not work if vectors are used 
			   e.g. two/four additions with one SSE instruction */
			if (sTmp[num]->U128[1] == 0) {
				if (clo_simulateOriginal) mpfr_set_prec(*fp, 24);

				Float* flp = &(sTmp[num]->U128[0]);
				mpfr_set_flt(*fp, *flp, STD_RND);
			} else {
				if (clo_simulateOriginal) mpfr_set_prec(*fp, 53);

				ULong ul = sTmp[num]->U128[1];
				ul <<= 32;
				ul |= sTmp[num]->U128[0];
				Double* db = &ul;
				mpfr_set_d(*fp, *db, STD_RND);
			}
			break;
		default:
			VG_(tool_panic)("Unhandled case in readSTemp\n");
			break;
	}
}

static void getFileName(Char* name) {
	Char tempName[256];
	struct vg_stat st;
	int i;
	for (i = 1; i < 100; ++i) {
		VG_(sprintf)(tempName, "%s_%d", name, i);
		SysRes res = VG_(stat)(tempName, &st);
		if (sr_isError(res)) {
			break;
		}
	}
	VG_(sprintf)(name, "%s_%d", name, i);
}

static __inline__
void fwrite_flush(void) {
    if ((fwrite_fd>=0) && (fwrite_pos>0)) {
		VG_(write)(fwrite_fd, (void*)fwrite_buf, fwrite_pos);
	}
    fwrite_pos = 0;
}

static void my_fwrite(Int fd, Char* buf, Int len) {
    if (fwrite_fd != fd) {
		fwrite_flush();
		fwrite_fd = fd;
    }
    if (len > FWRITE_THROUGH) {
		fwrite_flush();
		VG_(write)(fd, (void*)buf, len);
		return;
    }
    if (FWRITE_BUFSIZE - fwrite_pos <= len) {
		fwrite_flush();
	}
    VG_(strncpy)(fwrite_buf + fwrite_pos, buf, len);
    fwrite_pos += len;
}

static void dumpPSO() {
	Char fname[256];
	HChar* clientName = VG_(args_the_exename);
	VG_(sprintf)(fname, "%s_pso.log", clientName);

	getFileName(fname);
	SysRes fileRes = VG_(open)(fname, VKI_O_CREAT|VKI_O_TRUNC|VKI_O_WRONLY, VKI_S_IRUSR|VKI_S_IWUSR);
	if (sr_isError(fileRes)) {
		VG_(umsg)("SHADOW VALUES (%s): Failed to create or open the file!\n", fname);
		return;
	}
	Int file = sr_Res(fileRes);

	VG_(umsg)("Dump PSO into %s\n", fname);
	PSOperation * next;
	VG_(HT_ResetIter)(detectedPSO);
	while (next = VG_(HT_Next)(detectedPSO)) {
		VG_(describe_IP)(next->key, description, DESCRIPTION_SIZE);
		VG_(strcat)(description, "\n");
		my_fwrite(file, (void*)description, VG_(strlen)(description));
	}
	fwrite_flush();
	VG_(close)(file);
}

static Bool isPSOFinished() {
	if (!clo_detect_pso) return True;
	return finishPSO;
}

static void collectPSO() {
	ErrorCount* next;
	VG_(HT_ResetIter)(errorMap);
	while (next = VG_(HT_Next)(errorMap)) {
		if (next->errCnt > next->totalCnt * PSO_PERCENTIGE_THRESHOLD) {
			PSOperation* p = VG_(malloc)("fd.initPSOperation.1", sizeof(PSOperation));
			p->key = next->key;
			p->falsePositive = next->ovCnt * 1.0 / next->totalCnt > PSO_FALSEPOSITIVE_PERCENTAGE ? True : False;
			VG_(HT_add_node)(detectedPSO, p);
			finishPSO = False;
			VG_(describe_IP)(p->key, description, DESCRIPTION_SIZE);
			VG_(umsg)("PSO at 			%s\n", description);
			VG_(umsg)("Total count 		%d\n", next->totalCnt);
			VG_(umsg)("Error count 		%d\n", next->errCnt);
			VG_(umsg)("Negative count   %d\n", next->ovCnt);
		}
	}
}

static void beginOneRun() {
	if (!clo_detect_pso) return;
	VG_(umsg)("One run for PSO detection begin.\n");
	errorMap = VG_(HT_construct)("Error map for detecting precision-specific operations");
	finishPSO = False;
}

static void endOneRun() {
	if (!clo_detect_pso) return;
	finishPSO = True;
	collectPSO();
	UInt n_memory = 0;
	ErrorCount** memory = VG_(HT_to_array)(errorMap, &n_memory);
	VG_(free)(memory);
	VG_(HT_destruct)(errorMap);
	VG_(umsg)("One run for PSO detection end.\n");
	if (finishPSO) {
		UWord temp[PSO_SIZE];
		int tempSize = 0;

		PSOperation* next;
		VG_(HT_ResetIter)(detectedPSO);
		while (next = VG_(HT_Next)(detectedPSO)) {
			if (next->falsePositive) {
				temp[tempSize++] = next->key;
			}
		}

		int i = 0;
		for(; i < tempSize; i++) {
			VG_(umsg)("Remove 0x%llX from precision-specific operations\n", (ULong)temp[i]);
			VG_(HT_remove)(detectedPSO, temp[i]);
		}

		VG_(HT_ResetIter)(detectedPSO);
		while (next = VG_(HT_Next)(detectedPSO)) {
			VG_(umsg)("Probable PSO at 0x%llX \n", (ULong)next->key);
		}

		dumpPSO();
	}
}

static void beginOneInstance() {
	if (!clo_detect_pso) return;
	findFirstPSO = False;
}

static ErrorCount * initErrorCount() {
	ErrorCount * e = VG_(malloc)("fd.initErrorCount.1", sizeof(ErrorCount));
	e->errCnt = 0;
	e->ovCnt = 0;
	e->totalCnt = 0;
	return e;
}

static void checkAndRecover(ShadowValue* svalue) {
	if (svalue) {
		mpfr_t org;
		mpfr_init(org);

		if (svalue->orgType == Ot_FLOAT) {
			mpfr_set_prec(org, 24);
			mpfr_set_flt(org, svalue->Org.fl, STD_RND);
		} else if (svalue->orgType == Ot_DOUBLE) {
			mpfr_set_prec(org, 53);
			mpfr_set_d(org, svalue->Org.db, STD_RND);
		} else {
			tl_assert(False);
		}

		if (mpfr_cmp(org, svalue->oriValue)) {
			VG_(umsg)("There may exists untracked operations! Recovering...\n");
			// Char mpfrBuf[MPFR_BUFSIZE];
			// mpfrToString(mpfrBuf, &org);
			// VG_(umsg)("ORI: %s\n", mpfrBuf);
			// mpfrToString(mpfrBuf, &(svalue->oriValue));
			// VG_(umsg)("SMU: %s\n", mpfrBuf);
			mpfr_set(svalue->value, org, STD_RND);
			mpfr_set(svalue->midValue, org, STD_RND);
			mpfr_set(svalue->oriValue, org, STD_RND);
		}

		mpfr_clear(org);
	}
}

static void computeRelativeError(ShadowValue* svalue, mpfr_t rel) {
	if ((!clo_detect_pso) || finishPSO) return;
	if (svalue) {
		mpfr_t org;
		mpfr_init(org);

		if (svalue->orgType == Ot_FLOAT) {
			mpfr_set_flt(org, svalue->Org.fl, STD_RND);
		} else if (svalue->orgType == Ot_DOUBLE) {
			mpfr_set_d(org, svalue->Org.db, STD_RND);
		} else {
			tl_assert(False);
		}

		if (mpfr_cmp_ui(svalue->value, 0) != 0 || mpfr_cmp_ui(org, 0) != 0) {
			mpfr_reldiff(rel, svalue->value, org, STD_RND);
			mpfr_abs(rel, rel, STD_RND);
		} else {
			mpfr_set_ui(rel, 0, STD_RND);
		}

		mpfr_clear(org);
	} else {
		mpfr_set_ui(rel, 0, STD_RND);
	}
}

static void printErrorShort(ShadowValue* svalue) {
	if (clo_detect_pso || clo_print_every_error || clo_error_localization) {
		if (svalue) {
			mpfr_t org, rel;
			mpfr_inits(org, rel, NULL);

			if (svalue->orgType == Ot_FLOAT) {
				mpfr_set_flt(org, svalue->Org.fl, STD_RND);
			} else if (svalue->orgType == Ot_DOUBLE) {
				mpfr_set_d(org, svalue->Org.db, STD_RND);
			} else {
				tl_assert(False);
			}

			if (mpfr_cmp_ui(svalue->value, 0) != 0 || mpfr_cmp_ui(org, 0) != 0) {
				mpfr_reldiff(rel, svalue->value, org, STD_RND);
				mpfr_abs(rel, rel, STD_RND);
			} else {
				mpfr_set_ui(rel, 0, STD_RND);
			}

			if (clo_detect_pso || clo_print_every_error || mpfr_cmp_d(rel, 1e-10) >= 0) {
				VG_(describe_IP)(svalue->origin, description, DESCRIPTION_SIZE);
				VG_(umsg)("Location: %s\n", description);
				Char mpfrBuf[MPFR_BUFSIZE];
				mpfrToString(mpfrBuf, &org);
				VG_(umsg)("ORIGINAL:         %s\n", mpfrBuf);
				mpfrToString(mpfrBuf, &(svalue->oriValue));
				VG_(umsg)("SIMULATE VALUE:   	 %s\n", mpfrBuf);
				mpfrToString(mpfrBuf, &(svalue->midValue));
				VG_(umsg)("MIDDLE VALUE:   	 %s\n", mpfrBuf);
				mpfrToString(mpfrBuf, &(svalue->value));
				VG_(umsg)("SHADOW VALUE:     %s\n", mpfrBuf);
				mpfrToString(mpfrBuf, &rel);
				VG_(umsg)("RELATIVE ERROR:   %s\n\n", mpfrBuf);
			}
			mpfr_clears(rel, org, NULL);
		} else if (clo_print_every_error) {
			VG_(umsg)("There exists no shadow value.\n");
		}
	}
}

static void analyzePSO(mpfr_t irel, ShadowValue* o) {
	if (findFirstPSO || (!clo_detect_pso) || finishPSO) {
		return;
	}

	// Calculate error inflation
	mpfr_t orel;
	mpfr_init(orel);
	computeRelativeError(o, orel);

	mpfr_t inflation;
	mpfr_init(inflation);
	if (mpfr_cmp_ui(irel, 0) != 0) {
		mpfr_div(inflation, orel, irel, STD_RND);
		mpfr_abs(inflation, inflation, STD_RND);
	} else if (mpfr_cmp_ui(orel, 0) != 0) {
		mpfr_set(inflation, orel, STD_RND);
	} else {
		mpfr_set_ui(inflation, 0, STD_RND);
	}
	// Char mpfrBuf[MPFR_BUFSIZE];
	// mpfrToString(mpfrBuf, &inflation);
	// VG_(umsg)("INFLATION:         %s\n", mpfrBuf);
	// mpfr_t irelCopy;
	// mpfr_init(irelCopy);
	// mpfr_set(irelCopy, irel, STD_RND);
	// mpfrToString(mpfrBuf, &irelCopy);
	// VG_(umsg)("INPUT RELATIVE ERROR:         %s\n", mpfrBuf);
	// mpfrToString(mpfrBuf, &orel);
	// VG_(umsg)("OUTPUT RELATIVE ERROR:         %s\n", mpfrBuf);
	// printErrorShort(o);

	// Get original value.
	mpfr_t org;
	mpfr_init(org);

	if (o->orgType == Ot_FLOAT) {
		mpfr_set_flt(org, o->Org.fl, STD_RND);
	} else if (o->orgType == Ot_DOUBLE) {
		mpfr_set_d(org, o->Org.db, STD_RND);
	} else {
		tl_assert(False);
	}
	mpfr_abs(org, org, STD_RND);

	// Add in maps
	if (VG_(HT_lookup)(detectedPSO, o->origin) != NULL) {
		if (mpfr_cmp_d(inflation, PSO_INFLATION_THRESHOLD) >= 0) {
			// Should not reach here
			// VG_(describe_IP)(o->origin, description, DESCRIPTION_SIZE);
			// VG_(umsg)("Warning: a precision-specific operation is not fixed at %s\n", description);
			// printErrorShort(o);
		}
		mpfr_clears(orel, inflation, org, NULL);
		return;
	}
	mpfr_t temp;
	mpfr_init(temp);
	ErrorCount* cnt = VG_(HT_lookup)(errorMap, o->origin);
	if (cnt == NULL) { 
		cnt = initErrorCount();
		cnt->key = o->origin;
		VG_(HT_add_node)(errorMap, cnt);
	}
	if (mpfr_cmp_d(inflation, PSO_INFLATION_THRESHOLD) >= 0) {
		mpfr_abs(temp, o->value, STD_RND);
		if (mpfr_cmp_d(org, PSO_OV_ZERO_BOUND) < 0 && mpfr_cmp_d(temp, PSO_SV_ZERO_BOUND) < 0) {
			cnt->ovCnt++;
		}
		cnt->errCnt++;
		cnt->totalCnt++;
		findFirstPSO = True;
	} else {
		cnt->totalCnt++;
	}

	mpfr_clears(orel, inflation, org, temp, NULL);
}

static Bool isOpFloat(IROp op) {
	switch (op) {
		/* unary float */
		case Iop_Sqrt32F0x4:
		case Iop_NegF32:
		case Iop_AbsF32:
		/* binary float */
		case Iop_Add32F0x4:
		case Iop_Sub32F0x4:
		case Iop_Mul32F0x4:
		case Iop_Div32F0x4:
		case Iop_Min32F0x4:
		case Iop_Max32F0x4:
			return True;
		/* unary double */
		case Iop_Sqrt64F0x2:
		case Iop_NegF64:
		case Iop_AbsF64:
		/* binary double */
		case Iop_Add64F0x2:
		case Iop_Sub64F0x2:
		case Iop_Mul64F0x2:
		case Iop_Div64F0x2:
		case Iop_Min64F0x2:
		case Iop_Max64F0x2:
		case Iop_CmpF64:
		case Iop_F64toI16S:
		case Iop_F64toI32S:
		case Iop_F64toI64S:
		case Iop_F64toI64U:
		case Iop_F64toI32U:
		/* ternary double */
		case Iop_AddF64:
		case Iop_SubF64:
		case Iop_MulF64:
		case Iop_DivF64:
			return False;
		default:
			VG_(tool_panic)("Unhandled operation in isOpFloat\n");
			return False;
	}
}

static void beginEmulateDouble() {
	mpfr_set_emin(-1073);
	mpfr_set_emax(1024);
}

static void endEmulate() {
	mpfr_set_emin(defaultEmin);
	mpfr_set_emax(defaultEmax);
}

static VG_REGPARM(2) void processUnOp(Addr addr, UWord ca) {
	// Do not analyze unary operation, because they are not precision-specific
	if (!clo_analyze) return;

	Int constArgs = (Int)ca;
	ULong argOpCount = 0;
	Addr argOrigin = 0;
	mpfr_exp_t argCanceled = 0;
	Addr argCancelOrigin = 0;
	// mpfr_t irel;
	// mpfr_init(irel);
 
	if (clo_simulateOriginal) {
		if (isOpFloat(unOpArgs->op)) {
			mpfr_set_prec(arg1tmpX, 24);
		} else {
			mpfr_set_prec(arg1tmpX, 53);
		}
	}

	if (isOpFloat(unOpArgs->op)) {
		mpfr_set_prec(arg1midX, 24);
		mpfr_set_prec(arg1oriX, 24);
	} else {
		mpfr_set_prec(arg1midX, 53);
		mpfr_set_prec(arg1oriX, 53);
	}

	if (constArgs & 0x1) {
		readSConst(0, &(arg1tmpX));
		mpfr_set(arg1midX, arg1tmpX, STD_RND);
		beginEmulateDouble();
		int t = mpfr_set(arg1oriX, arg1tmpX, STD_RND);
		mpfr_subnormalize(arg1oriX, t, STD_RND);
		endEmulate();
	} else {
		ShadowValue* argTmp = getTemp(unOpArgs->arg);
		// checkAndRecover(argTmp); // LIMITATION can not check here
		// computeRelativeError(irel, argTmp);
		if (argTmp) {
			// Char mpfrBuf[MPFR_BUFSIZE];
			// mpfrToString(mpfrBuf, &(argTmp->svalue));
			// VG_(umsg)("SHADOW VALUE: %s\n", mpfrBuf);
			// mpfrToString(mpfrBuf, &(argTmp->midValue));
			// VG_(umsg)("MIDDLE VALUE: %s\n", mpfrBuf);
			mpfr_set(arg1tmpX, argTmp->value, STD_RND);
			mpfr_set(arg1midX, argTmp->midValue, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg1oriX, argTmp->oriValue, STD_RND);
			mpfr_subnormalize(arg1oriX, t, STD_RND);
			endEmulate();
			argOpCount = argTmp->opCount;
			argOrigin = argTmp->origin;
			argCanceled = argTmp->canceled;
			argCancelOrigin = argTmp->cancelOrigin;
		} else {
			readSTemp(0, &(arg1tmpX));
			mpfr_set(arg1midX, arg1tmpX, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg1oriX, arg1tmpX, STD_RND);
			mpfr_subnormalize(arg1oriX, t, STD_RND);
			endEmulate();
		}
	}

	ShadowValue* res = setTemp(unOpArgs->wrTmp);
	if (clo_simulateOriginal) {
		if (isOpFloat(unOpArgs->op)) {
			mpfr_set_prec(res->value, 24);
		} else {
			mpfr_set_prec(res->value, 53);
		}
	}
	if (isOpFloat(unOpArgs->op)) {
		mpfr_set_prec(res->midValue, 24);
		mpfr_set_prec(res->oriValue, 24);
	} else {
		mpfr_set_prec(res->midValue, 53);
		mpfr_set_prec(res->oriValue, 53);
	}
	res->opCount = argOpCount + 1;
	res->origin = addr;

	fpOps++;

	// Bool needFix = clo_detect_pso && VG_(HT_lookup)(detectedPSO, addr) != NULL;
	// if (needFix) {
	// 	mpfr_set(arg1midX, arg1tmpX, STD_RND);
	// }

	IROp op = unOpArgs->op;
	int tv;
	switch (op) {
		case Iop_Sqrt32F0x4:
		case Iop_Sqrt64F0x2:
			mpfr_sqrt(res->value, arg1tmpX, STD_RND);
			mpfr_sqrt(res->midValue, arg1midX, STD_RND);
			beginEmulateDouble();	
			tv = mpfr_sqrt(res->oriValue, arg1oriX, STD_RND);
			mpfr_subnormalize(res->oriValue, tv, STD_RND);
			endEmulate();
			break;
		case Iop_NegF32:
		case Iop_NegF64:
			mpfr_neg(res->value, arg1tmpX, STD_RND);
			mpfr_neg(res->midValue, arg1midX, STD_RND);
			beginEmulateDouble();
			tv = mpfr_neg(res->oriValue, arg1oriX, STD_RND);
			mpfr_subnormalize(res->oriValue, tv, STD_RND);
			endEmulate();
			break;
		case Iop_AbsF32:
		case Iop_AbsF64:
			mpfr_abs(res->value, arg1tmpX, STD_RND);
			mpfr_abs(res->midValue, arg1midX, STD_RND);
			VG_(umsg)("In ABS!\n");
			beginEmulateDouble();
			tv = mpfr_abs(res->oriValue, arg1oriX, STD_RND);
			mpfr_subnormalize(res->oriValue, tv, STD_RND);
			endEmulate();
			break;
		default:
			VG_(tool_panic)("Unhandled case in processUnOp\n");
			break;
	}

	// if (needFix) {
	// 	mpfr_set(res->value, res->midValue, STD_RND);
	// }

	res->canceled = argCanceled;
	res->cancelOrigin = argCancelOrigin;

	if (clo_computeMeanValue) {
		if (isOpFloat(unOpArgs->op)) {
			mpfr_set_flt(meanOrg, unOpArgs->orgFloat, STD_RND);
		} else {
			mpfr_set_d(meanOrg, unOpArgs->orgDouble, STD_RND);
		}
		updateMeanValue(addr, unOpArgs->op, &(res->value), 0, argOrigin, 0, 0);
	}

	if (isOpFloat(unOpArgs->op)) {
		res->Org.fl = unOpArgs->orgFloat;
		res->orgType = Ot_FLOAT;
	} else {
		res->Org.db = unOpArgs->orgDouble;
		res->orgType = Ot_DOUBLE;
	}
	// if (clo_detect_pso && !finishPSO) {
	// 	analyzePSO(irel, res);
	// }
	// mpfr_clear(irel);
	if (clo_print_every_error) {
		printErrorShort(res);
	}
}

static void instrumentUnOp(IRSB* sb, IRTypeEnv* env, Addr addr, IRTemp wrTemp, IRExpr* unop, Int argTmpInstead) {
	tl_assert(unop->tag == Iex_Unop);

	if (clo_ignoreLibraries && isInLibrary(addr)) {
		return;
	}

	IRExpr* arg = unop->Iex.Unop.arg;
	tl_assert(arg->tag == Iex_RdTmp || arg->tag == Iex_Const);

	IROp op = unop->Iex.Unop.op;
	IRStmt* store = IRStmt_Store(Iend_LE, mkU64(&(unOpArgs->op)), mkU32(op));
	addStmtToIRSB(sb, store);
	store = IRStmt_Store(Iend_LE, mkU64(&(unOpArgs->wrTmp)), mkU32(wrTemp));
	addStmtToIRSB(sb, store);

	Int constArgs = 0;
	
	if (arg->tag == Iex_RdTmp) {
		if (argTmpInstead >= 0) {
			store = IRStmt_Store(Iend_LE, mkU64(&(unOpArgs->arg)), mkU32(argTmpInstead));
		} else {
			store = IRStmt_Store(Iend_LE, mkU64(&(unOpArgs->arg)), mkU32(unop->Iex.Unop.arg->Iex.RdTmp.tmp));
		}
		addStmtToIRSB(sb, store);

		writeSTemp(sb, env, arg->Iex.RdTmp.tmp, 0);
	} else {
		tl_assert(arg->tag == Iex_Const);
		writeSConst(sb, arg->Iex.Const.con, 0);
		constArgs |= 0x1;
	}

	if (isOpFloat(op)) {
		store = IRStmt_Store(Iend_LE, mkU64(&(unOpArgs->orgFloat)), IRExpr_RdTmp(wrTemp));
		addStmtToIRSB(sb, store);
	} else {
		store = IRStmt_Store(Iend_LE, mkU64(&(unOpArgs->orgDouble)), IRExpr_RdTmp(wrTemp));
		addStmtToIRSB(sb, store);
	}

	IRExpr** argv = mkIRExprVec_2(mkU64(addr), mkU64(constArgs));
	IRDirty* di = unsafeIRDirty_0_N(2, "processUnOp", VG_(fnptr_to_fnentry)(&processUnOp), argv);
	addStmtToIRSB(sb, IRStmt_Dirty(di));
}

static VG_REGPARM(2) void processBinOp(Addr addr, UWord ca) {
	if (!clo_analyze) return;

	Int constArgs = (Int)ca;
	Bool needFix = clo_detect_pso && VG_(HT_lookup)(detectedPSO, addr) != NULL;

	if (clo_simulateOriginal) {
		if (isOpFloat(binOpArgs->op)) {
			mpfr_set_prec(arg1tmpX, 24);
			mpfr_set_prec(arg2tmpX, 24);
		} else {
			mpfr_set_prec(arg1tmpX, 53);
			mpfr_set_prec(arg2tmpX, 53);
		}
	}

	if (isOpFloat(binOpArgs->op)) {
		mpfr_set_prec(arg1midX, 24);
		mpfr_set_prec(arg2midX, 24);
		mpfr_set_prec(arg1oriX, 24);
		mpfr_set_prec(arg2oriX, 24);
	} else {
		mpfr_set_prec(arg1midX, 53);
		mpfr_set_prec(arg2midX, 53);
		mpfr_set_prec(arg1oriX, 53);
		mpfr_set_prec(arg2oriX, 53);
	}

	ULong arg1opCount = 0;
	ULong arg2opCount = 0;
	Addr arg1origin = 0;
	Addr arg2origin = 0;
	mpfr_exp_t arg1canceled = 0;
	mpfr_exp_t arg2canceled = 0;
	mpfr_exp_t canceled = 0;
	Addr arg1CancelOrigin = 0;
	Addr arg2CancelOrigin = 0;
	mpfr_t irel1, irel2;
	mpfr_inits(irel1, irel2, NULL);

	Int exactBitsArg1, exactBitsArg2;
	if (isOpFloat(binOpArgs->op)) {
		exactBitsArg1 = 23;
		exactBitsArg2 = 23;
	} else {
		exactBitsArg1 = 52;
		exactBitsArg2 = 52;
	}

	if (constArgs & 0x1) {
		readSConst(0, &(arg1tmpX));
		mpfr_set(arg1midX, arg1tmpX, STD_RND);
		beginEmulateDouble();
		int t = mpfr_set(arg1oriX, arg1tmpX, STD_RND);
		mpfr_subnormalize(arg1oriX, t, STD_RND);
		endEmulate();
	} else {
		ShadowValue* arg1tmp = getTemp(binOpArgs->arg1);
		checkAndRecover(arg1tmp);
		computeRelativeError(arg1tmp, irel1);
		// if (needFix) printErrorShort(arg1tmp);
		if (arg1tmp) {
			// VG_(umsg)("have shadow 1\n");
			mpfr_set(arg1tmpX, arg1tmp->value, STD_RND);
			mpfr_set(arg1midX, arg1tmp->midValue, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg1oriX, arg1tmp->oriValue, STD_RND);
			mpfr_subnormalize(arg1oriX, t, STD_RND);
			endEmulate();
			arg1opCount = arg1tmp->opCount;
			arg1origin = arg1tmp->origin;
			arg1canceled = arg1tmp->canceled;
			arg1CancelOrigin = arg1tmp->cancelOrigin;

			if (clo_bad_cancellations) {
				readSTemp(0, &cancelTemp);
				if (mpfr_get_exp(cancelTemp) == mpfr_get_exp(arg1tmpX)) {
					mpfr_sub(cancelTemp, arg1tmpX, cancelTemp, STD_RND);
					if (mpfr_cmp_ui(cancelTemp, 0) != 0) {
						exactBitsArg1 = abs(mpfr_get_exp(arg1tmpX) - mpfr_get_exp(cancelTemp)) - 2;
						if (arg1tmp->orgType == Ot_FLOAT && exactBitsArg1 > 23) {
							exactBitsArg1 = 23;
						} else if (arg1tmp->orgType == Ot_DOUBLE && exactBitsArg1 > 52) {
							exactBitsArg1 = 52;
						}
					}
				} else {
					exactBitsArg1 = 0;
				}
			}
		} else {
			readSTemp(0, &(arg1tmpX));
			mpfr_set(arg1midX, arg1tmpX, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg1oriX, arg1tmpX, STD_RND);
			mpfr_subnormalize(arg1oriX, t, STD_RND);
			endEmulate();
		}
	}

	if (constArgs & 0x2) {
		readSConst(1, &(arg2tmpX));
		mpfr_set(arg2midX, arg2tmpX, STD_RND);
		beginEmulateDouble();
		int t = mpfr_set(arg2oriX, arg2tmpX, STD_RND);
		mpfr_subnormalize(arg2oriX, t, STD_RND);
		endEmulate();
	} else {
		ShadowValue* arg2tmp = getTemp(binOpArgs->arg2);
		checkAndRecover(arg2tmp);
		// VG_(umsg)("get %X\n", binOpArgs->arg2);
		computeRelativeError(arg2tmp, irel2);
		if (arg2tmp) {
			// if (needFix) printErrorShort(arg2tmp);
			// VG_(umsg)("have shadow 2\n");
			mpfr_set(arg2tmpX, arg2tmp->value, STD_RND);
			mpfr_set(arg2midX, arg2tmp->midValue, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg2oriX, arg2tmp->oriValue, STD_RND);
			mpfr_subnormalize(arg2oriX, t, STD_RND);
			endEmulate();
			arg2opCount = arg2tmp->opCount;
			arg2origin = arg2tmp->origin;
			arg2canceled = arg2tmp->canceled;
			arg2CancelOrigin = arg2tmp->cancelOrigin;

			if (clo_bad_cancellations) {
				readSTemp(1, &cancelTemp);
				if (mpfr_get_exp(cancelTemp) == mpfr_get_exp(arg2tmpX)) {
					mpfr_sub(cancelTemp, arg2tmpX, cancelTemp, STD_RND);
					if (mpfr_cmp_ui(cancelTemp, 0) != 0) {
						exactBitsArg2 = abs(mpfr_get_exp(arg2tmpX) - mpfr_get_exp(cancelTemp)) - 2;
						if (arg2tmp->orgType == Ot_FLOAT && exactBitsArg2 > 23) {
							exactBitsArg2 = 23;
						} else if (arg2tmp->orgType == Ot_DOUBLE && exactBitsArg2 > 52) {
							exactBitsArg2 = 52;
						}
					}
				} else {
					exactBitsArg2 = 0;
				}
			}
		} else {
			readSTemp(1, &(arg2tmpX));
			mpfr_set(arg2midX, arg2tmpX, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg2oriX, arg2tmpX, STD_RND);
			mpfr_subnormalize(arg2oriX, t, STD_RND);
			endEmulate();
		}
	}

	ShadowValue* res = setTemp(binOpArgs->wrTmp);
	// VG_(umsg)("processBinOp %X\n", binOpArgs->wrTmp);
	if (clo_simulateOriginal) {
		if (isOpFloat(binOpArgs->op)) {
			mpfr_set_prec(res->value, 24);
		} else {
			mpfr_set_prec(res->value, 53);
		}
	}

	if (isOpFloat(binOpArgs->op)) {
		mpfr_set_prec(res->midValue, 24);
		mpfr_set_prec(res->oriValue, 24);
	} else {
		mpfr_set_prec(res->midValue, 53);
		mpfr_set_prec(res->oriValue, 53);
	}

	res->opCount = 1;
	if (arg1opCount > arg2opCount) {
		res->opCount += arg1opCount;
	} else {
		res->opCount += arg2opCount;
	}
	res->origin = addr;

	fpOps++;

	if (needFix) {
		// VG_(umsg)("Need fix!\n");
		mpfr_set(arg1midX, arg1tmpX, STD_RND);
		mpfr_set(arg2midX, arg2tmpX, STD_RND);
		// Char mpfrBuf[MPFR_BUFSIZE];
		// mpfrToString(mpfrBuf, &arg1midX);
		// VG_(umsg)("Mid1 %s\n", mpfrBuf);
		// mpfrToString(mpfrBuf, &arg2midX);
		// VG_(umsg)("Mid2 %s\n", mpfrBuf);
	}

	int tv;
	switch (binOpArgs->op) {
		case Iop_Add32F0x4:
		case Iop_Add64F0x2:
			mpfr_add(res->value, arg1tmpX, arg2tmpX, STD_RND);
			mpfr_add(res->midValue, arg1midX, arg2midX, STD_RND);
			beginEmulateDouble();
			tv = mpfr_add(res->oriValue, arg1oriX, arg2oriX, STD_RND);
			mpfr_subnormalize(res->oriValue, tv, STD_RND);
			endEmulate();
			canceled = getCanceledBits(&(res->value), &(arg1tmpX), &(arg2tmpX));
			break;
		case Iop_Sub32F0x4:
		case Iop_Sub64F0x2:
			mpfr_sub(res->value, arg1tmpX, arg2tmpX, STD_RND);
			mpfr_sub(res->midValue, arg1midX, arg2midX, STD_RND);
			beginEmulateDouble();
			tv = mpfr_sub(res->oriValue, arg1oriX, arg2oriX, STD_RND);
			mpfr_subnormalize(res->oriValue, tv, STD_RND);
			endEmulate();
			canceled = getCanceledBits(&(res->value), &(arg1tmpX), &(arg2tmpX));
			break;
		case Iop_Mul32F0x4:
		case Iop_Mul64F0x2:
			mpfr_mul(res->value, arg1tmpX, arg2tmpX, STD_RND);
			mpfr_mul(res->midValue, arg1midX, arg2midX, STD_RND);
			beginEmulateDouble();
			tv = mpfr_mul(res->oriValue, arg1oriX, arg2oriX, STD_RND);
			mpfr_subnormalize(res->oriValue, tv, STD_RND);
			endEmulate();
			break;
		case Iop_Div32F0x4:
		case Iop_Div64F0x2:
			mpfr_div(res->value, arg1tmpX, arg2tmpX, STD_RND);
			mpfr_div(res->midValue, arg1midX, arg2midX, STD_RND);
			beginEmulateDouble();
			tv = mpfr_div(res->oriValue, arg1oriX, arg2oriX, STD_RND);
			mpfr_subnormalize(res->oriValue, tv, STD_RND);
			endEmulate();
			break;
		case Iop_Min32F0x4:
		case Iop_Min64F0x2:
			mpfr_min(res->value, arg1tmpX, arg2tmpX, STD_RND);
			mpfr_min(res->midValue, arg1midX, arg2midX, STD_RND);
			beginEmulateDouble();
			tv = mpfr_min(res->oriValue, arg1oriX, arg2oriX, STD_RND);
			mpfr_subnormalize(res->oriValue, tv, STD_RND);
			endEmulate();
			break;
		case Iop_Max32F0x4:
		case Iop_Max64F0x2:
			mpfr_max(res->value, arg1tmpX, arg2tmpX, STD_RND);
			mpfr_max(res->midValue, arg1midX, arg2midX, STD_RND);
			beginEmulateDouble();
			tv = mpfr_max(res->oriValue, arg1oriX, arg2oriX, STD_RND);
			mpfr_subnormalize(res->oriValue, tv, STD_RND);
			endEmulate();
			break;
		default:
			VG_(tool_panic)("Unhandled case in processBinOp\n");
			break;
	}

	mpfr_exp_t maxC = canceled;
	Addr maxCorigin = addr;
	if (arg1canceled > maxC) {
		maxC = arg1canceled;
		maxCorigin = arg1CancelOrigin;
	}
	if (arg2canceled > maxC) {
		maxC = arg2canceled;
		maxCorigin = arg2CancelOrigin;
	}
	res->canceled = maxC;
	res->cancelOrigin = maxCorigin;
	
	if (clo_computeMeanValue) {
		UInt cancellationBadness = 0;
		if (clo_bad_cancellations && canceled > 0) {
			Int exactBits = exactBitsArg1 < exactBitsArg2 ? exactBitsArg1 : exactBitsArg2;
			if (canceled > exactBits) {
				cancellationBadness = canceled - exactBits;
			}
		}

		if (isOpFloat(binOpArgs->op)) {
			mpfr_set_flt(meanOrg, binOpArgs->orgFloat, STD_RND);
		} else {
			mpfr_set_d(meanOrg, binOpArgs->orgDouble, STD_RND);
		}
		updateMeanValue(addr, binOpArgs->op, &(res->value), canceled, arg1origin, arg2origin, cancellationBadness);
	}

	if (isOpFloat(binOpArgs->op)) {
		res->Org.fl = binOpArgs->orgFloat;
		res->orgType = Ot_FLOAT;
	} else {
		res->Org.db = binOpArgs->orgDouble;
		res->orgType = Ot_DOUBLE;
	}
	if (needFix) {
		mpfr_set(res->value, res->midValue, STD_RND);
		// printErrorShort(res);
	}
	if (clo_detect_pso && !finishPSO) {
		mpfr_max(irel1, irel1, irel2, STD_RND);
		analyzePSO(irel1, res);
	}
	if (clo_print_every_error) {
		printErrorShort(res);
	}
}

static void instrumentBinOp(IRSB* sb, IRTypeEnv* env, Addr addr, IRTemp wrTemp, IRExpr* binop, Int arg1tmpInstead, Int arg2tmpInstead) {
	tl_assert(binop->tag == Iex_Binop);

	if (clo_ignoreLibraries && isInLibrary(addr)) {
		return;
	}

	IROp op = binop->Iex.Binop.op;
	IRExpr* arg1 = binop->Iex.Binop.arg1;
	IRExpr* arg2 = binop->Iex.Binop.arg2;

	tl_assert(arg1->tag == Iex_RdTmp || arg1->tag == Iex_Const);
	tl_assert(arg2->tag == Iex_RdTmp || arg2->tag == Iex_Const);

	Int constArgs = 0;

	IRStmt* store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->op)), mkU32(op));
	addStmtToIRSB(sb, store);
	store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->wrTmp)), mkU32(wrTemp));
	addStmtToIRSB(sb, store);

	if (arg1->tag == Iex_RdTmp) {
		if (arg1tmpInstead >= 0) {
			store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->arg1)), mkU32(arg1tmpInstead));
		} else {
			store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->arg1)), mkU32(arg1->Iex.RdTmp.tmp));
		}
		addStmtToIRSB(sb, store);

		writeSTemp(sb, env, arg1->Iex.RdTmp.tmp, 0);
	} else {
		tl_assert(arg1->tag == Iex_Const);

		writeSConst(sb, arg1->Iex.Const.con, 0);
		constArgs |= 0x1;
	}
	if (arg2->tag == Iex_RdTmp) {
		if (arg2tmpInstead >= 0) {
			store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->arg2)), mkU32(arg2tmpInstead));
		} else {
			store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->arg2)), mkU32(arg2->Iex.RdTmp.tmp));
		}
		addStmtToIRSB(sb, store);

		writeSTemp(sb, env, arg2->Iex.RdTmp.tmp, 1);
	} else {
		tl_assert(arg2->tag == Iex_Const);

		writeSConst(sb, arg2->Iex.Const.con, 1);
		constArgs |= 0x2;
	}

	if (isOpFloat(op)) {
		store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->orgFloat)), IRExpr_RdTmp(wrTemp));
		addStmtToIRSB(sb, store);
	} else {
		store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->orgDouble)), IRExpr_RdTmp(wrTemp));
		addStmtToIRSB(sb, store);
	}

	IRExpr** argv = mkIRExprVec_2(mkU64(addr), mkU64(constArgs));
	IRDirty* di = unsafeIRDirty_0_N(2, "processBinOp", VG_(fnptr_to_fnentry)(&processBinOp), argv);
	addStmtToIRSB(sb, IRStmt_Dirty(di));
}

static VG_REGPARM(2) void processTriOp(Addr addr, UWord ca) {
	if (!clo_analyze) return;

	Int constArgs = (Int)ca;
	IROp op = triOpArgs->op;

	if (clo_simulateOriginal) {
		if (isOpFloat(op)) {
			mpfr_set_prec(arg2tmpX, 24);
			mpfr_set_prec(arg3tmpX, 24);
		} else {
			mpfr_set_prec(arg2tmpX, 53);
			mpfr_set_prec(arg3tmpX, 53);
		}
	}

	if (isOpFloat(op)) {
		mpfr_set_prec(arg2midX, 24);
		mpfr_set_prec(arg3midX, 24);
		mpfr_set_prec(arg2oriX, 24);
		mpfr_set_prec(arg3oriX, 24);
	} else {
		mpfr_set_prec(arg2midX, 53);
		mpfr_set_prec(arg3midX, 53);
		mpfr_set_prec(arg2oriX, 53);
		mpfr_set_prec(arg3oriX, 53);
	}

	ULong arg2opCount = 0;
	ULong arg3opCount = 0;
	Addr arg2origin = 0;
	Addr arg3origin = 0;
	mpfr_exp_t arg2canceled = 0;
	mpfr_exp_t arg3canceled = 0;
	mpfr_exp_t canceled = 0;
	Addr arg2CancelOrigin = 0;
	Addr arg3CancelOrigin = 0;
	mpfr_t irel2, irel3;
	mpfr_inits(irel2, irel3, NULL);

	Int exactBitsArg2, exactBitsArg3;
	if (isOpFloat(op)) {
		exactBitsArg2 = 23;
		exactBitsArg3 = 23;
	} else {
		exactBitsArg2 = 52;
		exactBitsArg3 = 52;
	}

	if (constArgs & 0x2) {
		readSConst(1, &(arg2tmpX));
		mpfr_set(arg2midX, arg2tmpX, STD_RND);
		beginEmulateDouble();
		int t = mpfr_set(arg2oriX, arg2tmpX, STD_RND);
		mpfr_subnormalize(arg2oriX, t, STD_RND);
		endEmulate();
	} else {
		ShadowValue* arg2tmp = getTemp(triOpArgs->arg2);
		checkAndRecover(arg2tmp);
		computeRelativeError(arg2tmp, irel2);
		if (arg2tmp) {
			mpfr_set(arg2tmpX, arg2tmp->value, STD_RND);
			mpfr_set(arg2midX, arg2tmp->midValue, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg2oriX, arg2tmp->oriValue, STD_RND);
			mpfr_subnormalize(arg2oriX, t, STD_RND);
			endEmulate();
			arg2opCount = arg2tmp->opCount;
			arg2origin = arg2tmp->origin;
			arg2canceled = arg2tmp->canceled;
			arg2CancelOrigin = arg2tmp->cancelOrigin;

			if (clo_bad_cancellations) {
				readSTemp(1, &cancelTemp);
				if (mpfr_get_exp(cancelTemp) == mpfr_get_exp(arg2tmpX)) {
					mpfr_sub(cancelTemp, arg2tmpX, cancelTemp, STD_RND);
					if (mpfr_cmp_ui(cancelTemp, 0) != 0) {
						exactBitsArg2 = abs(mpfr_get_exp(arg2tmpX) - mpfr_get_exp(cancelTemp)) - 2;
						if (arg2tmp->orgType == Ot_FLOAT && exactBitsArg2 > 23) {
							exactBitsArg2 = 23;
						} else if (arg2tmp->orgType == Ot_DOUBLE && exactBitsArg2 > 52) {
							exactBitsArg2 = 52;
						}
					}
				} else {
					exactBitsArg2 = 0;
				}
			}
		} else {
			readSTemp(1, &(arg2tmpX));
			mpfr_set(arg2midX, arg2tmpX, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg2oriX, arg2tmpX, STD_RND);
			mpfr_subnormalize(arg2oriX, t, STD_RND);
			endEmulate();
		}
	}

	if (constArgs & 0x4) {
		readSConst(2, &(arg3tmpX));
		mpfr_set(arg3midX, arg3tmpX, STD_RND);
		beginEmulateDouble();
		int t = mpfr_set(arg3oriX, arg3tmpX, STD_RND);
		mpfr_subnormalize(arg3oriX, t, STD_RND);
		endEmulate();
	} else {
		ShadowValue* arg3tmp = getTemp(triOpArgs->arg3);
		checkAndRecover(arg3tmp);
		computeRelativeError(arg3tmp, irel3);
		if (arg3tmp) {
			mpfr_set(arg3tmpX, arg3tmp->value, STD_RND);
			mpfr_set(arg3midX, arg3tmp->midValue, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg3oriX, arg3tmp->oriValue, STD_RND);
			mpfr_subnormalize(arg3oriX, t, STD_RND);
			endEmulate();
			arg3opCount = arg3tmp->opCount;
			arg3origin = arg3tmp->origin;
			arg3canceled = arg3tmp->canceled;
			arg3CancelOrigin = arg3tmp->cancelOrigin;

			if (clo_bad_cancellations) {
				readSTemp(2, &cancelTemp);
				if (mpfr_get_exp(cancelTemp) == mpfr_get_exp(arg3tmpX)) {
					mpfr_sub(cancelTemp, arg3tmpX, cancelTemp, STD_RND);
					if (mpfr_cmp_ui(cancelTemp, 0) != 0) {
						exactBitsArg3 = abs(mpfr_get_exp(arg3tmpX) - mpfr_get_exp(cancelTemp)) - 2;
						if (arg3tmp->orgType == Ot_FLOAT && exactBitsArg3 > 23) {
							exactBitsArg3 = 23;
						} else if (arg3tmp->orgType == Ot_DOUBLE && exactBitsArg3 > 52) {
							exactBitsArg3 = 52;
						}
					}
				} else {
					exactBitsArg3 = 0;
				}
			}
		} else {
			readSTemp(2, &(arg3tmpX));
			mpfr_set(arg3midX, arg3tmpX, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg3oriX, arg3tmpX, STD_RND);
			mpfr_subnormalize(arg3oriX, t, STD_RND);
			endEmulate();
		}
	}

	ShadowValue* res = setTemp(triOpArgs->wrTmp);
	if (clo_simulateOriginal) {
		if (isOpFloat(op)) {
			mpfr_set_prec(res->value, 24);
		} else {
			mpfr_set_prec(res->value, 53);
		}
	}

	if (isOpFloat(op)) {
		mpfr_set_prec(res->midValue, 24);
		mpfr_set_prec(res->oriValue, 24);
	} else {
		mpfr_set_prec(res->midValue, 53);
		mpfr_set_prec(res->oriValue, 53);
	}

	res->opCount = 1;
	if (arg2opCount > arg3opCount) {
		res->opCount += arg2opCount;
	} else {
		res->opCount += arg3opCount;
	}
	res->origin = addr;

	fpOps++;

	Bool needFix = clo_detect_pso && VG_(HT_lookup)(detectedPSO, addr) != NULL;
	if (needFix) {
		mpfr_set(arg2midX, arg2tmpX, STD_RND);
		mpfr_set(arg3midX, arg3tmpX, STD_RND);
	}

	int tv;
	switch (op) {
		case Iop_AddF64:
			mpfr_add(res->value, arg2tmpX, arg3tmpX, STD_RND);
			mpfr_add(res->midValue, arg2midX, arg3midX, STD_RND);
			beginEmulateDouble();
			tv = mpfr_add(res->oriValue, arg2oriX, arg3oriX, STD_RND);
			mpfr_subnormalize(res->oriValue, tv, STD_RND);
			endEmulate();
			canceled = getCanceledBits(&(res->value), &(arg2tmpX), &(arg3tmpX));
			break;
		case Iop_SubF64:
			mpfr_sub(res->value, arg2tmpX, arg3tmpX, STD_RND);
			mpfr_sub(res->midValue, arg2midX, arg3midX, STD_RND);
			beginEmulateDouble();
			tv = mpfr_sub(res->oriValue, arg2oriX, arg3oriX, STD_RND);
			mpfr_subnormalize(res->oriValue, tv, STD_RND);
			endEmulate();
			canceled = getCanceledBits(&(res->value), &(arg2tmpX), &(arg3tmpX));
			break;
		case Iop_MulF64:
			mpfr_mul(res->value, arg2tmpX, arg3tmpX, STD_RND);
			mpfr_mul(res->midValue, arg2midX, arg3midX, STD_RND);
			beginEmulateDouble();
			tv = mpfr_mul(res->oriValue, arg2oriX, arg3oriX, STD_RND);
			mpfr_subnormalize(res->oriValue, tv, STD_RND);
			endEmulate();
			break;
		case Iop_DivF64:
			mpfr_div(res->value, arg2tmpX, arg3tmpX, STD_RND);
			mpfr_div(res->midValue, arg2midX, arg3midX, STD_RND);
			beginEmulateDouble();
			tv = mpfr_div(res->oriValue, arg2oriX, arg3oriX, STD_RND);
			mpfr_subnormalize(res->oriValue, tv, STD_RND);
			endEmulate();
			break;
		default:
			VG_(tool_panic)("Unhandled case in processTriOp");
			break;
	}

	if (needFix) {
		mpfr_set(res->value, res->midValue, STD_RND);
	}

	mpfr_exp_t maxC = canceled;
	Addr maxCorigin = addr;
	if (arg2canceled > maxC) {
		maxC = arg2canceled;
		maxCorigin = arg2CancelOrigin;
	}
	if (arg3canceled > maxC) {
		maxC = arg3canceled;
		maxCorigin = arg3CancelOrigin;
	}
	res->canceled = maxC;
	res->cancelOrigin = maxCorigin;

	if (clo_computeMeanValue) {
		UInt cancellationBadness = 0;
		if (clo_bad_cancellations && canceled > 0) {
			Int exactBits = exactBitsArg2 < exactBitsArg3 ? exactBitsArg2 : exactBitsArg3;
			if (canceled > exactBits) {
				cancellationBadness = canceled - exactBits;
			}
		}

		mpfr_set_d(meanOrg, triOpArgs->orgDouble, STD_RND);
		updateMeanValue(addr, op, &(res->value), canceled, arg2origin, arg3origin, cancellationBadness);
	}

	res->Org.db = triOpArgs->orgDouble;
	res->orgType = Ot_DOUBLE;
	if (clo_detect_pso && !finishPSO) {
		mpfr_max(irel2, irel2, irel3, STD_RND);
		analyzePSO(irel2, res);
	}
	if (clo_print_every_error) {
		printErrorShort(res);
	}
}

static void instrumentTriOp(IRSB* sb, IRTypeEnv* env, Addr addr, IRTemp wrTemp, IRExpr* triop, Int arg2tmpInstead, Int arg3tmpInstead) {
	tl_assert(triop->tag == Iex_Triop);

	if (clo_ignoreLibraries && isInLibrary(addr)) {
		return;
	}

	IROp op = triop->Iex.Triop.op;
	IRExpr* arg1 = triop->Iex.Triop.arg1;
	IRExpr* arg2 = triop->Iex.Triop.arg2;
	IRExpr* arg3 = triop->Iex.Triop.arg3;

	tl_assert(arg1->tag == Iex_Const);
	tl_assert(arg2->tag == Iex_RdTmp || arg2->tag == Iex_Const);
	tl_assert(arg3->tag == Iex_RdTmp || arg3->tag == Iex_Const);

	Int constArgs = 0;

	IRStmt* store = IRStmt_Store(Iend_LE, mkU64(&(triOpArgs->op)), mkU32(op));
	addStmtToIRSB(sb, store);
	store = IRStmt_Store(Iend_LE, mkU64(&(triOpArgs->wrTmp)), mkU32(wrTemp));
	addStmtToIRSB(sb, store);

	/* arg1 is ignored because it only contains the rounding mode for 
	   the operations instructed at the moment */

	if (arg2->tag == Iex_RdTmp) {
		if (arg2tmpInstead >= 0) {
			store = IRStmt_Store(Iend_LE, mkU64(&(triOpArgs->arg2)), mkU32(arg2tmpInstead));
		} else {
			store = IRStmt_Store(Iend_LE, mkU64(&(triOpArgs->arg2)), mkU32(triop->Iex.Triop.arg2->Iex.RdTmp.tmp));
		}
		addStmtToIRSB(sb, store);
		writeSTemp(sb, env, triop->Iex.Triop.arg2->Iex.RdTmp.tmp, 1);
	} else {
		tl_assert(arg2->tag == Iex_Const);
		writeSConst(sb, arg2->Iex.Const.con, 1);
		constArgs |= 0x2;
	}

	if (arg3->tag == Iex_RdTmp) {
		if (arg3tmpInstead >= 0) {
			store = IRStmt_Store(Iend_LE, mkU64(&(triOpArgs->arg3)), mkU32(arg3tmpInstead));
		} else {
			store = IRStmt_Store(Iend_LE, mkU64(&(triOpArgs->arg3)), mkU32(triop->Iex.Triop.arg3->Iex.RdTmp.tmp));
		}
		addStmtToIRSB(sb, store);
		writeSTemp(sb, env, triop->Iex.Triop.arg3->Iex.RdTmp.tmp, 2);
	} else {
		tl_assert(arg3->tag == Iex_Const);
		writeSConst(sb, arg3->Iex.Const.con, 2);
		constArgs |= 0x4;
	}

	store = IRStmt_Store(Iend_LE, mkU64(&(triOpArgs->orgDouble)), IRExpr_RdTmp(wrTemp));
	addStmtToIRSB(sb, store);

	IRExpr** argv = mkIRExprVec_2(mkU64(addr), mkU64(constArgs));
	IRDirty* di = unsafeIRDirty_0_N(2, "processTriOp", VG_(fnptr_to_fnentry)(&processTriOp), argv);
	addStmtToIRSB(sb, IRStmt_Dirty(di));
}

static VG_REGPARM(2) UInt processCmpF64(Addr addr, UWord ca) {
	if (!clo_analyze) return;

	Int constArgs = (Int)ca;

	if (clo_simulateOriginal) {
		if (isOpFloat(binOpArgs->op)) {
			mpfr_set_prec(arg1tmpX, 24);
			mpfr_set_prec(arg2tmpX, 24);
		} else {
			mpfr_set_prec(arg1tmpX, 53);
			mpfr_set_prec(arg2tmpX, 53);
		}
	}

	if (isOpFloat(binOpArgs->op)) {
		mpfr_set_prec(arg1midX, 24);
		mpfr_set_prec(arg2midX, 24);
		mpfr_set_prec(arg1oriX, 24);
		mpfr_set_prec(arg2oriX, 24);
	} else {
		mpfr_set_prec(arg1midX, 53);
		mpfr_set_prec(arg2midX, 53);
		mpfr_set_prec(arg1oriX, 53);
		mpfr_set_prec(arg2oriX, 53);
	}

	ULong arg1opCount = 0;
	ULong arg2opCount = 0;
	Addr arg1origin = 0;
	Addr arg2origin = 0;
	mpfr_exp_t arg1canceled = 0;
	mpfr_exp_t arg2canceled = 0;
	mpfr_exp_t canceled = 0;
	Addr arg1CancelOrigin = 0;
	Addr arg2CancelOrigin = 0;
	mpfr_t irel1, irel2;
	mpfr_inits(irel1, irel2, NULL);

	Int exactBitsArg1, exactBitsArg2;
	if (isOpFloat(binOpArgs->op)) {
		exactBitsArg1 = 23;
		exactBitsArg2 = 23;
	} else {
		exactBitsArg1 = 52;
		exactBitsArg2 = 52;
	}

	if (constArgs & 0x1) {
		readSConst(0, &(arg1tmpX));
		mpfr_set(arg1midX, arg1tmpX, STD_RND);
		beginEmulateDouble();
		int t = mpfr_set(arg1oriX, arg1tmpX, STD_RND);
		mpfr_subnormalize(arg1oriX, t, STD_RND);
		endEmulate();
	} else {
		ShadowValue* arg1tmp = getTemp(binOpArgs->arg1);
		checkAndRecover(arg1tmp);
		computeRelativeError(arg1tmp, irel1);
		// if (needFix) printErrorShort(arg1tmp);
		if (arg1tmp) {
			// VG_(umsg)("have shadow 1\n");
			mpfr_set(arg1tmpX, arg1tmp->value, STD_RND);
			mpfr_set(arg1midX, arg1tmp->midValue, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg1oriX, arg1tmp->oriValue, STD_RND);
			mpfr_subnormalize(arg1oriX, t, STD_RND);
			endEmulate();
			arg1opCount = arg1tmp->opCount;
			arg1origin = arg1tmp->origin;
			arg1canceled = arg1tmp->canceled;
			arg1CancelOrigin = arg1tmp->cancelOrigin;

			if (clo_bad_cancellations) {
				readSTemp(0, &cancelTemp);
				if (mpfr_get_exp(cancelTemp) == mpfr_get_exp(arg1tmpX)) {
					mpfr_sub(cancelTemp, arg1tmpX, cancelTemp, STD_RND);
					if (mpfr_cmp_ui(cancelTemp, 0) != 0) {
						exactBitsArg1 = abs(mpfr_get_exp(arg1tmpX) - mpfr_get_exp(cancelTemp)) - 2;
						if (arg1tmp->orgType == Ot_FLOAT && exactBitsArg1 > 23) {
							exactBitsArg1 = 23;
						} else if (arg1tmp->orgType == Ot_DOUBLE && exactBitsArg1 > 52) {
							exactBitsArg1 = 52;
						}
					}
				} else {
					exactBitsArg1 = 0;
				}
			}
		} else {
			readSTemp(0, &(arg1tmpX));
			mpfr_set(arg1midX, arg1tmpX, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg1oriX, arg1tmpX, STD_RND);
			mpfr_subnormalize(arg1oriX, t, STD_RND);
			endEmulate();
		}
	}

	if (constArgs & 0x2) {
		readSConst(1, &(arg2tmpX));
		mpfr_set(arg2midX, arg2tmpX, STD_RND);
		beginEmulateDouble();
		int t = mpfr_set(arg2oriX, arg2tmpX, STD_RND);
		mpfr_subnormalize(arg2oriX, t, STD_RND);
		endEmulate();
	} else {
		ShadowValue* arg2tmp = getTemp(binOpArgs->arg2);
		checkAndRecover(arg2tmp);
		// VG_(umsg)("get %X\n", binOpArgs->arg2);
		computeRelativeError(arg2tmp, irel2);
		if (arg2tmp) {
			// if (needFix) printErrorShort(arg2tmp);
			// VG_(umsg)("have shadow 2\n");
			mpfr_set(arg2tmpX, arg2tmp->value, STD_RND);
			mpfr_set(arg2midX, arg2tmp->midValue, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg2oriX, arg2tmp->oriValue, STD_RND);
			mpfr_subnormalize(arg2oriX, t, STD_RND);
			endEmulate();
			arg2opCount = arg2tmp->opCount;
			arg2origin = arg2tmp->origin;
			arg2canceled = arg2tmp->canceled;
			arg2CancelOrigin = arg2tmp->cancelOrigin;

			if (clo_bad_cancellations) {
				readSTemp(1, &cancelTemp);
				if (mpfr_get_exp(cancelTemp) == mpfr_get_exp(arg2tmpX)) {
					mpfr_sub(cancelTemp, arg2tmpX, cancelTemp, STD_RND);
					if (mpfr_cmp_ui(cancelTemp, 0) != 0) {
						exactBitsArg2 = abs(mpfr_get_exp(arg2tmpX) - mpfr_get_exp(cancelTemp)) - 2;
						if (arg2tmp->orgType == Ot_FLOAT && exactBitsArg2 > 23) {
							exactBitsArg2 = 23;
						} else if (arg2tmp->orgType == Ot_DOUBLE && exactBitsArg2 > 52) {
							exactBitsArg2 = 52;
						}
					}
				} else {
					exactBitsArg2 = 0;
				}
			}
		} else {
			readSTemp(1, &(arg2tmpX));
			mpfr_set(arg2midX, arg2tmpX, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg2oriX, arg2tmpX, STD_RND);
			mpfr_subnormalize(arg2oriX, t, STD_RND);
			endEmulate();
		}
	}

	int tv, oritv;
	// Char mpfrBuf[MPFR_BUFSIZE];
	// mpfrToString(mpfrBuf, &arg1tmpX);
	// VG_(umsg)("ARG1:         %s\n", mpfrBuf);
	// mpfrToString(mpfrBuf, &arg2tmpX);
	// VG_(umsg)("ARG2:         %s\n", mpfrBuf);
	// VG_(umsg)("original cond %d\n", binOpArgs->orgInt);
	IRExpr* lastBranchResult = NULL;
	switch (binOpArgs->op) {
		case Iop_CmpF64:
			tv = mpfr_cmp(arg1tmpX, arg2tmpX);
			oritv = mpfr_cmp(arg1oriX, arg2oriX);
			if (tv != oritv) {
				VG_(describe_IP)(addr, description, DESCRIPTION_SIZE);
				VG_(umsg)("Change branch at %s\n", description);
			}
			if (tv > 0) {
				return Ircr_GT;
			} else if (tv == 0) {
				return Ircr_EQ;
			} else {
				return Ircr_LT;
			}
			break;
		default:
			VG_(tool_panic)("Unhandled case in processCmpF64\n");
			break;
	}

	return lastBranchResult;
}

static void instrumentCmpF64(IRSB* sb, IRTypeEnv* env, Addr addr, IRTemp wrTemp, IRExpr* binop, Int arg1tmpInstead, Int arg2tmpInstead) {
	tl_assert(binop->tag == Iex_Binop);

	if (clo_ignoreLibraries && isInLibrary(addr)) {
		return;
	}

	IROp op = binop->Iex.Binop.op;
	IRExpr* arg1 = binop->Iex.Binop.arg1;
	IRExpr* arg2 = binop->Iex.Binop.arg2;

	tl_assert(arg1->tag == Iex_RdTmp || arg1->tag == Iex_Const);
	tl_assert(arg2->tag == Iex_RdTmp || arg2->tag == Iex_Const);

	Int constArgs = 0;

	IRStmt* store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->op)), mkU32(op));
	addStmtToIRSB(sb, store);

	if (arg1->tag == Iex_RdTmp) {
		if (arg1tmpInstead >= 0) {
			store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->arg1)), mkU32(arg1tmpInstead));
		} else {
			store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->arg1)), mkU32(arg1->Iex.RdTmp.tmp));
		}
		addStmtToIRSB(sb, store);

		writeSTemp(sb, env, arg1->Iex.RdTmp.tmp, 0);
	} else {
		tl_assert(arg1->tag == Iex_Const);

		writeSConst(sb, arg1->Iex.Const.con, 0);
		constArgs |= 0x1;
	}
	if (arg2->tag == Iex_RdTmp) {
		if (arg2tmpInstead >= 0) {
			store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->arg2)), mkU32(arg2tmpInstead));
		} else {
			store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->arg2)), mkU32(arg2->Iex.RdTmp.tmp));
		}
		addStmtToIRSB(sb, store);

		writeSTemp(sb, env, arg2->Iex.RdTmp.tmp, 1);
	} else {
		tl_assert(arg2->tag == Iex_Const);

		writeSConst(sb, arg2->Iex.Const.con, 1);
		constArgs |= 0x2;
	}

	IRExpr** argv = mkIRExprVec_2(mkU64(addr), mkU64(constArgs));
	IRDirty* di = unsafeIRDirty_1_N(wrTemp, 2, "processCmpF64", VG_(fnptr_to_fnentry)(&processCmpF64), argv);
	addStmtToIRSB(sb, IRStmt_Dirty(di));
}

static Double processCvtOpKernel(Addr addr, UWord ca) {
	Int constArgs = (Int)ca;

	if (clo_simulateOriginal) {
		if (isOpFloat(binOpArgs->op)) {
			mpfr_set_prec(arg2tmpX, 24);
		} else {
			mpfr_set_prec(arg2tmpX, 53);
		}
	}

	if (isOpFloat(binOpArgs->op)) {
		mpfr_set_prec(arg2midX, 24);
		mpfr_set_prec(arg2oriX, 24);
	} else {
		mpfr_set_prec(arg2midX, 53);
		mpfr_set_prec(arg2oriX, 53);
	}

	ULong arg2opCount = 0;
	Addr arg2origin = 0;
	mpfr_exp_t arg2canceled = 0;
	mpfr_exp_t canceled = 0;
	Addr arg2CancelOrigin = 0;
	mpfr_t irel2;
	mpfr_inits(irel2, NULL);

	Int exactBitsArg1, exactBitsArg2;
	if (isOpFloat(binOpArgs->op)) {
		exactBitsArg2 = 23;
	} else {
		exactBitsArg2 = 52;
	}

	if (constArgs & 0x2) {
		readSConst(1, &(arg2tmpX));
		mpfr_set(arg2midX, arg2tmpX, STD_RND);
		beginEmulateDouble();
		int t = mpfr_set(arg2oriX, arg2tmpX, STD_RND);
		mpfr_subnormalize(arg2oriX, t, STD_RND);
		endEmulate();
	} else {
		ShadowValue* arg2tmp = getTemp(binOpArgs->arg2);
		checkAndRecover(arg2tmp);
		computeRelativeError(arg2tmp, irel2);
		if (arg2tmp) {
			mpfr_set(arg2tmpX, arg2tmp->value, STD_RND);
			mpfr_set(arg2midX, arg2tmp->midValue, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg2oriX, arg2tmp->oriValue, STD_RND);
			mpfr_subnormalize(arg2oriX, t, STD_RND);
			endEmulate();
			arg2opCount = arg2tmp->opCount;
			arg2origin = arg2tmp->origin;
			arg2canceled = arg2tmp->canceled;
			arg2CancelOrigin = arg2tmp->cancelOrigin;

			if (clo_bad_cancellations) {
				readSTemp(1, &cancelTemp);
				if (mpfr_get_exp(cancelTemp) == mpfr_get_exp(arg2tmpX)) {
					mpfr_sub(cancelTemp, arg2tmpX, cancelTemp, STD_RND);
					if (mpfr_cmp_ui(cancelTemp, 0) != 0) {
						exactBitsArg2 = abs(mpfr_get_exp(arg2tmpX) - mpfr_get_exp(cancelTemp)) - 2;
						if (arg2tmp->orgType == Ot_FLOAT && exactBitsArg2 > 23) {
							exactBitsArg2 = 23;
						} else if (arg2tmp->orgType == Ot_DOUBLE && exactBitsArg2 > 52) {
							exactBitsArg2 = 52;
						}
					}
				} else {
					exactBitsArg2 = 0;
				}
			}
		} else {
			readSTemp(1, &(arg2tmpX));
			mpfr_set(arg2midX, arg2tmpX, STD_RND);
			beginEmulateDouble();
			int t = mpfr_set(arg2oriX, arg2tmpX, STD_RND);
			mpfr_subnormalize(arg2oriX, t, STD_RND);
			endEmulate();
		}
	}

	return mpfr_get_d(arg2tmpX, STD_RND);
}

static VG_REGPARM(2) UInt processCvtI32U(Addr addr, UWord ca) {
	if (!clo_analyze) return;
	Double shadow_double = processCvtOpKernel(addr, ca);
	return shadow_double;
}

static VG_REGPARM(2) Int processCvtI32S(Addr addr, UWord ca) {
	if (!clo_analyze) return;
	Double shadow_double = processCvtOpKernel(addr, ca);
	return shadow_double;
}

static VG_REGPARM(2) ULong processCvtI64U(Addr addr, UWord ca) {
	if (!clo_analyze) return;
	Double shadow_double = processCvtOpKernel(addr, ca);
	return shadow_double;
}

static VG_REGPARM(2) Long processCvtI64S(Addr addr, UWord ca) {
	if (!clo_analyze) return;
	Double shadow_double = processCvtOpKernel(addr, ca);
	return shadow_double;
}

static VG_REGPARM(2) Short processCvtI16S(Addr addr, UWord ca) {
	if (!clo_analyze) return;
	Double shadow_double = processCvtOpKernel(addr, ca);
	return shadow_double;
}

static void instrumentCvtOp (IRSB* sb, IRTypeEnv* env, Addr addr, IRTemp wrTemp, IRExpr* binop, Int arg2tmpInstead, RetType retType) {
	tl_assert(binop->tag == Iex_Binop);

	if (clo_ignoreLibraries && isInLibrary(addr)) {
		return;
	}

	IROp op = binop->Iex.Binop.op;
	IRExpr* arg2 = binop->Iex.Binop.arg2;

	tl_assert(arg2->tag == Iex_RdTmp || arg2->tag == Iex_Const);

	Int constArgs = 0;

	IRStmt* store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->op)), mkU32(op));
	addStmtToIRSB(sb, store);

	if (arg2->tag == Iex_RdTmp) {
		if (arg2tmpInstead >= 0) {
			store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->arg2)), mkU32(arg2tmpInstead));
		} else {
			store = IRStmt_Store(Iend_LE, mkU64(&(binOpArgs->arg2)), mkU32(arg2->Iex.RdTmp.tmp));
		}
		addStmtToIRSB(sb, store);

		writeSTemp(sb, env, arg2->Iex.RdTmp.tmp, 1);
	} else {
		tl_assert(arg2->tag == Iex_Const);

		writeSConst(sb, arg2->Iex.Const.con, 1);
		constArgs |= 0x2;
	}

	IRExpr** argv = mkIRExprVec_2(mkU64(addr), mkU64(constArgs));
	IRDirty* di;
	switch(retType) {
		case Rt_I16S:
			di = unsafeIRDirty_1_N(wrTemp, 2, "processCvtI16S", VG_(fnptr_to_fnentry)(&processCvtI16S), argv);
			break;
		case Rt_I32S:
			di = unsafeIRDirty_1_N(wrTemp, 2, "processCvtI32S", VG_(fnptr_to_fnentry)(&processCvtI32S), argv);
			break;
		case Rt_I64S:
			di = unsafeIRDirty_1_N(wrTemp, 2, "processCvtI64S", VG_(fnptr_to_fnentry)(&processCvtI64S), argv);
			break;
		case Rt_I32U:
			di = unsafeIRDirty_1_N(wrTemp, 2, "processCvtI32U", VG_(fnptr_to_fnentry)(&processCvtI32U), argv);
			break;
		case Rt_I64U:
			di = unsafeIRDirty_1_N(wrTemp, 2, "processCvtI64U", VG_(fnptr_to_fnentry)(&processCvtI64U), argv);
			break;
		default:
			VG_(tool_panic)("Should not reach here\n");
	}
	
	addStmtToIRSB(sb, IRStmt_Dirty(di));
}

static VG_REGPARM(1) void processMux0X(UWord ca) {
	// VG_(umsg)("processMux0X\n");
	if (!clo_analyze) return;

	Int constArgs = (Int)ca;
	ShadowValue *aexpr0 = NULL;
	ShadowValue *aexprX = NULL;

	if (constArgs & 0x2) {
		if (!muxArgs->condVal) {
			return;
		}
	} else {
		aexpr0 = getTemp(muxArgs->expr0);
		if (!aexpr0 && !muxArgs->condVal) {
			return;
		}
	}

	if (constArgs & 0x4) {
		if (muxArgs->condVal) {
			return;
		}
	} else {
		aexprX = getTemp(muxArgs->exprX);
		if (!aexprX && muxArgs->condVal) {
			return;
		}
	}

	ShadowValue* res = setTemp(muxArgs->wrTmp);
	// VG_(umsg)("processMux0X %X\n", muxArgs->wrTmp);
	if (muxArgs->condVal) {
		copyShadowValue(res, aexprX);
	} else {
		copyShadowValue(res, aexpr0);
	}
}

static void instrumentMux0X(IRSB* sb, IRTypeEnv* env, IRTemp wrTemp, IRExpr* mux, Int arg0tmpInstead, Int argXtmpInstead) {
	tl_assert(mux->tag == Iex_Mux0X);

	IRExpr* cond = mux->Iex.Mux0X.cond;
	IRExpr* expr0 = mux->Iex.Mux0X.expr0;
	IRExpr* exprX = mux->Iex.Mux0X.exprX;

	tl_assert(cond->tag == Iex_RdTmp);
	tl_assert(expr0->tag == Iex_RdTmp || expr0->tag == Iex_Const);
	tl_assert(exprX->tag == Iex_RdTmp || exprX->tag == Iex_Const);

	Int constArgs = 0;
	IRStmt* store = IRStmt_Store(Iend_LE, mkU64(&(muxArgs->wrTmp)), mkU32(wrTemp));
	addStmtToIRSB(sb, store);
	store = IRStmt_Store(Iend_LE, mkU64(&(muxArgs->condVal)), mux->Iex.Mux0X.cond);
	addStmtToIRSB(sb, store);

	if (expr0->tag == Iex_RdTmp) {
		if (arg0tmpInstead >= 0) {
			store = IRStmt_Store(Iend_LE, mkU64(&(muxArgs->expr0)), mkU32(arg0tmpInstead));
		} else {
			store = IRStmt_Store(Iend_LE, mkU64(&(muxArgs->expr0)), mkU32(mux->Iex.Mux0X.expr0->Iex.RdTmp.tmp));
		}
		addStmtToIRSB(sb, store);
	} else {
		constArgs |= 0x2;
	}

	if (exprX->tag == Iex_RdTmp) {
		if (argXtmpInstead >= 0) {
			store = IRStmt_Store(Iend_LE, mkU64(&(muxArgs->exprX)), mkU32(argXtmpInstead));
		} else {
			store = IRStmt_Store(Iend_LE, mkU64(&(muxArgs->exprX)), mkU32(mux->Iex.Mux0X.exprX->Iex.RdTmp.tmp));
		}
		addStmtToIRSB(sb, store);
	} else {
		constArgs |= 0x4;
	}

	IRExpr** argv = mkIRExprVec_1(mkU64(constArgs));
	IRDirty* di = unsafeIRDirty_0_N(1, "processMux0X", VG_(fnptr_to_fnentry)(&processMux0X), argv);
	addStmtToIRSB(sb, IRStmt_Dirty(di));
}

static VG_REGPARM(2) void processLoad(UWord tmp, Addr addr) {
	if (!clo_analyze) return;

	/* check if this memory address is shadowed */
	ShadowValue* av = VG_(HT_lookup)(globalMemory, addr);
	if (!av || !(av->active)) {
		return;
	}
	// VG_(umsg)("processLoad %X\n", tmp);
	// VG_(umsg)("load address: %lX\n", addr);
	ShadowValue* res = setTemp(tmp);
	copyShadowValue(res, av);
	// VG_(umsg)("av\n");
	// printErrorShort(av);
	// VG_(umsg)("res\n");
	// printErrorShort(res);
}

static void instrumentLoad(IRSB* sb, IRTypeEnv* env, IRStmt* wrTmp) {
	tl_assert(wrTmp->tag == Ist_WrTmp);
	tl_assert(wrTmp->Ist.WrTmp.data->tag == Iex_Load);
	IRExpr* load = wrTmp->Ist.WrTmp.data;

	if (load->Iex.Load.addr->tag != Iex_RdTmp) {
		return;
	}
	
	IRExpr** argv = mkIRExprVec_2(mkU64(wrTmp->Ist.WrTmp.tmp), load->Iex.Load.addr);
	IRDirty* di = unsafeIRDirty_0_N(2, "processLoad", VG_(fnptr_to_fnentry)(&processLoad), argv);
	addStmtToIRSB(sb, IRStmt_Dirty(di));
}

static VG_REGPARM(3) void processStore(Addr addr, UWord t, UWord isFloat) {
	// VG_(umsg)("processStore\n");
	Int tmp = (Int)t;
	ShadowValue* res = NULL;
	ShadowValue* currentVal = VG_(HT_lookup)(globalMemory, addr);

	if (clo_analyze && tmp >= 0) {
		/* check if this memory address is shadowed */
		ShadowValue* av = getTemp(tmp);
		// VG_(umsg)("processStore %X\n", tmp);
		// VG_(umsg)("Store address: %lX\n", addr);
		// Addr lastAddr = addr - 4;

		if (av) {
			// VG_(umsg)("processStore2 %X\n", tmp);
			if (currentVal) {
				res = currentVal;
				copyShadowValue(res, av);
				res->active = True;
			} else {
				res = initShadowValue((UWord)addr);
				copyShadowValue(res, av);
				VG_(HT_add_node)(globalMemory, res);
				// VG_(umsg)("processStore create\n");
			}

			if ((Bool)isFloat) {
				res->orgType = Ot_FLOAT;
			} else {
				res->orgType = Ot_DOUBLE;
			}
			if (res->orgType == Ot_FLOAT) {
				res->Org.fl = storeArgs->orgFloat;
			} else if (res->orgType == Ot_DOUBLE) {
				res->Org.db = storeArgs->orgDouble;
			} else {
				tl_assert(False);
			}
	
			if (activeStages > 0) {
				updateStages(addr, res->orgType == Ot_FLOAT);
			}
		}
	}

	if (currentVal && !res) {
		currentVal->active = False;
	}
}

static void instrumentStore(IRSB* sb, IRTypeEnv* env, IRStmt* store, Int argTmpInstead) {
	tl_assert(store->tag == Ist_Store);

	Bool isFloat = True;
	IRExpr* data = store->Ist.Store.data;
	if (data->tag == Iex_RdTmp) {
		/* I32 and I64 have to be instrumented due to SSE */
		switch (typeOfIRTemp(env, data->Iex.RdTmp.tmp)) {
			case Ity_I64:
			case Ity_F64:
			case Ity_V128:
				isFloat = False;
				break;
			default:
				break;
		}
	}

	IRExpr* addr = store->Ist.Store.addr;
	/* const needed, but only to delete */
	tl_assert(data->tag == Iex_RdTmp || data->tag == Iex_Const);

	Int num = -1;
	IRType type = Ity_F32;
	if (data->tag != Iex_Const) {
		if (argTmpInstead >= 0) {
			num = argTmpInstead;
		} else {
			num = data->Iex.RdTmp.tmp;
		}
		type = typeOfIRTemp(env, num);

		if (isFloat) {
			IRStmt* store = IRStmt_Store(Iend_LE, mkU64(&(storeArgs->orgFloat)), IRExpr_RdTmp(data->Iex.RdTmp.tmp));
			addStmtToIRSB(sb, store);
		} else {
			IRStmt* store = IRStmt_Store(Iend_LE, mkU64(&(storeArgs->orgDouble)), IRExpr_RdTmp(data->Iex.RdTmp.tmp));
			addStmtToIRSB(sb, store);
		}
	}
	
	IRExpr** argv = mkIRExprVec_3(addr, mkU64(num), mkU64(isFloat));
	IRDirty* di = unsafeIRDirty_0_N(3, "processStore", VG_(fnptr_to_fnentry)(&processStore), argv);
	addStmtToIRSB(sb, IRStmt_Dirty(di));
}

static VG_REGPARM(2) void processPut(UWord offset, UWord t) {
	// VG_(umsg)("processPut\n");
	ShadowValue* res = NULL;
	ThreadId tid = VG_(get_running_tid)();
	ShadowValue* currentVal = threadRegisters[tid][offset];

	Int tmp = (Int)t;
	if (clo_analyze && tmp >= 0) {
		/* check if a shadow value exits */
		ShadowValue* av = getTemp(tmp);
		if (av) {
			if (currentVal) {
				/* reuse allocated space if possible ... */
				res = currentVal;
				copyShadowValue(res, av);
			} else {
				/* ... else allocate new space */
				res = initShadowValue((UWord)offset);
				copyShadowValue(res, av);
				threadRegisters[tid][offset] = res;
			}
			res->active = True;
		}
	}

	if (currentVal && !res) {
		/* Invalidate existing shadow value (not free) because 
           something was stored in this register */
		currentVal->active = False;
	}
}

static void instrumentPut(IRSB* sb, IRTypeEnv* env, IRStmt* st, Int argTmpInstead) {
	tl_assert(st->tag == Ist_Put);
	IRExpr* data = st->Ist.Put.data;
	tl_assert(data->tag == Iex_RdTmp || data->tag == Iex_Const);

	Int offset = st->Ist.Put.offset;
	tl_assert(offset >= 0 && offset < MAX_REGISTERS);

	Int tmpNum = -1;
	if (data->tag == Iex_RdTmp) {
		if (argTmpInstead >= 0) {
			tmpNum = argTmpInstead;
		} else {
			tmpNum = data->Iex.RdTmp.tmp;
		}
	}

	IRExpr** argv = mkIRExprVec_2(mkU64(offset), mkU64(tmpNum));
	IRDirty* di = unsafeIRDirty_0_N(2, "processPut", VG_(fnptr_to_fnentry)(&processPut), argv);
	addStmtToIRSB(sb, IRStmt_Dirty(di));
}

static VG_REGPARM(2) void processGet(UWord offset, UWord tmp) {
	// VG_(umsg)("processGet\n");
	if (!clo_analyze) return;

	ThreadId tid = VG_(get_running_tid)();
	ShadowValue* av = threadRegisters[tid][offset];
	if (!av || !(av->active)) {
		return;
	}

	ShadowValue* res = setTemp((Int)tmp);
	// VG_(umsg)("processGet %X\n", tmp);
	copyShadowValue(res, av);
}

static void instrumentGet(IRSB* sb, IRTypeEnv* env, IRStmt* st) {
	tl_assert(st->tag == Ist_WrTmp);
	tl_assert(st->Ist.WrTmp.data->tag == Iex_Get);

	Int tmpNum = st->Ist.WrTmp.tmp;
	Int offset = st->Ist.WrTmp.data->Iex.Get.offset;
	tl_assert(offset >= 0 && offset < MAX_REGISTERS);

	IRExpr** argv = mkIRExprVec_2(mkU64(offset), mkU64(tmpNum));
	IRDirty* di = unsafeIRDirty_0_N(2, "processGet", VG_(fnptr_to_fnentry)(&processGet), argv);
	addStmtToIRSB(sb, IRStmt_Dirty(di));
}

static VG_REGPARM(3) void processPutI(UWord t, UWord b, UWord n) {
	// VG_(umsg)("processPutI\n");
	Int tmp = (Int)t;
	Int nElems = (Int)n;
	Int base = (Int)b;
	Int bias = (Int)(circRegs->bias);

	/* (ix + bias) % num-of-elems-in-the-array */
	Int offset = base + ((circRegs->ix + bias) % nElems);
	tl_assert(offset >= 0 && offset < MAX_REGISTERS);

	ShadowValue* res = NULL;
	ThreadId tid = VG_(get_running_tid)();
	ShadowValue* currentVal = threadRegisters[tid][offset];

	if (clo_analyze && tmp >= 0) {
		/* check if a shadow value exits */
		ShadowValue* av = getTemp(tmp);
		if (av) {
			if (currentVal) {
				/* reuse allocated space if possible ... */
				res = currentVal;
				copyShadowValue(res, av);
			} else {
				/* ... else allocate new space */
				res = initShadowValue((UWord)offset);
				copyShadowValue(res, av);
				threadRegisters[tid][offset] = res;
			}
			res->active = True;
		}
	}

	if (currentVal && !res) {
		/* Invalidate existing shadow value (not free) because 
           something was stored in this register. */
		currentVal->active = False;
	}
}

static void instrumentPutI(IRSB* sb, IRTypeEnv* env, IRStmt* st, Int argTmpInstead) {
	tl_assert(st->tag == Ist_PutI);
	IRExpr* data = st->Ist.PutI.data;
	IRExpr* ix = st->Ist.PutI.ix;
	IRRegArray* descr = st->Ist.PutI.descr;
	Int bias = st->Ist.PutI.bias;

	tl_assert(data->tag == Iex_RdTmp || data->tag == Iex_Const);
	tl_assert(ix->tag == Iex_RdTmp || ix->tag == Iex_Const);

	tl_assert(ix->tag == Iex_RdTmp ? typeOfIRTemp(env, ix->Iex.RdTmp.tmp) == Ity_I32 : True);
	tl_assert(ix->tag == Iex_Const ? ix->Iex.Const.con->tag == Ico_U32 : True);

	Int tmpNum = -1;
	if (data->tag == Iex_RdTmp) {
		if (argTmpInstead >= 0) {
			tmpNum = argTmpInstead;
		} else {
			tmpNum = data->Iex.RdTmp.tmp;
		}
	}

	IRStmt* store = IRStmt_Store(Iend_LE, mkU64(&(circRegs->bias)), mkU64(bias));
	addStmtToIRSB(sb, store);
	store = IRStmt_Store(Iend_LE, mkU64(&(circRegs->ix)), ix);
	addStmtToIRSB(sb, store);

	IRExpr** argv = mkIRExprVec_3(mkU64(tmpNum), mkU64(descr->base), mkU64(descr->nElems));
	IRDirty* di = unsafeIRDirty_0_N(3, "processPutI", VG_(fnptr_to_fnentry)(&processPutI), argv);
	addStmtToIRSB(sb, IRStmt_Dirty(di));
}

static VG_REGPARM(3) void processGetI(UWord tmp, UWord b, UWord n) {
	// VG_(umsg)("processGetI\n");
	if (!clo_analyze) return;

	Int nElems = (Int)n;
	Int base = (Int)b;
	Int bias = (Int)(circRegs->bias);

	/* (ix + bias) % num-of-elems-in-the-array */
	Int offset = base + ((circRegs->ix + bias) % nElems);
	tl_assert(offset >= 0 && offset < MAX_REGISTERS);

	ThreadId tid = VG_(get_running_tid)();
	ShadowValue* av = threadRegisters[tid][offset];
	if (!av || !(av->active)) {
		return;
	}

	ShadowValue* res = setTemp((Int)tmp);
	// VG_(umsg)("processGetI %X\n", tmp);
	copyShadowValue(res, av);
}

static void instrumentGetI(IRSB* sb, IRTypeEnv* env, IRStmt* st) {
	tl_assert(st->tag == Ist_WrTmp);
	tl_assert(st->Ist.WrTmp.data->tag == Iex_GetI);
	IRExpr* get = st->Ist.WrTmp.data;

	IRExpr* ix = get->Iex.GetI.ix;
	IRRegArray* descr = get->Iex.GetI.descr;
	Int bias = get->Iex.GetI.bias;

	tl_assert(ix->tag == Iex_RdTmp || ix->tag == Iex_Const);
	tl_assert(ix->tag == Iex_RdTmp ? typeOfIRTemp(env, ix->Iex.RdTmp.tmp) == Ity_I32 : True);
	tl_assert(ix->tag == Iex_Const ? ix->Iex.Const.con->tag == Ico_U32 : True);

	Int tmpNum = st->Ist.WrTmp.tmp;

	IRStmt* store = IRStmt_Store(Iend_LE, mkU64(&(circRegs->bias)), mkU64(bias));
	addStmtToIRSB(sb, store);
	store = IRStmt_Store(Iend_LE, mkU64(&(circRegs->ix)), ix);
	addStmtToIRSB(sb, store);

	IRExpr** argv = mkIRExprVec_3(mkU64(tmpNum), mkU64(descr->base), mkU64(descr->nElems));
	IRDirty* di = unsafeIRDirty_0_N(3, "processGetI", VG_(fnptr_to_fnentry)(&processGetI), argv);
	addStmtToIRSB(sb, IRStmt_Dirty(di));
}

static void instrumentEnterSB(IRSB* sb) {
	/* inlining of sbExecuted++ */
	IRExpr* load = IRExpr_Load(Iend_LE, Ity_I64, mkU64(&sbExecuted));
	IRTemp t1 = newIRTemp(sb->tyenv, Ity_I64);
  	addStmtToIRSB(sb, IRStmt_WrTmp(t1, load));
	IRExpr* add = IRExpr_Binop(Iop_Add64, IRExpr_RdTmp(t1), mkU64(1));
  	IRTemp t2 = newIRTemp(sb->tyenv, Ity_I64);
  	addStmtToIRSB(sb, IRStmt_WrTmp(t2, add));
	IRStmt* store = IRStmt_Store(Iend_LE, mkU64(&sbExecuted), IRExpr_RdTmp(t2));
	addStmtToIRSB(sb, store);
}

static void reportUnsupportedOp(IROp op) {
	if (!VG_(OSetWord_Contains)(unsupportedOps, (UWord)op)) {
		VG_(OSetWord_Insert)(unsupportedOps, (UWord)op);
	}
}

static IRSB* fd_instrument(VgCallbackClosure* closure, IRSB* sbIn,
                      VexGuestLayout* layout, VexGuestExtents* vge,
                      IRType gWordTy, IRType hWordTy)
{
	Int			i;
	IRSB*      	sbOut;
   	IRType     	type;
   	IRTypeEnv* 	tyenv = sbIn->tyenv;
	IRExpr* 	expr;
	/* address of current client instruction */
	Addr		cia = 0, dst;

	if (gWordTy != hWordTy) {
		/* This case is not supported yet. */
		VG_(tool_panic)("host/guest word size mismatch");
	}

	sbCounter++;
	totalIns += sbIn->stmts_used;

	/* set up SB */
	sbOut = deepCopyIRSBExceptStmts(sbIn);

	/* copy verbatim any IR preamble preceding the first IMark */
	i = 0;
	while (i < sbIn->stmts_used && sbIn->stmts[i]->tag != Ist_IMark) {
		addStmtToIRSB( sbOut, sbIn->stmts[i] );
		i++;
	}

	/* perform optimizations for each superblock */

	Bool sbInstrNeeded = False;
	if (maxTemps < tyenv->types_used) {
		maxTemps = tyenv->types_used;
	}

	Int j;

	Bool impReg[MAX_REGISTERS];
	for (j = 0; j < MAX_REGISTERS; j++) {
		impReg[j] = True;
	}
	Int impTmp[tyenv->types_used];
	for (j = 0; j < tyenv->types_used; j++) {
		impTmp[j] = 0;
	}

	/* backward */
	for (j = sbIn->stmts_used - 1; j >= i; j--) {
		IRStmt* st = sbIn->stmts[j];
		if (!st || st->tag == Ist_NoOp) continue;
      
		switch (st->tag) {
			case Ist_Put:
				impReg[st->Ist.Put.offset] = False;

				if (st->Ist.Put.data->tag == Iex_RdTmp) {
					impTmp[st->Ist.Put.data->Iex.RdTmp.tmp] = 1;
				}
				break;
			case Ist_Store:
				if (st->Ist.Store.data->tag == Iex_RdTmp) {
					impTmp[st->Ist.Store.data->Iex.RdTmp.tmp] = 1;
				}
				break;
			case Ist_WrTmp:
				expr = st->Ist.WrTmp.data;

		        switch (expr->tag) {
					case Iex_Get:
						impReg[expr->Iex.Get.offset] = True;
						break;
					case Iex_Unop:
						switch (expr->Iex.Unop.op) {
							case Iop_Sqrt32F0x4:
							case Iop_Sqrt64F0x2:
							case Iop_NegF32:
							case Iop_NegF64:
							case Iop_AbsF32:
							case Iop_AbsF64:
							case Iop_F32toF64:
							case Iop_ReinterpI64asF64:
							case Iop_32UtoV128:
							case Iop_V128to64:
							case Iop_V128HIto64:
							case Iop_64to32:
							case Iop_64HIto32:
							case Iop_64UtoV128:
							case Iop_32Uto64:
								if (expr->Iex.Unop.arg->tag == Iex_RdTmp) {
									impTmp[expr->Iex.Unop.arg->Iex.RdTmp.tmp] = 1;
								}
								break;
							default:
								/* backward -> args are important */
								if (expr->Iex.Unop.arg->tag == Iex_RdTmp && impTmp[expr->Iex.Unop.arg->Iex.RdTmp.tmp] == 0) {
									impTmp[expr->Iex.Unop.arg->Iex.RdTmp.tmp] = -1;
								}
								break;
						}
						break;
					case Iex_Binop:
						switch (expr->Iex.Binop.op) {
							case Iop_Add32F0x4:
							case Iop_Sub32F0x4:
							case Iop_Mul32F0x4:
							case Iop_Div32F0x4:
							case Iop_Add64F0x2:
							case Iop_Sub64F0x2:
							case Iop_Mul64F0x2:
							case Iop_Div64F0x2:
							case Iop_Min32F0x4:
							case Iop_Min64F0x2:
							case Iop_Max32F0x4:
							case Iop_Max64F0x2:
							case Iop_CmpF64: 
							case Iop_F64toF32:
							case Iop_64HLtoV128:
							case Iop_32HLto64:
								if (expr->Iex.Binop.arg1->tag == Iex_RdTmp) {
									impTmp[expr->Iex.Binop.arg1->Iex.RdTmp.tmp] = 1;
								}
							case Iop_F64toI16S:
							case Iop_F64toI32S:
							case Iop_F64toI64S:
							case Iop_F64toI64U:
							case Iop_F64toI32U:
								if (expr->Iex.Binop.arg2->tag == Iex_RdTmp) {
									impTmp[expr->Iex.Binop.arg2->Iex.RdTmp.tmp] = 1;
								}
								break;
							default:
								/* backward -> args are important */
								if (expr->Iex.Binop.arg1->tag == Iex_RdTmp && impTmp[expr->Iex.Binop.arg1->Iex.RdTmp.tmp] == 0) {
									impTmp[expr->Iex.Binop.arg1->Iex.RdTmp.tmp] = -1;
								}
								if (expr->Iex.Binop.arg2->tag == Iex_RdTmp && impTmp[expr->Iex.Binop.arg2->Iex.RdTmp.tmp] == 0) {
									impTmp[expr->Iex.Binop.arg2->Iex.RdTmp.tmp] = -1;
								}
								break;
						}
						break;
					case Iex_Triop:
						switch (expr->Iex.Triop.op) {
							case Iop_AddF64:
							case Iop_SubF64:
							case Iop_MulF64:
							case Iop_DivF64:
								if (expr->Iex.Triop.arg2->tag == Iex_RdTmp) {
									impTmp[expr->Iex.Triop.arg2->Iex.RdTmp.tmp] = 1;
								}
								if (expr->Iex.Triop.arg3->tag == Iex_RdTmp) {
									impTmp[expr->Iex.Triop.arg3->Iex.RdTmp.tmp] = 1;
								}
								break;
							default:
								/* backward -> args are important */
								if (expr->Iex.Triop.arg2->tag == Iex_RdTmp && impTmp[expr->Iex.Triop.arg2->Iex.RdTmp.tmp] == 0) {
									impTmp[expr->Iex.Triop.arg2->Iex.RdTmp.tmp] = -1;
								}
								if (expr->Iex.Triop.arg3->tag == Iex_RdTmp && impTmp[expr->Iex.Triop.arg3->Iex.RdTmp.tmp] == 0) {
									impTmp[expr->Iex.Triop.arg3->Iex.RdTmp.tmp] = -1;
								}
								break;
						}
						break;
					case Iex_Mux0X:
						/* nothing, impTmp is already true */
						break;
					default:
						break;
				}
				break;
			default:
				break;
		}
	}

	Int tmpInstead[tyenv->types_used];
	for (j = 0; j < tyenv->types_used; j++) {
		tmpInstead[j] = -1;
	}

	Int tmpInReg[MAX_REGISTERS];
	for (j = 0; j < MAX_REGISTERS; j++) {
		tmpInReg[j] = -1;
	}

	/* forward */
	for (j = i; j < sbIn->stmts_used; j++) {
		IRStmt* st = sbIn->stmts[j];
		if (!st || st->tag == Ist_NoOp) continue;
      
		switch (st->tag) {
			case Ist_Put:
				if (st->Ist.Put.data->tag == Iex_RdTmp) {
					tmpInReg[st->Ist.Put.offset] = st->Ist.Put.data->Iex.RdTmp.tmp;
				} else {
					tmpInReg[st->Ist.Put.offset] = -1;
				}
				break;
			case Ist_Store:
				break;
			case Ist_WrTmp:
				expr = st->Ist.WrTmp.data;
		        switch (expr->tag) {
					case Iex_Load:
						break;
					case Iex_Get:
						if (tmpInReg[expr->Iex.Get.offset] >= 0) {
							if (tmpInstead[tmpInReg[expr->Iex.Get.offset]] >= 0) {
								tmpInstead[st->Ist.WrTmp.tmp] = tmpInstead[tmpInReg[expr->Iex.Get.offset]];
							} else {
								tmpInstead[st->Ist.WrTmp.tmp] = tmpInReg[expr->Iex.Get.offset];
							}
						}
						break;
					case Iex_RdTmp:
						tmpInstead[st->Ist.WrTmp.tmp] = tmpInstead[expr->Iex.RdTmp.tmp];
						break;
					case Iex_Unop:
						switch (expr->Iex.Unop.op) {
							case Iop_F32toF64:
							case Iop_ReinterpI64asF64:
							case Iop_32UtoV128:
							case Iop_V128to64:
							case Iop_V128HIto64:
							case Iop_64to32:
							case Iop_64HIto32:
							case Iop_64UtoV128:
							case Iop_32Uto64:
								if (expr->Iex.Unop.arg->tag == Iex_RdTmp) {
									if (tmpInstead[expr->Iex.Unop.arg->Iex.RdTmp.tmp] >= 0) {
										tmpInstead[st->Ist.WrTmp.tmp] = tmpInstead[expr->Iex.Unop.arg->Iex.RdTmp.tmp];
									} else {
										tmpInstead[st->Ist.WrTmp.tmp] = expr->Iex.Unop.arg->Iex.RdTmp.tmp;
									}
								}
								break;
							default:
								break;
						}
						break;
					case Iex_Binop:
						switch (expr->Iex.Binop.op) {
							case Iop_F64toF32:
								if (expr->Iex.Binop.arg2->tag == Iex_RdTmp) {
									if (tmpInstead[expr->Iex.Binop.arg2->Iex.RdTmp.tmp] >= 0) {
										tmpInstead[st->Ist.WrTmp.tmp] = tmpInstead[expr->Iex.Binop.arg2->Iex.RdTmp.tmp]; 
									} else {
										tmpInstead[st->Ist.WrTmp.tmp] = expr->Iex.Binop.arg2->Iex.RdTmp.tmp;
									}
								}
								break;
							case Iop_64HLtoV128:
							case Iop_32HLto64:
								if (expr->Iex.Binop.arg1->tag == Iex_RdTmp) {
									if (tmpInstead[expr->Iex.Binop.arg1->Iex.RdTmp.tmp] >= 0) {
										tmpInstead[st->Ist.WrTmp.tmp] = tmpInstead[expr->Iex.Binop.arg1->Iex.RdTmp.tmp]; 
									} else {
										tmpInstead[st->Ist.WrTmp.tmp] = expr->Iex.Binop.arg1->Iex.RdTmp.tmp;
									}
								} else if (expr->Iex.Binop.arg2->tag == Iex_RdTmp) {
									if (tmpInstead[expr->Iex.Binop.arg2->Iex.RdTmp.tmp] >= 0) {
										tmpInstead[st->Ist.WrTmp.tmp] = tmpInstead[expr->Iex.Binop.arg2->Iex.RdTmp.tmp]; 
									} else {
										tmpInstead[st->Ist.WrTmp.tmp] = expr->Iex.Binop.arg2->Iex.RdTmp.tmp;
									}
								}
								break;
							default:
								break;
						}
						break;
					default:
						break;
				}
				break;
			default:
				break;
		}
	}

	instrumentEnterSB(sbOut);

	Int arg1tmpInstead = -1;
	Int arg2tmpInstead = -1;
	RetType retType;

	/* This is the main loop which hads instructions for the analysis (instrumentation).*/
	for (/*use current i*/; i < sbIn->stmts_used; i++) {
		IRStmt* st = sbIn->stmts[i];
		if (!st || st->tag == Ist_NoOp) continue;
      
		switch (st->tag) {
			case Ist_AbiHint:
				addStmtToIRSB(sbOut, st);
				break;
			case Ist_Put:
				addStmtToIRSB(sbOut, st);
				putCount++;

				if (st->Ist.Put.offset != 168) {
					arg1tmpInstead = -1;
					if (st->Ist.Put.data->tag == Iex_RdTmp) {
						arg1tmpInstead = tmpInstead[st->Ist.Put.data->Iex.RdTmp.tmp];
					}
					instrumentPut(sbOut, tyenv, st, arg1tmpInstead);
				} else {
					putsIgnored++;
				}
				break;
			case Ist_PutI:
				addStmtToIRSB(sbOut, st);
				arg1tmpInstead = -1;
				if (st->Ist.PutI.data->tag == Iex_RdTmp) {
					arg1tmpInstead = tmpInstead[st->Ist.PutI.data->Iex.RdTmp.tmp];
				}
				instrumentPutI(sbOut, tyenv, st, arg1tmpInstead);
				break;
			case Ist_IMark:
				/* address of current instruction */
				cia = st->Ist.IMark.addr;
				addStmtToIRSB(sbOut, st);
				break;
			case Ist_Exit:
				addStmtToIRSB(sbOut, st);
				break;
			case Ist_WrTmp:
				expr = st->Ist.WrTmp.data;
		        type = typeOfIRExpr(sbOut->tyenv, expr);
		        tl_assert(type != Ity_INVALID);

		        switch (expr->tag) {
					case Iex_Const:
						addStmtToIRSB(sbOut, st);
						break;
					case Iex_Load:
						addStmtToIRSB(sbOut, st);
						loadCount++;
						instrumentLoad(sbOut, tyenv, st);
						break;
					case Iex_Get:
						addStmtToIRSB(sbOut, st);
						getCount++;

						if (tmpInstead[st->Ist.WrTmp.tmp] < 0) {
							instrumentGet(sbOut, tyenv, st);
						} else {
							getsIgnored++;
						}
						break;
					case Iex_GetI:
						addStmtToIRSB(sbOut, st);

						if (tmpInstead[st->Ist.WrTmp.tmp] < 0) {
							instrumentGetI(sbOut, tyenv, st);
						}
						break;
					case Iex_Unop:
						// opToStr(expr->Iex.Unop.op);
						// VG_(umsg)("una : %s\n", opStr);
						switch (expr->Iex.Unop.op) {
							case Iop_Sqrt32F0x4:
							case Iop_Sqrt64F0x2:
							case Iop_NegF32:
							case Iop_NegF64:
							case Iop_AbsF32:
							case Iop_AbsF64:
								addStmtToIRSB(sbOut, st);

								arg1tmpInstead = -1;
								if (expr->Iex.Unop.arg->tag == Iex_RdTmp) {
									arg1tmpInstead = tmpInstead[expr->Iex.Unop.arg->Iex.RdTmp.tmp];
								}
								instrumentUnOp(sbOut, tyenv, cia, st->Ist.WrTmp.tmp, expr, arg1tmpInstead);
								break;
							case Iop_F32toF64:
							case Iop_ReinterpI64asF64:
							case Iop_32UtoV128:
							case Iop_V128to64:
							case Iop_V128HIto64:
							case Iop_64to32:
							case Iop_64HIto32:
							case Iop_64UtoV128:
							case Iop_32Uto64:
								/* ignored floating-point and related SSE operations */
								addStmtToIRSB(sbOut, st);
								break;
							case Iop_Recip32Fx4:
        					case Iop_Sqrt32Fx4:
							case Iop_RSqrt32Fx4:
							case Iop_RoundF32x4_RM:
							case Iop_RoundF32x4_RP:
							case Iop_RoundF32x4_RN:
							case Iop_RoundF32x4_RZ:
							case Iop_Recip32F0x4:
							case Iop_RSqrt32F0x4:
							case Iop_Recip64Fx2:
							case Iop_Sqrt64Fx2:
							case Iop_RSqrt64Fx2:
							case Iop_Recip64F0x2:
							case Iop_RSqrt64F0x2:
							case Iop_SinF64:
							case Iop_CosF64:
							case Iop_TanF64:
							case Iop_2xm1F64:
							case Iop_Est5FRSqrt:
							case Iop_RoundF64toF64_NEAREST:
							case Iop_RoundF64toF64_NegINF:
							case Iop_RoundF64toF64_PosINF:
							case Iop_RoundF64toF64_ZERO:
							case Iop_TruncF64asF32:
								addStmtToIRSB(sbOut, st);
								reportUnsupportedOp(expr->Iex.Unop.op);
								break;
							default:
								addStmtToIRSB(sbOut, st);
								break;
						}
						break;
					case Iex_Binop:
						// opToStr(expr->Iex.Binop.op);
						// VG_(umsg)("bin : %s\n", opStr);
						switch (expr->Iex.Binop.op) {
							case Iop_CmpF64:
								if (clo_goto_shadow_branch) {
									arg1tmpInstead = -1;
									arg2tmpInstead = -1;
									if (expr->Iex.Binop.arg1->tag == Iex_RdTmp) {
										arg1tmpInstead = tmpInstead[expr->Iex.Binop.arg1->Iex.RdTmp.tmp];
									}
									if (expr->Iex.Binop.arg2->tag == Iex_RdTmp) {
										arg2tmpInstead = tmpInstead[expr->Iex.Binop.arg2->Iex.RdTmp.tmp];
									}
									instrumentCmpF64(sbOut, tyenv, cia, st->Ist.WrTmp.tmp, expr, arg1tmpInstead, arg2tmpInstead);
								} else {
									addStmtToIRSB(sbOut, st);
								}
								break;
							case Iop_Add32F0x4:
							case Iop_Add64F0x2:
							case Iop_Sub32F0x4:
							case Iop_Mul32F0x4:
							case Iop_Div32F0x4:
							case Iop_Sub64F0x2:
							case Iop_Mul64F0x2:
							case Iop_Div64F0x2:
							case Iop_Min32F0x4:
							case Iop_Min64F0x2:
							case Iop_Max32F0x4:
							case Iop_Max64F0x2:
								addStmtToIRSB(sbOut, st);

								arg1tmpInstead = -1;
								arg2tmpInstead = -1;
								if (expr->Iex.Binop.arg1->tag == Iex_RdTmp) {
									arg1tmpInstead = tmpInstead[expr->Iex.Binop.arg1->Iex.RdTmp.tmp];
								}
								if (expr->Iex.Binop.arg2->tag == Iex_RdTmp) {
									arg2tmpInstead = tmpInstead[expr->Iex.Binop.arg2->Iex.RdTmp.tmp];
								}
								instrumentBinOp(sbOut, tyenv, cia, st->Ist.WrTmp.tmp, expr, arg1tmpInstead, arg2tmpInstead);
								break;
							case Iop_F64toF32:
							case Iop_64HLtoV128:
							case Iop_32HLto64:
								/* ignored floating-point and related SSE operations */
								addStmtToIRSB(sbOut, st);
								break;
							case Iop_F64toI16S:
							case Iop_F64toI32S:
							case Iop_F64toI64S:
							case Iop_F64toI64U:
							case Iop_F64toI32U:
								if (clo_track_int) {
									arg2tmpInstead = -1;
									if (expr->Iex.Binop.arg2->tag == Iex_RdTmp) {
										arg2tmpInstead = tmpInstead[expr->Iex.Binop.arg2->Iex.RdTmp.tmp];
									}
									switch(expr->Iex.Binop.op) {
										case Iop_F64toI16S:
											retType = Rt_I16S;
											break;
										case Iop_F64toI32S:
											retType = Rt_I32S;
											break;
										case Iop_F64toI64S:
											retType = Rt_I64S;
											break;
										case Iop_F64toI64U:
											retType = Rt_I64U;
											break;
										case Iop_F64toI32U:
											retType = Rt_I32U;
											break;
										default:
											VG_(tool_panic)("Should not reach here\n");
									}
									instrumentCvtOp(sbOut, tyenv, cia, st->Ist.WrTmp.tmp, expr, arg2tmpInstead, retType);
								} else {
									addStmtToIRSB(sbOut, st);
								}
								break;
							case Iop_Add32Fx4:
							case Iop_Sub32Fx4:
							case Iop_Mul32Fx4:
							case Iop_Div32Fx4:
							case Iop_Max32Fx4:
							case Iop_Min32Fx4:
							case Iop_Add64Fx2:
							case Iop_Sub64Fx2:
							case Iop_Mul64Fx2:
							case Iop_Div64Fx2:
							case Iop_Max64Fx2:
							case Iop_Min64Fx2:
							case Iop_SqrtF64:
							case Iop_SqrtF64r32:
							case Iop_SqrtF32:
							case Iop_AtanF64:
							case Iop_Yl2xF64:
							case Iop_Yl2xp1F64:
							case Iop_PRemF64:
							case Iop_PRemC3210F64:
							case Iop_PRem1F64:
							case Iop_PRem1C3210F64:
							case Iop_ScaleF64:
							case Iop_PwMax32Fx2:
							case Iop_PwMin32Fx2:
							case Iop_SinF64:
							case Iop_CosF64:
							case Iop_TanF64:
							case Iop_2xm1F64:
							case Iop_RoundF64toF32:
								addStmtToIRSB(sbOut, st);
								reportUnsupportedOp(expr->Iex.Binop.op);
								break;
							default:
								addStmtToIRSB(sbOut, st);
								break;
						}
						break;
					case Iex_Triop:
						// opToStr(expr->Iex.Triop.op);
						// VG_(umsg)("tri : %s\n", opStr);
						switch (expr->Iex.Triop.op) {
							case Iop_AddF64:
							case Iop_SubF64:
							case Iop_MulF64:
							case Iop_DivF64:
								addStmtToIRSB(sbOut, st);
								
								arg1tmpInstead = -1;
								arg2tmpInstead = -1;
								if (expr->Iex.Triop.arg2->tag == Iex_RdTmp) {
									arg1tmpInstead = tmpInstead[expr->Iex.Triop.arg2->Iex.RdTmp.tmp];
								}
								if (expr->Iex.Triop.arg3->tag == Iex_RdTmp) {
									arg2tmpInstead = tmpInstead[expr->Iex.Triop.arg3->Iex.RdTmp.tmp];
								}
								instrumentTriOp(sbOut, tyenv, cia, st->Ist.WrTmp.tmp, expr, arg1tmpInstead, arg2tmpInstead);
								break;
							case Iop_AddF32:
							case Iop_SubF32:
							case Iop_MulF32:
							case Iop_DivF32:
							case Iop_AddF64r32:
							case Iop_SubF64r32:
							case Iop_MulF64r32:
							case Iop_DivF64r32:
							case Iop_AtanF64:
      						case Iop_Yl2xF64:
      						case Iop_Yl2xp1F64:
      						case Iop_PRemF64:
      						case Iop_PRemC3210F64:
      						case Iop_PRem1F64:
      						case Iop_PRem1C3210F64:
      						case Iop_ScaleF64:
								addStmtToIRSB(sbOut, st);
								reportUnsupportedOp(expr->Iex.Triop.op);
								break;
							default:
								addStmtToIRSB(sbOut, st);
								break;
						}
						break;
					case Iex_Qop:
						switch (expr->Iex.Qop.op) {
							case Iop_MAddF64r32:
							case Iop_MSubF64r32:
							case Iop_MAddF64:
							case Iop_MSubF64:
								addStmtToIRSB(sbOut, st);
								reportUnsupportedOp(expr->Iex.Qop.op);
								break;
							default:
								addStmtToIRSB(sbOut, st);
								break;
						}
						break;
					case Iex_Mux0X:
						addStmtToIRSB(sbOut, st);

						arg1tmpInstead = -1;
						arg2tmpInstead = -1;
						if (expr->Iex.Mux0X.expr0->tag == Iex_RdTmp) {
							arg1tmpInstead = tmpInstead[expr->Iex.Mux0X.expr0->Iex.RdTmp.tmp];
						}
						if (expr->Iex.Mux0X.exprX->tag == Iex_RdTmp) {
							arg2tmpInstead = tmpInstead[expr->Iex.Mux0X.exprX->Iex.RdTmp.tmp];
						}
						instrumentMux0X(sbOut, tyenv, st->Ist.WrTmp.tmp, expr, arg1tmpInstead, arg2tmpInstead);
						break;
					case Iex_CCall:
						addStmtToIRSB(sbOut, st);
						break;
					default:
						addStmtToIRSB(sbOut, st);
						break;
				}
				break;
			case Ist_Store:
				addStmtToIRSB(sbOut, st);

				arg1tmpInstead = -1;
				if (st->Ist.Store.data->tag == Iex_RdTmp) {
					arg1tmpInstead = tmpInstead[st->Ist.Store.data->Iex.RdTmp.tmp];
				}
				instrumentStore(sbOut, tyenv, st, arg1tmpInstead);
				storeCount++;
				break;
			default:
				addStmtToIRSB(sbOut, st);
				break;
		}
	}
    return sbOut;
}

static void getIntroducedError(mpfr_t* introducedError, MeanValue* mv) {
	mpfr_set_ui(*introducedError, 0, STD_RND);
	mpfr_abs(introMaxError, mv->max, STD_RND);
	if (mv->arg1 != 0 && mv->arg2 != 0) {
		MeanValue* mv1 = VG_(HT_lookup)(meanValues, mv->arg1);
		MeanValue* mv2 = VG_(HT_lookup)(meanValues, mv->arg2);
		tl_assert(mv1 && mv2);
		mpfr_abs(introErr1, mv1->max, STD_RND);
		mpfr_abs(introErr2, mv2->max, STD_RND);

		if (mv->arg1 == mv->key && mv->arg2 == mv->key) {
			mpfr_set(*introducedError, introMaxError, STD_RND);
		} else if (mpfr_cmp(introErr1, introErr2) > 0) {
			if (mpfr_cmp(introMaxError, introErr1) > 0 || mpfr_cmp(introMaxError, introErr2) > 0) {
				if (mv->arg1 == mv->key) {
					mpfr_set(*introducedError, introMaxError, STD_RND);
				} else {
					mpfr_sub(*introducedError, introMaxError, introErr1, STD_RND);
				}
			} else {
				/* introduced error gets negative */
				mpfr_sub(*introducedError, introMaxError, introErr2, STD_RND);
			}
		} else if (mpfr_cmp(introMaxError, introErr2) > 0 || mpfr_cmp(introMaxError, introErr1) > 0) {
			if (mv->arg2 == mv->key) {
				mpfr_set(*introducedError, introMaxError, STD_RND);
			} else {
				mpfr_sub(*introducedError, introMaxError, introErr2, STD_RND);
			}
		} else {
			/* introduced error gets negative */
			mpfr_sub(*introducedError, introMaxError, introErr1, STD_RND);
		}
	} else if (mv->arg1 != 0) {
		MeanValue* mv1 = VG_(HT_lookup)(meanValues, mv->arg1);
		tl_assert(mv1);
		mpfr_abs(introErr1, mv1->max, STD_RND);

		if (mv->arg1 == mv->key) {
			mpfr_set(*introducedError, introMaxError, STD_RND);
		} else {
			/* introduced error can get negative */
			mpfr_sub(*introducedError, introMaxError, introErr1, STD_RND);
		}
	} else if (mv->arg2 != 0) {
		MeanValue* mv2 = VG_(HT_lookup)(meanValues, mv->arg2);
		tl_assert(mv2);
		mpfr_abs(introErr2, mv2->max, STD_RND);

		if (mv->arg2 == mv->key) {
			mpfr_set(*introducedError, introMaxError, STD_RND);
		} else {
			/* introduced error can get negative */
			mpfr_sub(*introducedError, introMaxError, introErr2, STD_RND);
		}
	} else {
		mpfr_set(*introducedError, introMaxError, STD_RND);
	}
}

static void writeOriginGraph(Int file, Addr oldAddr, Addr origin, Int arg, Int level, Int edgeColor, Bool careVisited) {
	if (level > MAX_LEVEL_OF_GRAPH) {
		if (careVisited) {
			MeanValue* mv = VG_(HT_lookup)(meanValues, oldAddr);
			mv->visited = True;
		}
		return;
	}

	if (level <= 1) {
		my_fwrite(file, "graph: {\n", 9);
		my_fwrite(file, "title: \"Created with FpDebug\"\n", 30);
		my_fwrite(file, "classname 1 : \"FpDebug\"\n", 24);

		Int i;
		for (i = 50; i < 150; i++) {
			VG_(sprintf)(formatBuf, "colorentry %d : 255 %d 0\n", i, (Int)((255.0 / 100.0) * (i - 50)));
			my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
		}
		for (i = 150; i < 250; i++) {
			VG_(sprintf)(formatBuf, "colorentry %d : %d 255 0\n", i, (Int)((255.0 / 100.0) * (i - 150)));
			my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
		}

		/* the set is used to avoid cycles */
		if (originAddrSet) {
			VG_(OSetWord_Destroy)(originAddrSet);
		}
		originAddrSet = VG_(OSetWord_Create)(VG_(malloc), "fd.writeOriginGraph.1", VG_(free));
	} 
	tl_assert(originAddrSet);

	MeanValue* mv = VG_(HT_lookup)(meanValues, origin);
	tl_assert(mv);

	if (careVisited) {
		mv->visited = True;
	}
	
	Bool cycle = False;
	Bool inLibrary = False;
	if (VG_(OSetWord_Contains)(originAddrSet, origin)) {
		cycle = True;
	} else {
		/* create node */
		VG_(describe_IP)(origin, description, DESCRIPTION_SIZE);
		if (ignoreFile(description)) {
			inLibrary = True;
		}

		if (clo_ignoreAccurate && mpfr_cmp_ui(mv->max, 0) == 0) {
			return;
		}

		Int color = 150; /* green */

		if (level > 1) {
			MeanValue* oldMv = VG_(HT_lookup)(meanValues, oldAddr);
			tl_assert(oldMv);

			getIntroducedError(&dumpGraphDiff, mv);

			if (mpfr_cmp_ui(dumpGraphDiff, 0) > 0) {
				mpfr_exp_t exp = mpfr_get_exp(dumpGraphDiff);
				if (exp > 1) exp = 1;
				if (exp < -8) exp = -8;
				exp = 9 + (exp - 1); /* range 0..9 */
				color = 149 - (exp * 10);
			}
		} else {
			color = 1; /* blue */
		}
		
		mpfr_div_ui(dumpGraphMeanError, mv->sum, mv->count, STD_RND);

		opToStr(mv->op);
		Char meanErrorStr[MPFR_BUFSIZE];
		mpfrToStringShort(meanErrorStr, &dumpGraphMeanError);
		Char maxErrorStr[MPFR_BUFSIZE];
		mpfrToStringShort(maxErrorStr, &(mv->max));

		Char canceledAvg[10];
		if (mv->overflow) {
			VG_(sprintf)(canceledAvg, "overflow");
		} else {
			VG_(sprintf)(canceledAvg, "%ld", mv->canceledSum / mv->count);
		}

		Char filename[20];
		filename[0] = '\0';
		VG_(get_filename)(origin, filename, 19);

		UInt linenum = -1;
		Bool gotLine = VG_(get_linenum)(origin, &linenum);
		Char linenumber[10];
		linenumber[0] = '\0';
		if (gotLine) {
			VG_(sprintf)(linenumber, ":%u", linenum);
		}

		VG_(sprintf)(formatBuf, "node: { title: \"0x%lX\" label: \"%s (%s%s)\" color: %d info1: \"%s (%'u)\" info2: \"avg: %s, max: %s\" "
			"info3: \"canceled - avg: %s, max: %ld\" }\n", 
			origin, opStr, filename, linenumber, color, description, mv->count, meanErrorStr, maxErrorStr, canceledAvg, mv->canceledMax);
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	}

	if (level > 1) {
		/* create edge */
		MeanValue* oldMv = VG_(HT_lookup)(meanValues, oldAddr);
		tl_assert(oldMv);

		getIntroducedError(&dumpGraphDiff, mv);

		Char diffStr[30];
		mpfrToStringShort(&diffStr, &dumpGraphDiff);

		VG_(sprintf)(formatBuf, "edge: { sourcename: \"0x%lX\" targetname: \"0x%lX\" label: \"%s\" class: 1 color : %d }\n", origin, oldAddr, diffStr, edgeColor);
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	}
	
	if (cycle) {
		return;
	}

	if (mv != NULL) {
		VG_(OSetWord_Insert)(originAddrSet, origin);
		if (mv->arg1 != 0 && mv->arg2 != 0) {
			MeanValue* mv1 = VG_(HT_lookup)(meanValues, mv->arg1);
			MeanValue* mv2 = VG_(HT_lookup)(meanValues, mv->arg2);
			tl_assert(mv1 && mv2);

			Bool leftErrGreater = True;
			mpfr_abs(dumpGraphErr1, mv1->max, STD_RND);
			mpfr_abs(dumpGraphErr2, mv2->max, STD_RND);

			if (mpfr_cmp(dumpGraphErr1, dumpGraphErr2) < 0) {
				leftErrGreater = False;
			}
			mpfr_sub(dumpGraphDiff, dumpGraphErr1, dumpGraphErr2, STD_RND);
			mpfr_abs(dumpGraphDiff, dumpGraphDiff, STD_RND);
			mpfr_exp_t exp = mpfr_get_exp(dumpGraphDiff);
			if (exp > 1) exp = 1;
			if (exp < -8) exp = -8;
			exp = 9 + (exp - 1);
			Int red = 149 - (exp * 10);
			if (red > 120) red = 120;
			Int green = red + 100;

			VG_(describe_IP)(mv->arg1, description, DESCRIPTION_SIZE);
			if (!inLibrary || !ignoreFile(description)) {
				writeOriginGraph(file, origin, mv->arg1, 1, ++level, (leftErrGreater ? red : green), careVisited);
			}
			VG_(describe_IP)(mv->arg2, description, DESCRIPTION_SIZE);
			if (!inLibrary || !ignoreFile(description)) {
				writeOriginGraph(file, origin, mv->arg2, 2, level, (leftErrGreater ? green : red), careVisited);
			}
		} else if (mv->arg1 != 0) {
			VG_(describe_IP)(mv->arg1, description, DESCRIPTION_SIZE);
			if (!inLibrary || !ignoreFile(description)) {
				writeOriginGraph(file, origin, mv->arg1, 1, ++level, 1, careVisited);
			}
		} else if (mv->arg2 != 0) {
			VG_(describe_IP)(mv->arg2, description, DESCRIPTION_SIZE);
			if (!inLibrary || !ignoreFile(description)) {
				writeOriginGraph(file, origin, mv->arg2, 2, ++level, 1, careVisited);
			}
		}
	}
}

static Bool dumpGraph(Char* fileName, ULong addr, Bool conditional, Bool careVisited) {
	/*if (!clo_computeMeanValue) {
		VG_(umsg)("DUMP GRAPH (%s): Mean error computation has to be active!\n", fileName);
		return False;
	}

	ShadowValue* svalue = VG_(HT_lookup)(globalMemory, addr);
	if (svalue) {
		if (careVisited) {
			MeanValue* mv = VG_(HT_lookup)(meanValues, svalue->origin);
			tl_assert(mv);
			if (mv->visited) {
				return False;
			}
		}

		VG_(describe_IP)(svalue->origin, description, DESCRIPTION_SIZE);
		if (ignoreFile(description)) {
			return False;
		}

		if (svalue->orgType == Ot_FLOAT) {
			mpfr_set_flt(dumpGraphOrg, svalue->Org.fl, STD_RND);
		} else if (svalue->orgType == Ot_DOUBLE) {
			mpfr_set_d(dumpGraphOrg, svalue->Org.db, STD_RND);
		} else {
			tl_assert(False);
		}

		if (mpfr_cmp_ui(svalue->value, 0) != 0 || mpfr_cmp_ui(dumpGraphOrg, 0) != 0) {
			mpfr_reldiff(dumpGraphRel, svalue->value, dumpGraphOrg, STD_RND);
			mpfr_abs(dumpGraphRel, dumpGraphRel, STD_RND);
		} else {
			mpfr_set_ui(dumpGraphRel, 0, STD_RND);
		}

		if (conditional && mpfr_cmp_ui(dumpGraphRel, 0) == 0) {
			return False;
		}

		SysRes file = VG_(open)(fileName, VKI_O_CREAT|VKI_O_TRUNC|VKI_O_WRONLY, VKI_S_IRUSR|VKI_S_IWUSR);
		if (!sr_isError(file)) {
			writeOriginGraph(sr_Res(file), 0, svalue->origin, 0, 1, 1, careVisited);
			my_fwrite(sr_Res(file), "}\n", 2);
			fwrite_flush();
			VG_(close)(sr_Res(file));
			VG_(umsg)("DUMP GRAPH (%s): successful\n", fileName);
			return True;
		} else {
			VG_(umsg)("DUMP GRAPH (%s): Failed to create or open the file!\n", fileName);
			return False;
		}
	} else {
		VG_(umsg)("DUMP GRAPH (%s): Shadow variable was not found!\n", fileName);
		VG_(get_and_pp_StackTrace)(VG_(get_running_tid)(), 16);
		return False;
	}*/
	return False;
}

static void printError(Char* varName, ULong addr, Bool conditional) {
	mpfr_t org, diff, rel;
	ShadowValue* svalue = VG_(HT_lookup)(globalMemory, addr);;
	if (svalue) {
		mpfr_inits(diff, rel, NULL);
		
		Bool isFloat = svalue->orgType == Ot_FLOAT;
		if (svalue->orgType == Ot_FLOAT) {
			mpfr_init(org);
			mpfr_set_flt(org, svalue->Org.fl, STD_RND);
		} else if (svalue->orgType == Ot_DOUBLE) {
			mpfr_init_set_d(org, svalue->Org.db, STD_RND);
		} else {
			tl_assert(False);
		}

		if (mpfr_cmp_ui(svalue->value, 0) != 0 || mpfr_cmp_ui(org, 0) != 0) {
			mpfr_reldiff(rel, svalue->value, org, STD_RND);
			mpfr_abs(rel, rel, STD_RND);
		} else {
			mpfr_set_ui(rel, 0, STD_RND);
		}

		if (conditional && mpfr_cmp_ui(rel, 0) == 0) {
			mpfr_clears(org, diff, rel, NULL);
			return;
		}

		mpfr_sub(diff, svalue->value, org, STD_RND);

		Char typeName[7];
		if (isFloat) {
			VG_(strcpy)(typeName, "float");
		} else {
			VG_(strcpy)(typeName, "double");
		}

		VG_(umsg)("(%s) %s PRINT ERROR OF: 0x%lX\n", typeName, varName, addr);
		Char mpfrBuf[MPFR_BUFSIZE];
		mpfrToString(mpfrBuf, &org);
		VG_(umsg)("(%s) %s ORIGINAL:         %s\n", typeName, varName, mpfrBuf);
		mpfrToString(mpfrBuf, &(svalue->value));
		VG_(umsg)("(%s) %s SHADOW VALUE:     %s\n", typeName, varName, mpfrBuf);
		mpfrToString(mpfrBuf, &(svalue->midValue));
		VG_(umsg)("(%s) %s MIDDLE:           %s\n", typeName, varName, mpfrBuf);
		mpfrToString(mpfrBuf, &(svalue->oriValue));
		VG_(umsg)("(%s) %s SIMULATE:         %s\n", typeName, varName, mpfrBuf);
		mpfrToString(mpfrBuf, &diff);
		VG_(umsg)("(%s) %s ABSOLUTE ERROR:   %s\n", typeName, varName, mpfrBuf);
		mpfrToString(mpfrBuf, &rel);
		VG_(umsg)("(%s) %s RELATIVE ERROR:   %s\n", typeName, varName, mpfrBuf);
		VG_(umsg)("(%s) %s CANCELED BITS:     %lld\n", typeName, varName, svalue->canceled);

		VG_(describe_IP)(svalue->origin, description, DESCRIPTION_SIZE);
		VG_(umsg)("(%s) %s Last operation: %s\n", typeName, varName, description);

		if (svalue->canceled > 0 && svalue->cancelOrigin > 0) {
			VG_(describe_IP)(svalue->cancelOrigin, description, DESCRIPTION_SIZE);
			VG_(umsg)("(%s) %s Cancellation origin: %s\n", typeName, varName, description);
		}
		
		VG_(umsg)("(%s) %s Operation count (max path): %'lu\n", typeName, varName, svalue->opCount);

		mpfr_clears(org, diff, rel, NULL);
	} else {
		VG_(umsg)("There exists no shadow value for %s!\n", varName);
		VG_(get_and_pp_StackTrace)(VG_(get_running_tid)(), 16);
	}
}

static Bool isErrorGreater(ULong addrFp, ULong addrErr) {
	mpfr_t org, rel;
	Double* errorBound = (Double*)addrErr;

	ShadowValue* svalue = VG_(HT_lookup)(globalMemory, addrFp);
	if (svalue) {
		mpfr_init(rel);
		
		Bool isFloat = svalue->orgType == Ot_FLOAT;
		if (svalue->orgType == Ot_FLOAT) {
			mpfr_init(org);
			mpfr_set_flt(org, svalue->Org.fl, STD_RND);
		} else if (svalue->orgType == Ot_DOUBLE) {
			mpfr_init_set_d(org, svalue->Org.db, STD_RND);
		} else {
			tl_assert(False);
		}

		if (mpfr_cmp_ui(svalue->value, 0) != 0 || mpfr_cmp_ui(org, 0) != 0) {
			mpfr_reldiff(rel, svalue->value, org, STD_RND);
			mpfr_abs(rel, rel, STD_RND);
		} else {
			mpfr_set_ui(rel, 0, STD_RND);
		}
		
		Bool isGreater = mpfr_cmp_d(rel, *errorBound) >= 0; 

		mpfr_clears(org, rel, NULL);
		return isGreater;
	} else {
		VG_(umsg)("Error greater: there exists no shadow value!\n");
		VG_(get_and_pp_StackTrace)(VG_(get_running_tid)(), 16);
		return False;
	}
}

static void resetShadowValues(void) {
	Int i, j;
	for (i = 0; i < VG_N_THREADS; i++) {
		for (j = 0; j < MAX_REGISTERS; j++) {
			if (threadRegisters[i][j] != NULL) {
				threadRegisters[i][j]->active = False;
			}
		}
	}
	for (i = 0; i < MAX_TEMPS; i++) {
		if (localTemps[i] != NULL) {
			localTemps[i]->version = 0;
		}
	}
	ShadowValue* next;
	VG_(HT_ResetIter)(globalMemory);
	while (next = VG_(HT_Next)(globalMemory)) {
		next->active = False;
	}
}

static void insertShadow(ULong addrFp) {
	ShadowValue* svalue = VG_(HT_lookup)(globalMemory, addrFp);
	if (svalue) {
		if (svalue->orgType == Ot_FLOAT) {
			mpfr_set_prec(svalue->midValue, 24);
			mpfr_set(svalue->midValue, svalue->value, STD_RND);

			// Float* orgFl = (Float*)addrFp;
			// *orgFl = mpfr_get_flt(svalue->value, STD_RND);
   //                      unsigned int ul = *(unsigned int*)addrFp;
   //                      VG_(umsg)("----address-----%X\n", ul);
   //                      double temp = mpfr_get_d(svalue->value, STD_RND);
   //                      unsigned int ul2 = *(unsigned int*)(&temp);
   //                      VG_(umsg)("---mpfr----%X\n", ul2);
			// unsigned int ul3 = *(unsigned int*)(orgFl);
			// VG_(umsg)("----orgfl---%X\n", ul3);
		} else if (svalue->orgType == Ot_DOUBLE) {
			mpfr_set_prec(svalue->midValue, 53);
			mpfr_set(svalue->midValue, svalue->value, STD_RND);
			// mpfr_set(svalue->midValue, svalue->value, STD_RND);
			// Double* orgDb = (Double*)addrFp;
			// *orgDb = mpfr_get_d(svalue->value, STD_RND);
			// unsigned long long ull = *(unsigned long long*)addrFp;
			// VG_(umsg)("----***-----%lX\n", ull);
			// double temp = mpfr_get_d(svalue->value, STD_RND);
			// unsigned long long ull2 = *(unsigned long long*)(&temp);
			// VG_(umsg)("---second----%lX\n", ull2);
			//mpfr_out_str(stdout, 10, 50, svalue->value, STD_RND);
		} else {
			tl_assert(False);
		}
	}
}

/*********************/
static void setShadow(ULong addrFp) {
	ShadowValue* svalue = VG_(HT_lookup)(globalMemory, addrFp);
	if (svalue) {
		mpfr_set(svalue->value, svalue->midValue, STD_RND);
	}
}

static void shadowToOriginal(ULong addrFp) {
	ShadowValue* svalue = VG_(HT_lookup)(globalMemory, addrFp);
	if (svalue) {
		if (svalue->orgType == Ot_FLOAT) {
			Float* orgFl = (Float*)addrFp;
			*orgFl = mpfr_get_flt(svalue->value, STD_RND);
		} else if (svalue->orgType == Ot_DOUBLE) {
			Double* orgDb = (Double*)addrFp;
			*orgDb = mpfr_get_d(svalue->value, STD_RND);
		} else {
			tl_assert(False);
		}
	}
}

static void originalToShadow(ULong addrFp) {
	ShadowValue* svalue = VG_(HT_lookup)(globalMemory, addrFp);
	if (svalue) {
		if (svalue->orgType == Ot_FLOAT) {
			Float* orgFl = (Float*)addrFp;
			mpfr_set_flt(svalue->value, *orgFl, STD_RND);
			mpfr_set_prec(svalue->midValue, 24);
			mpfr_set_flt(svalue->midValue, *orgFl, STD_RND);
		} else if (svalue->orgType == Ot_DOUBLE) {
			Double* orgDb = (Double*)addrFp;
			mpfr_set_d(svalue->value, *orgDb, STD_RND);
			mpfr_set_prec(svalue->midValue, 53);
			mpfr_set_d(svalue->midValue, *orgDb, STD_RND);
		} else {
			tl_assert(False);
		}
	}
}

static void setOriginal(ULong addrFp, ULong addrVal) {
	ShadowValue* svalue = VG_(HT_lookup)(globalMemory, addrFp);
	if (svalue) {
		if (svalue->orgType == Ot_FLOAT) {
			Float* orgFl = (Float*)addrFp;
			Float* value = (Float*)addrVal;
			*orgFl = *value;
			mpfr_set_prec(svalue->midValue, 24);
			mpfr_set_flt(svalue->midValue, *orgFl, STD_RND);
		} else if (svalue->orgType == Ot_DOUBLE) {
			Double* orgDb = (Double*)addrFp;
			Double* value = (Double*)addrVal;
			*orgDb = *value;
			mpfr_set_prec(svalue->midValue, 53);
			mpfr_set_d(svalue->midValue, *orgDb, STD_RND);
		} else {
			tl_assert(False);
		}
	}
}

static void setShadowBy(ULong addrDst, ULong addrSrc) {
	ShadowValue* dvalue = VG_(HT_lookup)(globalMemory, addrDst);
	ShadowValue* svalue = VG_(HT_lookup)(globalMemory, addrSrc);
	if (dvalue && svalue) {
		mpfr_set(dvalue->value, svalue->value, STD_RND);
		mpfr_set(dvalue->midValue, svalue->midValue, STD_RND);
	}
}

static void getRelativeError(ULong addr, Char* rel_str) {
	mpfr_t org, diff, rel;

	ShadowValue* svalue = VG_(HT_lookup)(globalMemory, addr);
	
	if (svalue) {
		mpfr_inits(diff, rel, NULL);
		
		Bool isFloat = svalue->orgType == Ot_FLOAT;
		if (svalue->orgType == Ot_FLOAT) {
			mpfr_init(org);
			mpfr_set_flt(org, svalue->Org.fl, STD_RND);
		} else if (svalue->orgType == Ot_DOUBLE) {
			mpfr_init_set_d(org, svalue->Org.db, STD_RND);
		} else {
			tl_assert(False);
		}

		if (mpfr_cmp_ui(svalue->value, 0) != 0 || mpfr_cmp_ui(org, 0) != 0) {
			mpfr_reldiff(rel, svalue->value, org, STD_RND);
			mpfr_abs(rel, rel, STD_RND);
		} else {
			mpfr_set_ui(rel, 0, STD_RND);
		}

		mpfrToStringE(rel_str, &rel);
		//VG_(umsg)("RELATIVE ERROR: %s\n", rel_str);

		mpfr_clears(org, diff, rel, NULL);
	} else {
		VG_(strcpy)(rel_str, "0.0e+0");
		//VG_(get_and_pp_StackTrace)(VG_(get_running_tid)(), 16);
	}
}

static void getShadow(ULong addr, Char* sd) {
	ShadowValue* svalue = VG_(HT_lookup)(globalMemory, addr);
	if (svalue) {
		Char mpfrBuf[MPFR_BUFSIZE];
		mpfrToStringE(mpfrBuf, &(svalue->value));
		VG_(strcpy)(sd, mpfrBuf);
	} else {
		VG_(strcpy)(sd, "noshadow");
	}
}

static void printOriginalAndShadow(Char* varName, Int type, ULong addr) {
	mpfr_t org;
	mpfr_init(org);
	Char mpfrBuf[MPFR_BUFSIZE];
	if (type == 0) {
		Float* fl = (Float*)addr;
		mpfr_set_flt(org, *fl, STD_RND);
		mpfrToStringE(mpfrBuf, &org);
		VG_(umsg)("(float) %s ORIGINAL VALUE:		%s\n", varName, mpfrBuf);
		getShadow(addr, mpfrBuf);
		VG_(umsg)("(float) %s SHADOW VALUE:		%s\n", varName, mpfrBuf);
	} else if (type == 1) {
		Double* db = (Double*)addr;
		mpfr_set_d(org, *db, STD_RND);
		mpfrToStringE(mpfrBuf, &org);
		VG_(umsg)("(double) %s ORIGINAL VALUE:		%s\n", varName, mpfrBuf);
		getShadow(addr, mpfrBuf);
		VG_(umsg)("(double) %s SHADOW VALUE:		%s\n", varName, mpfrBuf);
	} else {
		VG_(tool_panic)("Unhandled value type\n");
	}
	mpfr_clear(org);
}

/*********************/



static void beginAnalyzing(void) {
	clo_analyze = True;
}

static void endAnalyzing(void) {
    if (!clo_ignore_end) {
		clo_analyze = False;
    }
}

static void writeWarning(Int file) {
	if (VG_(OSetWord_Size)(unsupportedOps) == 0) {
		return;
	}
	VG_(sprintf)(formatBuf, "Unsupported operations detected: ");
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));

	VG_(OSetWord_ResetIter)(unsupportedOps);
	UWord next = 0;
	Bool first = True;
	while (VG_(OSetWord_Next)(unsupportedOps, &next)) {
		IROp op = (IROp)next;
		opToStr(op);
		if (first) {
			VG_(sprintf)(formatBuf, "%s", opStr);
		} else {
			VG_(sprintf)(formatBuf, ", %s", opStr);
		}
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
		first = False;
	}
	VG_(sprintf)(formatBuf, "\n\n");
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
}

static void writeShadowValue(Int file, ShadowValue* svalue, Int num) {
	tl_assert(svalue);

	Bool isFloat = svalue->orgType == Ot_FLOAT;
	if (svalue->orgType == Ot_FLOAT) {
		mpfr_set_flt(writeSvOrg, svalue->Org.fl, STD_RND);
	} else if (svalue->orgType == Ot_DOUBLE) {
		mpfr_set_d(writeSvOrg, svalue->Org.db, STD_RND);
	} else {
		tl_assert(False);
	}

	if (mpfr_cmp_ui(svalue->value, 0) != 0 || mpfr_cmp_ui(writeSvOrg, 0) != 0) {
		mpfr_reldiff(writeSvRelError, svalue->value, writeSvOrg, STD_RND);
		mpfr_abs(writeSvRelError, writeSvRelError, STD_RND);
	} else {
		mpfr_set_ui(writeSvRelError, 0, STD_RND);
	}

	mpfr_sub(writeSvDiff, svalue->value, writeSvOrg, STD_RND);

	Char mpfrBuf[MPFR_BUFSIZE];
	Char typeName[7];
	if (isFloat) {
		VG_(strcpy)(typeName, "float");
	} else {
		VG_(strcpy)(typeName, "double");
	}

	VG_(sprintf)(formatBuf, "%d: 0x%lX of type %s\n", num, svalue->key, typeName);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	mpfrToString(mpfrBuf, &writeSvOrg);
	VG_(sprintf)(formatBuf, "    original:         %s\n", mpfrBuf);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	mpfrToString(mpfrBuf, &(svalue->value));
	VG_(sprintf)(formatBuf, "    shadow value:     %s\n", mpfrBuf);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	mpfrToString(mpfrBuf, &writeSvDiff);
	VG_(sprintf)(formatBuf, "    absolute error:   %s\n", mpfrBuf);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	mpfrToString(mpfrBuf, &writeSvRelError);
	VG_(sprintf)(formatBuf, "    relative error:   %s\n", mpfrBuf);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	VG_(sprintf)(formatBuf, "    maximum number of canceled bits: %ld\n", svalue->canceled);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));

	if (svalue->canceled > 0 && svalue->cancelOrigin > 0) {
		VG_(describe_IP)(svalue->cancelOrigin, description, DESCRIPTION_SIZE);
		VG_(sprintf)(formatBuf, "    origin of maximum cancellation: %s\n", description);
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	}

	VG_(describe_IP)(svalue->origin, description, DESCRIPTION_SIZE);
	VG_(sprintf)(formatBuf, "    last operation: %s\n", description);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	VG_(sprintf)(formatBuf, "    operation count (max path): %'lu\n", svalue->opCount);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
}

static Bool areSvsEqual(ShadowValue* sv1, ShadowValue* sv2) {
	if (sv1->opCount == sv2->opCount && sv1->origin == sv2->origin && 
		sv1->canceled == sv2->canceled && sv1->cancelOrigin == sv2->cancelOrigin && 
		sv1->orgType == sv2->orgType && mpfr_cmp(sv1->value, sv2->value) == 0)
	{
		return (sv1->orgType == Ot_FLOAT && sv1->Org.fl == sv2->Org.fl) || 
			   (sv1->orgType == Ot_DOUBLE && sv1->Org.db == sv2->Org.db); 
	}
	return False;
}

static Int compareShadowValues(void* n1, void* n2) {
	ShadowValue* sv1 = *(ShadowValue**)n1;
	ShadowValue* sv2 = *(ShadowValue**)n2;
	if (sv1->opCount < sv2->opCount) return 1;
	if (sv1->opCount > sv2->opCount) return -1;
	if (sv1->key < sv2->key) return -1;
	if (sv1->key > sv2->key) return 1;
	return 0;
}

static void writeMemorySpecial(ShadowValue** memory, UInt n_memory) {
	Char fname[256];
	HChar* clientName = VG_(args_the_exename);
	VG_(sprintf)(fname, "%s_shadow_values_special", clientName);

	getFileName(fname);
	SysRes fileRes = VG_(open)(fname, VKI_O_CREAT|VKI_O_TRUNC|VKI_O_WRONLY, VKI_S_IRUSR|VKI_S_IWUSR);
	if (sr_isError(fileRes)) {
		VG_(umsg)("SHADOW VALUES (%s): Failed to create or open the file!\n", fname);
		return;
	}
	Int file = sr_Res(fileRes);
	writeWarning(file);

	UInt specialFps = 0;
	UInt skippedLibrary = 0;
	UInt numWritten = 0;
	UInt total = 0;
	Int i;
	for (i = 0; i < n_memory; i++) {
		if (i > 0 && areSvsEqual(memory[i-1], memory[i])) {
			continue;
		}
		total++;

		if (memory[i]->orgType == Ot_FLOAT) {
			mpfr_set_flt(endAnalysisOrg, memory[i]->Org.fl, STD_RND);
		} else if (memory[i]->orgType == Ot_DOUBLE) {
			mpfr_set_d(endAnalysisOrg, memory[i]->Org.db, STD_RND);
		} else {
			tl_assert(False);
		}

		/* not a normal number => NaN, +Inf, or -Inf */
		if (mpfr_number_p(endAnalysisOrg) == 0) {
			specialFps++;

			if (clo_ignoreLibraries) {
				VG_(describe_IP)(memory[i]->origin, description, DESCRIPTION_SIZE);
				if (ignoreFile(description)) {
					skippedLibrary++;
					continue;
				}
			}

			if (numWritten < MAX_ENTRIES_PER_FILE) {
				numWritten++;
				writeShadowValue(file, memory[i], total);
				VG_(sprintf)(formatBuf, "\n");
				my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
			}
		} else if (!clo_ignoreAccurate && numWritten < MAX_ENTRIES_PER_FILE) {
			numWritten++;
			writeShadowValue(file, memory[i], i);
			VG_(sprintf)(formatBuf, "\n");
			my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
		}
	}

	VG_(sprintf)(formatBuf, "%'u%s out of %'u shadow values are in this file\n", numWritten, 
		numWritten == MAX_ENTRIES_PER_FILE ? " (maximum number written to file)" : "", total);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	if (skippedLibrary > 0) {
		VG_(sprintf)(formatBuf, "%'u are skipped because they are from a library\n", skippedLibrary);
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	}
	VG_(sprintf)(formatBuf, "%'u out of %'u shadow values are special (NaN, +Inf, or -Inf)\n", specialFps, n_memory);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	VG_(sprintf)(formatBuf, "total number of floating-point operations: %'lu\n", fpOps);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	VG_(sprintf)(formatBuf, "number of executed blocks: %'lu\n", sbExecuted);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));

	fwrite_flush();
	VG_(close)(file);
	VG_(umsg)("SHADOW VALUES (%s): successful\n", fname);
}

static void writeMemoryCanceled(ShadowValue** memory, UInt n_memory) {
	Char fname[256];
	HChar* clientName = VG_(args_the_exename);
	VG_(sprintf)(fname, "%s_shadow_values_canceled", clientName);

	getFileName(fname);
	SysRes fileRes = VG_(open)(fname, VKI_O_CREAT|VKI_O_TRUNC|VKI_O_WRONLY, VKI_S_IRUSR|VKI_S_IWUSR);
	if (sr_isError(fileRes)) {
		VG_(umsg)("SHADOW VALUES (%s): Failed to create or open the file!\n", fname);
		return;
	}
	Int file = sr_Res(fileRes);
	writeWarning(file);

	UInt fpsWithError = 0;
	UInt skippedLibrary = 0;
	UInt numWritten = 0;
	UInt total = 0;
	Int i;
	for (i = 0; i < n_memory; i++) {
		if (i > 0 && areSvsEqual(memory[i-1], memory[i])) {
			continue;
		}
		total++;

		if (memory[i]->canceled > CANCEL_LIMIT) {
			fpsWithError++;

			if (clo_ignoreLibraries) {
				VG_(describe_IP)(memory[i]->origin, description, DESCRIPTION_SIZE);
				if (ignoreFile(description)) {
					skippedLibrary++;
					continue;
				}
			}

			if (numWritten < MAX_ENTRIES_PER_FILE) {
				numWritten++;
				writeShadowValue(file, memory[i], i);
				VG_(sprintf)(formatBuf, "\n");
				my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
			}
		} else if (!clo_ignoreAccurate && numWritten < MAX_ENTRIES_PER_FILE) {
			numWritten++;
			writeShadowValue(file, memory[i], total);
			VG_(sprintf)(formatBuf, "\n");
			my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
		}
	}

	VG_(sprintf)(formatBuf, "%'u%s out of %'u shadow values are in this file\n", numWritten, 
		numWritten == MAX_ENTRIES_PER_FILE ? " (maximum number written to file)" : "", total);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	if (skippedLibrary > 0) {
		VG_(sprintf)(formatBuf, "%'u are skipped because they are from a library\n", skippedLibrary);
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	}
	VG_(sprintf)(formatBuf, "%'u out of %'u shadow values have more than %'d canceled bits\n", fpsWithError, total, CANCEL_LIMIT);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	VG_(sprintf)(formatBuf, "total number of floating-point operations: %'lu\n", fpOps);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	VG_(sprintf)(formatBuf, "number of executed blocks: %'lu\n", sbExecuted);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));

	fwrite_flush();
	VG_(close)(file);
	VG_(umsg)("SHADOW VALUES (%s): successful\n", fname);
}

static void writeMemoryRelError(ShadowValue** memory, UInt n_memory) {
	Char fname[256];
	HChar* clientName = VG_(args_the_exename);
	VG_(sprintf)(fname, "%s_shadow_values_relative_error", clientName);

	getFileName(fname);
	SysRes fileRes = VG_(open)(fname, VKI_O_CREAT|VKI_O_TRUNC|VKI_O_WRONLY, VKI_S_IRUSR|VKI_S_IWUSR);
	if (sr_isError(fileRes)) {
		VG_(umsg)("SHADOW VALUES (%s): Failed to create or open the file!\n", fname);
		return;
	}
	Int file = sr_Res(fileRes);
	writeWarning(file);

	UInt fpsWithError = 0;
	UInt skippedLibrary = 0;
	UInt numWritten = 0;
	UInt total = 0;
	Int j = 1;
	Int i;
	for (i = 0; i < n_memory; i++) {
		if (i > 0 && areSvsEqual(memory[i-1], memory[i])) {
			continue;
		}
		total++;

		if (memory[i]->orgType == Ot_FLOAT) {
			mpfr_set_flt(endAnalysisOrg, memory[i]->Org.fl, STD_RND);
		} else if (memory[i]->orgType == Ot_DOUBLE) {
			mpfr_set_d(endAnalysisOrg, memory[i]->Org.db, STD_RND);
		} else {
			tl_assert(False);
		}

		Bool hasError = True;
		if (mpfr_cmp_ui(memory[i]->value, 0) != 0 || mpfr_cmp_ui(endAnalysisOrg, 0) != 0) {
			mpfr_reldiff(endAnalysisRelError, memory[i]->value, endAnalysisOrg, STD_RND);

			if (mpfr_cmp_ui(endAnalysisRelError, 0) != 0) {
				fpsWithError++;

				if (clo_ignoreLibraries) {
					VG_(describe_IP)(memory[i]->origin, description, DESCRIPTION_SIZE);
					if (ignoreFile(description)) {
						skippedLibrary++;
						continue;
					}
				}

				if (numWritten < MAX_ENTRIES_PER_FILE) {
					numWritten++;
					writeShadowValue(file, memory[i], total);

					if (j <= MAX_DUMPED_GRAPHS) {
						VG_(sprintf)(filename, "%s_%d_%d.vcg", clientName, j, i);
						if (dumpGraph(filename, memory[i]->key, True, True)) {
							VG_(sprintf)(formatBuf, "    graph dumped: %s\n", filename);
							my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));

							j++;
						}
					}

					VG_(sprintf)(formatBuf, "\n");
					my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
				}
			} else {
				hasError = False;
			}
		} else {
			hasError = False;
		}
		if (!clo_ignoreAccurate && !hasError && numWritten < MAX_ENTRIES_PER_FILE) {
			numWritten++;
			writeShadowValue(file, memory[i], i);
			VG_(sprintf)(formatBuf, "\n");
			my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
		}
	}

	VG_(sprintf)(formatBuf, "%'u%s out of %'u shadow values are in this file\n", numWritten, 
		numWritten == MAX_ENTRIES_PER_FILE ? " (maximum number written to file)" : "", total);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	if (skippedLibrary > 0) {
		VG_(sprintf)(formatBuf, "%'u are skipped because they are from a library\n", skippedLibrary);
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	}
	VG_(sprintf)(formatBuf, "%'u out of %'u shadow values have an error\n", fpsWithError, total);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	VG_(sprintf)(formatBuf, "%'u graph(s) have been dumped\n", j - 1);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	VG_(sprintf)(formatBuf, "total number of floating-point operations: %'lu\n", fpOps);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	VG_(sprintf)(formatBuf, "number of executed blocks: %'lu\n", sbExecuted);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));

	fwrite_flush();
	VG_(close)(file);
	VG_(umsg)("SHADOW VALUES (%s): successful\n", fname);
}

static void endAnalysis(void) {
	UInt n_memory = 0;
	ShadowValue** memory = VG_(HT_to_array)(globalMemory, &n_memory);
	VG_(ssort)(memory, n_memory, sizeof(VgHashNode*), compareShadowValues);

	//writeMemoryRelError(memory, n_memory);
	//writeMemoryCanceled(memory, n_memory);
	//writeMemorySpecial(memory, n_memory);

	VG_(free)(memory);
}

static Int compareMVAddr(void* n1, void* n2) {
	MeanValue* mv1 = *(MeanValue**)n1;
	MeanValue* mv2 = *(MeanValue**)n2;
	if (mv1->key < mv2->key) return -1;
	if (mv1->key > mv2->key) return  1;
	return 0;
}

static Int compareMVCanceled(void* n1, void* n2) {
	MeanValue* mv1 = *(MeanValue**)n1;
	MeanValue* mv2 = *(MeanValue**)n2;
	if (mv1->cancellationBadnessMax < mv2->cancellationBadnessMax) return 1;
	if (mv1->cancellationBadnessMax > mv2->cancellationBadnessMax) return -1;
	if (mv1->canceledMax < mv2->canceledMax) return 1;
	if (mv1->canceledMax > mv2->canceledMax) return -1;
	return 0;
}

static Int compareMVIntroError(void* n1, void* n2) {
	MeanValue* mv1 = *(MeanValue**)n1;
	MeanValue* mv2 = *(MeanValue**)n2;

	getIntroducedError(&compareIntroErr1, mv1);
	getIntroducedError(&compareIntroErr2, mv2);
	Int cmp = mpfr_cmp(compareIntroErr1, compareIntroErr2);

	if (cmp < 0) return 1;
	if (cmp > 0) return -1;
	return 0;
}

static void writeMeanValues(Char* fname, Int (*cmpFunc) (void*, void*), Bool forCanceled) {
	if (!clo_computeMeanValue) {
		return;
	}

	getFileName(fname);
	SysRes fileRes = VG_(open)(fname, VKI_O_CREAT|VKI_O_TRUNC|VKI_O_WRONLY, VKI_S_IRUSR|VKI_S_IWUSR);
	if (sr_isError(fileRes)) {
		VG_(umsg)("MEAN ERRORS (%s): Failed to create or open the file!\n", fname);
		return;
	}
	Int file = sr_Res(fileRes);
	writeWarning(file);

	UInt n_values = 0;
	MeanValue** values = VG_(HT_to_array)(meanValues, &n_values);
	VG_(ssort)(values, n_values, sizeof(VgHashNode*), cmpFunc);

	mpfr_t meanError, maxError, introducedError, err1, err2;
	mpfr_inits(meanError, maxError, introducedError, err1, err2, NULL);
	Int fpsWritten = 0;
	Int skipped = 0;
	Int skippedLibrary = 0;
	Int i;
	for (i = 0; i < n_values; i++) {
		if (clo_ignoreAccurate && !forCanceled && mpfr_cmp_ui(values[i]->sum, 0) == 0) {
			skipped++;
			continue;
		}

		if (clo_ignoreAccurate && forCanceled && values[i]->canceledMax == 0) {
			skipped++;
			continue;
		} 

		VG_(describe_IP)(values[i]->key, description, DESCRIPTION_SIZE);
		if (ignoreFile(description)) {
			skippedLibrary++;
			continue;
		}

		if (i > MAX_ENTRIES_PER_FILE) {
			continue;
		}

		fpsWritten++;
		mpfr_div_ui(meanError, values[i]->sum, values[i]->count, STD_RND);

		opToStr(values[i]->op);
		Char meanErrorStr[MPFR_BUFSIZE];
		mpfrToString(meanErrorStr, &meanError);
		Char maxErrorStr[MPFR_BUFSIZE];
		mpfrToString(maxErrorStr, &(values[i]->max));

		VG_(sprintf)(formatBuf, "%s %s (%'u)\n", description, opStr, values[i]->count);
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
		VG_(sprintf)(formatBuf, "    avg error: %s\n", meanErrorStr);
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
		VG_(sprintf)(formatBuf, "    max error: %s\n", maxErrorStr);
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));

		if (values[i]->overflow) {
			VG_(sprintf)(formatBuf, "    canceled bits - max: %'ld, avg: overflow\n", values[i]->canceledMax);
		} else {
			mpfr_exp_t meanCanceledBits = values[i]->canceledSum / values[i]->count;
			VG_(sprintf)(formatBuf, "    canceled bits - max: %'ld, avg: %'ld\n", values[i]->canceledMax, meanCanceledBits);
		}
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));

		if (clo_bad_cancellations) {
			Char avgCancellationBadness[10];
			VG_(percentify)(values[i]->cancellationBadnessSum, values[i]->count * values[i]->cancellationBadnessMax, 2, 10, avgCancellationBadness);
			VG_(sprintf)(formatBuf, "    cancellation badness - max: %'ld, avg (sum/(count*max)):%s\n", values[i]->cancellationBadnessMax, avgCancellationBadness);
			my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
		}

		getIntroducedError(&introducedError, values[i]);
		if (mpfr_cmp_ui(introducedError, 0) > 0) {
			Char introErrorStr[MPFR_BUFSIZE];
			mpfrToString(introErrorStr, &introducedError);
			VG_(sprintf)(formatBuf, "    introduced error (max path): %s\n", introErrorStr);
		} else {
			VG_(sprintf)(formatBuf, "    no error has been introduced (max path)\n");
		}
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
		VG_(sprintf)(formatBuf, "    origin of the arguments (max path): 0x%lX, 0x%lX\n\n", values[i]->arg1, values[i]->arg2);
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	}

	VG_(sprintf)(formatBuf, "%'d%s out of %'d operations are listed in this file\n", 
		fpsWritten, fpsWritten == MAX_ENTRIES_PER_FILE ? " (maximum number written to file)" : "", n_values);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	if (skipped > 0) {
		if (forCanceled) {
			VG_(sprintf)(formatBuf, "%'d operations have been skipped because no bits were canceled\n", skipped);
			my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
		} else {
			VG_(sprintf)(formatBuf, "%'d operations have been skipped because they are accurate\n", skipped);
			my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
		}
	}
	if (skippedLibrary > 0) {
		VG_(sprintf)(formatBuf, "%'d operations have been skipped because they are in a library\n", skippedLibrary);
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	}

	fwrite_flush();
	VG_(close)(file);
	VG_(umsg)("MEAN ERRORS (%s): successful\n", fname);

	mpfr_clears(meanError, maxError, introducedError, err1, err2, NULL);
	VG_(free)(values);
}

static Int compareStageReports(void* n1, void* n2) {
	StageReport* sr1 = *(StageReport**)n1;
	StageReport* sr2 = *(StageReport**)n2;
	if (sr1->count < sr2->count) return 1;
	if (sr1->count > sr2->count) return -1;

	if (sr1->iterMin < sr2->iterMin) return 1;
	if (sr1->iterMin > sr2->iterMin) return -1;

	if (sr1->iterMax < sr2->iterMax) return 1;
	if (sr1->iterMax > sr2->iterMax) return -1;

	if (sr1->origin < sr2->origin) return 1;
	if (sr1->origin > sr2->origin) return -1;
	return 0;
}

static void writeStageReports(Char* fname) {
	Bool writeReports = False;
	Int i;
	for (i = 0; i < MAX_STAGES; i++) {
		if (stageReports[i]) writeReports = True;
	}
	if (!writeReports) {
		return;
	}

	getFileName(fname);
	SysRes fileRes = VG_(open)(fname, VKI_O_CREAT|VKI_O_TRUNC|VKI_O_WRONLY, VKI_S_IRUSR|VKI_S_IWUSR);
	if (sr_isError(fileRes)) {
		VG_(umsg)("STAGE REPORTS (%s): Failed to create or open the file!\n", fname);
		return;
	}
	Int file = sr_Res(fileRes);
	writeWarning(file);

	Int reportsWritten = 0;
	Int totalReports = 0;
	Int numStages = 0;
	Int j;
	for (i = 0; i < MAX_STAGES; i++) {
		if (!stageReports[i]) {
			continue;
		}
		numStages++;

		UInt n_reports = 0;
		StageReport** reports = VG_(HT_to_array)(stageReports[i], &n_reports);
		VG_(ssort)(reports, n_reports, sizeof(VgHashNode*), compareStageReports);
		totalReports += n_reports;

		VG_(sprintf)(formatBuf, "Stage %d:\n\n", i);
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));

		for (j = 0; j < n_reports; j++) {
			if (reportsWritten > MAX_ENTRIES_PER_FILE) {
				break;
			}
			/* avoid output of duplicates */
			if (j > 0 && reports[j-1]->count == reports[j]->count &&
				reports[j-1]->iterMin == reports[j]->iterMin && reports[j-1]->iterMax == reports[j]->iterMax &&
				reports[j-1]->origin == reports[j]->origin)
			{
				totalReports--;
				continue;
			}
			reportsWritten++;

			VG_(sprintf)(formatBuf, "(%d) 0x%lX (%'u)\n", i, reports[j]->key, reports[j]->count);
			my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
			VG_(sprintf)(formatBuf, "    executions: [%u, %u]\n", reports[j]->iterMin, reports[j]->iterMax);
			my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
			VG_(sprintf)(formatBuf, "    origin: 0x%lX\n\n", reports[j]->origin);
			my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
		}

		VG_(sprintf)(formatBuf, "\n");
		my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	
		VG_(free)(reports);
		if (reportsWritten > MAX_ENTRIES_PER_FILE) {
			break;
		}
	}

	VG_(sprintf)(formatBuf, "%'d%s out of %'d reports are listed in this file\n", 
		reportsWritten, reportsWritten == MAX_ENTRIES_PER_FILE ? " (maximum number written to file)" : "", totalReports);
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	VG_(sprintf)(formatBuf, "%d stage%s produced reports\n", numStages, numStages > 1 ? "s" : "");
	my_fwrite(file, (void*)formatBuf, VG_(strlen)(formatBuf));
	
	fwrite_flush();
	VG_(close)(file);
	VG_(umsg)("STAGE REPORTS (%s): successful\n", fname);
}

static void fd_fini(Int exitcode) {
	endAnalysis();

	/*HChar* clientName = VG_(args_the_exename);
	VG_(sprintf)(filename, "%s_mean_errors_addr", clientName);
	writeMeanValues(filename, &compareMVAddr, False);
	if (clo_bad_cancellations) {
		VG_(sprintf)(filename, "%s_mean_errors_canceled", clientName);
		writeMeanValues(filename, &compareMVCanceled, True);
	}
	VG_(sprintf)(filename, "%s_mean_errors_intro", clientName);
	writeMeanValues(filename, &compareMVIntroError, False);

	VG_(sprintf)(filename, "%s_stage_reports", clientName);
	writeStageReports(filename);*/

#ifndef NDEBUG
	VG_(umsg)("DEBUG - Client exited with code: %d\n", exitcode);
	VG_(dmsg)("DEBUG - SBs: %'lu, executed: %'lu, instr: %'lu\n", sbCounter, sbExecuted, totalIns);
	VG_(dmsg)("DEBUG - ShadowValues (frees/mallocs): %'lu/%'lu, diff: %'lu\n", avFrees, avMallocs, avMallocs - avFrees);
	VG_(dmsg)("DEBUG - Floating-point operations: %'lu\n", fpOps);
	VG_(dmsg)("DEBUG - Max temps: %'u\n", maxTemps);
	VG_(dmsg)("OPTIMIZATION - GET:   total %'u, ignored: %'u\n", getCount, getsIgnored);
	VG_(dmsg)("OPTIMIZATION - STORE: total %'u, ignored: %'u\n", storeCount, storesIgnored);
	VG_(dmsg)("OPTIMIZATION - PUT:   total %'u, ignored: %'u\n", putCount, putsIgnored);
	VG_(dmsg)("OPTIMIZATION - LOAD:  total %'u, ignored: %'u\n", loadCount, loadsIgnored);
#endif
}

/* Returns True if there is a return value. */
static Bool fd_handle_client_request(ThreadId tid, UWord* arg, UWord* ret) {
	switch (arg[0]) {
		case VG_USERREQ__PRINT_ERROR:
			printError((Char*)arg[1], arg[2], False);
			break;
		case VG_USERREQ__COND_PRINT_ERROR:
			printError((Char*)arg[1], arg[2], True);
			break;
		case VG_USERREQ__DUMP_ERROR_GRAPH:
			dumpGraph((Char*)arg[1], arg[2], False, False);
			break;
		case VG_USERREQ__COND_DUMP_ERROR_GRAPH:
			dumpGraph((Char*)arg[1], arg[2], True, False);
			break;
		case VG_USERREQ__BEGIN_STAGE:
			stageStart((Int)arg[1]);
			break;
		case VG_USERREQ__END_STAGE:
			stageEnd((Int)arg[1]);
			break;
		case VG_USERREQ__CLEAR_STAGE:
			stageClear((Int)arg[1]);
			break;
		case VG_USERREQ__ERROR_GREATER:
			*ret  = (UWord)isErrorGreater(arg[1], arg[2]);
			return True;
		case VG_USERREQ__RESET:
			resetShadowValues();
			break;
		case VG_USERREQ__INSERT_SHADOW:
			insertShadow(arg[1]);
			break;
		/*****************/
		case VG_USERREQ__SET_SHADOW:
			setShadow(arg[1]);
			break;
		case VG_USERREQ__ORIGINAL_TO_SHADOW:
			originalToShadow(arg[1]);
			break;
		case VG_USERREQ__SHADOW_TO_ORIGINAL:
			shadowToOriginal(arg[1]);
			break;
		case VG_USERREQ__SET_ORIGINAL:
			setOriginal(arg[1], arg[2]);
			break;
		case VG_USERREQ__SET_SHADOW_BY:
			setShadowBy(arg[1], arg[2]);
			break;
	    case VG_USERREQ__GET_RELATIVE_ERROR:
	    	getRelativeError(arg[1], (Char*)arg[2]);
	    	break;
	    case VG_USERREQ__PSO_BEGIN_RUN:
	    	beginOneRun();
	    	break;
	    case VG_USERREQ__PSO_END_RUN:
	    	endOneRun();
	    	break;
	    case VG_USERREQ__PSO_BEGIN_INSTANCE:
	    	beginOneInstance();
	    	break;
	    case VG_USERREQ__IS_PSO_FINISHED:
	    	*ret = (UWord)isPSOFinished();
	    	return True;
	    case VG_USERREQ__GET_SHADOW:
	    	getShadow(arg[1], (Char*)arg[2]);
	    	break;
	    case VG_USERREQ__PRINT_VALUES:
	    	printOriginalAndShadow((Char*)arg[1], arg[2], arg[3]);
	    	break;
		/*****************/
		case VG_USERREQ__BEGIN:
			beginAnalyzing();
			break;
		case VG_USERREQ__END:
			endAnalyzing();
			break;
	}
	return False;
}

static void* gmp_alloc(size_t t) {
	return VG_(malloc)("fd.gmp_alloc.1", t);
}

static void* gmp_realloc(void* p, size_t t1, size_t t2) {
	return VG_(realloc)("fd.gmp_realloc.1", p, t1);
}

static void gmp_free(void* p, size_t t) {
	VG_(free)(p);
}

static void fd_post_clo_init(void) {
	VG_(umsg)("precision=%ld\n", clo_precision);
	VG_(umsg)("mean-error=%s\n", clo_computeMeanValue ? "yes" : "no");
	VG_(umsg)("ignore-libraries=%s\n", clo_ignoreLibraries ? "yes" : "no");
	VG_(umsg)("ignore-accurate=%s\n", clo_ignoreAccurate ? "yes" : "no");
	VG_(umsg)("sim-original=%s\n", clo_simulateOriginal ? "yes" : "no");
	VG_(umsg)("analyze-all=%s\n", clo_analyze ? "yes" : "no");
	VG_(umsg)("bad-cancellations=%s\n", clo_bad_cancellations ? "yes" : "no");
    VG_(umsg)("ignore-end=%s\n", clo_ignore_end ? "yes" : "no");
    VG_(umsg)("error-localization=%s\n", clo_error_localization ? "yes" : "no");
    VG_(umsg)("print-every-error=%s\n", clo_print_every_error ? "yes" : "no");
    VG_(umsg)("detect-pso=%s\n", clo_detect_pso ? "yes" : "no");
    VG_(umsg)("goto-shadow-branch=%s\n", clo_goto_shadow_branch ? "yes" : "no");
    VG_(umsg)("track-int=%s\n", clo_track_int ? "yes" : "no");

	mpfr_set_default_prec(clo_precision);
	defaultEmin = mpfr_get_emin();
	defaultEmax = mpfr_get_emax();

	globalMemory = VG_(HT_construct)("Global memory");
	meanValues = VG_(HT_construct)("Mean values");
	detectedPSO = VG_(HT_construct)("Detected precision-specific operations");

	storeArgs = VG_(malloc)("fd.init.1", sizeof(Store));
	muxArgs = VG_(malloc)("fd.init.2", sizeof(Mux0X));
	unOpArgs = VG_(malloc)("fd.init.3", sizeof(UnOp));
	binOpArgs = VG_(malloc)("fd.init.4", sizeof(BinOp));
	triOpArgs = VG_(malloc)("fd.init.5", sizeof(TriOp));
	circRegs = VG_(malloc)("fd.init.6", sizeof(CircularRegs));

	mpfr_inits(meanOrg, meanRelError, NULL);
	mpfr_inits(stageOrg, stageDiff, stageRelError, NULL);
	mpfr_inits(dumpGraphOrg, dumpGraphRel, dumpGraphDiff, dumpGraphMeanError, dumpGraphErr1, dumpGraphErr2, NULL);
	mpfr_inits(endAnalysisOrg, endAnalysisRelError, NULL);

	mpfr_inits(introMaxError, introErr1, introErr2, NULL);
	mpfr_inits(compareIntroErr1, compareIntroErr2, NULL);
	mpfr_inits(writeSvOrg, writeSvDiff, writeSvRelError, NULL);
	mpfr_init(cancelTemp);
	mpfr_inits(arg1tmpX, arg2tmpX, arg3tmpX, NULL);
	mpfr_inits(arg1midX, arg2midX, arg3midX, NULL);
	mpfr_inits(arg1oriX, arg2oriX, arg3oriX, NULL);
	mpfr_set_d(arg1midX, 1.0, STD_RND);
	mpfr_set_d(arg2midX, 1.0, STD_RND);
	mpfr_set_d(arg3midX, 1.0, STD_RND);
	mpfr_set_d(arg1oriX, 1.0, STD_RND);
	mpfr_set_d(arg2oriX, 1.0, STD_RND);
	mpfr_set_d(arg3oriX, 1.0, STD_RND);

	Int i;
	for (i = 0; i < TMP_COUNT; i++) {
		sTmp[i] = VG_(malloc)("fd.init.7", sizeof(ShadowTmp));
		sTmp[i]->U128 = VG_(malloc)("fd.init.8", sizeof(UInt) * 4);	/* 128bit */
	}

	for (i = 0; i < CONST_COUNT; i++) {
		sConst[i] = VG_(malloc)("fd.init.9", sizeof(ShadowTmp));
	}

	Int j;
	for (i = 0; i < VG_N_THREADS; i++) {
		for (j = 0; j < MAX_REGISTERS; j++) {
			threadRegisters[i][j] = NULL;
		}
	}
	for (i = 0; i < MAX_TEMPS; i++) {
		localTemps[i] = NULL;
	}

	for (i = 0; i < MAX_STAGES; i++) {
		stages[i] = NULL;
		stageReports[i] = NULL;
	}

	unsupportedOps = VG_(OSetWord_Create)(VG_(malloc), "fd.init.10", VG_(free));
}

static void fd_pre_clo_init(void) {
   	VG_(details_name)            ("FpDebug");
   	VG_(details_version)         ("0.1");
   	VG_(details_description)     ("Floating-point arithmetic debugger");
   	VG_(details_copyright_author)("Copyright (C) 2010-2011 by Florian Benz.");
   	VG_(details_bug_reports_to)  ("florianbenz1@gmail.com");

   	VG_(basic_tool_funcs)        (fd_post_clo_init,
                                 fd_instrument,
                                 fd_fini);

	VG_(needs_command_line_options)(fd_process_cmd_line_option,
									fd_print_usage,
									fd_print_debug_usage);
	
	VG_(needs_client_requests)   (fd_handle_client_request);

	/* Calls to C library functions in GMP and MPFR have to be replaced with the Valgrind versions.
	   The function mp_set_memory_functions is part of GMP and thus MPFR, all others have been added 
	   to MPFR. Therefore, this only works with pateched versions of GMP and MPFR. */
	mp_set_memory_functions(gmp_alloc, gmp_realloc, gmp_free);
	mpfr_set_strlen_function(VG_(strlen));
	mpfr_set_strcpy_function(VG_(strcpy));
	mpfr_set_memmove_function(VG_(memmove));
	mpfr_set_memcmp_function(VG_(memcmp));
	mpfr_set_memset_function(VG_(memset));
}

VG_DETERMINE_INTERFACE_VERSION(fd_pre_clo_init)

/*--------------------------------------------------------------------*/
/*--- end                                                          ---*/
/*--------------------------------------------------------------------*/

