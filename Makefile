LINK.o = $(LINK.cc)
CXXFLAGS = -std=c++17 -g -Wall -Wno-sign-compare 
CCFLAGS = -g

.PHONY: all

all: rel-link


o:
	mkdir $<

rel-link: o/link.o o/mapped_file.o o/omf.o
	$(LINK.o) $^ $(LDLIBS) -o $@

o/mapped_file.o : mapped_file.cpp mapped_file.h unique_resource.h
o/link.o : link.cpp mapped_file.h omf.h
o/omf.o : omf.cpp omf.h

o/%.o: %.cpp | o
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.cpp: %.re2c
	re2c -W -o $@ $<
