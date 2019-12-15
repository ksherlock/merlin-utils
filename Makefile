LINK.o = $(LINK.cc)
CXXFLAGS = -std=c++17 -g -Wall -Wno-sign-compare 
CCFLAGS = -g
CPPFLAGS += -I afp/include

.PHONY: all clean

all: merlin-link

clean:
	$(RM) -rf merlin-link o
	$(MAKE) -C afp clean


o:
	mkdir o

merlin-link: o/main.o o/link.o o/script.o o/mapped_file.o o/omf.o o/set_file_type.o afp/libafp.a
	$(LINK.o) $^ $(LDLIBS) -o $@

o/mapped_file.o : mapped_file.cpp mapped_file.h unique_resource.h
o/link.o : link.cpp mapped_file.h omf.h
o/omf.o : omf.cpp omf.h

o/%.o: %.cpp | o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

%.cpp: %.re2c
	re2c -W -o $@ $<

.PHONY: subdirs
subdirs:
	$(MAKE) -C afp


afp/libafp.a : subdirs
