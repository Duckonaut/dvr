set windows-shell := ["powershell.exe", "-c"]

[unix]
clean:
    rm -rf ./build
    rm -rf ./release
    rm -rf ./dist

[windows]
clean:
    rm -Recurse -Force ./build
    rm -Recurse -Force ./release
    rm -Recurse -Force ./dist

[unix]
setup:
    meson setup build -Dimgui=true

[unix]
resetup:
    meson setup build --wipe -Dimgui=true

[windows]
setup:
    meson setup build --backend vs -Dimgui=true

[windows]
resetup:
    meson setup build --backend vs --wipe -Dimgui=true

build: setup
    meson compile -C build

[unix]
release:
    meson setup release -Dbuildtype=release --wipe
    meson compile -C release

[windows]
release:
    meson setup release -Dbuildtype=release --backend vs --wipe
    meson compile -C release

[unix]
run: build
    ./build/dvr

[windows]
run: build
    ./build/dvr.exe
