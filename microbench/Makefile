FLAGS =
LDFLAGS = -latomic
GPP = g++

### if libpapi is installed but you do not want to use it, invoke make with extra argument "has_libpapi=0"
has_libpapi=0
ifneq ($(has_libpapi), 0)
	FLAGS += -DUSE_PAPI
	LDFLAGS += -lpapi
endif

### if libnuma is installed but you do not want to use it, invoke make with extra argument "has_libnuma=0"
has_libnuma=0
ifneq ($(has_libnuma), 0)
	FLAGS += -DUSE_LIBNUMA
	LDFLAGS += -lnuma
endif

use_asan=0
ifneq ($(use_asan), 0)
	LDFLAGS += -fsanitize=address -static-libasan
endif

use_asserts=0
ifeq ($(use_asserts), 0)
	FLAGS += -DNDEBUG
endif

## The harness owns several fixed-size per-thread arrays. Set this to the
## largest thread count that a binary will ever execute with.
max_threads=128

use_fopenmp=1
ifeq ($(use_fopenmp), 1)
	FLAGS += -fopenmp
endif

use_timelines=0
ifeq ($(use_timelines), 1)
	FLAGS += -DMEASURE_TIMELINE_STATS
endif

no_optimize=0
ifeq ($(no_optimize), 1)
	FLAGS += -O0 -fno-inline-functions -fno-inline
else
	FLAGS += -O3
endif

debug_timeline_record_every_deamortized_free=0
ifeq ($(debug_timeline_record_every_deamortized_free), 1)
	FLAGS += -DTIMELINE_RECORD_EVERY_DEAMORTIZED_FREE
endif

## setting this to 0 enables experimental deamortized freeing in the DEBRA memory reclamation algorithm.
## this can improve performance substantially, especially when using jemalloc or tcmalloc,
## but it should only be used for data structures and workloads in which, on average,
## at most one object per data structure operation is allocated.
##
debra_orig_free=1
ifeq ($(debra_orig_free), 1)
	FLAGS += -DDEBRA_ORIGINAL_FREE
endif

debug_timeline_record_every_deamortized_free=0
ifeq ($(debug_timeline_record_every_deamortized_free), 1)
	FLAGS += -DTIMELINE_RECORD_EVERY_DEAMORTIZED_FREE
endif

## setting this to 0 enables experimental deamortized freeing in the DEBRA memory reclamation algorithm.
## this can improve performance substantially, especially when using jemalloc or tcmalloc,
## but it should only be used for data structures and workloads in which, on average,
## at most one object per data structure operation is allocated.
##
debra_orig_free=1
ifeq ($(debra_orig_free), 1)
	FLAGS += -DDEBRA_ORIGINAL_FREE
endif

skip_validation=0
ifeq ($(skip_validation), 1)
	FLAGS += -DSKIP_VALIDATION
endif

measure_versions=0
ifeq ($(measure_versions), 1)
	FLAGS += -DMEASURE_VERSIONS
endif

measure_prev=0
ifeq ($(measure_prev), 1)
	FLAGS += -DMEASURE_PREV_TRAVERSAL
endif

# FLAGS += -march=native
FLAGS += -DMAX_THREADS_POW2=$(max_threads)
FLAGS += -DCPU_FREQ_GHZ=2.1 #$(shell ./experiments/get_cpu_ghz.sh)
FLAGS += -DMEMORY_STATS=if\(1\) -DMEMORY_STATS2=if\(0\)
FLAGS += -std=c++17 -g -mcx16 -gdwarf
FLAGS += -fno-omit-frame-pointer
#FLAGS += -DCALL_DEBUG_GC
#FLAGS += -DNO_CLEANUP_AFTER_WORKLOAD ### avoid executing data structure destructors, to save teardown time at the end of each trial (useful with massive trees)
#FLAGS += -DRAPID_RECLAMATION
#FLAGS += -DMEASURE_REBUILDING_TIME
#FLAGS += -DMEASURE_TIMELINE_STATS
# FLAGS += -DUSE_TREE_STATS
#FLAGS += -DOVERRIDE_PRINT_STATS_ON_ERROR
#FLAGS += -Wno-format
FLAGS += -DMEASURE_VERSIONS
FLAGS += -DMEASURE_PREV_TRAVERSAL
FLAGS += $(xargs)

LDFLAGS += -L../lib
LDFLAGS += -I./ -I../ `find ../common -type d | sed s/^/-I/`
LDFLAGS += -lpthread
LDFLAGS += -ldl
LDFLAGS += -mrtm

