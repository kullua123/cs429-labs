#include "simulate.h"
#include "debug.h"
#include "utils.h"
#include "cds.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int address;
    int length;
    enum MemoryAccessType type;
} MemoryReference;

static int traceLineNumber;

void readReference(FILE* traceFile, MemoryReference* reference) {
    int c;
    int address;
    int n;
    c = skipBlanks(traceFile);
    address = 0;
    while (isHex(c)) {
        address = (address << 4) | hexValue(c);
        c = getc(traceFile);
    }
    if (c != ',') {
        fprintf(stderr, "bad trace file input at line %d: %c\n", traceLineNumber, c);
        exit(-1);
    }
    n = 0;
    c = getc(traceFile);
    while (isdigit(c)) {
        n = n * 10 + decValue(c);
        c = getc(traceFile);
    }
    /* skip to end of line */
    while ((c != '\n') && (c != EOF)) {
        c = getc(traceFile);
    }
    /* define reference fields */
    reference->address = address;
    reference->length = n;
}

int readTraceFile(FILE* traceFile, MemoryReference *reference) {
    int c;
    traceLineNumber = 0;
    while ((c = getc(traceFile)) != EOF) {
        ++traceLineNumber;
        while (isspace(c) && (c != EOF)) {
            c = getc(traceFile);
        }
        switch (c) {
            case 'I':
                reference->type = FETCH;
                readReference(traceFile, reference);
                return 'I';
            case 'M':
            case 'S':
                reference->type = STORE;
                readReference(traceFile, reference);
                return 'S';
            case 'L':
                reference->type = LOAD;
                readReference(traceFile, reference);
                return 'L';
        }
        // Not a reference line, skip
        while ((c != '\n') && (c != EOF)) {
            c = getc(traceFile);
        }
    }
    return EOF;
}

void checkForDecay(int time, Cache* cache) {
    if (cache->replacementPolicy != LFU) {
        return;
    }
    if (!(time % cache->lfuDecayInterval)) {
        int i;
        if (debug) {
            fprintf(debugFile, "%s: LFU decay for all LFU counters\n", cache->name);
        }
        for (i = 0; i < cache->entries; ++i) {
            cache->cacheLine[i].replacementData >>= 1;
        }
    }
}

int getBaseCacheAddress(Cache* cache, int address) {
    return address & (~mask(logOfTwo(cache->cacheLineSize)));
}

int computeSetIndex(Cache* cache, int address) {
    return (address >> logOfTwo(cache->cacheLineSize)) & mask(logOfTwo(cache->entries / cache->numberOfWays)) * cache->numberOfWays;
}

int searchCacheFor(Cache* cache, int address) {
    int cacheEntryIndex;
    int i;
    address = getBaseCacheAddress(cache, address);
    cacheEntryIndex = computeSetIndex(cache, address);
    if (debug) {
        fprintf(debugFile, "%s: search cache lines %d to %d for 0x%08X\n", cache->name, cacheEntryIndex, cacheEntryIndex + cache->numberOfWays - 1, address);
    }
    for (i = 0; i < cache->numberOfWays; ++i) {
        if (cache->cacheLine[cacheEntryIndex + i].valid && (address == cache->cacheLine[cacheEntryIndex+i].tag)) {
            return cacheEntryIndex + i;
        }
    }
    return -1;
}

int searchVictimCacheFor(Cache* cache, int address) {
    int i;
    address = getBaseCacheAddress(cache, address);
    for (i = 0; i < cache->victimCache.entries; ++i) {
        if (cache->victimCache.cacheLine[i].valid && (address == cache->victimCache.cacheLine[i].tag)) {
            return i;
        }
    }
    return -1;
}

