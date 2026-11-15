/*
 * Copyright (c) 2018-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "pas_config.h"

#if LIBPAS_ENABLED

#include "pas_megapage_cache.h"

#include "pas_bootstrap_free_heap.h"
#include "pas_large_free_heap_config.h"
#include "pas_mte.h"
#include "pas_page_malloc.h"
#include "pas_payload_reservation_page_list.h"
#include "pas_zero_memory.h"
#include <stdio.h>

/* Glass: on Windows a megapage is only reserved, and each page is committed when the cache hands it out. A megapage
   (16 MB) arrives committed from its provider, and Windows charges committed memory to the process whether it is
   touched or not, so every process paid for whole megapages of which it used a few pages (about 100 MB per tab
   process). Megapage caches only ever hold ordinary read-write heap memory (the bootstrap heap's, or a reserved
   primitive heap's), so the pages come back read-write. */
#if PAS_OS(WINDOWS)
#include <windows.h>
#define PAS_MEGAPAGE_CACHE_COMMITS_LAZILY 1
#else
#define PAS_MEGAPAGE_CACHE_COMMITS_LAZILY 0
#endif

typedef struct {
    pas_megapage_cache* cache;
    pas_megapage_cache_config* cache_config;
    pas_heap* heap;
    pas_physical_memory_transaction* transaction;
} megapage_cache_allocate_aligned_data;

static pas_aligned_allocation_result megapage_cache_allocate_aligned(size_t size,
                                                                     pas_alignment alignment,
                                                                     void* arg)
{
    megapage_cache_allocate_aligned_data* data;
    pas_megapage_cache* cache;
    pas_megapage_cache_config* cache_config;
    pas_aligned_allocation_result result;
    size_t new_size;
    size_t new_alignment_size;
    pas_alignment new_alignment;
    uintptr_t begin;
    uintptr_t end;
    size_t offset_from_allocation_to_boundary;
    pas_allocation_result bootstrap_result;
    pas_zero_mode zero_mode;
    char* base_before_exclusion;
    char* base_after_exclusion;
    size_t size_after_exclusion;
    
    data = arg;
    cache = data->cache;
    cache_config = data->cache_config;
    
    PAS_ASSERT(!(cache_config->excluded_size % cache_config->allocation_size));

    PAS_ASSERT(pas_is_power_of_2(cache_config->allocation_size));
    pas_alignment_validate(cache_config->allocation_alignment);

    PAS_ASSERT(size == cache_config->allocation_size);
    PAS_ASSERT(pas_alignment_is_equal(alignment, cache_config->allocation_alignment));
    
    if (cache_config->allocation_alignment.alignment_begin) {
        offset_from_allocation_to_boundary =
            cache_config->allocation_alignment.alignment -
            cache_config->allocation_alignment.alignment_begin;
    } else
        offset_from_allocation_to_boundary = 0;
    
    new_size = PAS_MAX(cache_config->megapage_size, cache_config->allocation_size);
    new_alignment_size = PAS_MAX(new_size, alignment.alignment);
    new_alignment =
        offset_from_allocation_to_boundary
        ? pas_alignment_create(new_alignment_size,
                               new_alignment_size - offset_from_allocation_to_boundary)
        : pas_alignment_create_traditional(new_alignment_size);
    
    pas_zero_memory(&result, sizeof(result));
    
    bootstrap_result = cache->provider(
        new_size, new_alignment,
        "pas_megapage_cache/chunk",
        data->heap,
        data->transaction,
        cache->provider_arg);
    if (!bootstrap_result.did_succeed)
        return result;

    pas_payload_reservation_page_list_append(pas_range_create(bootstrap_result.begin,
                                                              bootstrap_result.begin + new_size));

    base_before_exclusion = (char*)bootstrap_result.begin;
    PAS_ASSERT(base_before_exclusion);

    zero_mode = bootstrap_result.zero_mode;
#if PAS_MEGAPAGE_CACHE_COMMITS_LAZILY
    /* Decommitted pages read as zero once they are committed again. */
    pas_page_malloc_decommit_without_mprotect(
        base_before_exclusion, new_size, /* is_symmetric */ true, pas_page_flags_none);
    zero_mode = pas_zero_mode_is_all_zero;
#endif
    if (cache_config->should_zero) {
        switch (zero_mode) {
        case pas_zero_mode_may_have_non_zero:
            pas_zero_memory(base_before_exclusion, new_size);
            zero_mode = pas_zero_mode_is_all_zero;
            break;
        case pas_zero_mode_is_all_zero:
            break;
        }
    }
    
    begin = (uintptr_t)base_before_exclusion;
    PAS_PROFILE(MEGAPAGE_SET, begin);
    PAS_MTE_CLEAR(begin);

    end = begin + new_size;

    PAS_ASSERT(pas_alignment_is_ptr_aligned(cache_config->allocation_alignment, begin));
    PAS_ASSERT(pas_alignment_is_ptr_aligned(cache_config->allocation_alignment, end));

    PAS_ASSERT(pas_alignment_is_ptr_aligned(new_alignment, begin));
    PAS_ASSERT(pas_alignment_is_ptr_aligned(new_alignment, end));

    PAS_ASSERT(end > begin);

    begin += offset_from_allocation_to_boundary;
    end += offset_from_allocation_to_boundary;

    PAS_ASSERT(pas_is_aligned(begin, cache_config->megapage_size));
    PAS_ASSERT(pas_is_aligned(end, cache_config->megapage_size));
    PAS_ASSERT(pas_is_aligned(begin, new_alignment_size));
    PAS_ASSERT(pas_is_aligned(end, new_alignment_size));
    PAS_ASSERT(end > begin);

    if (cache_config->table_set_by_index) {
        size_t index_begin;
        size_t index_end;
        size_t index;
        
        index_begin = begin / cache_config->megapage_size;
        index_end = end / cache_config->megapage_size;
        for (index = index_end; index-- > index_begin;)
            cache_config->table_set_by_index(index, cache_config->table_set_by_index_arg);
    } else
        PAS_ASSERT(!cache_config->table_set_by_index_arg);

    base_after_exclusion = base_before_exclusion + cache_config->excluded_size;
    size_after_exclusion = new_size - cache_config->excluded_size;

    PAS_ASSERT(size <= size_after_exclusion);

    result.result = base_after_exclusion;
    result.result_size = size;
    result.left_padding = base_after_exclusion;
    result.left_padding_size = 0;
    result.right_padding = base_after_exclusion + size;
    result.right_padding_size = size_after_exclusion - size;
    result.zero_mode = zero_mode;
    
    return result;
}

