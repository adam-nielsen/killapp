HOST=i686-mingw32-
RC=windres
TARGET=killapp.exe
OBJ=killapp.o killapp.res.o


.PHONY: all clean

all: $(TARGET)

clean:
	rm -f $(TARGET) $(OBJ)

$(TARGET): $(OBJ)
	$(HOST)$(CXX) -mwindows -o $@ $+

%.o: %.cpp
	$(HOST)$(CXX) -c -o killapp.o killapp.cpp

%.res.o: %.rc
	$(HOST)$(RC) -i $< -o $@