int findVictimInCache(Cache* cache, int address) {
    int i;
    int victim;
    int min;
    int firstIndex = computeSetIndex(cache, address);
    int setSize = cache->numberOfWays;
    if (debug) {
        fprintf(debugFile, "%s: look for victim in %d lines starting at %d\n", cache->name,  setSize, firstIndex);
    }
    for (i = 0; i < setSize; ++i) {
        if (!cache->cacheLine[firstIndex + i].valid) {
            victim = firstIndex + i;
            if (debug) {
                fprintf(debugFile, "%s: found empty cache entry at %d\n", cache->name,  victim);
            }
            return victim;
        }
    }
    // No empty cache
    victim = firstIndex;
    switch (cache->replacementPolicy) {
        case FIFO:
        case LRU:
        case LFU:
            min = cache->cacheLine[firstIndex].replacementData;
            if (debug) {
                fprintf(debugFile, "%s: replacement data: [%d, 0x%08X]: %d", cache->name, victim, cache->cacheLine[victim].tag, min);
            }
            for (i = 1; i < setSize; ++i) {
                if (debug) {
                    fprintf(debugFile, ", [%d, 0x%08X]: %d", firstIndex+i, cache->cacheLine[firstIndex+i].tag, cache->cacheLine[firstIndex+i].replacementData);
                }
                if (cache->cacheLine[firstIndex + i].replacementData < min) {
                    victim = firstIndex + i;
                    min = cache->cacheLine[firstIndex + i].replacementData;
                }
            }
            if (debug) {
                fprintf(debugFile, "\n");
            }
            break;
        case RANDOM:
            victim = firstIndex + (random() % setSize);
            break;
    }
    if (debug) {
        fprintf(debugFile, "%s: found victim in entry %d\n", cache->name,  victim);
    }
    return victim;
}

int findVictimInVictimCache(VictimCache* victimCache) {
    int i;
    int victim;
    int min;
    for (i = 0; i < victimCache->entries; ++i) {
        if (!(victimCache->cacheLine[i].valid)) {
            return i;
        }
    }
    // No empty cache
    victim = 0;
    min = victimCache->cacheLine[0].replacementData;
    for (i = 1; i < victimCache->entries; ++i) {
        if (victimCache->cacheLine[i].replacementData < min) {
            victim = i;
            min = victimCache->cacheLine[i].replacementData;
        }
    }
    return victim;
}

void setReplacementData(int time, Cache* cache, CacheLine* cacheEntry) {
    switch (cache->replacementPolicy) {
        case FIFO:
            cacheEntry->replacementData = time;
            break;
        case LRU:
            cacheEntry->replacementData = time;
            break;
        case LFU:
            cacheEntry->replacementData = 0;
        case RANDOM:
            break;
    }
}

void updateReplacementData(int time, Cache* cache, CacheLine* cacheEntry) {
    switch (cache->replacementPolicy) {
        case FIFO:
            break;
        case LRU:
            cacheEntry->replacementData = time;
            break;
        case LFU:
            cacheEntry->replacementData += 1;
        case RANDOM:
            break;
    }
}

char cacheFull(Cache* cache) {
    int i;
    for (i = 0; i < cache->entries; ++i) {
        if (!cache->cacheLine[i].valid) {
            return 0;
        }
    }
    return 1;
}

void swapCacheLines(CacheLine* a, CacheLine* b) {
    CacheLine tmp;
    memcpy(&tmp, a, sizeof(CacheLine));
    memcpy(a, b, sizeof(CacheLine));
    memcpy(b, &tmp, sizeof(CacheLine));
}

