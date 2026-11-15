/*
 * Copyright (c) 2018-2026 Apple Inc. All rights reserved.
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

#include "pas_bootstrap_free_heap.h"

#include "pas_config.h"
#include "pas_heap_lock.h"
#include "pas_large_free_heap_config.h"
#include "pas_enumerable_page_malloc.h"
#include "pas_page_malloc.h"
#include "pas_simple_free_heap_helpers.h"

#if PAS_BOOTSTRAP_FREE_HEAP_COMMITS_LAZILY
/* Glass: an aligned mapping (a 16 MB megapage aligned to 16 MB maps 32 MB) leaves padding that goes on the free list
   for later allocations. On Windows pas_page_malloc commits all of it, and Windows charges committed memory to the
   process whether it is touched or not, so that padding is decommitted here (it stays reserved) and
   pas_simple_large_free_heap commits whatever is later allocated from this heap. */
static void decommit_padding(void* padding, size_t size)
{
    uintptr_t begin;
    uintptr_t end;

    begin = pas_round_up_to_power_of_2((uintptr_t)padding, pas_page_malloc_alignment());
    end = pas_round_down_to_power_of_2((uintptr_t)padding + size, pas_page_malloc_alignment());
    if (end <= begin)
        return;

    pas_page_malloc_decommit_without_mprotect(
        (void*)begin, end - begin, /* is_symmetric */ true, pas_memory_flavor_plain);
}
#endif

static pas_aligned_allocation_result bootstrap_source_allocate_aligned(size_t size,
                                                                       pas_alignment alignment,
                                                                       void* arg)
{
    static const bool verbose = PAS_SHOULD_LOG(PAS_LOG_BOOTSTRAP_HEAPS);
    pas_memory_flavor flavor = (pas_memory_flavor)(uintptr_t)arg;

    if (verbose)
        pas_log("%s bootstrap heap allocating %zu\n", pas_memory_flavor_get_string(flavor), size);

    pas_aligned_allocation_result retval = pas_enumerable_page_malloc_try_allocate_without_deallocating_padding(size, alignment, flavor);

#if PAS_BOOTSTRAP_FREE_HEAP_COMMITS_LAZILY
    /* Only the plain heap: pas_simple_large_free_heap commits what is allocated from pas_bootstrap_free_heap. */
    if (retval.result && flavor == pas_memory_flavor_plain) {
        decommit_padding(retval.left_padding, retval.left_padding_size);
        decommit_padding(retval.right_padding, retval.right_padding_size);
    }
#endif

    if (verbose)
        pas_log("%s bootstrap heap done allocating, returning %p.\n", pas_memory_flavor_get_string(flavor), retval.result);

    return retval;
}

static void initialize_config(pas_large_free_heap_config* config, pas_memory_flavor flavor)
{
    config->type_size = 1;
    config->min_alignment = 1;
    config->aligned_allocator = bootstrap_source_allocate_aligned;
    config->aligned_allocator_arg = (void*)(uintptr_t)flavor;
    config->deallocator = NULL;
    config->deallocator_arg = NULL;
}

static void pas_bootstrap_free_heap_initialize_config(pas_large_free_heap_config* config)
{
    initialize_config(config, pas_memory_flavor_plain);
}

#define PAS_SIMPLE_FREE_HEAP_NAME pas_bootstrap_free_heap
#define PAS_SIMPLE_FREE_HEAP_ID(suffix) pas_bootstrap_free_heap ## suffix
#include "pas_simple_free_heap_definitions.def"
#undef PAS_SIMPLE_FREE_HEAP_NAME
#undef PAS_SIMPLE_FREE_HEAP_ID

static void pas_mte_bootstrap_free_heap_initialize_config(pas_large_free_heap_config* config)
{
    initialize_config(config, pas_memory_flavor_mte);
}

#define PAS_SIMPLE_FREE_HEAP_NAME pas_mte_bootstrap_free_heap
#define PAS_SIMPLE_FREE_HEAP_ID(suffix) pas_mte_bootstrap_free_heap ## suffix
#include "pas_simple_free_heap_definitions.def"
#undef PAS_SIMPLE_FREE_HEAP_NAME
#undef PAS_SIMPLE_FREE_HEAP_ID

#endif /* LIBPAS_ENABLED */
