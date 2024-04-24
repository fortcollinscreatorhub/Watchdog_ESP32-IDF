# Development Instructions

These instructions should work on any system that has a recent verion of
Docker installed. For example, native Ubuntu Linux 22.04, or Ubuntu Linux
under WSL (Windows Subsystem for Linux).

# Create a development container

TODO: Creation of the container should be done via a Dockerfile.

Note: Older versions of ESP-IDF only work on older versions of Ubuntu. I
haven't experimented fully to find the newest Ubuntu that is supported by
the various versions of ESP-IDF that I've tried.

Note: The commands below use ESP-IDF v4.1.4. v4.2.5 also works. v4.3.7 and
v4.4.7 have problems with pip package versions during the install phase.

```shell
apt update
apt -y install docker.io
# You may need to edit Docker configuration to allow your user to use Docker,
# and then log out and back in.
docker container run -itd --name build-fcch-compressor --entrypoint bash ubuntu:18.04
docker exec -it build-fcch-compressor bash
apt update
apt -y install \
    build-essential \
    cmake \
    git \
    libusb-1.0-0 \
    python3 python3-pip python3-setuptools \
    #
mkdir -p ~/git_wa/
git clone --recursive -b v4.1.4 https://github.com/espressif/esp-idf.git ~/git_wa/esp-idf-v4.1.4
~/git_wa/esp-idf-v4.1.4/install.sh esp32
```

# Get a shell prompt inside the development container

TODO: an ephemeral container should be launched just to run the build, rather
than creating a long-running container that the user configures and runs
commands within.

```shell
docker start build-fcch-compressor bash
docker exec -it build-fcch-compressor bash
```

# Clone the source code for this project

TODO: This should be done on the host, not in the container, and the files 
mounted into the container during the build...

```shell
git clone https://github.com/fortcollinscreatorhub/Watchdog_ESP32-IDF.git ~/git_wa/FCCH-compressor-controller
```

# Compile the project

```shell
. ~/git_wa/esp-idf-v4.1.4/export.sh
cd ~/git_wa/FCCH-compressor-controller
idf.py build
```

The result will be: `build/Watchdog_ESP32-IDF.bin`.

# Destroy the container

Only do this if you aren't going to create another build. If you destroy the
container, you will need to run all the setup steps again.

```shell
docker stop build-fcch-compressor
docker rm build-fcch-compressor
```
