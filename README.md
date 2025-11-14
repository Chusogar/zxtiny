# ZX Spectrum Emulator zxtiny

This is a ZX Spectrum emulator in C/C++, webassembly (and ESP32, RPI2040....among others in a near future)

# Building for the desktop

The code is designed to run on linux, but it shold be pretty easy to get running on other platforms.

```
mkdir build
cd build
cmake ..
make
./zxtiny ./roms/spectrum
```


# Using Emscripten
You can also build it for the web if you have emscripten installed.

```
emcmake cmake .. -DROMS_DIR=../roms/spectrum && emmake make
```
and you wull obtain four "zxtiny.(js|wasm|data|html)" files which can be hosted on a web server.


# Run Emscripten

```
python3 -m http.server
```

Now go to http://localhost:8000/zxtiny.html

