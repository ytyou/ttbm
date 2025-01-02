default: build

CC = gcc
CPP = g++
LINK = $(CPP)
RM = /bin/rm -f

# sanitize only works on 64-bit host
uname_m := $(shell uname -m)
SANITIZE.x86_64 := -fsanitize=address
SANITIZE += $(SANITIZE.$(uname_m))

# add -flto=auto flag on non-arm platforms
ifneq ($(findstring arm,$(uname_m)),arm)
LINKOPT += -flto=auto
endif

#DEBUG_FLAGS = -g3 -rdynamic -ggdb -D_DEBUG -D_TEST -D_GLIBCXX_DEBUG -O0 $(SANITIZE) -fno-omit-frame-pointer
PROD_FLAGS = -O3 -march=native -ffast-math
#LEAK_FLAGS = -D_LEAK_DETECTION
#PROF_FLAGS = -pg -D_GLIBCXX_DEBUG

CFLAGS = -I include -std=c++11 -Wno-unused-result -Wno-format-truncation -Wno-psabi -Wno-deprecated-declarations $(LINKOPT) $(DEBUG_FLAGS) $(PROF_FLAGS) $(LEAK_FLAGS) $(PROD_FLAGS)
LFLAGS = -pthread $(LINKOPT) $(DEBUG_FLAGS) $(PROF_FLAGS)
LIBS = $(DEBUG_LIBS) -lz

INCS = $(wildcard include/*.h)

SRCS = $(wildcard src/*.cpp src/*/*.cpp)
TMPS = $(subst src,objs,$(SRCS))
OBJS = $(patsubst %.cpp,%.o,$(TMPS))
OBJI = $(filter-out objs/main.o,$(OBJS))

TARGET = bin/bm

build: $(TARGET)

bin/bm: $(OBJS)
	@mkdir -p $(@D)
	$(LINK) $(LFLAGS) -o $(TARGET) $(OBJS) $(LIBS)

objs/%.o: src/%.cpp $(INCS)
	@mkdir -p $(@D)
	$(CPP) $(CFLAGS) -c $< -o $@

objs/*/%.o: src/*/%.cpp $(INCS)
	@mkdir -p $(@D)
	$(CPP) $(CFLAGS) -c $< -o $@

bm: clean build
all: build

clean:
	$(RM) $(OBJS) $(TARGET)
