SRCS       := $(wildcard *.cpp)
EXECUTABLE := rvcpp

.SUFFIXES:
.PHONY: all clean

all: $(EXECUTABLE)

clean:
	rm -f $(EXECUTABLE)

$(EXECUTABLE): $(SRCS) $(wildcard include/*.h) $(wildcard include/*/*.h)
	g++ -std=c++17 -O3 -Wall -Wextra -I include $(SRCS) -o $(EXECUTABLE)
