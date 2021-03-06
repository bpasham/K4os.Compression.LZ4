/*
    LZ4 HC - High Compression Mode of LZ4
    Copyright (C) 2011-2017, Yann Collet.

    BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following disclaimer
    in the documentation and/or other materials provided with the
    distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

    You can contact the author at :
       - LZ4 source repository : https://github.com/lz4/lz4
       - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/
/* note : lz4hc is not an independent module, it requires lz4.h/lz4.c for proper compilation */


/* *************************************
*  Tuning Parameter
***************************************/

/*! HEAPMODE :
 *  Select how default compression function will allocate workplace memory,
 *  in stack (0:fastest), or in heap (1:requires malloc()).
 *  Since workplace is rather large, heap mode is recommended.
 */
#ifndef LZ4HC_HEAPMODE
#  define LZ4HC_HEAPMODE 1
#endif


/*===    Dependency    ===*/
#define LZ4_HC_STATIC_LINKING_ONLY
#include "lz4hc.h"


/*===   Common LZ4 definitions   ===*/
#if defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif
#if defined (__clang__)
#  pragma clang diagnostic ignored "-Wunused-function"
#endif

#define LZ4_COMMONDEFS_ONLY
#include "lz4.c"   /* LZ4_count, constants, mem */


/*===   Constants   ===*/
#define OPTIMAL_ML (int)((ML_MASK-1)+MINMATCH)


/*===   Macros   ===*/
#define MIN(a,b)   ( (a) < (b) ? (a) : (b) )
#define MAX(a,b)   ( (a) > (b) ? (a) : (b) )
#define HASH_FUNCTION(i)         (((i) * 2654435761U) >> ((MINMATCH*8)-LZ4HC_HASH_LOG))
#define DELTANEXTMAXD(p)         chainTable[(p) & LZ4HC_MAXD_MASK]    /* flexible, LZ4HC_MAXD dependent */
#define DELTANEXTU16(table, pos) table[(ushort)(pos)]   /* faster */

static uint LZ4HC_hashPtr(void* ptr) { return HASH_FUNCTION(LZ4_read32(ptr)); }



/**************************************
*  HC Compression
**************************************/
static void LZ4HC_init (LZ4HC_CCtx_internal* hc4, byte* start)
{
    MEM_INIT((void*)hc4->hashTable, 0, sizeof(hc4->hashTable));
    MEM_INIT(hc4->chainTable, 0xFF, sizeof(hc4->chainTable));
    hc4->nextToUpdate = 64 KB;
    hc4->base = start - 64 KB;
    hc4->end = start;
    hc4->dictBase = start - 64 KB;
    hc4->dictLimit = 64 KB;
    hc4->lowLimit = 64 KB;
}


/* Update chains up to ip (excluded) */
LZ4_FORCE_INLINE void LZ4HC_Insert (LZ4HC_CCtx_internal* hc4, byte* ip)
{
    ushort* chainTable = hc4->chainTable;
    uint* hashTable  = hc4->hashTable;
    byte* base = hc4->base;
    uint target = (uint)(ip - base);
    uint idx = hc4->nextToUpdate;

    while (idx < target) {
        uint h = LZ4HC_hashPtr(base+idx);
        size_t delta = idx - hashTable[h];
        if (delta>MAX_DISTANCE) delta = MAX_DISTANCE;
        DELTANEXTU16(chainTable, idx) = (ushort)delta;
        hashTable[h] = idx;
        idx++;
    }

    hc4->nextToUpdate = target;
}

/** LZ4HC_countBack() :
 * @return : negative value, nb of common bytes before ip/match */
LZ4_FORCE_INLINE
int LZ4HC_countBack(byte* ip, byte* match,
                    byte* iMin, byte* mMin)
{
    int back=0;
    while ( (ip+back > iMin)
         && (match+back > mMin)
         && (ip[back-1] == match[back-1]))
            back--;
    return back;
}

/* LZ4HC_countPattern() :
 * pattern32 must be a sample of repetitive pattern of length 1, 2 or 4 (but not 3!) */
static unsigned LZ4HC_countPattern(byte* ip, byte* iEnd, uint pattern32)
{
    byte* iStart = ip;
    reg_t pattern = (sizeof(pattern)==8) ? (reg_t)pattern32 + (((reg_t)pattern32) << 32) : pattern32;

    while ((ip < iEnd-(sizeof(pattern)-1))) {
        reg_t diff = LZ4_read_ARCH(ip) ^ pattern;
        if (!diff) { ip+=sizeof(pattern); continue; }
        ip += LZ4_NbCommonBytes(diff);
        return (unsigned)(ip - iStart);
    }

    if (LZ4_isLittleEndian()) {
        reg_t patternByte = pattern;
        while ((ip<iEnd) && (*ip == (byte)patternByte)) {
            ip++; patternByte >>= 8;
        }
    } else {  /* big endian */
        uint bitOffset = (sizeof(pattern)*8) - 8;
        while (ip < iEnd) {
            byte byte = (byte)(pattern >> bitOffset);
            if (*ip != byte) break;
            ip ++; bitOffset -= 8;
        }
    }

    return (unsigned)(ip - iStart);
}