void pas_megapage_cache_construct(pas_megapage_cache* cache,
                                  pas_heap_page_provider provider,
                                  pas_megapage_cache_size cache_size)
{
    pas_simple_large_free_heap_construct(&cache->free_heap);
    cache->provider = provider;
    cache->provider_arg = (void*)(uintptr_t)cache_size;
    cache->committed_begin = 0;
    cache->committed_end = 0;
}

#if PAS_MEGAPAGE_CACHE_COMMITS_LAZILY
/* Commits the page being handed out. Pages leave a megapage in address order, so this commits a window of
   PAS_MEGAPAGE_CACHE_COMMIT_WINDOW around the page, within its megapage, and the following pages in that window need
   no system call: one call per 16 KB page made allocation-heavy scripts measurably slower. */
#define PAS_MEGAPAGE_CACHE_COMMIT_WINDOW ((size_t)256 * 1024)

static void commit_page(pas_megapage_cache* cache, pas_megapage_cache_config* cache_config, uintptr_t page)
{
    uintptr_t begin;
    uintptr_t end;

    begin = page;
    end = page + cache_config->allocation_size;

    if (begin >= cache->committed_begin && end <= cache->committed_end)
        return;

    if (!cache_config->allocation_alignment.alignment_begin
        && cache_config->allocation_size < PAS_MEGAPAGE_CACHE_COMMIT_WINDOW
        && cache_config->megapage_size >= PAS_MEGAPAGE_CACHE_COMMIT_WINDOW) {
        uintptr_t megapage_begin;
        megapage_begin = pas_round_down_to_power_of_2(page, cache_config->megapage_size);
        if (end <= megapage_begin + cache_config->megapage_size) {
            begin = pas_round_down_to_power_of_2(begin, PAS_MEGAPAGE_CACHE_COMMIT_WINDOW);
            end = pas_round_up_to_power_of_2(end, PAS_MEGAPAGE_CACHE_COMMIT_WINDOW);
        }
    }

    /* One call does it unless the range spans two reservations, which pas_page_malloc knows how to walk. */
    if (!VirtualAlloc((void*)begin, end - begin, MEM_COMMIT, PAGE_READWRITE)) {
        pas_page_malloc_commit_without_mprotect(
            (void*)begin, end - begin, /* is_symmetric */ true, pas_page_flags_none);
    }

    cache->committed_begin = begin;
    cache->committed_end = end;
}
#endif

void* pas_megapage_cache_try_allocate(pas_megapage_cache* cache,
                                      pas_megapage_cache_config* cache_config,
                                      pas_heap* heap,
                                      pas_physical_memory_transaction* transaction)
{
    megapage_cache_allocate_aligned_data data;
    pas_large_free_heap_config config;
    pas_allocation_result result;

    PAS_ASSERT(!(cache_config->excluded_size % cache_config->allocation_size));

    data.cache = cache;
    data.cache_config = cache_config;
    data.heap = heap;
    data.transaction = transaction;
    
    pas_zero_memory(&config, sizeof(config));
    config.type_size = 1;
    config.min_alignment = 1;
    config.aligned_allocator = megapage_cache_allocate_aligned;
    config.aligned_allocator_arg = &data;
    config.deallocator = NULL;
    config.deallocator_arg = NULL;
    
    result = pas_simple_large_free_heap_try_allocate(
        &cache->free_heap,
        cache_config->allocation_size,
        cache_config->allocation_alignment,
        &config);
    
    if (!result.did_succeed)
        return NULL;
    
    PAS_ASSERT(result.begin);
#if PAS_MEGAPAGE_CACHE_COMMITS_LAZILY
    commit_page(cache, cache_config, result.begin);
#endif
    if (cache_config->should_zero)
        PAS_ASSERT(result.zero_mode == pas_zero_mode_is_all_zero);
    
    return (void*)result.begin;
}

#endif /* LIBPAS_ENABLED */
