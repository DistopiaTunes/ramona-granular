TARGET = ramona_player
OBJS = main.o

PSPSDK = $(shell psp-config --pspsdk-path)
SDL_CONFIG = $(PSPSDK)/../bin/sdl2-config

CFLAGS = -O2 -G0 -Wall $(shell $(SDL_CONFIG) --cflags)
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = $(shell $(SDL_CONFIG) --libs) -lpspvfpu -lpsphprm -lpspsdk -lpspctrl -lpspumd -lpsprtc -lpsppower -lpspgum -lpspgu -lpspaudiolib -lpspaudio -lstdc++ -lm

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Ramona Granular

include $(PSPSDK)/lib/build.mak
