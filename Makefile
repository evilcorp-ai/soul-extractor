CC ?= gcc
CFLAGS := -std=c99 -O2 -Wall -Wextra -pedantic -Wno-unused-parameter $(CFLAGS)
O = obj/

ifeq ($(OS),Windows_NT)
    TARGET = unimgc.exe
    MKDIR = if not exist $(subst /,\,$(O)) mkdir $(subst /,\,$(O))
    RM = if exist $(subst /,\,$(O)) rmdir /s /q $(subst /,\,$(O)) & if exist $(TARGET) del /q $(TARGET)
else
    TARGET = unimgc
    MKDIR = mkdir -p $(O)
    RM = rm -rf $(TARGET) $(O)
    CPPFLAGS := -D_POSIX_C_SOURCE=200112L -D_FILE_OFFSET_BITS=64 $(CPPFLAGS)
endif

.PHONY: all clean
all: $(TARGET)

clean:
	$(RM)

$(TARGET): $(O)unimgc.o $(O)image.o $(O)lzo.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

$(O)%.o: %.c | $(O)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(O):
	$(MKDIR)
