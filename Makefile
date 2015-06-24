.PHONY: all clean

all: xug

resources.cpp: xug.gresource.xml window.ui app-menu.ui
	glib-compile-resources $< --target=$@ --sourcedir=. --generate-source

xug: main.cpp resources.cpp
	$(CXX) -o $@ -g -O3 -std=c++11 `pkg-config --cflags --libs gtk+-3.0` `pkg-config --cflags --libs xmms2-client` `pkg-config --cflags --libs xmms2-client-glib` $^

clean:
	rm -f xug resources.cpp