/* LZ4HC_reverseCountPattern() :
 * pattern must be a sample of repetitive pattern of length 1, 2 or 4 (but not 3!)
 * read using natural platform endianess */
static unsigned LZ4HC_reverseCountPattern(byte* ip, byte* iLow, uint pattern)
{
    byte* iStart = ip;

    while ((ip >= iLow+4)) {
        if (LZ4_read32(ip-4) != pattern) break;
        ip -= 4;
    }
    {   byte* bytePtr = (byte*)(&pattern) + 3; /* works for any endianess */
        while ((ip>iLow)) {
            if (ip[-1] != *bytePtr) break;
            ip--; bytePtr--;
    }   }
    return (unsigned)(iStart - ip);
}

typedef enum { rep_untested, rep_not, rep_confirmed } repeat_state_e;

LZ4_FORCE_INLINE int LZ4HC_InsertAndGetWiderMatch (
    LZ4HC_CCtx_internal* hc4,
    byte* ip,
    byte* iLowLimit,
    byte* iHighLimit,
    int longest,
    byte** matchpos,
    byte** startpos,
    int maxNbAttempts,
    int patternAnalysis)
{
    ushort* chainTable = hc4->chainTable;
    uint* HashTable = hc4->hashTable;
    byte* base = hc4->base;
    uint dictLimit = hc4->dictLimit;
    byte* lowPrefixPtr = base + dictLimit;
    uint lowLimit = (hc4->lowLimit + 64 KB > (uint)(ip-base)) ? hc4->lowLimit : (uint)(ip - base) - MAX_DISTANCE;
    byte* dictBase = hc4->dictBase;
    int delta = (int)(ip-iLowLimit);
    int nbAttempts = maxNbAttempts;
    uint pattern = LZ4_read32(ip);
    uint matchIndex;
    repeat_state_e repeat = rep_untested;
    size_t srcPatternLength = 0;

    DEBUGLOG(7, "LZ4HC_InsertAndGetWiderMatch");
    /* First Match */
    LZ4HC_Insert(hc4, ip);
    matchIndex = HashTable[LZ4HC_hashPtr(ip)];
    DEBUGLOG(7, "First match at index %u / %u (lowLimit)",
                matchIndex, lowLimit);

    while ((matchIndex>=lowLimit) && (nbAttempts)) {
        DEBUGLOG(7, "remaining attempts : %i", nbAttempts);
        nbAttempts--;
        if (matchIndex >= dictLimit) {
            byte* matchPtr = base + matchIndex;
            if (*(iLowLimit + longest) == *(matchPtr - delta + longest)) {
                if (LZ4_read32(matchPtr) == pattern) {
                    int mlt = MINMATCH + LZ4_count(ip+MINMATCH, matchPtr+MINMATCH, iHighLimit);
    #if 0
                    /* more generic but unfortunately slower on clang */
                    int back = LZ4HC_countBack(ip, matchPtr, iLowLimit, lowPrefixPtr);
    #else
                    int back = 0;
                    while ( (ip+back > iLowLimit)
                         && (matchPtr+back > lowPrefixPtr)
                         && (ip[back-1] == matchPtr[back-1])) {
                            back--;
                    }
    #endif
                    mlt -= back;

                    if (mlt > longest) {
                        longest = mlt;
                        *matchpos = matchPtr+back;
                        *startpos = ip+back;
                }   }
            }
        } else {   /* matchIndex < dictLimit */
            byte* matchPtr = dictBase + matchIndex;
            if (LZ4_read32(matchPtr) == pattern) {
                int mlt;
                int back = 0;
                byte* vLimit = ip + (dictLimit - matchIndex);
                if (vLimit > iHighLimit) vLimit = iHighLimit;
                mlt = LZ4_count(ip+MINMATCH, matchPtr+MINMATCH, vLimit) + MINMATCH;
                if ((ip+mlt == vLimit) && (vLimit < iHighLimit))
                    mlt += LZ4_count(ip+mlt, base+dictLimit, iHighLimit);
                while ( (ip+back > iLowLimit)
                     && (matchIndex+back > lowLimit)
                     && (ip[back-1] == matchPtr[back-1]))
                        back--;
                mlt -= back;
                if (mlt > longest) {
                    longest = mlt;
                    *matchpos = base + matchIndex + back;
                    *startpos = ip + back;
        }   }   }

        {   uint nextOffset = DELTANEXTU16(chainTable, matchIndex);
            matchIndex -= nextOffset;
            if (patternAnalysis && nextOffset==1) {
                /* may be a repeated pattern */
                if (repeat == rep_untested) {
                    if ( ((pattern & 0xFFFF) == (pattern >> 16))
                      &  ((pattern & 0xFF)   == (pattern >> 24)) ) {
                        repeat = rep_confirmed;
                        srcPatternLength = LZ4HC_countPattern(ip+4, iHighLimit, pattern) + 4;
                    } else {
                        repeat = rep_not;
                }   }
                if ( (repeat == rep_confirmed)
                  && (matchIndex >= dictLimit) ) {   /* same segment only */
                    byte* matchPtr = base + matchIndex;
                    if (LZ4_read32(matchPtr) == pattern) {  /* good candidate */
                        size_t forwardPatternLength = LZ4HC_countPattern(matchPtr+sizeof(pattern), iHighLimit, pattern) + sizeof(pattern);
                        byte* maxLowPtr = (lowPrefixPtr + MAX_DISTANCE >= ip) ? lowPrefixPtr : ip - MAX_DISTANCE;
                        size_t backLength = LZ4HC_reverseCountPattern(matchPtr, maxLowPtr, pattern);
                        size_t currentSegmentLength = backLength + forwardPatternLength;

                        if ( (currentSegmentLength >= srcPatternLength)   /* current pattern segment large enough to contain full srcPatternLength */
                          && (forwardPatternLength <= srcPatternLength) ) { /* haven't reached this position yet */
                            matchIndex += (uint)forwardPatternLength - (uint)srcPatternLength;  /* best position, full pattern, might be followed by more match */
                        } else {
                            matchIndex -= (uint)backLength;   /* let's go to farthest segment position, will find a match of length currentSegmentLength + maybe some back */
                        }
        }   }   }   }
    }  /* while ((matchIndex>=lowLimit) && (nbAttempts)) */

    return longest;
}

