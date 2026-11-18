all:
	g++ -std=c++17 main.cpp WheelSupports.cpp FFBIPC.cpp -framework CoreFoundation -framework IOKit -pthread -o FreeTheWheel

TELEMETRY_SDK = telemetry/scs-sdk

telemetry-probe:
	clang++ -std=c++17 -arch x86_64 -dynamiclib -fvisibility=hidden \
		-I$(TELEMETRY_SDK) telemetry/FTWTelemetry.cpp \
		-o telemetry/libFTWTelemetry.dylib
