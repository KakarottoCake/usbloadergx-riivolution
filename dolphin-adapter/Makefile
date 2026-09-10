# fstadapter: Dolphin-side harness for the REAL Riivo install path.
# Modeled on the devkitPPC wii application template. Adds explicit rules
# compiling the production TUs unmodified (RiivoFstInstall.cpp always;
# RiivoBoot.cpp for the v3+ production-caller harness, garbage-collected
# at link so only InstallPendingFst's closure is retained); nothing from
# the loader is copied or forked here. probe.S in source/ builds with the
# template's own SFILES rule.
.SUFFIXES:
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

TARGET		:=	fstadapter
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	shim

CFLAGS	= -g -O2 -Wall $(MACHDEP) $(INCLUDE)
CXXFLAGS	=	$(CFLAGS)

# Production allocator routing: every malloc/free/realloc in this binary
# (including libstdc++'s, which is what vector/string use) goes through
# memory/mem2.cpp's two-heap chain, exactly like the loader. That is the
# point of the v6 memwindow: same allocator, same MEM2_init(48), same
# big-allocations-to-MEM2 policy as hardware boot.
LDFLAGS	=	-g $(MACHDEP) -Wl,-Map,$(notdir $@).map -Wl,--gc-sections -Wl,-wrap,malloc,-wrap,free,-wrap,memalign,-wrap,calloc,-wrap,realloc,-wrap,malloc_usable_size

LIBS	:=	-logc -lm
LIBDIRS	:=

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(sFILES:.s=.o) $(SFILES:.S=.o)
export OFILES := $(OFILES_BIN) $(OFILES_SOURCES) RiivoFstInstall.o RiivoBoot.o RiivoFstBuild.o RiivoFst.o RiivoFstWalk.o RiivoFragPlan.o RiivoValidate.o mem2.o mem2alloc.o

export HFILES := $(addsuffix .h,$(subst .,_,$(BINFILES)))

# GXSRC is the loader source tree (relative, in-repo). RiivoBoot.cpp's
# broad GX includes resolve through it; the shim still shadows gecko.h
# via -iquote order.
export GXSRC := $(CURDIR)/../source

export INCLUDE	:=	$(foreach dir,$(INCLUDES), -iquote $(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD) \
					-I$(LIBOGC_INC) \
					-I$(CURDIR)/../source/riivo \
					-I"$(GXSRC)" -I"$(GXSRC)/riivo"

export LIBPATHS	:= -L$(LIBOGC_LIB) $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export OUTPUT	:=	$(CURDIR)/$(TARGET)
.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

else

# Production TU, compiled unmodified. Paths are relative to $(BUILD);
# the adapter root is one level up, the repo root two.
RIIVOSRC	:=	$(CURDIR)/../../source/riivo
MEMSRC		:=	$(CURDIR)/../../source/memory
ADAPTSHIM	:=	$(CURDIR)/../shim

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

$(OFILES_SOURCES) : $(HFILES)

RiivoFstInstall.o : $(RIIVOSRC)/RiivoFstInstall.cpp
	@echo $(notdir $<)
	$(CXX) $(CXXFLAGS) -I$(RIIVOSRC) -iquote $(ADAPTSHIM) -c $< -o $@

# Production caller TU, compiled unmodified. Section-per-function so the
# linker keeps only InstallPendingFst's closure. The .d file is deleted,
# not included (dependency paths are only space-safe on some make
# versions).
RiivoBoot.o : $(RIIVOSRC)/RiivoBoot.cpp
	@echo $(notdir $<)
	$(CXX) $(CXXFLAGS) -ffunction-sections -fdata-sections -I$(RIIVOSRC) \
		-iquote $(ADAPTSHIM) -I"$(GXSRC)" -I"$(GXSRC)/riivo" -c $< -o $@
	@rm -f RiivoBoot.d

# Production serializer + walker TUs for the v6 memwindow (real ops).
RiivoFstBuild.o : $(RIIVOSRC)/RiivoFstBuild.cpp
	@echo $(notdir $<)
	$(CXX) $(CXXFLAGS) -I$(RIIVOSRC) -iquote $(ADAPTSHIM) -c $< -o $@

RiivoFst.o : $(RIIVOSRC)/RiivoFst.cpp
	@echo $(notdir $<)
	$(CXX) $(CXXFLAGS) -I$(RIIVOSRC) -iquote $(ADAPTSHIM) -c $< -o $@

RiivoFstWalk.o : $(RIIVOSRC)/RiivoFstWalk.cpp
	@echo $(notdir $<)
	$(CXX) $(CXXFLAGS) -I$(RIIVOSRC) -iquote $(ADAPTSHIM) -c $< -o $@

# Pure frag-region planner for the v7 deep mirror (REAL TU).
RiivoFragPlan.o : $(RIIVOSRC)/RiivoFragPlan.cpp
	@echo $(notdir $<)
	$(CXX) $(CXXFLAGS) -I$(RIIVOSRC) -iquote $(ADAPTSHIM) -c $< -o $@

# Pure validation window for the deep runs (REAL TU, no mirror).
RiivoValidate.o : $(RIIVOSRC)/RiivoValidate.cpp
	@echo $(notdir $<)
	$(CXX) $(CXXFLAGS) -I$(RIIVOSRC) -iquote $(ADAPTSHIM) -c $< -o $@

# Production allocator, compiled unmodified (same two-heap chain the
# loader boots with; see LDFLAGS -wrap above).
mem2.o : $(MEMSRC)/mem2.cpp
	@echo $(notdir $<)
	$(CXX) $(CXXFLAGS) -I$(MEMSRC) -iquote $(ADAPTSHIM) -I"$(GXSRC)" -c $< -o $@

mem2alloc.o : $(MEMSRC)/mem2alloc.cpp
	@echo $(notdir $<)
	$(CXX) $(CXXFLAGS) -I$(MEMSRC) -iquote $(ADAPTSHIM) -I"$(GXSRC)" -c $< -o $@

-include $(DEPENDS)

endif