void simulateReferenceToCacheLine(CacheDescription* cacheDescription, MemoryReference* reference) {
    int cacheEntryIndex;
    int victim;
    CacheLine* cacheEntry = 0;
    Cache* cache = cacheDescription->cache;
    if (debug) {
        fprintf(debugFile, "%s: %s Reference 0x%08X of length %d\n", cacheDescription->name, memoryAccessTypeName(reference->type), reference->address, reference->length);
    }
    ++cache->totalCacheAccess;
    cacheEntryIndex = searchCacheFor(cache, reference->address); // Find in cache
    if (0 <= cacheEntryIndex) { // Found
        ++cache->totalCacheHits;
        if (debug) {
            fprintf(debugFile, "%s: Found address 0x%08X in cache line %d\n", cacheDescription->name, reference->address, cacheEntryIndex);
        }
        cacheEntry = &cache->cacheLine[cacheEntryIndex];
        updateReplacementData(cacheDescription->numberOfMemoryReference, cache, cacheEntry);
    } else { // Not found
        // TODO consider write-thru
        ++cache->totalCacheMisses;
        if (!cacheFull(cache)) {
            victim = -1;
        } else {
            ++cache->victimCache.totalCacheAccess;
            victim = searchVictimCacheFor(cache, reference->address); // Go into victim cache
        }
        if (0 <= victim) { // Found in victim cache
            ++cache->victimCache.totalCacheHits;
            cacheEntryIndex = findVictimInCache(cache, reference->address);
            if (cache->cacheLine[cacheEntryIndex].valid && cache->cacheLine[cacheEntryIndex].dirty) {
                ++cache->totalMissWrites;
            }
            swapCacheLines(&cache->cacheLine[cacheEntryIndex], &cache->victimCache.cacheLine[victim]);
            cache->victimCache.cacheLine[victim].replacementData = cacheDescription->numberOfMemoryReference;
            cacheEntry = &cache->cacheLine[cacheEntryIndex];
        } else { // Not found
            ++cache->totalMissReads;
            ++cache->victimCache.totalCacheMisses;
            ++cache->victimCache.totalMissReads;
            cacheEntryIndex = findVictimInCache(cache, reference->address);
            if (debug) {
                fprintf(debugFile, "%s: Pick victim %d to replace\n", cacheDescription->name,  cacheEntryIndex);
            }
            victim = findVictimInVictimCache(&cache->victimCache);
            if (cache->victimCache.cacheLine[victim].valid && cache->victimCache.cacheLine[victim].dirty) {
                ++cache->victimCache.totalMissWrites;
            }
            swapCacheLines(&cache->cacheLine[cacheEntryIndex], &cache->victimCache.cacheLine[victim]);
            cache->victimCache.cacheLine[victim].replacementData = cacheDescription->numberOfMemoryReference;
            cacheEntry = &cache->cacheLine[cacheEntryIndex];
            cacheEntry->tag = getBaseCacheAddress(cache, reference->address);
            cacheEntry->valid = 1;
            cacheEntry->dirty = 0;
            if (debug) {
                fprintf(debugFile, "%s: Read cache line 0x%08X into entry %d\n", cacheDescription->name,  cacheEntry->tag, cacheEntryIndex);
            }
        }
        setReplacementData(cacheDescription->numberOfMemoryReference, cache, cacheEntry);
    }
    if (cache->replacementPolicy == LFU) {
        checkForDecay(cacheDescription->numberOfMemoryReference, cache);
    }
    if (reference->type == STORE) {
        cacheEntry->dirty = 1;
    }
//     if (reference->type == STORE) {
//         /* If it's not write-back, then it is write-thru.
//            For write-thru, if it's a write, we write to memory. */
//         if (!cacheDescription->cache->writeBack) {
//             cacheDescription->cache->totalMissWrites += 1;
//             if (debug) fprintf(debugFile, "%s: Write cache line 0x%08X thru to memory\n", cacheDescription->name,  cacheEntry->tag);
//         } else {
//             /* For write-back, if it's a write, it's dirty. */
//             cacheEntry->dirty = 1;
//         }
//     }
}

void simulateReferenceToMemory(CacheDescription* cacheDescription, MemoryReference* reference) {
    ++cacheDescription->numberOfMemoryReference;
    ++cacheDescription->numberOfType[reference->type];
    // Check if the entire reference fits into just one cache line
    if (getBaseCacheAddress(cacheDescription->cache, reference->address) == getBaseCacheAddress(cacheDescription->cache, reference->address + reference->length -1)) {
        simulateReferenceToCacheLine(cacheDescription, reference);
    } else {
        /* reference spans two cache lines.  Convert it to two
references: the first cache line, and the second cache line */
        MemoryReference reference1;
        MemoryReference reference2;
        /* easiest to compute the second part first */
        reference2.type = reference->type;
        reference2.address = getBaseCacheAddress(cacheDescription->cache, reference->address + reference->length -1);
        reference2.length = reference->address + reference->length - reference2.address;
        reference1.type = reference->type;
        reference1.address = reference->address;
        reference1.length = reference->length - reference2.length;

        /* but we do the references first, then second */
        simulateReferenceToCacheLine(cacheDescription, &reference1);
        simulateReferenceToCacheLine(cacheDescription, &reference2);
    }
}

void simulateCaches(const char* traceFileName) {
    FILE* traceFile;
    MemoryReference reference;
    traceFile = fopen(traceFileName, "r");
    if (!traceFile) {
        fprintf (stderr,"Cannot open trace file %s\n", traceFileName);
        exit(1);
    }
    while (readTraceFile(traceFile, &reference) != EOF) {
        CacheDescription* cacheDescription = cacheDescriptionRoot;
        while (cacheDescription) {
            simulateReferenceToMemory(cacheDescription, &reference);
            cacheDescription = cacheDescription->next;
        }
    }
    fclose(traceFile);
}