LZ4_FORCE_INLINE
int LZ4HC_InsertAndFindBestMatch(LZ4HC_CCtx_internal* hc4,   /* Index table will be updated */
                                 byte* ip, byte* iLimit,
                                 byte** matchpos,
                                 int maxNbAttempts,
                                 int patternAnalysis)
{
    byte* uselessPtr = ip;
    /* note : LZ4HC_InsertAndGetWiderMatch() is able to modify the starting position of a match (*startpos),
     * but this won't be the case here, as we define iLowLimit==ip,
     * so LZ4HC_InsertAndGetWiderMatch() won't be allowed to search past ip */
    return LZ4HC_InsertAndGetWiderMatch(hc4, ip, ip, iLimit, MINMATCH-1, matchpos, &uselessPtr, maxNbAttempts, patternAnalysis);
}



typedef enum {
    noLimit = 0,
    limitedOutput = 1,
    limitedDestSize = 2,
} limitedOutput_directive;

/* LZ4HC_encodeSequence() :
 * @return : 0 if ok,
 *           1 if buffer issue detected */
LZ4_FORCE_INLINE int LZ4HC_encodeSequence (
    byte** ip,
    byte** op,
    byte** anchor,
    int matchLength,
    byte* match,
    limitedOutput_directive limit,
    byte* oend)
{
    size_t length;
    byte* token = (*op)++;

#if defined(LZ4_DEBUG) && (LZ4_DEBUG >= 2)
    static byte* start = NULL;
    static uint totalCost = 0;
    uint pos = (start==NULL) ? 0 : (uint)(*anchor - start);
    uint ll = (uint)(*ip - *anchor);
    uint llAdd = (ll>=15) ? ((ll-15) / 255) + 1 : 0;
    uint mlAdd = (matchLength>=19) ? ((matchLength-19) / 255) + 1 : 0;
    uint cost = 1 + llAdd + ll + 2 + mlAdd;
    if (start==NULL) start = *anchor;  /* only works for single segment */
    //g_debuglog_enable = (pos >= 2228) & (pos <= 2262);
    DEBUGLOG(2, "pos:%7u -- literals:%3u, match:%4i, offset:%5u, cost:%3u + %u",
                pos,
                (uint)(*ip - *anchor), matchLength, (uint)(*ip-match),
                cost, totalCost);
    totalCost += cost;
#endif

    /* Encode Literal length */
    length = (size_t)(*ip - *anchor);
    if ((limit) && ((*op + (length >> 8) + length + (2 + 1 + LASTLITERALS)) > oend)) return 1;   /* Check output limit */
    if (length >= RUN_MASK) {
        size_t len = length - RUN_MASK;
        *token = (RUN_MASK << ML_BITS);
        for(; len >= 255 ; len -= 255) *(*op)++ = 255;
        *(*op)++ = (byte)len;
    } else {
        *token = (byte)(length << ML_BITS);
    }

    /* Copy Literals */
    LZ4_wildCopy(*op, *anchor, (*op) + length);
    *op += length;

    /* Encode Offset */
    LZ4_writeLE16(*op, (ushort)(*ip-match)); *op += 2;

    /* Encode MatchLength */
    assert(matchLength >= MINMATCH);
    length = (size_t)(matchLength - MINMATCH);
    if ((limit) && (*op + (length >> 8) + (1 + LASTLITERALS) > oend)) return 1;   /* Check output limit */
    if (length >= ML_MASK) {
        *token += ML_MASK;
        length -= ML_MASK;
        for(; length >= 510 ; length -= 510) { *(*op)++ = 255; *(*op)++ = 255; }
        if (length >= 255) { length -= 255; *(*op)++ = 255; }
        *(*op)++ = (byte)length;
    } else {
        *token += (byte)(length);
    }

    /* Prepare next loop */
    *ip += matchLength;
    *anchor = *ip;

    return 0;
}