.PHONY: default
default: ds-reclaim
	@echo
	@echo "===================================================================="
	@echo "== Compilation target was:"
	@echo "===================================================================="
	@echo "== 'ds-reclaim' which produces binaries for all combinations of"
	@echo "==              the make variables DATA_STRUCTURES, RECLAIMERS"
	@echo "=="
	@echo "== for example, you can specify multiple RECLAIMERS by running, eg."
	@echo "==              make RECLAIMERS='debra none' -j"
	@echo "===================================================================="
	@echo "== Other target options:"
	@echo "===================================================================="
	@echo "== 'ds-reclaim-pool' which produces binaries for combinations of"
	@echo "==                   make vars DATA_STRUCTURES, RECLAIMERS, POOLS"
	@echo "=="
	@echo "== 'all' which produces binaries for combinations of"
	@echo "== 	    make vars DATA_STRUCTURES, RECLAIMERS, POOLS, ALLOCATORS"
	@echo "=="
	@echo "==     NOTE: all target is !!DEPRECATED!!"
	@echo "==           use ds-reclaim going forward."
	@echo "===================================================================="
	@echo

bin_dir=bin
dir_guard:
	@mkdir -p $(bin_dir)


################################################################################
#### BEGIN check for proper git cloning WITH SUBMODULES
################################################################################
define submodules_err_msg=

================================================================================
==== ERROR: CANNOT FIND SUBMODULES                                          ====
================================================================================
==== You most likely used the wrong git clone command...                    ====
====                                                                        ====
==== Note that a special argument is needed for cloning:                    ====
==== git clone https://gitlab.com/trbot86/setbench.git --recurse-submodules ====
================================================================================
endef
dir_exists_tools=$(shell if [ -d "../tools" ]; then echo 1 ; else echo 0 ; fi )
dir_exists_recordmgr=$(shell if [ -d "../common/recordmgr" ]; then echo 1 ; else echo 0 ; fi )
num_files_tools=$(shell ls ../tools | wc -l 2>/dev/null )
num_files_recordmgr=$(shell ls ../common/recordmgr | wc -l 2>/dev/null )
ifeq ($(dir_exists_tools),0)
    $(error $(submodules_err_msg))
endif
ifeq ($(dir_exists_recordmgr),0)
    $(error $(submodules_err_msg))
endif
ifeq ($(num_files_tools),0)
    $(error $(submodules_err_msg))
endif
ifeq ($(num_files_recordmgr),0)
    $(error $(submodules_err_msg))
endif
################################################################################
#### END check for proper git cloning WITH SUBMODULES
################################################################################



################################################################################
#### DECIDING WHICH COMBINATIONS OF: DATA STRUCTURE, RECLAIMER, POOL, ALLOCATOR
#### SHOULD BE COMPILED
################################################################################
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_O*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_No_Alloc*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_No_Aug*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_No_CAS*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Lock_Free_Tree*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Lock_Free_Tree_Augmented*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Lock_Free_Tree_NoAlloc*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Lock_Free_Tree_NoCAS*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_Updated*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_DCAS*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_Baseline*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_FatNode*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_v3*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_v4*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_FatNode_PadFull*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_FatNode_ChildVC*/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_FatNode_NoChain*/adapter.h))
DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_FatNode_ChildPtr/adapter.h))
DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_FatNode_NoVC/adapter.h))
DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_FatNode_NoVC_Pad/adapter.h))
DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_FatNode_NoVC_Optimised/adapter.h))
DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Lock_Free_Tree_Augmented_FatNodePtrs/adapter.h))
DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Lock_Free_Tree_Augmented_FatNodePtrs_Pad/adapter.h))
DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_Baseline/adapter.h))
DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_FatNode_ChildVC/adapter.h))
DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Lock_Free_Tree_Augmented/adapter.h))
DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Lock_Free_Tree_NoAlloc/adapter.h))
# DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/Trie_valid_Read*/adapter.h))




RECLAIMERS=debra
POOLS=none
ALLOCATORS=new ## allocators are DEPRECATED (using LD_PRELOAD to load allocators instead!)

## statements like the below can be used to compile subsets of the data structures...
#DATA_STRUCTURES=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/brown_ext_ist*/adapter.h))
#DATA_STRUCTURES+=$(patsubst ../ds/%/adapter.h,%,$(wildcard ../ds/bronson*/adapter.h))

## preview the set of DATA STRUCTURES
$(info $(DATA_STRUCTURES))

################################################################################
#### CONSTRUCTING CUSTOM BUILD TARGETS FOR EACH COMBINATION
#### and adding them to combined targets:
####    ds-reclaim
####	ds-reclaim-pool
####	DEPRECATED-ds-alloc-reclaim-pool
################################################################################

## create build targets for combinations of DATA_STRUCTURES and RECLAIMERS
## and add them to a combined target: "ds-reclaim"
define create-target-ds-reclaim =
$(1).$(2): dir_guard
	$(GPP) ./main.cpp -o $(bin_dir)/$(1).$(2) -I../ds/$(1) -DDS_TYPENAME=$(1) -DRECLAIM_TYPE=$(2) $(FLAGS) $(LDFLAGS)
