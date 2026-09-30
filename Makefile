# wnip - Windows native screenshot & annotation tool
# Build:  make            (release, stripped)
#         make DEBUG=1    (with symbols)
#         make test       (build + run headless unit tests)
#         make run        (build + launch)
#
# The version number lives in src/version.h.

CC       := gcc
WINDRES  := windres
PYTHON   := python

SRCDIR   := src
OBJDIR   := build/obj
BINDIR   := build
TARGET   := $(BINDIR)/wnip.exe
TESTBIN  := $(BINDIR)/wnip_tests.exe

# The version lives in src/version.h and is shared by the C sources and the
# resource script, so there is nothing to keep in sync here.

COMMON_DEFS := -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
               -DWIN32_LEAN_AND_MEAN -DNOMINMAX
WARNFLAGS := -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type \
             -Wno-missing-field-initializers

ifeq ($(DEBUG),1)
OPT      := -O0 -g3
LDFLAGS  := -municode
DEFS     := $(COMMON_DEFS) -DWNIP_DEBUG=1
else
OPT      := -O2 -g0
LDFLAGS  := -municode -mwindows -Wl,--gc-sections -s
DEFS     := $(COMMON_DEFS)
endif

# The unit tests print to the console, so they are never a GUI subsystem app.
TESTLDFLAGS := -municode

CFLAGS   := -std=c11 $(OPT) -municode $(DEFS) -Isrc $(WARNFLAGS) -fno-strict-aliasing -ffunction-sections -fdata-sections

# Track header dependencies so editing a header rebuilds what includes it.
DEPFLAGS := -MMD -MP

LIBS     := -luser32 -lgdi32 -lole32 -loleaut32 -luuid -lshell32 -lshlwapi -lcomctl32 \
            -lcomdlg32 -ldwmapi -lgdiplus -lmsimg32 -luxtheme -lversion -ladvapi32 \
            -lwindowscodecs -lpropsys -lruntimeobject

# Files that are GUI-only (excluded from the headless unit test binary).
GUI_ONLY := src/main.c src/tray.c src/overlay.c src/editor.c src/pin.c \
            src/scrolling.c src/settings_dlg.c src/ocr.c \
            src/ocr_window.c src/about.c src/actions.c src/selftest.c

MFDIR    := $(BINDIR)/mf
MF_OBJ   := $(MFDIR)/default-manifest.o

SRC_ALL  := $(filter-out $(SRCDIR)/tests.c,$(wildcard $(SRCDIR)/*.c))
SRC_TEST := $(filter-out $(GUI_ONLY) $(SRCDIR)/tests.c,$(wildcard $(SRCDIR)/*.c)) src/tests.c

OBJ_ALL  := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRC_ALL))
OBJ_TEST := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRC_TEST))
RES_OBJ  := $(OBJDIR)/wnip_res.o

# -B makes GCC pick up our manifest object instead of its own stub (see
# src/wnip_manifest.rc).  The path must be absolute; GCC does not resolve a
# relative -B directory while looking for start files.  Only needed to link.
MFFLAG   := -B$(CURDIR)/$(MFDIR)/

.PHONY: all clean run test icon dirs

all: icon dirs $(TARGET)

icon: res/wnip.ico

res/wnip.ico: tools/mkicon.py
	@$(PYTHON) tools/mkicon.py

dirs:
	@mkdir -p $(OBJDIR) $(BINDIR)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(RES_OBJ): $(SRCDIR)/wnip.rc res/wnip.ico res/wnip.manifest | dirs
	$(WINDRES) --include-dir=. --include-dir=$(SRCDIR) -O coff $< -o $@

$(MF_OBJ): $(SRCDIR)/wnip_manifest.rc res/wnip.manifest | dirs
	@mkdir -p $(MFDIR)
	$(WINDRES) --include-dir=. -O coff $< -o $@

$(TARGET): $(OBJ_ALL) $(RES_OBJ) $(MF_OBJ) | dirs
	$(CC) $(MFFLAG) $(LDFLAGS) -o $@ $(OBJ_ALL) $(RES_OBJ) $(LIBS)
	@echo "built $@"

$(TESTBIN): $(OBJ_TEST) | dirs
	$(CC) $(TESTLDFLAGS) -o $@ $(OBJ_TEST) $(LIBS)
	@echo "built $@"

run: all
	@$(TARGET)

test: $(TESTBIN)
	@$(TESTBIN)

clean:
	rm -rf build

# Header dependencies emitted by the compiler (-MMD).  Keep this at the very
# end: an included makefile that defines a target before `all` would become the
# default goal, and plain `make` would then build a single object file.
-include $(OBJ_ALL:.o=.d) $(OBJ_TEST:.o=.d)