/* btopt */
#include "lz4opt.h"


static int LZ4HC_compress_hashChain (
    LZ4HC_CCtx_internal* ctx,
    byte* source,
    byte* dest,
    int* srcSizePtr,
    int maxOutputSize,
    unsigned maxNbAttempts,
    limitedOutput_directive limit
    )
{
    int inputSize = *srcSizePtr;
    int patternAnalysis = (maxNbAttempts > 64);   /* levels 8+ */

    byte* ip = (byte*) source;
    byte* anchor = ip;
    byte* iend = ip + inputSize;
    byte* mflimit = iend - MFLIMIT;
    byte* matchlimit = (iend - LASTLITERALS);

    byte* optr = (byte*) dest;
    byte* op = (byte*) dest;
    byte* oend = op + maxOutputSize;

    int   ml, ml2, ml3, ml0;
    byte* ref = NULL;
    byte* start2 = NULL;
    byte* ref2 = NULL;
    byte* start3 = NULL;
    byte* ref3 = NULL;
    byte* start0;
    byte* ref0;

    /* init */
    *srcSizePtr = 0;
    if (limit == limitedDestSize) oend -= LASTLITERALS;                  /* Hack for support LZ4 format restriction */
    if (inputSize < LZ4_minLength) goto _last_literals;                  /* Input too small, no compression (all literals) */

    /* Main Loop */
    while (ip < mflimit) {
        ml = LZ4HC_InsertAndFindBestMatch (ctx, ip, matchlimit, &ref, maxNbAttempts, patternAnalysis);
        if (ml<MINMATCH) { ip++; continue; }

        /* saved, in case we would skip too much */
        start0 = ip;
        ref0 = ref;
        ml0 = ml;

_Search2:
        if (ip+ml < mflimit)
            ml2 = LZ4HC_InsertAndGetWiderMatch(ctx,
                            ip + ml - 2, ip + 0, matchlimit, ml, &ref2, &start2,
                            maxNbAttempts, patternAnalysis);
        else
            ml2 = ml;

        if (ml2 == ml) { /* No better match */
            optr = op;
            if (LZ4HC_encodeSequence(&ip, &op, &anchor, ml, ref, limit, oend)) goto _dest_overflow;
            continue;
        }

        if (start0 < ip) {
            if (start2 < ip + ml0) {  /* empirical */
                ip = start0;
                ref = ref0;
                ml = ml0;
            }
        }

        /* Here, start0==ip */
        if ((start2 - ip) < 3) {  /* First Match too small : removed */
            ml = ml2;
            ip = start2;
            ref =ref2;
            goto _Search2;
        }

_Search3:
        /* At this stage, we have :
        *  ml2 > ml1, and
        *  ip1+3 <= ip2 (usually < ip1+ml1) */
        if ((start2 - ip) < OPTIMAL_ML) {
            int correction;
            int new_ml = ml;
            if (new_ml > OPTIMAL_ML) new_ml = OPTIMAL_ML;
            if (ip+new_ml > start2 + ml2 - MINMATCH) new_ml = (int)(start2 - ip) + ml2 - MINMATCH;
            correction = new_ml - (int)(start2 - ip);
            if (correction > 0) {
                start2 += correction;
                ref2 += correction;
                ml2 -= correction;
            }
        }
        /* Now, we have start2 = ip+new_ml, with new_ml = min(ml, OPTIMAL_ML=18) */

        if (start2 + ml2 < mflimit)
            ml3 = LZ4HC_InsertAndGetWiderMatch(ctx,
                            start2 + ml2 - 3, start2, matchlimit, ml2, &ref3, &start3,
                            maxNbAttempts, patternAnalysis);
        else
            ml3 = ml2;

        if (ml3 == ml2) {  /* No better match : 2 sequences to encode */
            /* ip & ref are known; Now for ml */
            if (start2 < ip+ml)  ml = (int)(start2 - ip);
            /* Now, encode 2 sequences */
            optr = op;
            if (LZ4HC_encodeSequence(&ip, &op, &anchor, ml, ref, limit, oend)) goto _dest_overflow;
            ip = start2;
            optr = op;
            if (LZ4HC_encodeSequence(&ip, &op, &anchor, ml2, ref2, limit, oend)) goto _dest_overflow;
            continue;
        }

        if (start3 < ip+ml+3) {  /* Not enough space for match 2 : remove it */
            if (start3 >= (ip+ml)) {  /* can write Seq1 immediately ==> Seq2 is removed, so Seq3 becomes Seq1 */
                if (start2 < ip+ml) {
                    int correction = (int)(ip+ml - start2);
                    start2 += correction;
                    ref2 += correction;
                    ml2 -= correction;
                    if (ml2 < MINMATCH) {
                        start2 = start3;
                        ref2 = ref3;
                        ml2 = ml3;
                    }
                }

                optr = op;
                if (LZ4HC_encodeSequence(&ip, &op, &anchor, ml, ref, limit, oend)) goto _dest_overflow;
                ip  = start3;
                ref = ref3;
                ml  = ml3;

                start0 = start2;
                ref0 = ref2;
                ml0 = ml2;
                goto _Search2;
            }

            start2 = start3;
            ref2 = ref3;
            ml2 = ml3;
            goto _Search3;
        }

        /*
        * OK, now we have 3 ascending matches; let's write at least the first one
        * ip & ref are known; Now for ml
        */
        if (start2 < ip+ml) {
            if ((start2 - ip) < (int)ML_MASK) {
                int correction;
                if (ml > OPTIMAL_ML) ml = OPTIMAL_ML;
                if (ip + ml > start2 + ml2 - MINMATCH) ml = (int)(start2 - ip) + ml2 - MINMATCH;
                correction = ml - (int)(start2 - ip);
                if (correction > 0) {
                    start2 += correction;
                    ref2 += correction;
                    ml2 -= correction;
                }
            } else {
                ml = (int)(start2 - ip);
            }
        }
        optr = op;
        if (LZ4HC_encodeSequence(&ip, &op, &anchor, ml, ref, limit, oend)) goto _dest_overflow;

        ip = start2;
        ref = ref2;
        ml = ml2;

        start2 = start3;
        ref2 = ref3;
        ml2 = ml3;

        goto _Search3;
    }

_last_literals:
    /* Encode Last Literals */
    {   size_t lastRunSize = (size_t)(iend - anchor);  /* literals */
        size_t litLength = (lastRunSize + 255 - RUN_MASK) / 255;
        size_t totalSize = 1 + litLength + lastRunSize;
        if (limit == limitedDestSize) oend += LASTLITERALS;  /* restore correct value */
        if (limit && (op + totalSize > oend)) {
            if (limit == limitedOutput) return 0;  /* Check output limit */
            /* adapt lastRunSize to fill 'dest' */
            lastRunSize  = (size_t)(oend - op) - 1;
            litLength = (lastRunSize + 255 - RUN_MASK) / 255;
            lastRunSize -= litLength;
        }
        ip = anchor + lastRunSize;

        if (lastRunSize >= RUN_MASK) {
            size_t accumulator = lastRunSize - RUN_MASK;
            *op++ = (RUN_MASK << ML_BITS);
            for(; accumulator >= 255 ; accumulator -= 255) *op++ = 255;
            *op++ = (byte) accumulator;
        } else {
            *op++ = (byte)(lastRunSize << ML_BITS);
        }
        memcpy(op, anchor, lastRunSize);
        op += lastRunSize;
    }

    /* End */
    *srcSizePtr = (int) (((byte*)ip) - source);
    return (int) (((byte*)op)-dest);

_dest_overflow:
    if (limit == limitedDestSize) {
        op = optr;  /* restore correct out pointer */
        goto _last_literals;
    }
    return 0;
}