ds-reclaim: $(1).$(2)
endef
define create-target-openbwtree-ds-reclaim =
$(1).$(2): dir_guard
	$(GPP) ./main.cpp ../ds/$(1)/bwtree.cpp -o $(bin_dir)/$(1).$(2) -I../ds/$(1) -DDS_TYPENAME=$(1) -DRECLAIM_TYPE=$(2) $(FLAGS) -Wno-invalid-offsetof $(LDFLAGS) -latomic
ds-reclaim: $(1).$(2)
endef
$(foreach ds,$(DATA_STRUCTURES), \
	$(foreach reclaim,$(RECLAIMERS), \
		$(if $(filter wang_openbwtree,$(ds)), \
			$(eval $(call create-target-openbwtree-ds-reclaim,$(ds),$(reclaim))), \
			$(eval $(call create-target-ds-reclaim,$(ds),$(reclaim))) \
		) \
	) \
)


## create build targets for combinations of DATA_STRUCTURES, RECLAIMERS and POOLS
## and add them to a combined target: "ds-reclaim-pool"
define create-target-ds-reclaim-pool =
_$(1).$(2).p$(3): dir_guard
	$(GPP) ./main.cpp -o $(bin_dir)/$(1).$(2).p$(3) -I../ds/$(1) -DDS_TYPENAME=$(1) -DRECLAIM_TYPE=$(2) -DPOOL_TYPE=$(3) $(FLAGS) $(LDFLAGS)
ds-reclaim-pool: _$(1).$(2).p$(3)
endef
define create-target-openbwtree-ds-reclaim-pool =
_$(1).$(2).p$(3): dir_guard
	$(GPP) ./main.cpp ../ds/$(1)/bwtree.cpp -o $(bin_dir)/$(1).$(2).p$(3) -I../ds/$(1) -DDS_TYPENAME=$(1) -DRECLAIM_TYPE=$(2) -DPOOL_TYPE=$(3) $(FLAGS) -Wno-invalid-offsetof $(LDFLAGS) -latomic
ds-reclaim-pool: _$(1).$(2).p$(3)
endef
$(foreach ds,$(DATA_STRUCTURES), \
	$(foreach reclaim,$(RECLAIMERS), \
		$(foreach pool,$(POOLS), \
			$(if $(filter wang_openbwtree,$(ds)), \
				$(eval $(call create-target-openbwtree-ds-reclaim-pool,$(ds),$(reclaim),$(pool))), \
				$(eval $(call create-target-ds-reclaim-pool,$(ds),$(reclaim),$(pool))) \
			) \
		) \
	) \
)

## THE FOLLOWING IS DEPRECATED -- USE LD_PRELOAD TO LOAD ALLOCATORS INSTEAD!
## create build targets for combinations of DATA_STRUCTURES, RECLAIMERS, POOLS and ALLOCATORS
## and add them to a combined target: "DEPRECATED-ds-alloc-reclaim-pool"
define create-target-ds-alloc-reclaim-pool =
_$(1).$(2).$(3).p$(4): dir_guard
	$(GPP) ./main.cpp -o $(bin_dir)/$(1).$(2).$(3).p$(4) -I../ds/$(1) -DDS_TYPENAME=$(1) -DALLOC_TYPE=$(2) -DRECLAIM_TYPE=$(3) -DPOOL_TYPE=$(4) $(FLAGS) $(LDFLAGS)
all: _$(1).$(2).$(3).p$(4)
endef
define create-target-openbwtree-ds-alloc-reclaim-pool =
_$(1).$(2).$(3).p$(4): dir_guard
	$(GPP) ./main.cpp ../ds/$(1)/bwtree.cpp -o $(bin_dir)/$(1).$(2).$(3).p$(4) -I../ds/$(1) -DDS_TYPENAME=$(1) -DALLOC_TYPE=$(2) -DRECLAIM_TYPE=$(3) -DPOOL_TYPE=$(4) $(FLAGS) -Wno-invalid-offsetof $(LDFLAGS) -latomic
all: _$(1).$(2).$(3).p$(4)
endef

$(foreach ds,$(DATA_STRUCTURES), \
	$(foreach alloc,$(ALLOCATORS), \
		$(foreach reclaim,$(RECLAIMERS), \
			$(foreach pool,$(POOLS), \
				$(if $(filter wang_openbwtree,$(ds)), \
					$(eval $(call create-target-openbwtree-ds-alloc-reclaim-pool,$(ds),$(alloc),$(reclaim),$(pool))), \
					$(eval $(call create-target-ds-alloc-reclaim-pool,$(ds),$(alloc),$(reclaim),$(pool))) \
				) \
			) \
		) \
	) \
)

clean:
	rm $(bin_dir)/*.out
