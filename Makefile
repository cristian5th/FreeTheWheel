all:
	g++ -std=c++17 main.cpp WheelSupports.cpp FFBIPC.cpp -framework CoreFoundation -framework IOKit -pthread -o FreeTheWheel