static int LZ4HC_compress_generic (
    LZ4HC_CCtx_internal* ctx,
    byte* src,
    byte* dst,
    int* srcSizePtr,
    int dstCapacity,
    int cLevel,
    limitedOutput_directive limit
    )
{
    typedef enum { lz4hc, lz4opt } lz4hc_strat_e;
    typedef struct {
        lz4hc_strat_e strat;
        uint nbSearches;
        uint targetLength;
    } cParams_t;
    static cParams_t clTable[LZ4HC_CLEVEL_MAX+1] = {
        { lz4hc,    2, 16 },  /* 0, unused */
        { lz4hc,    2, 16 },  /* 1, unused */
        { lz4hc,    2, 16 },  /* 2, unused */
        { lz4hc,    4, 16 },  /* 3 */
        { lz4hc,    8, 16 },  /* 4 */
        { lz4hc,   16, 16 },  /* 5 */
        { lz4hc,   32, 16 },  /* 6 */
        { lz4hc,   64, 16 },  /* 7 */
        { lz4hc,  128, 16 },  /* 8 */
        { lz4hc,  256, 16 },  /* 9 */
        { lz4opt,  96, 64 },  /*10==LZ4HC_CLEVEL_OPT_MIN*/
        { lz4opt, 512,128 },  /*11 */
        { lz4opt,8192, LZ4_OPT_NUM },  /* 12==LZ4HC_CLEVEL_MAX */
    };

    if (limit == limitedDestSize && dstCapacity < 1) return 0;         /* Impossible to store anything */
    if ((uint)*srcSizePtr > (uint)LZ4_MAX_INPUT_SIZE) return 0;          /* Unsupported input size (too large or negative) */

    ctx->end += *srcSizePtr;
    if (cLevel < 1) cLevel = LZ4HC_CLEVEL_DEFAULT;   /* note : convention is different from lz4frame, maybe something to review */
    cLevel = MIN(LZ4HC_CLEVEL_MAX, cLevel);
    assert(cLevel >= 0);
    assert(cLevel <= LZ4HC_CLEVEL_MAX);
    {   cParams_t cParam = clTable[cLevel];
        if (cParam.strat == lz4hc)
            return LZ4HC_compress_hashChain(ctx,
                                src, dst, srcSizePtr, dstCapacity,
                                cParam.nbSearches, limit);
        assert(cParam.strat == lz4opt);
        return LZ4HC_compress_optimal(ctx,
                            src, dst, srcSizePtr, dstCapacity,
                            cParam.nbSearches, cParam.targetLength, limit,
                            cLevel == LZ4HC_CLEVEL_MAX);  /* ultra mode */
    }
}


