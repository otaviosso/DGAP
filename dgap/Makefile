# See LICENSE.txt for license details.

CXX_FLAGS += -std=c++11 -g -rdynamic -O3 -Wall -g
LDLIBS = -lpmemobj -lpmem -lralloc -lpthread
PAR_FLAG = -fopenmp
RALLOC_PATH = ../ralloc

ifneq (,$(findstring icpc,$(CXX)))
	PAR_FLAG = -openmp
endif

ifneq (,$(findstring sunCC,$(CXX)))
	CXX_FLAGS = -std=c++11 -xO3 -m64 -xtarget=native
	PAR_FLAG = -xopenmp
endif

ifneq ($(SERIAL), 1)
	CXX_FLAGS += $(PAR_FLAG)
endif

KERNELS = bc bfs cc_sv pr
SUITE = $(KERNELS)

.PHONY: all
all: $(SUITE)

% : src/%.cc src/*.h
	(cd $(RALLOC_PATH)/test && make libralloc.a)
	$(CXX) -L $(RALLOC_PATH)/test -I$(RALLOC_PATH)/src  $< -o $@ $(CXX_FLAGS) $(LDLIBS)

.PHONY: clean
clean:
	rm -f $(SUITE) test/out/*
