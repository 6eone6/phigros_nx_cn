# Phigros — Nintendo Switch port (Unity 2022.3 / IL2CPP wrapper)
 
This is a native wrapper / loader that runs the original ARM64 Android build of
Phigros on Switch homebrew. It contains **no game code and no game assets** — it
loads the game's own libraries and recreates, natively, the thin Android/JNI
layer the Unity engine expects.
 
## Install & run
 
You need files from your own copy of the Phigros 3.19.5 APK.
 
Put the `.nro` in any folder under `sdmc:/switch/` and place your game files next
to it — the loader finds its folder at runtime, so the name is up to you:
 
```
sdmc:/switch/phigros
├── phigros_nx.nro
├── libmain.so  libunity.so  libil2cpp.so   <- from your APK: lib/arm64-v8a/
├── libnativeaudioe7.so                     
└── assets/                                 
    ├── bin/Data/                           
    └── aa/                                 
```
You need to combine the assets folder from the base.apk, split_UnityDataAssetPack.apk and split_UnityStreamingAssetsPack.apk aproximately 2.5GB.

Launch via title override (hold R while starting an installed game) or a
forwarder.
 

 ## Building
 
Requires devkitPro with the `switch-dev` group plus these portlibs:
 
```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-libpng switch-zlib
 
export DEVKITPRO=/opt/devkitpro
make                        # -> phigros_nx.nro
```
 
## Credits
 
The loader/shim infrastructure (so_util, libc_shim, jni_fake, unity_jni,
opensles, diagnostics) derives from the open-source Switch `.so`-loader lineage
— Andy Nguyen, fgsfds and ChanseyIsTheBest, building on TheOfficialFloW's
Vita/Switch loader tradition — reaching this project via the Zookeeper DX, PvZ
Fusion and Fruit Ninja ports, with CloverPit as a reference (its OpenSL
buffer-queue fix is the reason audio here is clean). All MIT-licensed. Thanks to
everyone in that lineage for making this approach possible.