int LZ4_sizeofStateHC(void) { return sizeof(LZ4_streamHC_t); }

int LZ4_compress_HC_extStateHC (void* state, byte* src, byte* dst, int srcSize, int dstCapacity, int compressionLevel)
{
    LZ4HC_CCtx_internal* ctx = &((LZ4_streamHC_t*)state)->internal_donotuse;
    if (((size_t)(state)&(sizeof(void*)-1)) != 0) return 0;   /* Error : state is not aligned for pointers (32 or 64 bits) */
    LZ4HC_init (ctx, (byte*)src);
    if (dstCapacity < LZ4_compressBound(srcSize))
        return LZ4HC_compress_generic (ctx, src, dst, &srcSize, dstCapacity, compressionLevel, limitedOutput);
    else
        return LZ4HC_compress_generic (ctx, src, dst, &srcSize, dstCapacity, compressionLevel, noLimit);
}

int LZ4_compress_HC(byte* src, byte* dst, int srcSize, int dstCapacity, int compressionLevel)
{
#if defined(LZ4HC_HEAPMODE) && LZ4HC_HEAPMODE==1
    LZ4_streamHC_t* statePtr = (LZ4_streamHC_t*)malloc(sizeof(LZ4_streamHC_t));
#else
    LZ4_streamHC_t state;
    LZ4_streamHC_t* statePtr = &state;
#endif
    int cSize = LZ4_compress_HC_extStateHC(statePtr, src, dst, srcSize, dstCapacity, compressionLevel);
#if defined(LZ4HC_HEAPMODE) && LZ4HC_HEAPMODE==1
    free(statePtr);
#endif
    return cSize;
}

/* LZ4_compress_HC_destSize() :
 * only compatible with regular HC parser */
int LZ4_compress_HC_destSize(void* LZ4HC_Data, byte* source, byte* dest, int* sourceSizePtr, int targetDestSize, int cLevel)
{
    LZ4HC_CCtx_internal* ctx = &((LZ4_streamHC_t*)LZ4HC_Data)->internal_donotuse;
    LZ4HC_init(ctx, (byte*) source);
    return LZ4HC_compress_generic(ctx, source, dest, sourceSizePtr, targetDestSize, cLevel, limitedDestSize);
}



/**************************************
*  Streaming Functions
**************************************/
/* allocation */
LZ4_streamHC_t* LZ4_createStreamHC(void) { return (LZ4_streamHC_t*)malloc(sizeof(LZ4_streamHC_t)); }
int             LZ4_freeStreamHC (LZ4_streamHC_t* LZ4_streamHCPtr) {
    if (!LZ4_streamHCPtr) return 0;  /* support free on NULL */
    free(LZ4_streamHCPtr);
    return 0;
}


/* initialization */
void LZ4_resetStreamHC (LZ4_streamHC_t* LZ4_streamHCPtr, int compressionLevel)
{
    LZ4_STATIC_ASSERT(sizeof(LZ4HC_CCtx_internal) <= sizeof(size_t) * LZ4_STREAMHCSIZE_SIZET);   /* if compilation fails here, LZ4_STREAMHCSIZE must be increased */
    LZ4_streamHCPtr->internal_donotuse.base = NULL;
    LZ4_setCompressionLevel(LZ4_streamHCPtr, compressionLevel);
}

void LZ4_setCompressionLevel(LZ4_streamHC_t* LZ4_streamHCPtr, int compressionLevel)
{
    if (compressionLevel < 1) compressionLevel = 1;
    if (compressionLevel > LZ4HC_CLEVEL_MAX) compressionLevel = LZ4HC_CLEVEL_MAX;
    LZ4_streamHCPtr->internal_donotuse.compressionLevel = compressionLevel;
}

int LZ4_loadDictHC (LZ4_streamHC_t* LZ4_streamHCPtr, byte* dictionary, int dictSize)
{
    LZ4HC_CCtx_internal* ctxPtr = &LZ4_streamHCPtr->internal_donotuse;
    if (dictSize > 64 KB) {
        dictionary += dictSize - 64 KB;
        dictSize = 64 KB;
    }
    LZ4HC_init (ctxPtr, (byte*)dictionary);
    ctxPtr->end = (byte*)dictionary + dictSize;
    if (dictSize >= 4) LZ4HC_Insert (ctxPtr, ctxPtr->end-3);
    return dictSize;
}


