# firmware/drivers/drivers.mk - build wiring for the drivers module
# (fragment style per docs/rewrite/architecture.md section 2; included by the root
# Makefile).
#
# Exactly ONE panel I/O backend is linked per image:
#   target firmware -> panel_io_rknanoc.c (MMIO contract, target TODOs)
#   host tools/tests -> panel_io_sim.c    (simulated panel, PPM dumps)

DRIVERS_DIR := firmware/drivers

DRIVERS_COMMON_SRCS := \
  $(DRIVERS_DIR)/display.c \
  $(DRIVERS_DIR)/panel_init_rknanoc.c \
  $(DRIVERS_DIR)/font.c \
  $(DRIVERS_DIR)/font8x8.c \
  $(DRIVERS_DIR)/text.c \
  $(DRIVERS_DIR)/icons.c \
  $(DRIVERS_DIR)/fonts/fonts_s8.c \
  $(DRIVERS_DIR)/fonts/icons_data.c

# Sources compiled for the ARM target image. The fonts other than S8 are not
# linked: they are the "fonts" module (fonts/rechord_fonts.bin, make_fw1 --module),
# loaded at boot by text_load.c.
DRIVERS_TARGET_SRCS := \
  $(DRIVERS_COMMON_SRCS) \
  $(DRIVERS_DIR)/text_load.c \
  $(DRIVERS_DIR)/panel_io_rknanoc.c

# Sources compiled for host tests/tools.
DRIVERS_HOST_SRCS := \
  $(DRIVERS_COMMON_SRCS) \
  $(DRIVERS_DIR)/fonts/fonts_blob.c \
  $(DRIVERS_DIR)/panel_io_sim.c

DRIVERS_INCLUDE_DIRS := $(DRIVERS_DIR)