/* compression */

static void LZ4HC_setExternalDict(LZ4HC_CCtx_internal* ctxPtr, byte* newBlock)
{
    if (ctxPtr->end >= ctxPtr->base + 4) LZ4HC_Insert (ctxPtr, ctxPtr->end-3);   /* Referencing remaining dictionary content */

    /* Only one memory segment for extDict, so any previous extDict is lost at this stage */
    ctxPtr->lowLimit  = ctxPtr->dictLimit;
    ctxPtr->dictLimit = (uint)(ctxPtr->end - ctxPtr->base);
    ctxPtr->dictBase  = ctxPtr->base;
    ctxPtr->base = newBlock - ctxPtr->dictLimit;
    ctxPtr->end  = newBlock;
    ctxPtr->nextToUpdate = ctxPtr->dictLimit;   /* match referencing will resume from there */
}

static int LZ4_compressHC_continue_generic (LZ4_streamHC_t* LZ4_streamHCPtr,
                                            byte* src, byte* dst,
                                            int* srcSizePtr, int dstCapacity,
                                            limitedOutput_directive limit)
{
    LZ4HC_CCtx_internal* ctxPtr = &LZ4_streamHCPtr->internal_donotuse;
    /* auto-init if forgotten */
    if (ctxPtr->base == NULL) LZ4HC_init (ctxPtr, (byte*) src);

    /* Check overflow */
    if ((size_t)(ctxPtr->end - ctxPtr->base) > 2 GB) {
        size_t dictSize = (size_t)(ctxPtr->end - ctxPtr->base) - ctxPtr->dictLimit;
        if (dictSize > 64 KB) dictSize = 64 KB;
        LZ4_loadDictHC(LZ4_streamHCPtr, (byte*)(ctxPtr->end) - dictSize, (int)dictSize);
    }

    /* Check if blocks follow each other */
    if ((byte*)src != ctxPtr->end) LZ4HC_setExternalDict(ctxPtr, (byte*)src);

    /* Check overlapping input/dictionary space */
    {   byte* sourceEnd = (byte*) src + *srcSizePtr;
        byte* dictBegin = ctxPtr->dictBase + ctxPtr->lowLimit;
        byte* dictEnd   = ctxPtr->dictBase + ctxPtr->dictLimit;
        if ((sourceEnd > dictBegin) && ((byte*)src < dictEnd)) {
            if (sourceEnd > dictEnd) sourceEnd = dictEnd;
            ctxPtr->lowLimit = (uint)(sourceEnd - ctxPtr->dictBase);
            if (ctxPtr->dictLimit - ctxPtr->lowLimit < 4) ctxPtr->lowLimit = ctxPtr->dictLimit;
        }
    }

    return LZ4HC_compress_generic (ctxPtr, src, dst, srcSizePtr, dstCapacity, ctxPtr->compressionLevel, limit);
}

int LZ4_compress_HC_continue (LZ4_streamHC_t* LZ4_streamHCPtr, byte* src, byte* dst, int srcSize, int dstCapacity)
{
    if (dstCapacity < LZ4_compressBound(srcSize))
        return LZ4_compressHC_continue_generic (LZ4_streamHCPtr, src, dst, &srcSize, dstCapacity, limitedOutput);
    else
        return LZ4_compressHC_continue_generic (LZ4_streamHCPtr, src, dst, &srcSize, dstCapacity, noLimit);
}

int LZ4_compress_HC_continue_destSize (LZ4_streamHC_t* LZ4_streamHCPtr, byte* src, byte* dst, int* srcSizePtr, int targetDestSize)
{
    return LZ4_compressHC_continue_generic(LZ4_streamHCPtr, src, dst, srcSizePtr, targetDestSize, limitedDestSize);
}



/* dictionary saving */

int LZ4_saveDictHC (LZ4_streamHC_t* LZ4_streamHCPtr, byte* safeBuffer, int dictSize)
{
    LZ4HC_CCtx_internal* streamPtr = &LZ4_streamHCPtr->internal_donotuse;
    int prefixSize = (int)(streamPtr->end - (streamPtr->base + streamPtr->dictLimit));
    if (dictSize > 64 KB) dictSize = 64 KB;
    if (dictSize < 4) dictSize = 0;
    if (dictSize > prefixSize) dictSize = prefixSize;
    memmove(safeBuffer, streamPtr->end - dictSize, dictSize);
    {   uint endIndex = (uint)(streamPtr->end - streamPtr->base);
        streamPtr->end = (byte*)safeBuffer + dictSize;
        streamPtr->base = streamPtr->end - endIndex;
        streamPtr->dictLimit = endIndex - dictSize;
        streamPtr->lowLimit = endIndex - dictSize;
        if (streamPtr->nextToUpdate < streamPtr->dictLimit) streamPtr->nextToUpdate = streamPtr->dictLimit;
    }
    return dictSize;
}


/***********************************
*  Deprecated Functions
***********************************/
/* These functions currently generate deprecation warnings */
/* Deprecated compression functions */
int LZ4_compressHC(byte* src, byte* dst, int srcSize) { return LZ4_compress_HC (src, dst, srcSize, LZ4_compressBound(srcSize), 0); }
int LZ4_compressHC_limitedOutput(byte* src, byte* dst, int srcSize, int maxDstSize) { return LZ4_compress_HC(src, dst, srcSize, maxDstSize, 0); }
int LZ4_compressHC2(byte* src, byte* dst, int srcSize, int cLevel) { return LZ4_compress_HC (src, dst, srcSize, LZ4_compressBound(srcSize), cLevel); }
int LZ4_compressHC2_limitedOutput(byte* src, byte* dst, int srcSize, int maxDstSize, int cLevel) { return LZ4_compress_HC(src, dst, srcSize, maxDstSize, cLevel); }
int LZ4_compressHC_withStateHC (void* state, byte* src, byte* dst, int srcSize) { return LZ4_compress_HC_extStateHC (state, src, dst, srcSize, LZ4_compressBound(srcSize), 0); }
int LZ4_compressHC_limitedOutput_withStateHC (void* state, byte* src, byte* dst, int srcSize, int maxDstSize) { return LZ4_compress_HC_extStateHC (state, src, dst, srcSize, maxDstSize, 0); }
int LZ4_compressHC2_withStateHC (void* state, byte* src, byte* dst, int srcSize, int cLevel) { return LZ4_compress_HC_extStateHC(state, src, dst, srcSize, LZ4_compressBound(srcSize), cLevel); }
int LZ4_compressHC2_limitedOutput_withStateHC (void* state, byte* src, byte* dst, int srcSize, int maxDstSize, int cLevel) { return LZ4_compress_HC_extStateHC(state, src, dst, srcSize, maxDstSize, cLevel); }
int LZ4_compressHC_continue (LZ4_streamHC_t* ctx, byte* src, byte* dst, int srcSize) { return LZ4_compress_HC_continue (ctx, src, dst, srcSize, LZ4_compressBound(srcSize)); }
int LZ4_compressHC_limitedOutput_continue (LZ4_streamHC_t* ctx, byte* src, byte* dst, int srcSize, int maxDstSize) { return LZ4_compress_HC_continue (ctx, src, dst, srcSize, maxDstSize); }


/* Deprecated streaming functions */
int LZ4_sizeofStreamStateHC(void) { return LZ4_STREAMHCSIZE; }

int LZ4_resetStreamStateHC(void* state, byte* inputBuffer)
{
    LZ4HC_CCtx_internal *ctx = &((LZ4_streamHC_t*)state)->internal_donotuse;
    if ((((size_t)state) & (sizeof(void*)-1)) != 0) return 1;   /* Error : pointer is not aligned for pointer (32 or 64 bits) */
    LZ4HC_init(ctx, (byte*)inputBuffer);
    ctx->inputBuffer = (byte*)inputBuffer;
    return 0;
}

void* LZ4_createHC (byte* inputBuffer)
{
    LZ4_streamHC_t* hc4 = (LZ4_streamHC_t*)ALLOCATOR(1, sizeof(LZ4_streamHC_t));
    if (hc4 == NULL) return NULL;   /* not enough memory */
    LZ4HC_init (&hc4->internal_donotuse, (byte*)inputBuffer);
    hc4->internal_donotuse.inputBuffer = (byte*)inputBuffer;
    return hc4;
}

int LZ4_freeHC (void* LZ4HC_Data) {
    if (!LZ4HC_Data) return 0;  /* support free on NULL */
    FREEMEM(LZ4HC_Data);
    return 0;
}

int LZ4_compressHC2_continue (void* LZ4HC_Data, byte* src, byte* dst, int srcSize, int cLevel)
{
    return LZ4HC_compress_generic (&((LZ4_streamHC_t*)LZ4HC_Data)->internal_donotuse, src, dst, &srcSize, 0, cLevel, noLimit);
}

int LZ4_compressHC2_limitedOutput_continue (void* LZ4HC_Data, byte* src, byte* dst, int srcSize, int dstCapacity, int cLevel)
{
    return LZ4HC_compress_generic (&((LZ4_streamHC_t*)LZ4HC_Data)->internal_donotuse, src, dst, &srcSize, dstCapacity, cLevel, limitedOutput);
}

byte* LZ4_slideInputBufferHC(void* LZ4HC_Data)
{
    LZ4HC_CCtx_internal* hc4 = &((LZ4_streamHC_t*)LZ4HC_Data)->internal_donotuse;
    int dictSize = LZ4_saveDictHC((LZ4_streamHC_t*)LZ4HC_Data, (byte*)(hc4->inputBuffer), 64 KB);
    return (byte*)(hc4->inputBuffer + dictSize);
}
